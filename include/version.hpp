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
inline std::string GetVersionString() {
  return std::to_string(CLIENT_VERSION_MAJOR) + "." +
         std::to_string(CLIENT_VERSION_MINOR) + "." +
         std::to_string(CLIENT_VERSION_PATCH);
}

// Copyright
constexpr const char *COPYRIGHT_YEAR = "2025";
constexpr const char *COPYRIGHT_HOLDERS = "The Unicity Foundation";

// User agent for P2P network
// Format: /CoinbaseChain:1.0.0/
inline std::string GetUserAgent() {
  return "/CoinbaseChain:" + GetVersionString() + "/";
}

// Full version info for display
inline std::string GetFullVersionString() {
  return "CoinbaseChain version " + GetVersionString();
}

// Get copyright string
inline std::string GetCopyrightString() {
  return "Copyright (C) " + std::string(COPYRIGHT_YEAR) + " " +
         std::string(COPYRIGHT_HOLDERS);
}

// ANSI color codes
namespace colors {
constexpr const char *RESET = "\033[0m";
constexpr const char *BLUE = "\033[1;34m";  // Mainnet
constexpr const char *RED = "\033[1;31m";   // Testnet
constexpr const char *GREEN = "\033[1;32m"; // Regtest
} // namespace colors

// Print startup banner with chain type
inline std::string GetStartupBanner(const std::string &chain_type) {
  // Select color based on network
  const char *color = colors::RESET;
  if (chain_type == "MAINNET") {
    color = colors::BLUE;
  } else if (chain_type == "TESTNET") {
    color = colors::RED;
  } else if (chain_type == "REGTEST") {
    color = colors::GREEN;
  }

  std::string banner;
  banner += "\n";
  banner += color; // Start coloring
  banner +=
      "╔═══════════════════════════════════════════════════════════════╗\n";
  banner +=
      "║                                                               ║\n";
  banner +=
      "║       ██╗   ██╗███╗   ██╗██╗ ██████╗██╗████████╗██╗   ██╗    ║\n";
  banner +=
      "║       ██║   ██║████╗  ██║██║██╔════╝██║╚══██╔══╝╚██╗ ██╔╝    ║\n";
  banner +=
      "║       ██║   ██║██╔██╗ ██║██║██║     ██║   ██║    ╚████╔╝     ║\n";
  banner +=
      "║       ██║   ██║██║╚██╗██║██║██║     ██║   ██║     ╚██╔╝      ║\n";
  banner +=
      "║       ╚██████╔╝██║ ╚████║██║╚██████╗██║   ██║      ██║       ║\n";
  banner +=
      "║        ╚═════╝ ╚═╝  ╚═══╝╚═╝ ╚═════╝╚═╝   ╚═╝      ╚═╝       ║\n";
  banner +=
      "║                                                               ║\n";
  banner +=
      "║                         Proof of Work                         ║\n";
  banner +=
      "║                                                               ║\n";
  banner +=
      "╟───────────────────────────────────────────────────────────────╢\n";
  std::string version_str = GetVersionString();
  banner += "║  Version: " + version_str;
  // Pad to align with box
  size_t version_padding = 54 - version_str.length();
  banner += std::string(version_padding, ' ') + "║\n";

  banner += "║  Network: " + chain_type;
  size_t network_padding = 54 - chain_type.length();
  banner += std::string(network_padding, ' ') + "║\n";

  banner +=
      "╟───────────────────────────────────────────────────────────────╢\n";
  banner += "║  " + GetCopyrightString();
  size_t copyright_padding = 62 - GetCopyrightString().length();
  banner += std::string(copyright_padding, ' ') + "║\n";
  banner += "╚═══════════════════════════════════════════════════════════════╝";
  banner += colors::RESET; // Reset color
  banner += "\n\n";

  return banner;
}

} // namespace coinbasechain

#endif // COINBASECHAIN_VERSION_HPP
