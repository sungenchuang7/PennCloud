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

void *frontendThread(void *arg) {
  //start by blocking all SIGINT signals in the threads
  int comm_fd = *(int *) arg;

  // TODO: heartbeat check

  pthread_exit(NULL);
}

// takes in -p command specifying which port the server should be hosted on for frontend servers to connect to
int main(int argc, char *argv[])
{
  // push some stuff onto server_clients to test
  server_clients["first"] = 0;
  server_clients["second"] = 0;
  server_clients["third"] = 0;

  int c; // var for getopt
  bool vflag = false;
  int p = 5000; // default port is 5000
	while ((c = getopt (argc, argv, "vp:")) != -1)
		switch (c)
		{
			case 'v':
				vflag = true;
				break;
      case 'p':
        p = atoi(optarg);
        break;
			case '?':
        if (optopt == 'p')
					fprintf(stderr, "Option -%c requires an argument. \n", optopt);
        else if (isprint (optopt))
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
  u_servaddr.sin_port = htons(8000);

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
      if (pthread_create(&temp_thread, NULL, frontendThread, &comm_fd) != 0) {
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
      fprintf(stderr, "min server is: %s with %d clients \n", min_server.c_str(), min_clients);
      server_clients[min_server]++;
      std::string msg = "HTTP/1.1 301 Moved Permanently\nLocation: https://www.google.com/";
      write(comm_fd, msg.c_str(), strlen(msg.c_str()));
    }
  }
}