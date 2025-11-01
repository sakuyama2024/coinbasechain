// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

//DTC1

#include "chain/chain.hpp"
#include "util/logging.hpp"
#include <algorithm>

namespace coinbasechain {
namespace chain {

void CChain::SetTip(CBlockIndex &block) {
  CBlockIndex *pindex = &block;

  LOG_CHAIN_TRACE("CChain::SetTip: new_tip={} height={}",
                  pindex->GetBlockHash().ToString().substr(0, 16),
                  pindex->nHeight);

  // Defensive: refuse to operate on negative heights (corrupted input)
  if (pindex->nHeight < 0) {
    LOG_CHAIN_ERROR("CChain::SetTip: negative height {} for block {} - aborting SetTip",
                    pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,16));
    return;
  }

  vChain.resize(static_cast<size_t>(pindex->nHeight) + 1);

  // Walk backwards from tip, filling in the vector
  int blocks_updated = 0;
  while (pindex) {
    // Guard against corrupted heights while walking back
    if (pindex->nHeight < 0) {
      LOG_CHAIN_ERROR("CChain::SetTip: encountered negative height {} while backtracking at block {}",
                      pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,16));
      break;
    }
    size_t idx = static_cast<size_t>(pindex->nHeight);
    if (idx >= vChain.size()) {
      vChain.resize(idx + 1);
    }
    if (vChain[idx] == pindex) {
      break; // already set from a previous walk
    }
    vChain[idx] = pindex;
    pindex = pindex->pprev;
    blocks_updated++;
  }

  LOG_CHAIN_TRACE("CChain::SetTip: Updated {} block entries in chain", blocks_updated);
}

std::vector<uint256> LocatorEntries(const CBlockIndex *index) {
  int step = 1;
  std::vector<uint256> have;

  if (index == nullptr) {
    LOG_CHAIN_TRACE("LocatorEntries: index is null, returning empty");
    return have;
  }

  LOG_CHAIN_TRACE("LocatorEntries: starting from height={}", index->nHeight);
  have.reserve(32);

  while (index) {
    have.emplace_back(index->GetBlockHash());

    if (index->nHeight == 0)
      break;

    // Exponentially larger steps back, plus the genesis block
    int height = std::max(index->nHeight - step, 0);

    // Use GetAncestor to jump back
    index = index->GetAncestor(height);

    // After first 10 entries, double the step size
    if (have.size() > 10)
      step *= 2;
  }

  LOG_CHAIN_TRACE("LocatorEntries: Created locator with {} entries", have.size());
  return have;
}

CBlockLocator GetLocator(const CBlockIndex *index) {
  return CBlockLocator{LocatorEntries(index)};
}

CBlockLocator CChain::GetLocator() const {
  return ::coinbasechain::chain::GetLocator(Tip());
}

const CBlockIndex *CChain::FindFork(const CBlockIndex *pindex) const {
  if (pindex == nullptr) {
    LOG_CHAIN_TRACE("CChain::FindFork: pindex is null, returning null");
    return nullptr;
  }

  LOG_CHAIN_TRACE("CChain::FindFork: comparing against chain (tip_height={}) with pindex height={}",
                  Height(), pindex->nHeight);

  // If pindex is taller than us, bring it down to our height
  if (pindex->nHeight > Height()) {
    LOG_CHAIN_TRACE("CChain::FindFork: pindex is taller, descending from {} to {}",
                    pindex->nHeight, Height());
    pindex = pindex->GetAncestor(Height());
  }

  // Walk backwards until we find a block that's in our chain
  int steps = 0;
  while (pindex && !Contains(pindex)) {
    pindex = pindex->pprev;
    steps++;
  }

  if (pindex) {
    LOG_CHAIN_TRACE("CChain::FindFork: Found fork point at height={} hash={} (walked {} steps)",
                    pindex->nHeight, pindex->GetBlockHash().ToString().substr(0, 16), steps);
  } else {
    LOG_CHAIN_TRACE("CChain::FindFork: No fork point found (walked {} steps back to null)", steps);
  }

  return pindex;
}

CBlockIndex *CChain::FindEarliestAtLeast(int64_t nTime, int height) const {
  std::pair<int64_t, int> blockparams = std::make_pair(nTime, height);

  // Binary search for first block that meets the criteria
  std::vector<CBlockIndex *>::const_iterator lower = std::lower_bound(
      vChain.begin(), vChain.end(), blockparams,
      [](CBlockIndex *pBlock, const std::pair<int64_t, int> &params) -> bool {
        // Core semantics: compare monotonic nTimeMax first, then height
        return pBlock->nTimeMax < params.first || pBlock->nHeight < params.second;
      });

  return (lower == vChain.end() ? nullptr : *lower);
}

} // namespace chain
} // namespace coinbasechain
