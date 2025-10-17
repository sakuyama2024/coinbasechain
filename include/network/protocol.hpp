#ifndef COINBASECHAIN_PROTOCOL_HPP
#define COINBASECHAIN_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <array>

namespace coinbasechain {
namespace protocol {

// Protocol version
constexpr uint32_t PROTOCOL_VERSION = 70016;  // Bitcoin Core 0.16.0+
constexpr uint32_t MIN_PEER_PROTO_VERSION = 70001;

// Network magic bytes - unique identifier for the network
// Using custom values (not Bitcoin mainnet to avoid accidental connections)
namespace magic {
    constexpr uint32_t MAINNET = 0xC0C0C0C0;   // Custom mainnet magic
    constexpr uint32_t TESTNET = 0xC0C0C0C1;   // Custom testnet magic
    constexpr uint32_t REGTEST = 0xC0C0C0C2;   // Custom regtest magic
}

// Default ports
namespace ports {
    constexpr uint16_t MAINNET = 8333;
    constexpr uint16_t TESTNET = 18333;
    constexpr uint16_t REGTEST = 18444;
}

// Service flags - what services this node provides
// Headers-only chain: only NODE_NETWORK is needed
enum ServiceFlags : uint64_t {
    NODE_NONE = 0,
    NODE_NETWORK = (1 << 0),        // Can serve block headers
};

// Message types - 12 bytes, null-padded
// Headers-only chain: no transactions, compact blocks, bloom filters, or mempool
namespace commands {
    // Handshake
    constexpr const char* VERSION = "version";
    constexpr const char* VERACK = "verack";

    // Peer discovery
    constexpr const char* ADDR = "addr";
    constexpr const char* GETADDR = "getaddr";

    // Block announcements and requests
    constexpr const char* INV = "inv";
    constexpr const char* GETDATA = "getdata";
    constexpr const char* NOTFOUND = "notfound";
    constexpr const char* GETHEADERS = "getheaders";
    constexpr const char* HEADERS = "headers";
    constexpr const char* SENDHEADERS = "sendheaders";  // Push-based header sync

    // Keep-alive
    constexpr const char* PING = "ping";
    constexpr const char* PONG = "pong";
}

// Inventory types for INV/GETDATA messages
// Headers-only chain: only MSG_BLOCK is needed (for block announcements)
enum class InventoryType : uint32_t {
    ERROR = 0,
    MSG_BLOCK = 2,  // Used for block hash announcements (triggers GETHEADERS)
};

// Message header constants
constexpr size_t MESSAGE_HEADER_SIZE = 24;
constexpr size_t COMMAND_SIZE = 12;
constexpr size_t CHECKSUM_SIZE = 4;

// Message size limits
constexpr uint32_t MAX_MESSAGE_SIZE = 32 * 1024 * 1024;  // 32 MB
constexpr uint32_t MAX_INV_SIZE = 50000;
constexpr uint32_t MAX_HEADERS_SIZE = 2000;
constexpr uint32_t MAX_ADDR_SIZE = 1000;

// Timeouts (matching Bitcoin Core)
constexpr int VERSION_HANDSHAKE_TIMEOUT_SEC = 60;  // 1 minute for handshake
constexpr int PING_INTERVAL_SEC = 120;              // 2 minutes between pings
constexpr int PING_TIMEOUT_SEC = 20 * 60;           // 20 minutes - peer must respond to ping
constexpr int INACTIVITY_TIMEOUT_SEC = 20 * 60;     // 20 minutes - matches Bitcoin's TIMEOUT_INTERVAL

// Network address constants
constexpr size_t MAX_SUBVERSION_LENGTH = 256;
constexpr int64_t TIMESTAMP_ALLOWANCE_SEC = 2 * 60 * 60;  // 2 hours

// User agent string
constexpr const char* USER_AGENT = "/CoinbaseChain:0.1.0/";

/**
 * Message header structure (24 bytes)
 * Layout:
 *   - magic (4 bytes): Network identifier
 *   - command (12 bytes): Command name (null-padded)
 *   - length (4 bytes): Payload length
 *   - checksum (4 bytes): First 4 bytes of double SHA256 of payload
 */
struct MessageHeader {
    uint32_t magic;
    std::array<char, COMMAND_SIZE> command;
    uint32_t length;
    std::array<uint8_t, CHECKSUM_SIZE> checksum;

    MessageHeader();
    MessageHeader(uint32_t magic, const std::string& cmd, uint32_t len);

    // Get command as string (strips null padding)
    std::string get_command() const;

    // Set command from string (adds null padding)
    void set_command(const std::string& cmd);
};

/**
 * Network address structure (30 bytes without timestamp, 34 with)
 */
struct NetworkAddress {
    uint64_t services;
    std::array<uint8_t, 16> ip;  // IPv6 format (IPv4 mapped)
    uint16_t port;

    NetworkAddress();
    NetworkAddress(uint64_t svcs, const std::array<uint8_t, 16>& addr, uint16_t p);

    // Helper to create from IPv4
    static NetworkAddress from_ipv4(uint64_t services, uint32_t ipv4, uint16_t port);

    // Helper to get IPv4 (returns 0 if not IPv4-mapped)
    uint32_t get_ipv4() const;

    // Check if this is IPv4-mapped
    bool is_ipv4() const;
};

/**
 * Timestamped network address (34 bytes)
 */
struct TimestampedAddress {
    uint32_t timestamp;
    NetworkAddress address;

    TimestampedAddress();
    TimestampedAddress(uint32_t ts, const NetworkAddress& addr);
};

/**
 * Inventory vector - identifies a transaction or block
 */
struct InventoryVector {
    InventoryType type;
    std::array<uint8_t, 32> hash;  // SHA256 hash

    InventoryVector();
    InventoryVector(InventoryType t, const std::array<uint8_t, 32>& h);
};

} // namespace protocol
} // namespace coinbasechain

#endif // COINBASECHAIN_PROTOCOL_HPP
