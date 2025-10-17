// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#ifndef COINBASECHAIN_CHAIN_CHAIN_HPP
#define COINBASECHAIN_CHAIN_CHAIN_HPP

#include "chain/block_index.hpp"
#include "primitives/block.h"
#include <vector>

namespace coinbasechain {
namespace chain {

/**
 * CChain - An in-memory indexed chain of blocks
 *
 * Represents a single linear chain as a vector of CBlockIndex pointers.
 * Used for the "active chain" (best known chain) and for tracking competing forks.
 *
 * Key properties:
 * - Fast O(1) access by height: chain[height]
 * - Linear vector (not a tree)
 * - Does NOT own the CBlockIndex objects (just pointers)
 *
 */

class CChain {
private:
    std::vector<CBlockIndex*> vChain;

public:
    CChain() = default;

    // Prevent copying (chains should be owned, not copied)
    CChain(const CChain&) = delete;
    CChain& operator=(const CChain&) = delete;

    /**
     * Returns the index entry for the genesis block of this chain, or nullptr if none.
     */
    CBlockIndex* Genesis() const
    {
        return vChain.size() > 0 ? vChain[0] : nullptr;
    }

    /**
     * Returns the index entry for the tip of this chain, or nullptr if none.
     */
    CBlockIndex* Tip() const
    {
        return vChain.size() > 0 ? vChain[vChain.size() - 1] : nullptr;
    }

    /**
     * Returns the index entry at a particular height in this chain, or nullptr if no such height exists.
     */
    CBlockIndex* operator[](int nHeight) const
    {
        if (nHeight < 0 || nHeight >= (int)vChain.size())
            return nullptr;
        return vChain[nHeight];
    }

    /**
     * Efficiently check whether a block is present in this chain.
     */
    bool Contains(const CBlockIndex* pindex) const
    {
        if (!pindex) return false;  // Null check to prevent segfault
        if (pindex->nHeight < 0 || pindex->nHeight >= (int)vChain.size()) {
            return false;  // Height out of bounds
        }
        return vChain[pindex->nHeight] == pindex;
    }

    /**
     * Find the successor of a block in this chain, or nullptr if the given index is not found or is the tip.
     */
    CBlockIndex* Next(const CBlockIndex* pindex) const
    {
        if (Contains(pindex))
            return (*this)[pindex->nHeight + 1];
        else
            return nullptr;
    }

    /**
     * Return the maximal height in the chain.
     * Is equal to chain.Tip() ? chain.Tip()->nHeight : -1.
     */
    int Height() const
    {
        return int(vChain.size()) - 1;
    }

    /**
     * Set/initialize a chain with a given tip.
     * Walks backwards from the tip using pprev to populate the entire vector.
     */
    void SetTip(CBlockIndex& block);

    /**
     * Clear the chain (empty the vector)
     */
    void Clear()
    {
        vChain.clear();
    }

    /**
     * Return a CBlockLocator that refers to the tip of this chain.
     * Used for GETHEADERS messages.
     */
    CBlockLocator GetLocator() const;

    /**
     * Find the last common block between this chain and a block index entry.
     * Returns the fork point.
     */
    const CBlockIndex* FindFork(const CBlockIndex* pindex) const;

    /**
     * Find the earliest block with timestamp equal or greater than the given time
     * and height equal or greater than the given height.
     */
    CBlockIndex* FindEarliestAtLeast(int64_t nTime, int height) const;
};

/**
 * Get a locator for a block index entry.
 * Returns exponentially spaced hashes for efficient sync.
 */
CBlockLocator GetLocator(const CBlockIndex* index);

/**
 * Construct a list of hash entries to put in a locator.
 * Returns hashes at exponentially increasing intervals.
 *
 * Example for height 1000:
 * [1000, 999, 998, 996, 992, 984, 968, 936, 872, 744, 488, 0]
 *  ^     ^    ^    ^    ^    ^    ^    ^    ^    ^    ^    ^
 *  tip   -1   -2   -4   -8  -16  -32  -64 -128 -256 -512  genesis
 */
std::vector<uint256> LocatorEntries(const CBlockIndex* index);

} // namespace chain
} // namespace coinbasechain

#endif // COINBASECHAIN_CHAIN_CHAIN_HPP
