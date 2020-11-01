#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "libs/readerwriterqueue/readerwriterqueue.h"

#define MAX_EVENTS 10000
#define PORT "4000"
#define BACKLOG 100

/*
 *
 * 
 * 
 * Do something to deal with clients disconnecting
 * 
 * 
 * 
*/

struct itc_s { //itc = inter thread communication struct
  itc_s(std::string payload = "", int socketFd = 0, int threadID = 0){
    this->payload = payload;
    this->socketFd = socketFd;
    this->threadID = threadID;
  }
  std::string payload;
  int socketFd;
  int threadID;
};

std::string toReadableIP(sockaddr_storage *clientAddr){
    sockaddr_in* sockaddr = (sockaddr_in*)clientAddr;
    void* addr = &sockaddr->sin_addr;
    char ip_string[INET6_ADDRSTRLEN];
    inet_ntop(((sockaddr_in*)clientAddr)->sin_family, addr, ip_string, sizeof(ip_string));
    return std::string(ip_string);
}

void makeSocketNonBlocking(int sockfd){
  int flags = fcntl(sockfd, F_GETFL);
  if(flags == -1){
    perror("fcntl retrieving flags");
    exit(1);
  }
  if(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1){
    perror("fcntl setting O_NONBLOCK");
    exit(1);
  }
}

typedef moodycamel::ReaderWriterQueue<itc_s> stringQueue;

const auto network_threads = std::max((unsigned int)1, std::thread::hardware_concurrency()-1);

int *eventFdArrayToProcessingThread = nullptr;
int *eventFdArrayFromProcessingThread = nullptr;
stringQueue *toProcessingThreadArray = nullptr;
stringQueue *fromProcessingThreadArray = nullptr;

const uint64_t writeVariable = 1; //write this to the eventfd used to synchronise the threads

struct { //used to synchronise processing thread setting up epoll for eventfd with the network threads, which need to make the eventfd's
  const int eventFd = eventfd(0, 0);
  int counter = 0;
} wakeUpProcessingThread;

void sendMessage(int socketFd, itc_s *data, std::unordered_map<int, std::string> *residualToSendData, int epoll_fd){
  int writtenBytes = write(socketFd, data->payload.c_str(), data->payload.size());

  if(writtenBytes == -1){
    perror("write to client socket failed");
    exit(1);
  }else if(writtenBytes != data->payload.size()){
    residualToSendData->insert({ socketFd, data->payload.substr(writtenBytes, data->payload.size()) }); //the remaining data is put into here
    
    epoll_event writeAgainEvent;
    writeAgainEvent.data.fd = socketFd;
    writeAgainEvent.events = EPOLLIN | EPOLLET | EPOLLOUT;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, socketFd, &writeAgainEvent) == -1){
      perror("EPOLL_CTL_MOD for EPOLLOUT failed");
      exit(1);
    }
  }
}

void network_io(int id){
  const auto toProcessingThread = &toProcessingThreadArray[id];
  const auto fromProcessingThread = &fromProcessingThreadArray[id];

  eventFdArrayToProcessingThread[id] = eventfd(0, EFD_NONBLOCK);
  const auto eventFdToProcessingThread = eventFdArrayToProcessingThread[id];

  eventFdArrayFromProcessingThread[id] = eventfd(0, EFD_NONBLOCK);
  const auto eventFdFromProcessingThread = eventFdArrayFromProcessingThread[id];
  
  wakeUpProcessingThread.counter++; //increment the counter, once it is equal to network thread then wake up the processing thread (if it's asleep), or just let it through
  if(wakeUpProcessingThread.counter == network_threads){
    write(wakeUpProcessingThread.eventFd, &writeVariable, sizeof(writeVariable));
  }

  std::unordered_set<int> liveClientSockets; //the set of live sockets in this thread
  
  //for the socket stuff
  int serverFd, clientFd;
  addrinfo hints, *serverInfo, *traverser;
  sockaddr_storage clientAddress;
  socklen_t client_size;
  bool yes = true;
  char remoteAddress[INET6_ADDRSTRLEN];
  int responseCode;

  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC; //either IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; //TCP
  hints.ai_flags = AI_PASSIVE; //local IP
  
  if((responseCode = getaddrinfo(NULL, PORT, &hints, &serverInfo)) != 0){
    perror("getaddrinfo");
    exit(1);
  }

  for(traverser = serverInfo; traverser != NULL; traverser = traverser->ai_next){
    if((serverFd = socket(traverser->ai_family, traverser->ai_socktype, traverser->ai_protocol)) == -1){ //try to make a server socket
      perror("server socket");
      continue;
    }

    makeSocketNonBlocking(serverFd); //make the socket non blocking
    
    if(setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, (int*)&yes, sizeof(int)) == -1){ //try to set the SO_REUSEADDR flag, SOL_SOCKET is the protocl level
      perror("server setsockopt");
      exit(1);
    }
    
    if(setsockopt(serverFd, SOL_SOCKET, SO_REUSEPORT, (int*)&yes, sizeof(int)) == -1){ //try to set the SO_REUSEPORT flag, SOL_SOCKET is the protocl level
      perror("server setsockopt");
      exit(1);
    }

    if(bind(serverFd, traverser->ai_addr, traverser->ai_addrlen) == -1){ //try to bind the server socket
      perror("server bind");
      continue;
    }

    break;
  }
  
  freeaddrinfo(serverInfo);

  if(traverser == NULL){
    perror("Either no socket was made or it wasn't able to bind");
    exit(1);
  }
  
  //std::string ip = toReadableIP((sockaddr_storage*)traverser->ai_addr);
  //std::cout << ip << "\n";
  
  if(listen(serverFd, BACKLOG) == -1){ //set this as a listen socket
    perror("listen failed");
    exit(1);
  }
  
  /**
   * Use epoll for monitoring eventFd as well as the listening socket, and the user sockets
   *  -When eventFd is readable then take the data from the fromProcessingThread queue and send it through the appropriate socket
   *  -When the listening socket is readable then accept a new connection
   *  -When a client socket is readable then read some data, and then send it through toProcessingThread, and do a write event on eventFd
   * Do everything in a non blocking fashion, and use edge-triggered mode for epoll
   * */

  int epoll_fd, event_count;

  if((epoll_fd = epoll_create1(0)) == -1){
    perror("Epoll couldn't be instantiated");
    exit(1);
  }
  
  epoll_event event; //struct for an epoll event, we'll add and it and then reuse it later for some other event as the original has already been added
  std::memset(&event, 0, sizeof(event));

  event.events = EPOLLIN | EPOLLET;
  event.data.fd = eventFdFromProcessingThread;
  if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, eventFdFromProcessingThread, &event) == -1){
    perror("epoll_ctl eventFd");
    exit(1);
  }

  event.data.fd = serverFd;
  if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serverFd, &event) == -1){
    perror("epoll_ctl serverFd");
    exit(1);
  }

  epoll_event events[MAX_EVENTS];

  std::unordered_map<int, std::string> residualToSendData; //if all of the data wasn't sent at once, then the data left is stored here

  while(true){
    event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for(int i = 0; i < event_count; i++){
      if(events[i].data.fd == eventFdFromProcessingThread){ //if data has been sent from the processing thread
        while(true){
          uint64_t readBuffer;
          int responseCode = read(eventFdFromProcessingThread, &readBuffer, sizeof(readBuffer));
          if(responseCode < 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK){ //have read all data
              break;
            }
          }

          itc_s data;
          if(fromProcessingThread->try_dequeue(data)){ //dequeue some data
            if(liveClientSockets.count(data.socketFd) && data.threadID != -1){ //if it's a valid live client
              sendMessage(data.socketFd, &data, &residualToSendData, epoll_fd);
            }else if(data.socketFd == -1 && data.threadID == -1){ //this means it's for broadcast
              for(const auto &socketFd : liveClientSockets){
                sendMessage(socketFd, &data, &residualToSendData, epoll_fd);
              }
            }else if(data.socketFd != -1 && data.threadID == -1){ //this is the case where we are broadcasting, but excluding one socket
              for(const auto &socketFd : liveClientSockets){
                if(socketFd != data.socketFd){
                  sendMessage(socketFd, &data, &residualToSendData, epoll_fd);
                }
              }
            }
          }
        }
      }else if(events[i].data.fd == serverFd){ //if there is an incoming connection
        //std::cout << "got a new connection\n";
        client_size = sizeof(clientAddress);
        clientFd = accept(serverFd, (sockaddr*)&clientAddress, &client_size);
        //std::cout << "accepted a new connection\n";

        if(clientFd < 0){
          if(errno == EAGAIN || errno == EWOULDBLOCK){
            std::cout << "accept returned EAGAIN or EWOULDBLOCK on thread with ID " << id << "\n";
          }else{ //some other error
            perror("accept failed");
            exit(1);
          }
        }else{ //if successfully accepted
          makeSocketNonBlocking(clientFd);

          liveClientSockets.insert(clientFd);

          event.data.fd = clientFd;
          if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, clientFd, &event) == -1){
            perror("epoll_ctl failed for a client socket");
            exit(1);
          }
          //std::cout << "succesfully listening to the new connection\n";
        }
      }else if(events[i].events & EPOLLOUT){ //is this one of the sockets that still needs to write some data?
        auto payload = residualToSendData[events[i].data.fd];
        int writtenBytes = write(events[i].data.fd, &payload, sizeof(payload));
        if(writtenBytes == -1){
          perror("write failed for a residual amount of data");
          exit(1);
        }else if(writtenBytes != sizeof(payload)){ //still more to send
          residualToSendData[events[i].data.fd] = payload.substr(writtenBytes, sizeof(payload));
        }else{
          event.data.fd = events[i].data.fd;
          if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, events[i].data.fd, &event) == -1){ //basically removing the EPOLLOUT flag
            perror("epoll_ctl failed for returning a modified socket event back to normal");
            exit(1);
          }
          residualToSendData.erase(events[i].data.fd); //removes the residual data entry
        }
      }else{ //if a socket is ready to read
        std::string buffer;
        while(true){
          char tempBuffer[1024];
          std::memset(tempBuffer, 0, sizeof(tempBuffer));
          
          ssize_t readBytes = read(events[i].data.fd, tempBuffer, sizeof(tempBuffer));
          
          if(readBytes == -1){
            if(errno == EAGAIN || EWOULDBLOCK){ //finished reading data
              break;
            }else{
              perror("read from client");
              exit(1);
            }
          }else if(readBytes == 0){ //finished reading data
            break;
          }else{
            buffer += tempBuffer; //append the read data to the buffer
          }
        }
        if(buffer.size()){ //so empty messages aren't sent through
          toProcessingThread->try_enqueue(itc_s(buffer, events[i].data.fd, id)); //enqueue the buffer data and send it to the processing thread
          write(eventFdToProcessingThread, &writeVariable, sizeof(writeVariable)); //to signal that data has been pushed to the queue
        }
      }
    }
  }
}

void broadcast(std::string payload, int socketFd = -1){ //socketFd's are unique to the program, so it can't trigger the exclude-this-socket condition on more than one thread
  for(int i = 0; i < network_threads; i++){
    fromProcessingThreadArray[i].try_enqueue(itc_s(payload, socketFd, -1)); //socketFd and thread ID of -1 implies this is for broadcast
    write(eventFdArrayFromProcessingThread[i], &writeVariable, sizeof(writeVariable));
  }
}

int main(){
  std::thread threadContainer[network_threads];

  toProcessingThreadArray = new stringQueue[network_threads];
  fromProcessingThreadArray = new stringQueue[network_threads];
  eventFdArrayToProcessingThread = new int[network_threads];
  eventFdArrayFromProcessingThread = new int[network_threads];

  /*
  * Use epoll for monitoring eventFdArrray in edge-triggered mode, read data and make all the eventFds non blocking
  * When data comes through, it should be in a structure which indicates what socketFd, and what thread ID it came from
  * Then once finished, send back data with that information, possibly in a different structure though 
  * */

  for(int i = 0; i < network_threads; i++){
    threadContainer[i] = std::thread(network_io, i);
  }
  
  int epoll_fd, event_count;

  if((epoll_fd = epoll_create1(0)) == -1){
    perror("Epoll couldn't be instantiated on the processing thread");
    exit(1);
  }

  epoll_event event;
  std::memset(&event, 0, sizeof(event));

  { //this scope is where we synchronise the network threads with this thread (the processing thread)
    epoll_event singleEventEvents[1];

    event.events = EPOLLIN;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeUpProcessingThread.eventFd, &event) == -1){
      perror("global eventFd epoll_ctl failed");
      exit(1);
    }

    epoll_wait(epoll_fd, singleEventEvents, 1, -1); //block on this thread until the eventFd is written to
    
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, wakeUpProcessingThread.eventFd, &event); //delete the event explicitly as we no longer need it
  }

  event.events = EPOLLIN | EPOLLET;

  epoll_event events[network_threads];
  
  for(int i = 0; i < network_threads; i++){ //register the eventfds
    const auto eventFd = eventFdArrayToProcessingThread[i];
    event.data.u32 = i; //the thread ID

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, eventFd, &event) == -1){
      perror("epoll_ctl in processing thread for the eventfds");
      exit(1);
    }
  }

  while(true){
    event_count = epoll_wait(epoll_fd, events, network_threads, -1); //wait for events through this

    for(int i = 0; i < event_count; i++){
      while(true){
        uint64_t readBytes;
        int responseCode = read(eventFdArrayToProcessingThread[events[i].data.u32], &readBytes, sizeof(readBytes));

        if(responseCode < 0){
          if(errno == EAGAIN || errno == EWOULDBLOCK){ //have read all data
            break;
          }
        }

        itc_s data;
        if(toProcessingThreadArray[events[i].data.u32].try_dequeue(data)){
          /**
           * 
           * Process the data here however you wish
           * 
           * */
          std::string someProcessedData = data.payload;

          data.payload = someProcessedData;

          broadcast(data.payload, data.socketFd); //broadcast function sends it to all threads
          //fromProcessingThreadArray[events[i].data.u32].try_enqueue(data); //sent back to the appropriate thread, for the appropriate socket
          //write(eventFdArrayFromProcessingThread[events[i].data.u32], &writeVariable, sizeof(writeVariable)); //notify the thread
        }
      }
    }
  }
}