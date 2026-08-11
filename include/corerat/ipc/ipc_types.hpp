#pragma once

/**
 * @file ipc_types.hpp
 * @brief Shared IPC backend types: IpcConfig and IpcResult.
 *
 * Both TiMS and EVL backends use the same config struct and result enum so
 * that mailbox.hpp can pass a single config object to either backend and
 * EvlMailbox does not need to include tims_backend.hpp.
 */

#include <cstddef>
#include <cstdint>
#include <sertial/containers/fixed_string.hpp>

namespace corerat {

// ============================================================================
// Backend-agnostic result codes
// ============================================================================

enum class IpcResult {
    SUCCESS            =  0,
    ERROR_INIT         = -1,
    ERROR_SEND         = -2,
    ERROR_RECEIVE      = -3,
    ERROR_TIMEOUT      = -4,
    ERROR_INVALID_MSG  = -5,
    ERROR_NOT_INIT     = -6
};

// Keep TimsResult as a type alias so existing code compiles unchanged.
using TimsResult = IpcResult;

// ============================================================================
// Backend-agnostic mailbox configuration
// ============================================================================

struct IpcConfig {
    sertial::fixed_string<32> mailbox_name;
    uint32_t mailbox_id          = 0;
    std::size_t max_msg_size     = 4096;
    uint32_t priority            = 0;
    bool     realtime            = false;
    /// Maximum number of remote handles (used by EvlMailbox; ignored by TiMS).
    uint32_t max_remote_handles  = 16;

    IpcConfig() : mailbox_name("default") {}
};

// Keep TimsConfig as a type alias so existing code compiles unchanged.
using TimsConfig = IpcConfig;

} // namespace corerat
