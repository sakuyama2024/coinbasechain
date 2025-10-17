// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "chain/chain.hpp"
#include <algorithm>

namespace coinbasechain {
namespace chain {

void CChain::SetTip(CBlockIndex& block)
{
    CBlockIndex* pindex = &block;
    vChain.resize(pindex->nHeight + 1);

    // Walk backwards from tip, filling in the vector
    while (pindex && vChain[pindex->nHeight] != pindex) {
        vChain[pindex->nHeight] = pindex;
        pindex = pindex->pprev;
    }
}

std::vector<uint256> LocatorEntries(const CBlockIndex* index)
{
    int step = 1;
    std::vector<uint256> have;

    if (index == nullptr)
        return have;

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

    return have;
}

CBlockLocator GetLocator(const CBlockIndex* index)
{
    return CBlockLocator{LocatorEntries(index)};
}

CBlockLocator CChain::GetLocator() const
{
    return ::coinbasechain::chain::GetLocator(Tip());
}

const CBlockIndex* CChain::FindFork(const CBlockIndex* pindex) const
{
    if (pindex == nullptr) {
        return nullptr;
    }

    // If pindex is taller than us, bring it down to our height
    if (pindex->nHeight > Height())
        pindex = pindex->GetAncestor(Height());

    // Walk backwards until we find a block that's in our chain
    while (pindex && !Contains(pindex))
        pindex = pindex->pprev;

    return pindex;
}

CBlockIndex* CChain::FindEarliestAtLeast(int64_t nTime, int height) const
{
    std::pair<int64_t, int> blockparams = std::make_pair(nTime, height);

    // Binary search for first block that meets the criteria
    std::vector<CBlockIndex*>::const_iterator lower = std::lower_bound(
        vChain.begin(), vChain.end(), blockparams,
        [](CBlockIndex* pBlock, const std::pair<int64_t, int>& params) -> bool {
            // We need nTimeMax, but we don't cache it
            // For now, just use block time (good enough for headers-only)
            return pBlock->GetBlockTime() < params.first || pBlock->nHeight < params.second;
        });

    return (lower == vChain.end() ? nullptr : *lower);
}

} // namespace chain
} // namespace coinbasechain
