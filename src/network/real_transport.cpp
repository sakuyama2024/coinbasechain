// Copyright (c) 2024 Coinbase Chain
// Real transport implementation using boost::asio TCP sockets

#include "network/real_transport.hpp"
#include "network/protocol.hpp"
#include "util/logging.hpp"
#include <thread>

namespace coinbasechain {
namespace network {

// ============================================================================
// RealTransportConnection
// ============================================================================

std::atomic<uint64_t> RealTransportConnection::next_id_{1};

TransportConnectionPtr RealTransportConnection::create_outbound(
    boost::asio::io_context &io_context, const std::string &address,
    uint16_t port, ConnectCallback callback) {
  auto conn = std::shared_ptr<RealTransportConnection>(
      new RealTransportConnection(io_context, false));
  conn->do_connect(address, port, callback);
  return conn;
}

TransportConnectionPtr
RealTransportConnection::create_inbound(boost::asio::io_context &io_context,
                                        boost::asio::ip::tcp::socket socket) {
  auto conn = std::shared_ptr<RealTransportConnection>(
      new RealTransportConnection(io_context, true));
  conn->socket_ = std::move(socket);
  conn->open_ = true;

  // Get remote endpoint
  try {
    auto remote_ep = conn->socket_.remote_endpoint();
    conn->remote_addr_ = remote_ep.address().to_string();
    conn->remote_port_ = remote_ep.port();
  } catch (const std::exception &e) {
    LOG_NET_TRACE("failed to get remote endpoint: {}", e.what());
  }

  return conn;
}

RealTransportConnection::RealTransportConnection(
    boost::asio::io_context &io_context, bool is_inbound)
    : io_context_(io_context), socket_(io_context), is_inbound_(is_inbound),
      id_(next_id_++), recv_buffer_(RECV_BUFFER_SIZE) {}

RealTransportConnection::~RealTransportConnection() {
  // SECURITY: Destructor should be defensive-only, never initiate cleanup
  // All cleanup must happen in close() while the shared_ptr is still alive
  // Reason: If we close socket here, pending async operations might invoke
  // callbacks during/after destructor execution (use-after-free)
  //
  // Correct lifecycle: close() is called -> callbacks cleared -> socket closed
  // Then later: destructor runs on already-cleaned-up object
  if (open_.load()) {
    LOG_NET_ERROR("CRITICAL: RealTransportConnection destructor called without "
                  "prior close() - address:{}:{}. This indicates a lifecycle bug. "
                  "close() must be called while shared_ptr is alive.",
                  remote_addr_, remote_port_);
    // Don't attempt cleanup here - would risk use-after-free if async ops pending
  }
}

void RealTransportConnection::do_connect(const std::string &address,
                                         uint16_t port,
                                         ConnectCallback callback) {
  remote_addr_ = address;
  remote_port_ = port;

  // Resolve address
  auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(io_context_);
  resolver->async_resolve(
      address, std::to_string(port),
      [this, self = shared_from_this(), callback,
       resolver](const boost::system::error_code &ec,
                 boost::asio::ip::tcp::resolver::results_type results) {
        if (ec) {
          LOG_NET_TRACE("failed to resolve {}: {}", remote_addr_, ec.message());
          if (callback) {
            try {
              callback(false);
            } catch (const std::exception &e) {
              LOG_WARN("Exception in connect callback: {}", e.what());
            } catch (...) {
              LOG_WARN("Unknown exception in connect callback");
            }
          }
          return;
        }

        // Connect to first resolved endpoint
        boost::asio::async_connect(
            socket_, results,
            [this, self, callback](const boost::system::error_code &ec,
                                   const boost::asio::ip::tcp::endpoint &) {
              if (ec) {
                LOG_NET_TRACE("failed to connect to {}:{}: {}", remote_addr_,
                          remote_port_, ec.message());
                if (callback) {
                  try {
                    callback(false);
                  } catch (const std::exception &e) {
                    LOG_WARN("Exception in connect callback: {}", e.what());
                  } catch (...) {
                    LOG_WARN("Unknown exception in connect callback");
                  }
                }
                return;
              }

              open_ = true;

              // Set useful TCP options (best-effort)
              boost::system::error_code opt_ec;
              socket_.set_option(boost::asio::ip::tcp::no_delay(true), opt_ec);
              socket_.set_option(boost::asio::socket_base::keep_alive(true), opt_ec);

              // Canonicalize remote address/port from the actual socket endpoint
              try {
                auto ep = socket_.remote_endpoint();
                remote_addr_ = ep.address().to_string();
                remote_port_ = ep.port();
              } catch (const std::exception &e) {
                LOG_NET_TRACE("failed to get remote endpoint after connect: {}", e.what());
              } catch (...) {
                LOG_NET_TRACE("unknown exception getting remote endpoint after connect");
              }

              LOG_NET_TRACE("connected to {}:{}", remote_addr_, remote_port_);
              if (callback) {
                try {
                  callback(true);
                } catch (const std::exception &e) {
                  LOG_NET_TRACE("exception in connect callback: {}", e.what());
                } catch (...) {
                  LOG_NET_TRACE("unknown exception in connect callback");
                }
              }
            });
      });
}

void RealTransportConnection::start() {
  if (!open_)
    return;
  start_read();
}

void RealTransportConnection::start_read() {
  if (!open_)
    return;

  socket_.async_read_some(
      boost::asio::buffer(recv_buffer_),
      [this, self = shared_from_this()](const boost::system::error_code &ec,
                                        size_t bytes_transferred) {
        if (ec) {
          if (ec != boost::asio::error::eof &&
              ec != boost::asio::error::operation_aborted) {
            LOG_NET_TRACE("read error from {}:{}: {}", remote_addr_, remote_port_,
                      ec.message());
          }
          // Save disconnect callback before close() clears it
          auto saved_disconnect_cb = disconnect_callback_;
          close();
          // Invoke saved callback after close() (safe: we're in async callback with shared_ptr)
          if (saved_disconnect_cb) {
            try {
              saved_disconnect_cb();
            } catch (const std::exception &e) {
              LOG_NET_TRACE("exception in disconnect callback: {}", e.what());
            } catch (...) {
              LOG_NET_TRACE("unknown exception in disconnect callback");
            }
          }
          return;
        }

        if (bytes_transferred > 0 && receive_callback_) {
          LOG_NET_TRACE("tcp received {} bytes from {}:{}", bytes_transferred,
                        remote_addr_, remote_port_);
          std::vector<uint8_t> data(recv_buffer_.begin(),
                                    recv_buffer_.begin() + bytes_transferred);
          try {
            receive_callback_(data);
          } catch (const std::exception &e) {
            LOG_NET_TRACE("exception in receive callback from {}:{}: {}",
                     remote_addr_, remote_port_, e.what());
          } catch (...) {
            LOG_NET_TRACE("unknown exception in receive callback from {}:{}",
                     remote_addr_, remote_port_);
          }
        }

        // Continue reading
        start_read();
      });
}

bool RealTransportConnection::send(const std::vector<uint8_t> &data) {
  if (!open_)
    return false;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // DoS Protection: Enforce send queue size limit (prevent slow-reading peer from exhausting memory)
    // If peer is not reading fast enough, disconnect rather than accumulating unbounded queue
    if (send_queue_bytes_ + data.size() > protocol::DEFAULT_SEND_QUEUE_SIZE) {
      LOG_NET_WARN("Send queue overflow (current: {} bytes, incoming: {} bytes, limit: {} bytes), "
                   "disconnecting slow-reading peer {}:{}",
                   send_queue_bytes_, data.size(), protocol::DEFAULT_SEND_QUEUE_SIZE,
                   remote_addr_, remote_port_);
      // Mark as closed to prevent further sends; actual disconnect happens in close()
      open_ = false;

      // Schedule disconnect callback on io_context thread to avoid lock ordering issues
      boost::asio::post(io_context_, [this, self = shared_from_this()]() {
        close();
        if (disconnect_callback_) {
          try {
            disconnect_callback_();
          } catch (const std::exception &e) {
            LOG_NET_TRACE("exception in disconnect callback: {}", e.what());
          } catch (...) {
            LOG_NET_TRACE("unknown exception in disconnect callback");
          }
        }
      });

      return false;
    }

    send_queue_.push(data);
    send_queue_bytes_ += data.size();
  }

  // Trigger write if not already writing
  boost::asio::post(io_context_,
                    [this, self = shared_from_this()]() { do_write(); });

  return true;
}

void RealTransportConnection::do_write() {
  if (!open_)
    return;

  std::lock_guard<std::mutex> lock(send_mutex_);

  if (writing_ || send_queue_.empty()) {
    return;
  }

  writing_ = true;
  auto &data = send_queue_.front();

  boost::asio::async_write(
      socket_, boost::asio::buffer(data),
      [this, self = shared_from_this()](const boost::system::error_code &ec,
                                        size_t bytes_transferred) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        writing_ = false;

        if (ec) {
          LOG_NET_TRACE("write error to {}:{}: {}", remote_addr_, remote_port_,
                    ec.message());
          // Save disconnect callback before close() clears it
          auto saved_disconnect_cb = disconnect_callback_;
          close();
          // Invoke saved callback after close() (safe: we're in async callback with shared_ptr)
          if (saved_disconnect_cb) {
            try {
              saved_disconnect_cb();
            } catch (const std::exception &e) {
              LOG_NET_TRACE("exception in disconnect callback: {}", e.what());
            } catch (...) {
              LOG_NET_TRACE("unknown exception in disconnect callback");
            }
          }
          return;
        }

        // Remove sent message and update byte counter
        size_t sent_bytes = send_queue_.front().size();
        send_queue_.pop();
        send_queue_bytes_ -= sent_bytes;

        // Continue writing if more queued
        // IMPORTANT: Don't call do_write() directly here - that would cause
        // a deadlock because we're already holding send_mutex_. Instead, post
        // to io_context to call it after the lock is released.
        if (!send_queue_.empty()) {
          boost::asio::post(io_context_,
                            [this, self = shared_from_this()]() { do_write(); });
        }
      });
}

void RealTransportConnection::close() {
  if (!open_.exchange(false)) {
    return; // Already closed
  }

  // SECURITY: Clear callbacks BEFORE shutting down socket to prevent use-after-free
  // If pending async operations complete after close(), they should not invoke callbacks
  receive_callback_ = {};
  disconnect_callback_ = {};

  boost::system::error_code ec;
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
  socket_.close(ec);

  // Clear pending sends
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    writing_ = false;
    std::queue<std::vector<uint8_t>> empty;
    std::swap(send_queue_, empty);
    send_queue_bytes_ = 0;
  }
}

bool RealTransportConnection::is_open() const { return open_; }

std::string RealTransportConnection::remote_address() const {
  return remote_addr_;
}

uint16_t RealTransportConnection::remote_port() const { return remote_port_; }

void RealTransportConnection::set_receive_callback(ReceiveCallback callback) {
  receive_callback_ = callback;
}

void RealTransportConnection::set_disconnect_callback(
    DisconnectCallback callback) {
  disconnect_callback_ = callback;
}

// ============================================================================
// RealTransport
// ============================================================================

RealTransport::RealTransport(size_t io_threads)
    : desired_io_threads_(io_threads) {
}

RealTransport::~RealTransport() { stop(); }

TransportConnectionPtr RealTransport::connect(const std::string &address,
                                              uint16_t port,
                                              ConnectCallback callback) {
  return RealTransportConnection::create_outbound(io_context_, address, port,
                                                  callback);
}

bool RealTransport::listen(
    uint16_t port,
    std::function<void(TransportConnectionPtr)> accept_callback) {
  if (acceptor_) {
    LOG_NET_TRACE("already listening");
    return false;
  }

  accept_callback_ = accept_callback;

  try {
    using tcp = boost::asio::ip::tcp;
    acceptor_ = std::make_unique<tcp::acceptor>(io_context_);

    // Try dual-stack (IPv6 with v6_only=false); fall back to IPv4-only on failure
    try {
      acceptor_->open(tcp::v6());
      acceptor_->set_option(boost::asio::ip::v6_only(false));
      acceptor_->set_option(tcp::acceptor::reuse_address(true));
      acceptor_->bind(tcp::endpoint(tcp::v6(), port));
      acceptor_->listen();
    } catch (const std::exception &) {
      boost::system::error_code ec;
      acceptor_->close(ec);
      acceptor_->open(tcp::v4());
      acceptor_->set_option(tcp::acceptor::reuse_address(true));
      acceptor_->bind(tcp::endpoint(tcp::v4(), port));
      acceptor_->listen();
    }

    LOG_NET_INFO("listening on port {}", port);
    start_accept();
    return true;

  } catch (const std::exception &e) {
    LOG_NET_ERROR("failed to listen on port {}: {}", port, e.what());
    return false;
  }
}

void RealTransport::start_accept() {
  if (!acceptor_)
    return;

  acceptor_->async_accept([this](const boost::system::error_code &ec,
                                 boost::asio::ip::tcp::socket socket) {
    handle_accept(ec, std::move(socket));
  });
}

void RealTransport::handle_accept(const boost::system::error_code &ec,
                                  boost::asio::ip::tcp::socket socket) {
  if (ec) {
    if (ec != boost::asio::error::operation_aborted) {
      LOG_NET_TRACE("accept error: {}", ec.message());
      // Continue accepting despite error
      start_accept();
    }
    return;
  }

  // Set useful TCP options on the accepted socket (best-effort)
  {
    boost::system::error_code opt_ec;
    socket.set_option(boost::asio::ip::tcp::no_delay(true), opt_ec);
    socket.set_option(boost::asio::socket_base::keep_alive(true), opt_ec);
  }

  // Create inbound connection
  auto conn = RealTransportConnection::create_inbound(io_context_, std::move(socket));

  // Notify callback (wrap in try-catch to ensure accept loop continues)
  if (accept_callback_) {
    try {
      accept_callback_(conn);
    } catch (const std::exception &e) {
      LOG_NET_TRACE("exception in accept callback: {}", e.what());
    } catch (...) {
      LOG_NET_TRACE("unknown exception in accept callback");
    }
  }

  // Continue accepting
  start_accept();
}

void RealTransport::stop_listening() {
  if (acceptor_) {
    boost::system::error_code ec;
    acceptor_->close(ec);
    acceptor_.reset();
  }
}

void RealTransport::run() {
  if (running_.exchange(true)) {
    return;
  }
  io_context_.restart();
  work_guard_ = std::make_unique<boost::asio::executor_work_guard<
      boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(io_context_));
  for (size_t i = 0; i < desired_io_threads_; i++) {
    io_threads_.emplace_back([this]() { io_context_.run(); });
  }
}

void RealTransport::stop() {
  running_.store(false);

  LOG_NET_TRACE("stopping transport");

  stop_listening();

  // Stop io_context
  work_guard_.reset();
  io_context_.stop();

  // Join IO threads
  for (auto &thread : io_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  io_threads_.clear();
}

} // namespace network
} // namespace coinbasechain
