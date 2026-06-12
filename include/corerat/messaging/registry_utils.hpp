#pragma once

/**
 * @file registry_utils.hpp
 * @brief Compile-time utilities for querying and manipulating message registries
 *
 * Provides query helpers for extracting message subsets, filtering by
 * prefix/subprefix, and request/reply lookup.
 *
 * All utilities are constexpr and work at compile-time for zero runtime overhead.
 */

#include "message_id.hpp"
#include <cstddef>
#include <tuple>
#include <type_traits>

namespace corerat {

// Forward declarations
template<typename... MessageDefs>
class MessageRegistry;

namespace registry {

// ============================================================================
// Tuple Utilities
// ============================================================================

template<typename T>
inline constexpr std::size_t tuple_size_v = std::tuple_size_v<T>;

template<typename T>
inline constexpr bool is_empty_tuple_v = (std::tuple_size_v<T> == 0);

template<typename T, typename Tuple>
struct ContainsType;

template<typename T, typename... Us>
struct ContainsType<T, std::tuple<Us...>> : std::disjunction<std::is_same<T, Us>...> {};

template<typename T, typename Tuple>
inline constexpr bool contains_type_v = ContainsType<T, Tuple>::value;

// ============================================================================
// Internal Helpers
// ============================================================================

namespace detail {

template<MessagePrefix P, typename MessageDef>
struct MatchesPrefix {
    static constexpr bool value = (MessageDef::prefix == P);
};

template<MessagePrefix P, uint8_t SubP, typename MessageDef>
struct MatchesSubPrefix {
    static constexpr bool value = (MessageDef::prefix == P) &&
                                  (MessageDef::subprefix == SubP);
};

template<template<typename> class Predicate, typename Tuple>
struct FilterTuple;

template<template<typename> class Predicate>
struct FilterTuple<Predicate, std::tuple<>> {
    using type = std::tuple<>;
};

template<template<typename> class Predicate, typename First, typename... Rest>
struct FilterTuple<Predicate, std::tuple<First, Rest...>> {
private:
    using RestFiltered = typename FilterTuple<Predicate, std::tuple<Rest...>>::type;
public:
    using type = std::conditional_t<
        Predicate<First>::value,
        decltype(std::tuple_cat(std::declval<std::tuple<First>>(), std::declval<RestFiltered>())),
        RestFiltered
    >;
};

template<template<typename> class Predicate, typename Tuple>
using filter_tuple_t = typename FilterTuple<Predicate, Tuple>::type;

template<typename MessageDefsTuple>
struct ExtractPayloads;

template<typename... MessageDefs>
struct ExtractPayloads<std::tuple<MessageDefs...>> {
    using type = std::tuple<typename MessageDefs::Payload...>;
};

template<typename MessageDefsTuple>
using extract_payloads_t = typename ExtractPayloads<MessageDefsTuple>::type;

} // namespace detail

// ============================================================================
// Public aliases for detail utilities
// ============================================================================

/// Extract all payload types from a MessageDefsTuple (result is std::tuple<Payload...>)
template<typename MessageDefsTuple>
using extract_payloads_t = detail::extract_payloads_t<MessageDefsTuple>;

/// Get all payload types registered in a Registry
template<typename Registry>
using get_all_payloads_t = typename Registry::PayloadTypes;

// ============================================================================
// Message Filtering by Prefix / SubPrefix
// ============================================================================

template<MessagePrefix P, typename Registry>
struct FilterByPrefix {
private:
    template<typename T>
    using Predicate = detail::MatchesPrefix<P, T>;
    using MessageDefs = typename Registry::MessageDefsTuple;
public:
    using type = detail::filter_tuple_t<Predicate, MessageDefs>;
};

template<MessagePrefix P, typename Registry>
using filter_by_prefix_t = typename FilterByPrefix<P, Registry>::type;

template<MessagePrefix P, uint8_t SubP, typename Registry>
struct FilterBySubPrefix {
private:
    template<typename T>
    using Predicate = detail::MatchesSubPrefix<P, SubP, T>;
    using MessageDefs = typename Registry::MessageDefsTuple;
public:
    using type = detail::filter_tuple_t<Predicate, MessageDefs>;
};

template<MessagePrefix P, uint8_t SubP, typename Registry>
using filter_by_subprefix_t = typename FilterBySubPrefix<P, SubP, Registry>::type;

// ============================================================================
// Convenient Category Filters
// ============================================================================

template<typename Registry>
using data_messages_t = filter_by_subprefix_t<
    MessagePrefix::UserDefined,
    static_cast<uint8_t>(UserSubPrefix::Data),
    Registry
>;

template<typename Registry>
using command_messages_t = filter_by_subprefix_t<
    MessagePrefix::UserDefined,
    static_cast<uint8_t>(UserSubPrefix::Commands),
    Registry
>;

template<typename Registry>
using event_messages_t = filter_by_subprefix_t<
    MessagePrefix::UserDefined,
    static_cast<uint8_t>(UserSubPrefix::Events),
    Registry
>;

template<typename Registry>
using subscription_messages_t = filter_by_subprefix_t<
    MessagePrefix::System,
    static_cast<uint8_t>(SystemSubPrefix::Subscription),
    Registry
>;

template<typename Registry>
using system_control_messages_t = filter_by_subprefix_t<
    MessagePrefix::System,
    static_cast<uint8_t>(SystemSubPrefix::Control),
    Registry
>;

// ============================================================================
// Request / Reply Filters
// ============================================================================

template<typename MessageDef>
struct IsRequest {
    static constexpr bool value = MessageDef::has_reply && MessageDef::is_request;
};

template<typename MessageDef>
struct IsReply {
    static constexpr bool value = (MessageDef::local_id > MAX_MESSAGE_ID);
};

template<typename MessageDef>
inline constexpr bool is_reply_v = IsReply<MessageDef>::value;

template<typename Registry>
struct FilterRequests {
private:
    template<typename T>
    using Predicate = IsRequest<T>;
    using MessageDefs = typename Registry::MessageDefsTuple;
public:
    using type = detail::filter_tuple_t<Predicate, MessageDefs>;
};

template<typename Registry>
using filter_requests_t = typename FilterRequests<Registry>::type;

template<typename Registry>
struct FilterReplies {
private:
    template<typename T>
    using Predicate = IsReply<T>;
    using MessageDefs = typename Registry::MessageDefsTuple;
public:
    using type = detail::filter_tuple_t<Predicate, MessageDefs>;
};

template<typename Registry>
using filter_replies_t = typename FilterReplies<Registry>::type;

// ============================================================================
// Convenience Count Functions
// ============================================================================

template<typename Registry>
constexpr size_t data_message_count() {
    return std::tuple_size_v<data_messages_t<Registry>>;
}

template<typename Registry>
constexpr size_t command_message_count() {
    return std::tuple_size_v<command_messages_t<Registry>>;
}

template<typename Registry>
constexpr size_t subscription_message_count() {
    return std::tuple_size_v<subscription_messages_t<Registry>>;
}

template<typename Registry>
constexpr size_t request_message_count() {
    return std::tuple_size_v<filter_requests_t<Registry>>;
}

template<typename Registry>
constexpr size_t reply_message_count() {
    return std::tuple_size_v<filter_replies_t<Registry>>;
}

// ============================================================================
// MessageDef Lookup by Payload Type
// ============================================================================

template<typename PayloadT, typename Registry>
struct FindMessageDefForPayload {
private:
    template<typename MessageDef>
    static constexpr bool matches = std::is_same_v<typename MessageDef::Payload, PayloadT>;

    template<typename Tuple, size_t Index = 0>
    struct FindImpl;

    template<typename... MessageDefs, size_t Index>
    struct FindImpl<std::tuple<MessageDefs...>, Index> {
        using type = void;
    };

    template<typename... MessageDefs, size_t Index>
        requires (Index < sizeof...(MessageDefs))
    struct FindImpl<std::tuple<MessageDefs...>, Index> {
    private:
        using Current = std::tuple_element_t<Index, std::tuple<MessageDefs...>>;
    public:
        using type = std::conditional_t<
            matches<Current>,
            Current,
            typename FindImpl<std::tuple<MessageDefs...>, Index + 1>::type
        >;
    };

public:
    using type = typename FindImpl<typename Registry::MessageDefsTuple>::type;
};

template<typename PayloadT, typename Registry>
using find_message_def_t = typename FindMessageDefForPayload<PayloadT, Registry>::type;

template<typename PayloadT, typename Registry>
struct HasMessageDef {
    static constexpr bool value = !std::is_same_v<find_message_def_t<PayloadT, Registry>, void>;
};

template<typename PayloadT, typename Registry>
inline constexpr bool has_message_def_v = HasMessageDef<PayloadT, Registry>::value;

template<typename RequestPayload, typename Registry>
struct GetReplyPayloadFor {
private:
    using RequestDef = find_message_def_t<RequestPayload, Registry>;
    static constexpr bool has_valid_reply =
        !std::is_same_v<RequestDef, void> && RequestDef::has_reply;
public:
    using type = std::conditional_t<
        has_valid_reply,
        typename RequestDef::ReplyMessageDef::Payload,
        void
    >;
};

template<typename RequestPayload, typename Registry>
using get_reply_payload_t = typename GetReplyPayloadFor<RequestPayload, Registry>::type;

template<typename PayloadT, typename Registry>
struct IsRequestPayload {
private:
    using MessageDef = find_message_def_t<PayloadT, Registry>;
public:
    static constexpr bool value =
        !std::is_same_v<MessageDef, void> &&
        MessageDef::has_reply &&
        MessageDef::is_request;
};

template<typename PayloadT, typename Registry>
inline constexpr bool is_request_payload_v = IsRequestPayload<PayloadT, Registry>::value;

// ============================================================================
// Debug / Introspection
// ============================================================================

template<typename Registry>
struct RegistryStats {
    static constexpr size_t total_messages       = Registry::size();
    static constexpr size_t data_messages        = data_message_count<Registry>();
    static constexpr size_t command_messages     = command_message_count<Registry>();
    static constexpr size_t subscription_messages = subscription_message_count<Registry>();
    static constexpr size_t request_messages     = request_message_count<Registry>();
    static constexpr size_t reply_messages       = reply_message_count<Registry>();

    static_assert(data_messages + command_messages + subscription_messages <= total_messages,
                  "Category counts should not exceed total messages");
};

} // namespace registry
} // namespace corerat
