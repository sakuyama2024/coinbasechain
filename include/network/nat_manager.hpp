// Copyright (c) 2024 Coinbase Chain
// NAT traversal manager using UPnP

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace coinbasechain {
namespace network {

class NATManager {
public:
    NATManager();
    ~NATManager();

    // Start NAT traversal (discovery + port mapping)
    bool Start(uint16_t internal_port);

    // Stop and cleanup port mappings
    void Stop();

    // Get discovered external IP
    std::string GetExternalIP() const;

    // Get mapped external port
    uint16_t GetExternalPort() const;

    // Check if port mapping is active
    bool IsPortMapped() const { return port_mapped_; }

private:
    void DiscoverUPnPDevice();
    bool MapPort(uint16_t internal_port);
    void UnmapPort();
    void RefreshMapping();  // Periodic refresh thread

    std::string external_ip_;
    uint16_t internal_port_{0};
    uint16_t external_port_{0};

    std::atomic<bool> port_mapped_{false};
    std::atomic<bool> running_{false};

    std::thread refresh_thread_;

    // UPnP device info
    std::string gateway_url_;
    std::string control_url_;
};

} // namespace network
} // namespace coinbasechain
