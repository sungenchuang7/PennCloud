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

std::string STATICS_LOC = "./statics/";
int MAX_BUFF_SIZE = 1000;

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

// TODO: Can combine these static file rendering functions
// TODO: Put routes in map:function pairs

// BACKEND GET ROUTES

// Ping master node for backend server address
std::string get_backend_address()
{
  return "";
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
    return "--ERR failed to get value from backend server";
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

  // Get user sid cookie value
  std::string sid = cookies["sid"];

  // TODO: Make call to backend for message headers and arrival times for user, for now hardcode dummy values

  // Map of email messages {message_id, subject, sender, arrival_time}
  std::unordered_map<std::string, Email> emails = {
      {"message1", {"message1", "Subject 1", "user1@localhost", "Sat Apr 13 00:00:00 2024"}},
      {"message2", {"message2", "Subject 2", "user2@localhost", "Sun Apr 14 00:00:00 2024"}},
      {"message3", {"message3", "Subject 3", "user3@localhost", "Sun Apr 15 00:00:00 2024"}}};

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
  for (auto const &email : emails)
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
  // TODO: Date header

  // Content Type header
  headers += "Content-Type: text/html\r\n";

  // Content Length header
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// TODO: Gets message with message_id /inbox/:message_id
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

  // TODO: Make call to backend for message with message_id, for now hardcode temp messages
  std::string temp_message;
  if (message_id == "message1")
  {
    temp_message = "This is the message for message1.";
  }
  else if (message_id == "message2")
  {
    temp_message = "This is the message for message2.";
  }
  else if (message_id == "message3")
  {
    temp_message = "This is the message for message3.";
  }
  else
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
  std::string email_html = "\n<p>";
  email_html += temp_message;
  email_html += "</p>";
  message_body.insert(insert_index + insert_tag.length(), email_html);

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

// FRONTEND POST ROUTES

// Returns map of post body {key, value}
std::unordered_map<std::string, std::string> parse_post_body(std::string body)
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
  std::unordered_map<std::string, std::string> body_map = parse_post_body(body);

  std::string username = body_map["username"];
  std::string password = body_map["password"];

  // TODO: Ping backend master for backend server address
  // Check if username and password are correct
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
    return std::make_tuple(init_response, headers, message_body);
  }
}

// TODO: Send a message
std::tuple<std::string, std::string, std::string> post_send_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  std::unordered_map<std::string, std::string> body_map = parse_post_body(body);
  std::string recipient = body_map["recipient"];
  std::string subject = body_map["subject"];
  std::string message = body_map["message"];
  std::cerr << "Recipient: " << recipient << std::endl;
  std::cerr << "Subject: " << subject << std::endl;
  std::cerr << "Message: " << message << std::endl;

  // TODO: Convert message to email format
  // DATE: current date and time
  // FROM: usename@domain (not sure how to get this)
  // TO: recipient@domain
  // SUBJECT: subject
  // MESSAGE: message

  // TODO: Create message_id using hash of message with timestamp

  // TODO: Make backend call to insert message into database
  // For now, just return success

  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = "New message sent successfully.";
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// TODO: Delete a message
std::tuple<std::string, std::string, std::string> post_delete_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  // TODO: Call backend to delete the key value pair
  // For now just return success
  std::string init_response = req_init_line->version + " 200 OK\r\n";
  std::string message_body = "Message deleted successfully.";
  std::string headers = "";
  headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
  return std::make_tuple(init_response, headers, message_body);
}

// TODO: Reply to a message
std::tuple<std::string, std::string, std::string> post_reply_message(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> req_headers, std::string body)
{
  return std::make_tuple("", "", "");
}

// TODO: Forward a message
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

  // TODO: Make call to backend for files, for now hardcode dummy values

  // Map of files {file_id, name, is_directory}
  std::unordered_map<std::string, File> files = {
      {"file1", {"file1", "File 1", false}},
      {"file2", {"file2", "File 2", false}},
      {"file3", {"file3", "File 3", false}}};

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
    file_script += "file_id: \"" + file.second.file_id + "\",";
    file_script += "name: \"" + file.second.name + "\",";
    if (file.second.is_directory)
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
  std::string file_id = req_init_line->path.substr(6);

  // TODO: Make call to backend for message with file_id, for now hardcode temp messages
  std::string temp_message;
  std::string name;
  if (file_id == "file1")
  {
    name = "file 1";
    temp_message = "This is file 1.";
  }
  else if (file_id == "file2")
  {
    name = "file 2";
    temp_message = "This is file 2.";
  }
  else if (file_id == "file3")
  {
    name = "file 3";
    temp_message = "This is file 3.";
  }
  else
  {
    std::string init_response = req_init_line->version + " 404 Not Found\r\n";
    std::string message_body = "No such file exists.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
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
  name_html += name;
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