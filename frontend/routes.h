#ifndef ROUTES_H
#define ROUTES_H

#include <tuple>
#include <unordered_map>

struct ReqInitLine
{
  std::string method;
  std::string path;
  std::string version;
  ReqInitLine(std::string method, std::string uri, std::string version) : method(method), path(path), version(version) {}
};

// GET ROUTES
std::tuple<std::string, std::string, std::string> get_index(ReqInitLine *req_init_line);
std::tuple<std::string, std::string, std::string> get_signup(ReqInitLine *req_init_line);
std::tuple<std::string, std::string, std::string> get_home(ReqInitLine *req_init_line, std::unordered_map<std::string, std::string> headers);

// POST ROUTES
std::tuple<std::string, std::string, std::string> post_login(ReqInitLine *req_init_line, std::string body);

#endif