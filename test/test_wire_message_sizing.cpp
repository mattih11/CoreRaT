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
#include <corerat/logging/logging.hpp>
#include <cassert>
#include <array>

using namespace corerat;

static corerat::TerminalSink g_sink{};
static corerat::RtLogger<64> g_logger{0x00000003u, corerat::LogLevel::Trace};

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
    g_logger.add_sink(&g_sink);
    g_logger.start_drain();

    RTLOG_INFO(g_logger) << "=== CoreRaT WireMessage buffer sizing test ===";

    // Individual sizes (WireHeader + payload, serialised)
    constexpr size_t tiny_size   = sertial::Message<WireMessage<TinyCmd>>::max_buffer_size;
    constexpr size_t small_size  = sertial::Message<WireMessage<SmallCmd>>::max_buffer_size;
    constexpr size_t medium_size = sertial::Message<WireMessage<MediumData>>::max_buffer_size;
    constexpr size_t large_size  = sertial::Message<WireMessage<LargeData>>::max_buffer_size;

    RTLOG_INFO(g_logger) << "Individual serialised sizes (WireHeader + payload):";
    RTLOG_INFO(g_logger) << "  TinyCmd:    " << static_cast<uint64_t>(tiny_size)   << " bytes";
    RTLOG_INFO(g_logger) << "  SmallCmd:   " << static_cast<uint64_t>(small_size)  << " bytes";
    RTLOG_INFO(g_logger) << "  MediumData: " << static_cast<uint64_t>(medium_size) << " bytes";
    RTLOG_INFO(g_logger) << "  LargeData:  " << static_cast<uint64_t>(large_size)  << " bytes";

    // WireHeader: 2×uint32 + uint64 + 4×uint32 = 4+4+8+4+4+4+4 = 32 bytes
    static_assert(sizeof(WireHeader) == 32, "WireHeader size mismatch");
    RTLOG_INFO(g_logger) << "  WireHeader size: " << static_cast<uint64_t>(sizeof(WireHeader)) << " bytes (expected 32): PASS";

    // Registry max_message_size = largest WireMessage in the registry (no GetData expansion)
    constexpr size_t registry_max = TestRegistry::max_message_size;
    assert(registry_max == large_size);
    RTLOG_INFO(g_logger) << "Registry::max_message_size: " << static_cast<uint64_t>(registry_max) << " bytes (matches LargeData): PASS";

    // max_size_for_types<> — subset optimisation
    constexpr size_t cmd_max = TestRegistry::max_size_for_types<TinyCmd, SmallCmd>();
    assert(cmd_max == small_size);
    assert(cmd_max < medium_size);
    RTLOG_INFO(g_logger) << "max_size_for_types<TinyCmd, SmallCmd>: " << static_cast<uint64_t>(cmd_max) << " bytes (matches SmallCmd): PASS";
    RTLOG_INFO(g_logger) << "  Memory saving vs registry_max: " << static_cast<uint64_t>(registry_max - cmd_max) << " bytes (" << (100.0 * static_cast<double>(registry_max - cmd_max) / static_cast<double>(registry_max)) << "%)";

    constexpr size_t tiny_only = TestRegistry::max_size_for_types<TinyCmd>();
    assert(tiny_only == tiny_size);
    RTLOG_INFO(g_logger) << "max_size_for_types<TinyCmd>: " << static_cast<uint64_t>(tiny_only) << " bytes (matches TinyCmd): PASS";
    RTLOG_INFO(g_logger) << "  Memory saving vs registry_max: " << static_cast<uint64_t>(registry_max - tiny_only) << " bytes (" << (100.0 * static_cast<double>(registry_max - tiny_only) / static_cast<double>(registry_max)) << "%)";

    constexpr size_t data_max = TestRegistry::max_size_for_types<MediumData>();
    assert(data_max == medium_size);
    RTLOG_INFO(g_logger) << "max_size_for_types<MediumData>: " << static_cast<uint64_t>(data_max) << " bytes (matches MediumData): PASS";

    // is_registered checks
    static_assert( TestRegistry::is_registered<TinyCmd>,    "TinyCmd not registered");
    static_assert( TestRegistry::is_registered<LargeData>,  "LargeData not registered");
    RTLOG_INFO(g_logger) << "is_registered<T>: PASS";
    RTLOG_INFO(g_logger) << "=== ALL TESTS PASSED ===";
    g_logger.stop_drain();
    return 0;
}
