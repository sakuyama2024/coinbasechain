// Copyright (c) 2024 Coinbase Chain
// Connection types implementation

#include "network/connection_types.hpp"

namespace coinbasechain {
namespace network {

std::string ConnectionTypeAsString(ConnectionType conn_type) {
  switch (conn_type) {
  case ConnectionType::INBOUND:
    return "inbound";
  case ConnectionType::OUTBOUND:
    return "outbound";
  case ConnectionType::MANUAL:
    return "manual";
  case ConnectionType::FEELER:
    return "feeler";
  default:
    return "unknown";
  }
}

} // namespace network
} // namespace coinbasechain
