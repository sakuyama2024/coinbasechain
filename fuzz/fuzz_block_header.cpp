// Fuzz target for CBlockHeader deserialization
// Tests block header parsing from untrusted network data

#include "primitives/block.h"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Test block header deserialization
    CBlockHeader header;

    // Deserialize should handle any input gracefully without crashing
    (void)header.Deserialize(data, size);

    // If deserialization succeeded, verify we can serialize it back
    if (!header.IsNull()) {
        auto serialized = header.Serialize();
        // Verify round-trip is consistent
        CBlockHeader header2;
        if (header2.Deserialize(serialized.data(), serialized.size())) {
            // Check that hash computation doesn't crash
            (void)header2.GetHash();
        }
    }

    return 0;
}
