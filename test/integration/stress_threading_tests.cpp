// Copyright (c) 2024 Coinbase Chain
// Stress tests for threading safety

#include <catch_amalgamated.hpp>
#include "chain/chainstate_manager.hpp"
#include "chain/chainparams.hpp"
#include "chain/block.hpp"
#include "chain/pow.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>

using namespace coinbasechain;

TEST_CASE("Stress test: High concurrency validation", "[stress][threading]") {
    auto params = chain::ChainParams::CreateRegTest();
    validation::ChainstateManager chainstate(*params, 100);
    CBlockHeader genesis = params->GenesisBlock();
    REQUIRE(chainstate.Initialize(genesis));

    // Add genesis as a candidate so ActivateBestChain works
    const auto* genesis_index = chainstate.GetTip();
    REQUIRE(genesis_index != nullptr);
    chainstate.TryAddBlockIndexCandidate(const_cast<chain::CBlockIndex*>(genesis_index));

    SECTION("Hammer GetTip from many threads") {
        // Simulate many RPC threads querying tip simultaneously
        constexpr int NUM_THREADS = 16;
        constexpr int QUERIES_PER_THREAD = 1000;

        std::atomic<int> successful_queries{0};
        std::atomic<int> failed_queries{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < QUERIES_PER_THREAD; j++) {
                    try {
                        const chain::CBlockIndex* tip = chainstate.GetTip();
                        if (tip != nullptr) {
                            successful_queries++;
                            // Access fields to ensure no race on the object itself
                            volatile int height = tip->nHeight;
                            (void)height;
                        } else {
                            failed_queries++;
                        }
                    } catch (...) {
                        failed_queries++;
                    }
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        INFO("Successful queries: " << successful_queries.load());
        INFO("Failed queries: " << failed_queries.load());
        REQUIRE(successful_queries == NUM_THREADS * QUERIES_PER_THREAD);
        REQUIRE(failed_queries == 0);
    }

    SECTION("Mixed reads and writes under load") {
        // Simulate network threads adding blocks while RPC threads query state
        constexpr int NUM_READER_THREADS = 8;
        constexpr int NUM_WRITER_THREADS = 4;
        constexpr int OPERATIONS_PER_THREAD = 100;

        std::atomic<bool> keep_running{true};
        std::atomic<int> read_ops{0};
        std::atomic<int> write_ops{0};
        std::vector<std::thread> threads;

        // Reader threads - continuously query tip
        for (int i = 0; i < NUM_READER_THREADS; i++) {
            threads.emplace_back([&]() {
                while (keep_running) {
                    const chain::CBlockIndex* tip = chainstate.GetTip();
                    if (tip) {
                        read_ops++;
                        volatile int height = tip->nHeight;
                        (void)height;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            });
        }

        // Writer threads - add blocks sequentially (will mostly fail but that's OK)
        for (int i = 0; i < NUM_WRITER_THREADS; i++) {
            threads.emplace_back([&, i]() {
                for (int j = 0; j < OPERATIONS_PER_THREAD; j++) {
                    // Try to add a block
                    CBlockHeader header;
                    header.nVersion = 1;
                    header.nTime = static_cast<uint32_t>(std::time(nullptr)) + i * 1000 + j;
                    header.nBits = params->GenesisBlock().nBits;
                    header.nNonce = 0;

                    const chain::CBlockIndex* tip = chainstate.GetTip();
                    if (tip) {
                        header.hashPrevBlock = tip->GetBlockHash();

                        // Don't mine - just try to accept (will fail PoW but tests locking)
                        validation::ValidationState state;
                        chainstate.AcceptBlockHeader(header, state);
                        write_ops++;
                    }

                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });
        }

        // Let them run for a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        keep_running = false;

        for (auto& t : threads) {
            t.join();
        }

        INFO("Read operations: " << read_ops.load());
        INFO("Write operations: " << write_ops.load());
        REQUIRE(read_ops > 0);
        REQUIRE(write_ops > 0);
    }

    SECTION("Rapid ActivateBestChain calls") {
        // Test that ActivateBestChain can be called concurrently without deadlock
        constexpr int NUM_THREADS = 8;
        constexpr int CALLS_PER_THREAD = 50;

        std::atomic<int> successful_activations{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < CALLS_PER_THREAD; j++) {
                    if (chainstate.ActivateBestChain(nullptr)) {
                        successful_activations++;
                    }
                    // Small delay to allow interleaving
                    std::this_thread::yield();
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        INFO("Successful activations: " << successful_activations.load());
        // All should succeed (even though they don't change anything)
        REQUIRE(successful_activations == NUM_THREADS * CALLS_PER_THREAD);
    }

    SECTION("Chaos test: random operations from many threads") {
        // Randomly mix all operations to create maximum contention
        constexpr int NUM_THREADS = 12;
        constexpr int OPS_PER_THREAD = 200;

        std::atomic<int> total_ops{0};
        std::atomic<bool> any_crash{false};
        std::vector<std::thread> threads;

        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&, i]() {
                std::mt19937 rng(i);
                std::uniform_int_distribution<int> dist(0, 2);

                try {
                    for (int j = 0; j < OPS_PER_THREAD; j++) {
                        int op = dist(rng);

                        switch (op) {
                            case 0: {
                                // GetTip
                                const chain::CBlockIndex* tip = chainstate.GetTip();
                                if (tip) {
                                    volatile int h = tip->nHeight;
                                    (void)h;
                                }
                                break;
                            }
                            case 1: {
                                // ActivateBestChain
                                chainstate.ActivateBestChain(nullptr);
                                break;
                            }
                            case 2: {
                                // Try AcceptBlockHeader (will fail but tests locking)
                                CBlockHeader header;
                                header.nVersion = 1;
                                header.nTime = static_cast<uint32_t>(std::time(nullptr)) + i * 1000 + j;
                                header.nBits = params->GenesisBlock().nBits;
                                header.nNonce = j;

                                const chain::CBlockIndex* tip = chainstate.GetTip();
                                if (tip) {
                                    header.hashPrevBlock = tip->GetBlockHash();
                                    validation::ValidationState state;
                                    chainstate.AcceptBlockHeader(header, state);
                                }
                                break;
                            }
                        }

                        total_ops++;
                    }
                } catch (...) {
                    any_crash = true;
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        INFO("Total operations: " << total_ops.load());
        REQUIRE(!any_crash);
        REQUIRE(total_ops == NUM_THREADS * OPS_PER_THREAD);
    }
}

TEST_CASE("Stress test: Iterator invalidation", "[stress][threading]") {
    // Test that we don't crash from iterator invalidation
    auto params = chain::ChainParams::CreateRegTest();
    validation::ChainstateManager chainstate(*params, 100);
    CBlockHeader genesis = params->GenesisBlock();
    REQUIRE(chainstate.Initialize(genesis));

    // Add genesis as a candidate
    const auto* genesis_index = chainstate.GetTip();
    REQUIRE(genesis_index != nullptr);
    chainstate.TryAddBlockIndexCandidate(const_cast<chain::CBlockIndex*>(genesis_index));

    SECTION("Modify setBlockIndexCandidates while iterating") {
        // This would crash without proper locking
        constexpr int NUM_THREADS = 4;
        std::atomic<bool> keep_running{true};
        std::atomic<int> iterations{0};
        std::vector<std::thread> threads;

        // Thread that modifies candidates
        threads.emplace_back([&]() {
            int count = 0;
            while (keep_running && count < 100) {
                // TryAddBlockIndexCandidate modifies setBlockIndexCandidates
                const chain::CBlockIndex* tip = chainstate.GetTip();
                if (tip) {
                    // Just call it with the tip (no-op but tests locking)
                    chainstate.TryAddBlockIndexCandidate(const_cast<chain::CBlockIndex*>(tip));
                }
                count++;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Threads that read state (would iterate over candidates internally)
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&]() {
                while (keep_running) {
                    // ActivateBestChain calls FindMostWorkChain which iterates setBlockIndexCandidates
                    chainstate.ActivateBestChain(nullptr);
                    iterations++;
                    std::this_thread::yield();
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        keep_running = false;

        for (auto& t : threads) {
            t.join();
        }

        INFO("Iterations: " << iterations.load());
        REQUIRE(iterations > 0);
    }
}

TEST_CASE("Stress test: Long-running concurrent operations", "[stress][threading][.slow]") {
    // Longer test - only run when explicitly requested (use -c to include [.slow] tests)
    auto params = chain::ChainParams::CreateRegTest();
    validation::ChainstateManager chainstate(*params, 100);
    CBlockHeader genesis = params->GenesisBlock();
    REQUIRE(chainstate.Initialize(genesis));

    // Add genesis as a candidate
    const auto* genesis_index = chainstate.GetTip();
    REQUIRE(genesis_index != nullptr);
    chainstate.TryAddBlockIndexCandidate(const_cast<chain::CBlockIndex*>(genesis_index));

    SECTION("Sustained load for 5 seconds") {
        constexpr int NUM_THREADS = 16;
        std::atomic<bool> keep_running{true};
        std::atomic<uint64_t> total_ops{0};
        std::atomic<bool> any_error{false};
        std::vector<std::thread> threads;

        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&, i]() {
                std::mt19937 rng(i);
                std::uniform_int_distribution<int> dist(0, 2);

                while (keep_running) {
                    try {
                        int op = dist(rng);
                        switch (op) {
                            case 0:
                                chainstate.GetTip();
                                break;
                            case 1:
                                chainstate.ActivateBestChain(nullptr);
                                break;
                            case 2: {
                                const chain::CBlockIndex* tip = chainstate.GetTip();
                                if (tip) {
                                    chainstate.TryAddBlockIndexCandidate(const_cast<chain::CBlockIndex*>(tip));
                                }
                                break;
                            }
                        }
                        total_ops++;
                    } catch (...) {
                        any_error = true;
                    }
                }
            });
        }

        // Run for 5 seconds
        std::this_thread::sleep_for(std::chrono::seconds(5));
        keep_running = false;

        for (auto& t : threads) {
            t.join();
        }

        INFO("Total operations in 5 seconds: " << total_ops.load());
        REQUIRE(!any_error);
        REQUIRE(total_ops > 10000); // Should do many ops in 5 seconds
    }
}
