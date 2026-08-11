/**
 * @file logging.hpp
 * @brief CoreRaT RT logging — single include for user code.
 *
 * Pulls in all logging headers and defines the RTLOG_* convenience macros.
 *
 * Quick start:
 * @code
 *   #include <corerat/logging/logging.hpp>
 *
 *   class MySensor {
 *       corerat::RtLogger<64> logger_{mailbox_id_};
 *       corerat::TerminalSink terminal_sink_{};
 *
 *       int moduleInit() {
 *           logger_.add_sink(&terminal_sink_);
 *           logger_.start_drain();
 *           return 0;
 *       }
 *
 *       void moduleCleanup() { logger_.stop_drain(); }
 *
 *       void loop() {
 *           RTLOG_INFO(logger_)  << "speed=" << speed_ << " hdg=" << hdg_;
 *           RTLOG_ERROR(logger_) << "fault=0x" << corerat::RtHex{code};
 *           RTLOG_DEBUG(logger_) << "temp=" << temp_ << " deg";
 *       }
 *   };
 * @endcode
 *
 * @note All RTLOG_* macros create a temporary RtLogStream on the stack.
 *       The message is assembled via operator<<, then committed atomically
 *       in the destructor at the semicolon.  OOB-safe on EVL platform.
 */
#pragma once

#include "corerat/logging/log_level.hpp"
#include "corerat/logging/detail/rt_append.hpp"
#include "corerat/logging/rt_log_entry.hpp"
#include "corerat/logging/rt_logger_base.hpp"
#include "corerat/logging/rt_log_stream.hpp"
#include "corerat/logging/log_sink.hpp"
#include "corerat/logging/rt_logger.hpp"

// ============================================================================
// RTLOG_* macros
//
// Each macro creates a temporary RtLogStream<> bound to the given logger.
// The stream is inactive (zero-cost) when lvl > logger.level().
//
// Usage:   RTLOG_INFO(my_logger) << "value=" << v;
// ============================================================================

/// @brief Log at Fatal level — always shown, system-critical.
#define RTLOG_FATAL(logger) corerat::RtLogStream<>((logger), corerat::LogLevel::Fatal)

/// @brief Log at Error level.
#define RTLOG_ERROR(logger) corerat::RtLogStream<>((logger), corerat::LogLevel::Error)

/// @brief Log at Warn level.
#define RTLOG_WARN(logger)  corerat::RtLogStream<>((logger), corerat::LogLevel::Warn)

/// @brief Log at Info level.
#define RTLOG_INFO(logger)  corerat::RtLogStream<>((logger), corerat::LogLevel::Info)

/// @brief Log at Debug level.
#define RTLOG_DEBUG(logger) corerat::RtLogStream<>((logger), corerat::LogLevel::Debug)

/// @brief Log at Trace level — fine-grained hot-path tracing.
#define RTLOG_TRACE(logger) corerat::RtLogStream<>((logger), corerat::LogLevel::Trace)
