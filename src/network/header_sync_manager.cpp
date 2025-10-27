#include "network/header_sync_manager.hpp"
#include "network/peer_manager.hpp"
#include "network/peer.hpp"
#include "network/banman.hpp"
#include "network/protocol.hpp"
#include "chain/chainstate_manager.hpp"
#include "chain/validation.hpp"
#include "chain/block.hpp"
#include "chain/logging.hpp"
#include "chain/time.hpp"
#include <cstring>
#include <chrono>

namespace coinbasechain {
namespace network {

HeaderSyncManager::HeaderSyncManager(validation::ChainstateManager& chainstate,
                                     PeerManager& peer_mgr,
                                     BanMan* ban_man)
    : chainstate_manager_(chainstate),
      peer_manager_(peer_mgr),
      ban_man_(ban_man) {}

void HeaderSyncManager::SetSyncPeer(uint64_t peer_id) {
  auto now = std::chrono::steady_clock::now();
  int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       now.time_since_epoch())
                       .count();

  sync_peer_id_.store(peer_id, std::memory_order_release);
  sync_start_time_.store(now_us, std::memory_order_release);
  last_headers_received_.store(now_us, std::memory_order_release);
  initial_sync_started_.store(true, std::memory_order_release);
}

void HeaderSyncManager::ClearSyncPeer() {
  sync_peer_id_.store(0, std::memory_order_release);
}

void HeaderSyncManager::OnPeerDisconnected(uint64_t peer_id) {
  // Bitcoin Core cleanup (FinalizeNode): if (state->fSyncStarted) nSyncStarted--;
  // If this was our sync peer, reset sync state to allow retry with another peer
  uint64_t current_sync_peer = sync_peer_id_.load(std::memory_order_acquire);
  
  if (current_sync_peer == peer_id) {
    LOG_NET_DEBUG("Sync peer {} disconnected, clearing sync state", peer_id);
    
    // Reset all sync state
    sync_peer_id_.store(0, std::memory_order_release);
    initial_sync_started_.store(false, std::memory_order_release);
    sync_start_time_.store(0, std::memory_order_release);
    last_headers_received_.store(0, std::memory_order_release);
  }
}

void HeaderSyncManager::CheckInitialSync() {
  // If we've already started initial sync once, do nothing. Prevents fan-out to multiple peers.
  if (initial_sync_started_.load(std::memory_order_acquire)) {
    return;
  }

  // Matches Bitcoin's initial headers sync logic in SendMessages
  // Only actively request headers from a single peer (like Bitcoin's
  // nSyncStarted check)

  // If we already have a sync peer, nothing to do (atomic load)
  if (sync_peer_id_.load(std::memory_order_acquire) != 0) {
    return;
  }

  // Protect peer selection with mutex to prevent race conditions
  // Multiple CheckInitialSync() calls could happen concurrently from timers
  std::lock_guard<std::mutex> lock(sync_mutex_);

  // Double-check after acquiring lock (another thread may have set sync peer)
  if (sync_peer_id_.load(std::memory_order_acquire) != 0) {
    return;
  }

  // Try outbound peers first (preferred for initial sync)
  auto outbound_peers = peer_manager_.get_outbound_peers();

  for (const auto &peer : outbound_peers) {
    if (!peer || !peer->successfully_connected()) {
      continue; // Skip peers that haven't completed handshake
    }

    // Bitcoin Core: Check !state.fSyncStarted to avoid re-requesting from same peer
    if (peer->sync_started()) {
      continue; // Already started sync with this peer
    }

    // We found a ready outbound peer! Start initial sync with this peer
    int current_height = chainstate_manager_.GetChainHeight();
LOG_NET_DEBUG("initial getheaders ({}) peer={}",
                 current_height, peer->id());

    SetSyncPeer(peer->id());
    peer->set_sync_started(true);  // Bitcoin Core: state.fSyncStarted = true
    initial_sync_started_.store(true, std::memory_order_release);

    // Send GETHEADERS to initiate sync (like Bitcoin's "initial getheaders")
    RequestHeadersFromPeer(peer);

    return; // Only sync from one peer at a time
  }

  // Fallback: If no outbound peers available, try inbound peers
  // (Bitcoin allows inbound peers if m_num_preferred_download_peers == 0)
  auto inbound_peers = peer_manager_.get_inbound_peers();

  for (const auto &peer : inbound_peers) {
    if (!peer || !peer->successfully_connected()) {
      continue; // Skip peers that haven't completed handshake
    }

    // Bitcoin Core: Check !state.fSyncStarted to avoid re-requesting from same peer
    if (peer->sync_started()) {
      continue; // Already started sync with this peer
    }

    // We found a ready inbound peer! Use it as fallback for initial sync
    int current_height = chainstate_manager_.GetChainHeight();
LOG_NET_DEBUG("initial getheaders ({}) peer={}",
                 current_height, peer->id());

    SetSyncPeer(peer->id());
    peer->set_sync_started(true);  // Bitcoin Core: state.fSyncStarted = true
    initial_sync_started_.store(true, std::memory_order_release);

    // Send GETHEADERS to initiate sync
    RequestHeadersFromPeer(peer);

    return; // Only sync from one peer at a time
  }
}

void HeaderSyncManager::RequestHeadersFromPeer(PeerPtr peer) {
  if (!peer) {
    return;
  }

  // Get block locator from chainstate
  // For initial sync, use pprev trick
  // This ensures we get non-empty response even if peer is at same tip
  CBlockLocator locator = GetLocatorFromPrev();

  // Create GETHEADERS message
  auto msg = std::make_unique<message::GetHeadersMessage>();
  msg->version = protocol::PROTOCOL_VERSION;

  // Convert locator hashes from uint256 to std::array<uint8_t, 32>
  for (const auto &hash : locator.vHave) {
    std::array<uint8_t, 32> hash_array;
    std::memcpy(hash_array.data(), hash.data(), 32);
    msg->block_locator_hashes.push_back(hash_array);
  }

  // hash_stop is all zeros (get as many as possible)
  msg->hash_stop.fill(0);

  LOG_NET_TRACE("requesting headers from peer={} (locator size: {})",
                peer->id(), msg->block_locator_hashes.size());

  peer->send_message(std::move(msg));
}

bool HeaderSyncManager::HandleHeadersMessage(PeerPtr peer,
                                              message::HeadersMessage *msg) {
  if (!peer || !msg) {
    return false;
  }

  const auto &headers = msg->headers;
  int peer_id = peer->id();

  // Bitcoin Core approach: If the last header in this batch is already in our chain
  // and is an ancestor of our best header or tip, skip all DoS checks for this entire batch.
  // This prevents false positives when reconnecting to peers after manual InvalidateBlock.
  //
  // Simplified implementation: If header exists and has been validated (nChainWork > 0),
  // skip DoS checks. This achieves the same goal without needing GetAncestor().
  bool skip_dos_checks = false;
  if (!headers.empty()) {
    const chain::CBlockIndex* last_header_index =
        chainstate_manager_.LookupBlockIndex(headers.back().GetHash());
    if (last_header_index && last_header_index->nChainWork > 0) {
      skip_dos_checks = true;
      LOG_NET_TRACE("Peer {} sent {} headers, last header already validated (log2_work={:.6f}), skipping DoS checks",
                    peer_id, headers.size(), std::log(last_header_index->nChainWork.getdouble()) / std::log(2.0));
    }
  }

  LOG_NET_TRACE("Processing {} headers from peer {}, skip_dos_checks={}",
                headers.size(), peer_id, skip_dos_checks);

  // Update last headers received timestamp (atomic store)
  auto now_tp = std::chrono::steady_clock::now();
  int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       now_tp.time_since_epoch())
                       .count();
  last_headers_received_.store(now_us, std::memory_order_release);

  // Process headers (logic moved from HeaderSync::ProcessHeaders)
  if (headers.empty()) {
    // Bitcoin Core (line 2896): "Nothing interesting. Stop asking this peers for more headers."
    // Empty headers means peer has reached the end of their chain - they have no more to give us.
    // This could happen if peer reorged to our chain, or we've synced to their tip.
LOG_NET_DEBUG("received headers (0) peer={}", peer_id);
    ClearSyncPeer();
    return true;
  }

  // DoS Protection: Check headers message size limit
  if (headers.size() > protocol::MAX_HEADERS_SIZE) {
    LOG_NET_ERROR("Rejecting oversized headers message from peer {} (size={}, max={})",
              peer_id, headers.size(), protocol::MAX_HEADERS_SIZE);
    peer_manager_.ReportOversizedMessage(peer_id);
    // Check if peer should be disconnected
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      if (ban_man_) {
        ban_man_->Discourage(peer->address());
      }
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

LOG_NET_DEBUG("received headers ({}) peer={}", headers.size(), peer_id);

  // DoS Protection: Check if first header connects to known chain
  const uint256 &first_prev = headers[0].hashPrevBlock;
  bool prev_exists = chainstate_manager_.LookupBlockIndex(first_prev) != nullptr;

  if (!prev_exists) {
    LOG_NET_WARN("headers don't connect to known chain from peer={} (first prevhash: {})",
             peer_id, first_prev.ToString());
    peer_manager_.IncrementUnconnectingHeaders(peer_id);
    // Check if peer should be disconnected
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      if (ban_man_) {
        ban_man_->Discourage(peer->address());
      }
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  // Headers connect - reset unconnecting counter
  peer_manager_.ResetUnconnectingHeaders(peer_id);

  // DoS Protection: Cheap PoW commitment check
  bool pow_ok = chainstate_manager_.CheckHeadersPoW(headers);
  if (!pow_ok) {
    LOG_NET_ERROR("headers failed PoW commitment check from peer={}", peer_id);
    peer_manager_.ReportInvalidPoW(peer_id);
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      if (ban_man_) {
        ban_man_->Discourage(peer->address());
      }
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  // DoS Protection: Check headers are continuous
  bool continuous_ok = validation::CheckHeadersAreContinuous(headers);
  if (!continuous_ok) {
    LOG_NET_ERROR("non-continuous headers from peer={}", peer_id);
    peer_manager_.ReportNonContinuousHeaders(peer_id);
    if (peer_manager_.ShouldDisconnect(peer_id)) {
      if (ban_man_) {
        ban_man_->Discourage(peer->address());
      }
      peer_manager_.remove_peer(peer_id);
    }
    ClearSyncPeer();
    return false;
  }

  // DoS Protection: Check for low-work headers (Bitcoin Core approach)
  // Bitcoin Core: net_processing.cpp lines 2993-2999
  // Only run anti-DoS check if we haven't already validated this work
  if (!skip_dos_checks) {
    const chain::CBlockIndex* chain_start = chainstate_manager_.LookupBlockIndex(headers[0].hashPrevBlock);
    if (chain_start) {
      arith_uint256 total_work = chain_start->nChainWork + validation::CalculateHeadersWork(headers);

      // Get our dynamic anti-DoS threshold
      arith_uint256 minimum_work = validation::GetAntiDoSWorkThreshold(
          chainstate_manager_.GetTip(),
          chainstate_manager_.GetParams(),
          chainstate_manager_.IsInitialBlockDownload());

      // Bitcoin Core logic (TryLowWorkHeadersSync): Avoid DoS via low-difficulty-headers
      // by only processing if the headers are part of a chain with sufficient work.
      if (total_work < minimum_work) {
        // Only give up on this peer if their headers message was NOT full;
        // otherwise they have more headers after this, and their full chain
        // might have sufficient work even if this batch doesn't.
        if (headers.size() != protocol::MAX_HEADERS_SIZE) {
          // Batch was not full - peer has no more headers to offer
          LOG_NET_TRACE("ignoring low-work chain from peer={} (work={}, threshold={}, height={})",
                        peer_id,
                        total_work.ToString(),
                        minimum_work.ToString(),
                        chain_start->nHeight + headers.size());
          // Bitcoin Core: Does NOT clear sync peer here, just ignores the headers
          // We match this by not calling ClearSyncPeer()
          return true;  // Handled (by ignoring)
        }
        // Batch was full - peer likely has more headers with additional work coming.
        // Don't abandon peer, just skip processing this batch and request more.
        LOG_NET_TRACE("low-work headers from peer={} but batch is full (size={}, work={}, threshold={}), continuing sync",
                      peer_id,
                      headers.size(),
                      total_work.ToString(),
                      minimum_work.ToString());
        // Bitcoin Core: Enters special HeadersSyncState here to track low-work sync
        // We simplify by just requesting more headers from the same peer
        RequestHeadersFromPeer(peer);
        return true;  // Handled (by requesting more)
      }
    }
  } else {
    LOG_NET_TRACE("Skipping low-work check for peer {} (headers already validated)", peer_id);
  }

  // Store batch size under lock
  {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    last_batch_size_ = headers.size();
  }

  // Accept all headers into block index
  for (const auto &header : headers) {
    validation::ValidationState state;
    chain::CBlockIndex *pindex =
        chainstate_manager_.AcceptBlockHeader(header, state, peer_id);

    if (!pindex) {
      const std::string &reason = state.GetRejectReason();

      // Orphaned header - not an error
      if (reason == "orphaned") {
        LOG_NET_TRACE("header from peer={} cached as orphan: {}",
                 peer_id, header.GetHash().ToString().substr(0, 16));
        continue;
      }

      // DoS Protection: Orphan limit exceeded
      if (reason == "orphan-limit") {
        LOG_NET_TRACE("peer={} exceeded orphan limit", peer_id);
        peer_manager_.ReportTooManyOrphans(peer_id);
        if (peer_manager_.ShouldDisconnect(peer_id)) {
          if (ban_man_) {
            ban_man_->Discourage(peer->address());
          }
          peer_manager_.remove_peer(peer_id);
        }
        ClearSyncPeer();
        return false;
      }

      // Duplicate header - Bitcoin Core approach: only penalize if it's a duplicate of an INVALID header
      if (reason == "duplicate") {
        const chain::CBlockIndex* existing = chainstate_manager_.LookupBlockIndex(header.GetHash());
        LOG_NET_TRACE("Duplicate header from peer {}: {} (existing={}, valid={}, skip_dos_checks={})",
                      peer_id, header.GetHash().ToString().substr(0, 16),
                      existing ? "yes" : "no", existing ? existing->IsValid() : false, skip_dos_checks);

        // Bitcoin Core approach: If we're skipping DoS checks (last header was on active chain),
        // don't penalize for any duplicates. This handles manual InvalidateBlock correctly.
        if (skip_dos_checks) {
          LOG_NET_TRACE("Skipping DoS check for duplicate header (batch contains ancestors)");
          continue;  // Skip this header, process next one (no penalty)
        }

        if (existing && existing->IsValid()) {
          // Header already accepted and valid - not an attack, just redundant
          LOG_NET_TRACE("Duplicate header is valid, skipping (no penalty)");
          continue;  // Skip this header, process next one (no penalty)
        }

        // If we get here: duplicate invalid header AND not an ancestor
        // This means peer is sending blocks that failed consensus validation
        LOG_NET_WARN("Peer {} sent duplicate INVALID header (not an ancestor): {}",
                     peer_id, header.GetHash().ToString().substr(0, 16));
        peer_manager_.ReportInvalidHeader(peer_id, reason);
        if (peer_manager_.ShouldDisconnect(peer_id)) {
          if (ban_man_) {
            ban_man_->Discourage(peer->address());
          }
          peer_manager_.remove_peer(peer_id);
        }
        ClearSyncPeer();
        return false;
      }

      // Invalid header - instant disconnect
      if (reason == "high-hash" || reason == "bad-diffbits" ||
          reason == "time-too-old" || reason == "time-too-new" ||
          reason == "bad-version" ||
          reason == "bad-prevblk" || reason == "bad-genesis" ||
          reason == "genesis-via-accept") {
        LOG_NET_ERROR("peer={} sent invalid header: {}", peer_id, reason);
        peer_manager_.ReportInvalidHeader(peer_id, reason);
        if (peer_manager_.ShouldDisconnect(peer_id)) {
          if (ban_man_) {
            ban_man_->Discourage(peer->address());
          }
          peer_manager_.remove_peer(peer_id);
        }
        ClearSyncPeer();
        return false;
      }

      // Unknown rejection reason - log and fail
      LOG_NET_ERROR("failed to accept header from peer={} - hash: {} reason: {} debug: {}",
                peer_id, header.GetHash().ToString(), reason, state.GetDebugMessage());
      ClearSyncPeer();
      return false;
    }

    // Add to candidate set for batch activation
    chainstate_manager_.TryAddBlockIndexCandidate(pindex);
  }

  // Activate best chain ONCE for the entire batch
  LOG_NET_TRACE("calling ActivateBestChain for batch of {} headers", headers.size());
  bool activate_result = chainstate_manager_.ActivateBestChain(nullptr);
  LOG_NET_TRACE("ActivateBestChain returned {}", activate_result ? "true" : "FALSE");
  if (!activate_result) {
    ClearSyncPeer();
    return false;
  }

  // Show progress during IBD or new block notification
  if (chainstate_manager_.IsInitialBlockDownload()) {
    const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
    if (tip) {
      LOG_NET_TRACE("synchronizing block headers, height: {}", tip->nHeight);
    }
  } else {
    const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
    if (tip) {
      LOG_NET_TRACE("new block header: height={} hash={}...", tip->nHeight,
               tip->GetBlockHash().ToString().substr(0, 16));
    }
  }

  // Check if we should request more headers
  // Bitcoin Core: Never clears fSyncStarted after receiving headers successfully
  // Only timeout clears it. This prevents trying all peers sequentially.
  if (ShouldRequestMore()) {
    RequestHeadersFromPeer(peer);
  } else {
    // No more headers to request from this peer - clear sync peer
    // so we can try another peer if needed
    ClearSyncPeer();
  }

  return true;
}

bool HeaderSyncManager::HandleGetHeadersMessage(
    PeerPtr peer, message::GetHeadersMessage *msg) {
  if (!peer || !msg) {
    return false;
  }

  LOG_NET_TRACE("peer={} requested headers (locator size: {})", peer->id(),
                msg->block_locator_hashes.size());

  // Find the fork point using the block locator
  // IMPORTANT: Only consider blocks on the ACTIVE chain, not side chains
  // Otherwise we might find a fork point on a side chain we know about
  const chain::CBlockIndex *fork_point = nullptr;
  for (const auto &hash_array : msg->block_locator_hashes) {
    // Convert std::array<uint8_t, 32> to uint256
    uint256 hash;
    std::memcpy(hash.data(), hash_array.data(), 32);

    const chain::CBlockIndex *pindex =
        chainstate_manager_.LookupBlockIndex(hash);
    if (chainstate_manager_.IsOnActiveChain(pindex)) {
      // Found a block that exists AND is on our active chain
      fork_point = pindex;
      LOG_NET_TRACE("found fork point at height {} (hash={}) on active chain",
                   fork_point->nHeight, hash.ToString().substr(0, 16));
      break;
    }
  }

  if (!fork_point) {
    // No common blocks - start from genesis
    fork_point = chainstate_manager_.GetTip();
    while (fork_point && fork_point->pprev) {
      fork_point = fork_point->pprev;
    }
    if (fork_point) {
      LOG_NET_TRACE("no common blocks in locator - using genesis at height {}",
                   fork_point->nHeight);
    }
  }

  if (!fork_point) {
    LOG_NET_TRACE("no blocks to send to peer={}", peer->id());
    return false;
  }

  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  LOG_NET_TRACE("preparing headers: fork_point height={} tip height={}",
                fork_point->nHeight, tip ? tip->nHeight : -1);

  // Build HEADERS response
  auto response = std::make_unique<message::HeadersMessage>();

  // Start from the block after fork point and collect headers
  const chain::CBlockIndex *pindex =
      chainstate_manager_.GetBlockAtHeight(fork_point->nHeight + 1);

  // Respect hash_stop (0 = no limit)
  uint256 stop_hash;
  bool has_stop = false;
  {
    // Convert std::array<uint8_t, 32> to uint256
    std::memcpy(stop_hash.data(), msg->hash_stop.data(), 32);
    has_stop = !stop_hash.IsNull();
  }

  while (pindex && response->headers.size() < protocol::MAX_HEADERS_SIZE) {
    CBlockHeader hdr = pindex->GetBlockHeader();
    response->headers.push_back(hdr);

    // If caller requested a stop-hash, include it and then stop
    if (has_stop && pindex->GetBlockHash() == stop_hash) {
      break;
    }

    if (pindex == tip) {
      break;
    }

    // Get next block in active chain
    pindex = chainstate_manager_.GetBlockAtHeight(pindex->nHeight + 1);
  }

LOG_NET_DEBUG("sending headers ({}) peer={}",
                response->headers.size(), peer->id());

  peer->send_message(std::move(response));
  return true;
}

bool HeaderSyncManager::IsSynced(int64_t max_age_seconds) const {
  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip) {
    return false;
  }

  // Check if tip is recent (use util::GetTime() to support mock time in tests)
  int64_t now = util::GetTime();
  int64_t tip_age = now - tip->nTime;

  return tip_age < max_age_seconds;
}

bool HeaderSyncManager::ShouldRequestMore() const {
  // Bitcoin Core logic (net_processing.cpp line 3019):
  // if (nCount == MAX_HEADERS_RESULTS && !have_headers_sync)
  //
  // Request more if batch was full (peer may have more headers).
  // We don't have Bitcoin's HeadersSyncState mechanism, so we always
  // behave like have_headers_sync=false.
  //
  // IMPORTANT: Do NOT check IsSynced() here! In regtest, blocks are mined
  // instantly so tip is always "recent", which would cause us to abandon
  // sync peers prematurely. Bitcoin Core doesn't check sync state here either.
  std::lock_guard<std::mutex> lock(sync_mutex_);
  return last_batch_size_ == protocol::MAX_HEADERS_SIZE;
}

CBlockLocator HeaderSyncManager::GetLocatorFromPrev() const {
  // Matches Bitcoin's initial sync logic
  // Start from pprev of tip to ensure non-empty response
  const chain::CBlockIndex *tip = chainstate_manager_.GetTip();
  if (!tip) {
    // At genesis, just use tip
    return chainstate_manager_.GetLocator();
  }

  if (tip->pprev) {
    // Use pprev - ensures peer sends back at least 1 header (our current tip)
    return chainstate_manager_.GetLocator(tip->pprev);
  } else {
    // Tip is genesis (no pprev), use tip itself
    return chainstate_manager_.GetLocator();
  }
}

} // namespace network
} // namespace coinbasechain
