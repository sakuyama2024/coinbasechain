#include "network/message_dispatcher.hpp"
#include "network/peer.hpp"
#include "network/message.hpp"
#include "util/logging.hpp"
#include <algorithm>

namespace coinbasechain {
namespace network {

void MessageDispatcher::RegisterHandler(const std::string& command,
                                         MessageHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[command] = std::move(handler);
  LOG_NET_DEBUG("Registered handler for command: {}", command);
}

void MessageDispatcher::UnregisterHandler(const std::string& command) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.erase(command);
  LOG_NET_DEBUG("Unregistered handler for command: {}", command);
}

bool MessageDispatcher::Dispatch(PeerPtr peer,
                                  const std::string& command,
                                  ::coinbasechain::message::Message* msg) {
  if (!msg) {
    LOG_NET_WARN("MessageDispatcher::Dispatch called with null message");
    return false;
  }

  // Get handler (lock scope minimized)
  MessageHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handlers_.find(command);
    if (it == handlers_.end()) {
      LOG_NET_DEBUG("No handler for command: {}", command);
      return false;
    }
    handler = it->second;
  }

  // Execute handler (outside lock - handlers may take time)
  try {
    return handler(peer, msg);
  } catch (const std::exception& e) {
    LOG_NET_ERROR("Handler exception for command {}: {}", command, e.what());
    return false;
  }
}

bool MessageDispatcher::HasHandler(const std::string& command) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handlers_.count(command) > 0;
}

std::vector<std::string> MessageDispatcher::GetRegisteredCommands() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(handlers_.size());
  for (const auto& [cmd, _] : handlers_) {
    result.push_back(cmd);
  }
  std::sort(result.begin(), result.end());
  return result;
}

} // namespace network
} // namespace coinbasechain
