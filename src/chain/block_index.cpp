// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/block_index.hpp"
#include "arith_uint256.h"
#include <iomanip>
#include <sstream>

namespace coinbasechain {
namespace chain {

std::string CBlockIndex::ToString() const {
  std::ostringstream ss;
  ss << "CBlockIndex(pprev=" << pprev << ", nHeight=" << nHeight
     << ", minerAddress=" << minerAddress.ToString()
     << ", hashBlock=" << (phashBlock ? phashBlock->ToString() : "null") << ")";
  return ss.str();
}

arith_uint256 GetBlockProof(const CBlockIndex &block) {
  arith_uint256 bnTarget;
  bool fNegative;
  bool fOverflow;
  bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);

  if (fNegative || fOverflow || bnTarget == 0)
    return arith_uint256(0);

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
