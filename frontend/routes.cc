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

std::string STATICS_LOC = "./statics/";


// TODO: Can combine these static file rendering functions
// TODO: Put routes in map:function pairs
// GET ROUTES
std::tuple<std::string, std::string, std::string> get_index(ReqInitLine *req_init_line)
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

std::tuple<std::string, std::string, std::string> get_home(ReqInitLine *req_init_line)
{
  // TODO: Add check for if logged in
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

// POST ROUTES
std::vector<std::string> parse_post_body(std::string body)
{
  std::vector<std::string> tokens;
  size_t pos = 0;
  while ((pos = body.find("&")) != std::string::npos)
  {
    tokens.push_back(body.substr(0, pos));
    body.erase(0, pos + 1);
  }
  tokens.push_back(body);
  return tokens;
}

std::tuple<std::string, std::string, std::string> post_login(ReqInitLine *req_init_line, std::string body)
{
  // Parse body for username and password
  std::vector<std::string> tokens = parse_post_body(body);

  std::string username;
  std::string password;

  for (std::string token : tokens)
  {
    if (token.find("username") != std::string::npos)
    {
      username = token.substr(token.find("=") + 1);
    }
    else if (token.find("password") != std::string::npos)
    {
      password = token.substr(token.find("=") + 1);
    }
  }

  // TODO: Check if username and password are correct
  // For now, just return valid if the username=cis505 and password=pass
  if (username != "cis505" || password != "pass")
  {
    std::string init_response = req_init_line->version + " 401 Unauthorized\r\n";
    std::string message_body = "Incorrect username or password. Please try again.";
    std::string headers = "";
    headers += "Content-Length: " + std::to_string(message_body.length()) + "\r\n";
    return std::make_tuple(init_response, headers, message_body);
  } else {
    std::string message_body = "";
    std::string init_response = req_init_line->version + " 303 Found\r\n";
    std::string headers = "";
    headers += "Location: /home\r\n";
    headers += "Content-Length: 0\r\n"; // Need this header on all post responses
    // TODO: Set cookie/auth token
    return std::make_tuple(init_response, headers, message_body);
  }
}