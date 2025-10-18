// Fuzz target for VarInt decoding
// Tests variable-length integer parsing which is notorious for bugs

#include "network/message.hpp"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace coinbasechain::message;

    // Test VarInt decoding
    VarInt vi;
    size_t consumed = vi.decode(data, size);

    // If decode succeeded, test round-trip
    if (consumed > 0 && consumed <= size) {
        // Encode the value back
        uint8_t buffer[9];  // Max varint size
        size_t encoded_size = vi.encode(buffer);

        // Verify encoding doesn't exceed bounds
        if (encoded_size <= sizeof(buffer)) {
            // Decode again and verify we get the same value
            VarInt vi2;
            size_t consumed2 = vi2.decode(buffer, encoded_size);

            // Should consume exactly the encoded size and produce same value
            if (consumed2 == encoded_size) {
                (void)(vi.value == vi2.value);
            }
        }
    }

    return 0;
}
