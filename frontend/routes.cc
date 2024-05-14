#include "routes.h"
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
#include <uuid/uuid.h>
#include <openssl/md5.h>
#include <algorithm>
#include <resolv.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <netdb.h>

std::string STATICS_LOC = "./statics/";
int MAX_BUFF_SIZE = 1000;
static std::map<std::string, std::string> usernames;
std::string MASTER_SERVER_ADDR = "127.0.0.1";
int MASTER_SERVER_PORT = 20000;

struct Email
{
  std::string message_id;
  std::string subject;
  std::string sender;
  std::string arrival_time;
};

struct File
{
  std::string file_id;
  std::string name;
  bool is_directory;
};

void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer)
{
  /* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */

  MD5_CTX c;
  MD5_Init(&c);
  MD5_Update(&c, data, dataLengthBytes);
  MD5_Final(digestBuffer, &c);
}

// Returns a map of cookies {cookie_key, cookie_value}
std::unordered_map<std::string, std::string> parse_cookies(std::string cookies)
{
  std::unordered_map<std::string, std::string> cookie_map;
  std::string cookies_remove_whitespace;
  for (char c : cookies)
  {
    if (c != ' ')
    {
      cookies_remove_whitespace += c;
    }
  }
  std::istringstream ss(cookies_remove_whitespace);
  std::string cookie_key;
  while (std::getline(ss, cookie_key, ';'))
  {
    size_t pos = cookie_key.find("=");
    std::string key = cookie_key.substr(0, pos);
    std::string value = cookie_key.substr(pos + 1);
    cookie_map[key] = value;
  }
  return cookie_map;
}

// BACKEND GET ROUTES

// Ping master node for backend server address
std::string get_backend_address(std::string rowkey)
{
  // Create socket connection with server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(MASTER_SERVER_PORT);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    return "--ERR failed to connect to backend server";
  }

  // Read for +OK server ready
  std::string command;
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command.length() < 3 || command.substr(0, 3) != "+OK")
  {
    return "--ERR failed to connect to backend server: " + command;
  }

  // Make INIT, request
  std::string request = "INIT," + rowkey + "\r\n";
  write(sock, request.c_str(), request.length());
  // Read response from server
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  // Handle errors
  if (command.length() < 5 || command.substr(0, 5) != "RDIR,")
  {
    return "--ERR INIT failed: " + command;
  }
  // Close connection
  close(sock);
  // Return backend server address
  return command.substr(5);
}
std::vector<std::tuple<std::string, std::string>> get_backend_servers_status()
{
  // Create socket connection with server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(MASTER_SERVER_PORT);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    return {};
  }

  // Read for +OK server ready
  std::string command;
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command.length() < 3 || command.substr(0, 3) != "+OK")
  {
    return {};
  }

  // Make STAT, request
  std::string request = "STAT\r\n";
  write(sock, request.c_str(), request.length());
  // Read response from server
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  // Close connection
  close(sock);

  if (command.length() < 3 || command.substr(0, 3) != "+OK")
  {
    return {};
  }

  // Parse response

  // Remove +OK, from response
  command = command.substr(4);

  // Split response by ,
  std::vector<std::tuple<std::string, std::string>> servers;
  std::string delimiter = ",";
  size_t pos = 0;
  std::string token;
  while ((pos = command.find(delimiter)) != std::string::npos)
  {
    token = command.substr(0, pos);
    std::string status;
    if (token.substr(token.length() - 1) == "0")
    {
      status = "Down";
    }
    else
    {
      status = "Alive";
    }
    servers.push_back(std::make_tuple(token.substr(0, token.length() - 2), status));
    command.erase(0, pos + delimiter.length());
  }

  return servers;
}
std::vector<std::tuple<std::string, std::string, std::string>> get_frontend_servers_status()
{
  // Create socket connection with loadbalancer
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in lbaddr;
  bzero(&lbaddr, sizeof(lbaddr));
  lbaddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &lbaddr.sin_addr);
  lbaddr.sin_port = htons(5000);

  // Connect to loadbalancer
  if (connect(sock, (struct sockaddr *)&lbaddr, sizeof(lbaddr)) < 0)
  {
    return {};
  }

  // Send STAT
  std::string request = "STAT\r\n";
  write(sock, request.c_str(), request.length());

  // Read response
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;
  std::string command;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  std::vector<std::tuple<std::string, std::string, std::string>> servers;
  // Parse response
  // 127.0.0.1:8080,1,1\n
  // 127.0.0.1:8081,1,1\n
  // 127.0.0.1:8082,1,0\n

  // Split response by \n
  std::string delimiter = "\n";
  size_t pos = 0;
  std::string token;
  while ((pos = command.find(delimiter)) != std::string::npos)
  {
    token = command.substr(0, pos);
    // std::cerr << "token: " << token << std::endl;
    std::string address = token.substr(0, token.find(","));
    token.erase(0, token.find(",") + 1);
    std::string status = token.substr(0, token.find(","));
    token.erase(0, token.find(",") + 1);
    std::string num_clients = token;
    servers.push_back(std::make_tuple(address, status, num_clients));
    command.erase(0, pos + delimiter.length());
  }

  // Return vector of tuples {server_address, status, num_clients}
  return servers;
}
std::vector<std::tuple<std::string, std::string>> get_kvs_pairs(int group)
{
  std::vector<std::tuple<std::string, std::string>> pairs;

  // Create socket connection with master server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in master_serveraddr;
  bzero(&master_serveraddr, sizeof(master_serveraddr));
  master_serveraddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &master_serveraddr.sin_addr);
  master_serveraddr.sin_port = htons(MASTER_SERVER_PORT);

  // Connect to master backend
  if (connect(sock, (struct sockaddr *)&master_serveraddr, sizeof(master_serveraddr)) < 0)
  {
    return {};
  }

  // Ping master server for backend server address for each replica group
  std::string backend_addr;
  if (group == 1)
  {
    backend_addr = get_backend_address("a");
  }
  else if (group == 2)
  {
    backend_addr = get_backend_address("j");
  }

  else if (group == 3)
  {
    backend_addr = get_backend_address("s");
  }

  if (backend_addr.substr(0, 5) == "--ERR")
  {
    return {};
  }

  int backend_port = std::stoi(backend_addr.substr(backend_addr.find(":") + 1));
  // Make a PAIR request to backend server
  std::string request = "PAIR\r\n";

  int s = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in serveraddr;
  bzero(&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &serveraddr.sin_addr);
  serveraddr.sin_port = htons(backend_port);

  if (connect(s, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
  {
    return {};
  }

  write(s, request.c_str(), request.length());

  // Read response from server
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;
  std::string command;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(s, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }
  // std::cerr << "command: " << command << std::endl;

  if (command.length() < 3 || command.substr(0, 3) != "+OK")
  {
    return {};
  }

  // Read for kv pairs
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(s, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  // std::cerr << "command: " << command << std::endl;

  // Remove +OK from response
  if (command.length() > 4)
  {
    command = command.substr(4);
    // Parse response
    // Split response by :
    std::string delimiter = ":";
    size_t pos = 0;
    std::string token;
    while ((pos = command.find(delimiter)) != std::string::npos)
    {
      token = command.substr(0, pos);
      // Split token by ,
      std::string row = token.substr(0, token.find(","));
      std::string col = token.substr(token.find(",") + 1);
      pairs.push_back(std::make_tuple(row, col));
      command.erase(0, pos + delimiter.length());
    }

    if (command.length() > 0)
    {
      // Split token by ,
      std::string row = command.substr(0, command.find(","));
      std::string col = command.substr(command.find(",") + 1);
      pairs.push_back(std::make_tuple(row, col));
    }
  }
  // for (auto pair : pairs)
  // {
  //   std::cerr << std::get<0>(pair) << " " << std::get<1>(pair) << std::endl;
  // }
  close(s);

  // Return vector of tuples {row, col, value}
  return pairs;
}
// Make a get request to the backend server for value of key
// Returns error message if request fails
std::string get_kvs(std::string ip, int port, std::string row, std::string col)
{
  // Create socket connection with server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(port);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    return "--ERR failed to connect to backend server";
  }

  // Make get request to server
  std::string request = "GET:" + row + ":" + col + "\r\n";
  write(sock, request.c_str(), request.length());

  std::string command;
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command == "+OK Server ready")
  {
    // Read from server for +OK<CRLF>
    bzero(buffer, MAX_BUFF_SIZE);
    has_full_command = false;
    end_index = 0;

    while (!has_full_command)
    {
      char c;
      if (read(sock, &c, 1) > 0)
      {
        buffer[end_index] = c;

        if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
        {
          has_full_command = true;
          buffer[end_index - 1] = '\0'; // Replace \r\n with \0
          command = buffer;
        }

        end_index++;
      }
    }
  }

  // Check if get request returned +OK
  if (command != "+OK")
  {
    return "--ERR failed to get value from backend server: " + command;
  }
  // Read from server until <CRLF>.<CRLF>
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;
  std::vector<char> vec;
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      vec.push_back(c);

      if (end_index >= 4 && c == '\n' && vec[end_index - 1] == '\r' && vec[end_index - 2] == '.' && vec[end_index - 3] == '\n' && vec[end_index - 4] == '\r')
      {
        has_full_command = true;
        vec.pop_back();
        vec.pop_back();
        vec.pop_back();
        vec.pop_back();
        vec.pop_back();
        command = std::string(vec.begin(), vec.end());
        // std::cout << "vec as hex: " << std::endl;
        // for (char c : vec)
        // {
        //   std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)c << " ";
        // }
        // command = buffer;
      }

      end_index++;
    }
  }

  // Close connection
  close(sock);
  // std::cout << "command buffer as hex: before return" << std::endl;
  // for (char c : command)
  // {
  //   std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)c << " ";
  // }
  return command;
}

// Make a put or cput request to the backend server
// Returns error message if request fails
std::string put_kvs(std::string ip, int port, std::string row, std::string col, std::string value, bool is_cput, std::string prev_value)
{
  // Create socket connection with server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(port);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    return "--ERR failed to connect to backend server";
  }

  // Read for +OK
  std::string command;
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command != "+OK Server ready")
  {
    return "--ERR failed to connect to backend server";
  }

  // Make a (c)put request to server
  if (is_cput)
  {
    std::string request = "CPUT:" + row + ":" + col + "\r\n";
    write(sock, request.c_str(), request.length());
  }
  else
  {
    std::string request = "PUT:" + row + ":" + col + "\r\n";
    write(sock, request.c_str(), request.length());
  }

  // Read for +OK Send value with DATA
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command != "+OK Send value with DATA" && command != "+OK Send first value with DATA")
  {
    return "--ERR failed to create (C)PUT request: " + command;
  }

  // If is cput, send previous value to server
  if (is_cput)
  {
    // Send data to server
    std::string request = "DATA\r\n";
    write(sock, request.c_str(), request.length());
    // Read for +OK
    bzero(buffer, MAX_BUFF_SIZE);
    has_full_command = false;
    end_index = 0;

    while (!has_full_command)
    {
      char c;
      if (read(sock, &c, 1) > 0)
      {
        buffer[end_index] = c;

        if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
        {
          has_full_command = true;
          buffer[end_index - 1] = '\0'; // Replace \r\n with \0
          command = buffer;
        }

        end_index++;
      }
    }

    if (command != "+OK Enter value ending with <CRLF>.<CRLF>")
    {
      return "--ERR failed to send DATA to backend server: " + command;
    }
    // Write prev_value to server ending in <CRLF>.<CRLF>
    request = prev_value + "\r\n.\r\n";
    write(sock, request.c_str(), request.length());
    // Read for +OK
    bzero(buffer, MAX_BUFF_SIZE);
    has_full_command = false;
    end_index = 0;

    while (!has_full_command)
    {
      char c;
      if (read(sock, &c, 1) > 0)
      {
        buffer[end_index] = c;

        if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
        {
          has_full_command = true;
          buffer[end_index - 1] = '\0'; // Replace \r\n with \0
          command = buffer;
        }

        end_index++;
      }
    }
    if (command != "+OK Enter second value with DATA")
    {
      return "--ERR failed to store value in backend server: " + command;
    }
  }

  // Send data to server
  std::string request = "DATA\r\n";
  write(sock, request.c_str(), request.length());
  // Read for +OK
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command != "+OK Enter value ending with <CRLF>.<CRLF>")
  {
    return "--ERR failed to send DATA to backend server: " + command;
  }

  // Write new value to server ending in <CRLF>.<CRLF>
  request = value + "\r\n.\r\n";
  write(sock, request.c_str(), request.length());
  // Read for +OK
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }
  if (command != "+OK Value added")
  {
    return "--ERR failed to store value in backend server: " + command;
  }

  // Close connection
  close(sock);
  return "+OK Value added";
}

std::string delete_kvs(std::string ip, int port, std::string row, std::string col)
{
  // Create socket connection with server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(port);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    return "--ERR failed to connect to backend server";
  }

  // Make get request to server
  std::string request = "DELE:" + row + ":" + col + "\r\n";
  write(sock, request.c_str(), request.length());

  std::string command;
  char buffer[MAX_BUFF_SIZE];
  int end_index = 0;
  bool has_full_command = false;

  // Read from server for line that ends in <CRLF>
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 1] = '\0'; // Replace \r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  if (command == "+OK Server ready")
  {
    // Read from server for +OK<CRLF>
    bzero(buffer, MAX_BUFF_SIZE);
    has_full_command = false;
    end_index = 0;

    while (!has_full_command)
    {
      char c;
      if (read(sock, &c, 1) > 0)
      {
        buffer[end_index] = c;

        if (end_index >= 1 && c == '\n' && buffer[end_index - 1] == '\r')
        {
          has_full_command = true;
          buffer[end_index - 1] = '\0'; // Replace \r\n with \0
          command = buffer;
        }

        end_index++;
      }
    }
  }

  // Check if get request returned +OK
  if (command.find("+OK") != 0)
  {
    return "--ERR failed to get value from backend server: " + command;
  }
  // Close connection
  close(sock);
  return command;
}
// FRONTEND GET ROUTES
std::tuple<std::string, std::string, std::string> get_index(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Read in HTML file
  std::ifstream file(STATICS_LOC + "index.html");
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

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Check if user has a cookie, if not generate a random one
  if (req_headers.find("Cookie") == req_headers.end() || req_headers["Cookie"].find("sid") == std::string::npos)
  {
    uuid_t uuid;
    uuid_generate(uuid);
    char uuid_str[37];
    uuid_unparse(uuid, uuid_str);
    headers += "Set-Cookie: sid=" + std::string(uuid_str) + "; Path=/\r\n";
    // set username for cookie to empty string
    usernames[std::string(uuid_str)] = "";
  }

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_signup(ReqInitLine *req_init_line)
{
  // Read in HTML file
  std::ifstream file(STATICS_LOC + "signup.html");
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

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_home(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Read in HTML file
  std::ifstream file(STATICS_LOC + "home.html");
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

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_inbox(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Get username
  std::string sid = cookies["sid"];
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("email_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // Get email metadata
  std::string email_metadata = get_kvs(backend_address, backend_port, "email_" + username, "metadata.txt");
  if (email_metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::cerr << "Email metadata: " << email_metadata << std::endl;
  std::unordered_map<std::string, Email> emails = {};

  // Add email data to emails map
  std::istringstream ss(email_metadata);
  std::string message_id;
  while (std::getline(ss, message_id, '\n'))
  {
    if (message_id != "Metadata")
    {
      // Ping backend master for backend server address
      std::string backend_address_port = get_backend_address("email_" + username);
      if (backend_address_port.substr(0, 5) == "--ERR")
      {
        // server group is down! return 503 error
        std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
        std::string message_body = "Servers are down. Please try again later.";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
      std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
      int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
      std::cerr << "Message ID: " << message_id << std::endl;
      std::string email_data = get_kvs(backend_address, backend_port, "email_" + username, message_id + ".txt");
      if (email_data.substr(0, 5) == "--ERR")
      {
        // failed to connect to backend! return an error
        std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
        std::string message_body = "An error occurred. Please try again later.";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
      std::istringstream ss(email_data);
      std::string line, arrival_time, sender, recipient, subject, message;
      while (std::getline(ss, line, '\n'))
      {
        if (line.find("DATE: ") == 0)
        {
          arrival_time = line.substr(6);
        }
        else if (line.find("FROM: ") == 0)
        {
          sender = line.substr(6);
        }
        else if (line.find("TO: ") == 0)
        {
          recipient = line.substr(4);
        }
        else if (line.find("SUBJECT: ") == 0)
        {
          subject = line.substr(9);
        }
        else if (line.find("MESSAGE: ") == 0)
        {
          message = line.substr(9);
        }
      }
      emails[message_id] = {message_id, subject, sender, arrival_time};
    }
  }

  // Sort by arrival time
  std::vector<std::pair<std::string, Email>> sorted_emails(emails.begin(), emails.end());
  std::sort(sorted_emails.begin(), sorted_emails.end(), [](const std::pair<std::string, Email> &a, const std::pair<std::string, Email> &b)
            { return a.second.arrival_time > b.second.arrival_time; });

  // Map of email messages {message_id, subject, sender, arrival_time}
  // std::unordered_map<std::string, Email> emails = {
  //     {"message1", {"message1", "Subject 1", "user1@localhost", "Sat Apr 13 00:00:00 2024"}},
  //     {"message2", {"message2", "Subject 2", "user2@localhost", "Sun Apr 14 00:00:00 2024"}},
  //     {"message3", {"message3", "Subject 3", "user3@localhost", "Sun Apr 15 00:00:00 2024"}}};

  // Create response and send HTML
  std::ifstream file(STATICS_LOC + "inbox.html");
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

  // Insert emails into HTML after <script> tag
  std::string insert_tag = "<script>";
  int insert_index = message_body.find(insert_tag);
  std::string email_script = "\nconst emails = [\n";
  for (auto const &email : sorted_emails)
  {
    email_script += "{";
    email_script += "message_id: \"" + email.second.message_id + "\",";
    email_script += "subject: \"" + email.second.subject + "\",";
    email_script += "sender: \"" + email.second.sender + "\",";
    email_script += "arrival_time: \"" + email.second.arrival_time + "\"";
    email_script += "},\n";
  }
  email_script += "];\n";
  message_body.insert(insert_index + insert_tag.length(), email_script);

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// Gets message with message_id /inbox/:message_id
std::tuple<std::string, std::string, std::string> get_inbox_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Get message_id from path
  std::string message_id = req_init_line->path.substr(7);
  std::cerr << "Message ID: " << message_id << std::endl;

  // Make call to backend for message with message_id
  std::string sid = cookies["sid"];
  std::string username = usernames[sid];
  std::cerr << "Username: " << username << std::endl;
  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("email_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
  std::string message = get_kvs(backend_address, backend_port, "email_" + username, message_id + ".txt");
  std::cerr << "Message: " << message << std::endl;
  if (message.find("--ERR") == 0)
  {
    std::string init_response = req_init_line->version + " 404 Not Found\r\n";
    std::string message_body = "No such message exists.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Create response and send HTML
  std::ifstream file(STATICS_LOC + "message.html");
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

  // Insert message into HTML
  std::string insert_tag = "<div class=\"email-container\">";
  int insert_index = message_body.find(insert_tag);
  std::string email_html = "\n<p id=\"email-text\">";
  for (int i = 0; i < message.length(); i++)
  {
    if (message[i] == '\n')
    {
      email_html += "<br>";
    }
    else
    {
      email_html += message[i];
    }
  }
  email_html += "</p>";
  message_body.insert(insert_index + insert_tag.length(), email_html);

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_change_password(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Read in HTML file
  std::ifstream file(STATICS_LOC + "changepass.html");
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

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_admin(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Read in HTML file
  std::ifstream file(STATICS_LOC + "admin.html");
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

  // Get statuses of backend servers
  std::vector<std::tuple<std::string, std::string>> backend_servers = get_backend_servers_status();

  std::string insert_tag = "<script>";
  int insert_index = message_body.find(insert_tag);
  std::string backend_server_script = "\nconst backend_servers = [\n";
  for (auto const &server : backend_servers)
  {
    backend_server_script += "{";
    backend_server_script += "server_address: \"" + std::get<0>(server) + "\",";
    backend_server_script += "status: \"" + std::get<1>(server) + "\",";
    backend_server_script += "},\n";
  }
  backend_server_script += "];\n";
  message_body.insert(insert_index + insert_tag.length(), backend_server_script);

  std::vector<std::tuple<std::string, std::string, std::string>> frontend_servers = get_frontend_servers_status();
  std::string frontend_server_script = "\nconst frontend_servers = [\n";
  for (auto const &server : frontend_servers)
  {
    frontend_server_script += "{";
    frontend_server_script += "server_address: \"" + std::get<0>(server) + "\",";
    frontend_server_script += "status: \"" + std::get<1>(server) + "\",";
    frontend_server_script += "num_connections: " + std::get<2>(server) + ",";
    frontend_server_script += "},\n";
  }
  frontend_server_script += "];\n";
  message_body.insert(insert_index + insert_tag.length(), frontend_server_script);

  // Get key value pairs
  for (int i = 1; i <= 3; i++)
  {
    std::vector<std::tuple<std::string, std::string>> kv_pairs = get_kvs_pairs(i);
    std::string kv_script = "\nconst kv_pairs" + std::string("_") + std::to_string(i) + " = [\n";
    for (auto const &kv : kv_pairs)
    {
      kv_script += "{";
      kv_script += "rowkey: \"" + std::get<0>(kv) + "\",";
      kv_script += "columnkey: \"" + std::get<1>(kv) + "\",";
      kv_script += "},\n";
    }
    kv_script += "];\n";

    message_body.insert(insert_index + insert_tag.length(), kv_script);
  }

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";

  // Return response
  return std::make_tuple(init_response, headers, message_body);
}
// FRONTEND POST ROUTES

// Returns map of post body {key, value}
std::unordered_map<std::string, std::string> parse_post_body_url_encoded(std::string body)
{
  std::unordered_map<std::string, std::string> post_body_map;
  std::istringstream ss(body);
  std::string element;
  while (std::getline(ss, element, '&'))
  {
    post_body_map[element.substr(0, element.find("="))] = element.substr(element.find("=") + 1);
  }

  // URL decode the values
  for (auto &element : post_body_map)
  {
    std::string value = element.second;
    std::string decoded_value = "";
    for (int i = 0; i < value.length(); i++)
    {
      if (value[i] == '+')
      {
        decoded_value += ' ';
      }
      else if (value[i] == '%' && value.length() > i + 2)
      {
        std::string hex = value.substr(i + 1, 2);
        i += 2;
        char decoded_char = (char)strtol(hex.c_str(), NULL, 16);
        decoded_value += decoded_char;
      }
      else
      {
        decoded_value += value[i];
      }
    }
    element.second = decoded_value;
  }
  return post_body_map;
}

std::tuple<std::string, std::string, std::string> post_login(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for username and password
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);

  std::string username = body_map["username"];
  std::string password = body_map["password"];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("user_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // Check if username and password are correct
  std::string command = get_kvs(backend_address, backend_port, "user_" + username, "password.txt");

  if (password != command)
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "Incorrect username or password. Please try again.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  else
  {
    std::string message_body = "";
    std::string init_response = req_init_line->version + " 303 Found\r\n";
    std::string headers = "";
    headers += "Location: /home\r\n";
    headers += "Content-Length: 0\r\n"; // Need this header on all post responses

    // Add auth token as a cookie, change token value to equal sid
    std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
    std::string sid = cookies["sid"];
    headers += "Set-Cookie: auth_token=" + sid + "; Path=/\r\n";
    usernames[sid] = username;
    return std::make_tuple(init_response, headers, message_body);
  }
}

std::tuple<std::string, std::string, std::string> post_signup(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for username and password
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);

  std::string username = body_map["username"];
  std::string password = body_map["password"];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("user_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  fprintf(stderr, "backendaddressport: %s, before stoi\n", backend_address_port.c_str());
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
  fprintf(stderr, "After\n");

  // create user file
  std::string command = put_kvs(backend_address, backend_port, "user_" + username, "password.txt", password, false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  backend_address_port = get_backend_address("email_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // after creating user file, also create metadata files for file and email
  // create email metadata file
  command = put_kvs(backend_address, backend_port, "email_" + username, "metadata.txt", "Metadata\n", false, ""); // Need to send non empty value

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
  // create file metadata file, and set home directory / to uuid 1
  command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", "/:1\n", false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // create next id column
  command = put_kvs(backend_address, backend_port, "file_" + username, "nextid.txt", "2", false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // create home directory
  command = put_kvs(backend_address, backend_port, "file_" + username, "1.txt", "is_directory:true\nparent:0\nchildren_files:\nchildren_folders:\n", false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /home\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  // Add auth token as a cookie, change token value to equal sid
  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  std::string sid = cookies["sid"];
  headers += "Set-Cookie: auth_token=" + sid + "; Path=/\r\n";
  usernames[sid] = username;

  return std::make_tuple(init_response, headers, message_body);
}

// Send a message
std::tuple<std::string, std::string, std::string> post_send_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  std::string recipient = body_map["recipient"];
  std::string subject = body_map["subject"];
  std::string message = body_map["message"];
  std::cerr << "Recipient(s): " << recipient << std::endl;
  std::cerr << "Subject: " << subject << std::endl;
  std::cerr << "Message: " << message << std::endl;

  // Parse multiple recipients
  std::vector<std::string> recipients;
  std::istringstream ss(recipient);
  std::string element;
  while (std::getline(ss, element, ';'))
  {
    recipients.push_back(element);
  }
  for (std::string recipient : recipients)
  {
    // Convert message to email format
    // DATE: current date and time
    // FROM: usename@domain
    // TO: recipient@domain
    // SUBJECT: subject
    // MESSAGE: message
    std::string username = usernames[parse_cookies(req_headers["Cookie"])["sid"]];
    std::time_t email_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string email_time_string = std::ctime(&email_time);
    std::string email_str = "DATE: " + email_time_string;
    email_str += "FROM: " + username + "@penncloud\n";
    email_str += "TO: " + recipient + "\n";
    email_str += "SUBJECT: " + subject + "\n";
    email_str += "MESSAGE: " + message + "\n";

    // Create a message_id that is a hash of the email string
    unsigned char digest_buf[MD5_DIGEST_LENGTH];
    computeDigest((char *)email_str.c_str(), strlen(email_str.c_str()), digest_buf);
    std::string uidl;
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
    {
      uidl += "0123456789ABCDEF"[digest_buf[i] / 16];
      uidl += "0123456789ABCDEF"[digest_buf[i] % 16];
    }

    // Make backend call to insert message into database only if the recipeint is a penncloud user
    if (recipient.find("@penncloud") != std::string::npos)
    {
      // Ping backend master for backend server address
      std::string recipient_remove_domain = recipient.substr(0, recipient.find("@"));
      std::string backend_address_port = get_backend_address("email_" + recipient_remove_domain);
      if (backend_address_port.substr(0, 5) == "--ERR")
      {
        // server group is down! return 503 error
        std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
        std::string message_body = "Servers are down. Please try again later.";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
      std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
      int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
      // Check if user exists
      std::string user_exists = get_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt");
      if (user_exists.length() <= 5 || user_exists.length() > 5 && user_exists.substr(0, 5) != "--ERR")
      {
        std::string put_email_response = put_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, uidl + ".txt", email_str, false, "");
        if (put_email_response.substr(0, 5) == "--ERR")
        {
          // error occurred when putting-- return ERR
          std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
          std::string message_body = "Servers are down. Please try again later.";
          std::string headers = "";
          headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
          return std::make_tuple(init_response, headers, message_body);
        }
        // std::cerr << "Put email response: " << put_email_response << std::endl;
        // Run GET and CPUT until successful
        std::string put_metadata_response;
        while (put_metadata_response != "+OK Value added")
        {
          backend_address_port = get_backend_address("email_" + recipient_remove_domain);
          if (backend_address_port.substr(0, 5) == "--ERR")
          {
            // server group is down! return 503 error
            std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
            std::string message_body = "Servers are down. Please try again later.";
            std::string headers = "";
            headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
            return std::make_tuple(init_response, headers, message_body);
          }
          backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
          backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));
          // std::cerr << "Recipient: " << recipient_remove_domain << std::endl;
          std::string metadata = get_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt");
          // std::cerr << "Get metadata response: " << metadata << std::endl;
          if (metadata.length() <= 5 || metadata.length() > 5 && metadata.substr(0, 5) != "--ERR")
          {
            put_metadata_response = put_kvs(backend_address, backend_port, "email_" + recipient_remove_domain, "metadata.txt", metadata + uidl + "\n", true, metadata);
            // std::cerr << "Put metadata response: " << put_metadata_response << std::endl;
          }
        }
      }
      else
      {
        std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
        std::string message_body = "The recipent(s) do not have an account in penncloud.";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
    }
    else
    {
      // User is outside of @penncloud

      // Get ip address of external email server
      std::string domain = recipient.substr(recipient.find("@") + 1);
      unsigned char nsbuf[4096];
      int l = res_query(domain.c_str(), C_IN, T_MX, (u_char *)&nsbuf, sizeof(nsbuf));
      if (l < 0)
      {
        std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
        std::string message_body = "Error: Could not find MX record for domain " + domain;
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }

      ns_msg handle;
      ns_rr rr;
      if (ns_initparse(nsbuf, l, &handle) < 0)
      {
        std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
        std::string message_body = "Error: Could not parse DNS response";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
      std::string ip_addr;
      for (int i = 0; i < l; i++)
      {
        if (ip_addr != "")
        {
          break;
        }
        ns_parserr(&handle, ns_s_an, 1, &rr);
        char dispbuf[4096];
        ns_sprintrr(&handle, &rr, NULL, NULL, dispbuf, sizeof(dispbuf));
        // std::cerr << "DNS Response: " << dispbuf << std::endl;
        const unsigned char *mx_data = ns_rr_rdata(rr);
        mx_data += 2;
        unsigned char mx_domain[NS_MAXDNAME];
        ns_name_unpack(ns_msg_base(handle), ns_msg_end(handle), mx_data, mx_domain, NS_MAXDNAME);
        // std::cerr << "mx_domain: " << mx_domain << std::endl;
        char mx_domain_ascii[NS_MAXDNAME];
        ns_name_ntop(mx_domain, mx_domain_ascii, NS_MAXDNAME);
        struct hostent *mx_host = gethostbyname(mx_domain_ascii);
        ip_addr = inet_ntoa(*(struct in_addr *)mx_host->h_addr_list[0]);
      }

      // std::cerr << "ip_addr: " << ip_addr << std::endl;

      // Send info to smtp client
      std::string smtp_client_msg = ip_addr + ":25\n";   // Remote server address
      smtp_client_msg += username + "@seas.upenn.edu\n"; // From (change to seas.upenn.edu to avoid spam filter)
      smtp_client_msg += recipient + "\n";               // To

      smtp_client_msg += "From: <" + username + "@seas.upenn.edu>\n";
      smtp_client_msg += "To: <" + recipient + ">\n";
      smtp_client_msg += "Date: " + email_time_string;
      smtp_client_msg += "Subject: " + subject + "\n";
      smtp_client_msg += "Message-ID: <" + uidl + "@seas.upenn.edu>\n";
      smtp_client_msg += message + "\r\n"; // Data

      // Send message to smtp client
      int smtp_client_socket = socket(AF_INET, SOCK_STREAM, 0);
      struct sockaddr_in servaddr;
      bzero(&servaddr, sizeof(servaddr));
      servaddr.sin_family = AF_INET;
      std::string localhost_addr = "127.0.0.1";
      inet_pton(AF_INET, localhost_addr.c_str(), &servaddr.sin_addr);
      servaddr.sin_port = htons(2501);

      connect(smtp_client_socket, (struct sockaddr *)&servaddr, sizeof(servaddr));
      send(smtp_client_socket, smtp_client_msg.c_str(), smtp_client_msg.length(), 0);

      // Receive response from smtp client
      char smtp_client_response[1024];
      bzero(smtp_client_response, 1024);
      int bytes_read = read(smtp_client_socket, smtp_client_response, 1024);
      smtp_client_response[bytes_read] = '\0';
      close(smtp_client_socket);
      std::string smtp_client_response_str = smtp_client_response;
      std::cerr << "SMTP Client Response: " << smtp_client_response_str << std::endl;
      if (smtp_client_response_str.find("+OK") == std::string::npos)
      {
        std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
        std::string message_body = "Error: Could not send email to external server";
        std::string headers = "";
        headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
        return std::make_tuple(init_response, headers, message_body);
      }
    }
  }
  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = "New message sent successfully.";
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// Delete a message
std::tuple<std::string, std::string, std::string> post_delete_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for message_id
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  std::string message_id = body_map["message_id"];
  std::cerr << "Message ID: " << message_id << std::endl;

  // Get username
  std::string sid = parse_cookies(req_headers["Cookie"])["sid"];
  std::string username = usernames[sid];
  std::cerr << "Username: " << username << std::endl;

  // Delete message from metadata
  // Run GET and CPUT until successful
  std::string put_metadata_response;
  while (put_metadata_response != "+OK Value added")
  {
    // Ping backend master for backend server address
    std::string backend_address_port = get_backend_address("email_" + username);
    if (backend_address_port.substr(0, 5) == "--ERR")
    {
      // server group is down! return 503 error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "Servers are down. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }
    std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
    int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

    std::string metadata = get_kvs(backend_address, backend_port, "email_" + username, "metadata.txt");
    std::cerr << "Get metadata response: " << metadata << std::endl;
    if (metadata.length() <= 5 || metadata.length() > 5 && metadata.substr(0, 5) != "--ERR")
    {
      std::string new_metadata = metadata.substr(0, metadata.find(message_id) - 1) + metadata.substr(metadata.find(message_id) + message_id.length());
      put_metadata_response = put_kvs(backend_address, backend_port, "email_" + username, "metadata.txt", new_metadata, true, metadata);
      std::cerr << "Put metadata response: " << put_metadata_response << std::endl;
    }
  }

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("email_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // Call delete on email message
  std::string delete_email_response = delete_kvs(backend_address, backend_port, "email_" + username, message_id + ".txt");
  std::cerr << "Delete email response: " << delete_email_response << std::endl;

  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = "Message deleted successfully.";
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// Reply to a message (uses post send)
std::tuple<std::string, std::string, std::string> post_reply_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  return std::make_tuple("", "", "");
}

// Forward a message (uses post send)
std::tuple<std::string, std::string, std::string> post_forward_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  return std::make_tuple("", "", "");
}

std::tuple<std::string, std::string, std::string> post_change_password(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for username and password
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);

  std::string username = body_map["username"];
  std::string password = body_map["password"];

  // Check that logged in user is the same user that is trying to change passsword
  std::string sid = parse_cookies(req_headers["Cookie"])["sid"];
  if (usernames[sid] != username)
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in as the user you are trying to change the password for.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Update with new password
  std::string put_metadata_response;
  while (put_metadata_response != "+OK Value added")
  {
    // Ping backend master for backend server address
    std::string backend_address_port = get_backend_address("user_" + username);
    if (backend_address_port.substr(0, 5) == "--ERR")
    {
      // server group is down! return 503 error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "Servers are down. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }
    std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
    int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

    std::string old_password = get_kvs(backend_address, backend_port, "user_" + username, "password.txt");
    if (old_password.length() <= 5 || old_password.length() > 5 && old_password.substr(0, 5) != "--ERR")
    {
      put_metadata_response = put_kvs(backend_address, backend_port, "user_" + username, "password.txt", password, true, old_password);
    }
  }

  // Redirect to login page
  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Found\r\n";
  std::string headers = "";
  headers += "Location: /\r\n";
  headers += "Content-Length: 0\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> post_kill_server(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for server address
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  std::string server_address = body_map["server_id"];
  std::cerr << "Server Address: " << server_address << std::endl;

  // Connect to master server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(MASTER_SERVER_PORT);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Cannot connect to master backend";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    close(sock);
    return std::make_tuple(init_response, headers, message_body);
  }

  // Read for +OK Sever ready
  char ready_response[MAX_BUFF_SIZE];
  bzero(ready_response, MAX_BUFF_SIZE);
  int bytes_read = read(sock, ready_response, MAX_BUFF_SIZE);
  ready_response[bytes_read] = '\0';
  std::cerr << "Ready Response: " << ready_response << std::endl;
  if (std::string(ready_response).find("+OK") == std::string::npos)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Master server not ready";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    close(sock);
    return std::make_tuple(init_response, headers, message_body);
  }

  // Send kill to master server
  std::string kill_msg = "KILL," + server_address + "\r\n";
  send(sock, kill_msg.c_str(), kill_msg.length(), 0);

  // Receive response from master server
  char response[MAX_BUFF_SIZE];
  bzero(response, MAX_BUFF_SIZE);
  bytes_read = read(sock, response, MAX_BUFF_SIZE);
  response[bytes_read] = '\0';
  std::string response_str = response;
  std::cerr << "Response: " << response_str << std::endl;
  close(sock);
  if (response_str.find("+OK") == std::string::npos)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Error: Could not kill server";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = response_str;
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> post_restart_server(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Parse body for server address
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  std::string server_address = body_map["server_id"];
  std::cerr << "Server Address: " << server_address << std::endl;

  // Connect to master server
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  struct sockaddr_in servaddr;
  bzero(&servaddr, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  inet_pton(AF_INET, MASTER_SERVER_ADDR.c_str(), &servaddr.sin_addr);
  servaddr.sin_port = htons(MASTER_SERVER_PORT);

  // Connect to server
  if (connect(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Cannot connect to master backend";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    close(sock);
    return std::make_tuple(init_response, headers, message_body);
  }

  // Read for +OK Sever ready
  char ready_response[MAX_BUFF_SIZE];
  bzero(ready_response, MAX_BUFF_SIZE);
  int bytes_read = read(sock, ready_response, MAX_BUFF_SIZE);
  ready_response[bytes_read] = '\0';
  std::cerr << "Ready Response: " << ready_response << std::endl;
  if (std::string(ready_response).find("+OK") == std::string::npos)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Master server not ready";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    close(sock);
    return std::make_tuple(init_response, headers, message_body);
  }

  // Send kill to master server
  std::string kill_msg = "RVIV," + server_address + "\r\n";
  send(sock, kill_msg.c_str(), kill_msg.length(), 0);

  // Receive response from master server
  char response[MAX_BUFF_SIZE];
  bzero(response, MAX_BUFF_SIZE);
  bytes_read = read(sock, response, MAX_BUFF_SIZE);
  response[bytes_read] = '\0';
  std::string response_str = response;
  std::cerr << "Response: " << response_str << std::endl;
  close(sock);
  if (response_str.find("+OK") == std::string::npos)
  {
    // Send error back to frontend
    std::string init_response = req_init_line->version + " 500 Internal Server Error\r\n";
    std::string message_body = "Error: Could not restart server";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    close(sock);
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = response_str;
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  close(sock);
  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> get_kvs_data(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  std::string row_key = body_map["rowkey"];
  std::string column_key = body_map["columnkey"];
  // query master for backend server
  std::string backend_address_port = get_backend_address(row_key);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  std::string response = get_kvs(backend_address, backend_port, row_key, column_key);
  if (response.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string headers = "Content-Type: text/html\r\n";
  headers += "Content-Length: " + std::to_string(response.length()) + "\r\n";
  return std::make_tuple(init_response, headers, response);
}

// File system functions
// GET

// function to return and display all children of a folder specified in request path
std::tuple<std::string, std::string, std::string> get_storage(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  fprintf(stderr, "logged in: %s\n", username.c_str());

  // Get path of folder
  std::string folder_path;
  if (req_init_line->path.length() == 8)
  {
    // path is just /storage
    folder_path = "";
  }
  else
  {
    folder_path = req_init_line->path.substr(8);
    if (folder_path == "/")
    {
      folder_path = "";
    }
  }

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // query backend for the uuid of this folder
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse metadata file for the uuid of the folder
  std::string searchable = folder_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // use uuid to get folder data
  std::string folder_data = get_kvs(backend_address, backend_port, "file_" + username, uuid);
  if (folder_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // start getting children
  std::vector<File *> files;
  // parse children_files data for children files
  int curr_ind = folder_data.find("children_files:\n");
  folder_data = folder_data.substr(curr_ind + 16);
  int test_ind = folder_data.find("children_folders:\n");
  curr_ind = folder_data.find("\n");
  while (curr_ind < test_ind)
  {
    std::string file_value = folder_data.substr(0, curr_ind);
    col_ind = file_value.find(":");
    std::string file_name = file_value.substr(0, col_ind);
    File *temp = new File();
    temp->is_directory = false;
    temp->file_id = folder_path + "/" + file_name;
    temp->name = file_name;
    files.push_back(temp);
    folder_data = folder_data.substr(curr_ind + 1);
    curr_ind = folder_data.find("\n");
    test_ind = folder_data.find("children_folders:");
  }
  // parse children_folders for children folders
  // at this point, curr_ind should point to the \n at the end of children_files:
  folder_data = folder_data.substr(curr_ind + 1);
  curr_ind = folder_data.find("\n");
  while (curr_ind != std::string::npos)
  {
    std::string folder_value = folder_data.substr(0, curr_ind);
    col_ind = folder_value.find(":");
    std::string folder_name = folder_value.substr(0, col_ind);
    File *temp = new File();
    temp->is_directory = true;
    temp->file_id = folder_path + "/" + folder_name;
    temp->name = folder_name;
    files.push_back(temp);
    folder_data = folder_data.substr(curr_ind + 1);
    curr_ind = folder_data.find("\n");
  }

  // Create response and send HTML
  std::ifstream storage_file(STATICS_LOC + "storage.html");
  std::string message_body;

  if (storage_file.is_open())
  {
    std::string line;
    while (getline(storage_file, line))
    {
      message_body += line + "\n";
    }
    storage_file.close();
  }
  else
  {
    std::string response = req_init_line->version + " 404 Not Found\r\n";
    return std::make_tuple(response, "", "");
  }

  // Insert files into HTML after <script> tag
  std::string insert_tag = "<script>";
  int insert_index = message_body.find(insert_tag);
  std::string file_script = "\nconst files = [\n";
  for (auto const &file : files)
  {
    file_script += "{";
    file_script += "file_id: \"" + file->file_id + "\",";
    file_script += "name: \"" + file->name + "\",";
    if (file->is_directory)
    {
      fprintf(stderr, "is directory!\n");
      file_script += "is_directory: true";
    }
    else
    {
      fprintf(stderr, "is not directory!\n");
      file_script += "is_directory: false";
    }
    file_script += "},\n";
  }
  file_script += "];\n";
  message_body.insert(insert_index + insert_tag.length(), file_script);
  // Insert path into HTML after <!-- comment
  insert_tag = "<!--add folder name here-->";
  insert_index = message_body.find(insert_tag);
  std::string insert_string;
  if (folder_path == "")
  {
    insert_string = "<h2>/</h2>";
  }
  else
  {
    insert_string = "<h2>" + folder_path + "</h2>";
  }
  message_body.insert(insert_index + insert_tag.length(), insert_string);

  if (folder_path != "")
  {
    insert_tag = "<!--delete and rename buttons here-->";
    insert_index = message_body.find(insert_tag);
    insert_string = "<div class=\"option-container\">\n<div class=\"container\">\n<h3>Delete</h3>\n<form action=\"/delete\" method=\"post\" id=\"delete-form\">\n<input type=\"submit\" value=\"Delete This Folder\">\n</form>\n</div>\n<br>\n<div class=\"container\"><h3>Rename</h3>\n<form action=\"/rename\" method=\"post\" id=\"rename-form\">\n<label for=\"name\">New Name: </label>\n<input type=\"text\" id=\"name\" name=\"name\" required>\n<br><br><input type=\"submit\" value=\"Rename This Folder\"></form>\n</div>\n<div class=\"container\">\n<h3>Move</h3><form action=\"/move\" id=\"move-form\" method=\"post\">\n<label for=\"dest\">Move to Folder: </label>\n<input type=\"text\" id=\"dest\" name=\"dest\" required>\n<br><br>\n<input type=\"submit\" value=\"Move\">\n</form>\n<br></div>\n</div>\n";
    message_body.insert(insert_index + insert_tag.length(), insert_string);
  }

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// function to display file options for file specified in path
std::tuple<std::string, std::string, std::string> get_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Get path of folder
  std::string file_path = req_init_line->path.substr(5);
  int slash_ind = file_path.find("/");
  std::string file_name = file_path;
  while (slash_ind != std::string::npos)
  {
    file_name = file_name.substr(slash_ind + 1);
    slash_ind = file_name.find("/");
  }

  // Create response and send HTML
  std::ifstream file(STATICS_LOC + "file.html");
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

  // Insert message into HTML
  std::string insert_tag = "<!-- Insert file name here using c++ code-->";
  int insert_index = message_body.find(insert_tag);
  std::string name_html = "\n<h1>";
  name_html += file_name;
  name_html += "</h1>";
  message_body.insert(insert_index + insert_tag.length(), name_html);

  // Create inital response line
  std::string init_response = req_init_line->version + " 200 OK\r\n";

  // Create headers
  std::string headers;

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// function to upload file
std::tuple<std::string, std::string, std::string> post_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse request for file data
  int file_index = body.find("Content-Disposition: form-data; name=\"filename\"");
  std::string temp = body.substr(file_index);
  // get the file name
  int name_index = temp.find("filename=");
  int end_name_index = temp.find("\r\n");
  std::string file_name = temp.substr(name_index + 10, end_name_index - name_index - 9);
  // remove end quotation mark on file name
  int quote_index = file_name.find("\"");
  file_name = file_name.substr(0, quote_index);

  // get important file info
  int content_index = temp.find("Content-Type:");
  temp = temp.substr(content_index);
  // parse out extraneous info
  int extra_index = temp.find("-----------------------");
  std::string file_info = temp.substr(0, extra_index);

  // get the path name from the url
  std::string file_path = req_init_line->path.substr(7);

  std::string new_file_name = file_path + "/" + file_name;

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse metadata file for the uuid of the folder
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // use uuid to get folder data
  std::string folder_data = get_kvs(backend_address, backend_port, "file_" + username, uuid);
  if (folder_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // update folder_data string to include new file in children files
  int curr_ind = folder_data.find("children_folders:\n");
  std::string prefolder = folder_data.substr(0, curr_ind);
  std::string postfolder = folder_data.substr(curr_ind);
  std::string new_folder_data = prefolder + file_name + "\n" + postfolder;
  std::string command = put_kvs(backend_address, backend_port, "file_" + username, uuid, new_folder_data, true, folder_data);

  // now, access next_uuid to get a uuid for this new file
  std::string next_uuid = get_kvs(backend_address, backend_port, "file_" + username, "nextid.txt");
  if (next_uuid.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  int new_uuid = std::stoi(next_uuid);
  next_uuid = std::to_string(new_uuid + 1);

  command = put_kvs(backend_address, backend_port, "file_" + username, "nextid.txt", next_uuid, false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // now, edit metadata file to contain mapping of file path to this uuid
  std::string new_metadata = metadata + new_file_name + ":" + std::to_string(new_uuid) + "\n";
  command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_metadata, true, metadata);

  // create the new file
  command = put_kvs(backend_address, backend_port, "file_" + username, std::to_string(new_uuid) + ".txt", "is_directory:false\nparent:" + uuid + "\n", false, "");
  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // write to new row data of the file
  backend_address_port = get_backend_address(username + "_" + std::to_string(new_uuid) + ".txt");
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // write data in new row
  command = put_kvs(backend_address, backend_port, username + "_" + std::to_string(new_uuid) + ".txt", "data.txt", file_info, false, "");
  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /storage" + file_path + "\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  return std::make_tuple(init_response, headers, message_body);
}

// function to retrieve and download file data
std::tuple<std::string, std::string, std::string> download_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];
  // get the path name from the url
  std::string file_path = req_init_line->path.substr(9);

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse metadata file for the uuid of the file
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // open file data
  backend_address_port = get_backend_address(username + "_" + uuid);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  std::string file_data = get_kvs(backend_address, backend_port, username + "_" + uuid, "data.txt");
  if (file_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  int last_index = file_path.find_last_of("/");
  std::string file_name = file_path.substr(last_index + 1);

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string headers = "";

  headers += "Content-Disposition: attachment; filename=\"" + file_name + "\"\r\n";
  int split_ind = file_data.find("\r\n");
  std::string next_header = file_data.substr(0, split_ind);
  std::string data = file_data.substr(split_ind + 4);
  // get rid of any extra \r\n's
  data.pop_back();
  data.pop_back();

  headers += "Content-Length: " + std::to_string(data.length()) + "\r\n";
  headers += next_header + "\r\n";
  // headers += "Content-Type: application/octet-stream\r\n";
  headers += "Date: Mon, 29 Apr 2024 22:14:46 GMT\r\n";
  message_body = data;

  return std::make_tuple(init_response, headers, message_body);
}


// function to create new folder
std::tuple<std::string, std::string, std::string> post_folder(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse request for folder data
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  // get the folder name
  std::string folder_name = body_map["name"];

  fprintf(stderr, "%s\n", folder_name.c_str());

  // get the path name from the url
  std::string file_path = req_init_line->path.substr(11);
  fprintf(stderr, "%s\n", file_path.c_str());

  std::string new_folder_name = file_path + "/" + folder_name;

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // parse metadata file for the uuid of the folder
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // use uuid to get folder data
  std::string folder_data = get_kvs(backend_address, backend_port, "file_" + username, uuid);
  if (folder_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // update folder_data string to include new file in children folders
  std::string new_folder_data = folder_data + folder_name + "\n";
  std::string command = put_kvs(backend_address, backend_port, "file_" + username, uuid, new_folder_data, true, folder_data);

  // now, access next_uuid to get a uuid for this new folder
  std::string next_uuid = get_kvs(backend_address, backend_port, "file_" + username, "nextid.txt");
  if (next_uuid.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  int new_uuid = std::stoi(next_uuid);
  next_uuid = std::to_string(new_uuid + 1);

  command = put_kvs(backend_address, backend_port, "file_" + username, "nextid.txt", next_uuid, false, "");

  if (command.substr(0, 5) == "--ERR")
  {
    // error occurred when putting-- return ERR
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // now, edit metadata file to contain mapping of file path to this uuid
  std::string new_metadata = metadata + new_folder_name + ":" + std::to_string(new_uuid) + "\n";
  command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_metadata, true, metadata);

  // finally, create the new file
  command = put_kvs(backend_address, backend_port, "file_" + username, std::to_string(new_uuid) + ".txt", "is_directory:true\nparent:" + uuid + "\nchildren_files:\nchildren_folders:\n", false, "");

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /storage" + file_path + "\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  return std::make_tuple(init_response, headers, message_body);
}


// function to delete file or folder
std::tuple<std::string, std::string, std::string> delete_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // get the path name from the url
  std::string file_path = req_init_line->path.substr(7);
  fprintf(stderr, "file_path: %s\n", file_path.c_str());

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // find the data entry for this value specifically in metadata
  int next_ind = metadata.find("\n" + file_path + ":");
  std::string pre_meta = metadata.substr(0, next_ind + 1);
  std::string post_meta = metadata.substr(next_ind + 1);
  int del_ind = post_meta.find("\n");
  std::string new_meta = pre_meta + post_meta.substr(del_ind + 1);

  // write new metadata file
  std::string command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
  metadata = new_meta;
  fprintf(stderr, "post metadata\n");

  // find the uuid of the file
  int colon_ind = post_meta.find(":");
  std::string uuid = post_meta.substr(colon_ind + 1);
  int end_ind = uuid.find("\n");
  uuid = uuid.substr(0, end_ind);

  // open the data for this file
  std::string file_data = get_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt");
  if (file_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // find parent uuid
  int parent_ind = file_data.find("parent:");
  std::string parent_uuid = file_data.substr(parent_ind + 7);
  end_ind = parent_uuid.find("\n");
  parent_uuid = parent_uuid.substr(0, end_ind);

  // open the data for the parent uuid
  std::string parent_data = get_kvs(backend_address, backend_port, "file_" + username, parent_uuid);
  if (parent_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // find name of thing to be deleted
  int dash_ind = file_path.find_last_of('/');
  std::string file_name = file_path.substr(dash_ind + 1);

  int to_delete = parent_data.find("\n" + file_name + "\n");
  std::string new_parent_data = parent_data.substr(0, to_delete + 1) + parent_data.substr(to_delete + file_name.length() + 2);

  // write new parent file
  command = put_kvs(backend_address, backend_port, "file_" + username, parent_uuid, new_parent_data, true, parent_data);

  // delete uuid file
  command = delete_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt");

  // delete data of file
  std::string data_backend_address_port = get_backend_address(username + "_" + uuid + ".txt");
  if (data_backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string data_backend_address = data_backend_address_port.substr(0, data_backend_address_port.find(":"));
  int data_backend_port = std::stoi(data_backend_address_port.substr(data_backend_address_port.find(":") + 1));

  command = delete_kvs(data_backend_address, data_backend_port, username + "_" + uuid + ".txt", "data.txt");

  // while something exists in the metadata that is a child of this path, delete it
  next_ind = metadata.find("\n" + file_path + "/");
  while (next_ind != std::string::npos)
  {
    pre_meta = metadata.substr(0, next_ind + 1);
    post_meta = metadata.substr(next_ind + 1);
    del_ind = post_meta.find("\n");
    new_meta = pre_meta + post_meta.substr(del_ind + 1);

    // write new metadata file
    command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
    metadata = new_meta;

    // find the uuid of the file
    colon_ind = post_meta.find(":");
    uuid = post_meta.substr(colon_ind + 1);
    end_ind = uuid.find("\n");
    uuid = uuid.substr(0, end_ind);

    // delete this uuid
    command = delete_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt");
    next_ind = metadata.find("\n" + file_path + "/");

    data_backend_address_port = get_backend_address(username + "_" + uuid + ".txt");
    if (data_backend_address_port.substr(0, 5) == "--ERR")
    {
      // server group is down! return 503 error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "Servers are down. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }
    data_backend_address = data_backend_address_port.substr(0, data_backend_address_port.find(":"));
    data_backend_port = std::stoi(data_backend_address_port.substr(data_backend_address_port.find(":") + 1));

    command = delete_kvs(data_backend_address, data_backend_port, username + "_" + uuid + ".txt", "data.txt");
  }

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /storage\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  return std::make_tuple(init_response, headers, message_body);
}

// function to rename file or folder
std::tuple<std::string, std::string, std::string> rename_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  // get the new name from the body
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  // get the folder name
  std::string new_name = body_map["name"];

  // get the path name from the url
  std::string file_path = req_init_line->path.substr(7);
  fprintf(stderr, "file_path: %s\n", file_path.c_str());

  // find name of thing to be renamed
  int dash_ind = file_path.find_last_of('/');
  std::string file_name = file_path.substr(dash_ind + 1);
  int dot_ind = file_name.find(".");
  std::string extension = "";
  if (dot_ind != std::string::npos)
  {
    extension = file_name.substr(dot_ind);
  }
  std::string new_path = file_path.substr(0, dash_ind + 1) + new_name + extension;

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // find the data entry for this value specifically in metadata
  int next_ind = metadata.find("\n" + file_path + ":");
  std::string pre_meta = metadata.substr(0, next_ind + 1);
  std::string post_meta = metadata.substr(next_ind + 1);
  int del_ind = post_meta.find(":");
  std::string new_meta = pre_meta + new_path + post_meta.substr(del_ind);

  // write new metadata file
  std::string command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
  metadata = new_meta;
  fprintf(stderr, "post metadata\n");

  // find the uuid of the file
  int colon_ind = post_meta.find(":");
  std::string uuid = post_meta.substr(colon_ind + 1);
  int end_ind = uuid.find("\n");
  uuid = uuid.substr(0, end_ind);

  // open the data for this file
  std::string file_data = get_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt");
  if (file_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  // find parent uuid
  int parent_ind = file_data.find("parent:");
  std::string parent_uuid = file_data.substr(parent_ind + 7);
  end_ind = parent_uuid.find("\n");
  parent_uuid = parent_uuid.substr(0, end_ind);

  // open the data for the parent uuid
  std::string parent_data = get_kvs(backend_address, backend_port, "file_" + username, parent_uuid);
  if (parent_data.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  int to_delete = parent_data.find("\n" + file_name + "\n");
  if (to_delete != std::string::npos)
  {
    std::string new_parent_data = parent_data.substr(0, to_delete + 1) + new_name + extension + "\n" + parent_data.substr(to_delete + file_name.length() + 2);

    // write new parent file
    command = put_kvs(backend_address, backend_port, "file_" + username, parent_uuid, new_parent_data, true, parent_data);
  }

  // while something exists in the metadata that is a child of this path, rename it
  next_ind = metadata.find("\n" + file_path + "/");
  while (next_ind != std::string::npos)
  {
    new_meta = metadata;
    new_meta.replace(next_ind, file_path.length() + 1, "\n" + new_path);

    // write new metadata file
    command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
    metadata = new_meta;

    next_ind = metadata.find("\n" + file_path + "/");
  }

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /storage\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  return std::make_tuple(init_response, headers, message_body);
}

// function to move file or folder to new path specified in body
std::tuple<std::string, std::string, std::string> move_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // Check if user is logged in (auth_token=sid)
  if (req_headers.find("Cookie") == req_headers.end())
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  std::unordered_map<std::string, std::string> cookies = parse_cookies(req_headers["Cookie"]);
  if (cookies.find("auth_token") == cookies.end() || cookies["auth_token"] != cookies["sid"])
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "You must be logged in to view this page.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  // get the new route from the body
  std::unordered_map<std::string, std::string> body_map = parse_post_body_url_encoded(body);
  // get the folder name
  std::string new_parent = body_map["dest"];
  int repl_val = new_parent.find("%2F");
  while (repl_val != std::string::npos)
  {
    new_parent.replace(repl_val, 3, "/");
    repl_val = new_parent.find("%2F");
  }

  // get the path name from the url
  std::string file_path = req_init_line->path.substr(5);

  // find name of thing to be moved
  int dash_ind = file_path.find_last_of('/');
  std::string file_name = file_path.substr(dash_ind + 1);
  int dot_ind = file_name.find(".");
  std::string extension = "";
  if (dot_ind != std::string::npos)
  {
    extension = file_name.substr(dot_ind);
  }
  std::string new_path;
  if (new_parent == "/")
  {
    new_path = new_parent + file_name;
  }
  else
  {
    new_path = new_parent + "/" + file_name;
  }

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie
  std::string username = usernames[sid];

  // Ping backend master for backend server address
  std::string backend_address_port = get_backend_address("file_" + username);
  if (backend_address_port.substr(0, 5) == "--ERR")
  {
    // server group is down! return 503 error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "Servers are down. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }
  std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // open metadata file
  std::string metadata = get_kvs(backend_address, backend_port, "file_" + username, "metadata.txt");
  if (metadata.substr(0, 5) == "--ERR")
  {
    // failed to connect to backend! return an error
    std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
    std::string message_body = "An error occurred. Please try again later.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  }

  // find the uuid for the new parent
  int par_int;
  if (new_parent == "/")
  {
    par_int = metadata.find(new_parent + ":");
  }
  else
  {
    par_int = metadata.find("\n" + new_parent + ":");
  }
  if (par_int == std::string::npos)
  {
    // specified file path doesn't exist!
    std::string message_body = "";
    std::string init_response = req_init_line->version + " 308 Invalid Path\r\n";
    std::string headers = "";
    headers += "Location: /invalidpath\r\n";
    headers += "Content-Length: 0\r\n"; // Need this header on all post responses

    return std::make_tuple(init_response, headers, message_body);
  }
  else
  {
    // find the data entry for this value specifically in metadata
    int next_ind = metadata.find("\n" + file_path + ":");
    std::string pre_meta = metadata.substr(0, next_ind + 1);
    std::string post_meta = metadata.substr(next_ind + 1);
    int del_ind = post_meta.find(":");
    std::string new_meta = pre_meta + new_path + post_meta.substr(del_ind);

    // write new metadata file
    std::string command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
    metadata = new_meta;

    // find the uuid of this file
    int colon_ind = post_meta.find(":");
    std::string uuid = post_meta.substr(colon_ind + 1);
    int end_ind = uuid.find("\n");
    uuid = uuid.substr(0, end_ind);
    fprintf(stderr, "got uuid");

    post_meta = metadata.substr(par_int + 1);
    colon_ind = post_meta.find(":");
    std::string new_parent_uuid = post_meta.substr(colon_ind + 1);
    end_ind = new_parent_uuid.find("\n");
    new_parent_uuid = new_parent_uuid.substr(0, end_ind);
    fprintf(stderr, "got parent uuid %s\n", new_parent_uuid.c_str());

    // open the data for this file
    std::string file_data = get_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt");
    if (file_data.substr(0, 5) == "--ERR")
    {
      // failed to connect to backend! return an error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "An error occurred. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }

    // find whether this is a directory or a file
    int is_directory_ind = file_data.find("is_directory:");
    std::string file_is_directory = file_data.substr(is_directory_ind + 13);
    end_ind = file_is_directory.find("\n");
    file_is_directory = file_is_directory.substr(0, end_ind);
    fprintf(stderr, "got is_directory");

    // find parent uuid
    int parent_ind = file_data.find("parent:");
    std::string pre_parent = file_data.substr(0, parent_ind + 7);
    std::string post_parent = file_data.substr(parent_ind + 7);
    end_ind = post_parent.find("\n");
    std::string parent_uuid = post_parent.substr(0, end_ind);
    std::string rest_of_file = post_parent.substr(end_ind);
    std::string new_file_value = pre_parent + new_parent_uuid + ".txt" + rest_of_file;
    fprintf(stderr, "got old parent id");

    // write new_file_value
    command = put_kvs(backend_address, backend_port, "file_" + username, uuid + ".txt", new_file_value, false, "");
    if (command.substr(0, 5) == "--ERR")
    {
      // error occurred when putting-- return ERR
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "Servers are down. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }

    // open the data for the old parent uuid
    std::string parent_data = get_kvs(backend_address, backend_port, "file_" + username, parent_uuid);
    if (parent_data.substr(0, 5) == "--ERR")
    {
      // failed to connect to backend! return an error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "An error occurred. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }

    // delete this file from children of old parent
    int to_delete = parent_data.find("\n" + file_name + "\n");
    std::string new_parent_data = parent_data.substr(0, to_delete + 1) + parent_data.substr(to_delete + file_name.length() + 2);

    // write new parent data
    command = put_kvs(backend_address, backend_port, "file_" + username, parent_uuid, new_parent_data, true, parent_data);

    // open the data for the new parent uuid
    parent_data = get_kvs(backend_address, backend_port, "file_" + username, new_parent_uuid + ".txt");
    if (parent_data.substr(0, 5) == "--ERR")
    {
      // failed to connect to backend! return an error
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "An error occurred. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }

    std::string new_parent_value = "";

    // add this value to the children field of the parent
    if (file_is_directory == "true")
    {
      new_parent_value = parent_data + file_name + "\n";
    }
    else
    {
      int curr_ind = parent_data.find("children_folders:\n");
      std::string prefolder = parent_data.substr(0, curr_ind);
      std::string postfolder = parent_data.substr(curr_ind);
      new_parent_value = prefolder + file_name + "\n" + postfolder;
      fprintf(stderr, "new value: %s\n", new_parent_value.c_str());
    }
    // write new value to new parent
    command = put_kvs(backend_address, backend_port, "file_" + username, new_parent_uuid + ".txt", new_parent_value, false, "");
    if (command.substr(0, 5) == "--ERR")
    {
      // error occurred when putting-- return ERR
      std::string init_response = req_init_line->version + " 503 Service Unavailable\r\n";
      std::string message_body = "Servers are down. Please try again later.";
      std::string headers = "";
      headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
      return std::make_tuple(init_response, headers, message_body);
    }

    // while something exists in the metadata that is a child of this path, rename it
    next_ind = metadata.find("\n" + file_path + "/");
    while (next_ind != std::string::npos)
    {
      new_meta = metadata;
      new_meta.replace(next_ind, file_path.length() + 1, "\n" + new_path);

      // write new metadata file
      command = put_kvs(backend_address, backend_port, "file_" + username, "metadata.txt", new_meta, true, metadata);
      metadata = new_meta;

      next_ind = metadata.find("\n" + file_path + "/");
    }

    std::string message_body = "";
    std::string init_response = req_init_line->version + " 303 Success\r\n";
    std::string headers = "";
    headers += "Location: /storage\r\n";
    headers += "Content-Length: 0\r\n"; // Need this header on all post responses

    return std::make_tuple(init_response, headers, message_body);
  }
}