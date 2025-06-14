// http_server.hpp
#pragma once

#include <functional>
#include <map>
#include <netinet/in.h>
#include <string>
#include <unordered_map>

using HttpHandler = std::function<void(int client_fd)>;

struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> query_params;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string body;

  void send(int code, const std::string &response_body) {
    status = code;
    body = response_body;
  }
};

struct HttpServer {
  int port;
  std::map<std::string,
           std::function<void(const HttpRequest &, HttpResponse &)>>
      handlers;

  HttpServer(int p) : port(p) {}

  void route(const std::string &path,
             std::function<void(const HttpRequest &, HttpResponse &)> h) {
    handlers[path] = h;
  }

  void run(); // Implemented in cpp
};
