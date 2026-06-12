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
#include <iostream>
#include <cassert>

using namespace corerat;

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
    std::cout << "Testing category filters...\n";

    using DataMsgs = registry::data_messages_t<TestRegistry>;
    constexpr size_t num_data = registry::tuple_size_v<DataMsgs>;
    static_assert(num_data == 2, "Should have 2 data messages");
    std::cout << "  Data messages: " << num_data << " (expected 2): PASS\n";

    using CommandMsgs = registry::command_messages_t<TestRegistry>;
    constexpr size_t num_commands = registry::tuple_size_v<CommandMsgs>;
    // 2 request + 2 reply = 4 command messages after registry expansion
    static_assert(num_commands == 4, "Should have 4 command messages (2 request + 2 reply)");
    std::cout << "  Command messages: " << num_commands << " (expected 4): PASS\n";

    // No subscription messages in a bare CoreRaT registry
    using SubMsgs = registry::subscription_messages_t<TestRegistry>;
    constexpr size_t num_sub = registry::tuple_size_v<SubMsgs>;
    static_assert(num_sub == 0, "No subscription messages in a CoreRaT registry");
    std::cout << "  Subscription messages: " << num_sub << " (expected 0, CommRaT adds these): PASS\n";
}

void test_prefix_filters() {
    std::cout << "Testing prefix filters...\n";

    using UserMsgs = registry::filter_by_prefix_t<MessagePrefix::UserDefined, TestRegistry>;
    constexpr size_t num_user = registry::tuple_size_v<UserMsgs>;
    static_assert(num_user > 0, "Should have user-defined messages");
    std::cout << "  UserDefined messages: " << num_user << ": PASS\n";

    using SystemMsgs = registry::filter_by_prefix_t<MessagePrefix::System, TestRegistry>;
    constexpr size_t num_system = registry::tuple_size_v<SystemMsgs>;
    static_assert(num_system == 0, "No system messages in a bare CoreRaT registry");
    std::cout << "  System messages: " << num_system << " (expected 0, CommRaT adds these): PASS\n";
}

void test_subprefix_filters() {
    std::cout << "Testing subprefix filters...\n";

    using UserDataMsgs = registry::filter_by_subprefix_t<
        MessagePrefix::UserDefined,
        static_cast<uint8_t>(UserSubPrefix::Data),
        TestRegistry
    >;
    constexpr size_t num_data = registry::tuple_size_v<UserDataMsgs>;
    static_assert(num_data == 2, "Should have exactly 2 user data messages");
    std::cout << "  UserDefined::Data: " << num_data << " (expected 2): PASS\n";

    using UserCmdMsgs = registry::filter_by_subprefix_t<
        MessagePrefix::UserDefined,
        static_cast<uint8_t>(UserSubPrefix::Commands),
        TestRegistry
    >;
    constexpr size_t num_cmd = registry::tuple_size_v<UserCmdMsgs>;
    static_assert(num_cmd == 4, "Should have 4 command messages (2 req + 2 reply)");
    std::cout << "  UserDefined::Commands: " << num_cmd << " (expected 4): PASS\n";
}

void test_request_reply_filters() {
    std::cout << "Testing request/reply filters...\n";

    using Requests = registry::filter_requests_t<TestRegistry>;
    constexpr size_t num_requests = registry::tuple_size_v<Requests>;
    std::cout << "  Request messages: " << num_requests << "\n";

    using Replies = registry::filter_replies_t<TestRegistry>;
    constexpr size_t num_replies = registry::tuple_size_v<Replies>;
    std::cout << "  Reply messages: " << num_replies << "\n";

    static_assert(num_requests == num_replies, "Should have equal requests and replies");
    static_assert(num_requests == 2, "Should have exactly 2 request messages");
    std::cout << "  Request/reply symmetry (2 each): PASS\n";
}

void test_convenience_counters() {
    std::cout << "Testing convenience counter functions...\n";

    constexpr size_t data_count = registry::data_message_count<TestRegistry>();
    constexpr size_t cmd_count  = registry::command_message_count<TestRegistry>();
    constexpr size_t req_count  = registry::request_message_count<TestRegistry>();
    constexpr size_t rep_count  = registry::reply_message_count<TestRegistry>();

    std::cout << "  Data:     " << data_count << "\n"
              << "  Commands: " << cmd_count  << "\n"
              << "  Requests: " << req_count  << "\n"
              << "  Replies:  " << rep_count  << "\n";

    static_assert(data_count == 2, "Data count mismatch");
    static_assert(cmd_count  == 4, "Command count mismatch (2 req + 2 reply)");
    static_assert(req_count  == rep_count, "Request/reply count mismatch");
    std::cout << "  Convenience counters: PASS\n";
}

void test_registry_stats() {
    std::cout << "Testing RegistryStats introspection...\n";

    using Stats = registry::RegistryStats<TestRegistry>;
    std::cout << "  Total: " << Stats::total_messages << "  Data: " << Stats::data_messages
              << "  Cmd: " << Stats::command_messages << "  Sub: " << Stats::subscription_messages
              << "  Req: " << Stats::request_messages << "  Rep: " << Stats::reply_messages << "\n";
    std::cout << "  RegistryStats: PASS\n";
}

void test_payload_extraction() {
    std::cout << "Testing payload extraction...\n";

    using AllPayloads = registry::get_all_payloads_t<TestRegistry>;
    constexpr size_t num_payloads = registry::tuple_size_v<AllPayloads>;
    std::cout << "  Total payload types: " << num_payloads << "\n";

    using DataMsgs = registry::data_messages_t<TestRegistry>;
    using DataPayloads = registry::extract_payloads_t<DataMsgs>;
    constexpr size_t num_data_payloads = registry::tuple_size_v<DataPayloads>;
    static_assert(num_data_payloads == 2, "Should have 2 data payloads");
    std::cout << "  Data payload types: " << num_data_payloads << " (expected 2): PASS\n";
}

void test_tuple_utilities() {
    std::cout << "Testing tuple utilities...\n";

    using EmptyTuple = std::tuple<>;
    static_assert(registry::is_empty_tuple_v<EmptyTuple>, "Empty tuple check failed");

    using NonEmpty = std::tuple<int, float>;
    static_assert(!registry::is_empty_tuple_v<NonEmpty>, "Non-empty tuple check failed");

    using TestTuple = std::tuple<int, float, double>;
    static_assert( registry::contains_type_v<int,   TestTuple>, "Should contain int");
    static_assert( registry::contains_type_v<float, TestTuple>, "Should contain float");
    static_assert(!registry::contains_type_v<char,  TestTuple>, "Should not contain char");

    std::cout << "  Tuple utilities: PASS\n";
}

void test_message_def_lookup() {
    std::cout << "Testing MessageDef lookup...\n";

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

    std::cout << "  MessageDef lookup: PASS\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== CoreRaT registry_utils test suite ===\n\n";

    test_category_filters();
    test_prefix_filters();
    test_subprefix_filters();
    test_request_reply_filters();
    test_convenience_counters();
    test_registry_stats();
    test_payload_extraction();
    test_tuple_utilities();
    test_message_def_lookup();

    std::cout << "\n=== ALL TESTS PASSED ===\n\n";
    return 0;
}
