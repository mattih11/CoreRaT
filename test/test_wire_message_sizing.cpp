/**
 * @file test_wire_message_sizing.cpp
 * @brief Test compile-time buffer sizing for WireMessage<T>
 *
 * Adapted from CommRaT test_mailbox_sizing.cpp.
 * Uses WireMessage<T> instead of TimsMessage<T>.
 * No GetData overhead (CommRaT adds that on top).
 */

#include "corerat/messaging/message_id.hpp"
#include "corerat/messaging/message_registry.hpp"
#include "corerat/messaging/wire_message.hpp"
#include <sertial/sertial.hpp>
#include <iostream>
#include <cassert>
#include <array>

using namespace corerat;

// Test payload types
struct TinyCmd    { uint8_t  value; };
struct SmallCmd   { uint32_t value; };
struct MediumData { std::array<float, 32> values; };   // 128-byte payload
struct LargeData  { std::array<uint8_t, 3000> buffer; };

using TestRegistry = MessageRegistry<
    MessageDefinition<TinyCmd,    MessagePrefix::UserDefined, UserSubPrefix::Commands>,
    MessageDefinition<SmallCmd,   MessagePrefix::UserDefined, UserSubPrefix::Commands>,
    MessageDefinition<MediumData, MessagePrefix::UserDefined, UserSubPrefix::Data>,
    MessageDefinition<LargeData,  MessagePrefix::UserDefined, UserSubPrefix::Data>
>;

int main() {
    std::cout << "=== CoreRaT WireMessage buffer sizing test ===\n\n";

    // Individual sizes (WireHeader + payload, serialised)
    constexpr size_t tiny_size   = sertial::Message<WireMessage<TinyCmd>>::max_buffer_size;
    constexpr size_t small_size  = sertial::Message<WireMessage<SmallCmd>>::max_buffer_size;
    constexpr size_t medium_size = sertial::Message<WireMessage<MediumData>>::max_buffer_size;
    constexpr size_t large_size  = sertial::Message<WireMessage<LargeData>>::max_buffer_size;

    std::cout << "Individual serialised sizes (WireHeader + payload):\n"
              << "  TinyCmd:    " << tiny_size   << " bytes\n"
              << "  SmallCmd:   " << small_size  << " bytes\n"
              << "  MediumData: " << medium_size << " bytes\n"
              << "  LargeData:  " << large_size  << " bytes\n\n";

    // WireHeader: 2×uint32 + uint64 + 4×uint32 = 4+4+8+4+4+4+4 = 32 bytes
    static_assert(sizeof(WireHeader) == 32, "WireHeader size mismatch");
    std::cout << "  WireHeader size: " << sizeof(WireHeader) << " bytes (expected 32): PASS\n\n";

    // Registry max_message_size = largest WireMessage in the registry (no GetData expansion)
    constexpr size_t registry_max = TestRegistry::max_message_size;
    assert(registry_max == large_size);
    std::cout << "Registry::max_message_size: " << registry_max
              << " bytes (matches LargeData): PASS\n\n";

    // max_size_for_types<> — subset optimisation
    constexpr size_t cmd_max = TestRegistry::max_size_for_types<TinyCmd, SmallCmd>();
    assert(cmd_max == small_size);
    assert(cmd_max < medium_size);
    std::cout << "max_size_for_types<TinyCmd, SmallCmd>: " << cmd_max
              << " bytes (matches SmallCmd): PASS\n";
    std::cout << "  Memory saving vs registry_max: "
              << (registry_max - cmd_max) << " bytes ("
              << (100.0 * (registry_max - cmd_max) / registry_max) << "%)\n\n";

    constexpr size_t tiny_only = TestRegistry::max_size_for_types<TinyCmd>();
    assert(tiny_only == tiny_size);
    std::cout << "max_size_for_types<TinyCmd>: " << tiny_only
              << " bytes (matches TinyCmd): PASS\n";
    std::cout << "  Memory saving vs registry_max: "
              << (registry_max - tiny_only) << " bytes ("
              << (100.0 * (registry_max - tiny_only) / registry_max) << "%)\n\n";

    constexpr size_t data_max = TestRegistry::max_size_for_types<MediumData>();
    assert(data_max == medium_size);
    std::cout << "max_size_for_types<MediumData>: " << data_max
              << " bytes (matches MediumData): PASS\n\n";

    // is_registered checks
    static_assert( TestRegistry::is_registered<TinyCmd>,    "TinyCmd not registered");
    static_assert( TestRegistry::is_registered<LargeData>,  "LargeData not registered");
    std::cout << "is_registered<T>: PASS\n\n";

    std::cout << "=== ALL TESTS PASSED ===\n";
    return 0;
}
