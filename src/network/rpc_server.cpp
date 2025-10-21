// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "network/rpc_server.hpp"
#include "chain/chainparams.hpp"
#include "chain/miner.hpp"
#include "chain/uint.hpp"
#include "network/network_manager.hpp"
#include "network/peer_manager.hpp"
#include "chain/logging.hpp"
#include "chain/time.hpp"
#include "chain/chainstate_manager.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace coinbasechain {
namespace rpc {

RPCServer::RPCServer(const std::string &socket_path,
                     validation::ChainstateManager &chainstate_manager,
                     network::NetworkManager &network_manager,
                     mining::CPUMiner *miner, const chain::ChainParams &params,
                     std::function<void()> shutdown_callback)
    : socket_path_(socket_path), chainstate_manager_(chainstate_manager),
      network_manager_(network_manager), miner_(miner), params_(params),
      shutdown_callback_(shutdown_callback), server_fd_(-1), running_(false),
      shutting_down_(false) {
  RegisterHandlers();
}

RPCServer::~RPCServer() { Stop(); }

// ============================================================================
// SECURITY: Input Validation Helpers
// ============================================================================

std::optional<int> RPCServer::SafeParseInt(const std::string& str, int min, int max) {
  try {
    size_t pos = 0;
    long value = std::stol(str, &pos);

    // Check entire string was consumed
    if (pos != str.size()) {
      return std::nullopt;
    }

    // Check bounds
    if (value < min || value > max) {
      return std::nullopt;
    }

    return static_cast<int>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uint256> RPCServer::SafeParseHash(const std::string& str) {
  // Check length
  if (str.size() != 64) {
    return std::nullopt;
  }

  // Check characters
  for (char c : str) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return std::nullopt;
    }
  }

  uint256 hash;
  hash.SetHex(str);
  return hash;
}

std::optional<uint16_t> RPCServer::SafeParsePort(const std::string& str) {
  try {
    size_t pos = 0;
    long value = std::stol(str, &pos);

    // Check entire string was consumed
    if (pos != str.size()) {
      return std::nullopt;
    }

    // Check valid port range
    if (value < 1 || value > 65535) {
      return std::nullopt;
    }

    return static_cast<uint16_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::string RPCServer::EscapeJSONString(const std::string& str) {
  std::ostringstream oss;
  for (char c : str) {
    switch (c) {
      case '"':  oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default:
        if (c < 0x20) {
          oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c));
        } else {
          oss << c;
        }
    }
  }
  return oss.str();
}

void RPCServer::RegisterHandlers() {
  // Blockchain commands
  handlers_["getinfo"] = [this](const auto &p) { return HandleGetInfo(p); };
  handlers_["getblockchaininfo"] = [this](const auto &p) {
    return HandleGetBlockchainInfo(p);
  };
  handlers_["getblockcount"] = [this](const auto &p) {
    return HandleGetBlockCount(p);
  };
  handlers_["getblockhash"] = [this](const auto &p) {
    return HandleGetBlockHash(p);
  };
  handlers_["getblockheader"] = [this](const auto &p) {
    return HandleGetBlockHeader(p);
  };
  handlers_["getbestblockhash"] = [this](const auto &p) {
    return HandleGetBestBlockHash(p);
  };
  handlers_["getdifficulty"] = [this](const auto &p) {
    return HandleGetDifficulty(p);
  };

  // Mining commands
  handlers_["getmininginfo"] = [this](const auto &p) {
    return HandleGetMiningInfo(p);
  };
  handlers_["getnetworkhashps"] = [this](const auto &p) {
    return HandleGetNetworkHashPS(p);
  };
  handlers_["startmining"] = [this](const auto &p) {
    return HandleStartMining(p);
  };
  handlers_["stopmining"] = [this](const auto &p) {
    return HandleStopMining(p);
  };
  handlers_["generate"] = [this](const auto &p) { return HandleGenerate(p); };

  // Network commands
  handlers_["getconnectioncount"] = [this](const auto &p) {
    return HandleGetConnectionCount(p);
  };
  handlers_["getpeerinfo"] = [this](const auto &p) {
    return HandleGetPeerInfo(p);
  };
  handlers_["addnode"] = [this](const auto &p) { return HandleAddNode(p); };
  handlers_["setban"] = [this](const auto &p) { return HandleSetBan(p); };
  handlers_["listbanned"] = [this](const auto &p) {
    return HandleListBanned(p);
  };
  handlers_["getaddrmaninfo"] = [this](const auto &p) {
    return HandleGetAddrManInfo(p);
  };

  // Control commands
  handlers_["stop"] = [this](const auto &p) { return HandleStop(p); };

  // Testing commands
  handlers_["setmocktime"] = [this](const auto &p) {
    return HandleSetMockTime(p);
  };
  handlers_["invalidateblock"] = [this](const auto &p) {
    return HandleInvalidateBlock(p);
  };
}

bool RPCServer::Start() {
  if (running_) {
    return true;
  }

  // Remove old socket file if it exists
  unlink(socket_path_.c_str());

  // SECURITY FIX: Set restrictive umask before creating socket
  mode_t old_umask = umask(0077);  // rw------- for socket file

  // Create Unix domain socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    umask(old_umask);  // Restore umask
    LOG_ERROR("Failed to create RPC socket");
    return false;
  }

  // Bind to socket
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("Failed to bind RPC socket to {}", socket_path_);
    close(server_fd_);
    server_fd_ = -1;
    umask(old_umask);  // Restore umask
    return false;
  }

  // Restore umask
  umask(old_umask);

  // SECURITY FIX: Explicitly set permissions (double-check)
  chmod(socket_path_.c_str(), 0600);  // Only owner can access

  // Listen for connections
  if (listen(server_fd_, 5) < 0) {
    LOG_ERROR("Failed to listen on RPC socket");
    close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  running_ = true;
  server_thread_ = std::thread(&RPCServer::ServerThread, this);

  LOG_INFO("RPC server started on {}", socket_path_);
  return true;
}

void RPCServer::Stop() {
  if (!running_) {
    return;
  }

  // SECURITY FIX: Set shutdown flag to reject new requests
  shutting_down_.store(true, std::memory_order_release);
  running_ = false;

  if (server_fd_ >= 0) {
    close(server_fd_);
    server_fd_ = -1;
  }

  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  unlink(socket_path_.c_str());

  LOG_INFO("RPC server stopped");
}

void RPCServer::ServerThread() {
  while (running_) {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(server_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (running_) {
        LOG_WARN("Failed to accept RPC connection");
      }
      continue;
    }

    HandleClient(client_fd);
    close(client_fd);
  }
}

void RPCServer::HandleClient(int client_fd) {
  // Check shutdown flag
  if (shutting_down_.load(std::memory_order_acquire)) {
    std::string error = "{\"error\":\"Server shutting down\"}\n";
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  // SECURITY FIX: Use vector to avoid buffer overflow
  std::vector<char> buffer(4096);
  ssize_t received = recv(client_fd, buffer.data(), buffer.size(), 0);

  if (received <= 0) {
    return;
  }

  // SECURITY FIX: Bounds check before creating string
  if (received >= static_cast<ssize_t>(buffer.size())) {
    LOG_ERROR("RPC request too large: {} bytes", received);
    std::string error = "{\"error\":\"Request too large\"}\n";
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  std::string request(buffer.data(), received);

  // SECURITY FIX: Use proper JSON parsing instead of hand-rolled parser
  std::string method;
  std::vector<std::string> params;

  try {
    nlohmann::json j = nlohmann::json::parse(request);

    // Extract method
    if (!j.contains("method") || !j["method"].is_string()) {
      std::string error = "{\"error\":\"Missing or invalid method field\"}\n";
      send(client_fd, error.c_str(), error.size(), 0);
      return;
    }

    method = j["method"].get<std::string>();

    // Extract params (optional)
    if (j.contains("params")) {
      if (j["params"].is_array()) {
        for (const auto& param : j["params"]) {
          if (param.is_string()) {
            params.push_back(param.get<std::string>());
          } else {
            // Convert non-string params to string
            params.push_back(param.dump());
          }
        }
      } else if (j["params"].is_string()) {
        // Single string param
        params.push_back(j["params"].get<std::string>());
      }
    }
  } catch (const nlohmann::json::exception& e) {
    LOG_WARN("RPC JSON parse error: {}", e.what());
    std::string error = "{\"error\":\"Invalid JSON\"}\n";
    send(client_fd, error.c_str(), error.size(), 0);
    return;
  }

  // Execute command
  std::string response = ExecuteCommand(method, params);

  // Send response
  send(client_fd, response.c_str(), response.size(), 0);
}

std::string RPCServer::ExecuteCommand(const std::string &method,
                                      const std::vector<std::string> &params) {
  auto it = handlers_.find(method);
  if (it == handlers_.end()) {
    return "{\"error\":\"Unknown command\"}\n";
  }

  try {
    return it->second(params);
  } catch (const std::exception &e) {
    // SECURITY FIX: Log full error internally but return sanitized error to client
    LOG_ERROR("RPC command '{}' failed: {}", method, e.what());

    // Return sanitized error message (escape special characters)
    std::ostringstream oss;
    oss << "{\"error\":\"" << EscapeJSONString(e.what()) << "\"}\n";
    return oss.str();
  }
}

std::string RPCServer::HandleGetInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Get difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"version\": \"0.1.0\",\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\",\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"headers\": " << height << ",\n"
      << "  \"bestblockhash\": \""
      << (tip ? tip->GetBlockHash().GetHex() : "null") << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"mediantime\": " << (tip ? tip->GetMedianTimePast() : 0) << ",\n"
      << "  \"connections\": 0\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockchainInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Calculate difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\",\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"headers\": " << height << ",\n"
      << "  \"bestblockhash\": \""
      << (tip ? tip->GetBlockHash().GetHex() : "null") << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"time\": " << (tip ? tip->nTime : 0) << ",\n"
      << "  \"mediantime\": " << (tip ? tip->GetMedianTimePast() : 0) << ",\n"
      << "  \"chainwork\": \"" << (tip ? tip->nChainWork.GetHex() : "0")
      << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockCount(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  std::ostringstream oss;
  oss << height << "\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBlockHash(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing height parameter\"}\n";
  }

  // SECURITY FIX: Safe integer parsing with bounds check
  auto height_opt = SafeParseInt(params[0], 0, 10000000);
  if (!height_opt) {
    return "{\"error\":\"Invalid height (must be 0-10000000)\"}\n";
  }

  int height = *height_opt;
  auto *index = chainstate_manager_.GetBlockAtHeight(height);

  if (!index) {
    return "{\"error\":\"Block height out of range\"}\n";
  }

  return index->GetBlockHash().GetHex() + "\n";
}

std::string
RPCServer::HandleGetBlockHeader(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing block hash parameter\"}\n";
  }

  // SECURITY FIX: Safe hash parsing with validation
  auto hash_opt = SafeParseHash(params[0]);
  if (!hash_opt) {
    return "{\"error\":\"Invalid block hash (must be 64 hex characters)\"}\n";
  }

  uint256 hash = *hash_opt;

  auto *index = chainstate_manager_.LookupBlockIndex(hash);
  if (!index) {
    return "{\"error\":\"Block not found\"}\n";
  }

  // Calculate difficulty
  double difficulty = 1.0;
  if (index->nBits != 0) {
    int nShift = (index->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(index->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  // Calculate confirmations
  auto *tip = chainstate_manager_.GetTip();
  int confirmations = -1;
  if (chainstate_manager_.IsOnActiveChain(index)) {
    confirmations = tip->nHeight - index->nHeight + 1;
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"hash\": \"" << index->GetBlockHash().GetHex() << "\",\n"
      << "  \"confirmations\": " << confirmations << ",\n"
      << "  \"height\": " << index->nHeight << ",\n"
      << "  \"version\": " << index->nVersion << ",\n"
      << "  \"versionHex\": \"" << std::hex << std::setw(8) << std::setfill('0')
      << index->nVersion << std::dec << "\",\n"
      << "  \"time\": " << index->nTime << ",\n"
      << "  \"mediantime\": " << index->GetMedianTimePast() << ",\n"
      << "  \"nonce\": " << index->nNonce << ",\n"
      << "  \"bits\": \"" << std::hex << std::setw(8) << std::setfill('0')
      << index->nBits << std::dec << "\",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"chainwork\": \"" << index->nChainWork.GetHex() << "\",\n"
      << "  \"previousblockhash\": \""
      << (index->pprev ? index->pprev->GetBlockHash().GetHex() : "null")
      << "\",\n"
      << "  \"rx_hash\": \"" << index->hashRandomX.GetHex() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetBestBlockHash(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  if (!tip) {
    return "null\n";
  }

  return tip->GetBlockHash().GetHex() + "\n";
}

std::string
RPCServer::HandleGetConnectionCount(const std::vector<std::string> &params) {
  size_t count = network_manager_.active_peer_count();

  std::ostringstream oss;
  oss << count << "\n";
  return oss.str();
}

std::string
RPCServer::HandleGetPeerInfo(const std::vector<std::string> &params) {
  // Get all peers from NetworkManager
  auto all_peers = network_manager_.peer_manager().get_all_peers();

  std::ostringstream oss;
  oss << "[\n";

  for (size_t i = 0; i < all_peers.size(); i++) {
    const auto &peer = all_peers[i];
    if (!peer)
      continue;

    const auto &stats = peer->stats();

    // Calculate connection duration in seconds
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        now - stats.connected_time);

    // Get misbehavior score from PeerManager
    int misbehavior_score = 0;
    bool should_disconnect = false;
    try {
      auto &peer_mgr = network_manager_.peer_manager();
      misbehavior_score = peer_mgr.GetMisbehaviorScore(peer->id());
      should_disconnect = peer_mgr.ShouldDisconnect(peer->id());
    } catch (...) {
      // Peer might not be in sync manager yet (handshake incomplete)
    }

    oss << "  {\n"
        << "    \"id\": " << peer->id() << ",\n"
        << "    \"addr\": \"" << peer->address() << ":" << peer->port()
        << "\",\n"
        << "    \"inbound\": " << (peer->is_inbound() ? "true" : "false")
        << ",\n"
        << "    \"connected\": " << (peer->is_connected() ? "true" : "false")
        << ",\n"
        << "    \"successfully_connected\": "
        << (peer->successfully_connected() ? "true" : "false") << ",\n"
        << "    \"version\": " << peer->version() << ",\n"
        << "    \"subver\": \"" << peer->user_agent() << "\",\n"
        << "    \"services\": \"" << std::hex << std::setfill('0')
        << std::setw(16) << peer->services() << std::dec << "\",\n"
        << "    \"startingheight\": " << peer->start_height() << ",\n"
        << "    \"pingtime\": " << (stats.ping_time_ms / 1000.0) << ",\n"
        << "    \"bytessent\": " << stats.bytes_sent << ",\n"
        << "    \"bytesrecv\": " << stats.bytes_received << ",\n"
        << "    \"conntime\": " << duration.count() << ",\n"
        << "    \"misbehavior_score\": " << misbehavior_score << ",\n"
        << "    \"should_disconnect\": "
        << (should_disconnect ? "true" : "false") << "\n";

    if (i < all_peers.size() - 1) {
      oss << "  },\n";
    } else {
      oss << "  }\n";
    }
  }

  oss << "]\n";
  return oss.str();
}

std::string RPCServer::HandleAddNode(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing node address parameter\"}\n";
  }

  std::string node_addr = params[0];
  std::string command = "add"; // Default command
  if (params.size() > 1) {
    command = params[1];
  }

  // Parse address:port
  size_t colon_pos = node_addr.find_last_of(':');
  if (colon_pos == std::string::npos) {
    return "{\"error\":\"Invalid address format (use host:port)\"}\n";
  }

  std::string host = node_addr.substr(0, colon_pos);
  std::string port_str = node_addr.substr(colon_pos + 1);

  // SECURITY FIX: Safe port parsing with validation
  auto port_opt = SafeParsePort(port_str);
  if (!port_opt) {
    return "{\"error\":\"Invalid port (must be 1-65535)\"}\n";
  }

  uint16_t port = *port_opt;

  if (command == "add") {
    // Parse IP address and create NetworkAddress
    boost::system::error_code ec;
    auto ip_addr = boost::asio::ip::make_address(host, ec);

    if (ec) {
      std::ostringstream oss;
      oss << "{\"error\":\"Invalid IP address: " << ec.message() << "\"}\n";
      return oss.str();
    }

    // Create NetworkAddress
    protocol::NetworkAddress addr;
    addr.services = protocol::ServiceFlags::NODE_NETWORK;
    addr.port = port;

    // Convert to 16-byte IPv6 format (IPv4-mapped if needed)
    if (ip_addr.is_v4()) {
      auto v6_mapped = boost::asio::ip::make_address_v6(
          boost::asio::ip::v4_mapped, ip_addr.to_v4());
      auto bytes = v6_mapped.to_bytes();
      std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
    } else {
      auto bytes = ip_addr.to_v6().to_bytes();
      std::copy(bytes.begin(), bytes.end(), addr.ip.begin());
    }

    // Connect to the node
    bool success = network_manager_.connect_to(addr);
    if (!success) {
      return "{\"error\":\"Failed to connect to node\"}\n";
    }

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Connection initiated to " << node_addr << "\"\n"
        << "}\n";
    return oss.str();
  } else if (command == "remove") {
    // Find peer by address:port and disconnect (thread-safe)
    int peer_id =
        network_manager_.peer_manager().find_peer_by_address(host, port);

    if (peer_id < 0) {
      LOG_WARN("addnode remove: Peer not found: {}", node_addr);
      std::ostringstream oss;
      oss << "{\n"
          << "  \"error\": \"Peer not found: " << node_addr << "\"\n"
          << "}\n";
      return oss.str();
    }

    LOG_INFO("addnode remove: Found peer {} at {}, disconnecting", peer_id,
             node_addr);
    network_manager_.disconnect_from(peer_id);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Disconnected from " << node_addr << "\"\n"
        << "}\n";
    return oss.str();
  } else {
    return "{\"error\":\"Unknown command (use 'add' or 'remove')\"}\n";
  }
}

std::string RPCServer::HandleSetBan(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing subnet/IP parameter\"}\n";
  }

  std::string address = params[0];
  std::string command = "add"; // Default command
  if (params.size() > 1) {
    command = params[1];
  }

  if (command == "add") {
    // Optional bantime parameter (in seconds, 0 = permanent)
    int64_t bantime = 0; // 0 = permanent by default
    if (params.size() > 2) {
      try {
        bantime = std::stoll(params[2]);
      } catch (...) {
        return "{\"error\":\"Invalid bantime parameter\"}\n";
      }
    }

    // Ban the address
    network_manager_.ban_man().Ban(address, bantime);

    std::ostringstream oss;
    if (bantime > 0) {
      oss << "{\n"
          << "  \"success\": true,\n"
          << "  \"message\": \"Banned " << address << " for " << bantime
          << " seconds\"\n"
          << "}\n";
    } else {
      oss << "{\n"
          << "  \"success\": true,\n"
          << "  \"message\": \"Permanently banned " << address << "\"\n"
          << "}\n";
    }
    return oss.str();

  } else if (command == "remove") {
    network_manager_.ban_man().Unban(address);

    std::ostringstream oss;
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Unbanned " << address << "\"\n"
        << "}\n";
    return oss.str();

  } else {
    return "{\"error\":\"Unknown command (use 'add' or 'remove')\"}\n";
  }
}

std::string
RPCServer::HandleListBanned(const std::vector<std::string> &params) {
  auto banned = network_manager_.ban_man().GetBanned();

  std::ostringstream oss;
  oss << "[\n";

  size_t i = 0;
  for (const auto &[address, entry] : banned) {
    oss << "  {\n"
        << "    \"address\": \"" << address << "\",\n"
        << "    \"banned_until\": " << entry.nBanUntil << ",\n"
        << "    \"ban_created\": " << entry.nCreateTime << ",\n"
        << "    \"ban_reason\": \"manually added\"\n"
        << "  }";

    if (i < banned.size() - 1) {
      oss << ",";
    }
    oss << "\n";
    i++;
  }

  oss << "]\n";
  return oss.str();
}

std::string
RPCServer::HandleGetAddrManInfo(const std::vector<std::string> &params) {
  auto &addr_man = network_manager_.address_manager();

  size_t total = addr_man.size();
  size_t tried = addr_man.tried_count();
  size_t new_addrs = addr_man.new_count();

  std::ostringstream oss;
  oss << "{\n"
      << "  \"total\": " << total << ",\n"
      << "  \"tried\": " << tried << ",\n"
      << "  \"new\": " << new_addrs << "\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetDifficulty(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();

  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  std::ostringstream oss;
  oss << difficulty << "\n";
  return oss.str();
}

std::string
RPCServer::HandleGetMiningInfo(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();
  int height = tip ? tip->nHeight : -1;

  // Calculate difficulty
  double difficulty = 1.0;
  if (tip && tip->nBits != 0) {
    int nShift = (tip->nBits >> 24) & 0xff;
    double dDiff = (double)0x000fffff / (double)(tip->nBits & 0x00ffffff);
    while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
    }
    while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
    }
    difficulty = dDiff;
  }

  // Calculate network hashrate (simplified - based on last DEFAULT_HASHRATE_CALCULATION_BLOCKS)
  double networkhashps = 0.0;
  if (tip && tip->nHeight > 0) {
    int nblocks = std::min(protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS, tip->nHeight);
    const chain::CBlockIndex *pb = tip;
    const chain::CBlockIndex *pb0 = pb;

    // Walk back nblocks
    for (int i = 0; i < nblocks && pb0->pprev; i++) {
      pb0 = pb0->pprev;
    }

    int64_t timeDiff = pb->nTime - pb0->nTime;
    if (timeDiff > 0) {
      arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
      networkhashps = workDiff.getdouble() / timeDiff;
    }
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"blocks\": " << height << ",\n"
      << "  \"difficulty\": " << difficulty << ",\n"
      << "  \"networkhashps\": " << networkhashps << ",\n"
      << "  \"chain\": \"" << params_.GetChainTypeString() << "\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleGetNetworkHashPS(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();

  // Default to DEFAULT_HASHRATE_CALCULATION_BLOCKS
  int nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
  if (!params.empty()) {
    nblocks = std::stoi(params[0]);
    if (nblocks == -1 || nblocks == 0) {
      nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
    }
  }

  double networkhashps = 0.0;
  if (tip && tip->nHeight > 0) {
    nblocks = std::min(nblocks, tip->nHeight);
    const chain::CBlockIndex *pb = tip;
    const chain::CBlockIndex *pb0 = pb;

    // Walk back nblocks
    for (int i = 0; i < nblocks && pb0->pprev; i++) {
      pb0 = pb0->pprev;
    }

    int64_t timeDiff = pb->nTime - pb0->nTime;
    if (timeDiff > 0) {
      arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
      networkhashps = workDiff.getdouble() / timeDiff;
    }
  }

  std::ostringstream oss;
  oss << networkhashps << "\n";
  return oss.str();
}

std::string
RPCServer::HandleStartMining(const std::vector<std::string> &params) {
  if (!miner_) {
    return "{\"error\":\"Mining not available\"}\n";
  }

  if (miner_->IsMining()) {
    return "{\"error\":\"Already mining\"}\n";
  }

  // Note: Miner is now single-threaded for regtest (params ignored)
  bool started = miner_->Start();
  if (!started) {
    return "{\"error\":\"Failed to start mining\"}\n";
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"mining\": true,\n"
      << "  \"message\": \"Mining started\"\n"
      << "}\n";
  return oss.str();
}

std::string
RPCServer::HandleStopMining(const std::vector<std::string> &params) {
  if (!miner_) {
    return "{\"error\":\"Mining not available\"}\n";
  }

  if (!miner_->IsMining()) {
    return "{\"error\":\"Not currently mining\"}\n";
  }

  miner_->Stop();

  std::ostringstream oss;
  oss << "{\n"
      << "  \"mining\": false,\n"
      << "  \"message\": \"Mining stopped\"\n"
      << "}\n";
  return oss.str();
}

std::string RPCServer::HandleGenerate(const std::vector<std::string> &params) {
  if (!miner_) {
    return "{\"error\":\"Mining not available\"}\n";
  }

  // SECURITY FIX: Only allow generate on regtest
  if (params_.GetChainType() != chain::ChainType::REGTEST) {
    return "{\"error\":\"generate only available on regtest\"}\n";
  }

  if (params.empty()) {
    return "{\"error\":\"Missing number of blocks parameter\"}\n";
  }

  // SECURITY FIX: Safe integer parsing and reduced limit
  auto num_blocks_opt = SafeParseInt(params[0], 1, 100);
  if (!num_blocks_opt) {
    return "{\"error\":\"Invalid number of blocks (must be 1-100)\"}\n";
  }

  int num_blocks = *num_blocks_opt;

  // Get starting height
  auto *start_tip = chainstate_manager_.GetTip();
  int start_height = start_tip ? start_tip->nHeight : -1;

  std::vector<std::string> block_hashes;

  // Mine blocks one at a time
  for (int i = 0; i < num_blocks; i++) {
    // Start mining (single-threaded for regtest)
    if (!miner_->Start()) {
      std::ostringstream oss;
      oss << "{\"error\":\"Failed to start mining at block " << i << "\"}\n";
      return oss.str();
    }

    // Wait for block to be found (up to 60 seconds)
    int wait_count = 0;
    auto *current_tip = chainstate_manager_.GetTip();
    int current_height = current_tip ? current_tip->nHeight : -1;
    int expected_height = start_height + i + 1;

    while (current_height < expected_height && wait_count < 600) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      current_tip = chainstate_manager_.GetTip();
      current_height = current_tip ? current_tip->nHeight : -1;
      wait_count++;
    }

    // Stop mining for this block
    miner_->Stop();

    if (current_height < expected_height) {
      std::ostringstream oss;
      oss << "{\"error\":\"Timeout waiting for block " << (i + 1) << "\"}\n";
      return oss.str();
    }

    // Get the hash of the newly mined block
    if (current_tip) {
      block_hashes.push_back(current_tip->GetBlockHash().GetHex());
    }

    // Small delay between blocks to avoid rapid-fire issues
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Return list of block hashes
  std::ostringstream oss;
  oss << "[\n";
  for (size_t i = 0; i < block_hashes.size(); i++) {
    oss << "  \"" << block_hashes[i] << "\"";
    if (i < block_hashes.size() - 1) {
      oss << ",";
    }
    oss << "\n";
  }
  oss << "]\n";
  return oss.str();
}

std::string RPCServer::HandleStop(const std::vector<std::string> &params) {
  LOG_INFO("Received stop command via RPC");

  // SECURITY FIX: Set shutdown flag immediately to reject new requests
  shutting_down_.store(true, std::memory_order_release);

  // Trigger graceful shutdown via callback
  if (shutdown_callback_) {
    shutdown_callback_();
  }

  return "\"CoinbaseChain stopping\"\n";
}

std::string
RPCServer::HandleSetMockTime(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing timestamp parameter\"}\n";
  }

  // SECURITY FIX: Only allow setmocktime on regtest/testnet
  if (params_.GetChainType() == chain::ChainType::MAIN) {
    return "{\"error\":\"setmocktime not allowed on mainnet\"}\n";
  }

  int64_t mock_time;
  try {
    mock_time = std::stoll(params[0]);
  } catch (...) {
    return "{\"error\":\"Invalid timestamp format\"}\n";
  }

  // SECURITY FIX: Validate reasonable range (year 1970 to 2106)
  // Allow 0 for disabling mock time
  if (mock_time != 0 && (mock_time < 0 || mock_time > 4294967295LL)) {
    return "{\"error\":\"Timestamp out of range (must be 0 or 1-4294967295)\"}\n";
  }

  // Set mock time (0 to disable)
  util::SetMockTime(mock_time);

  std::ostringstream oss;
  if (mock_time == 0) {
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"message\": \"Mock time disabled\"\n"
        << "}\n";
  } else {
    oss << "{\n"
        << "  \"success\": true,\n"
        << "  \"mocktime\": " << mock_time << ",\n"
        << "  \"message\": \"Mock time set to " << mock_time << "\"\n"
        << "}\n";
  }

  return oss.str();
}

std::string
RPCServer::HandleInvalidateBlock(const std::vector<std::string> &params) {
  if (params.empty()) {
    return "{\"error\":\"Missing block hash parameter\"}\n";
  }

  // SECURITY FIX: Safe hash parsing with validation
  auto hash_opt = SafeParseHash(params[0]);
  if (!hash_opt) {
    return "{\"error\":\"Invalid block hash (must be 64 hex characters)\"}\n";
  }

  uint256 hash = *hash_opt;

  // Check if block exists
  auto *index = chainstate_manager_.LookupBlockIndex(hash);
  if (!index) {
    return "{\"error\":\"Block not found\"}\n";
  }

  // Invalidate the block
  bool success = chainstate_manager_.InvalidateBlock(hash);

  if (!success) {
    return "{\"error\":\"Failed to invalidate block\"}\n";
  }

  std::ostringstream oss;
  oss << "{\n"
      << "  \"success\": true,\n"
      << "  \"hash\": \"" << hash.GetHex() << "\",\n"
      << "  \"message\": \"Block and all descendants invalidated\"\n"
      << "}\n";

  return oss.str();
}

} // namespace rpc
} // namespace coinbasechain
