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

#include <QByteArrayView>
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
    QString data;
    QString id;
    std::optional<int> retry_ms;  // server-suggested reconnect delay
};

// Iterate SSE frames in a buffer, calling `cb(SseEvent)` for each
// complete event. `buf` is read up through the last complete event
// terminator (`\n\n` or `\r\n\r\n`); the unconsumed tail stays in
// `buf` for the next round. We capture `data:`, `id:`, and `retry:`
// per the SSE spec; `event:` is ignored (MCP doesn't use named
// events).
template <typename Callback> void drain_sse(QString& buf, Callback&& cb) {
    while (true) {
        qsizetype end = -1;
        qsizetype marker = 0;
        const qsizetype n = buf.size();
        for (qsizetype i = 0; i + 1 < n; ++i) {
            if (buf.at(i) == QLatin1Char('\n') &&
                buf.at(i + 1) == QLatin1Char('\n')) {
                end = i;
                marker = 2;
                break;
            }
            if (i + 3 < n && buf.at(i) == QLatin1Char('\r') &&
                buf.at(i + 1) == QLatin1Char('\n') &&
                buf.at(i + 2) == QLatin1Char('\r') &&
                buf.at(i + 3) == QLatin1Char('\n')) {
                end = i;
                marker = 4;
                break;
            }
        }
        if (end < 0)
            return;

        const QString event = buf.left(end);
        buf.remove(0, end + marker);

        SseEvent se;
        const QList<QStringView> lines = QStringView{event}.split(u'\n');
        for (QStringView line : lines) {
            if (line.endsWith(u'\r'))
                line.chop(1);
            if (line.isEmpty() || line.at(0) == u':')
                continue;  // comment/keepalive
            const qsizetype colon = line.indexOf(u':');
            const QStringView field = colon < 0 ? line : line.left(colon);
            QStringView value = colon < 0 ? QStringView{} : line.mid(colon + 1);
            if (value.startsWith(u' '))
                value = value.mid(1);
            if (field == QStringLiteral("data")) {
                if (!se.data.isEmpty())
                    se.data += QLatin1Char('\n');
                se.data += value.toString();
            } else if (field == QStringLiteral("id")) {
                se.id = value.toString();
            } else if (field == QStringLiteral("retry")) {
                bool ok = false;
                const int v = value.toInt(&ok);
                if (ok && v >= 0)
                    se.retry_ms = v;
            }
        }
        if (!se.data.isEmpty() || !se.id.isNull() || se.retry_ms.has_value()) {
            cb(std::move(se));
        }
    }
}

// Attach the headers common to every outbound request: protocol
// version, negotiated session id, bearer token, resumability cursor,
// and caller-supplied extras.
void set_common_headers(QNetworkRequest& req,
                        const HttpClientTransport::Options& opts,
                        const QString& sid, const QString& access_token,
                        const QString& last_event_id) {
    req.setRawHeader("MCP-Protocol-Version", opts.protocol_version.toUtf8());
    if (!sid.isNull()) {
        req.setRawHeader("Mcp-Session-Id", sid.toUtf8());
    }
    if (!access_token.isEmpty()) {
        req.setRawHeader("Authorization",
                         QByteArray("Bearer ") + access_token.toUtf8());
    }
    if (!last_event_id.isEmpty()) {
        req.setRawHeader("Last-Event-ID", last_event_id.toUtf8());
    }
    for (auto it = opts.extra_headers.constBegin();
         it != opts.extra_headers.constEnd(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
}

}  // namespace

// =====================================================================
// Construction / destruction
// =====================================================================

HttpClientTransport::HttpClientTransport(Options opts)
    : m_opts(std::move(opts)) {
    QUrl url{m_opts.url};
    if (!url.isValid() || url.scheme().isEmpty()) {
        throw std::invalid_argument(
            "HttpClientTransport: URL must contain a scheme "
            "(http:// or https://)");
    }
    m_impl = std::make_unique<ClientImpl>(std::move(url));
    if (!m_opts.session_id.isNull()) {
        m_session_id = m_opts.session_id;
    }
    // Seed the mutable m_access_token from Options. The Options copy
    // is no longer the source of truth; set_access_token rotates this
    // member so on_unauthorized callbacks can drive a token refresh.
    if (!m_opts.access_token.isNull()) {
        m_access_token = m_opts.access_token;
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

QString HttpClientTransport::session_id(void) const {
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
    QString sid;
    {
        std::lock_guard<std::mutex> lk(m_session_id_mu);
        sid = m_session_id;
    }
    if (!sid.isNull() && !sid.isEmpty() && m_impl) {
        try {
            QString token;
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
        MCP_LOG_ERROR(QString{"HttpClientTransport on_close threw: "} +
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
        MCP_LOG_ERROR(QString{"HttpClientTransport on_error threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpClientTransport on_error threw a non-std exception");
    }
}

void HttpClientTransport::deliver_frame(QByteArray raw) {
    if (!m_on_message)
        return;
    try {
        m_on_message(std::move(raw));
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(QString{"HttpClientTransport on_message threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR(
            "HttpClientTransport on_message threw a non-std exception");
    }
}

// =====================================================================
// Send path
// =====================================================================

std::error_code HttpClientTransport::send(QByteArrayView frame) {
    if (!m_started.load(std::memory_order_acquire) ||
        m_closed.load(std::memory_order_acquire)) {
        return std::make_error_code(std::errc::not_connected);
    }
    {
        std::lock_guard<std::mutex> lk(m_out_mu);
        m_out.emplace_back(frame.toByteArray());
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
        QByteArray frame;
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

void HttpClientTransport::process_send(QByteArray frame) {
    QString sid;
    {
        std::lock_guard<std::mutex> lk(m_session_id_mu);
        sid = m_session_id;
    }
    QString token;
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

    QNetworkReply* reply = nam.post(req, frame);
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
            first_set = m_session_id.isNull();
            m_session_id = sid_hdr;
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
        const QString challenge = reply->rawHeader("WWW-Authenticate");
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

    const QByteArray ct = reply->rawHeader("Content-Type");
    const QByteArray body = reply->readAll();

    if (ct.contains("text/event-stream")) {
        QString text = QString::fromUtf8(body);
        drain_sse(text, [this](SseEvent se) {
            // Track the last-seen event id so a future reconnect
            // (typically on the GET stream) can resume via
            // Last-Event-ID.
            if (!se.id.isNull() && !se.id.isEmpty()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_last_event_id = se.id;
            }
            if (!se.data.isEmpty())
                deliver_frame(se.data.toUtf8());
        });
        return;
    }
    // `application/json` and unknown content-types both pass the body
    // through unchanged.
    deliver_frame(body);
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
        QString sid;
        {
            std::unique_lock<std::mutex> lk(m_session_id_mu);
            m_session_id_cv.wait(lk, [this] {
                return !m_session_id.isNull() ||
                       m_closed.load(std::memory_order_acquire);
            });
            if (m_closed.load(std::memory_order_acquire))
                return;
            sid = m_session_id;
        }
        if (sid.isNull())
            continue;

        QString token;
        {
            std::lock_guard<std::mutex> lk(m_auth_mu);
            token = m_access_token;
        }
        // Resumability: hand back the last event id we saw on this
        // session so the server can replay anything we missed during
        // the disconnect.
        QString last_event_id;
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

        QString buf;
        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            buf += QString::fromUtf8(reply->readAll());
            drain_sse(buf, [this](SseEvent se) {
                if (!se.id.isNull() && !se.id.isEmpty()) {
                    std::lock_guard<std::mutex> lk(m_get_mu_state);
                    m_last_event_id = se.id;
                }
                if (se.retry_ms.has_value()) {
                    std::lock_guard<std::mutex> lk(m_get_mu_state);
                    m_next_retry_ms = se.retry_ms;
                }
                if (!se.data.isEmpty())
                    deliver_frame(se.data.toUtf8());
            });
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        loop.exec();

        // Drain anything that arrived after the last readyRead.
        buf += QString::fromUtf8(reply->readAll());
        drain_sse(buf, [this](SseEvent se) {
            if (!se.id.isNull() && !se.id.isEmpty()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_last_event_id = se.id;
            }
            if (se.retry_ms.has_value()) {
                std::lock_guard<std::mutex> lk(m_get_mu_state);
                m_next_retry_ms = se.retry_ms;
            }
            if (!se.data.isEmpty())
                deliver_frame(se.data.toUtf8());
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
                const QString challenge = reply->rawHeader("WWW-Authenticate");
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
                m_session_id = QString();
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

void HttpClientTransport::set_access_token(QString token) {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    m_access_token = std::move(token);
}

QString HttpClientTransport::access_token(void) const {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    return m_access_token;
}

}  // namespace mcp
