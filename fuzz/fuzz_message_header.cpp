// Fuzz target for message header parsing
// Tests parsing of the message header which includes magic bytes, command, length, and checksum

#include "network/message.hpp"
#include "network/protocol.hpp"
#include <cstdint>
#include <cstddef>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace coinbasechain::message;
    using namespace coinbasechain::protocol;

    // Test message header deserialization
    MessageHeader header;
    bool success = deserialize_header(data, size, header);

    if (success) {
        // If deserialization succeeded, test serialization round-trip
        auto serialized = serialize_header(header);

        MessageHeader header2;
        if (deserialize_header(serialized.data(), serialized.size(), header2)) {
            // Verify fields match
            (void)(header.magic == header2.magic);
            (void)(header.command == header2.command);
            (void)(header.length == header2.length);
            (void)(header.checksum == header2.checksum);
        }
    }

    return 0;
}
