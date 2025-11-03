#pragma once

/*
 SyncManager — unified blockchain synchronization coordinator for CoinbaseChain

 Purpose
 - Own and coordinate HeaderSyncManager and BlockRelayManager
 - Provide clean interface for sync-related protocol messages
 - Route sync messages to the appropriate manager

 Key responsibilities
 1. Own HeaderSyncManager and BlockRelayManager
 2. Handle sync-related protocol messages (HEADERS, GETHEADERS, INV)
 3. Provide accessor methods for owned managers

 Message Handling
 - HEADERS: Delegate to HeaderSyncManager
 - GETHEADERS: Delegate to HeaderSyncManager
 - INV: Delegate to BlockRelayManager

 Architecture
 This is a top-level manager that owns the sync subsystem components.
 It provides ownership and delegation, allowing NetworkManager to interact
 with sync logic through a single interface.

 Design Pattern
 - Ownership: Uses unique_ptr to own child managers
 - Delegation: Routes protocol messages to appropriate internal manager
 - Accessor pattern: Provides direct access to owned managers when needed

 Note: IBD (Initial Block Download) state is managed by ChainstateManager
 in the chain layer, not by network-layer managers.
*/

#include <memory>
#include "network/peer.hpp"

// Forward declarations
namespace coinbasechain {

namespace message {
class HeadersMessage;
class GetHeadersMessage;
class InvMessage;
}

namespace validation {
class ChainstateManager;
}

namespace network {

class HeaderSyncManager;
class BlockRelayManager;
class PeerLifecycleManager;

class BlockchainSyncManager {
public:
  // Constructor: Creates owned sync managers internally
  // BlockchainSyncManager creates and owns HeaderSyncManager and BlockRelayManager
  BlockchainSyncManager(validation::ChainstateManager& chainstate,
                        PeerLifecycleManager& peer_manager);

  ~BlockchainSyncManager();

  // Prevent copying (owns unique resources)
  BlockchainSyncManager(const BlockchainSyncManager&) = delete;
  BlockchainSyncManager& operator=(const BlockchainSyncManager&) = delete;

  // === Protocol Message Handlers ===
  // These delegate to the appropriate internal manager

  /**
   * Handle HEADERS message - processes block headers from peer
   * Delegates to HeaderSyncManager
   *
   * @param peer Peer that sent the headers
   * @param msg Headers message payload
   * @return true if handled successfully
   */
  bool HandleHeaders(PeerPtr peer, message::HeadersMessage* msg);

  /**
   * Handle GETHEADERS message - peer requesting headers from us
   * Delegates to HeaderSyncManager
   *
   * @param peer Peer requesting headers
   * @param msg GetHeaders message with locator and stop hash
   * @return true if handled successfully
   */
  bool HandleGetHeaders(PeerPtr peer, message::GetHeadersMessage* msg);

  /**
   * Handle INV message - inventory announcement (blocks/txs)
   * Delegates to BlockRelayManager
   *
   * @param peer Peer announcing inventory
   * @param msg Inventory message payload
   * @return true if handled successfully
   */
  bool HandleInv(PeerPtr peer, message::InvMessage* msg);

  // === Component Accessors ===
  // Provide access to owned managers for NetworkManager to call their methods
  // These replace direct member access after ownership transfer

  HeaderSyncManager& header_sync() { return *header_sync_manager_; }
  const HeaderSyncManager& header_sync() const { return *header_sync_manager_; }

  BlockRelayManager& block_relay() { return *block_relay_manager_; }
  const BlockRelayManager& block_relay() const { return *block_relay_manager_; }

private:
  // Phase 2: Owned managers via unique_ptr
  // BlockchainSyncManager now owns HeaderSyncManager and BlockRelayManager
  std::unique_ptr<HeaderSyncManager> header_sync_manager_;
  std::unique_ptr<BlockRelayManager> block_relay_manager_;
};

} // namespace network
} // namespace coinbasechain
