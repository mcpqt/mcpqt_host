// SPDX-License-Identifier: AGPL-3.0-only
#include "in_memory_transport.h"

#include "log.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace mcp::test {

// One side's queue + flags + cv. Two of them in a Channel make a pair.
struct Endpoint {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<QByteArray> q;
    bool closed = false;
};

struct Channel {
    Endpoint a;  // A consumes from here
    Endpoint b;  // B consumes from here
};

namespace {

inline Endpoint& endpoint_for(Channel& ch, InMemoryTransport::Side s) {
    return s == InMemoryTransport::Side::kA ? ch.a : ch.b;
}

inline Endpoint& peer_endpoint(Channel& ch, InMemoryTransport::Side s) {
    return s == InMemoryTransport::Side::kA ? ch.b : ch.a;
}

}  // namespace

InMemoryTransport::InMemoryTransport(Side side, std::shared_ptr<Channel> ch)
    : m_side(side), m_ch(std::move(ch)) {}

InMemoryTransport::~InMemoryTransport() {
    close();
}

void InMemoryTransport::on_message(MessageCallback cb) {
    m_on_message = std::move(cb);
}
void InMemoryTransport::on_error(ErrorCallback cb) {
    m_on_error = std::move(cb);
}
void InMemoryTransport::on_close(CloseCallback cb) {
    m_on_close = std::move(cb);
}

void InMemoryTransport::start(void) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return;
    }
    m_closed.store(false, std::memory_order_release);
    m_worker = std::thread([this] { run_dispatch(); });
}

void InMemoryTransport::close(void) {
    const bool first = !m_closed.exchange(true, std::memory_order_acq_rel);
    {
        // Wake our own dispatch loop.
        auto& ours = endpoint_for(*m_ch, m_side);
        std::lock_guard<std::mutex> lk(ours.mu);
        ours.closed = true;
        ours.cv.notify_all();
    }
    {
        // Tell the peer we're gone: setting the peer's incoming (its `ours`)
        // endpoint as closed makes its dispatch loop unwind once drained.
        auto& peer = peer_endpoint(*m_ch, m_side);
        std::lock_guard<std::mutex> lk(peer.mu);
        peer.closed = true;
        peer.cv.notify_all();
    }
    if (m_worker.joinable())
        m_worker.join();

    if (first && m_on_close) {
        try {
            m_on_close();
        } catch (const std::exception& e) {
            MCP_LOG_ERROR(QStringLiteral("on_close threw: ") + e.what());
        }
    }
}

std::error_code InMemoryTransport::send(QByteArrayView frame) {
    if (!m_started.load(std::memory_order_acquire) ||
        m_closed.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    auto& peer = peer_endpoint(*m_ch, m_side);
    std::lock_guard<std::mutex> lk(peer.mu);
    if (peer.closed) {
        return std::make_error_code(std::errc::not_connected);
    }
    peer.q.emplace_back(frame.toByteArray());
    peer.cv.notify_one();
    return {};
}

bool InMemoryTransport::is_open(void) const noexcept {
    return m_started.load(std::memory_order_acquire) &&
           !m_closed.load(std::memory_order_acquire);
}

void InMemoryTransport::run_dispatch(void) {
    auto& ours = endpoint_for(*m_ch, m_side);
    bool peer_initiated_close = false;
    while (true) {
        QByteArray frame;
        {
            std::unique_lock<std::mutex> lk(ours.mu);
            ours.cv.wait(lk, [&] { return ours.closed || !ours.q.empty(); });
            if (ours.closed && ours.q.empty()) {
                // If we're being torn down by the peer (m_closed flag was
                // flipped before our own close()), surface that as
                // on_close so user code mirrors EOF semantics.
                peer_initiated_close =
                    !m_closed.load(std::memory_order_acquire);
                break;
            }
            frame = std::move(ours.q.front());
            ours.q.pop_front();
        }
        if (m_on_message) {
            try {
                m_on_message(std::move(frame));
            } catch (const std::exception& e) {
                MCP_LOG_ERROR(QStringLiteral("on_message threw: ") + e.what());
            }
        }
    }
    if (peer_initiated_close) {
        m_closed.store(true, std::memory_order_release);
        if (m_on_close) {
            try {
                m_on_close();
            } catch (const std::exception& e) {
                MCP_LOG_ERROR(QStringLiteral("on_close threw: ") + e.what());
            }
        }
    }
}

InMemoryTransportPair make_in_memory_pair(void) {
    auto ch = std::make_shared<Channel>();
    return {
        std::make_unique<InMemoryTransport>(InMemoryTransport::Side::kA, ch),
        std::make_unique<InMemoryTransport>(InMemoryTransport::Side::kB, ch),
    };
}

}  // namespace mcp::test
