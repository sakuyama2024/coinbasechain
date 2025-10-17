// Copyright (c) 2024 Unicity Foundation
// Distributed under the MIT software license

#ifndef COINBASECHAIN_VERSION_HPP
#define COINBASECHAIN_VERSION_HPP

#include <string>

namespace coinbasechain {

// Software version
constexpr int CLIENT_VERSION_MAJOR = 1;
constexpr int CLIENT_VERSION_MINOR = 0;
constexpr int CLIENT_VERSION_PATCH = 0;

// Build version string
constexpr const char* CLIENT_VERSION_STRING = "1.0.0";

// Copyright
constexpr const char* COPYRIGHT_YEAR = "2024";
constexpr const char* COPYRIGHT_HOLDERS = "Unicity Foundation";

// Protocol version (separate from client version)
// Increment when P2P protocol changes
constexpr int PROTOCOL_VERSION = 1;
constexpr int MIN_PEER_PROTO_VERSION = 1;

// User agent for P2P network
// Format: /CoinbaseChain:1.0.0/
inline std::string GetUserAgent() {
    return "/CoinbaseChain:" + std::string(CLIENT_VERSION_STRING) + "/";
}

// Full version info for display
inline std::string GetFullVersionString() {
    return "CoinbaseChain version " + std::string(CLIENT_VERSION_STRING);
}

// Get copyright string
inline std::string GetCopyrightString() {
    return "Copyright (C) " + std::string(COPYRIGHT_YEAR) + " " + std::string(COPYRIGHT_HOLDERS);
}

// ANSI color codes
namespace colors {
    constexpr const char* RESET = "\033[0m";
    constexpr const char* BLUE = "\033[1;34m";    // Mainnet
    constexpr const char* RED = "\033[1;31m";     // Testnet
    constexpr const char* GREEN = "\033[1;32m";   // Regtest
}

// Print startup banner with chain type
inline std::string GetStartupBanner(const std::string& chain_type) {
    // Select color based on network
    const char* color = colors::RESET;
    if (chain_type == "MAINNET") {
        color = colors::BLUE;
    } else if (chain_type == "TESTNET") {
        color = colors::RED;
    } else if (chain_type == "REGTEST") {
        color = colors::GREEN;
    }

    std::string banner;
    banner += "\n";
    banner += color;  // Start coloring
    banner += "╔═══════════════════════════════════════════════════════════════╗\n";
    banner += "║                                                               ║\n";
    banner += "║       ██╗   ██╗███╗   ██╗██╗ ██████╗██╗████████╗██╗   ██╗    ║\n";
    banner += "║       ██║   ██║████╗  ██║██║██╔════╝██║╚══██╔══╝╚██╗ ██╔╝    ║\n";
    banner += "║       ██║   ██║██╔██╗ ██║██║██║     ██║   ██║    ╚████╔╝     ║\n";
    banner += "║       ██║   ██║██║╚██╗██║██║██║     ██║   ██║     ╚██╔╝      ║\n";
    banner += "║       ╚██████╔╝██║ ╚████║██║╚██████╗██║   ██║      ██║       ║\n";
    banner += "║        ╚═════╝ ╚═╝  ╚═══╝╚═╝ ╚═════╝╚═╝   ╚═╝      ╚═╝       ║\n";
    banner += "║                                                               ║\n";
    banner += "║                         Proof of Work                         ║\n";
    banner += "║                                                               ║\n";
    banner += "╟───────────────────────────────────────────────────────────────╢\n";
    banner += "║  Version: " + std::string(CLIENT_VERSION_STRING);
    // Pad to align with box
    size_t version_padding = 54 - std::string(CLIENT_VERSION_STRING).length();
    banner += std::string(version_padding, ' ') + "║\n";

    banner += "║  Network: " + chain_type;
    size_t network_padding = 54 - chain_type.length();
    banner += std::string(network_padding, ' ') + "║\n";

    banner += "╟───────────────────────────────────────────────────────────────╢\n";
    banner += "║  " + GetCopyrightString();
    size_t copyright_padding = 62 - GetCopyrightString().length();
    banner += std::string(copyright_padding, ' ') + "║\n";
    banner += "╚═══════════════════════════════════════════════════════════════╝";
    banner += colors::RESET;  // Reset color
    banner += "\n\n";

    return banner;
}

} // namespace coinbasechain

#endif // COINBASECHAIN_VERSION_HPP
