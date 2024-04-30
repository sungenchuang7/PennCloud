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

std::string STATICS_LOC = "./statics/";
int MAX_BUFF_SIZE = 1000;
// TODO: Implement address caching (map of rowkey to backend address)
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
  std::cerr << "Sending INIT request" << std::endl;
  // Read response from server
  bzero(buffer, MAX_BUFF_SIZE);
  has_full_command = false;
  end_index = 0;

  while (!has_full_command)
  {
    char c;
    std::cerr << "Waiting to read" << std::endl;
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
  while (!has_full_command)
  {
    char c;
    if (read(sock, &c, 1) > 0)
    {
      buffer[end_index] = c;

      if (end_index >= 4 && c == '\n' && buffer[end_index - 1] == '\r' && buffer[end_index - 2] == '.' && buffer[end_index - 3] == '\n' && buffer[end_index - 4] == '\r')
      {
        has_full_command = true;
        buffer[end_index - 4] = '\0'; // Replace \r\n.\r\n with \0
        command = buffer;
      }

      end_index++;
    }
  }

  // Close connection
  close(sock);
  return command;
}

// Make a put or cput request to the backend server
// Returns error message if request fails
std::string put_kvs(std::string ip, int port, std::string row, std::string col, std::string value, bool is_cput, std::string prev_value)
{
  // TODO: If (C)PUT or DATA fails, frontend should retry
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
  } else
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
  // TODO: Add check for if logged out
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
  // TODO: Date header

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
  // TODO: Add check for if logged out
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
  // TODO: Date header

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
  // TODO: Date header

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
  std::cerr << "Username: " << username << std::endl;

  // Get email metadata
  std::string email_metadata = get_kvs("127.0.0.1", 7000, "email_" + username, "metadata.txt");
  std::cerr << "Email metadata: " << email_metadata << std::endl;
  std::unordered_map<std::string, Email> emails = {};

  // Add email data to emails map
  std::istringstream ss(email_metadata);
  std::string message_id;
  while (std::getline(ss, message_id, '\n'))
  {
    if (message_id != "Metadata")
    {
      std::cerr << "Message ID: " << message_id << std::endl;
      std::string email_data = get_kvs("127.0.0.1", 7000, "email_" + username, message_id + ".txt");
      std::cerr << "Email data: " << email_data << std::endl;
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
  std::sort(sorted_emails.begin(), sorted_emails.end(), [](const std::pair<std::string, Email> &a, const std::pair<std::string, Email> &b) {
    return a.second.arrival_time > b.second.arrival_time;
  });
  
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
  std::string message = get_kvs("127.0.0.1", 7000, "email_" + username, message_id + ".txt");
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

// FRONTEND POST ROUTES

// Returns map of post body {key, value}
std::unordered_map<std::string, std::string> parse_post_body_url_encoded(std::string body)
{
  std::unordered_map<std::string, std::string> post_body_map;
  std::istringstream ss(body);
  std::string element;
  while (std::getline(ss, element, '&')) // TODO: This might be an issue an email or file contains a & character
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
  // std::string backend_address_port = get_backend_address("user_" + username);
  // std::string backend_address = backend_address_port.substr(0, backend_address_port.find(":"));
  // int backend_port = std::stoi(backend_address_port.substr(backend_address_port.find(":") + 1));

  // Check if username and password are correct
  // std::string command = get_kvs(backend_address, backend_port, "user_" + username, "password.txt");
  std::string command = get_kvs("127.0.0.1", 7000, "user_" + username, "password.txt");

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

  // TODO: ping backend master for backend server address
  // std::string backend_address = get_backend_address("user_" + username);
  
  // create user file 
  std::string command = put_kvs("127.0.0.1", 7000, "user_" + username, "password.txt", password, false, "");

  // TODO: handle error 

  // after creating user file, also create metadata files for file and email
  // create email metadata file
  command = put_kvs("127.0.0.1", 7000, "email_" + username, "metadata.txt", "Metadata\n", false, ""); // Need to send non empty value
  // TODO: handle error 
  // create file metadata file, and set home directory / to uuid 1
  command = put_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt", "/:1\n", false, "");
  // create next id column
  command = put_kvs("127.0.0.1", 7000, "file_" + username, "nextid.txt", "2", false, "");
  // create home directory
  command = put_kvs("127.0.0.1", 7000, "file_" + username, "1.txt", "is_directory:true\nparent:0\nchildren_files:\nchildren_folders:\n", false, "");
  
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
  // TODO: update sending to users outside @penncloud
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
    std::string email_str = "DATE: " + email_time_string + "\n";
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
      std::string recipient_remove_domain = recipient.substr(0, recipient.find("@"));
      std::string put_email_response = put_kvs("127.0.0.1", 7000, "email_" + recipient_remove_domain, uidl + ".txt", email_str, false, "");
      // std::cerr << "Put email response: " << put_email_response << std::endl;
      // Run GET and CPUT until successful
      std::string put_metadata_response;
      while (put_metadata_response != "+OK Value added")
      {
        // std::cerr << "Recipient: " << recipient_remove_domain << std::endl;
        std::string metadata = get_kvs("127.0.0.1", 7000, "email_" + recipient_remove_domain, "metadata.txt");
        // std::cerr << "Get metadata response: " << metadata << std::endl;
        if (metadata.length()  <= 5 || metadata.length() > 5 && metadata.substr(0, 5) != "--ERR")
        {
          put_metadata_response = put_kvs("127.0.0.1", 7000, "email_" + recipient_remove_domain, "metadata.txt", metadata + uidl + "\n", true, metadata);
          // std::cerr << "Put metadata response: " << put_metadata_response << std::endl;
        }    
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
    std::string metadata = get_kvs("127.0.0.1", 7000, "email_" + username, "metadata.txt");
    std::cerr << "Get metadata response: " << metadata << std::endl;
    if (metadata.length()  <= 5 || metadata.length() > 5 && metadata.substr(0, 5) != "--ERR")
    {
      std::string new_metadata = metadata.substr(0, metadata.find(message_id) - 1) + metadata.substr(metadata.find(message_id) + message_id.length());
      put_metadata_response = put_kvs("127.0.0.1", 7000, "email_" + username, "metadata.txt", new_metadata, true, metadata);
      std::cerr << "Put metadata response: " << put_metadata_response << std::endl;
    }    
  }

  // Call delete on email message
  std::string delete_email_response = delete_kvs("127.0.0.1", 7000, "email_" + username, message_id + ".txt");
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

// File system functions
// Get
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

  // Get path of folder 
  std::string folder_path;
  if (req_init_line->path.length() == 8) {
    // path is just /storage
    folder_path = "";
  } else {
    folder_path = req_init_line->path.substr(8);
    if (folder_path == "/") {
      folder_path = "";
    }
  }

  // query backend for the uuid of this folder
  std::string metadata = get_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt");

  // parse metadata file for the uuid of the folder
  std::string searchable = folder_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // use uuid to get folder data
  std::string folder_data = get_kvs("127.0.0.1", 7000, "file_" + username, uuid);

  // start getting children 
  std::vector<File*> files;
  // parse children_files data for children files
  int curr_ind = folder_data.find("children_files:\n");
  folder_data = folder_data.substr(curr_ind + 16);
  int test_ind = folder_data.find("children_folders:\n");
  curr_ind = folder_data.find("\n"); 
  while(curr_ind < test_ind) {
    std::string file_value = folder_data.substr(0, curr_ind);
    col_ind = file_value.find(":");
    std::string file_name = file_value.substr(0, col_ind);
    File* temp = new File(); 
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
  while (curr_ind != std::string::npos) {
    std::string folder_value = folder_data.substr(0, curr_ind);
    col_ind = folder_value.find(":");
    std::string folder_name = folder_value.substr(0, col_ind);
    File* temp = new File();
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

// TODO: Gets file with file_id /file/:message_id
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

  // Get message_id from path

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie 
  std::string username = usernames[sid];

  // Get path of folder 
  std::string file_path = req_init_line->path.substr(5);
  int slash_ind = file_path.find("/");
  std::string file_name = file_path;
  while (slash_ind != std::string::npos) {
    file_name = file_name.substr(slash_ind + 1);
    slash_ind = file_name.find("/");
  }
  
  // TODO: test this
  // query backend for the uuid of this folder
  std::string metadata = get_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt");

  // parse metadata file for the uuid of the folder
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // use uuid to get file data
  std::string file_data = get_kvs("127.0.0.1", 7000, "file_" + username, uuid);

  // parse file_data for actual data 
  int data_ind = temp.find("data:\n");
  std::string data = file_data.substr(data_ind + 6);

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
  // TODO: Date header

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

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
  int extra_index = temp.find("---");
  std::string file_info = temp.substr(0, extra_index);

  // get the path name from the url 
  std::string file_path = req_init_line->path.substr(7);

  std::string new_file_name = file_path + file_name;
  fprintf(stderr, "new file name: %s\n", new_file_name.c_str());

  // Get user sid cookie value
  std::string sid = cookies["sid"];
  // get username from cookie 
  std::string username = usernames[sid];
  fprintf(stderr, "username: %s\n", username.c_str());

  // open metadata file 
  std::string metadata = get_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt");
  fprintf(stderr, "metadata: %s\n", metadata.c_str());

  // parse metadata file for the uuid of the folder
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";
  fprintf(stderr, "uuid: %s\n", uuid.c_str());

  // use uuid to get folder data
  std::string folder_data = get_kvs("127.0.0.1", 7000, "file_" + username, uuid);
  fprintf(stderr, "folder_data: %s\n", folder_data.c_str());
  
  // update folder_data string to include new file in children files 
  int curr_ind = folder_data.find("children_folders:\n");
  std::string prefolder = folder_data.substr(0, curr_ind); 
  std::string postfolder = folder_data.substr(curr_ind);
  std::string new_folder_data = prefolder + file_name + "\n" + postfolder; 
  fprintf(stderr, "new_folder_data: %s\n", new_folder_data.c_str());
  // TODO: handle if this doesn't work later
  std::string command = put_kvs("127.0.0.1", 7000, "file_" + username, uuid, new_folder_data, true, folder_data);

  // now, access next_uuid to get a uuid for this new file 
  std::string next_uuid = get_kvs("127.0.0.1", 7000, "file_" + username, "nextid.txt");
  int new_uuid = std::stoi(next_uuid);
  next_uuid = std::to_string(new_uuid + 1);

  //TODO: handle if this doesn't work later
  command = put_kvs("127.0.0.1", 7000, "file_" + username, "nextid.txt", next_uuid, false, "");

  // now, edit metadata file to contain mapping of file path to this uuid 
  std::string new_metadata = metadata + new_file_name + ":" + std::to_string(new_uuid) + "\n";
  // TODO: handle if this doesn't work later 
  command = put_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt", new_metadata, true, metadata);

  // finally, create the new file 
  command = put_kvs("127.0.0.1", 7000, "file_" + username, std::to_string(new_uuid) + ".txt", "is_directory:false\nparent:" + uuid +"\ndata:" + file_info, false, "");

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 303 Success\r\n";
  std::string headers = "";
  headers += "Location: /storage\r\n";
  headers += "Content-Length: 0\r\n"; // Need this header on all post responses

  return std::make_tuple(init_response, headers, message_body);
}

std::tuple<std::string, std::string, std::string> download_file(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers) {
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

  // open metadata file 
  std::string metadata = get_kvs("127.0.0.1", 7000, "file_" + username, "metadata.txt");

  // parse metadata file for the uuid of the file
  std::string searchable = file_path + ":";
  int ind = metadata.find(searchable);
  std::string temp = metadata.substr(ind);
  int col_ind = temp.find(":");
  int end_ind = temp.find("\n");
  std::string uuid = temp.substr(col_ind + 1, end_ind - col_ind - 1) + ".txt";

  // open file
  std::string file_data = get_kvs("127.0.0.1", 7000, "file_" + username, uuid);
  // parse file for data
  int data_index = file_data.find("data:");
  file_data = file_data.substr(data_index + 5);
  fprintf(stderr, "file_data: %s\n", file_data.c_str());

  int last_index = file_path.find_last_of("/");
  std::string file_name = file_path.substr(last_index + 1);
  fprintf(stderr, "file_name: %s\n", file_name.c_str());

  std::string message_body = "";
  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string headers = "";

  headers += "Content-Disposition: attachment; filename=" + file_name + "\r\n";
  int split_ind = file_data.find("\r\n");
  std::string next_header = file_data.substr(0, split_ind + 2);
  std::string data = file_data.substr(split_ind + 4);
  headers += "Content-Length: " + std::to_string(data.length()) + "\r\n";
  headers += next_header;
  message_body = data;

  return std::make_tuple(init_response, headers, message_body);
}

