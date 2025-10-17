// Example usage and test
#include <iostream>

#include "uint.hpp"

int main() {
    // Test basic construction
    uint256 zero;
    uint256 one{1};

    std::cout << "Zero: " << zero.GetHex() << "\n";
    std::cout << "One: " << one.GetHex() << "\n";
    std::cout << "Is zero null? " << zero.IsNull() << "\n";
    std::cout << "Is one null? " << one.IsNull() << "\n";

    // Test hex conversion
    auto hash = uint256S("deadbeef");
    std::cout << "Hash: " << hash.GetHex() << "\n";

    // Test comparison with spaceship operator
    std::cout << "zero == one? " << (zero == one) << "\n";
    std::cout << "zero < one? " << (zero < one) << "\n";

    // Test with full 256-bit value
    auto full = uint256S("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    std::cout << "Full hash: " << full.GetHex() << "\n";

    // Test reading/writing integers at different positions
    uint256 test;
    test.SetUint64(0, 0xDEADBEEFCAFEBABE);
    test.SetUint32(2, 0x12345678);

    std::cout << "Test value after sets: " << test.GetHex() << "\n";
    std::cout << "Read uint64 at 0: " << std::hex << test.GetUint64(0) << "\n";
    std::cout << "Read uint32 at 2: " << std::hex << test.GetUint32(2) << "\n";

    return 0;
}
