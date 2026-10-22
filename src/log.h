// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Internal logging for the mcpqt library itself. This is *not* the MCP
// protocol-level logging that servers use to emit log messages to clients
// (see `notifications/message` in the spec). This module is for diagnostic
// output from the SDK to stderr — never to stdout, because stdout is owned by
// the stdio transport.

#include <QString>
#include <atomic>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>

namespace mcp
{

  enum class LogLevel : int
  {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    off = 5,
  };

  [[nodiscard]] QString to_string(LogLevel l) noexcept;

  /// Sink signature: receives a fully-formatted line (no trailing newline).
  /// The sink is invoked synchronously from whichever thread emitted the log.
  /// The default sink (the core's minimal fallback) writes a timestamped line
  /// to `stderr`.
  using LogSink = std::function<void(LogLevel, const QString&)>;

  /// Replace the global log sink. Pass `nullptr` to restore the core's default
  /// fallback sink. Returns false (and leaves the sink untouched) if a sink
  /// has already been claimed via `claim_log_sink()` — a claimed sink is
  /// exclusive and cannot be replaced. Thread-safe.
  [[nodiscard]] bool set_log_sink(LogSink sink);

  /// Install `sink` and take exclusive ownership of process-wide logging for
  /// the remainder of the process. The first successful claim wins: subsequent
  /// `claim_log_sink()` and `set_log_sink()` calls fail, so one logging
  /// provider (typically a plugin) cannot be displaced by another. A null sink
  /// is rejected. Thread-safe.
  ///
  /// The claimed sink executes code owned by the caller, so the caller must
  /// stay loaded for the process lifetime; `PluginManager` enforces this by
  /// pinning the plugin that claims logging.
  [[nodiscard]] bool claim_log_sink(LogSink sink);

  /// True once a sink has been claimed via `claim_log_sink()`.
  [[nodiscard]] bool log_sink_claimed(void) noexcept;

  /// Set the global minimum level. Calls below this level are dropped before
  /// any message is formatted. Thread-safe (atomic).
  void set_log_level(LogLevel level) noexcept;

  [[nodiscard]] LogLevel log_level(void) noexcept;

  /// Low-level entry point. Prefer the `MCP_LOG_*` macros below, which
  /// short-circuit at the call site when the level is disabled.
  void log_message(LogLevel level, const QString& msg,
                   std::source_location loc = std::source_location::current());

} // namespace mcp

#define MCP_LOG_TRACE(msg) ::mcp::log_message(::mcp::LogLevel::trace, msg, std::source_location::current())
#define MCP_LOG_DEBUG(msg) ::mcp::log_message(::mcp::LogLevel::debug, msg, std::source_location::current())
#define MCP_LOG_INFO(msg)  ::mcp::log_message(::mcp::LogLevel::info,  msg, std::source_location::current())
#define MCP_LOG_WARN(msg)  ::mcp::log_message(::mcp::LogLevel::warn,  msg, std::source_location::current())
#define MCP_LOG_ERROR(msg) ::mcp::log_message(::mcp::LogLevel::error, msg, std::source_location::current())
// clang-format on
