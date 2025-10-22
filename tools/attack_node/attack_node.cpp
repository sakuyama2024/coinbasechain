// Copyright (c) 2024 Coinbase Chain
// Attack Node - Test utility for DoS protection testing
//
// This tool connects to a node and sends malicious P2P messages to test
// DoS protection mechanisms. It should ONLY be used for testing on private networks.

#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

#include "network/protocol.hpp"
#include "network/message.hpp"
#include "chain/block.hpp"
#include "chain/sha256.hpp"

using namespace coinbasechain;
namespace asio = boost::asio;

class AttackNode {
public:
    AttackNode(asio::io_context& io_context, const std::string& host, uint16_t port)
        : socket_(io_context), host_(host), port_(port)
    {
    }

    bool connect() {
        try {
            asio::ip::tcp::resolver resolver(socket_.get_executor());
            auto endpoints = resolver.resolve(host_, std::to_string(port_));
            asio::connect(socket_, endpoints);
            std::cout << "✓ Connected to " << host_ << ":" << port_ << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "✗ Connection failed: " << e.what() << std::endl;
            return false;
        }
    }

    void send_raw_message(const std::string& command, const std::vector<uint8_t>& payload) {
        auto header = message::create_header(protocol::magic::REGTEST, command, payload);
        auto header_bytes = message::serialize_header(header);

        std::vector<uint8_t> full_message;
        full_message.insert(full_message.end(), header_bytes.begin(), header_bytes.end());
        full_message.insert(full_message.end(), payload.begin(), payload.end());

        asio::write(socket_, asio::buffer(full_message));
        std::cout << "→ Sent " << command << " (" << payload.size() << " bytes)" << std::endl;
    }

    void send_version() {
        message::VersionMessage msg;
        msg.version = protocol::PROTOCOL_VERSION;
        msg.services = protocol::NODE_NETWORK;
        msg.timestamp = std::time(nullptr);
        msg.addr_recv = protocol::NetworkAddress();
        msg.addr_from = protocol::NetworkAddress();
        msg.nonce = 0x123456789ABCDEF0ULL;  // Fixed nonce for testing
        msg.user_agent = "/AttackNode:0.1.0/";
        msg.start_height = 0;

        auto payload = msg.serialize();
        send_raw_message(protocol::commands::VERSION, payload);
    }

    void send_verack() {
        message::VerackMessage msg;
        auto payload = msg.serialize();
        send_raw_message(protocol::commands::VERACK, payload);
    }

    // Attack: Send headers with invalid PoW
    void attack_invalid_pow(const uint256& prev_hash) {
        std::cout << "\n=== ATTACK: Invalid PoW ===" << std::endl;

        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock = prev_hash;
        header.minerAddress.SetNull();
        header.nTime = std::time(nullptr);
        header.nBits = 0x00000001;  // Impossible difficulty
        header.nNonce = 0;
        header.hashRandomX.SetNull();

        std::vector<CBlockHeader> headers = {header};
        message::HeadersMessage msg;
        msg.headers = headers;
        auto payload = msg.serialize();
        send_raw_message(protocol::commands::HEADERS, payload);

        std::cout << "Expected: Peer should be disconnected immediately (score=100)" << std::endl;
    }

    // Attack: Send oversized headers message
    void attack_oversized_headers() {
        std::cout << "\n=== ATTACK: Oversized Headers ===" << std::endl;

        // Create more than MAX_HEADERS_COUNT (2000) headers
        // Use 2100 headers - just over the limit but still deserializable
        std::vector<CBlockHeader> headers;

        // Use a valid-looking RandomX hash for regtest
        uint256 dummyRandomXHash;
        dummyRandomXHash.SetHex("0000000000000000000000000000000000000000000000000000000000000000");

        CBlockHeader header;
        header.nVersion = 1;
        header.hashPrevBlock.SetNull();
        header.minerAddress.SetNull();
        header.nTime = std::time(nullptr);
        header.nBits = 0x207fffff;
        header.nNonce = 0;
        header.hashRandomX = dummyRandomXHash;  // Non-null for commitment check

        // Send 2100 headers (just over MAX_HEADERS_COUNT = 2000)
        for (int i = 0; i < 2100; i++) {
            headers.push_back(header);
        }

        message::HeadersMessage msg;
        msg.headers = headers;
        auto payload = msg.serialize();
        send_raw_message(protocol::commands::HEADERS, payload);

        std::cout << "Expected: Misbehavior +20 (oversized-headers)" << std::endl;
    }

    // Attack: Send non-continuous headers
    void attack_non_continuous_headers(const uint256& prev_hash) {
        std::cout << "\n=== ATTACK: Non-Continuous Headers ===" << std::endl;

        // Create headers that don't connect
        // Use a very small dummy RandomX hash that will pass regtest commitment check
        // For regtest (0x207fffff = max target), commitment must be < target
        // Use all zeros which will definitely pass
        uint256 dummyRandomXHash;
        dummyRandomXHash.SetHex("0000000000000000000000000000000000000000000000000000000000000000");

        CBlockHeader header1;
        header1.nVersion = 1;
        header1.hashPrevBlock = prev_hash;
        header1.minerAddress.SetNull();
        header1.nTime = std::time(nullptr);
        header1.nBits = 0x207fffff;
        header1.nNonce = 1;
        header1.hashRandomX = dummyRandomXHash;  // Valid-looking (non-null) RandomX hash

        CBlockHeader header2;
        header2.nVersion = 1;
        header2.hashPrevBlock.SetNull();  // Wrong! Doesn't connect to header1
        header2.minerAddress.SetNull();
        header2.nTime = std::time(nullptr);
        header2.nBits = 0x207fffff;
        header2.nNonce = 2;
        header2.hashRandomX = dummyRandomXHash;  // Valid-looking (non-null) RandomX hash

        std::vector<CBlockHeader> headers = {header1, header2};
        message::HeadersMessage msg;
        msg.headers = headers;
        auto payload = msg.serialize();
        send_raw_message(protocol::commands::HEADERS, payload);

        std::cout << "Expected: Misbehavior +20 (non-continuous-headers)" << std::endl;
    }

    // Attack: Spam with repeated non-continuous headers
    void attack_spam_non_continuous(const uint256& prev_hash, int count) {
        std::cout << "\n=== ATTACK: Spam Non-Continuous Headers (" << count << " times) ===" << std::endl;

        for (int i = 0; i < count; i++) {
            attack_non_continuous_headers(prev_hash);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "Expected: After 5 violations (5*20=100), peer should be disconnected" << std::endl;
    }

    // Wait for and read messages (to see VERACK, potential disconnects, etc.)
    void receive_messages(int timeout_sec = 5) {
        std::cout << "\n--- Listening for responses (" << timeout_sec << "s) ---" << std::endl;

        socket_.non_blocking(true);
        auto start = std::chrono::steady_clock::now();

        while (true) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= timeout_sec) {
                break;
            }

            try {
                std::vector<uint8_t> header_buf(protocol::MESSAGE_HEADER_SIZE);
                boost::system::error_code ec;
                size_t n = socket_.read_some(asio::buffer(header_buf), ec);

                if (ec == asio::error::would_block) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                } else if (ec) {
                    std::cout << "✗ Connection closed: " << ec.message() << std::endl;
                    break;
                }

                if (n == protocol::MESSAGE_HEADER_SIZE) {
                    protocol::MessageHeader header;
                    if (message::deserialize_header(header_buf.data(), header_buf.size(), header)) {
                        std::cout << "← Received: " << header.get_command() << " (" << header.length << " bytes)" << std::endl;

                        // Read payload
                        std::vector<uint8_t> payload_buf(header.length);
                        asio::read(socket_, asio::buffer(payload_buf));
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "✗ Read error: " << e.what() << std::endl;
                break;
            }
        }
    }

    void close() {
        socket_.close();
    }

private:
    asio::ip::tcp::socket socket_;
    std::string host_;
    uint16_t port_;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --host <host>        Target host (default: 127.0.0.1)\n"
              << "  --port <port>        Target port (default: 18444)\n"
              << "  --attack <type>      Attack type:\n"
              << "                         invalid-pow      : Send headers with invalid PoW\n"
              << "                         oversized        : Send oversized headers message\n"
              << "                         non-continuous   : Send non-continuous headers\n"
              << "                         spam-continuous  : Spam with non-continuous headers (5x)\n"
              << "                         all              : Run all attacks\n"
              << "  --help               Show this help\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 18444;
    std::string attack_type = "all";

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--attack" && i + 1 < argc) {
            attack_type = argv[++i];
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    std::cout << "=== Attack Node Test Tool ===" << std::endl;
    std::cout << "Target: " << host << ":" << port << std::endl;
    std::cout << "Attack: " << attack_type << std::endl;
    std::cout << "\nWARNING: This tool sends malicious P2P messages." << std::endl;
    std::cout << "Only use on private test networks!\n" << std::endl;

    // Get genesis hash for testing (in real test, we'd query via RPC)
    uint256 genesis_hash;
    genesis_hash.SetHex("0233b37bb6942bfb471cfd7fb95caab0e0f7b19cc8767da65fbef59eb49e45bd");

    // Helper lambda to perform handshake
    auto do_handshake = [](AttackNode& attacker) {
        std::cout << "\n--- Handshake ---" << std::endl;
        attacker.send_version();
        attacker.receive_messages(2);
        attacker.send_verack();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    };

    try {
        asio::io_context io_context;

        // If running "all" attacks, create separate connection for each to avoid
        // early disconnection affecting later tests
        if (attack_type == "all") {
            // Test 1: Invalid PoW (instant disconnect - score=100)
            std::cout << "\n========== TEST 1: Invalid PoW ==========" << std::endl;
            {
                AttackNode attacker(io_context, host, port);
                if (!attacker.connect()) return 1;
                do_handshake(attacker);
                attacker.attack_invalid_pow(genesis_hash);
                attacker.receive_messages(2);
                attacker.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Test 2: Oversized headers (+20 score)
            std::cout << "\n========== TEST 2: Oversized Headers ==========" << std::endl;
            {
                AttackNode attacker(io_context, host, port);
                if (!attacker.connect()) return 1;
                do_handshake(attacker);
                attacker.attack_oversized_headers();
                attacker.receive_messages(2);
                attacker.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Test 3: Non-continuous headers (+20 score)
            std::cout << "\n========== TEST 3: Non-Continuous Headers ==========" << std::endl;
            {
                AttackNode attacker(io_context, host, port);
                if (!attacker.connect()) return 1;
                do_handshake(attacker);
                attacker.attack_non_continuous_headers(genesis_hash);
                attacker.receive_messages(2);
                attacker.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            // Test 4: Spam attack (5x non-continuous = 100 score, disconnect)
            std::cout << "\n========== TEST 4: Spam Non-Continuous (5x) ==========" << std::endl;
            {
                AttackNode attacker(io_context, host, port);
                if (!attacker.connect()) return 1;
                do_handshake(attacker);
                attacker.attack_spam_non_continuous(genesis_hash, 5);
                attacker.receive_messages(3);
                attacker.close();
            }
        } else {
            // Single attack type - use one connection
            AttackNode attacker(io_context, host, port);

            if (!attacker.connect()) {
                return 1;
            }

            do_handshake(attacker);

            // Run single attack
            if (attack_type == "invalid-pow") {
                attacker.attack_invalid_pow(genesis_hash);
                attacker.receive_messages(2);
            } else if (attack_type == "oversized") {
                attacker.attack_oversized_headers();
                attacker.receive_messages(2);
            } else if (attack_type == "non-continuous") {
                attacker.attack_non_continuous_headers(genesis_hash);
                attacker.receive_messages(2);
            } else if (attack_type == "spam-continuous") {
                attacker.attack_spam_non_continuous(genesis_hash, 5);
                attacker.receive_messages(3);
            }

            attacker.close();
        }

        std::cout << "\n--- Test Complete ---" << std::endl;
        std::cout << "Check the target node's logs for misbehavior scores and disconnections." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
