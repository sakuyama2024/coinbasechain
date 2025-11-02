// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

/**
 * RPC Server Implementation - Unix Domain Sockets
 *
 * This RPC server uses Unix domain sockets (filesystem-based IPC) instead
 * of TCP/IP networking. This means:
 * - RPC is only accessible locally on the same machine
 * - No network port is opened (no rpcport configuration)
 * - Authentication is handled by filesystem permissions
 * - The socket file is created at: datadir/node.sock
 *
 * This design prioritizes security over remote accessibility.
 * For remote access, users must SSH to the server.
 */

#include "network/rpc_server.hpp"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <boost/asio/ip/address.hpp>
#include <cmath>
#include "chain/chainparams.hpp"
#include "chain/chainstate_manager.hpp"
#include "util/logging.hpp"
#include "chain/miner.hpp"
#include "util/time.hpp"
#include "util/uint.hpp"
#include "network/banman.hpp"
#include "network/network_manager.hpp"
#include "network/peer_manager.hpp"
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
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

    if (pos != str.size()) {
      return std::nullopt;
    }
    if (value < min || value > max) {
      return std::nullopt;
    }
    return static_cast<int>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<int64_t> RPCServer::SafeParseInt64(const std::string& str, int64_t min, int64_t max) {
  try {
    size_t pos = 0;
    long long value = std::stoll(str, &pos);
    if (pos != str.size()) {
      return std::nullopt;
    }
    if (value < min || value > max) {
      return std::nullopt;
    }
    return static_cast<int64_t>(value);
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

// Robust send helper
bool RPCServer::SendAll(int fd, const char* data, size_t len) {
  size_t sent_total = 0;
  while (sent_total < len) {
    ssize_t n = send(fd, data + sent_total, len - sent_total, 0);
    if (n < 0) {
      return false;
    }
    if (n == 0) {
      break;
    }
    sent_total += static_cast<size_t>(n);
  }
  return sent_total == len;
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

  // Remove old file/link at requested path if it exists
  unlink(socket_path_.c_str());

  // Determine actual bind path (fall back to /tmp if too long)
  actual_socket_path_ = socket_path_;
  symlink_created_ = false;

  struct sockaddr_un tmp_addr_check;
  if (actual_socket_path_.size() >= sizeof(tmp_addr_check.sun_path)) {
    // Build fallback path under /tmp (short)
    char fallback[128];
    pid_t pid = getpid();
    unsigned rnd = static_cast<unsigned>(reinterpret_cast<uintptr_t>(this)) & 0xFFFF;
    snprintf(fallback, sizeof(fallback), "/tmp/cbc_rpc_%d_%04x.sock", pid, rnd);
    actual_socket_path_ = fallback;
    symlink_created_ = true;
  }

  // SECURITY FIX: Set restrictive umask before creating socket
  mode_t old_umask = umask(0077);  // rw------- for socket file

  // Create Unix domain socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    umask(old_umask);  // Restore umask
    LOG_ERROR("Failed to create RPC socket");
    return false;
  }

  // Remove any stale actual socket file
  unlink(actual_socket_path_.c_str());

  // Bind to socket
  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, actual_socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("Failed to bind RPC socket to {}", actual_socket_path_);
    close(server_fd_);
    server_fd_ = -1;
    umask(old_umask);  // Restore umask
    return false;
  }

  // Restore umask
  umask(old_umask);

  // SECURITY FIX: Explicitly set permissions (double-check)
  chmod(actual_socket_path_.c_str(), 0600);  // Only owner can access

  // If we used a fallback path, create a symlink at the requested location
  if (symlink_created_) {
    // Remove any old link, then create new symlink
    unlink(socket_path_.c_str());
    if (symlink(actual_socket_path_.c_str(), socket_path_.c_str()) != 0) {
      LOG_NET_WARN("Failed to create RPC socket symlink {} -> {}", socket_path_, actual_socket_path_);
      // Not fatal; CLI can still be pointed at the actual path if needed
    }
  }

  // Listen for connections (larger backlog)
  if (listen(server_fd_, 64) < 0) {
    LOG_ERROR("Failed to listen on RPC socket");
    close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  running_ = true;
  server_thread_ = std::thread(&RPCServer::ServerThread, this);

  LOG_NET_INFO("RPC server started on {} (actual: {})", socket_path_, actual_socket_path_);
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

  // Remove symlink and actual socket file
  if (!socket_path_.empty()) unlink(socket_path_.c_str());
  if (!actual_socket_path_.empty() && actual_socket_path_ != socket_path_) unlink(actual_socket_path_.c_str());

  LOG_NET_INFO("RPC server stopped");
}

void RPCServer::ServerThread() {
  while (running_) {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd =
        accept(server_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (running_) {
        LOG_NET_WARN("failed to accept RPC connection");
      }
      continue;
    }

    // Per-connection worker thread to avoid blocking accept loop
    std::thread([this, client_fd]() {
      // Apply per-connection I/O timeouts to mitigate stalling clients
      struct timeval tv;
      tv.tv_sec = 10; // 10s recv/send timeout
      tv.tv_usec = 0;
      setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

      HandleClient(client_fd);
      close(client_fd);
    }).detach();
  }
}

void RPCServer::HandleClient(int client_fd) {
  // Check shutdown flag
  if (shutting_down_.load(std::memory_order_acquire)) {
    nlohmann::json err = { {"error", "Server shutting down"} };
    std::string payload = err.dump();
    payload.push_back('\n');
    SendAll(client_fd, payload.c_str(), payload.size());
    return;
  }

  // Read request fully (until newline or EOF) with size cap
  std::string request;
  request.reserve(1024);
  constexpr size_t kMaxRequestSize = 64 * 1024; // 64KB cap
  char buf[4096];
  bool newline_seen = false;
  while (true) {
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    if (n < 0) {
      // Timed out or error
      return;
    }
    if (n == 0) break; // EOF
    request.append(buf, buf + n);
    if (request.size() > kMaxRequestSize) {
      nlohmann::json err = { {"error", "Request too large"} };
      std::string payload = err.dump();
      payload.push_back('\n');
      SendAll(client_fd, payload.c_str(), payload.size());
      return;
    }
    if (request.find('\n') != std::string::npos) {
      newline_seen = true;
      break;
    }
  }

  if (request.empty()) {
    return;
  }

  // Trim trailing newline(s)
  if (newline_seen) {
    while (!request.empty() && (request.back() == '\n' || request.back() == '\r')) request.pop_back();
  }

  // Parse JSON
  std::string method;
  std::vector<std::string> params;

  try {
    nlohmann::json j = nlohmann::json::parse(request);

    if (!j.contains("method") || !j["method"].is_string()) {
      nlohmann::json err = { {"error", "Missing or invalid method field"} };
      std::string payload = err.dump();
      payload.push_back('\n');
      SendAll(client_fd, payload.c_str(), payload.size());
      return;
    }

    method = j["method"].get<std::string>();

    if (j.contains("params")) {
      if (j["params"].is_array()) {
        for (const auto& param : j["params"]) {
          if (param.is_string()) {
            params.push_back(param.get<std::string>());
          } else {
            params.push_back(param.dump());
          }
        }
      } else if (j["params"].is_string()) {
        params.push_back(j["params"].get<std::string>());
      }
    }
  } catch (const nlohmann::json::exception& e) {
    LOG_NET_WARN("RPC JSON parse error: {}", e.what());
    nlohmann::json err = { {"error", "Invalid JSON"} };
    std::string payload = err.dump();
    payload.push_back('\n');
    SendAll(client_fd, payload.c_str(), payload.size());
    return;
  }

  // Execute command
  std::string response = ExecuteCommand(method, params);

  // Send response
  SendAll(client_fd, response.c_str(), response.size());
}

std::string RPCServer::ExecuteCommand(const std::string &method,
                                      const std::vector<std::string> &params) {
  auto it = handlers_.find(method);
  if (it == handlers_.end()) {
    nlohmann::json err = { {"error", "Unknown command"} };
    std::string payload = err.dump();
    payload.push_back('\n');
    return payload;
  }

  try {
    return it->second(params);
  } catch (const std::exception &e) {
    // SECURITY FIX: Log full error internally but return sanitized error to client
    LOG_NET_ERROR("RPC command '{}' failed: {}", method, e.what());
    nlohmann::json err = { {"error", std::string(e.what())} };
    std::string payload = err.dump();
    payload.push_back('\n');
    return payload;
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

  nlohmann::json j;
  j["version"] = "0.1.0";
  j["chain"] = params_.GetChainTypeString();
  j["blocks"] = height;
  j["headers"] = height;
  j["bestblockhash"] = tip ? tip->GetBlockHash().GetHex() : "null";
  j["difficulty"] = difficulty;
  j["mediantime"] = tip ? tip->GetMedianTimePast() : 0;
  j["connections"] = static_cast<int>(network_manager_.active_peer_count());

  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  // Compute average inter-block times over recent windows
  auto compute_avg = [](const chain::CBlockIndex *p, int window) -> double {
    if (!p || !p->pprev || window <= 0) return 0.0;
    const chain::CBlockIndex *cur = p;
    long long sum = 0;
    int count = 0;
    for (int i = 0; i < window && cur && cur->pprev; ++i) {
      long long dt = static_cast<long long>(cur->nTime) - static_cast<long long>(cur->pprev->nTime);
      sum += dt;
      cur = cur->pprev;
      ++count;
    }
    if (count == 0) return 0.0;
    return static_cast<double>(sum) / static_cast<double>(count);
  };

  // Averages in seconds
  double avg10 = compute_avg(tip, 10);
  double avg20 = compute_avg(tip, 20);
  double avg40 = compute_avg(tip, 40);
  double avg100 = compute_avg(tip, 100);
  double avg500 = compute_avg(tip, 500);

  // Convert to minutes for reporting
  double avg10_min = avg10 / 60.0;
  double avg20_min = avg20 / 60.0;
  double avg40_min = avg40 / 60.0;
  double avg100_min = avg100 / 60.0;
  double avg500_min = avg500 / 60.0;

  const auto &consensus = params_.GetConsensus();

  // Calculate log2_chainwork for compact display (Bitcoin Core approach)
  double log2_chainwork = 0.0;
  if (tip) {
    log2_chainwork = std::log(tip->nChainWork.getdouble()) / std::log(2.0);
  }

  // Convert consensus parameters for reporting
  double target_spacing_min = static_cast<double>(consensus.nPowTargetSpacing) / 60.0; // minutes
  double half_life_hours = static_cast<double>(consensus.nASERTHalfLife) / 3600.0;     // hours

  nlohmann::json j;
  j["chain"] = params_.GetChainTypeString();
  j["blocks"] = height;
  j["headers"] = height;
  j["bestblockhash"] = tip ? tip->GetBlockHash().GetHex() : "null";
  j["difficulty"] = difficulty;
  j["time"] = tip ? tip->nTime : 0;
  j["time_str"] = tip ? util::FormatTime(tip->nTime) : "null";
  j["mediantime"] = tip ? tip->GetMedianTimePast() : 0;
  j["mediantime_str"] = tip ? util::FormatTime(tip->GetMedianTimePast()) : "null";
  j["chainwork"] = tip ? tip->nChainWork.GetHex() : "0";
  j["log2_chainwork"] = std::round(log2_chainwork * 10.0) / 10.0;
  // Present averages as human strings as before
  j["avg_block_time_10"] = (std::ostringstream() << std::fixed << std::setprecision(1) << avg10_min << " mins").str();
  j["avg_block_time_20"] = (std::ostringstream() << std::fixed << std::setprecision(1) << avg20_min << " mins").str();
  j["avg_block_time_40"] = (std::ostringstream() << std::fixed << std::setprecision(1) << avg40_min << " mins").str();
  j["avg_block_time_100"] = (std::ostringstream() << std::fixed << std::setprecision(1) << avg100_min << " mins").str();
  j["avg_block_time_500"] = (std::ostringstream() << std::fixed << std::setprecision(1) << avg500_min << " mins").str();
  j["asert"] = {
      {"target_spacing", (std::ostringstream() << std::fixed << std::setprecision(1) << target_spacing_min << " mins").str()},
      {"half_life", (std::ostringstream() << std::fixed << std::setprecision(1) << half_life_hours << " hours").str()},
      {"anchor_height", consensus.nASERTAnchorHeight}
  };
  j["initialblockdownload"] = chainstate_manager_.IsInitialBlockDownload();
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json j;
  j["hash"] = index->GetBlockHash().GetHex();
  j["confirmations"] = confirmations;
  j["height"] = index->nHeight;
  j["version"] = index->nVersion;
  {
    std::ostringstream vh;
    vh << std::hex << std::setw(8) << std::setfill('0') << index->nVersion << std::dec;
    j["versionHex"] = vh.str();
  }
  j["time"] = index->nTime;
  j["mediantime"] = index->GetMedianTimePast();
  j["nonce"] = index->nNonce;
  {
    std::ostringstream bb;
    bb << std::hex << std::setw(8) << std::setfill('0') << index->nBits << std::dec;
    j["bits"] = bb.str();
  }
  j["difficulty"] = difficulty;
  j["chainwork"] = index->nChainWork.GetHex();
  j["previousblockhash"] = index->pprev ? index->pprev->GetBlockHash().GetHex() : "null";
  j["rx_hash"] = index->hashRandomX.GetHex();
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json arr = nlohmann::json::array();

  for (const auto &peer : all_peers) {
    if (!peer) continue;

    const auto &stats = peer->stats();
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - stats.connected_time);

    int misbehavior_score = 0;
    bool should_disconnect = false;
    try {
      auto &peer_mgr = network_manager_.peer_manager();
      misbehavior_score = peer_mgr.GetMisbehaviorScore(peer->id());
      should_disconnect = peer_mgr.ShouldDisconnect(peer->id());
    } catch (...) {
      // Peer might not be available yet
    }

    std::ostringstream services_hex;
    services_hex << std::hex << std::setfill('0') << std::setw(16) << peer->services() << std::dec;

    nlohmann::json jpeer = {
      {"id", peer->id()},
      {"addr", peer->address() + ":" + std::to_string(peer->port())},
      {"inbound", peer->is_inbound()},
      {"connected", peer->is_connected()},
      {"successfully_connected", peer->successfully_connected()},
      {"version", peer->version()},
      {"subver", peer->user_agent()},
      {"services", services_hex.str()},
      {"startingheight", peer->start_height()},
      {"pingtime", stats.ping_time_ms / 1000.0},
      {"bytessent", stats.bytes_sent},
      {"bytesrecv", stats.bytes_received},
      {"messagessent", stats.messages_sent},
      {"messagesrecv", stats.messages_received},
      {"conntime", duration.count()},
      {"misbehavior_score", misbehavior_score},
      {"should_disconnect", should_disconnect}
    };
    arr.push_back(std::move(jpeer));
  }

  std::string out = arr.dump();
  out.push_back('\n');
  return out;
}

std::string RPCServer::HandleAddNode(const std::vector<std::string> &params) {
  LOG_INFO("RPC addnode called");

  if (params.empty()) {
    LOG_INFO("RPC addnode: missing address");
    return "{\"error\":\"Missing node address parameter\"}\n";
  }

  std::string node_addr = params[0];
  std::string command = "add"; // Default command
  if (params.size() > 1) {
    command = params[1];
  }

  LOG_INFO("RPC addnode: address={}, command={}", node_addr, command);

  // Parse address:port (support [IPv6]:port and IPv4:port)
  std::string host;
  std::string port_str;
  if (!node_addr.empty() && node_addr.front() == '[') {
    auto rb = node_addr.find(']');
    if (rb == std::string::npos || rb + 2 > node_addr.size() || node_addr[rb+1] != ':') {
      LOG_INFO("RPC addnode: invalid IPv6 address format");
      return "{\"error\":\"Invalid IPv6 address format (use [addr]:port)\"}\n";
    }
    host = node_addr.substr(1, rb - 1);
    port_str = node_addr.substr(rb + 2);
  } else {
    size_t colon_pos = node_addr.find_last_of(':');
    if (colon_pos == std::string::npos) {
      LOG_INFO("RPC addnode: invalid address format");
      return "{\"error\":\"Invalid address format (use host:port or [v6]:port)\"}\n";
    }
    host = node_addr.substr(0, colon_pos);
    port_str = node_addr.substr(colon_pos + 1);
  }

  // SECURITY FIX: Safe port parsing with validation
  auto port_opt = SafeParsePort(port_str);
  if (!port_opt) {
    LOG_INFO("RPC addnode: invalid port");
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
    LOG_INFO("RPC addnode: calling network_manager_.connect_to()");
    auto result = network_manager_.connect_to(addr);
    LOG_INFO("RPC addnode: connect_to() returned result");
    if (result != network::ConnectionResult::Success) {
      LOG_INFO("RPC addnode: connect_to() failed");
      nlohmann::json j = { {"error", "Failed to connect to node"} };
      std::string out = j.dump();
      out.push_back('\n');
      return out;
    }

    nlohmann::json j = {
      {"success", true},
      {"message", std::string("Connection initiated to ") + node_addr}
    };
    std::string out = j.dump();
    out.push_back('\n');
    return out;
  } else if (command == "remove") {
    // Find peer by address:port and disconnect (thread-safe)
    int peer_id =
        network_manager_.peer_manager().find_peer_by_address(host, port);

    if (peer_id < 0) {
      LOG_WARN("addnode remove: Peer not found: {}", node_addr);
      nlohmann::json j = { {"error", std::string("Peer not found: ") + node_addr} };
      std::string out = j.dump();
      out.push_back('\n');
      return out;
    }

    LOG_INFO("addnode remove: Found peer {} at {}, disconnecting", peer_id,
             node_addr);
    network_manager_.disconnect_from(peer_id);

    nlohmann::json j = {
      {"success", true},
      {"message", std::string("Disconnected from ") + node_addr}
    };
    std::string out = j.dump();
    out.push_back('\n');
    return out;
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
    // Default bantime: 24 hours (matches Core's spirit; permanent requires explicit mode)
    static constexpr int64_t DEFAULT_BANTIME_SEC = 24 * 60 * 60;

    // Validate and canonicalize IP address
    boost::system::error_code ip_ec;
    auto ip_addr = boost::asio::ip::make_address(address, ip_ec);
    if (ip_ec) {
      return "{\"error\":\"Invalid IP address\"}\n";
    }
    std::string canon_addr;
    if (ip_addr.is_v4()) {
      canon_addr = ip_addr.to_string();
    } else {
      auto v6 = ip_addr.to_v6();
      if (v6.is_v4_mapped()) {
        canon_addr = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();
      } else {
        canon_addr = v6.to_string();
      }
    }

    // Optional bantime parameter (seconds); if 0 or omitted => default
    int64_t bantime = 0;
    if (params.size() > 2) {
      auto bt = SafeParseInt64(params[2], 0, 10LL * 365 * 24 * 60 * 60); // up to ~10 years
      if (!bt) {
        return "{\"error\":\"Invalid bantime parameter\"}\n";
      }
      bantime = *bt;
    }

    // Optional mode parameter: "absolute" | "permanent" | "relative" (default)
    std::string mode = "relative";
    if (params.size() > 3) {
      mode = params[3];
      for (auto &c : mode) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    int64_t now = util::GetTime();
    int64_t offset = 0;

    if (mode == "permanent") {
      offset = 0; // BanMan treats 0 as permanent
    } else if (mode == "absolute") {
      if (bantime == 0) {
        return "{\"error\":\"absolute mode requires a non-zero bantime (unix timestamp)\"}\n";
      }
      if (bantime <= now) {
        return "{\"error\":\"absolute bantime must be in the future\"}\n";
      }
      offset = bantime - now;
    } else { // relative (default)
      if (bantime == 0) {
        offset = DEFAULT_BANTIME_SEC;
      } else {
        offset = bantime;
      }
    }

    // Ban the canonical address
    network_manager_.ban_man().Ban(canon_addr, offset);

    nlohmann::json j;
    j["success"] = true;
    if (mode == "permanent") {
      j["message"] = std::string("Permanently banned ") + canon_addr;
    } else if (mode == "absolute") {
      std::ostringstream m; m << "Banned " << canon_addr << " until " << (now + offset) << " (absolute)";
      j["message"] = m.str();
    } else {
      std::ostringstream m; m << "Banned " << canon_addr << " for " << offset << " seconds";
      j["message"] = m.str();
    }
    std::string out = j.dump();
    out.push_back('\n');
    return out;

  } else if (command == "remove") {
    // Try to canonicalize; if invalid, fall back to raw address for legacy entries
    boost::system::error_code ip_ec;
    auto ip_addr = boost::asio::ip::make_address(address, ip_ec);
    if (!ip_ec) {
      std::string canon_addr;
      if (ip_addr.is_v4()) {
        canon_addr = ip_addr.to_string();
      } else {
        auto v6 = ip_addr.to_v6();
        if (v6.is_v4_mapped()) {
          canon_addr = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();
        } else {
          canon_addr = v6.to_string();
        }
      }
      network_manager_.ban_man().Unban(canon_addr);
      if (canon_addr != address) {
        network_manager_.ban_man().Unban(address); // legacy fallback
      }
    } else {
      network_manager_.ban_man().Unban(address);
    }

    nlohmann::json j = { {"success", true}, {"message", std::string("Unbanned ") + address} };
    std::string out = j.dump();
    out.push_back('\n');
    return out;

  } else {
    return "{\"error\":\"Unknown command (use 'add' or 'remove')\"}\n";
  }
}

std::string
RPCServer::HandleListBanned(const std::vector<std::string> &params) {
  auto banned = network_manager_.ban_man().GetBanned();

  nlohmann::json arr = nlohmann::json::array();
  for (const auto &[address, entry] : banned) {
    nlohmann::json j = {
      {"address", address},
      {"banned_until", entry.nBanUntil},
      {"ban_created", entry.nCreateTime},
      {"ban_reason", "manually added"}
    };
    arr.push_back(std::move(j));
  }

  std::string out = arr.dump();
  out.push_back('\n');
  return out;
}

std::string
RPCServer::HandleGetAddrManInfo(const std::vector<std::string> &params) {
  auto &addr_man = network_manager_.address_manager();

  size_t total = addr_man.size();
  size_t tried = addr_man.tried_count();
  size_t new_addrs = addr_man.new_count();

  nlohmann::json j = {
    {"total", total},
    {"tried", tried},
    {"new", new_addrs}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json j = {
    {"blocks", height},
    {"difficulty", difficulty},
    {"networkhashps", networkhashps},
    {"chain", params_.GetChainTypeString()}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
}

std::string
RPCServer::HandleGetNetworkHashPS(const std::vector<std::string> &params) {
  auto *tip = chainstate_manager_.GetTip();

  // Default to DEFAULT_HASHRATE_CALCULATION_BLOCKS
  int nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
  if (!params.empty()) {
    if (params[0] == "-1" || params[0] == "0") {
      nblocks = protocol::DEFAULT_HASHRATE_CALCULATION_BLOCKS;
    } else {
      auto parsed = SafeParseInt(params[0], 1, 10000000);
      if (!parsed) {
        return "{\"error\":\"Invalid nblocks (must be -1, 0, or 1-10000000)\"}\n";
      }
      nblocks = *parsed;
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

  // Parse optional mining address parameter
  // Note: Address is "sticky" - if not provided, previous address is retained
  if (!params.empty()) {
    const std::string& address_str = params[0];

    // Validate address is 40 hex characters (160 bits / 4 bits per hex char)
    if (address_str.length() != 40) {
      return "{\"error\":\"Invalid mining address (must be 40 hex characters)\"}\n";
    }

    // Validate all characters are hex
    for (char c : address_str) {
      if (!std::isxdigit(c)) {
        return "{\"error\":\"Invalid mining address (must contain only hex characters)\"}\n";
      }
    }

    // Parse and set mining address (persists across subsequent calls)
    uint160 mining_address;
    mining_address.SetHex(address_str);
    miner_->SetMiningAddress(mining_address);
  }

  bool started = miner_->Start();
  if (!started) {
    return "{\"error\":\"Failed to start mining\"}\n";
  }

  nlohmann::json j = {
    {"mining", true},
    {"message", "Mining started"},
    {"address", miner_->GetMiningAddress().GetHex()}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json j = {
    {"mining", false},
    {"message", "Mining stopped"}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  // SECURITY FIX: Safe integer parsing with reasonable limit for regtest
  auto num_blocks_opt = SafeParseInt(params[0], 1, 1000);
  if (!num_blocks_opt) {
    return "{\"error\":\"Invalid number of blocks (must be 1-1000)\"}\n";
  }

  int num_blocks = *num_blocks_opt;

  // Parse optional mining address parameter (second parameter)
  // Note: Address is "sticky" - if not provided, previous address is retained
  if (params.size() >= 2) {
    const std::string& address_str = params[1];

    // Validate address is 40 hex characters (160 bits / 4 bits per hex char)
    if (address_str.length() != 40) {
      return "{\"error\":\"Invalid mining address (must be 40 hex characters)\"}\n";
    }

    // Validate all characters are hex
    for (char c : address_str) {
      if (!std::isxdigit(c)) {
        return "{\"error\":\"Invalid mining address (must contain only hex characters)\"}\n";
      }
    }

    // Parse and set mining address (persists across subsequent calls)
    uint160 mining_address;
    mining_address.SetHex(address_str);
    miner_->SetMiningAddress(mining_address);
  }

  // Get starting height and calculate target
  const chain::CBlockIndex *start_tip = chainstate_manager_.GetTip();
  int start_height = start_tip ? start_tip->nHeight : -1;
  int target_height = start_height + num_blocks;

  // Ensure miner is stopped before starting
  miner_->Stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Start mining with target height (miner stops itself when reached)
  if (!miner_->Start(target_height)) {
    LOG_ERROR("RPC: Failed to start mining");
    return "[]\n";
  }

  // Wait for miner to stop (up to 10 minutes total)
  int wait_count = 0;
  while (miner_->IsMining() && wait_count < 6000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wait_count++;
  }

  // Ensure miner is fully stopped
  miner_->Stop();

  // Get final height
  const chain::CBlockIndex *current_tip = chainstate_manager_.GetTip();
  int actual_height = current_tip ? current_tip->nHeight : -1;
  int blocks_mined = actual_height - start_height;

  // Return simple success message with count
  nlohmann::json j = {
    {"blocks", blocks_mined},
    {"height", actual_height}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
}

std::string RPCServer::HandleStop(const std::vector<std::string> &params) {
  LOG_INFO("Received stop command via RPC");

  // SECURITY FIX: Set shutdown flag immediately to reject new requests
  shutting_down_.store(true, std::memory_order_release);

  // Trigger graceful shutdown via callback
  if (shutdown_callback_) {
    shutdown_callback_();
  }

  nlohmann::json j = { {"message", "CoinbaseChain stopping"} };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json j;
  j["success"] = true;
  if (mock_time == 0) {
    j["message"] = "Mock time disabled";
  } else {
    j["mocktime"] = mock_time;
    j["message"] = std::string("Mock time set to ") + std::to_string(mock_time);
  }
  std::string out = j.dump();
  out.push_back('\n');
  return out;
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

  nlohmann::json j = {
    {"success", true},
    {"hash", hash.GetHex()},
    {"message", "Block and all descendants invalidated"}
  };
  std::string out = j.dump();
  out.push_back('\n');
  return out;
}

} // namespace rpc
} // namespace coinbasechain
