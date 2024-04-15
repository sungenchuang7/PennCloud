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
static bool vflag = false;

struct ConnectionInfo
{
  int comm_fd;
  std::string server_address;
  ConnectionInfo(int comm_fd, std::string server_address) : comm_fd(comm_fd), server_address(server_address) {}
};

void *frontendThread(void *args) {
  int comm_fd = ((ConnectionInfo *)args)->comm_fd;
  std::string addr = ((ConnectionInfo *)args)->server_address;
  int total_chars = 0;
  while(true) {
    //read 100 bytes into a read buffer
    char* read_buffer = (char*) malloc(100);
    bzero(read_buffer, 100);
    int num_read = read(comm_fd, read_buffer, 100);
    if (num_read < 0) {
      fprintf(stderr, "read failed!\n");
      exit(1);
    }
    if (num_read == 0) {
      // this means the client has closed the connection or died
      // remove client from server_clients for now 
      server_clients.erase(addr);

      free(read_buffer);
      close(comm_fd);
      if (vflag) {
        fprintf(stderr, "[%d] Connection closed\n", comm_fd);
      }
      pthread_exit(NULL);
    }
    // don't really care what this message is as long as it exists, so just erase read_buffer
    if (vflag){
      fprintf(stderr, "Message received: %s\n", read_buffer);
    }
    bzero(read_buffer, 100);
  }

  pthread_exit(NULL);
}

// takes in -p command specifying which port the server should be hosted on for frontend servers to connect to
int main(int argc, char *argv[])
{
  // push some stuff onto server_clients to test
  server_clients["www.google.com"] = 0;
  server_clients["www.youtube.com"] = 0;
  server_clients["www.yahoo.com"] = 0;

  int c; // var for getopt
  int p = 5000; // default port for frontend servers to connect to is 5000
	while ((c = getopt (argc, argv, "v")) != -1)
		switch (c)
		{
			case 'v':
				vflag = true;
				break;
			case '?':
        if (isprint (optopt))
					fprintf(stderr, "Unknown option '-%c'.\n", optopt);
				else
					fprintf(stderr, "Unknown option character '\\x%x',\n", optopt);
				return 1;
			default:
				abort();
		}


  //initialize frontend socket for servers

  int sockfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (sockfd < 0) {
    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
    exit(1);
  }

  //bind frontned socket to specified port

  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));

  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  servaddr.sin_port = htons(p);

  int opt = 1;
  int ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR|SO_REUSEPORT, &opt, sizeof(opt));
  if (ret < 0) {
    fprintf(stderr, "setsockopt failed\n");
    exit(1);
  }

  ret = bind(sockfd, (struct sockaddr*) &servaddr, sizeof(servaddr));
  if (ret < 0) {
    fprintf(stderr, "failed to bind (%s)\n", strerror(errno));
    exit(1);
  }

  //start listening on frontend socket
  listen(sockfd, 100);

  // initialize user socket
  int u_sockfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (u_sockfd < 0) {
    fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
    exit(1);
  }

  //bind user socket to specific port 

  struct sockaddr_in u_servaddr;
  bzero(&u_servaddr, sizeof(u_servaddr));

  u_servaddr.sin_family = AF_INET;
  u_servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  u_servaddr.sin_port = htons(8000); // default port for user socket is 8000

  int u_opt = 1;
  ret = setsockopt(u_sockfd, SOL_SOCKET, SO_REUSEADDR|SO_REUSEPORT, &u_opt, sizeof(u_opt));
  if (ret < 0) {
    fprintf(stderr, "setsockopt failed\n");
    exit(1);
  }

  ret = bind(u_sockfd, (struct sockaddr*) &u_servaddr, sizeof(u_servaddr));
  if (ret < 0) {
    fprintf(stderr, "Failed to bind user socket (%s)\n", strerror(errno));
    exit(1);
  }

  // start listening on user socket 
  listen(u_sockfd, 100);

  while(true) {
    struct sockaddr_in clientaddr;
    socklen_t clientaddrlen = sizeof(clientaddr);
    // accept frontend server connection
    int comm_fd = accept(sockfd, (struct sockaddr*) &clientaddr, &clientaddrlen);
    if (comm_fd != -1) {
      if (vflag) {
        fprintf(stderr, "[%d] New frontend server connection\n", comm_fd);
      }
      // new frontend server connection; add to map 
      std::string new_server = inet_ntoa(clientaddr.sin_addr);
      std::string server_port = new_server + ":" + std::to_string(ntohs(clientaddr.sin_port));
      server_clients[server_port] = 0;

      // create new thread for this specific frontend server connection 
      pthread_t temp_thread;
      ConnectionInfo *connection_info = new ConnectionInfo(comm_fd, server_port);
      if (pthread_create(&temp_thread, NULL, frontendThread, connection_info) != 0) {
          fprintf(stderr, "pthread_create error");
          return -1;
      }
    }

    // accept new user connection 
    comm_fd = accept(u_sockfd, (struct sockaddr*) &clientaddr, &clientaddrlen); 
    if (comm_fd != -1) {
      if (vflag) {
        fprintf(stderr, "[%d] New client connection\n", comm_fd);
      }

      std::string min_server;
      int min_clients = INT_MAX;
      std::map<std::string, int>::iterator it;
      //iterate through map to find server with fewest client connections 
      for (it = server_clients.begin(); it != server_clients.end(); it++) {
        if (it->second < min_clients) {
          min_clients = it->second; 
          min_server = it->first; 
        }
      }
      // redirect user to min_clients, min_server 
      std::string init_response = "HTTP/1.0 301 Moved Permanently\r\nLocation: http://";
      std::string response = init_response + min_server + "\r\n\r\n301 Moved Permanently";
      write(comm_fd, response.c_str(), strlen(response.c_str()));
      if (vflag) {
        fprintf(stderr, "min server is: %s with %d clients \n", min_server.c_str(), min_clients);
        fprintf(stderr, "responding with: %s\n", response.c_str());
      }
      server_clients[min_server]++;
    }
  }
}