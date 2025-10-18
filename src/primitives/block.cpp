// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/block.h"
#include "crypto/sha256.h"
#include "util/endian.hpp"
#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

namespace {
// Compile-time header size calculation - if this doesn't match HEADER_SIZE,
// compilation will fail (prevents silent drift during refactoring)
static constexpr size_t kHeaderSize =
    4 /*nVersion*/ + 32 /*hashPrevBlock*/ + 20 /*minerAddress*/ + 4 /*nTime*/ +
    4 /*nBits*/ + 4 /*nNonce*/ + 32 /*hashRandomX*/;
static_assert(kHeaderSize == CBlockHeader::HEADER_SIZE, "HEADER_SIZE mismatch");
} // namespace

uint256 CBlockHeader::GetHash() const noexcept {
  // Serialize using fixed-size array (no heap allocation)
  const auto s = SerializeFixed();

  // Double SHA256 - use separate hasher instances for safety
  // (not all CSHA256 implementations guarantee Reset() works after Finalize())
  uint8_t h1[32], h2[32];
  CSHA256().Write(s.data(), s.size()).Finalize(h1);
  CSHA256().Write(h1, 32).Finalize(h2);

  // CSHA256 outputs bytes in big-endian order, but uint256 stores bytes
  // in little-endian format. Reverse big-endian digest into little-endian
  // storage.
  uint256 out;
  std::reverse_copy(h2, h2 + 32, out.begin());
  return out;
}

CBlockHeader::HeaderBytes CBlockHeader::SerializeFixed() const noexcept {
  // CONSENSUS-CRITICAL: Wire format is exactly 100 bytes, all little-endian
  // See SERIALIZATION_SPECIFICATION.md for complete format documentation
  // Field order and sizes MUST NEVER CHANGE without a hard fork

  HeaderBytes data{};

  // nVersion (4 bytes, offset 0, little-endian)
  endian::WriteLE32(data.data() + OFF_VERSION, static_cast<uint32_t>(nVersion));

  // hashPrevBlock (32 bytes, offset 4)
  // uint256 stores bytes in internal format, copy directly to wire (no endian
  // swap)
  std::copy(hashPrevBlock.begin(), hashPrevBlock.end(),
            data.begin() + OFF_PREV);

  // minerAddress (20 bytes, offset 36)
  // uint160 stores bytes in internal format, copy directly to wire (no endian
  // swap)
  std::copy(minerAddress.begin(), minerAddress.end(), data.begin() + OFF_MINER);

  // nTime (4 bytes, offset 56, little-endian)
  endian::WriteLE32(data.data() + OFF_TIME, nTime);

  // nBits (4 bytes, offset 60, little-endian)
  endian::WriteLE32(data.data() + OFF_BITS, nBits);

  // nNonce (4 bytes, offset 64, little-endian)
  endian::WriteLE32(data.data() + OFF_NONCE, nNonce);

  // hashRandomX (32 bytes, offset 68)
  // uint256 stores bytes in internal format, copy directly to wire (no endian
  // swap)
  std::copy(hashRandomX.begin(), hashRandomX.end(), data.begin() + OFF_RANDOMX);

  return data;
}

std::vector<uint8_t> CBlockHeader::Serialize() const {
  // Wrap SerializeFixed() for API compatibility with code expecting vector
  auto arr = SerializeFixed();
  return std::vector<uint8_t>(arr.begin(), arr.end());
}

bool CBlockHeader::Deserialize(const uint8_t *data, size_t size) noexcept {
  // Consensus-critical: Reject if size doesn't exactly match HEADER_SIZE
  // This prevents silent truncation/padding that could cause consensus splits
  if (size != HEADER_SIZE) {
    return false;
  }

  // nVersion (4 bytes, offset 0, little-endian)
  nVersion = static_cast<int32_t>(endian::ReadLE32(data + OFF_VERSION));

  // hashPrevBlock (32 bytes, offset 4)
  // Copy directly from wire to internal format (no endian swap)
  std::copy(data + OFF_PREV, data + OFF_PREV + UINT256_BYTES,
            hashPrevBlock.begin());

  // minerAddress (20 bytes, offset 36)
  // Copy directly from wire to internal format (no endian swap)
  std::copy(data + OFF_MINER, data + OFF_MINER + UINT160_BYTES,
            minerAddress.begin());

  // nTime (4 bytes, offset 56, little-endian)
  nTime = endian::ReadLE32(data + OFF_TIME);

  // nBits (4 bytes, offset 60, little-endian)
  nBits = endian::ReadLE32(data + OFF_BITS);

  // nNonce (4 bytes, offset 64, little-endian)
  nNonce = endian::ReadLE32(data + OFF_NONCE);

  // hashRandomX (32 bytes, offset 68)
  // Copy directly from wire to internal format (no endian swap)
  std::copy(data + OFF_RANDOMX, data + OFF_RANDOMX + UINT256_BYTES,
            hashRandomX.begin());

  return true;
}

std::string CBlockHeader::ToString() const {
  std::stringstream s;
  s << "CBlockHeader(\n";
  s << "  version=" << nVersion << "\n";
  s << "  hashPrevBlock=" << hashPrevBlock.GetHex() << "\n";
  s << "  minerAddress=" << minerAddress.GetHex() << "\n";
  s << "  nTime=" << nTime << "\n";
  s << "  nBits=0x" << std::hex << nBits << std::dec << "\n";
  s << "  nNonce=" << nNonce << "\n";
  s << "  hashRandomX=" << hashRandomX.GetHex() << "\n";
  s << "  hash=" << GetHash().GetHex() << "\n";
  s << ")\n";
  return s.str();
}
