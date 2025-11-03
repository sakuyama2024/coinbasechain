#ifndef COINBASECHAIN_NETWORK_MESSAGE_DISPATCHER_HPP
#define COINBASECHAIN_NETWORK_MESSAGE_DISPATCHER_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace coinbasechain {

// Forward declaration
namespace message {
class Message;
} // namespace message

namespace network {

// Forward declarations
class Peer;
using PeerPtr = std::shared_ptr<Peer>;

/**
 * MessageDispatcher - Protocol message routing via handler registry
 *
 * Design:
 * - Managers register handlers for their message types
 * - Thread-safe registration and dispatch
 * - Extensible: new messages = new registration, no code changes
 *
 *
 * Usage:
 *   MessageDispatcher dispatcher;
 *   dispatcher.RegisterHandler("verack",
 *     [this](PeerPtr p, message::Message* m) {
 *       return connection_mgr_->HandleVerack(p);
 *     });
 *   dispatcher.Dispatch(peer, "verack", msg);
 */
class MessageDispatcher {
public:
  // Handler signature: takes peer + message, returns success
  using MessageHandler = std::function<bool(PeerPtr, ::coinbasechain::message::Message*)>;

  MessageDispatcher() = default;
  ~MessageDispatcher() = default;

  // Non-copyable
  MessageDispatcher(const MessageDispatcher&) = delete;
  MessageDispatcher& operator=(const MessageDispatcher&) = delete;

  /**
   * Register handler for a message command
   * Thread-safe, can be called during initialization
   *
   * Example:
   *   dispatcher.RegisterHandler("verack",
   *     [this](PeerPtr p, message::Message* m) {
   *       return connection_mgr_->HandleVerack(p);
   *     });
   *
   * @param command Message command string (e.g., "verack", "inv")
   * @param handler Function to handle this message type
   */
  void RegisterHandler(const std::string& command, MessageHandler handler);

  /**
   * Unregister handler (for testing/cleanup)
   *
   * @param command Message command to unregister
   */
  void UnregisterHandler(const std::string& command);

  /**
   * Dispatch message to registered handler
   *
   * @param peer Peer that sent the message
   * @param command Message command string
   * @param msg Parsed message object
   * @return false if no handler found or handler returns false, true otherwise
   */
  bool Dispatch(PeerPtr peer, const std::string& command, ::coinbasechain::message::Message* msg);

  /**
   * Check if handler exists for command
   *
   * @param command Message command to check
   * @return true if handler is registered
   */
  bool HasHandler(const std::string& command) const;

  /**
   * Get list of registered commands (for diagnostics)
   *
   * @return Sorted vector of registered command strings
   */
  std::vector<std::string> GetRegisteredCommands() const;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, MessageHandler> handlers_;
};

} // namespace network
} // namespace coinbasechain

#endif // COINBASECHAIN_NETWORK_MESSAGE_DISPATCHER_HPP
