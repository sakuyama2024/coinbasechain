// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "catch_amalgamated.hpp"
#include "util/threadsafe_containers.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace coinbasechain::util;

// ============================================================================
// ThreadSafeMap Tests
// ============================================================================

TEST_CASE("ThreadSafeMap: Basic operations", "[util][threadsafe][map]") {
    ThreadSafeMap<int, std::string> map;

    SECTION("Insert and Get") {
        REQUIRE(map.Insert(1, "one"));
        REQUIRE(map.Get(1).value() == "one");
    }

    SECTION("Insert duplicate returns false and updates") {
        REQUIRE(map.Insert(1, "one"));
        REQUIRE(!map.Insert(1, "ONE"));  // Returns false (updated, not inserted)
        REQUIRE(map.Get(1).value() == "ONE");  // Value updated
    }

    SECTION("TryInsert doesn't overwrite") {
        REQUIRE(map.TryInsert(1, "one"));
        REQUIRE(!map.TryInsert(1, "ONE"));  // Fails, doesn't overwrite
        REQUIRE(map.Get(1).value() == "one");  // Original value preserved
    }

    SECTION("Get non-existent key") {
        REQUIRE(!map.Get(999).has_value());
    }

    SECTION("Contains") {
        map.Insert(1, "one");
        REQUIRE(map.Contains(1));
        REQUIRE(!map.Contains(999));
    }

    SECTION("Size and Empty") {
        REQUIRE(map.Empty());
        REQUIRE(map.Size() == 0);

        map.Insert(1, "one");
        REQUIRE(!map.Empty());
        REQUIRE(map.Size() == 1);

        map.Insert(2, "two");
        REQUIRE(map.Size() == 2);
    }

    SECTION("Erase") {
        map.Insert(1, "one");
        REQUIRE(map.Erase(1));
        REQUIRE(!map.Contains(1));
        REQUIRE(!map.Erase(1));  // Second erase returns false
    }

    SECTION("Clear") {
        map.Insert(1, "one");
        map.Insert(2, "two");
        map.Clear();
        REQUIRE(map.Empty());
        REQUIRE(map.Size() == 0);
    }
}

TEST_CASE("ThreadSafeMap: Advanced operations", "[util][threadsafe][map]") {
    ThreadSafeMap<int, int> map;

    SECTION("GetOrInsert") {
        // Key doesn't exist, inserts and returns default
        int val = map.GetOrInsert(1, 100);
        REQUIRE(val == 100);
        REQUIRE(map.Get(1).value() == 100);

        // Key exists, returns existing value
        val = map.GetOrInsert(1, 999);
        REQUIRE(val == 100);  // Returns existing, not new value
    }

    SECTION("UpdateIf") {
        map.Insert(1, 10);

        // Update succeeds when predicate returns true
        bool updated = map.UpdateIf(1, [](int old_val) { return old_val == 10; }, 20);
        REQUIRE(updated);
        REQUIRE(map.Get(1).value() == 20);

        // Update fails when predicate returns false
        updated = map.UpdateIf(1, [](int old_val) { return old_val == 999; }, 30);
        REQUIRE(!updated);
        REQUIRE(map.Get(1).value() == 20);  // Unchanged

        // Update fails for non-existent key
        updated = map.UpdateIf(999, [](int) { return true; }, 40);
        REQUIRE(!updated);
    }

    SECTION("GetKeys") {
        map.Insert(1, 10);
        map.Insert(2, 20);
        map.Insert(3, 30);

        auto keys = map.GetKeys();
        REQUIRE(keys.size() == 3);

        // Keys should contain 1, 2, 3 (order may vary)
        std::sort(keys.begin(), keys.end());
        REQUIRE(keys[0] == 1);
        REQUIRE(keys[1] == 2);
        REQUIRE(keys[2] == 3);
    }

    SECTION("GetAll") {
        map.Insert(1, 10);
        map.Insert(2, 20);
        map.Insert(3, 30);

        auto entries = map.GetAll();
        REQUIRE(entries.size() == 3);

        // Sort by key for deterministic testing
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        REQUIRE(entries[0].first == 1);
        REQUIRE(entries[0].second == 10);
        REQUIRE(entries[1].first == 2);
        REQUIRE(entries[1].second == 20);
        REQUIRE(entries[2].first == 3);
        REQUIRE(entries[2].second == 30);
    }

    SECTION("ForEach") {
        map.Insert(1, 10);
        map.Insert(2, 20);
        map.Insert(3, 30);

        int sum = 0;
        map.ForEach([&sum](int key, int value) {
            sum += value;
        });
        REQUIRE(sum == 60);
    }
}

TEST_CASE("ThreadSafeMap: Concurrent access", "[util][threadsafe][map][concurrent]") {
    ThreadSafeMap<int, int> map;
    constexpr int num_threads = 10;
    constexpr int ops_per_thread = 100;

    SECTION("Concurrent inserts") {
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&map, t]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    int key = t * ops_per_thread + i;
                    map.Insert(key, key * 10);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // Verify all entries inserted
        REQUIRE(map.Size() == num_threads * ops_per_thread);

        // Verify some random values
        REQUIRE(map.Get(0).value() == 0);
        REQUIRE(map.Get(50).value() == 500);
        REQUIRE(map.Get(999).value() == 9990);
    }

    SECTION("Concurrent reads and writes") {
        // Pre-populate
        for (int i = 0; i < 100; ++i) {
            map.Insert(i, i);
        }

        std::atomic<int> read_sum{0};
        std::vector<std::thread> threads;

        // Reader threads
        for (int t = 0; t < num_threads / 2; ++t) {
            threads.emplace_back([&map, &read_sum]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    auto val = map.Get(i % 100);
                    if (val) {
                        read_sum += *val;
                    }
                }
            });
        }

        // Writer threads
        for (int t = 0; t < num_threads / 2; ++t) {
            threads.emplace_back([&map]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    map.Insert(i % 100, i);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // No crashes = success for thread safety
        REQUIRE(map.Size() == 100);
    }

    SECTION("Concurrent erases") {
        // Pre-populate
        for (int i = 0; i < 1000; ++i) {
            map.Insert(i, i);
        }

        std::atomic<int> erase_count{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&map, &erase_count, t]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    int key = t * ops_per_thread + i;
                    if (map.Erase(key)) {
                        erase_count++;
                    }
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // All erases should succeed (each thread erases unique keys)
        REQUIRE(erase_count == num_threads * ops_per_thread);
        REQUIRE(map.Empty());
    }
}

// ============================================================================
// ThreadSafeSet Tests
// ============================================================================

TEST_CASE("ThreadSafeSet: Basic operations", "[util][threadsafe][set]") {
    ThreadSafeSet<int> set;

    SECTION("Insert and Contains") {
        REQUIRE(set.Insert(1));
        REQUIRE(set.Contains(1));
    }

    SECTION("Insert duplicate") {
        REQUIRE(set.Insert(1));
        REQUIRE(!set.Insert(1));  // Returns false
        REQUIRE(set.Contains(1));
    }

    SECTION("Contains non-existent") {
        REQUIRE(!set.Contains(999));
    }

    SECTION("Size and Empty") {
        REQUIRE(set.Empty());
        REQUIRE(set.Size() == 0);

        set.Insert(1);
        REQUIRE(!set.Empty());
        REQUIRE(set.Size() == 1);

        set.Insert(2);
        REQUIRE(set.Size() == 2);

        set.Insert(2);  // Duplicate
        REQUIRE(set.Size() == 2);  // Size unchanged
    }

    SECTION("Erase") {
        set.Insert(1);
        REQUIRE(set.Erase(1));
        REQUIRE(!set.Contains(1));
        REQUIRE(!set.Erase(1));  // Second erase returns false
    }

    SECTION("Clear") {
        set.Insert(1);
        set.Insert(2);
        set.Clear();
        REQUIRE(set.Empty());
        REQUIRE(set.Size() == 0);
    }
}

TEST_CASE("ThreadSafeSet: Iteration", "[util][threadsafe][set]") {
    ThreadSafeSet<int> set;
    set.Insert(1);
    set.Insert(2);
    set.Insert(3);

    SECTION("GetAll") {
        auto elements = set.GetAll();
        REQUIRE(elements.size() == 3);

        // Sort for deterministic testing
        std::sort(elements.begin(), elements.end());
        REQUIRE(elements[0] == 1);
        REQUIRE(elements[1] == 2);
        REQUIRE(elements[2] == 3);
    }

    SECTION("ForEach") {
        int sum = 0;
        set.ForEach([&sum](int value) {
            sum += value;
        });
        REQUIRE(sum == 6);
    }
}

TEST_CASE("ThreadSafeSet: Concurrent access", "[util][threadsafe][set][concurrent]") {
    ThreadSafeSet<int> set;
    constexpr int num_threads = 10;
    constexpr int ops_per_thread = 100;

    SECTION("Concurrent inserts") {
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&set, t]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    int value = t * ops_per_thread + i;
                    set.Insert(value);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // Verify all entries inserted
        REQUIRE(set.Size() == num_threads * ops_per_thread);

        // Verify some random values
        REQUIRE(set.Contains(0));
        REQUIRE(set.Contains(500));
        REQUIRE(set.Contains(999));
    }

    SECTION("Concurrent reads and writes") {
        // Pre-populate
        for (int i = 0; i < 100; ++i) {
            set.Insert(i);
        }

        std::atomic<int> contains_count{0};
        std::vector<std::thread> threads;

        // Reader threads
        for (int t = 0; t < num_threads / 2; ++t) {
            threads.emplace_back([&set, &contains_count]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    if (set.Contains(i % 100)) {
                        contains_count++;
                    }
                }
            });
        }

        // Writer threads
        for (int t = 0; t < num_threads / 2; ++t) {
            threads.emplace_back([&set]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    set.Insert(100 + (i % 50));
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // No crashes = success for thread safety
        REQUIRE(set.Size() == 150);  // 100 original + 50 new unique
    }

    SECTION("Concurrent erases") {
        // Pre-populate
        for (int i = 0; i < 1000; ++i) {
            set.Insert(i);
        }

        std::atomic<int> erase_count{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&set, &erase_count, t]() {
                for (int i = 0; i < ops_per_thread; ++i) {
                    int value = t * ops_per_thread + i;
                    if (set.Erase(value)) {
                        erase_count++;
                    }
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        // All erases should succeed (each thread erases unique values)
        REQUIRE(erase_count == num_threads * ops_per_thread);
        REQUIRE(set.Empty());
    }
}

// ============================================================================
// Edge Cases and Special Scenarios
// ============================================================================

TEST_CASE("ThreadSafeMap: Complex value types", "[util][threadsafe][map]") {
    struct ComplexValue {
        int id;
        std::string name;
        std::vector<int> data;

        bool operator==(const ComplexValue& other) const {
            return id == other.id && name == other.name && data == other.data;
        }
    };

    ThreadSafeMap<int, ComplexValue> map;

    ComplexValue val1{1, "test", {1, 2, 3}};
    map.Insert(1, val1);

    auto retrieved = map.Get(1);
    REQUIRE(retrieved.has_value());
    REQUIRE(*retrieved == val1);
}
