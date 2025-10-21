// Copyright (c) 2024 Coinbase Chain
// NAT traversal manager implementation

#include "network/nat_manager.hpp"
#include "chain/logging.hpp"
#include <chrono>
#include <cstring>

#ifndef DISABLE_NAT_SUPPORT
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#endif

namespace coinbasechain {
namespace network {

namespace {
constexpr int UPNP_DISCOVER_TIMEOUT_MS = 2000;
constexpr int PORT_MAPPING_DURATION_SECONDS = 3600;  // 1 hour
constexpr int REFRESH_INTERVAL_SECONDS = 1800;       // 30 minutes
}

NATManager::NATManager() = default;

NATManager::~NATManager() {
    Stop();
}

bool NATManager::Start(uint16_t internal_port) {
    if (running_) {
        LOG_WARN("NAT manager already running");
        return false;
    }

    internal_port_ = internal_port;
    running_ = true;

    LOG_INFO("Starting NAT traversal for port {}", internal_port);

    // Discover UPnP device
    DiscoverUPnPDevice();

    if (gateway_url_.empty()) {
        LOG_WARN("No UPnP-capable gateway found");
        running_ = false;
        return false;
    }

    // Map port
    if (!MapPort(internal_port)) {
        LOG_ERROR("Failed to map port via UPnP");
        running_ = false;
        return false;
    }

    // Start refresh thread
    refresh_thread_ = std::thread([this]() {
        while (running_) {
            std::this_thread::sleep_for(
                std::chrono::seconds(REFRESH_INTERVAL_SECONDS));
            if (running_) {
                RefreshMapping();
            }
        }
    });

    LOG_INFO("NAT traversal successful - External {}:{}",
             external_ip_, external_port_);
    return true;
}

void NATManager::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO("Stopping NAT traversal");

    // Stop refresh thread
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }

    // Remove port mapping
    UnmapPort();
}

void NATManager::DiscoverUPnPDevice() {
#ifdef DISABLE_NAT_SUPPORT
    LOG_INFO("NAT support disabled at compile time");
    return;
#else
    int error = 0;
    UPNPDev* devlist = upnpDiscover(
        UPNP_DISCOVER_TIMEOUT_MS,
        nullptr,  // multicast interface
        nullptr,  // minissdpd socket path
        0,        // sameport
        0,        // ipv6
        2,        // ttl
        &error
    );

    if (!devlist) {
        LOG_DEBUG("UPnP discovery failed: error code {}", error);
        return;
    }

    // Get first valid IGD (Internet Gateway Device)
    UPNPUrls urls;
    IGDdatas data;
    char lanaddr[64];
    char wanaddr[64];

    int result = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));

    freeUPNPDevlist(devlist);

    if (result != 1) {
        LOG_DEBUG("No valid IGD found (result: {})", result);
        return;
    }

    // Store gateway info
    gateway_url_ = urls.controlURL;
    control_url_ = urls.controlURL;

    // Get external IP
    char ext_ip[40];
    if (UPNP_GetExternalIPAddress(
            urls.controlURL,
            data.first.servicetype,
            ext_ip) == UPNPCOMMAND_SUCCESS) {
        external_ip_ = ext_ip;
        LOG_DEBUG("Gateway found at {} (LAN: {}, WAN: {})",
                  control_url_, lanaddr, external_ip_);
    }

    FreeUPNPUrls(&urls);
#endif // DISABLE_NAT_SUPPORT
}

bool NATManager::MapPort(uint16_t internal_port) {
#ifdef DISABLE_NAT_SUPPORT
    LOG_DEBUG("NAT support disabled, skipping port mapping");
    return false;
#else
    if (gateway_url_.empty()) {
        return false;
    }

    // Reload UPnP device info for mapping
    int error = 0;
    UPNPDev* devlist = upnpDiscover(
        UPNP_DISCOVER_TIMEOUT_MS, nullptr, nullptr, 0, 0, 2, &error);

    if (!devlist) {
        return false;
    }

    UPNPUrls urls;
    IGDdatas data;
    char lanaddr[64];
    char wanaddr[64];

    int result = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);

    if (result != 1) {
        return false;
    }

    // Try to map the same port externally
    external_port_ = internal_port;

    char internal_port_str[16];
    char external_port_str[16];
    char duration_str[16];

    snprintf(internal_port_str, sizeof(internal_port_str), "%u", internal_port);
    snprintf(external_port_str, sizeof(external_port_str), "%u", external_port_);
    snprintf(duration_str, sizeof(duration_str), "%d", PORT_MAPPING_DURATION_SECONDS);

    int ret = UPNP_AddPortMapping(
        urls.controlURL,
        data.first.servicetype,
        external_port_str,  // external port
        internal_port_str,  // internal port
        lanaddr,            // internal client
        "CoinbaseChain P2P", // description
        "TCP",              // protocol
        nullptr,            // remote host (any)
        duration_str        // lease duration
    );

    FreeUPNPUrls(&urls);

    if (ret != UPNPCOMMAND_SUCCESS) {
        LOG_ERROR("UPnP port mapping failed: error code {}", ret);
        return false;
    }

    port_mapped_ = true;
    LOG_INFO("UPnP port mapping created: {} -> {}", external_port_, internal_port);
    return true;
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::UnmapPort() {
#ifdef DISABLE_NAT_SUPPORT
    return;
#else
    if (!port_mapped_ || gateway_url_.empty()) {
        return;
    }

    int error = 0;
    UPNPDev* devlist = upnpDiscover(
        UPNP_DISCOVER_TIMEOUT_MS, nullptr, nullptr, 0, 0, 2, &error);

    if (!devlist) {
        return;
    }

    UPNPUrls urls;
    IGDdatas data;
    char lanaddr[64];
    char wanaddr[64];

    int result = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
    freeUPNPDevlist(devlist);

    if (result != 1) {
        return;
    }

    char external_port_str[16];
    snprintf(external_port_str, sizeof(external_port_str), "%u", external_port_);

    UPNP_DeletePortMapping(
        urls.controlURL,
        data.first.servicetype,
        external_port_str,
        "TCP",
        nullptr
    );

    FreeUPNPUrls(&urls);

    port_mapped_ = false;
    LOG_INFO("UPnP port mapping removed");
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::RefreshMapping() {
    LOG_DEBUG("Refreshing UPnP port mapping");

    // Remove old mapping and create new one
    // This ensures the mapping stays active
    if (port_mapped_) {
        UnmapPort();
        MapPort(internal_port_);
    }
}

const std::string& NATManager::GetExternalIP() const {
    return external_ip_;
}

uint16_t NATManager::GetExternalPort() const {
    return external_port_;
}

} // namespace network
} // namespace coinbasechain
