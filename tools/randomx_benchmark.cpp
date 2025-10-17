// Quick benchmark to measure RandomX initialization and hashing performance
// Compares light mode vs fast mode

#include <randomx.h>
#include <iostream>
#include <chrono>
#include <vector>
#include <cstring>

using namespace std::chrono;

struct TestBlock {
    uint32_t nVersion;
    uint8_t hashPrevBlock[32];
    uint8_t hashMerkleRoot[32];
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
    uint8_t hashRandomX[32];
};

void print_duration(const char* label, steady_clock::time_point start, steady_clock::time_point end) {
    auto duration = duration_cast<milliseconds>(end - start).count();
    std::cout << label << ": " << duration << " ms (" << (duration / 1000.0) << " s)" << std::endl;
}

int main() {
    std::cout << "RandomX Benchmark - Light Mode vs Fast Mode\n";
    std::cout << "============================================\n\n";

    // Generate a test seed
    const char* seed = "CoinbaseChain/RandomX/Epoch/0";

    randomx_flags flags = randomx_get_flags();
    std::cout << "RandomX flags: 0x" << std::hex << flags << std::dec << std::endl;
    std::cout << "Hardware support: ";
    if (flags & RANDOMX_FLAG_JIT) std::cout << "JIT ";
    if (flags & RANDOMX_FLAG_HARD_AES) std::cout << "AES ";
    if (flags & RANDOMX_FLAG_LARGE_PAGES) std::cout << "HUGEPAGES ";
    std::cout << "\n\n";

    // ==================== LIGHT MODE ====================
    std::cout << "=== LIGHT MODE ===\n";

    auto light_cache_start = steady_clock::now();
    randomx_cache* light_cache = randomx_alloc_cache(flags);
    if (!light_cache) {
        std::cerr << "Failed to allocate light cache\n";
        return 1;
    }
    randomx_init_cache(light_cache, seed, strlen(seed));
    auto light_cache_end = steady_clock::now();
    print_duration("Light cache init", light_cache_start, light_cache_end);

    auto light_vm_start = steady_clock::now();
    randomx_vm* light_vm = randomx_create_vm(flags, light_cache, nullptr);
    if (!light_vm) {
        std::cerr << "Failed to create light VM\n";
        return 1;
    }
    auto light_vm_end = steady_clock::now();
    print_duration("Light VM creation", light_vm_start, light_vm_end);

    // Test light mode hashing (100 blocks)
    TestBlock block = {};
    block.nVersion = 1;
    block.nTime = 1234567890;
    block.nBits = 0x1d00ffff;

    char hash[RANDOMX_HASH_SIZE];

    auto light_hash_start = steady_clock::now();
    for (int i = 0; i < 100; i++) {
        block.nNonce = i;
        randomx_calculate_hash(light_vm, &block, sizeof(block), hash);
    }
    auto light_hash_end = steady_clock::now();
    auto light_hash_duration = duration_cast<milliseconds>(light_hash_end - light_hash_start).count();
    std::cout << "Light mode: 100 hashes in " << light_hash_duration << " ms "
              << "(" << (light_hash_duration / 100.0) << " ms/hash, "
              << (100000.0 / light_hash_duration) << " hash/sec)\n";

    std::cout << "\n";

    // ==================== FAST MODE ====================
    std::cout << "=== FAST MODE ===\n";

    auto fast_cache_start = steady_clock::now();
    randomx_cache* fast_cache = randomx_alloc_cache(flags | RANDOMX_FLAG_FULL_MEM);
    if (!fast_cache) {
        std::cerr << "Failed to allocate fast cache\n";
        return 1;
    }
    randomx_init_cache(fast_cache, seed, strlen(seed));
    auto fast_cache_end = steady_clock::now();
    print_duration("Fast cache init", fast_cache_start, fast_cache_end);

    auto dataset_start = steady_clock::now();
    randomx_dataset* dataset = randomx_alloc_dataset(flags | RANDOMX_FLAG_FULL_MEM);
    if (!dataset) {
        std::cerr << "Failed to allocate dataset\n";
        return 1;
    }
    auto dataset_alloc_end = steady_clock::now();
    print_duration("Dataset allocation", dataset_start, dataset_alloc_end);

    std::cout << "Initializing dataset (this takes a while)...\n";
    auto dataset_init_start = steady_clock::now();
    randomx_init_dataset(dataset, fast_cache, 0, randomx_dataset_item_count());
    auto dataset_init_end = steady_clock::now();
    print_duration("Dataset initialization", dataset_init_start, dataset_init_end);

    auto fast_vm_start = steady_clock::now();
    randomx_vm* fast_vm = randomx_create_vm(flags | RANDOMX_FLAG_FULL_MEM, nullptr, dataset);
    if (!fast_vm) {
        std::cerr << "Failed to create fast VM\n";
        return 1;
    }
    auto fast_vm_end = steady_clock::now();
    print_duration("Fast VM creation", fast_vm_start, fast_vm_end);

    // Test fast mode hashing (100 blocks)
    auto fast_hash_start = steady_clock::now();
    for (int i = 0; i < 100; i++) {
        block.nNonce = i;
        randomx_calculate_hash(fast_vm, &block, sizeof(block), hash);
    }
    auto fast_hash_end = steady_clock::now();
    auto fast_hash_duration = duration_cast<milliseconds>(fast_hash_end - fast_hash_start).count();
    std::cout << "Fast mode: 100 hashes in " << fast_hash_duration << " ms "
              << "(" << (fast_hash_duration / 100.0) << " ms/hash, "
              << (100000.0 / fast_hash_duration) << " hash/sec)\n";

    std::cout << "\n";

    // ==================== SUMMARY ====================
    std::cout << "=== SUMMARY ===\n";
    auto light_total = duration_cast<milliseconds>(light_vm_end - light_cache_start).count();
    auto fast_total = duration_cast<milliseconds>(fast_vm_end - fast_cache_start).count();

    std::cout << "Light mode total setup: " << light_total << " ms (" << (light_total / 1000.0) << " s)\n";
    std::cout << "Fast mode total setup:  " << fast_total << " ms (" << (fast_total / 1000.0) << " s)\n";
    std::cout << "\n";

    double speedup = (double)light_hash_duration / fast_hash_duration;
    std::cout << "Fast mode is " << speedup << "x faster at hashing\n";

    // Calculate break-even point
    double hash_time_saved_per_block = (light_hash_duration / 100.0) - (fast_hash_duration / 100.0);
    int breakeven_blocks = (fast_total - light_total) / hash_time_saved_per_block;
    std::cout << "Break-even point: ~" << breakeven_blocks << " blocks\n";
    std::cout << "(Fast mode becomes worth it after validating " << breakeven_blocks << " blocks)\n";

    // Cleanup
    randomx_destroy_vm(light_vm);
    randomx_release_cache(light_cache);
    randomx_destroy_vm(fast_vm);
    randomx_release_dataset(dataset);
    randomx_release_cache(fast_cache);

    return 0;
}
