#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/socket.h>
#include <vector>
#include <map>
#include <queue>
#include <chrono>
#include <tuple>
#include <climits>

// create a global map mapping server address/port to num active clients
static std::map<std::string, int> server_clients;
// also create a global map mapping connected servers to the port that they're hosting for clients
static std::map<std::string, std::string> real_server_address;
static bool vflag = false;

struct ConnectionInfo
{
  int comm_fd;
  std::string server_address;
  ConnectionInfo(int comm_fd, std::string server_address) : comm_fd(comm_fd), server_address(server_address) {}
};

void *frontendThread(void *args)
{
  int comm_fd = ((ConnectionInfo *)args)->comm_fd;
  std::string addr = ((ConnectionInfo *)args)->server_address;
  int total_chars = 0;
  char *buffer = (char *)malloc(1000);
  bzero(buffer, 1000);
  bool received_first_message = false;
  char *read_buffer = (char *)malloc(100);

  while (true)
  {
    // read 100 bytes into a read buffer
    bzero(read_buffer, 100);
    int num_read = read(comm_fd, read_buffer, 100);
    if (num_read < 0)
    {
      fprintf(stderr, "read failed!\n");
      exit(1);
    }
    if (num_read == 0)
    {
      // this means the frontend has closed the connection or died
      // remove frontend from server_clients for now
      server_clients.erase(addr);
      real_server_address.erase(addr);

      free(read_buffer);
      close(comm_fd);
      if (vflag)
      {
        fprintf(stderr, "[%d] Connection closed\n", comm_fd);
      }
      pthread_exit(NULL);
    }
    if (vflag)
    {
      fprintf(stderr, "Message received: %s\n", read_buffer);
    }
    if (!received_first_message)
    {
      // if we've already received the address, we don't care about the message so can just move on
      // if we haven't we need to store the message
      for (int i = 0; i < 100; i++)
      {
        buffer[total_chars + i] = read_buffer[i];
      }
      total_chars = total_chars + num_read;

      // iterate through buffer to check for /r/n
      int i = 0;
      while (!received_first_message && buffer[i] != '\0')
      {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n')
        {
          // this means we've found the end of the first message; should be address port information
          std::string temp = buffer;
          std::string final_addr = temp.substr(0, i);
          real_server_address[addr] = final_addr;
          received_first_message = true;
          free(buffer);
          break;
        }
        i++;
      }
    }

    // if the message from the frontend server is "STAT", return the information about live servers
    if (strncmp(read_buffer, "STAT", 4) == 0)
    {

      // Get data for servers
      std::string response_8080 = "127.0.0.1:8080,";
      std::string response_8081 = "127.0.0.1:8081,";
      std::string response_8082 = "127.0.0.1:8082,";
      std::map<std::string, std::string>::iterator real_server_address_it;
      for (real_server_address_it = real_server_address.begin(); real_server_address_it != real_server_address.end(); real_server_address_it++)
      {
        // check if each server is alive and return num of clients
        if (real_server_address_it->second == "127.0.0.1:8080")
        {
          response_8080 += "Alive,";
          response_8080 += std::to_string(server_clients[real_server_address_it->first]);
        }

        if (real_server_address_it->second == "127.0.0.1:8081")
        {
          response_8081 += "Alive,";
          response_8081 += std::to_string(server_clients[real_server_address_it->first]);
        }

        if (real_server_address_it->second == "127.0.0.1:8082")
        {
          response_8082 += "Alive,";
          response_8082 += std::to_string(server_clients[real_server_address_it->first]);
        }
      }

      // if string hasn't been updated, server is dead
      if (response_8080 == "127.0.0.1:8080,")
      {
        response_8080 += "Down,0";
      }

      if (response_8081 == "127.0.0.1:8081,")
      {
        response_8081 += "Down,0";
      }

      if (response_8082 == "127.0.0.1:8082,")
      {
        response_8082 += "Down,0";
      }
      if (vflag)
      {
        // print out server_clients
        std::map<std::string, int>::iterator it;
        std::cerr << "server_clients: " << std::endl;
        for (it = server_clients.begin(); it != server_clients.end(); it++)
        {
          std::cerr << it->first << " " << it->second << std::endl;
        }

        // print out real server address
        std::cerr << "real_server_address: " << std::endl;
        for (real_server_address_it = real_server_address.begin(); real_server_address_it != real_server_address.end(); real_server_address_it++)
        {
          std::cerr << real_server_address_it->first << " " << real_server_address_it->second << std::endl;
        }
      }

      // Send data to frontend server
      std::string response = response_8080 + "\n" + response_8081 + "\n" + response_8082 + "\n\r\n";
      if (vflag)
      {
        std::cerr << "response: " << response << std::endl;
      }
      write(comm_fd, response.c_str(), strlen(response.c_str()));
    }
    else if (strncmp(read_buffer, "JOIN", 4) == 0) 
    {
      // new connection on client 
      server_clients[addr]++;
    }
    else if (strncmp(read_buffer, "QUIT", 4) == 0)
    {
      // client has quit
      server_clients[addr]--;
    }
  }

  pthread_exit(NULL);
}

// takes in -p command specifying which port the server should be hosted on for frontend servers to connect to
int main(int argc, char *argv[])
{
  int c;        // var for getopt
  int p = 5000; // default port for frontend servers to connect to is 5000
  while ((c = getopt(argc, argv, "v")) != -1)
    switch (c)
    {
    case 'v':
      vflag = true;
      break;
    case '?':
      if (isprint(optopt))
        fprintf(stderr, "Unknown option '-%c'.\n", optopt);
      else
        fprintf(stderr, "Unknown option character '\\x%x',\n", optopt);
      return 1;
    default:
      abort();
    }

  // initialize frontend socket for servers

  int sockfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (sockfd < 0)
  {
    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
    exit(1);
  }

  // bind frontned socket to specified port

  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));

  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  servaddr.sin_port = htons(p);

  int opt = 1;
  int ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
  if (ret < 0)
  {
    fprintf(stderr, "setsockopt failed\n");
    exit(1);
  }

  ret = bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
  if (ret < 0)
  {
    fprintf(stderr, "failed to bind (%s)\n", strerror(errno));
    exit(1);
  }

  // start listening on frontend socket
  listen(sockfd, 100);

  // initialize user socket
  int u_sockfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (u_sockfd < 0)
  {
    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
    exit(1);
  }

  // bind user socket to specific port

  struct sockaddr_in u_servaddr;
  bzero(&u_servaddr, sizeof(u_servaddr));

  u_servaddr.sin_family = AF_INET;
  u_servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  u_servaddr.sin_port = htons(8000); // default port for user socket is 8000

  int u_opt = 1;
  ret = setsockopt(u_sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &u_opt, sizeof(u_opt));
  if (ret < 0)
  {
    fprintf(stderr, "setsockopt failed\n");
    exit(1);
  }

  ret = bind(u_sockfd, (struct sockaddr *)&u_servaddr, sizeof(u_servaddr));
  if (ret < 0)
  {
    fprintf(stderr, "Failed to bind user socket (%s)\n", strerror(errno));
    exit(1);
  }

  // start listening on user socket
  listen(u_sockfd, 100);

  while (true)
  {
    struct sockaddr_in clientaddr;
    socklen_t clientaddrlen = sizeof(clientaddr);
    // accept frontend server connection
    int comm_fd = accept(sockfd, (struct sockaddr *)&clientaddr, &clientaddrlen);
    if (comm_fd != -1)
    {
      if (vflag)
      {
        fprintf(stderr, "[%d] New frontend server connection\n", comm_fd);
      }
      // new frontend server connection; add to map
      std::string new_server = inet_ntoa(clientaddr.sin_addr);
      std::string server_port = new_server + ":" + std::to_string(ntohs(clientaddr.sin_port));
      server_clients[server_port] = 0;

      // create new thread for this specific frontend server connection
      pthread_t temp_thread;
      ConnectionInfo *connection_info = new ConnectionInfo(comm_fd, server_port);
      if (pthread_create(&temp_thread, NULL, frontendThread, connection_info) != 0)
      {
        fprintf(stderr, "pthread_create error");
        return -1;
      }
    }

    // accept new user connection
    comm_fd = accept(u_sockfd, (struct sockaddr *)&clientaddr, &clientaddrlen);
    if (comm_fd != -1)
    {
      if (vflag)
      {
        fprintf(stderr, "[%d] New client connection\n", comm_fd);
      }

      std::string min_server;
      int min_clients = INT_MAX;
      std::map<std::string, int>::iterator it;
      // iterate through map to find server with fewest client connections
      for (it = server_clients.begin(); it != server_clients.end(); it++)
      {
        if (it->second < min_clients)
        {
          min_clients = it->second;
          min_server = it->first;
        }
      }      
      // redirect user to min_clients, min_server
      std::string init_response = "HTTP/1.0 301 Moved Permanently\r\nLocation: http://";
      std::string response = init_response + real_server_address[min_server] + "\r\n\r\n301 Moved Permanently";
      if (vflag)
      {
        fprintf(stderr, "min server is: %s with %d clients. real server address is %s \n", min_server.c_str(), min_clients, real_server_address[min_server].c_str());
        fprintf(stderr, "responding with: %s\n", response.c_str());
      }
      write(comm_fd, response.c_str(), strlen(response.c_str()));
      // server_clients[min_server]++;
    }
  }
}