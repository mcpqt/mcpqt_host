// SPDX-License-Identifier: AGPL-3.0-only
#include "stdio_transport.h"

#include "log.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace mcp {

namespace {

constexpr std::size_t kReadChunkBytes = 8 * 1024;

// A small POSIX-flavored helper that turns errno into an std::error_code.
inline std::error_code last_errno_ec(void) noexcept {
    return std::error_code{errno, std::generic_category()};
}

// Set FD_CLOEXEC + O_NONBLOCK on a pipe end. Returns the resulting fd or
// -1 on failure (errno preserved).
int make_nonblocking(int fd) noexcept {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0)
        return -1;
    if (fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
        return -1;
    return fd;
}

// Close an fd ignoring EINTR.
void quiet_close(int fd) noexcept {
    if (fd < 0)
        return;
    while (::close(fd) < 0) {
        if (errno != EINTR)
            break;
    }
}

}  // namespace

StdioTransport::StdioTransport(void) : StdioTransport(Options{}) {}

StdioTransport::StdioTransport(Options opts) : m_opts(opts) {}

StdioTransport::~StdioTransport() {
    close();
    if (m_opts.owns_fds) {
        quiet_close(m_opts.read_fd);
        // Avoid double-closing a write_fd that aliases read_fd.
        if (m_opts.write_fd != m_opts.read_fd)
            quiet_close(m_opts.write_fd);
    }
}

void StdioTransport::on_message(MessageCallback cb) {
    m_on_message = std::move(cb);
}
void StdioTransport::on_error(ErrorCallback cb) {
    m_on_error = std::move(cb);
}
void StdioTransport::on_close(CloseCallback cb) {
    m_on_close = std::move(cb);
}

void StdioTransport::start(void) {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return;  // already running
    }
    m_closing.store(false, std::memory_order_release);
    m_close_fired.store(false, std::memory_order_release);

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        m_running.store(false, std::memory_order_release);
        deliver_error(last_errno_ec());
        return;
    }
    if (make_nonblocking(pipefd[0]) < 0 || make_nonblocking(pipefd[1]) < 0) {
        const auto ec = last_errno_ec();
        quiet_close(pipefd[0]);
        quiet_close(pipefd[1]);
        m_running.store(false, std::memory_order_release);
        deliver_error(ec);
        return;
    }
    m_wakeup_read_fd = pipefd[0];
    m_wakeup_write_fd = pipefd[1];

    m_read_thread = std::thread([this]() noexcept { this->read_loop(); });
}

void StdioTransport::close(void) {
    // Only the first caller to flip `m_closing` does the wake + cleanup. All
    // callers wait for the read thread to finish before returning, so the
    // destructor's contract ("on return, the worker is gone") holds even
    // when EOF or an error has already retired the loop.
    const bool first = !m_closing.exchange(true, std::memory_order_acq_rel);
    m_running.store(false, std::memory_order_release);

    if (first && m_wakeup_write_fd >= 0) {
        const char b = 0;
        for (;;) {
            ssize_t n = ::write(m_wakeup_write_fd, &b, 1);
            if (n == 1)
                break;
            if (n < 0 && errno == EINTR)
                continue;
            break;  // EAGAIN: the wake pipe is full, which still wakes the
                    // reader.
        }
    }

    if (m_read_thread.joinable())
        m_read_thread.join();

    if (first) {
        quiet_close(m_wakeup_read_fd);
        quiet_close(m_wakeup_write_fd);
        m_wakeup_read_fd = -1;
        m_wakeup_write_fd = -1;
    }

    // If a concurrent send() is in flight, wait for it to release the
    // write lock before we return. The destructor immediately follows
    // close() with fd cleanup (when owns_fds=true), so we must not let
    // a sender be mid-write() on a fd we're about to close.
    {
        std::lock_guard<std::mutex> lk(m_write_mutex);
    }

    fire_close();
}

std::error_code StdioTransport::send(std::string_view frame) {
    if (!m_running.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    if (frame.find('\n') != std::string_view::npos) {
        // Per spec: frames MUST NOT contain embedded newlines.
        return std::make_error_code(std::errc::invalid_argument);
    }

    std::lock_guard<std::mutex> lk(m_write_mutex);
    const char* data = frame.data();
    std::size_t left = frame.size();
    while (left > 0) {
        ssize_t n = ::write(m_opts.write_fd, data, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return last_errno_ec();
        }
        data += n;
        left -= static_cast<std::size_t>(n);
    }

    static const char nl = '\n';
    while (true) {
        ssize_t n = ::write(m_opts.write_fd, &nl, 1);
        if (n == 1)
            break;
        if (n < 0 && errno == EINTR)
            continue;
        return last_errno_ec();
    }
    return {};
}

void StdioTransport::read_loop(void) noexcept {
    std::string buffer;
    buffer.reserve(kReadChunkBytes);

    char chunk[kReadChunkBytes];

    while (m_running.load(std::memory_order_acquire)) {
        struct pollfd fds[2];
        fds[0].fd = m_opts.read_fd;
        fds[0].events = POLLIN;
        fds[1].fd = m_wakeup_read_fd;
        fds[1].events = POLLIN;

        int pr = ::poll(fds, 2, -1 /* infinite */);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            deliver_error(last_errno_ec());
            break;
        }

        if (fds[1].revents & POLLIN) {
            // Wake-up; either we're closing or the pipe has stale bytes.
            char drain[64];
            while (::read(m_wakeup_read_fd, drain, sizeof(drain)) >
                   0) { /* drain */
            }
            if (m_closing.load(std::memory_order_acquire))
                break;
        }

        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            deliver_error(std::make_error_code(std::errc::io_error));
            break;
        }

        if (fds[0].revents & POLLIN) {
            ssize_t n = ::read(m_opts.read_fd, chunk, sizeof(chunk));
            if (n == 0) {
                // EOF on the input.
                break;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                deliver_error(last_errno_ec());
                break;
            }

            for (ssize_t i = 0; i < n; ++i) {
                const char c = chunk[i];
                if (c == '\n') {
                    // Tolerate CRLF: peers writing through Windows-style
                    // line endings get their trailing \r stripped before
                    // we hand the buffer up. Per MCP spec the delimiter
                    // is '\n', so this keeps interop with peers that
                    // happen to ship CRLF without a fight.
                    if (!buffer.empty() && buffer.back() == '\r') {
                        buffer.pop_back();
                    }
                    if (!buffer.empty() && m_on_message) {
                        try {
                            m_on_message(std::move(buffer));
                        } catch (const std::exception& e) {
                            MCP_LOG_ERROR(std::string{"on_message threw: "} +
                                          e.what());
                        } catch (...) {
                            MCP_LOG_ERROR(
                                "on_message threw a non-std exception");
                        }
                    }
                    buffer.clear();
                    buffer.reserve(kReadChunkBytes);
                    continue;
                }
                if (buffer.size() >= m_opts.max_frame_bytes) {
                    deliver_error(
                        std::make_error_code(std::errc::message_size));
                    return fire_close();
                }
                buffer.push_back(c);
            }
        } else if (fds[0].revents & POLLHUP) {
            break;  // peer closed
        }
    }

    fire_close();
}

void StdioTransport::deliver_error(std::error_code ec) noexcept {
    if (!m_on_error)
        return;
    try {
        m_on_error(ec);
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"on_error threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("on_error threw a non-std exception");
    }
}

void StdioTransport::fire_close(void) noexcept {
    bool already = m_close_fired.exchange(true, std::memory_order_acq_rel);
    if (already)
        return;
    // Deliberately do NOT flip `m_running` here. EOF on the input
    // doesn't close the output direction; outstanding worker handlers
    // dispatched by the Session still need to write their responses.
    // close() is the canonical place that disables sends. The
    // on_close hook is purely informational — it lets the Session
    // start its drain so close() can be called from the right place.
    if (!m_on_close)
        return;
    try {
        m_on_close();
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"on_close threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("on_close threw a non-std exception");
    }
}

}  // namespace mcp
