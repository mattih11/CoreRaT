/**
 * @file test_message_id.cpp
 * @brief Test compile-time message ID assignment, auto-increment, and reply derivation
 *
 * Adapted from CommRaT test_auto_id_validation.cpp.
 * Tests corerat::MessageDefinition, MessageRegistry, and associated ID logic.
 */

#include "corerat/messaging/message_id.hpp"
#include "corerat/messaging/message_registry.hpp"
#include <corerat/logging/logging.hpp>
#include <cassert>

using namespace corerat;

static corerat::TerminalSink g_sink{};
static corerat::RtLogger<64> g_logger{0x00000001u, corerat::LogLevel::Trace};

// Test payload types
struct AutoID1 { int value; };
struct AutoID2 { int value; };
struct AutoID3 { int value; };
struct ManualID100 { int value; };
struct ReplyPayload { bool success; };

int main() {
    g_logger.add_sink(&g_sink);
    g_logger.start_drain();

    RTLOG_INFO(g_logger) << "Testing CoreRaT message ID assignment and validation...";

    // Test 1: AUTO_ID marker is 0
    {
        static_assert(DefaultMessageDef::id == 0, "AUTO_ID should be 0");
        RTLOG_INFO(g_logger) << "  AUTO_ID = 0: PASS";
    }

    // Test 2: MAX_MESSAGE_ID is 0x7FFF
    {
        static_assert(MAX_MESSAGE_ID == 0x7FFF, "MAX_MESSAGE_ID should be 0x7FFF");
        RTLOG_INFO(g_logger) << "  MAX_MESSAGE_ID = 0x7FFF: PASS";
    }

    // Test 3: Auto-assigned IDs start at 1 after registry processing
    {
        using AutoMsg1 = MessageDefinition<AutoID1, MessagePrefix::UserDefined, UserSubPrefix::Data>;
        using AutoMsg2 = MessageDefinition<AutoID2, MessagePrefix::UserDefined, UserSubPrefix::Data>;
        using AutoMsg3 = MessageDefinition<AutoID3, MessagePrefix::UserDefined, UserSubPrefix::Data>;

        using TestRegistry = MessageRegistry<AutoMsg1, AutoMsg2, AutoMsg3>;
        (void)sizeof(TestRegistry);

        static_assert(AutoMsg1::local_id == 0,    "Before registry processing, ID is 0");
        static_assert(AutoMsg1::needs_auto_id,     "Should need auto ID");

        // After processing, IDs 1/2/3 assigned in order
        using Processed = TestRegistry::MessageDefsTuple;
        using Def0 = std::tuple_element_t<0, Processed>;
        using Def1 = std::tuple_element_t<1, Processed>;
        using Def2 = std::tuple_element_t<2, Processed>;
        static_assert(Def0::local_id == 1, "First auto-ID should be 1");
        static_assert(Def1::local_id == 2, "Second auto-ID should be 2");
        static_assert(Def2::local_id == 3, "Third auto-ID should be 3");

        RTLOG_INFO(g_logger) << "  Auto-assigned IDs (1, 2, 3): PASS";
    }

    // Test 4: Reply ID = bitwise NOT of request ID
    {
        using RequestMsg = MessageDefinition<
            AutoID1,
            MessagePrefix::System,
            SystemSubPrefix::Subscription,
            0x0001,
            ReplyPayload
        >;

        static_assert(RequestMsg::has_reply,       "Should have reply");
        static_assert(RequestMsg::is_request,      "Should be marked as request");
        static_assert(RequestMsg::local_id == 1,   "Request ID = 1");

        using ReplyMsg = typename RequestMsg::ReplyMessageDef;
        constexpr int16_t req_id   = static_cast<int16_t>(RequestMsg::local_id);
        constexpr int16_t reply_id = static_cast<int16_t>(ReplyMsg::local_id);
        static_assert(reply_id == -req_id,              "Reply ID = -Request ID");
        static_assert(static_cast<uint16_t>(reply_id) == 0xFFFF, "Reply ID as uint16 = 0xFFFF");

        RTLOG_INFO(g_logger) << "  Reply ID = -Request ID: PASS";
    }

    // Test 5: Manual ID within range is preserved
    {
        using ValidMsg = MessageDefinition<
            ManualID100,
            MessagePrefix::UserDefined,
            UserSubPrefix::Data,
            100
        >;

        static_assert(ValidMsg::local_id == 100, "Manual ID should be preserved");
        static_assert(!ValidMsg::needs_auto_id,  "Should not need auto ID");

        RTLOG_INFO(g_logger) << "  Manual ID preserved: PASS";
    }

    // Test 6: MAX_MESSAGE_ID boundary (0x7FFF) is accepted
    {
        using MaxMsg = MessageDefinition<
            ManualID100,
            MessagePrefix::UserDefined,
            UserSubPrefix::Data,
            0x7FFF
        >;

        static_assert(MaxMsg::local_id == 0x7FFF, "Should allow MAX_MESSAGE_ID");

        RTLOG_INFO(g_logger) << "  MAX_MESSAGE_ID boundary (0x7FFF): PASS";
    }

    // Test 7: Negative IDs (reply messages) detected correctly
    {
        using ReplyMsg = MessageDefinition<
            ReplyPayload,
            MessagePrefix::System,
            SystemSubPrefix::Subscription,
            static_cast<uint16_t>(-10)
        >;

        static_assert(ReplyMsg::local_id == 0xFFF6,                    "Negative ID stored as uint16");
        static_assert(static_cast<int16_t>(ReplyMsg::local_id) == -10, "Interpreted as -10");
        static_assert(ReplyMsg::is_reply,   "Should be detected as reply");
        static_assert(!ReplyMsg::is_request,"Should not be request");
        static_assert(!ReplyMsg::has_reply, "Reply should not have reply");

        RTLOG_INFO(g_logger) << "  Negative IDs (reply messages): PASS";
    }

    // Test 8: make_message_id produces correct 32-bit value
    {
        constexpr uint32_t id = make_message_id(0x01, 0x00, 0x0001);
        static_assert(id == 0x01000001, "make_message_id encoding");
        RTLOG_INFO(g_logger) << "  make_message_id encoding: PASS";
    }

    // Test 9: is_registered<T> works on processed registry
    {
        using Msg = MessageDefinition<AutoID1, MessagePrefix::UserDefined, UserSubPrefix::Data>;
        using Reg = MessageRegistry<Msg>;
        static_assert(Reg::is_registered<AutoID1>, "AutoID1 should be registered");
        RTLOG_INFO(g_logger) << "  is_registered<T>: PASS";
    }

    RTLOG_INFO(g_logger) << "All message ID tests PASSED!";
    g_logger.stop_drain();
    return 0;
}
