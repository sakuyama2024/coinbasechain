// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef COINBASECHAIN_PRIMITIVES_BLOCK_H
#define COINBASECHAIN_PRIMITIVES_BLOCK_H

#include "uint.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// CBlockHeader - Block header structure (represents entire block in headers-only chain)
// Based on Bitcoin's block header, simplified for Coinbase Chain:
// - Uses minerAddress (uint160) instead of hashMerkleRoot
// - Includes hashRandomX for RandomX PoW algorithm
// - No transaction data (headers-only chain)
class CBlockHeader
{
public:
    // Block header fields (initialized to zero/null for safety)
    int32_t nVersion{0};
    uint256 hashPrevBlock{};        // Hash of previous block header (copied byte-for-byte as stored, no endian swap)
    uint160 minerAddress{};         // Miner's address (copied byte-for-byte as stored, no endian swap)
    uint32_t nTime{0};              // Unix timestamp
    uint32_t nBits{0};              // Difficulty target (compact format)
    uint32_t nNonce{0};             // Nonce for proof-of-work
    uint256 hashRandomX{};          // RandomX hash for PoW verification (copied byte-for-byte as stored, no endian swap)

    // Wire format constants
    static constexpr size_t UINT256_BYTES = 32;
    static constexpr size_t UINT160_BYTES = 20;

    // Serialized header size: 4 + 32 + 20 + 4 + 4 + 4 + 32 = 100 bytes
    static constexpr size_t HEADER_SIZE =
        4 +                          // nVersion (int32_t)
        UINT256_BYTES +              // hashPrevBlock
        UINT160_BYTES +              // minerAddress
        4 +                          // nTime (uint32_t)
        4 +                          // nBits (uint32_t)
        4 +                          // nNonce (uint32_t)
        UINT256_BYTES;               // hashRandomX

    // Field offsets within the 100-byte header (for serialization/deserialization)
    static constexpr size_t OFF_VERSION  = 0;
    static constexpr size_t OFF_PREV     = OFF_VERSION + 4;
    static constexpr size_t OFF_MINER    = OFF_PREV + UINT256_BYTES;
    static constexpr size_t OFF_TIME     = OFF_MINER + UINT160_BYTES;
    static constexpr size_t OFF_BITS     = OFF_TIME + 4;
    static constexpr size_t OFF_NONCE    = OFF_BITS + 4;
    static constexpr size_t OFF_RANDOMX  = OFF_NONCE + 4;

    // Compile-time verification - scalar types
    static_assert(sizeof(int32_t) == 4, "int32_t must be 4 bytes");
    static_assert(sizeof(uint32_t) == 4, "uint32_t must be 4 bytes");

    // Compile-time verification - hash/address types
    static_assert(sizeof(uint256) == UINT256_BYTES, "uint256 must be 32 bytes");
    static_assert(sizeof(uint160) == UINT160_BYTES, "uint160 must be 20 bytes");

    // Compile-time verification - total header size and offset math
    static_assert(HEADER_SIZE == 100, "Header size must be 100 bytes");
    static_assert(OFF_RANDOMX + UINT256_BYTES == HEADER_SIZE, "offset math must be correct");

    // Type alias for fixed-size header serialization
    using HeaderBytes = std::array<uint8_t, HEADER_SIZE>;

    CBlockHeader() = default;

    void SetNull() noexcept
    {
        nVersion = 0;
        hashPrevBlock.SetNull();
        minerAddress.SetNull();
        nTime = 0;
        nBits = 0;
        nNonce = 0;
        hashRandomX.SetNull();
    }

    [[nodiscard]] bool IsNull() const noexcept
    {
        return nTime == 0 && nBits == 0 && nNonce == 0 &&
               hashPrevBlock.IsNull() && minerAddress.IsNull() && hashRandomX.IsNull();
    }

    // Compute the hash of this header
    [[nodiscard]] uint256 GetHash() const noexcept;

    // Serialize to wire format (fixed-size, no heap allocation)
    // Note: Hash blobs (hashPrevBlock, minerAddress, hashRandomX) are copied
    // byte-for-byte as stored (no endian swap). Scalar fields use little-endian.
    [[nodiscard]] HeaderBytes SerializeFixed() const noexcept;

    // Serialize to wire format (heap-allocated vector for API compatibility)
    [[nodiscard]] std::vector<uint8_t> Serialize() const;

    // Deserialize from wire format
    // Note: Hash blobs (hashPrevBlock, minerAddress, hashRandomX) are copied
    // byte-for-byte as stored (no endian swap). Scalar fields use little-endian.
    [[nodiscard]] bool Deserialize(const uint8_t* data, size_t size) noexcept;

    // Deserialize from wire format (span-based overload for safer call sites)
    [[nodiscard]] bool Deserialize(std::span<const uint8_t> bytes) noexcept {
        return Deserialize(bytes.data(), bytes.size());
    }

    // Deserialize from fixed-size array (compile-time size enforcement)
    template <size_t N>
    [[nodiscard]] bool Deserialize(const std::array<uint8_t, N>& bytes) noexcept {
        if constexpr (N != HEADER_SIZE) return false;
        return Deserialize(bytes.data(), bytes.size());
    }

    // Get block timestamp
    [[nodiscard]] int64_t GetBlockTime() const noexcept
    {
        return static_cast<int64_t>(nTime);
    }

    // Human-readable string
    [[nodiscard]] std::string ToString() const;
};

// CBlockLocator - Describes a position in the block chain (for finding common ancestor with peer)
struct CBlockLocator
{
    std::vector<uint256> vHave;

    CBlockLocator() {}

    explicit CBlockLocator(std::vector<uint256>&& have) : vHave(std::move(have)) {}

    void SetNull() noexcept
    {
        vHave.clear();
    }

    [[nodiscard]] bool IsNull() const noexcept
    {
        return vHave.empty();
    }
};

#endif // COINBASECHAIN_PRIMITIVES_BLOCK_H
