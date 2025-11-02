// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "network/rpc_client.hpp"
#include <cstring>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace coinbasechain {
namespace rpc {

RPCClient::RPCClient(const std::string &socket_path)
    : socket_path_(socket_path), socket_fd_(-1) {}

RPCClient::~RPCClient() { Disconnect(); }

bool RPCClient::Connect() {
  if (socket_fd_ >= 0) {
    return true; // Already connected
  }

  // Create Unix domain socket
  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    return false;
  }

  // Set up socket address
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Connect to node
  if (connect(socket_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Optionally set generous timeouts to avoid indefinite hangs
  struct timeval tv;
  tv.tv_sec = 600; // 10 minutes to accommodate long ops like generate
  tv.tv_usec = 0;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  return true;
}

std::string RPCClient::ExecuteCommand(const std::string &method,
                                      const std::vector<std::string> &params) {
  if (!IsConnected()) {
    throw std::runtime_error("Not connected to node");
  }

  // Build JSON request safely
  nlohmann::json j;
  j["method"] = method;
  if (!params.empty()) {
    j["params"] = nlohmann::json::array();
    for (const auto& p : params) j["params"].push_back(p);
  }

  std::string request_str = j.dump();
  request_str.push_back('\n');

  // Robust send loop
  size_t total_sent = 0;
  while (total_sent < request_str.size()) {
    ssize_t s = send(socket_fd_, request_str.c_str() + total_sent, request_str.size() - total_sent, 0);
    if (s < 0) {
      throw std::runtime_error("Failed to send request");
    }
    if (s == 0) break;
    total_sent += static_cast<size_t>(s);
  }

  // Receive response fully until EOF
  std::string response;
  char buffer[4096];
  while (true) {
    ssize_t n = recv(socket_fd_, buffer, sizeof(buffer), 0);
    if (n < 0) {
      throw std::runtime_error("Failed to receive response");
    }
    if (n == 0) break; // EOF
    response.append(buffer, buffer + n);
  }
  return response;
}

void RPCClient::Disconnect() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

} // namespace rpc
} // namespace coinbasechain
