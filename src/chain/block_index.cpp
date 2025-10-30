// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/block_index.hpp"
#include "util/arith_uint256.hpp"
#include <sstream>
#include <string>

//DTC1

namespace coinbasechain {
namespace chain {

// Helper function for skip list: inverts the lowest set bit
// Bitcoin Core pattern for deterministic skip heights
static inline int InvertLowestOne(int n) { return n & (n - 1); }

// Calculate the skip height for a given height (Bitcoin Core algorithm)
// Returns the height of the ancestor this block should skip to
static int GetSkipHeight(int height) {
  if (height < 2)
    return 0;

  // Determine which height to skip to. When height is a power of 2,
  // skip back to the previous power of 2 (e.g., 8 -> 4, 16 -> 8).
  // Otherwise, skip to the most recent power of 2 less than height.
  // This creates a binary tree structure for efficient traversal.
  return (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1
                      : InvertLowestOne(height);
}

// Build skip list pointer (Bitcoin Core algorithm)
// Must be called when adding block to chain, after pprev and nHeight are set
void CBlockIndex::BuildSkip() {
  if (pprev)
    pskip = pprev->GetAncestor(GetSkipHeight(nHeight));
}

// Get ancestor at given height using skip list (O(log n) with skip list)
CBlockIndex *CBlockIndex::GetAncestor(int height) {
  if (height > nHeight || height < 0)
    return nullptr;

  CBlockIndex *pindexWalk = this;
  int heightWalk = nHeight;
  while (heightWalk > height) {
    int heightSkip = GetSkipHeight(heightWalk);
    int heightSkipPrev = GetSkipHeight(heightWalk - 1);
    // Use skip pointer when possible
    if (pindexWalk->pskip != nullptr &&
        (heightSkip == height ||
         (heightSkip > height && !(heightSkipPrev < heightSkip - 2 &&
                                   heightSkipPrev >= height)))) {
      // Use skip
      pindexWalk = pindexWalk->pskip;
      heightWalk = heightSkip;
    } else {
      // Fall back to pprev
      assert(pindexWalk->pprev);
      pindexWalk = pindexWalk->pprev;
      heightWalk--;
    }
  }
  return pindexWalk;
}

const CBlockIndex *CBlockIndex::GetAncestor(int height) const {
  return const_cast<CBlockIndex *>(this)->GetAncestor(height);
}

// Function used for debugging purposes
std::string CBlockIndex::ToString() const {
  std::ostringstream ss;
  ss << "CBlockIndex("
     << "hash=" << (phashBlock ? phashBlock->ToString().substr(0, 16) : "null")
     << ", height=" << nHeight
     << ", chainwork=0x" << nChainWork.GetHex()
     << ", status=0x" << std::hex << nStatus << std::dec
     << ", version=" << nVersion
     << ", time=" << nTime
     << ", bits=0x" << std::hex << nBits << std::dec
     << ", nonce=" << nNonce
     << ", miner=" << minerAddress.ToString()
     << ", randomx=" << hashRandomX.ToString().substr(0, 16)
     << ", timemax=" << nTimeMax
     << ", pprev=" << pprev
     << ")";
  return ss.str();
}

arith_uint256 GetBlockProof(const CBlockIndex &block) {
  arith_uint256 bnTarget;
  bool fNegative;
  bool fOverflow;
  bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);

  if (fNegative || fOverflow || bnTarget == 0)
    return arith_uint256(0);

  // Defensive: If bnTarget is MAX (all bits set), then bnTarget + 1 wraps to 0
  // Division by 0 is undefined behavior. Real networks won't hit this due to
  // compact encoding limits, but we guard against it anyway.
  if (bnTarget == ~arith_uint256())  // All bits set (MAX value)
    return arith_uint256(1);  // 2^256 / 2^256 = 1

  // We need to compute 2**256 / (bnTarget+1), but we can't represent 2**256
  // as it's too large for an arith_uint256. However, as 2**256 is at least as
  // large as bnTarget+1, it is equal to ((2**256 - bnTarget - 1) /
  // (bnTarget+1)) + 1, or ~bnTarget / (bnTarget+1) + 1.
  return (~bnTarget / (bnTarget + 1)) + 1;
}

const CBlockIndex *LastCommonAncestor(const CBlockIndex *pa,
                                      const CBlockIndex *pb) {
  if (pa == nullptr || pb == nullptr) {
    return nullptr;
  }

  // Bring both to same height
  if (pa->nHeight > pb->nHeight) {
    pa = pa->GetAncestor(pb->nHeight);
  } else if (pb->nHeight > pa->nHeight) {
    pb = pb->GetAncestor(pa->nHeight);
  }

  // Walk backwards until they meet
  while (pa != pb && pa && pb) {
    pa = pa->pprev;
    pb = pb->pprev;
  }

  // Return common ancestor (could be nullptr if chains diverged from different
  // genesis) Caller MUST check for nullptr and handle gracefully This can
  // happen with:
  // - Orphan chains from different networks
  // - Corrupted disk state
  // - Malicious peers sending fake chains
  return pa;
}

} // namespace chain
} // namespace coinbasechain
