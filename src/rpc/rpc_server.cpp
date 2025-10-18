// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "rpc/rpc_server.hpp"
#include "chain/chainparams.hpp"
#include "mining/miner.hpp"
#include "network/network_manager.hpp"
#include "sync/peer_manager.hpp"
#include "util/logging.hpp"
#include "util/time.hpp"
#include "validation/chainstate_manager.hpp"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/socket.h>
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
      shutdown_callback_(shutdown_callback), server_fd_(-1), running_(false) {
  RegisterHandlers();
}

RPCServer::~RPCServer() { Stop(); }

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
  handlers_["getpeerinfo"] = [this](const auto &p) {
    return HandleGetPeerInfo(p);
  };
  handlers_["addnode"] = [this](const auto &p) { return HandleAddNode(p); };

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

  // Create Unix domain socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
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
    return false;
  }

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
  char buffer[4096];
  ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

  if (received <= 0) {
    return;
  }

  buffer[received] = '\0';
  std::string request(buffer, received);

  // Simple JSON parsing (method and params)
  std::string method;
  std::vector<std::string> params;

  // Extract method
  size_t method_pos = request.find("\"method\":\"");
  if (method_pos != std::string::npos) {
    method_pos += 10;
    size_t method_end = request.find("\"", method_pos);
    if (method_end != std::string::npos) {
      method = request.substr(method_pos, method_end - method_pos);
    }
  }

  // Extract params (simple array parsing)
  size_t params_pos = request.find("\"params\":[");
  if (params_pos != std::string::npos) {
    params_pos += 10;
    size_t params_end = request.find("]", params_pos);
    if (params_end != std::string::npos) {
      std::string params_str =
          request.substr(params_pos, params_end - params_pos);
      // Parse comma-separated quoted strings
      size_t pos = 0;
      while (pos < params_str.size()) {
        size_t quote1 = params_str.find("\"", pos);
        if (quote1 == std::string::npos)
          break;
        size_t quote2 = params_str.find("\"", quote1 + 1);
        if (quote2 == std::string::npos)
          break;
        params.push_back(params_str.substr(quote1 + 1, quote2 - quote1 - 1));
        pos = quote2 + 1;
      }
    }
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
    std::ostringstream oss;
    oss << "{\"error\":\"" << e.what() << "\"}\n";
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

  int height = std::stoi(params[0]);
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

  uint256 hash;
  hash.SetHex(params[0]);

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

    // Get misbehavior score from sync::PeerManager
    int misbehavior_score = 0;
    bool should_disconnect = false;
    try {
      auto &sync_peer_mgr = network_manager_.header_sync().GetPeerManager();
      misbehavior_score = sync_peer_mgr.GetMisbehaviorScore(peer->id());
      should_disconnect = sync_peer_mgr.ShouldDisconnect(peer->id());
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
  uint16_t port = static_cast<uint16_t>(std::stoi(port_str));

  if (command == "add") {
    // Connect to the node
    bool success = network_manager_.connect_to(host, port);
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

  // Calculate network hashrate (simplified - based on last 120 blocks)
  double networkhashps = 0.0;
  if (tip && tip->nHeight > 0) {
    int nblocks = std::min(120, tip->nHeight);
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

  // Default to 120 blocks
  int nblocks = 120;
  if (!params.empty()) {
    nblocks = std::stoi(params[0]);
    if (nblocks == -1 || nblocks == 0) {
      nblocks = 120;
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

  // Optional: number of threads
  int threads = 0; // 0 = auto-detect
  if (!params.empty()) {
    threads = std::stoi(params[0]);
  }

  bool started = miner_->Start(threads);
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

  if (params.empty()) {
    return "{\"error\":\"Missing number of blocks parameter\"}\n";
  }

  int num_blocks = std::stoi(params[0]);
  if (num_blocks <= 0 || num_blocks > 1000) {
    return "{\"error\":\"Invalid number of blocks (must be 1-1000)\"}\n";
  }

  // Get starting height
  auto *start_tip = chainstate_manager_.GetTip();
  int start_height = start_tip ? start_tip->nHeight : -1;

  std::vector<std::string> block_hashes;

  // Mine blocks one at a time
  for (int i = 0; i < num_blocks; i++) {
    // Start mining with 1 thread (synchronous for regtest)
    // Multi-threading causes race conditions with easy difficulty
    if (!miner_->Start(1)) {
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

  int64_t mock_time = std::stoll(params[0]);

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

  uint256 hash;
  hash.SetHex(params[0]);

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
