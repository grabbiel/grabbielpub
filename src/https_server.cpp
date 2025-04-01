#include "http_server.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

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
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0) {
    std::cerr << "Bind failed.\n";
    close(server_fd);
    return;
  }

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

    request.method = "POST"; // Simplified for your service
    request.path = "/publish";
    request.body = std::string(buffer, bytes_read);

    if (handler) {
      handler(request, response);
    } else {
      response.send(500, "No handler assigned.");
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
