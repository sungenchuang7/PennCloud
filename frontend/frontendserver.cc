#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <cctype>
#include <signal.h>
#include <sys/file.h>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <sstream>
#include <string>
#include <map>
#include <iomanip>

int port = 8080;
bool debug_output = false;
volatile int fds[100] = {}; // File descriptors of clients
volatile pthread_t client_threads[100] = {};
volatile bool shutting_down = false;
int listenfd;

struct ConnectionInfo
{
  int comm_fd;
  int thread_no;
  ConnectionInfo(int comm_fd, int thread_no) : comm_fd(comm_fd), thread_no(thread_no) {}
};

struct ReqInitLine
{
  std::string method;
  std::string path;
  std::string version;
  ReqInitLine(std::string method, std::string uri, std::string version) : method(method), path(path), version(version) {}
};


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

// Check if the command is a request initial line, if so return true and a ReqInitLine object, else return false.
bool is_req_init_line(std::string command, ReqInitLine *req_init_line)
{
  std::istringstream iss(command);
  std::vector<std::string> values;
  std::string value;

  while (iss >> value)
  {
    values.push_back(value);
  }

  if (values.size() != 3)
  {
    return false;
  }

  std::string method = values[0];
  if (method != "GET" && method != "POST" && method != "HEAD")
  {
    return false;
  }

  req_init_line->method = method;
  req_init_line->path = values[1];
  req_init_line->version = values[2];
  return true;
}

bool is_header(std::string command, std::unordered_map<std::string, std::string> *headers)
{
  if (command.find(": ") == std::string::npos)
  {
    return false;
  }
  headers->insert(std::make_pair(command.substr(0, command.find(": ")), command.substr(command.find(": ") + 2)));
  return true;
}

std::tuple<std::string, std::string, std::string> get_index(ReqInitLine *req_init_line)
{
  // Read in HTML file
  std::ifstream file("index.html");
  std::string message_body;

  if (file.is_open())
  {
    std::string line;
    while (getline(file, line))
    {
      message_body += line + "\n";
    }
    file.close();
  }
  else
  {
    std::string response = req_init_line->version + " 404 Not Found\r\n";
    return std::make_tuple(response, "", "");
  }

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;
  // TODO: Date header

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}

void send_response(int fd, int thread_no, std::string init_response, std::string headers, std::string message_body)
{
  std::string response = init_response + headers + "\r\n" + message_body;
  write(fd, response.c_str(), response.length());
  if (debug_output)
  {
    std::cerr << "[" << thread_no << "] S: "
              << response;
  }
}
// TODO: If there is time, place all routes in a map and call the appropriate function

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

  bool req_init_line_read = false;
  bool end_of_headers_read = false;
  bool reading_message_body = false;
  std::string message_body_buf = "";

  ReqInitLine *req_init_line = new ReqInitLine("", "", "");
  std::unordered_map<std::string, std::string> headers;

  while (!sig_int)
  {
    char buffer[1000];
    int end_index = 0;
    bool has_full_command = false;
    int cr_index = -2;

    // Read from client until full line that ends with \r\n
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

    // Process commands

    if (debug_output)
    {
      std::cerr << "[" << thread_no << "] C: "
                << command << "\r\n";
    }

    // Check for inital line of request

    if (is_req_init_line(command, req_init_line))
    {
      // // GET
      // if (req_init_line->method == "GET")
      // {
      //   // Get the resource from req_init_line->path
      //   // TODO: Move to a get function
      //   // TODO: Cannot handle binary files atm not sure if we need to
      //   std::ifstream file(req_init_line->path);
      //   std::string resource;
      //   if (file.is_open())
      //   {
      //     std::string line;
      //     while (getline(file, line))
      //     {
      //       resource += line + "\n";
      //     }
      //     file.close();
      //   }
      //   else
      //   {
      //     std::string response = req_init_line->version + " 404 Not Found\r\n";
      //     write(fd, response.c_str(), response.length());
      //     if (debug_output)
      //     {
      //       std::cerr << "[" << thread_no << "] S: "
      //                 << response;
      //     }
      //     continue;
      //   }

      //   // Status Line
      //   std::string response = req_init_line->version + " 200 OK\r\n";

      //   // Headers
      //   // TODO: may need Date:, Content-Type:
      //   response += "Content-Length: " + std::to_string(resource.length()) + "\r\n"; // Length of message after transfer encodings applied
      //   response += "/r/n";

      //   // Message Body
      //   response += resource;

      //   write(fd, response.c_str(), response.length());
      //   if (debug_output)
      //   {
      //     std::cerr << "[" << thread_no << "] S: "
      //               << response;
      //   }
      // }
      // // POST
      // else if (req_init_line->method == "POST")
      // {
      //   std::string response = req_init_line->version + " 501 Not Implemented\r\n";
      //   write(fd, response.c_str(), response.length());
      //   if (debug_output)
      //   {
      //     std::cerr << "[" << thread_no << "] S: "
      //               << response;
      //   }
      // }
      // // HEAD
      // else if (req_init_line->method == "HEAD")
      // {
      // }
    }
    // Check for headers
    else if (is_header(command, &headers))
    {
    }
    // Check for end of headers
    else if (command == "")
    {

      // Read message body using content-length header
      if (headers.find("Content-Length") != headers.end())
      {
        int content_length = std::stoi(headers["Content-Length"]);
        int total_bytes_read = 0;
        char header_buf[1000];
        while (total_bytes_read < content_length)
        {
          int bytes_to_read = content_length - total_bytes_read;
          int bytes_read = read(fd, header_buf, bytes_to_read);
          total_bytes_read += bytes_read;
          // Might be missing null terminator
          message_body_buf += std::string(header_buf, bytes_read);
        }
      }

      // Construct and send response
      if (req_init_line->method == "GET")
      {
        if (req_init_line->path == "/")
        {
          std::tuple<std::string, std::string, std::string> response = get_index(req_init_line);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else
        {
          // TODO: Add headers
          std::string response = req_init_line->version + " 404 Not Found\r\n";
          write(fd, response.c_str(), response.length());
          if (debug_output)
          {
            std::cerr << "[" << thread_no << "] S: "
                      << response;
          }
        }
      }
      else if (req_init_line->method == "POST")
      {
        std::string response = req_init_line->version + " 501 Not Implemented\r\n";
        write(fd, response.c_str(), response.length());
        if (debug_output)
        {
          std::cerr << "[" << thread_no << "] S: "
                    << response;
        }
      }
      else if (req_init_line->method == "HEAD")
      {
        std::string response = req_init_line->version + " 501 Not Implemented\r\n";
        write(fd, response.c_str(), response.length());
        if (debug_output)
        {
          std::cerr << "[" << thread_no << "] S: "
                    << response;
        }
      }
    }
    else
    {
      std::string response = "400 Bad Request\r\n";
      write(fd, response.c_str(), response.length());
      if (debug_output)
      {
        std::cerr << "[" << thread_no << "] S: "
                  << "400 Bad Request\r\n";
      }
    }
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
      std::cerr << "Team 13" << std::endl;
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
    siginthandler(-1);
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
  std::string ip = "127.0.0.1"; // TODO: Change to config file later
  inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);

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

  if (debug_output)
  {
    std::cerr << "Listening on " << inet_ntoa(servaddr.sin_addr) << ":" << port << std::endl;
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
        // Create a new thread for the connection
        pthread_t thread_id;
        ConnectionInfo *connection_info = new ConnectionInfo(comm_fd, i);

        if (!shutting_down)
        {
          if (pthread_create(&thread_id, NULL, connection_thread, connection_info) != 0)
          {
            std::cerr << "Error in creating thread" << std::endl;
            siginthandler(-1);
            exit(EXIT_FAILURE);
          }
        }

        // Record file descriptor and thread id of new connection
        client_threads[i] = thread_id;
        fds[i] = comm_fd;

        if (debug_output)
        {
          std::cerr << "[" << i << "] "
                    << "New connection: " << inet_ntoa(clientaddr.sin_addr) << ":" << std::to_string(ntohs(clientaddr.sin_port)) << std::endl;
        }

        break;
      }
    }
  }

  return 0;
}
