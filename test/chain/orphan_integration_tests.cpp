// Orphan integration tests (ported to test2, chain-level)

#include "catch_amalgamated.hpp"
#include "test_chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/block_index.hpp"

using namespace coinbasechain;
using namespace coinbasechain::test;
using namespace coinbasechain::chain;
using coinbasechain::validation::ValidationState;

static CBlockHeader CreateTestHeader(const uint256& prevHash, uint32_t nTime, uint32_t nNonce = 12345) {
    CBlockHeader header; header.nVersion=1; header.hashPrevBlock=prevHash; header.minerAddress.SetNull(); header.nTime=nTime; header.nBits=0x207fffff; header.nNonce=nNonce; header.hashRandomX.SetNull(); return header;
}
static uint256 RandomHash(){ uint256 h; for(int i=0;i<32;i++) *(h.begin()+i)=rand()%256; return h; }

TEST_CASE("Orphan Integration - Multi-Peer Scenarios", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Two peers send competing orphan chains") {
        chainstate.Initialize(params->GenesisBlock()); const auto& genesis=params->GenesisBlock();
        CBlockHeader A1=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000); CBlockHeader A2=CreateTestHeader(A1.GetHash(), genesis.nTime+240,1001);
        CBlockHeader B1=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,2000); CBlockHeader B2=CreateTestHeader(B1.GetHash(), genesis.nTime+240,2001);
        ValidationState st; chainstate.AcceptBlockHeader(A2, st, 1); chainstate.AcceptBlockHeader(B2, st, 2); REQUIRE(chainstate.GetOrphanHeaderCount()==2);
        chainstate.AcceptBlockHeader(A1, st, 1); chainstate.AcceptBlockHeader(B1, st, 2);
        REQUIRE(chainstate.GetOrphanHeaderCount()==0);
        REQUIRE(chainstate.LookupBlockIndex(A1.GetHash())!=nullptr); REQUIRE(chainstate.LookupBlockIndex(A2.GetHash())!=nullptr); REQUIRE(chainstate.LookupBlockIndex(B1.GetHash())!=nullptr); REQUIRE(chainstate.LookupBlockIndex(B2.GetHash())!=nullptr);
    }

    SECTION("Multiple peers contribute to same orphan chain") {
        chainstate.Initialize(params->GenesisBlock()); const auto& genesis=params->GenesisBlock();
        CBlockHeader A=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000); uint256 hA=A.GetHash(); CBlockHeader B=CreateTestHeader(hA, genesis.nTime+240,1001); uint256 hB=B.GetHash(); CBlockHeader C=CreateTestHeader(hB, genesis.nTime+360,1002); uint256 hC=C.GetHash(); CBlockHeader D=CreateTestHeader(hC, genesis.nTime+480,1003);
        ValidationState st; chainstate.AcceptBlockHeader(D, st, 4); chainstate.AcceptBlockHeader(B, st, 2); chainstate.AcceptBlockHeader(C, st, 3); REQUIRE(chainstate.GetOrphanHeaderCount()==3);
        chainstate.AcceptBlockHeader(A, st, 1); REQUIRE(chainstate.GetOrphanHeaderCount()==0);
        REQUIRE(chainstate.LookupBlockIndex(hA)!=nullptr); REQUIRE(chainstate.LookupBlockIndex(hB)!=nullptr); REQUIRE(chainstate.LookupBlockIndex(hC)!=nullptr); REQUIRE(chainstate.LookupBlockIndex(D.GetHash())!=nullptr);
    }

    SECTION("Peer spamming orphans while legitimate chain progresses") {
        chainstate.Initialize(params->GenesisBlock()); const auto& genesis=params->GenesisBlock(); ValidationState st; CBlockHeader prev=genesis;
        for(int i=0;i<20;i++){ CBlockHeader next=CreateTestHeader(prev.GetHash(), prev.nTime+120,1000+i); chain::CBlockIndex* p=chainstate.AcceptBlockHeader(next, st, 1); if(p){ chainstate.TryAddBlockIndexCandidate(p); chainstate.ActivateBestChain(); } prev=next; }
        size_t validHeight=chainstate.GetChainHeight(); REQUIRE(validHeight==20);
        for(int i=0;i<100;i++){ uint256 up=RandomHash(); CBlockHeader o=CreateTestHeader(up, 1234567890+i, 2000+i); chainstate.AcceptBlockHeader(o, st, 2);} 
        REQUIRE(chainstate.GetOrphanHeaderCount() <= 50);
        REQUIRE(chainstate.GetChainHeight() == validHeight);
    }
}

TEST_CASE("Orphan Integration - Reorg Scenarios", "[orphan][integration]") {
    auto params = ChainParams::CreateRegTest(); TestChainstateManager chainstate(*params);

    SECTION("Orphan chain with more work triggers reorg") {
        chainstate.Initialize(params->GenesisBlock()); const auto& genesis=params->GenesisBlock();
        CBlockHeader A=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,1000); A.nBits=0x207fffff; ValidationState st; chain::CBlockIndex* pA=chainstate.AcceptBlockHeader(A, st, 1); if(pA){ chainstate.TryAddBlockIndexCandidate(pA); chainstate.ActivateBestChain(); }
        REQUIRE(chainstate.GetChainHeight()==1);
        CBlockHeader B1=CreateTestHeader(genesis.GetHash(), genesis.nTime+120,2000); B1.nBits=0x207fffff; uint256 hB1=B1.GetHash(); CBlockHeader B2=CreateTestHeader(hB1, genesis.nTime+240,2001); B2.nBits=0x207fffff; chainstate.AcceptBlockHeader(B2, st, 2); REQUIRE(chainstate.GetOrphanHeaderCount()==1);
        chain::CBlockIndex* pB1=chainstate.AcceptBlockHeader(B1, st, 2); if(pB1) chainstate.TryAddBlockIndexCandidate(pB1); chain::CBlockIndex* pB2=chainstate.LookupBlockIndex(B2.GetHash()); if(pB2) chainstate.TryAddBlockIndexCandidate(pB2); chainstate.ActivateBestChain();
        REQUIRE(chainstate.GetOrphanHeaderCount()==0);
    }
}
