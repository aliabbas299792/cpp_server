#include <iostream>

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>

#include <thread>

#define PORT "4000"
#define MAX_DATA_SIZE 1024 //make bytes at once

int main(){
  int sockfd, numBytes;
  addrinfo hints, *serverInfo, *traverser;
  int responseCode;
  char serverAddress[INET6_ADDRSTRLEN];
  
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  std::cout << "Please enter your username: ";
  std::string username;
  std::getline(std::cin, username);

  if((responseCode = getaddrinfo("erewhon.xyz", PORT, &hints, &serverInfo)) != 0){
    perror("getaddrinfo");
    exit(1);
  }

  for(traverser = serverInfo; traverser != NULL; traverser = traverser->ai_next){
    if((sockfd = socket(traverser->ai_family, traverser->ai_socktype, traverser->ai_protocol)) == -1){
      perror("socket creation");
      continue;
    }

    if(connect(sockfd, traverser->ai_addr, traverser->ai_addrlen) == -1){
      close(sockfd);
      perror("socket connect");
      continue;
    }

    break;
  }

  if(traverser == NULL){
    perror("socket failed to connect");
    exit(1);
  }

  freeaddrinfo(serverInfo);

  const auto receiveThread = std::thread([&]{
    while(true){ //add method for super long messages
      char buffer[MAX_DATA_SIZE];
      std:memset(buffer, 0, sizeof(buffer));
      if((numBytes = recv(sockfd, buffer, MAX_DATA_SIZE-1, 0)) == -1){
        perror("recv");
        exit(1);
      }
      std::cout << buffer << "\n";
    }
  });


  while(true){ //add method for super long messages
    std::string input;
    std::getline(std::cin, input);
    input = username + ": " + input;
    if(send(sockfd, input.c_str(), input.size(), 0) == -1){
      perror("send");
      exit(1);
    }
  }
}