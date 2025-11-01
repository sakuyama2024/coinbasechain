// Copyright (c) 2024 Coinbase Chain
// NAT traversal manager implementation

#include "network/nat_manager.hpp"
#include "util/logging.hpp"
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

// miniupnpc API version compatibility
// Version 2.1+ uses 7-argument UPNP_GetValidIGD (with wanaddr)
// Earlier versions use 5-argument UPNP_GetValidIGD (without wanaddr)
#ifndef DISABLE_NAT_SUPPORT
#if MINIUPNPC_API_VERSION >= 17
#define UPNP_GETVALIDIGD_ARGS(devlist, urls, data, lanaddr) \
    devlist, urls, data, lanaddr, sizeof(lanaddr), nullptr, 0
#else
#define UPNP_GETVALIDIGD_ARGS(devlist, urls, data, lanaddr) \
    devlist, urls, data, lanaddr, sizeof(lanaddr)
#endif
#endif
}

NATManager::NATManager() = default;

NATManager::~NATManager() {
    Stop();
}

bool NATManager::Start(uint16_t internal_port) {
    if (running_) {
        LOG_NET_TRACE("NAT manager already running");
        return false;
    }

    if (internal_port == 0) {
        LOG_NET_ERROR("invalid internal port: 0");
        return false;
    }

    internal_port_ = internal_port;
    running_ = true;

    LOG_NET_TRACE("starting NAT traversal for port {}", internal_port);

    // Discover UPnP device
    DiscoverUPnPDevice();

    if (control_url_.empty() || igd_service_type_.empty() || lanaddr_.empty()) {
        LOG_NET_DEBUG("no UPnP-capable gateway found");
        running_ = false;
        return false;
    }

    // Map port
    if (!MapPort(internal_port)) {
        LOG_NET_ERROR("failed to map port via UPnP");
        running_ = false;
        return false;
    }

    // Start refresh thread
    refresh_thread_ = std::thread([this]() {
        std::unique_lock<std::mutex> lock(refresh_mutex_);
        while (running_) {
            if (refresh_cv_.wait_for(lock, std::chrono::seconds(REFRESH_INTERVAL_SECONDS), [this]() { return !running_; })) {
                break; // stop requested
            }
            // Perform mapping refresh outside the lock
            lock.unlock();
            RefreshMapping();
            lock.lock();
        }
    });

    LOG_NET_TRACE("NAT traversal successful - external {}:{}",
             external_ip_, external_port_);
    return true;
}

void NATManager::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_NET_TRACE("stopping NAT traversal");

    // Stop refresh thread
    refresh_cv_.notify_all();
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }

    // Remove port mapping
    UnmapPort();
}

void NATManager::DiscoverUPnPDevice() {
#ifdef DISABLE_NAT_SUPPORT
    LOG_NET_TRACE("NAT support disabled at compile time");
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
        LOG_NET_DEBUG("UPnP discovery failed: error code {}", error);
        return;
    }

    // Get first valid IGD (Internet Gateway Device)
    UPNPUrls urls{};
    IGDdatas data{};
    char lanaddr[64] = {0};

    int result = UPNP_GetValidIGD(UPNP_GETVALIDIGD_ARGS(devlist, &urls, &data, lanaddr));

    freeUPNPDevlist(devlist);

    if (result != 1) {
        LOG_NET_DEBUG("no valid IGD found (result: {})", result);
        FreeUPNPUrls(&urls);
        return;
    }

    // Store gateway info (copy to our cached strings)
    control_url_ = urls.controlURL ? urls.controlURL : "";
    igd_service_type_ = data.first.servicetype; // fixed: servicetype is an array, always non-null
    lanaddr_ = lanaddr;

    // Get external IP
    char ext_ip[40] = {0};
    if (!control_url_.empty() && !igd_service_type_.empty() &&
        UPNP_GetExternalIPAddress(
            control_url_.c_str(),
            igd_service_type_.c_str(),
            ext_ip) == UPNPCOMMAND_SUCCESS) {
        external_ip_ = ext_ip;
        LOG_NET_TRACE("gateway found (LAN: {}, WAN: {})", lanaddr_, external_ip_);
    }

    FreeUPNPUrls(&urls);
#endif // DISABLE_NAT_SUPPORT
}

bool NATManager::MapPort(uint16_t internal_port) {
#ifdef DISABLE_NAT_SUPPORT
    LOG_NET_TRACE("NAT support disabled, skipping port mapping");
    return false;
#else
    if (control_url_.empty() || igd_service_type_.empty() || lanaddr_.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> guard(mapping_mutex_);

    // Try to map the same port externally
    external_port_ = internal_port;

    const std::string internal_port_str = std::to_string(internal_port);
    const std::string external_port_str = std::to_string(external_port_);
    const std::string duration_str = std::to_string(PORT_MAPPING_DURATION_SECONDS);

    int ret = UPNP_AddPortMapping(
        control_url_.c_str(),
        igd_service_type_.c_str(),
        external_port_str.c_str(),  // external port
        internal_port_str.c_str(),  // internal port
        lanaddr_.c_str(),           // internal client
        "CoinbaseChain P2P",       // description
        "TCP",                     // protocol
        nullptr,                    // remote host (any)
        duration_str.c_str()        // lease duration
    );

    if (ret != UPNPCOMMAND_SUCCESS) {
        LOG_NET_ERROR("UPnP port mapping failed: error code {}", ret);
        return false;
    }

    port_mapped_ = true;
    LOG_NET_TRACE("UPnP port mapping created/refreshed: {} -> {}", external_port_, internal_port);
    return true;
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::UnmapPort() {
#ifdef DISABLE_NAT_SUPPORT
    return;
#else
    if (!port_mapped_) {
        return;
    }

    if (control_url_.empty() || igd_service_type_.empty()) {
        port_mapped_ = false;
        return;
    }

    std::lock_guard<std::mutex> guard(mapping_mutex_);

    const std::string external_port_str = std::to_string(external_port_);

    int ret = UPNP_DeletePortMapping(
        control_url_.c_str(),
        igd_service_type_.c_str(),
        external_port_str.c_str(),
        "TCP",
        nullptr
    );
    (void)ret; // ignore ret; best-effort cleanup

    port_mapped_ = false;
    LOG_NET_TRACE("UPnP port mapping removed");
#endif // DISABLE_NAT_SUPPORT
}

void NATManager::RefreshMapping() {
    LOG_NET_TRACE("refreshing UPnP port mapping");

    if (!port_mapped_) return;

#ifndef DISABLE_NAT_SUPPORT
    // Re-issue the same AddPortMapping call to refresh/extend lease without tearing down
    {
        std::lock_guard<std::mutex> guard(mapping_mutex_);
        const std::string internal_port_str = std::to_string(internal_port_);
        const std::string external_port_str = std::to_string(external_port_);
        const std::string duration_str = std::to_string(PORT_MAPPING_DURATION_SECONDS);
        (void)UPNP_AddPortMapping(
            control_url_.c_str(),
            igd_service_type_.c_str(),
            external_port_str.c_str(),
            internal_port_str.c_str(),
            lanaddr_.c_str(),
            "CoinbaseChain P2P",
            "TCP",
            nullptr,
            duration_str.c_str());

        // Try to refresh external IP as well (it may change)
        char ext_ip[40] = {0};
        if (!control_url_.empty() && !igd_service_type_.empty() &&
            UPNP_GetExternalIPAddress(control_url_.c_str(), igd_service_type_.c_str(), ext_ip) == UPNPCOMMAND_SUCCESS &&
            ext_ip[0] != '\0') {
            external_ip_ = ext_ip;
        }
    }
#endif
}

const std::string& NATManager::GetExternalIP() const {
    return external_ip_;
}

uint16_t NATManager::GetExternalPort() const {
    return external_port_;
}

} // namespace network
} // namespace coinbasechain
