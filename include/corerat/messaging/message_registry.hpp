#pragma once

#include "wire_message.hpp"
#include "message_id.hpp"
#include <type_traits>
#include <tuple>
#include <optional>
#include <span>

namespace corerat {

// Forward declarations
template<typename... MessageDefs> class Mailbox;
template<typename... MessageDefs> class MessageRegistry;

// ============================================================================
// Request-Reply Message Expansion
// ============================================================================

template<typename... MessageDefs>
struct ExpandReplies;

template<>
struct ExpandReplies<> {
    using Result = std::tuple<>;
};

template<typename First, typename... Rest>
struct ExpandReplies<First, Rest...> {
private:
    using RestExpanded = typename ExpandReplies<Rest...>::Result;

public:
    using Result = std::conditional_t<
        First::has_reply,
        decltype(std::tuple_cat(
            std::declval<std::tuple<First, typename First::ReplyMessageDef>>(),
            std::declval<RestExpanded>()
        )),
        decltype(std::tuple_cat(
            std::declval<std::tuple<First>>(),
            std::declval<RestExpanded>()
        ))
    >;
};

// ============================================================================
// Message ID Auto-Increment System
// ============================================================================

template<typename T, typename = void>
struct GetReplyPayload {
    using type = void;
};

template<typename T>
struct GetReplyPayload<T, std::void_t<typename T::ReplyPayload>> {
    using type = typename T::ReplyPayload;
};

template<typename ProcessedTuple, typename... Remaining>
struct AutoAssignIDsProcess;

template<typename... ProcessedDefs>
struct AutoAssignIDsProcess<std::tuple<ProcessedDefs...>> {
    using Result = std::tuple<>;
};

template<typename... ProcessedDefs, typename First, typename... Rest>
struct AutoAssignIDsProcess<std::tuple<ProcessedDefs...>, First, Rest...> {
private:
    template<typename MessageDef>
    struct HighestID {
        static constexpr uint16_t value = []() constexpr {
            if constexpr (sizeof...(ProcessedDefs) == 0) {
                return 0;
            } else {
                uint16_t max_id = 0;
                ((max_id = (ProcessedDefs::prefix == MessageDef::prefix &&
                            ProcessedDefs::subprefix == MessageDef::subprefix &&
                            !ProcessedDefs::is_reply &&
                            ProcessedDefs::local_id > max_id) ?
                           ProcessedDefs::local_id : max_id), ...);
                return max_id;
            }
        }();
    };

    using CurrentProcessed = std::conditional_t<
        First::needs_auto_id,
        MessageDefinition<
            typename First::Payload,
            First::prefix,
            First::subprefix,
            HighestID<First>::value + 1,
            typename GetReplyPayload<First>::type
        >,
        First
    >;

    static_assert(!CurrentProcessed::needs_auto_id || CurrentProcessed::local_id <= MAX_MESSAGE_ID,
                  "Auto-assigned message ID exceeds MAX_MESSAGE_ID (0x7FFF)");

    using RestResult = typename AutoAssignIDsProcess<
        std::tuple<ProcessedDefs..., CurrentProcessed>, Rest...>::Result;

public:
    using Result = decltype(std::tuple_cat(
        std::declval<std::tuple<CurrentProcessed>>(),
        std::declval<RestResult>()
    ));
};

template<typename... MessageDefs>
struct AutoAssignIDs {
    using Result = typename AutoAssignIDsProcess<std::tuple<>, MessageDefs...>::Result;
};

// ============================================================================
// Compile-Time Message ID Collision Detection
// ============================================================================

template<typename... MessageDefs>
struct CheckCollisions {
    static constexpr bool check() {
        if constexpr (sizeof...(MessageDefs) <= 1) {
            return true;
        } else {
            return check_all_pairs<MessageDefs...>();
        }
    }

private:
    template<typename First, typename Second, typename... Rest>
    static constexpr bool check_all_pairs() {
        constexpr uint32_t id1 = make_message_id(
            static_cast<uint8_t>(First::prefix),
            First::subprefix, First::local_id);
        constexpr uint32_t id2 = make_message_id(
            static_cast<uint8_t>(Second::prefix),
            Second::subprefix, Second::local_id);

        static_assert(id1 != id2, "Message ID collision detected!");

        if constexpr (sizeof...(Rest) > 0) {
            return check_all_pairs<First, Rest...>() && check_all_pairs<Second, Rest...>();
        }
        return true;
    }
};

// ============================================================================
// MessageRegistry<...>
// ============================================================================

template<typename... MessageDefs>
class MessageRegistry {
private:
    // Step 1: Auto-assign IDs
    using IDsAssigned = typename AutoAssignIDs<MessageDefs...>::Result;

    // Step 2: Expand request messages to include their replies
    template<typename Tuple>
    struct TupleToList;

    template<typename... Defs>
    struct TupleToList<std::tuple<Defs...>> {
        using Expanded = typename ExpandReplies<Defs...>::Result;
    };

    using ProcessedDefs = typename TupleToList<IDsAssigned>::Expanded;

    // Extract payload types
    template<typename Tuple>
    struct ExtractPayloads;

    template<typename... Defs>
    struct ExtractPayloads<std::tuple<Defs...>> {
        using PayloadTypes = std::tuple<typename Defs::Payload...>;
    };

    static constexpr bool collisions_checked = CheckCollisions<MessageDefs...>::check();

public:
    using MessageDefsTuple = ProcessedDefs;
    using PayloadTypes     = typename ExtractPayloads<ProcessedDefs>::PayloadTypes;

    template<typename T, typename Tuple>
    struct IsInTuple;

    template<typename T, typename... Types>
    struct IsInTuple<T, std::tuple<Types...>> {
        static constexpr bool value = (std::is_same_v<T, Types> || ...);
    };

    template<typename T>
    static constexpr bool is_registered_v = IsInTuple<T, PayloadTypes>::value;

    static constexpr size_t num_types = sizeof...(MessageDefs);

    static constexpr size_t size() {
        return std::tuple_size_v<MessageDefsTuple>;
    }

    template<typename... Payloads>
    static constexpr size_t calc_max_size(std::tuple<Payloads...>*) {
        return std::max({sertial::Message<WireMessage<Payloads>>::max_buffer_size...});
    }

    static constexpr size_t max_message_size = calc_max_size(static_cast<PayloadTypes*>(nullptr));

    template<typename... SpecificTypes>
    static constexpr size_t max_size_for_types() {
        static_assert(sizeof...(SpecificTypes) > 0,
                      "max_size_for_types requires at least one type");
        static_assert((is_registered_v<SpecificTypes> && ...),
                      "All types must be registered in the message registry");
        return std::max({sertial::Message<WireMessage<SpecificTypes>>::max_buffer_size...});
    }

private:
    // Find MessageDefinition for a payload type
    template<typename PayloadT, typename Tuple>
    struct FindMessageDef;

    template<typename PayloadT, typename... Defs>
    struct FindMessageDef<PayloadT, std::tuple<Defs...>> {
    private:
        template<typename Def>
        static constexpr bool matches = std::is_same_v<typename Def::Payload, PayloadT>;

        template<typename First, typename... Remaining>
        static constexpr auto find_impl() {
            if constexpr (matches<First>) {
                return First{};
            } else if constexpr (sizeof...(Remaining) > 0) {
                return find_impl<Remaining...>();
            } else {
                return void{};
            }
        }

    public:
        using type = decltype(find_impl<Defs...>());
    };

    template<typename PayloadT>
    static constexpr uint32_t get_message_id_for() {
        using MessageDef = typename FindMessageDef<PayloadT, ProcessedDefs>::type;
        return make_message_id(
            static_cast<uint8_t>(MessageDef::prefix),
            MessageDef::subprefix,
            MessageDef::local_id
        );
    }

    template<typename T, typename Tuple, size_t Index = 0>
    struct TypeIndex;

    template<typename T, typename... Types, size_t Index>
    struct TypeIndex<T, std::tuple<Types...>, Index> {
        static constexpr size_t value = []() constexpr {
            size_t idx = num_types;
            size_t current = 0;
            ((std::is_same_v<T, Types> ? (idx = current, 0) : (++current, 0)), ...);
            return idx;
        }();
    };

    template<typename T>
    static constexpr size_t type_index() {
        return TypeIndex<T, PayloadTypes>::value;
    }

    // Lookup type by runtime message ID
    template<uint32_t ID, size_t Index, typename Tuple>
    struct TypeByID;

    template<uint32_t ID, size_t Index, typename... Defs>
    struct TypeByID<ID, Index, std::tuple<Defs...>> {
        using type = void;
        static constexpr bool found = false;
    };

    template<uint32_t ID, size_t Index, typename... Defs>
        requires (Index < sizeof...(Defs))
    struct TypeByID<ID, Index, std::tuple<Defs...>> {
    private:
        using CurrentDef = std::tuple_element_t<Index, std::tuple<Defs...>>;
        static constexpr uint32_t current_id = make_message_id(
            static_cast<uint8_t>(CurrentDef::prefix),
            CurrentDef::subprefix,
            CurrentDef::local_id
        );

    public:
        using type = std::conditional_t<
            current_id == ID,
            typename CurrentDef::Payload,
            typename TypeByID<ID, Index + 1, std::tuple<Defs...>>::type
        >;
        static constexpr bool found = (current_id == ID) ||
                                      TypeByID<ID, Index + 1, std::tuple<Defs...>>::found;
    };

public:
    // ========================================================================
    // Type Traits
    // ========================================================================

    template<typename T>
    static constexpr bool is_registered = is_registered_v<T>;

    template<typename T>
        requires is_registered_v<T>
    static constexpr uint32_t get_message_id() {
        return get_message_id_for<T>();
    }

    template<uint32_t ID>
    using PayloadTypeFor = typename TypeByID<ID, 0, ProcessedDefs>::type;

    template<uint32_t ID>
    static constexpr bool has_message_id = TypeByID<ID, 0, ProcessedDefs>::found;

    template<std::size_t I>
    using type_at = std::tuple_element_t<I, PayloadTypes>;

    template<typename T>
    static constexpr std::size_t get_type_index() {
        return type_index<T>();
    }

    // ========================================================================
    // Serialization Interface
    // ========================================================================

    template<typename PayloadT>
        requires is_registered_v<PayloadT>
    static auto serialize(WireMessage<PayloadT>& message) {
        message.header.msg_type = get_message_id<PayloadT>();
        auto result = sertial::Message<WireMessage<PayloadT>>::serialize(message);
        message.header.msg_size = static_cast<uint32_t>(result.size);
        return result;
    }

    template<typename MsgT>
        requires requires { typename MsgT::payload_type; } &&
                 is_registered_v<typename MsgT::payload_type>
    static auto deserialize(std::span<const std::byte> data) {
        return sertial::Message<MsgT>::deserialize(data);
    }

    // ========================================================================
    // Runtime Dispatch (Visitor Pattern)
    // ========================================================================

    template<typename Visitor>
    static bool visit(uint32_t msg_id, std::span<const std::byte> data, Visitor&& visitor) {
        return visit_impl<0>(msg_id, data, std::forward<Visitor>(visitor));
    }

    template<typename Callback>
    static bool dispatch(uint32_t msg_id, std::span<const std::byte> data, Callback&& callback) {
        return visit(msg_id, data, std::forward<Callback>(callback));
    }

    // ========================================================================
    // Compile-Time Information
    // ========================================================================

    static constexpr size_t max_buffer_size() { return max_message_size; }

    template<typename... Defs>
    static constexpr auto get_all_ids_impl(std::tuple<Defs...>*) {
        return std::array<uint32_t, sizeof...(Defs)>{
            make_message_id(
                static_cast<uint8_t>(Defs::prefix),
                Defs::subprefix,
                Defs::local_id
            )...
        };
    }

    static constexpr auto message_ids() {
        return get_all_ids_impl(static_cast<ProcessedDefs*>(nullptr));
    }

    // ========================================================================
    // System Infrastructure (for Module2 compatibility)
    // ========================================================================

    struct System {
        using WorkMailbox = Mailbox<MessageDefs...>;
    };

private:
    // Recursive visitor implementation
    template<size_t Index, typename Visitor, typename... Defs>
    static bool visit_impl_helper(uint32_t msg_id, std::span<const std::byte> data,
                                   Visitor&& visitor, std::tuple<Defs...>*) {
        if constexpr (Index >= sizeof...(Defs)) {
            return false;
        } else {
            using CurrentDef = std::tuple_element_t<Index, std::tuple<Defs...>>;
            using CurrentPayload = typename CurrentDef::Payload;

            constexpr uint32_t current_id = make_message_id(
                static_cast<uint8_t>(CurrentDef::prefix),
                CurrentDef::subprefix,
                CurrentDef::local_id
            );

            if (msg_id == current_id) {
                auto result = deserialize<WireMessage<CurrentPayload>>(data);
                if (result) {
                    std::forward<Visitor>(visitor)(*result);
                    return true;
                }
                return false;
            }

            return visit_impl_helper<Index + 1>(msg_id, data,
                                                std::forward<Visitor>(visitor),
                                                static_cast<std::tuple<Defs...>*>(nullptr));
        }
    }

    template<size_t Index, typename Visitor>
    static bool visit_impl(uint32_t msg_id, std::span<const std::byte> data, Visitor&& visitor) {
        return visit_impl_helper<Index>(msg_id, data, std::forward<Visitor>(visitor),
                                        static_cast<ProcessedDefs*>(nullptr));
    }
};

// ============================================================================
// Aliases
// ============================================================================

template<typename... MessageDefs>
using make_registry = MessageRegistry<MessageDefs...>;

template<typename... MessageDefs>
using CustomRegistry = MessageRegistry<MessageDefs...>;

} // namespace corerat
