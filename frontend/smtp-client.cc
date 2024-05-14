#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <cctype>
#include <signal.h>
#include <sstream>

int port = 2501;
bool debug_output = false;
volatile int fds[100] = {}; // File descriptors of clients
volatile pthread_t client_threads[100] = {};
int listenfd;
volatile bool shutting_down = false;

void siginthandler(int sig)
{
  // Write to all clients -ERR Server shutting down and kill all threads
  std::cerr << std::endl;
  shutting_down = true;
  for (int i = 0; i < 100; i++)
  {
    if (fds[i] != 0)
    {
      std::string response = "-ERR Server shutting down\r\n";
      write(fds[i], response.c_str(), response.length());
      if (debug_output)
      {
        std::cerr << "[" << i << "] S: "
                  << "-ERR Server shutting down\r\n";
      }
      pthread_cancel(client_threads[i]);
      if (debug_output)
      {
        std::cerr << "[" << i << "] Connection closed" << std::endl;
      }
    }
  }

  // Close all sockets
  close(listenfd);

  if (sig == -1) // Used for exiting without sigint
  {
    exit(EXIT_FAILURE);
  }
  exit(EXIT_SUCCESS);
}

struct ConnectionInfo
{
  int comm_fd;
  int thread_no;
  ConnectionInfo(int comm_fd, int thread_no) : comm_fd(comm_fd), thread_no(thread_no) {}
};

bool send_command(int sock, std::string request)
{
  char buffer_temp[1000];
  write(sock, request.c_str(), request.length());
  if (debug_output)
  {
    std::cerr << "S: " << request;
  }
  int bytes_read = read(sock, buffer_temp, 1000);
  buffer_temp[bytes_read] = '\0';
  std::string response = buffer_temp;
  if (debug_output)
  {
    std::cerr << "S: " << response;
  }
  if (response.find("250") == std::string::npos && response.find("220") == std::string::npos && response.find("354") == std::string::npos && response.find("221") == std::string::npos)
  {
    std::cerr << "Error in " << response << std::endl;
    return false;
  }
  return true;
}

void *connection_thread(void *args)
{
  bool sig_int = false;
  int fd = ((ConnectionInfo *)args)->comm_fd;
  int thread_no = ((ConnectionInfo *)args)->thread_no;
  std::string command;

  // Immediately terminate if shutting down
  if (shutting_down)
  {
    if (debug_output)
    {
      std::cerr << "[" << thread_no << "] Connection closed" << std::endl;
    }
    pthread_detach(client_threads[thread_no]);
    pthread_exit(NULL);
  }

  while (!sig_int)
  {
    char buffer[1000]; // TODO: might have to change
    int end_index = 0;
    bool has_full_command = false;
    int cr_index = -2;

    // Read from client until full line
    while (!has_full_command)
    {
      char c;
      if (read(fd, &c, 1) > 0)
      {
        buffer[end_index] = c;

        if (c == '\r')
        {
          cr_index = end_index;
        }

        if (c == '\n' && (cr_index == end_index - 1))
        {
          has_full_command = true;
          buffer[end_index - 1] = '\0'; // Replace \r with \0
          command = buffer;
        }

        end_index++;
      }
    }

    // Process command
    if (debug_output)
    {
      std::cerr << "[" << thread_no << "] C: "
                << command << "\r\n";
    }

    // Parse out mail from, rcpt to, and data
    // Remote server address \n
    // FROM \n
    // TO \n
    // DATA \r\n
    std::string line, ip_addr, from, to, data, domain;
    int ip_port;

    std::istringstream iss(command);
    int lines_read = 0;
    while (std::getline(iss, line, '\n'))
    {
      lines_read++;
      if (lines_read == 1)
      {
        ip_addr = line.substr(0, line.find(":"));
        ip_port = std::stoi(line.substr(line.find(":") + 1));
      }
      else if (lines_read == 2)
      {
        from = line;
      }
      else if (lines_read == 3)
      {
        to = line;
      }
      else
      {
        data += line + "\n";
      }
    }

    domain = from.substr(from.find("@") + 1, from.length() - from.find("@") - 1);

    // Connect to remote smtp server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr.c_str(), &servaddr.sin_addr);
    servaddr.sin_port = htons(ip_port);

    // Connect to server
    if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
      std::cerr << "Cannot connect to server" << std::endl;
    }


    // Send mail to server using smtp protocol

    // HELO [domain]
    if (!send_command(sock, "HELO " + domain + "\r\n"))
    {
      send(fd, "-ERR in HELO\r\n", strlen("-ERR in HELO\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    // MAIL FROM: [email]
    if (!send_command(sock, "MAIL FROM:<" + from + ">\r\n"))
    {
      send(fd, "-ERR in MAIL FROM\r\n", strlen("-ERR in MAIL FROM\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    // RCPT TO: [email]
    if (!send_command(sock, "RCPT TO:<" + to + ">\r\n"))
    {
      send(fd, "-ERR in RCPT TO\r\n", strlen("-ERR in RCPT TO\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    // DATA
    if (!send_command(sock, "DATA\r\n"))
    {
      send(fd, "-ERR in DATA\r\n", strlen("-ERR in DATA\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    // [message]
    if (!send_command(sock, data + "\r\n.\r\n" ))
    {
      send(fd, "-ERR in message\r\n", strlen("-ERR in message\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    // QUIT
    if (!send_command(sock, "QUIT\r\n"))
    {
      send(fd, "-ERR in QUIT\r\n", strlen("-ERR in QUIT\r\n"), 0);
      close(sock);
      sig_int = true;
      continue;
    }

    send(fd, "+OK Message sent\r\n", strlen("+OK Message sent\r\n"), 0);
    // Close connection
    close(sock);
    sig_int = true;    
  }

  // Close thread
  if (debug_output)
  {
    std::cerr << "[" << thread_no << "] Connection closed" << std::endl;
  }
  close(fd);
  fds[thread_no] = 0;
  pthread_detach(client_threads[thread_no]);
  pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
  // Parse command line options
  int opt = getopt(argc, argv, "p:av");
  while (opt != -1)
  {
    switch (opt)
    {
    case 'p':
      port = std::atoi(optarg);
      break;
    case 'a':
      std::cerr << "Anna Xia (annaxia)" << std::endl;
      exit(EXIT_SUCCESS);
    case 'v':
      debug_output = true;
      break;
    case '?':
      std::cerr << "Invalid arugment" << std::endl;
      exit(EXIT_FAILURE);
    }
    opt = getopt(argc, argv, "p:av");
  }

  // Create SIGINT singal
  if (signal(SIGINT, (__sighandler_t)siginthandler) < 0)
  {
    std::cerr << "Cannot create signal" << std::endl;
    exit(EXIT_FAILURE);
  }

  // Create a server socket and bind it to the listener
  listenfd = socket(PF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
  {
    std::cerr << "Cannot create socket" << std::endl;
    siginthandler(-1);
    exit(EXIT_FAILURE);
  }

  int sock_opt = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &sock_opt, sizeof(sock_opt)) < 0)
  {
    std::cerr << "Cannot setsockopt" << std::endl;
    siginthandler(-1);
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htons(INADDR_ANY);
  servaddr.sin_port = htons(port);

  if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    std::cerr << "Cannot bind" << std::endl;
    siginthandler(-1);
    exit(EXIT_FAILURE);
  }

  if (listen(listenfd, 100) < 0)
  {
    std::cerr << "Cannot listen" << std::endl;
    siginthandler(-1);
    exit(EXIT_FAILURE);
  }

  while (true)
  {
    // Accept a new connection
    struct sockaddr_in clientaddr;
    socklen_t clientaddrlen = sizeof(clientaddr);
    int comm_fd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientaddrlen);

    for (int i = 0; i < 100; i++)
    {
      if (fds[i] == 0)
      {

        // if (write(comm_fd, "+OK Server ready (Author: Anna Xia / annaxia)\r\n", strlen("+OK Server ready (Author: Anna Xia / annaxia)\r\n")) < 0)
        // {
        //   std::cerr << "Error in writing to client" << std::endl;
        //   siginthandler(-1);
        //   exit(EXIT_FAILURE);
        // }

        // Create a new thread for the connection
        pthread_t thread_id;
        ConnectionInfo *connection_info = new ConnectionInfo(comm_fd, i);

        if (!shutting_down)
        {
          if (pthread_create(&thread_id, NULL, connection_thread, connection_info) != 0)
          {
            std::cerr << "Error in creating thread" << std::endl;
            exit(EXIT_FAILURE);
          }
        }

        // Record file descriptor and thread id of new connection
        client_threads[i] = thread_id;
        fds[i] = comm_fd;

        if (debug_output)
        {
          std::cerr << "[" << i << "] "
                    << "New connection" << std::endl;
        }

        if (debug_output)
        {
          std::cerr << "[" << i << "] "
                    << "S: +OK Server ready (Author: Anna Xia / annaxia)\r\n";
        }

        break;
      }
    }
  }

  return 0;
}