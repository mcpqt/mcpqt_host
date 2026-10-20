// SPDX-License-Identifier: AGPL-3.0-only
//
// POSIX-flavoured stdio transport for MCP.
//
// Reads newline-delimited JSON-RPC frames from a "read" file descriptor
// (default: stdin / FD 0) and writes them to a "write" file descriptor
// (default: stdout / FD 1). Per the MCP spec, frames MUST NOT contain
// embedded newlines, and stdout MUST NOT carry anything other than valid
// MCP frames — diagnostics belong on stderr.
//
// The read loop uses `poll(2)` so that `close()` can interrupt a read in
// progress via a self-pipe wake-up; the legacy implementation got this
// wrong and deadlocked on shutdown.

#pragma once

#if defined(_WIN32)
#error "mcp::StdioTransport is POSIX-only (poll(2) + file descriptors). " \
    "On Windows, use the Streamable HTTP transport, or contribute a " \
    "Win32 stdio transport (overlapped I/O on stdin/stdout handles)."
#endif

#include "transport.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

namespace mcp {

class StdioTransport final : public Transport {
public:
    struct Options {
        /// File descriptor to read from (default: STDIN_FILENO).
        int read_fd = 0;
        /// File descriptor to write to (default: STDOUT_FILENO).
        int write_fd = 1;
        /// Maximum size of a single frame the parser will accept. Frames
        /// exceeding this size cause a transport error and disconnect.
        std::size_t max_frame_bytes = 16 * 1024 * 1024;
        /// Whether the transport owns and should `close()` the file
        /// descriptors on destruction. Stdin/stdout are the typical case
        /// (false). Pipe-based test transports usually want true.
        bool owns_fds = false;
    };

    StdioTransport(void);
    explicit StdioTransport(Options opts);
    ~StdioTransport() override;

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb) override;
    void on_close(CloseCallback cb) override;

    void start(void) override;
    void close(void) override;

    [[nodiscard]] std::error_code send(std::string_view frame) override;
    [[nodiscard]] bool is_open(void) const noexcept override {
        return m_running.load(std::memory_order_acquire);
    }

private:
    void read_loop(void) noexcept;
    void deliver_error(std::error_code ec) noexcept;
    void fire_close(void) noexcept;

    Options m_opts;

    int m_wakeup_read_fd = -1;  // self-pipe read end (closed-on-stop)
    int m_wakeup_write_fd = -1;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_closing{false};
    std::atomic<bool> m_close_fired{false};

    std::thread m_read_thread;
    std::mutex m_write_mutex;  // serializes writes to Options::write_fd

    // Callbacks. Set once before start(); read without locking thereafter.
    MessageCallback m_on_message;
    ErrorCallback m_on_error;
    CloseCallback m_on_close;
};

}  // namespace mcp
