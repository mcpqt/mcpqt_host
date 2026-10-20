// SPDX-License-Identifier: AGPL-3.0-only
#include "http_client_transport.h"

#include "log.h"

#include <QByteArray>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace mcp {

// =====================================================================
// pImpl
// =====================================================================

struct HttpClientTransport::ClientImpl {
    explicit ClientImpl(QUrl u) : url(std::move(u)) {}
    // Fully-parsed endpoint URL. QNetworkAccessManager is created
    // per-operation (on the thread that owns the operation), so we
    // don't hold long-lived Qt objects here — just the URL.
    QUrl url;
};

namespace {

/// One SSE event distilled out of a buffer. We carry the
/// MCP-relevant fields plus side-channel info (`id`, `retry`) so
/// the GET worker can implement spec-mandated resumability and
/// retry timing.
struct SseEvent {
    std::string data;
    std::optional<std::string> id;
    std::optional<int> retry_ms;  // server-suggested reconnect delay
};

// Iterate SSE frames in a buffer, calling `cb(SseEvent)` for each
// complete event. `buf` is read up through the last complete event
// terminator (`\n\n` or `\r\n\r\n`); the unconsumed tail stays in
// `buf` for the next round. We capture `data:`, `id:`, and `retry:`
// per the SSE spec; `event:` is ignored (MCP doesn't use named
// events).
template <typename Callback> void drain_sse(std::string& buf, Callback&& cb) {
    while (true) {
        std::size_t end = std::string::npos;
        std::size_t end_marker_len = 0;
        for (std::size_t i = 0; i + 1 < buf.size(); ++i) {
            if (buf[i] == '\n' && buf[i + 1] == '\n') {
                end = i;
                end_marker_len = 2;
                break;
            }
            if (i + 3 < buf.size() && buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                end = i;
                end_marker_len = 4;
                break;
            }
        }
        if (end == std::string::npos)
            return;

        const std::string event{buf.data(), end};
        buf.erase(0, end + end_marker_len);

        SseEvent se;
        std::size_t line_start = 0;
        while (line_start <= event.size()) {
            const auto line_end = event.find('\n', line_start);
            std::string_view line;
            if (line_end == std::string::npos) {
                line = std::string_view{event}.substr(line_start);
                line_start = event.size() + 1;
            } else {
                line = std::string_view{event}.substr(line_start,
                                                      line_end - line_start);
                line_start = line_end + 1;
            }
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty() || line[0] == ':')
                continue;  // comment/keepalive
            const auto colon = line.find(':');
            std::string_view field, value;
            if (colon == std::string_view::npos) {
                field = line;
                value = {};
            } else {
                field = line.substr(0, colon);
                value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ')
                    value.remove_prefix(1);
            }
            if (field == "data") {
                if (!se.data.empty())
                    se.data += '\n';
                se.data.append(value);
            } else if (field == "id") {
                se.id = std::string{value};
            } else if (field == "retry") {
                // Per SSE spec: integer milliseconds. Ignore on
                // parse error / negative.
                try {
                    int v = std::stoi(std::string{value});
                    if (v >= 0)
                        se.retry_ms = v;
                } catch (...) {
                }
            }
        }
        if (!se.data.empty() || se.id.has_value() || se.retry_ms.has_value()) {
            cb(std::move(se));
        }
    }
}

// Attach the headers common to every outbound request: protocol
// version, negotiated session id, bearer token, resumability cursor,
// and caller-supplied extras.
void set_common_headers(QNetworkRequest& req,
                        const HttpClientTransport::Options& opts,
                        const std::optional<std::string>& sid,
                        const std::string& access_token,
                        const std::string& last_event_id) {
    req.setRawHeader("MCP-Protocol-Version",
                     QByteArray::fromStdString(opts.protocol_version));
    if (sid.has_value()) {
        req.setRawHeader("Mcp-Session-Id", QByteArray::fromStdString(*sid));
    }
    if (!access_token.empty()) {
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") +
                             QByteArray::fromStdString(access_token));
    }
    if (!last_event_id.empty()) {
        req.setRawHeader("Last-Event-ID",
                         QByteArray::fromStdString(last_event_id));
    }
    for (const auto& [k, v] : opts.extra_headers) {
        req.setRawHeader(QByteArray::fromStdString(k),
                         QByteArray::fromStdString(v));
    }
}

}  // namespace

// =====================================================================
// Construction / destruction
// =====================================================================

HttpClientTransport::HttpClientTransport(Options opts)
    : m_opts(std::move(opts)) {
    QUrl url{QString::fromStdString(m_opts.url)};
    if (!url.isValid() || url.scheme().isEmpty()) {
        throw std::invalid_argument(
            "HttpClientTransport: URL must contain a scheme "
            "(http:// or https://)");
    }
    m_impl = std::make_unique<ClientImpl>(std::move(url));
    if (m_opts.session_id.has_value()) {
        m_session_id = *m_opts.session_id;
    }
    // Seed the mutable m_access_token from Options. The Options copy
    // is no longer the source of truth; set_access_token rotates this
    // member so on_unauthorized callbacks can drive a token refresh.
    if (m_opts.access_token.has_value()) {
        m_access_token = *m_opts.access_token;
    }
}

HttpClientTransport::~HttpClientTransport() {
    close();
}

void HttpClientTransport::on_message(MessageCallback cb) {
    m_on_message = std::move(cb);
}
void HttpClientTransport::on_error(ErrorCallback cb) {
    m_on_error = std::move(cb);
}
void HttpClientTransport::on_close(CloseCallback cb) {
    m_on_close = std::move(cb);
}

std::optional<std::string> HttpClientTransport::session_id(void) const {
    std::lock_guard<std::mutex> lk(m_session_id_mu);
    return m_session_id;
}

// =====================================================================
// Lifecycle
// =====================================================================

void HttpClientTransport::start(void) {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return;
    }
    m_closed.store(false, std::memory_order_release);
    m_close_fired.store(false, std::memory_order_release);
    m_worker = std::thread([this] { worker_loop(); });
    if (m_opts.open_get_stream) {
        m_get_thread = std::thread([this] { run_get_stream(); });
    }
}

void HttpClientTransport::close(void) {
    if (m_closed.exchange(true, std::memory_order_acq_rel)) {
        if (m_worker.joinable())
            m_worker.join();
        if (m_get_thread.joinable())
            m_get_thread.join();
        return;
    }

    // Spec: a client that no longer needs a session SHOULD send an
    // HTTP DELETE on the MCP path so the server can free per-session
    // state immediately. Best-effort: failure here is fine — close()
    // always proceeds.
    std::optional<std::string> sid;
    {
        std::lock_guard<std::mutex> lk(m_session_id_mu);
        sid = m_session_id;
    }
    if (sid.has_value() && !sid->empty() && m_impl) {
        try {
            std::string token;
            {
                std::lock_guard<std::mutex> lk(m_auth_mu);
                token = m_access_token;
            }
            QNetworkAccessManager nam;
            QNetworkRequest req(m_impl->url);
            set_common_headers(req, m_opts, sid, token, "");
            req.setTransferTimeout(2000);
            QNetworkReply* reply = nam.deleteResource(req);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop,
                             &QEventLoop::quit);
            loop.exec();
        } catch (...) {
            // best-effort; ignore
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        m_out_cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(m_session_id_mu);
        m_session_id_cv.notify_all();
    }

    if (m_worker.joinable())
        m_worker.join();
    if (m_get_thread.joinable())
        m_get_thread.join();

    {
        std::unique_lock<std::mutex> lk(m_inflight_mu);
        m_inflight_cv.wait(lk, [this] { return m_inflight == 0; });
    }
    fire_close();
}

void HttpClientTransport::fire_close(void) noexcept {
    if (m_close_fired.exchange(true, std::memory_order_acq_rel))
        return;
    if (!m_on_close)
        return;
    try {
        m_on_close();
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_close threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_close threw a non-std exception");
    }
}

void HttpClientTransport::deliver_error(std::error_code ec) noexcept {
    if (!m_on_error)
        return;
    try {
        m_on_error(ec);
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_error threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_error threw a non-std exception");
    }
}

void HttpClientTransport::deliver_frame(std::string raw) {
    if (!m_on_message)
        return;
    try {
        m_on_message(std::move(raw));
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_message threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR(
            "HttpClientTransport on_message threw a non-std exception");
    }
}

// =====================================================================
// Send path
// =====================================================================

std::error_code HttpClientTransport::send(std::string_view frame) {
    if (!m_started.load(std::memory_order_acquire) ||
        m_closed.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        m_out.emplace_back(frame);
        m_out_cv.notify_one();
    }
    return {};
}

void HttpClientTransport::worker_loop(void) {
    // Each outbound frame is processed on its own thread so a slow
    // POST (e.g. one held open by the server while it's awaiting a
    // sampling reply that needs to come back through us) doesn't
    // serialise with later sends. QNetworkAccessManager is asynchronous
    // and would allow one manager to multiplex these, but keeping the
    // per-send thread preserves the exact concurrency semantics the
    // transport had before (and each thread drives its own QNAM +
    // QEventLoop).
    while (true) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lk(m_out_mu);
            m_out_cv.wait(lk, [this] {
                return m_closed.load(std::memory_order_acquire) ||
                       !m_out.empty();
            });
            if (m_closed.load(std::memory_order_acquire) && m_out.empty())
                return;
            frame = std::move(m_out.front());
            m_out.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(m_inflight_mu);
            ++m_inflight;
        }
        std::thread([this, f = std::move(frame)]() mutable {
            process_send(std::move(f));
            std::lock_guard<std::mutex> lk(m_inflight_mu);
            --m_inflight;
            m_inflight_cv.notify_all();
        }).detach();
    }
}

void HttpClientTransport::process_send(std::string frame) {
    std::optional<std::string> sid;
    {
        std::lock_guard<std::mutex> lk(m_session_id_mu);
        sid = m_session_id;
    }
    std::string token;
    {
        std::lock_guard<std::mutex> lk(m_auth_mu);
        token = m_access_token;
    }

    // Each send runs on its own thread (worker_loop dispatches), so
    // we create a per-call QNetworkAccessManager + event loop. This
    // keeps the "many concurrent POSTs, no serialisation" property
    // the transport relies on.
    QNetworkAccessManager nam;
    QNetworkRequest req(m_impl->url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json, text/event-stream");
    set_common_headers(req, m_opts, sid, token, "");
    if (m_opts.request_timeout.count() > 0) {
        req.setTransferTimeout(
            static_cast<int>(m_opts.request_timeout.count()));
    }

    QNetworkReply* reply = nam.post(
        req, QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Capture session id if the server set one. Notify the GET
    // worker so it can issue its first GET right away, instead of
    // sleeping out a poll interval.
    const QByteArray sid_hdr = reply->rawHeader("Mcp-Session-Id");
    if (!sid_hdr.isEmpty()) {
        bool first_set = false;
        {
            std::lock_guard<std::mutex> lk(m_session_id_mu);
            first_set = !m_session_id.has_value();
            m_session_id = sid_hdr.toStdString();
        }
        if (first_set)
            m_session_id_cv.notify_all();
    }

    // A status of 0 means the request never got an HTTP response
    // (connection refused, timeout, TLS failure, ...).
    if (status == 0) {
        deliver_error(std::make_error_code(std::errc::io_error));
        return;
    }
    if (status == 202) {
        // Empty 202 Accepted — server absorbed a notification or response.
        // A successful response also re-arms the on_unauthorized
        // callback so the next 401-batch can fire it again.
        m_auth_callback_armed.store(true, std::memory_order_release);
        return;
    }
    if (status == 401 || status == 403) {
        // Surface OAuth challenge to the application via the
        // optional callback. The transport itself can't drive the
        // OAuth dance; the application either updates `access_token`
        // (via set_access_token) and retries, or destroys this
        // transport and creates a new one with the new token.
        //
        // Many in-flight POSTs hitting a 401 batch would otherwise
        // each call the callback, racing the application's refresh.
        // Fire ONCE per batch — re-arm on the next 2xx.
        const std::string challenge =
            reply->rawHeader("WWW-Authenticate").toStdString();
        bool expected = true;
        if (m_opts.on_unauthorized &&
            m_auth_callback_armed.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel)) {
            try {
                m_opts.on_unauthorized(status, challenge);
            } catch (...) {
            }
        }
        deliver_error(std::make_error_code(std::errc::permission_denied));
        return;
    }
    if (status < 200 || status >= 300) {
        deliver_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
    // Any 2xx re-arms the dedup flag.
    m_auth_callback_armed.store(true, std::memory_order_release);

    const std::string ct = reply->rawHeader("Content-Type").toStdString();
    std::string body = reply->readAll().toStdString();

    if (ct.find("text/event-stream") != std::string::npos) {
        drain_sse(body, [this](SseEvent se) {
            // Track the last-seen event id so a future reconnect
            // (typically on the GET stream) can resume via
            // Last-Event-ID.
            if (se.id.has_value() && !se.id->empty()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_last_event_id = *se.id;
            }
            if (!se.data.empty())
                deliver_frame(std::move(se.data));
        });
        return;
    }
    // `application/json` and unknown content-types both pass the body
    // through unchanged.
    deliver_frame(std::move(body));
}

// =====================================================================
// GET stream (server-initiated)
// =====================================================================

void HttpClientTransport::run_get_stream(void) noexcept {
    // The server's GET handler needs Mcp-Session-Id; it has no way
    // to associate an anonymous GET with the right session. So this
    // worker waits on a cv for the session id to be negotiated by
    // a POST response, and issues the GET as soon as it arrives —
    // no polling latency between "initialize() returned" and "GET
    // stream is open" (which a tool handler triggering server.sample()
    // would otherwise race).
    while (m_started.load(std::memory_order_acquire) &&
           !m_closed.load(std::memory_order_acquire)) {
        std::optional<std::string> sid;
        {
            std::unique_lock<std::mutex> lk(m_session_id_mu);
            m_session_id_cv.wait(lk, [this] {
                return m_session_id.has_value() ||
                       m_closed.load(std::memory_order_acquire);
            });
            if (m_closed.load(std::memory_order_acquire))
                return;
            sid = m_session_id;
        }
        if (!sid.has_value())
            continue;

        std::string token;
        {
            std::lock_guard<std::mutex> lk(m_auth_mu);
            token = m_access_token;
        }
        // Resumability: hand back the last event id we saw on this
        // session so the server can replay anything we missed during
        // the disconnect.
        std::string last_event_id;
        {
            std::lock_guard<std::mutex> lk(m_get_mu_state);
            last_event_id = m_last_event_id;
        }

        QNetworkAccessManager nam;
        QNetworkRequest req(m_impl->url);
        req.setRawHeader("Accept", "text/event-stream");
        set_common_headers(req, m_opts, sid, token, last_event_id);
        // The GET stream stays open indefinitely; no transfer timeout.
        // Heartbeats keep it alive.

        std::string buf;
        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            buf.append(reply->readAll());
            drain_sse(buf, [this](SseEvent se) {
                if (se.id.has_value() && !se.id->empty()) {
                    std::lock_guard<std::mutex> lk(m_get_mu_state);
                    m_last_event_id = *se.id;
                }
                if (se.retry_ms.has_value()) {
                    std::lock_guard<std::mutex> lk(m_get_mu_state);
                    m_next_retry_ms = se.retry_ms;
                }
                if (!se.data.empty())
                    deliver_frame(std::move(se.data));
            });
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        loop.exec();

        // Drain anything that arrived after the last readyRead.
        buf.append(reply->readAll());
        drain_sse(buf, [this](SseEvent se) {
            if (se.id.has_value() && !se.id->empty()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_last_event_id = *se.id;
            }
            if (se.retry_ms.has_value()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_next_retry_ms = se.retry_ms;
            }
            if (!se.data.empty())
                deliver_frame(std::move(se.data));
        });

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // 405 Method Not Allowed is the spec's way of saying "no
        // server-initiated stream"; retry isn't useful, exit cleanly.
        if (status == 405)
            return;

        // 401/403: the access token has expired or been revoked.
        // Without this branch, run_get_stream would busy-retry every
        // 200 ms forever, blasting the IdP with no way for the
        // application to refresh the token. Surface the challenge
        // and exit; the application is expected to call
        // set_access_token() and start a new transport (or restart
        // this one) to recover.
        if (status == 401 || status == 403) {
            if (m_opts.on_unauthorized) {
                const std::string challenge =
                    reply->rawHeader("WWW-Authenticate").toStdString();
                try {
                    m_opts.on_unauthorized(status, challenge);
                } catch (...) {
                }
            }
            return;
        }

        // 404: the server has expired our session. Per spec, the
        // client MUST treat this as "session is gone" — clear the
        // negotiated id, and exit so the next outbound POST
        // re-initializes. We don't re-open a GET stream from here:
        // the next initialize will give us a fresh session id, at
        // which point the GET worker re-arms.
        if (status == 404) {
            {
                std::lock_guard<std::mutex> lk(m_session_id_mu);
                m_session_id.reset();
            }
            return;
        }

        if (m_closed.load(std::memory_order_acquire))
            return;

        // Backoff before reconnecting after EOF/error. Honour the
        // server's `retry:` if it sent one; clamp to a sane band so
        // a misbehaving server can't drive us into a hot loop or
        // perma-sleep. Default 200ms when the server hasn't said
        // otherwise.
        std::chrono::milliseconds delay{200};
        {
            std::lock_guard<std::mutex> lk(m_get_mu_state);
            if (m_next_retry_ms.has_value()) {
                int v = *m_next_retry_ms;
                if (v < 50)
                    v = 50;
                if (v > 60'000)
                    v = 60'000;
                delay = std::chrono::milliseconds{v};
            }
        }
        std::this_thread::sleep_for(delay);
    }
}

// =====================================================================
// Mutable access-token API
// =====================================================================

void HttpClientTransport::set_access_token(std::string token) {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    m_access_token = std::move(token);
}

std::string HttpClientTransport::access_token(void) const {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    return m_access_token;
}

}  // namespace mcp
