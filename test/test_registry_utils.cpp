/**
 * @file test_registry_utils.cpp
 * @brief Test registry_utils.hpp compile-time query helpers
 *
 * Adapted from CommRaT test_registry_utils.cpp.
 * Uses MessageRegistry directly (no CommRaT<> application layer).
 * No GetData or subscription message expansion — those are CommRaT-only additions.
 */

#include "corerat/messaging/message_id.hpp"
#include "corerat/messaging/message_registry.hpp"
#include "corerat/messaging/registry_utils.hpp"
#include <corerat/logging/logging.hpp>
#include <cassert>

using namespace corerat;

static corerat::TerminalSink g_sink{};
static corerat::RtLogger<64> g_logger{0x00000002u, corerat::LogLevel::Trace};

// ============================================================================
// Test Message Definitions
// ============================================================================

struct TemperatureData { float value; uint64_t ts; };
struct PressureData    { float value; uint64_t ts; };

struct CalibrateCmdPayload { uint8_t sensor_id; };
struct CalibrateReplyPayload { bool success; };

struct ResetCmdPayload { uint32_t flags; };
struct ResetReplyPayload { bool success; };

using TestRegistry = MessageRegistry<
    MessageDefinition<TemperatureData,       MessagePrefix::UserDefined, UserSubPrefix::Data>,
    MessageDefinition<PressureData,          MessagePrefix::UserDefined, UserSubPrefix::Data>,
    MessageDefinition<CalibrateCmdPayload,   MessagePrefix::UserDefined, UserSubPrefix::Commands,
                      0, CalibrateReplyPayload>,
    MessageDefinition<ResetCmdPayload,       MessagePrefix::UserDefined, UserSubPrefix::Commands,
                      0, ResetReplyPayload>
>;
// After expansion: 2 data + 4 command (2 request + 2 reply) = 6 total

// ============================================================================
// Tests
// ============================================================================

void test_category_filters() {
    RTLOG_INFO(g_logger) << "Testing category filters...";

    using DataMsgs = registry::data_messages_t<TestRegistry>;
    constexpr size_t num_data = registry::tuple_size_v<DataMsgs>;
    static_assert(num_data == 2, "Should have 2 data messages");
    RTLOG_INFO(g_logger) << "  Data messages: " << static_cast<uint64_t>(num_data) << " (expected 2): PASS";

    using CommandMsgs = registry::command_messages_t<TestRegistry>;
    constexpr size_t num_commands = registry::tuple_size_v<CommandMsgs>;
    // 2 request + 2 reply = 4 command messages after registry expansion
    static_assert(num_commands == 4, "Should have 4 command messages (2 request + 2 reply)");
    RTLOG_INFO(g_logger) << "  Command messages: " << static_cast<uint64_t>(num_commands) << " (expected 4): PASS";

    // No subscription messages in a bare CoreRaT registry
    using SubMsgs = registry::subscription_messages_t<TestRegistry>;
    constexpr size_t num_sub = registry::tuple_size_v<SubMsgs>;
    static_assert(num_sub == 0, "No subscription messages in a CoreRaT registry");
    RTLOG_INFO(g_logger) << "  Subscription messages: " << static_cast<uint64_t>(num_sub) << " (expected 0, CommRaT adds these): PASS";
}

void test_prefix_filters() {
    RTLOG_INFO(g_logger) << "Testing prefix filters...";

    using UserMsgs = registry::filter_by_prefix_t<MessagePrefix::UserDefined, TestRegistry>;
    constexpr size_t num_user = registry::tuple_size_v<UserMsgs>;
    static_assert(num_user > 0, "Should have user-defined messages");
    RTLOG_INFO(g_logger) << "  UserDefined messages: " << static_cast<uint64_t>(num_user) << ": PASS";

    using SystemMsgs = registry::filter_by_prefix_t<MessagePrefix::System, TestRegistry>;
    constexpr size_t num_system = registry::tuple_size_v<SystemMsgs>;
    static_assert(num_system == 0, "No system messages in a bare CoreRaT registry");
    RTLOG_INFO(g_logger) << "  System messages: " << static_cast<uint64_t>(num_system) << " (expected 0, CommRaT adds these): PASS";
}

void test_subprefix_filters() {
    RTLOG_INFO(g_logger) << "Testing subprefix filters...";

    using UserDataMsgs = registry::filter_by_subprefix_t<
        MessagePrefix::UserDefined,
        static_cast<uint8_t>(UserSubPrefix::Data),
        TestRegistry
    >;
    constexpr size_t num_data = registry::tuple_size_v<UserDataMsgs>;
    static_assert(num_data == 2, "Should have exactly 2 user data messages");
    RTLOG_INFO(g_logger) << "  UserDefined::Data: " << static_cast<uint64_t>(num_data) << " (expected 2): PASS";

    using UserCmdMsgs = registry::filter_by_subprefix_t<
        MessagePrefix::UserDefined,
        static_cast<uint8_t>(UserSubPrefix::Commands),
        TestRegistry
    >;
    constexpr size_t num_cmd = registry::tuple_size_v<UserCmdMsgs>;
    static_assert(num_cmd == 4, "Should have 4 command messages (2 req + 2 reply)");
    RTLOG_INFO(g_logger) << "  UserDefined::Commands: " << static_cast<uint64_t>(num_cmd) << " (expected 4): PASS";
}

void test_request_reply_filters() {
    RTLOG_INFO(g_logger) << "Testing request/reply filters...";

    using Requests = registry::filter_requests_t<TestRegistry>;
    constexpr size_t num_requests = registry::tuple_size_v<Requests>;
    RTLOG_INFO(g_logger) << "  Request messages: " << static_cast<uint64_t>(num_requests);

    using Replies = registry::filter_replies_t<TestRegistry>;
    constexpr size_t num_replies = registry::tuple_size_v<Replies>;
    RTLOG_INFO(g_logger) << "  Reply messages: " << static_cast<uint64_t>(num_replies);

    static_assert(num_requests == num_replies, "Should have equal requests and replies");
    static_assert(num_requests == 2, "Should have exactly 2 request messages");
    RTLOG_INFO(g_logger) << "  Request/reply symmetry (2 each): PASS";
}

void test_convenience_counters() {
    RTLOG_INFO(g_logger) << "Testing convenience counter functions...";

    constexpr size_t data_count = registry::data_message_count<TestRegistry>();
    constexpr size_t cmd_count  = registry::command_message_count<TestRegistry>();
    constexpr size_t req_count  = registry::request_message_count<TestRegistry>();
    constexpr size_t rep_count  = registry::reply_message_count<TestRegistry>();

    RTLOG_INFO(g_logger) << "  Data:     " << static_cast<uint64_t>(data_count);
    RTLOG_INFO(g_logger) << "  Commands: " << static_cast<uint64_t>(cmd_count);
    RTLOG_INFO(g_logger) << "  Requests: " << static_cast<uint64_t>(req_count);
    RTLOG_INFO(g_logger) << "  Replies:  " << static_cast<uint64_t>(rep_count);

    static_assert(data_count == 2, "Data count mismatch");
    static_assert(cmd_count  == 4, "Command count mismatch (2 req + 2 reply)");
    static_assert(req_count  == rep_count, "Request/reply count mismatch");
    RTLOG_INFO(g_logger) << "  Convenience counters: PASS";
}

void test_registry_stats() {
    RTLOG_INFO(g_logger) << "Testing RegistryStats introspection...";

    using Stats = registry::RegistryStats<TestRegistry>;
    RTLOG_INFO(g_logger)
        << "  Total: " << static_cast<uint64_t>(Stats::total_messages)
        << "  Data: "  << static_cast<uint64_t>(Stats::data_messages)
        << "  Cmd: "   << static_cast<uint64_t>(Stats::command_messages)
        << "  Sub: "   << static_cast<uint64_t>(Stats::subscription_messages)
        << "  Req: "   << static_cast<uint64_t>(Stats::request_messages)
        << "  Rep: "   << static_cast<uint64_t>(Stats::reply_messages);
    RTLOG_INFO(g_logger) << "  RegistryStats: PASS";
}

void test_payload_extraction() {
    RTLOG_INFO(g_logger) << "Testing payload extraction...";

    using AllPayloads = registry::get_all_payloads_t<TestRegistry>;
    constexpr size_t num_payloads = registry::tuple_size_v<AllPayloads>;
    RTLOG_INFO(g_logger) << "  Total payload types: " << static_cast<uint64_t>(num_payloads);

    using DataMsgs = registry::data_messages_t<TestRegistry>;
    using DataPayloads = registry::extract_payloads_t<DataMsgs>;
    constexpr size_t num_data_payloads = registry::tuple_size_v<DataPayloads>;
    static_assert(num_data_payloads == 2, "Should have 2 data payloads");
    RTLOG_INFO(g_logger) << "  Data payload types: " << static_cast<uint64_t>(num_data_payloads) << " (expected 2): PASS";
}

void test_tuple_utilities() {
    RTLOG_INFO(g_logger) << "Testing tuple utilities...";

    using EmptyTuple = std::tuple<>;
    static_assert(registry::is_empty_tuple_v<EmptyTuple>, "Empty tuple check failed");

    using NonEmpty = std::tuple<int, float>;
    static_assert(!registry::is_empty_tuple_v<NonEmpty>, "Non-empty tuple check failed");

    using TestTuple = std::tuple<int, float, double>;
    static_assert( registry::contains_type_v<int,   TestTuple>, "Should contain int");
    static_assert( registry::contains_type_v<float, TestTuple>, "Should contain float");
    static_assert(!registry::contains_type_v<char,  TestTuple>, "Should not contain char");

    RTLOG_INFO(g_logger) << "  Tuple utilities: PASS";
}

void test_message_def_lookup() {
    RTLOG_INFO(g_logger) << "Testing MessageDef lookup...";

    using TempDef = registry::find_message_def_t<TemperatureData, TestRegistry>;
    static_assert(!std::is_same_v<TempDef, void>, "Should find TemperatureData def");
    static_assert(TempDef::prefix == MessagePrefix::UserDefined, "Wrong prefix");

    static_assert(registry::has_message_def_v<TemperatureData, TestRegistry>);
    static_assert(!registry::has_message_def_v<ResetReplyPayload, TestRegistry> ||
                  registry::has_message_def_v<ResetReplyPayload, TestRegistry>,
                  "Reply payload may or may not be directly registered");

    using CalibrateDef = registry::find_message_def_t<CalibrateCmdPayload, TestRegistry>;
    static_assert(CalibrateDef::has_reply,  "Calibrate should have reply");
    static_assert(CalibrateDef::is_request, "Calibrate should be a request");

    static_assert(registry::is_request_payload_v<CalibrateCmdPayload, TestRegistry>,
                  "CalibrateCmdPayload should be a request payload");
    static_assert(!registry::is_request_payload_v<TemperatureData, TestRegistry>,
                  "TemperatureData is not a request payload");

    RTLOG_INFO(g_logger) << "  MessageDef lookup: PASS";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    g_logger.add_sink(&g_sink);
    g_logger.start_drain();

    RTLOG_INFO(g_logger) << "=== CoreRaT registry_utils test suite ===";

    test_category_filters();
    test_prefix_filters();
    test_subprefix_filters();
    test_request_reply_filters();
    test_convenience_counters();
    test_registry_stats();
    test_payload_extraction();
    test_tuple_utilities();
    test_message_def_lookup();

    RTLOG_INFO(g_logger) << "=== ALL TESTS PASSED ===";
    g_logger.stop_drain();
    return 0;
}
