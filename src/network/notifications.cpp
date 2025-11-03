// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "network/notifications.hpp"
#include <algorithm>

namespace coinbasechain {

// ============================================================================
// NetworkNotifications::Subscription
// ============================================================================

NetworkNotifications::Subscription::Subscription(NetworkNotifications *owner,
                                                 size_t id)
    : owner_(owner), id_(id), active_(true) {}

NetworkNotifications::Subscription::~Subscription() { Unsubscribe(); }

NetworkNotifications::Subscription::Subscription(
    Subscription &&other) noexcept
    : owner_(other.owner_), id_(other.id_), active_(other.active_) {
  other.owner_ = nullptr;
  other.active_ = false;
}

NetworkNotifications::Subscription &
NetworkNotifications::Subscription::operator=(Subscription &&other) noexcept {
  if (this != &other) {
    Unsubscribe();
    owner_ = other.owner_;
    id_ = other.id_;
    active_ = other.active_;
    other.owner_ = nullptr;
    other.active_ = false;
  }
  return *this;
}

void NetworkNotifications::Subscription::Unsubscribe() {
  if (active_ && owner_) {
    owner_->Unsubscribe(id_);
    active_ = false;
  }
}

// ============================================================================
// NetworkNotifications
// ============================================================================

NetworkNotifications::Subscription
NetworkNotifications::SubscribePeerConnected(PeerConnectedCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.peer_connected = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

NetworkNotifications::Subscription
NetworkNotifications::SubscribePeerDisconnected(
    PeerDisconnectedCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.peer_disconnected = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

NetworkNotifications::Subscription
NetworkNotifications::SubscribeInvalidHeader(InvalidHeaderCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.invalid_header = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

NetworkNotifications::Subscription
NetworkNotifications::SubscribeLowWorkHeaders(
    LowWorkHeadersCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.low_work_headers = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

NetworkNotifications::Subscription
NetworkNotifications::SubscribeInvalidBlock(InvalidBlockCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.invalid_block = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

NetworkNotifications::Subscription
NetworkNotifications::SubscribeMisbehavior(MisbehaviorCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t id = next_id_++;

  CallbackEntry entry;
  entry.id = id;
  entry.misbehavior = std::move(callback);
  callbacks_.push_back(std::move(entry));

  return Subscription(this, id);
}

void NetworkNotifications::NotifyPeerConnected(
    int peer_id, const std::string &address, uint16_t port,
    const std::string &connection_type) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.peer_connected) {
      entry.peer_connected(peer_id, address, port, connection_type);
    }
  }
}

void NetworkNotifications::NotifyPeerDisconnected(int peer_id,
                                                  const std::string &address, uint16_t port,
                                                  const std::string &reason, bool mark_addr_good) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.peer_disconnected) {
      entry.peer_disconnected(peer_id, address, port, reason, mark_addr_good);
    }
  }
}

void NetworkNotifications::NotifyInvalidHeader(int peer_id, const uint256 &hash,
                                               const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.invalid_header) {
      entry.invalid_header(peer_id, hash, reason);
    }
  }
}

void NetworkNotifications::NotifyLowWorkHeaders(int peer_id, size_t count,
                                                const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.low_work_headers) {
      entry.low_work_headers(peer_id, count, reason);
    }
  }
}

void NetworkNotifications::NotifyInvalidBlock(int peer_id, const uint256 &hash,
                                              const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.invalid_block) {
      entry.invalid_block(peer_id, hash, reason);
    }
  }
}

void NetworkNotifications::NotifyMisbehavior(int peer_id, int penalty,
                                             const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &entry : callbacks_) {
    if (entry.misbehavior) {
      entry.misbehavior(peer_id, penalty, reason);
    }
  }
}

void NetworkNotifications::Unsubscribe(size_t id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it =
      std::find_if(callbacks_.begin(), callbacks_.end(),
                   [id](const CallbackEntry &entry) { return entry.id == id; });

  if (it != callbacks_.end()) {
    callbacks_.erase(it);
  }
}

NetworkNotifications &NetworkNotifications::Get() {
  static NetworkNotifications instance;
  return instance;
}

} // namespace coinbasechain
