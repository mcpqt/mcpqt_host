// SPDX-License-Identifier: AGPL-3.0-only
//
// In-memory paired transport for tests. A shared `Channel` holds two
// in/out queues; each `InMemoryTransport` is a unique-owned end on top
// of the channel and is therefore movable into a Session.

#pragma once

#include "transport.h"

#include <QByteArrayView>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace mcp::test {

class InMemoryTransport;

struct Channel;  // forward

struct InMemoryTransportPair {
    std::unique_ptr<InMemoryTransport> a;
    std::unique_ptr<InMemoryTransport> b;
};

InMemoryTransportPair make_in_memory_pair(void);

class InMemoryTransport final : public Transport {
public:
    enum class Side : int { kA = 0, kB = 1 };

    InMemoryTransport(Side side, std::shared_ptr<Channel> ch);
    ~InMemoryTransport() override;

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb) override;
    void on_close(CloseCallback cb) override;
    void start(void) override;
    void close(void) override;
    [[nodiscard]] std::error_code send(QByteArrayView frame) override;
    [[nodiscard]] bool is_open(void) const noexcept override;

private:
    void run_dispatch(void);

    Side m_side;
    std::shared_ptr<Channel> m_ch;

    std::atomic<bool> m_started{false};
    std::atomic<bool> m_closed{false};

    std::thread m_worker;

    MessageCallback m_on_message;
    ErrorCallback m_on_error;
    CloseCallback m_on_close;
};

}  // namespace mcp::test
