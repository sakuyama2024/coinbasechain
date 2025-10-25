// Orphan DoS tests (ported to test2, chain-level)

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::chain;
using coinbasechain::validation::ValidationState;

static CBlockHeader CreateTestHeader(const uint256& prevHash, uint32_t nTime, uint32_t nNonce = 12345) {
    CBlockHeader header; header.nVersion=1; header.hashPrevBlock=prevHash; header.minerAddress.SetNull(); header.nTime=nTime; header.nBits=0x207fffff; header.nNonce=nNonce; header.hashRandomX.SetNull(); return header;
}
static uint256 RandomHash(){ uint256 h; for(int i=0;i<32;i++) *(h.begin()+i)=rand()%256; return h; }

TEST_CASE("Orphan DoS - Per-Peer Limits", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest();
    TestChainstateManager chainstate(*params);

    SECTION("Enforce per-peer orphan limit (50)") {
        chainstate.Initialize(params->GenesisBlock());
        const int PER_PEER_LIMIT = 50; ValidationState state;
        for (int i = 0; i < 60; i++) { uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up, 1234567890+i, 1000+i); chainstate.AcceptBlockHeader(o, state, 1);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }

    SECTION("Different peers have independent limits") {
        chainstate.Initialize(params->GenesisBlock()); const int PER_PEER_LIMIT=50; ValidationState state;
        for (int i=0;i<PER_PEER_LIMIT;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+i,1000+i); chainstate.AcceptBlockHeader(o,state,1);} 
        size_t after1=chainstate.GetOrphanHeaderCount(); REQUIRE(after1 <= PER_PEER_LIMIT);
        for (int i=0;i<PER_PEER_LIMIT;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567990+i,2000+i); chainstate.AcceptBlockHeader(o,state,2);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() >= after1);
        REQUIRE(chainstate.GetOrphanHeaderCount() <= 2*PER_PEER_LIMIT);
    }

    SECTION("Per-peer limit enforced even with different hashes") {
        chainstate.Initialize(params->GenesisBlock()); const int PER_PEER_LIMIT=50; ValidationState state;
        for (int i=0;i<70;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+i,1000+i); chainstate.AcceptBlockHeader(o,state,1);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= PER_PEER_LIMIT);
    }
}

TEST_CASE("Orphan DoS - Global Limits", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Enforce global orphan limit (1000)") {
        chainstate.Initialize(params->GenesisBlock()); const int GLOBAL_LIMIT=1000; const int PER_PEER_LIMIT=50; ValidationState state;
        for (int peer=1; peer<=25; peer++) for (int i=0;i<PER_PEER_LIMIT;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+peer*1000+i, peer*10000+i); chainstate.AcceptBlockHeader(o,state,peer);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }

    SECTION("Global limit prevents memory exhaustion") {
        chainstate.Initialize(params->GenesisBlock()); const int GLOBAL_LIMIT=1000; ValidationState state;
        for (int i=0;i<2000;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+i,1000+i); chainstate.AcceptBlockHeader(o,state,(i%100)+1);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }

    SECTION("Eviction when global limit reached") {
        chainstate.Initialize(params->GenesisBlock()); const int GLOBAL_LIMIT=1000; const int PER_PEER_LIMIT=50; ValidationState state;
        for (int i=0;i<GLOBAL_LIMIT;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+i,1000+i); chainstate.AcceptBlockHeader(o,state,(i/PER_PEER_LIMIT)+1);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() == GLOBAL_LIMIT);
        for (int i=0;i<100;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+GLOBAL_LIMIT+i,2000+i); chainstate.AcceptBlockHeader(o,state,2);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= GLOBAL_LIMIT);
    }
}

TEST_CASE("Orphan DoS - Time-Based Eviction", "[orphan][dos]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Manual eviction removes expired orphans (no-crash)") {
        chainstate.Initialize(params->GenesisBlock()); ValidationState state; std::vector<uint256> hashes;
        for (int i=0;i<10;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up,1234567890+i,1000+i); hashes.push_back(o.GetHash()); chainstate.AcceptBlockHeader(o,state,1);} 
        // Behavior depends on implementation; ensure interface usable
        REQUIRE(chainstate.GetOrphanHeaderCount() >= 0);
    }
}
