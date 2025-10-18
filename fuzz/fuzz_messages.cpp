// Fuzz target for network message deserialization
// Tests all message types for crash-free parsing of untrusted network data

#include "network/message.hpp"
#include "network/protocol.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace coinbasechain::message;
    using namespace coinbasechain::protocol;

    if (size < 1) return 0;

    // Use first byte to select message type
    uint8_t msg_type = data[0];
    const uint8_t* payload = data + 1;
    size_t payload_size = size - 1;

    std::unique_ptr<Message> msg;

    // Create message based on type selector
    switch (msg_type % 11) {
        case 0:
            msg = std::make_unique<VersionMessage>();
            break;
        case 1:
            msg = std::make_unique<VerackMessage>();
            break;
        case 2:
            msg = std::make_unique<PingMessage>();
            break;
        case 3:
            msg = std::make_unique<PongMessage>();
            break;
        case 4:
            msg = std::make_unique<AddrMessage>();
            break;
        case 5:
            msg = std::make_unique<GetAddrMessage>();
            break;
        case 6:
            msg = std::make_unique<InvMessage>();
            break;
        case 7:
            msg = std::make_unique<GetDataMessage>();
            break;
        case 8:
            msg = std::make_unique<NotFoundMessage>();
            break;
        case 9:
            msg = std::make_unique<GetHeadersMessage>();
            break;
        case 10:
            msg = std::make_unique<HeadersMessage>();
            break;
    }

    if (!msg) return 0;

    // Test deserialization - should handle any input gracefully
    bool success = msg->deserialize(payload, payload_size);

    // If deserialization succeeded, test serialization round-trip
    if (success) {
        try {
            auto serialized = msg->serialize();

            // Create new message of same type and deserialize
            std::unique_ptr<Message> msg2;
            switch (msg_type % 11) {
                case 0: msg2 = std::make_unique<VersionMessage>(); break;
                case 1: msg2 = std::make_unique<VerackMessage>(); break;
                case 2: msg2 = std::make_unique<PingMessage>(); break;
                case 3: msg2 = std::make_unique<PongMessage>(); break;
                case 4: msg2 = std::make_unique<AddrMessage>(); break;
                case 5: msg2 = std::make_unique<GetAddrMessage>(); break;
                case 6: msg2 = std::make_unique<InvMessage>(); break;
                case 7: msg2 = std::make_unique<GetDataMessage>(); break;
                case 8: msg2 = std::make_unique<NotFoundMessage>(); break;
                case 9: msg2 = std::make_unique<GetHeadersMessage>(); break;
                case 10: msg2 = std::make_unique<HeadersMessage>(); break;
            }

            if (msg2) {
                (void)msg2->deserialize(serialized.data(), serialized.size());
            }
        } catch (...) {
            // Serialization might throw on allocation, that's ok
        }
    }

    return 0;
}
