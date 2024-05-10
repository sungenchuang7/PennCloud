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
#include "routes.h"

int port = 8080;
bool debug_output = false;
volatile int fds[100] = {}; // File descriptors of clients
volatile pthread_t client_threads[100] = {};
volatile pthread_t hb_thread;
volatile bool shutting_down = false;
int listenfd;
int MAX_BUFF = 1000;

struct ConnectionInfo
{
  int comm_fd;
  int thread_no;
  ConnectionInfo(int comm_fd, int thread_no) : comm_fd(comm_fd), thread_no(thread_no) {}
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

// Check if the line is a header, if so adds it to the headers map and returns true, else returns false.
bool is_header(std::string command, std::unordered_map<std::string, std::string> *headers)
{
  if (command.find(": ") == std::string::npos)
  {
    return false;
  }
  headers->insert(std::make_pair(command.substr(0, command.find(": ")), command.substr(command.find(": ") + 2)));
  return true;
}

void send_response(int fd, int thread_no, std::string init_response, std::string headers, std::string message_body)
{
  std::string response = init_response + headers + "\r\n" + message_body;
  write(fd, response.c_str(), response.length());
  if (debug_output)
  {
    std::cerr << "[" << thread_no << "] S: "
              << response << std::endl;
  }
}

void *heartbeat_thread(void *args)
{
  bool sig_int = false;
  int comm_fd = *(int *)args;
  // first message has to tell the address information for clients to connect to
  std::string addr = "127.0.0.1:" + std::to_string(port) + "\r\n"; // TODO: change to look at debug later
  write(comm_fd, addr.c_str(), strlen(addr.c_str()));

  if (shutting_down)
  {
    if (debug_output)
    {
      std::cerr << "heartbeat thread closed" << std::endl;
    }
    pthread_detach(hb_thread);
    pthread_exit(NULL);
  }

  while (!sig_int)
  {
    sleep(3); // sleep for 3 seconds
    char alive_msg[] = "+OK server alive!";
    write(comm_fd, alive_msg, strlen(alive_msg));
  }
  // Close thread
  if (debug_output)
  {
    std::cerr << "heartbeat thread closed" << std::endl;
  }
  close(comm_fd);
  pthread_detach(hb_thread);
  pthread_exit(NULL);
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

  bool req_init_line_read = false;
  bool end_of_headers_read = false;
  bool reading_message_body = false;
  std::string message_body_buf = "";

  ReqInitLine *req_init_line = new ReqInitLine("", "", "");
  std::unordered_map<std::string, std::string> headers;

  while (!sig_int)
  {
    char buffer[MAX_BUFF];
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
      req_init_line_read = true;
    }
    // Check for headers
    else if (is_header(command, &headers))
    {
    }
    // Check for end of headers
    else if (command == "")
    {
      end_of_headers_read = true;
      // Read message body using content-length header
      if (headers.find("Content-Length") != headers.end())
      {
        int content_length = std::stoi(headers["Content-Length"]);
        int total_bytes_read = 0;
        char header_buf[MAX_BUFF];
        while (total_bytes_read < content_length)
        {
          int bytes_to_read = std::min(MAX_BUFF, content_length - total_bytes_read);
          int bytes_read = read(fd, header_buf, bytes_to_read);
          total_bytes_read += bytes_read;
          // Might be missing null terminator
          message_body_buf += std::string(header_buf, bytes_read);
        }
      }

      // Construct and send response
      if (req_init_line->method == "GET" || req_init_line->method == "HEAD")
      {
        if (req_init_line->path == "/")
        {
          std::tuple<std::string, std::string, std::string> response = get_index(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path == "/signup")
        {
          std::tuple<std::string, std::string, std::string> response = get_signup(req_init_line);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path == "/home")
        {
          std::tuple<std::string, std::string, std::string> response = get_home(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path == "/inbox")
        {
          std::tuple<std::string, std::string, std::string> response = get_inbox(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/inbox/") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = get_inbox_message(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/storage") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = get_storage(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/file/") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = get_file(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/download") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = download_file(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/changepassword") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = get_change_password(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else if (req_init_line->path.find("/admin") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = get_admin(req_init_line, headers);
          if (req_init_line->method == "GET")
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
          }
          else
          {
            send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), "");
          }
        }
        else
        {
          std::string message_body = "404 Not Found";
          std::string init_response_line = req_init_line->version + " 404 Not Found\r\n";
          std::string headers = "";
          headers += "Content-Type: text/plain\r\n";
          headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
          send_response(fd, thread_no, init_response_line, headers, message_body);
        }
      }
      else if (req_init_line->method == "POST")
      {
        if (debug_output)
        {
          std::cerr << "[" << thread_no << "] Message body: " << message_body_buf << std::endl;
        }
        if (req_init_line->path == "/login")
        {
          std::tuple<std::string, std::string, std::string> response = post_login(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path == "/signup")
        {
          std::tuple<std::string, std::string, std::string> response = post_signup(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path == "/send")
        {
          std::tuple<std::string, std::string, std::string> response = post_send_message(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path == "/delete-email")
        {
          std::tuple<std::string, std::string, std::string> response = post_delete_message(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/upload") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = post_file(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/new_folder") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = post_folder(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/delete") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = delete_file(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/rename") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = rename_file(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/move") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = move_file(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/changepassword") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = post_change_password(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/download") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = download_file(req_init_line, headers);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/kill-server") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = post_kill_server(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else if (req_init_line->path.find("/restart-server") != std::string::npos)
        {
          std::tuple<std::string, std::string, std::string> response = post_restart_server(req_init_line, headers, message_body_buf);
          send_response(fd, thread_no, std::get<0>(response), std::get<1>(response), std::get<2>(response));
        }
        else
        {
          std::string message_body = "404 Not Found";
          std::string init_response_line = req_init_line->version + " 404 Not Found\r\n";
          std::string headers = "";
          headers += "Content-Type: text/plain\r\n";
          headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
          send_response(fd, thread_no, init_response_line, headers, message_body);
        }
      }
      else
      {
        std::string message_body = "501 Not Implemented";
        std::string init_response_line = req_init_line->version + " 501 Not Implemented\r\n";
        std::string headers = "";
        headers += "Content-Type: text/plain\r\n";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        send_response(fd, thread_no, init_response_line, headers, message_body);
      }

      // Reset variables
      req_init_line_read = false;
      end_of_headers_read = false;
      reading_message_body = false;
      message_body_buf = "";
      headers.clear();
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

  // Create a socket to connect to load balancer
  int lbfd = socket(PF_INET, SOCK_STREAM, 0);
  if (lbfd < 0)
  {
    std::cerr << "Cannot create socket" << std::endl;
    siginthandler(-1);
    exit(EXIT_FAILURE);
  }

  // Connect to load balancer
  struct sockaddr_in lbservaddr;
  bzero(&lbservaddr, sizeof(lbservaddr));
  lbservaddr.sin_family = AF_INET;
  lbservaddr.sin_port = htons(5000);
  std::string lbip = "127.0.0.1"; // TODO: Change to config file later
  inet_pton(AF_INET, lbip.c_str(), &(lbservaddr.sin_addr));
  connect(lbfd, (struct sockaddr *)&lbservaddr, sizeof(lbservaddr));

  // create heartbeat thread
  if (!shutting_down)
  {
    pthread_t hb_thread_info;
    if (pthread_create(&hb_thread_info, NULL, heartbeat_thread, &lbfd) < 0)
    {
      std::cerr << "Error in creating thread" << std::endl;
      siginthandler(-1);
      exit(EXIT_FAILURE);
    }
    hb_thread = hb_thread_info;
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