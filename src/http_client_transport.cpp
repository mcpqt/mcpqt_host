// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
#include "mcp/http_client_transport.hpp"

#include "mcp/log.hpp"

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
    std::string                data;
    std::optional<std::string> id;
    std::optional<int>         retry_ms;  // server-suggested reconnect delay
};

// Iterate SSE frames in a buffer, calling `cb(SseEvent)` for each
// complete event. `buf` is read up through the last complete event
// terminator (`\n\n` or `\r\n\r\n`); the unconsumed tail stays in
// `buf` for the next round. We capture `data:`, `id:`, and `retry:`
// per the SSE spec; `event:` is ignored (MCP doesn't use named
// events).
template <typename Callback>
void drain_sse(std::string& buf, Callback&& cb) {
    while (true) {
        std::size_t end = std::string::npos;
        std::size_t end_marker_len = 0;
        for (std::size_t i = 0; i + 1 < buf.size(); ++i) {
            if (buf[i] == '\n' && buf[i + 1] == '\n') {
                end = i; end_marker_len = 2; break;
            }
            if (i + 3 < buf.size() && buf[i] == '\r' && buf[i + 1] == '\n'
                && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                end = i; end_marker_len = 4; break;
            }
        }
        if (end == std::string::npos) return;

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
                line = std::string_view{event}.substr(line_start, line_end - line_start);
                line_start = line_end + 1;
            }
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == ':') continue;  // comment/keepalive
            const auto colon = line.find(':');
            std::string_view field, value;
            if (colon == std::string_view::npos) {
                field = line; value = {};
            } else {
                field = line.substr(0, colon);
                value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            }
            if (field == "data") {
                if (!se.data.empty()) se.data += '\n';
                se.data.append(value);
            } else if (field == "id") {
                se.id = std::string{value};
            } else if (field == "retry") {
                // Per SSE spec: integer milliseconds. Ignore on
                // parse error / negative.
                try {
                    int v = std::stoi(std::string{value});
                    if (v >= 0) se.retry_ms = v;
                } catch (...) {}
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
void set_common_headers(QNetworkRequest&                             req,
                        const HttpClientTransport::Options&          opts,
                        const std::optional<std::string>&            sid,
                        const std::string&                           access_token,
                        const std::string&                           last_event_id) {
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

HttpClientTransport::HttpClientTransport(Options opts) : opts_(std::move(opts)) {
    QUrl url{QString::fromStdString(opts_.url)};
    if (!url.isValid() || url.scheme().isEmpty()) {
        throw std::invalid_argument(
            "HttpClientTransport: URL must contain a scheme "
            "(http:// or https://)");
    }
    impl_ = std::make_unique<ClientImpl>(std::move(url));
    if (opts_.session_id.has_value()) {
        session_id_ = *opts_.session_id;
    }
    // Seed the mutable access_token_ from Options. The Options copy
    // is no longer the source of truth; set_access_token rotates this
    // member so on_unauthorized callbacks can drive a token refresh.
    if (opts_.access_token.has_value()) {
        access_token_ = *opts_.access_token;
    }
}

HttpClientTransport::~HttpClientTransport() { close(); }

void HttpClientTransport::on_message(MessageCallback cb) { on_message_ = std::move(cb); }
void HttpClientTransport::on_error(ErrorCallback cb)     { on_error_   = std::move(cb); }
void HttpClientTransport::on_close(CloseCallback cb)     { on_close_   = std::move(cb); }

std::optional<std::string> HttpClientTransport::session_id() const {
    std::lock_guard<std::mutex> lk(session_id_mu_);
    return session_id_;
}

// =====================================================================
// Lifecycle
// =====================================================================

void HttpClientTransport::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        return;
    }
    closed_.store(false, std::memory_order_release);
    close_fired_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { worker_loop(); });
    if (opts_.open_get_stream) {
        get_thread_ = std::thread([this] { run_get_stream(); });
    }
}

void HttpClientTransport::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        if (worker_.joinable()) worker_.join();
        if (get_thread_.joinable()) get_thread_.join();
        return;
    }

    // Spec: a client that no longer needs a session SHOULD send an
    // HTTP DELETE on the MCP path so the server can free per-session
    // state immediately. Best-effort: failure here is fine — close()
    // always proceeds.
    std::optional<std::string> sid;
    {
        std::lock_guard<std::mutex> lk(session_id_mu_);
        sid = session_id_;
    }
    if (sid.has_value() && !sid->empty() && impl_) {
        try {
            std::string token;
            {
                std::lock_guard<std::mutex> lk(auth_mu_);
                token = access_token_;
            }
            QNetworkAccessManager nam;
            QNetworkRequest req(impl_->url);
            set_common_headers(req, opts_, sid, token, "");
            req.setTransferTimeout(2000);
            QNetworkReply* reply = nam.deleteResource(req);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished,
                             &loop, &QEventLoop::quit);
            loop.exec();
        } catch (...) {
            // best-effort; ignore
        }
    }

    {
        std::lock_guard<std::mutex> lk(out_mu_);
        out_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(session_id_mu_);
        session_id_cv_.notify_all();
    }

    if (worker_.joinable()) worker_.join();
    if (get_thread_.joinable()) get_thread_.join();

    {
        std::unique_lock<std::mutex> lk(inflight_mu_);
        inflight_cv_.wait(lk, [this] { return inflight_ == 0; });
    }
    fire_close();
}

void HttpClientTransport::fire_close() noexcept {
    if (close_fired_.exchange(true, std::memory_order_acq_rel)) return;
    if (!on_close_) return;
    try { on_close_(); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_close threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_close threw a non-std exception");
    }
}

void HttpClientTransport::deliver_error(std::error_code ec) noexcept {
    if (!on_error_) return;
    try { on_error_(ec); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_error threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_error threw a non-std exception");
    }
}

void HttpClientTransport::deliver_frame(std::string raw) {
    if (!on_message_) return;
    try { on_message_(std::move(raw)); }
    catch (const std::exception& e) {
        MCP_LOG_ERROR(std::string{"HttpClientTransport on_message threw: "} + e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_message threw a non-std exception");
    }
}

// =====================================================================
// Send path
// =====================================================================

std::error_code HttpClientTransport::send(std::string_view frame) {
    if (!started_.load(std::memory_order_acquire) ||
         closed_.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    {
        std::lock_guard<std::mutex> lk(out_mu_);
        out_.emplace_back(frame);
        out_cv_.notify_one();
    }
    return {};
}

void HttpClientTransport::worker_loop() {
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
            std::unique_lock<std::mutex> lk(out_mu_);
            out_cv_.wait(lk, [this] {
                return closed_.load(std::memory_order_acquire) || !out_.empty();
            });
            if (closed_.load(std::memory_order_acquire) && out_.empty()) return;
            frame = std::move(out_.front());
            out_.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(inflight_mu_);
            ++inflight_;
        }
        std::thread([this, f = std::move(frame)]() mutable {
            process_send(std::move(f));
            std::lock_guard<std::mutex> lk(inflight_mu_);
            --inflight_;
            inflight_cv_.notify_all();
        }).detach();
    }
}

void HttpClientTransport::process_send(std::string frame) {
    std::optional<std::string> sid;
    {
        std::lock_guard<std::mutex> lk(session_id_mu_);
        sid = session_id_;
    }
    std::string token;
    {
        std::lock_guard<std::mutex> lk(auth_mu_);
        token = access_token_;
    }

    // Each send runs on its own thread (worker_loop dispatches), so
    // we create a per-call QNetworkAccessManager + event loop. This
    // keeps the "many concurrent POSTs, no serialisation" property
    // the transport relies on.
    QNetworkAccessManager nam;
    QNetworkRequest req(impl_->url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json, text/event-stream");
    set_common_headers(req, opts_, sid, token, "");
    if (opts_.request_timeout.count() > 0) {
        req.setTransferTimeout(static_cast<int>(opts_.request_timeout.count()));
    }

    QNetworkReply* reply =
        nam.post(req, QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
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
            std::lock_guard<std::mutex> lk(session_id_mu_);
            first_set = !session_id_.has_value();
            session_id_ = sid_hdr.toStdString();
        }
        if (first_set) session_id_cv_.notify_all();
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
        auth_callback_armed_.store(true, std::memory_order_release);
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
        if (opts_.on_unauthorized &&
            auth_callback_armed_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel)) {
            try { opts_.on_unauthorized(status, challenge); }
            catch (...) {}
        }
        deliver_error(std::make_error_code(std::errc::permission_denied));
        return;
    }
    if (status < 200 || status >= 300) {
        deliver_error(std::make_error_code(std::errc::protocol_error));
        return;
    }
    // Any 2xx re-arms the dedup flag.
    auth_callback_armed_.store(true, std::memory_order_release);

    const std::string ct = reply->rawHeader("Content-Type").toStdString();
    std::string body = reply->readAll().toStdString();

    if (ct.find("text/event-stream") != std::string::npos) {
        drain_sse(body, [this](SseEvent se) {
            // Track the last-seen event id so a future reconnect
            // (typically on the GET stream) can resume via
            // Last-Event-ID.
            if (se.id.has_value() && !se.id->empty()) {
                std::lock_guard<std::mutex> lk(get_mu_state_);
                last_event_id_ = *se.id;
            }
            if (!se.data.empty()) deliver_frame(std::move(se.data));
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

void HttpClientTransport::run_get_stream() noexcept {
    // The server's GET handler needs Mcp-Session-Id; it has no way
    // to associate an anonymous GET with the right session. So this
    // worker waits on a cv for the session id to be negotiated by
    // a POST response, and issues the GET as soon as it arrives —
    // no polling latency between "initialize() returned" and "GET
    // stream is open" (which a tool handler triggering server.sample()
    // would otherwise race).
    while (started_.load(std::memory_order_acquire) &&
          !closed_.load(std::memory_order_acquire)) {
        std::optional<std::string> sid;
        {
            std::unique_lock<std::mutex> lk(session_id_mu_);
            session_id_cv_.wait(lk, [this] {
                return session_id_.has_value() ||
                       closed_.load(std::memory_order_acquire);
            });
            if (closed_.load(std::memory_order_acquire)) return;
            sid = session_id_;
        }
        if (!sid.has_value()) continue;

        std::string token;
        {
            std::lock_guard<std::mutex> lk(auth_mu_);
            token = access_token_;
        }
        // Resumability: hand back the last event id we saw on this
        // session so the server can replay anything we missed during
        // the disconnect.
        std::string last_event_id;
        {
            std::lock_guard<std::mutex> lk(get_mu_state_);
            last_event_id = last_event_id_;
        }

        QNetworkAccessManager nam;
        QNetworkRequest req(impl_->url);
        req.setRawHeader("Accept", "text/event-stream");
        set_common_headers(req, opts_, sid, token, last_event_id);
        // The GET stream stays open indefinitely; no transfer timeout.
        // Heartbeats keep it alive.

        std::string buf;
        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            buf.append(reply->readAll());
            drain_sse(buf, [this](SseEvent se) {
                if (se.id.has_value() && !se.id->empty()) {
                    std::lock_guard<std::mutex> lk(get_mu_state_);
                    last_event_id_ = *se.id;
                }
                if (se.retry_ms.has_value()) {
                    std::lock_guard<std::mutex> lk(get_mu_state_);
                    next_retry_ms_ = se.retry_ms;
                }
                if (!se.data.empty()) deliver_frame(std::move(se.data));
            });
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        // Drain anything that arrived after the last readyRead.
        buf.append(reply->readAll());
        drain_sse(buf, [this](SseEvent se) {
            if (se.id.has_value() && !se.id->empty()) {
                std::lock_guard<std::mutex> lk(get_mu_state_);
                last_event_id_ = *se.id;
            }
            if (se.retry_ms.has_value()) {
                std::lock_guard<std::mutex> lk(get_mu_state_);
                next_retry_ms_ = se.retry_ms;
            }
            if (!se.data.empty()) deliver_frame(std::move(se.data));
        });

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // 405 Method Not Allowed is the spec's way of saying "no
        // server-initiated stream"; retry isn't useful, exit cleanly.
        if (status == 405) return;

        // 401/403: the access token has expired or been revoked.
        // Without this branch, run_get_stream would busy-retry every
        // 200 ms forever, blasting the IdP with no way for the
        // application to refresh the token. Surface the challenge
        // and exit; the application is expected to call
        // set_access_token() and start a new transport (or restart
        // this one) to recover.
        if (status == 401 || status == 403) {
            if (opts_.on_unauthorized) {
                const std::string challenge =
                    reply->rawHeader("WWW-Authenticate").toStdString();
                try { opts_.on_unauthorized(status, challenge); }
                catch (...) {}
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
                std::lock_guard<std::mutex> lk(session_id_mu_);
                session_id_.reset();
            }
            return;
        }

        if (closed_.load(std::memory_order_acquire)) return;

        // Backoff before reconnecting after EOF/error. Honour the
        // server's `retry:` if it sent one; clamp to a sane band so
        // a misbehaving server can't drive us into a hot loop or
        // perma-sleep. Default 200ms when the server hasn't said
        // otherwise.
        std::chrono::milliseconds delay{200};
        {
            std::lock_guard<std::mutex> lk(get_mu_state_);
            if (next_retry_ms_.has_value()) {
                int v = *next_retry_ms_;
                if (v < 50) v = 50;
                if (v > 60'000) v = 60'000;
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
    std::lock_guard<std::mutex> lk(auth_mu_);
    access_token_ = std::move(token);
}

std::string HttpClientTransport::access_token() const {
    std::lock_guard<std::mutex> lk(auth_mu_);
    return access_token_;
}

}  // namespace mcp
