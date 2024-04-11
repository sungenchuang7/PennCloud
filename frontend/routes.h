#ifndef ROUTES_H
#define ROUTES_H

#include <tuple>

struct ReqInitLine
{
  std::string method;
  std::string path;
  std::string version;
  ReqInitLine(std::string method, std::string uri, std::string version) : method(method), path(path), version(version) {}
};

std::tuple<std::string, std::string, std::string> get_index(ReqInitLine *req_init_line);
std::tuple<std::string, std::string, std::string> get_signup(ReqInitLine *req_init_line);
std::tuple<std::string, std::string, std::string> get_home(ReqInitLine *req_init_line);

#endif