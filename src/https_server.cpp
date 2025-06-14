#include "http_server.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>

// Parse query string from URL
void parse_query_string(HttpRequest &req, const std::string &query_string) {
  std::string key, value;
  bool parsing_key = true;
  for (char c : query_string) {
    if (c == '=') {
      parsing_key = false;
    } else if (c == '&') {
      if (!key.empty()) {
        req.query_params[key] = value;
      }
      key.clear();
      value.clear();
      parsing_key = true;
    } else {
      if (parsing_key) {
        key += c;
      } else {
        value += c;
      }
    }
  }
  if (!key.empty()) {
    req.query_params[key] = value;
  }
}

// Parse HTTP request headers and first line
void parse_request(const std::string &request_str, HttpRequest &req) {
  std::istringstream request_stream(request_str);
  std::string line;

  // Parse first line (GET /path?query HTTP/1.1)
  if (std::getline(request_stream, line)) {
    std::istringstream line_stream(line);
    line_stream >> req.method;

    std::string path_with_query;
    line_stream >> path_with_query;

    // Split path and query
    size_t query_pos = path_with_query.find('?');
    if (query_pos != std::string::npos) {
      req.path = path_with_query.substr(0, query_pos);
      std::string query = path_with_query.substr(query_pos + 1);
      parse_query_string(req, query);
    } else {
      req.path = path_with_query;
    }
  }

  // Parse headers
  while (std::getline(request_stream, line) && !line.empty() && line != "\r") {
    size_t colon_pos = line.find(':');
    if (colon_pos != std::string::npos) {
      std::string header_name = line.substr(0, colon_pos);
      std::string header_value = line.substr(colon_pos + 1);

      // Trim leading whitespace from header value
      header_value.erase(0, header_value.find_first_not_of(" \t"));
      // Trim trailing \r if present
      if (!header_value.empty() && header_value.back() == '\r') {
        header_value.pop_back();
      }

      req.headers[header_name] = header_value;
    }
  }

  // For POST requests, extract body
  if (req.method == "POST") {
    // Find where the headers end and body begins
    size_t body_start = request_str.find("\r\n\r\n");
    if (body_start != std::string::npos) {
      body_start += 4; // Move past \r\n\r\n
      req.body = request_str.substr(body_start);
    }
  }
}

void HttpServer::run() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    std::cerr << "Failed to create socket.\n";
    return;
  }

  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = inet_addr("127.0.0.1");
  address.sin_port = htons(port);

  std::cout << "[*] Attempting to bind to 127.0.0.1:" << port << "..."
            << std::endl;
  if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0) {
    std::cerr << "Bind failed.\n";
    close(server_fd);
    return;
  }
  std::cout << "[+] Successfully bound to port " << port << std::endl;

  if (listen(server_fd, 5) < 0) {
    std::cerr << "Listen failed.\n";
    close(server_fd);
    return;
  }

  std::cout << "Server running on port " << port << "...\n";

  while (true) {
    sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);
    int client_fd = accept(server_fd, (sockaddr *)&client_address, &client_len);
    if (client_fd < 0) {
      std::cerr << "Failed to accept connection.\n";
      continue;
    }

    char buffer[4096] = {0};
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
      close(client_fd);
      continue;
    }

    HttpRequest request;
    HttpResponse response;

    // Parse the HTTP request
    std::string request_str(buffer, bytes_read);
    parse_request(request_str, request);

    // Debug output
    std::cout << "Received request: " << request.method << " " << request.path
              << std::endl;
    for (const auto &[key, value] : request.query_params) {
      std::cout << "Query param: " << key << " = " << value << std::endl;
    }

    if (handlers.find(request.path) != handlers.end()) {
      handlers[request.path](request, response);
    } else {
      response.send(404, "Not Found");
    }

    std::string http_response =
        "HTTP/1.1 " + std::to_string(response.status) +
        " OK\r\nContent-Length: " + std::to_string(response.body.size()) +
        "\r\nContent-Type: text/plain\r\n\r\n" + response.body;

    send(client_fd, http_response.c_str(), http_response.size(), 0);
    close(client_fd);
  }

  close(server_fd);
}
