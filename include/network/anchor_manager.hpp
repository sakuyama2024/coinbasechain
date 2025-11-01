#pragma once

#include "network/protocol.hpp"
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace coinbasechain {
namespace network {

// Forward declarations
class PeerManager;

/**
 * AnchorManager - Manages anchor peer persistence for eclipse attack resistance
 *
 * Responsibilities:
 * - Select high-quality anchor peers from current connections
 * - Save anchor peers to disk for restart recovery
 * - Load and reconnect to anchor peers on startup
 *
 * Bitcoin Core uses anchors to mitigate eclipse attacks by remembering
 * a few high-quality peers from previous sessions. On restart, we reconnect
 * to these anchors before accepting other connections, making it harder for
 * an attacker to isolate the node.
 *
 * Extracted from NetworkManager to improve modularity and maintainability.
 */
class AnchorManager {
public:
  // Callback type for converting NetworkAddress to IP string
  using AddressToStringCallback = std::function<std::optional<std::string>(const protocol::NetworkAddress&)>;

  // Callback type for initiating connections (second parameter: noban flag)
  using ConnectCallback = std::function<void(const protocol::NetworkAddress&, bool noban)>;

  AnchorManager(PeerManager& peer_mgr,
                AddressToStringCallback addr_to_str_cb,
                ConnectCallback connect_cb);

  // Get current anchor peers from connected outbound peers
  std::vector<protocol::NetworkAddress> GetAnchors() const;

  // Save current anchors to file
  bool SaveAnchors(const std::string& filepath);

  // Load anchors from file and reconnect to them
  bool LoadAnchors(const std::string& filepath);

private:
  PeerManager& peer_manager_;
  AddressToStringCallback addr_to_string_callback_;
  ConnectCallback connect_callback_;
};

} // namespace network
} // namespace coinbasechain


