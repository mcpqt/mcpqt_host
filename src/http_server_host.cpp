// SPDX-License-Identifier: AGPL-3.0-only
#include "http_server_host.h"

#include "error.h"
#include "json.h"
#include "log.h"
#include "protocol.h"
#include "server.h"
#include "session.h"
#include "transport.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QFuture>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QHttpServerRouter>
#include <QHttpServerRouterRule>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QPromise>
#include <QRegularExpressionMatch>
#include <QString>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcp {

namespace {

// =====================================================================
// Per-session HTTP transport adapter
// =====================================================================
//
// One of these lives inside each per-session SessionContext. When a
// POST arrives carrying a JSON-RPC *request*, the host enqueues a
// pending response slot keyed by the request id, hands the body to
// `deliver`, and awaits the slot's future. The Session dispatches the
// request to a worker, the user's handler returns, the Session calls
// `send(response_frame)`, and that send routes to the matching slot
// to satisfy the future. The QTcpSocket timer then writes the JSON to
// the HTTP response.
//
// Notifications and responses inbound from the peer get fed via
// `deliver` and produce no future; the POST handler returns 202.
//
// Server-initiated traffic (transport->send called with no matching
// pending slot) is buffered and drained by the GET-opened SSE stream.

class HttpSessionTransport final : public Transport {
public:
    void on_message(MessageCallback cb) override {
        // Just stash the callback. Crucially we do NOT yet allow
        // dispatch: Session::Session() wires on_message BEFORE
        // Server::run registers any of its request handlers. A frame
        // delivered in that window would route into an empty handler
        // map and emit a spurious "method not found: initialize"
        // response. The `m_started` gate, flipped by start(), is what
        // marks the dispatcher as fully populated.
        std::lock_guard<std::mutex> lk(m_buffered_mu);
        m_on_message = std::move(cb);
    }
    void on_error(ErrorCallback cb) override { m_on_error = std::move(cb); }
    void on_close(CloseCallback cb) override { m_on_close = std::move(cb); }

    void start(void) override {
        // Flip the gate and drain anything that arrived before all
        // request handlers were registered. By the time Session::start
        // runs us, set_request_handler in Server::run has completed.
        std::deque<QByteArray> drain;
        {
            std::lock_guard<std::mutex> lk(m_buffered_mu);
            m_started = true;
            m_buffered.swap(drain);
        }
        if (m_on_message) {
            for (auto& f : drain)
                m_on_message(std::move(f));
        }
    }

    void close(void) override {
        if (m_closed.exchange(true, std::memory_order_acq_rel))
            return;
        // Cancel any waiters on pending POST replies.
        std::map<QString, std::promise<QByteArray>> drained;
        {
            std::lock_guard<std::mutex> lk(m_pending_mu);
            drained.swap(m_pending);
        }
        for (auto& [id, p] : drained) {
            try {
                throw Error{error_code::internal_error, "session closed"};
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        }
        // Wake any GET stream waiter so the http handler can return.
        {
            std::lock_guard<std::mutex> lk(m_get_mu);
            m_get_cv.notify_all();
        }
        if (m_on_close) {
            try {
                m_on_close();
            } catch (const std::exception& e) {
                MCP_LOG_ERROR(QString{"HttpSessionTransport on_close threw: "} +
                              e.what());
            } catch (...) {
                MCP_LOG_ERROR(
                    "HttpSessionTransport on_close threw a non-std exception");
            }
        }
    }

    [[nodiscard]] bool is_closed(void) const noexcept {
        return m_closed.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::error_code send(QByteArrayView frame) override {
        if (m_closed.load(std::memory_order_acquire)) {
            return std::make_error_code(std::errc::not_connected);
        }
        // Parse the frame's id (responses have one) so we can route
        // back to the awaiting POST slot.
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray(frame.data(), static_cast<int>(frame.size())), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            MCP_LOG_ERROR("HttpSessionTransport: bad outbound JSON");
            return std::make_error_code(std::errc::invalid_argument);
        }
        const QJsonObject j = doc.object();
        const bool has_id = j.contains("id") && !j.value("id").isNull();
        const bool has_method = j.contains("method");

        if (has_id && !has_method) {
            // Response — route to the awaiting POST.
            QString key = canonical_id(j.value("id"));
            std::promise<QByteArray> awaiter;
            {
                std::lock_guard<std::mutex> lk(m_pending_mu);
                auto it = m_pending.find(key);
                if (it == m_pending.end()) {
                    MCP_LOG_WARN(
                        "HttpSessionTransport: no awaiter for response id=" +
                        key);
                    return std::make_error_code(std::errc::invalid_argument);
                }
                awaiter = std::move(it->second);
                m_pending.erase(it);
            }
            awaiter.set_value(frame.toByteArray());
            return {};
        }

        // Notification or server-initiated request. Always buffer
        // here: if a GET stream is already open, the GET handler
        // picks it up immediately; if not, frames wait until the
        // client opens its GET stream so a tool handler that fires
        // server.sample() right after initialize doesn't lose its
        // request to a startup race.
        std::lock_guard<std::mutex> lk(m_get_mu);
        m_get_queue.emplace_back(frame.toByteArray());
        m_get_cv.notify_all();
        return {};
    }

    // Host-facing: try to grab a frame for the GET stream. Returns a
    // frame if one was available, std::nullopt otherwise. Caller must
    // call `enter_get_stream()` once before the first drain so outbound
    // traffic stops being dropped.
    void enter_get_stream(void) {
        std::lock_guard<std::mutex> lk(m_get_mu);
        m_get_active = true;
    }
    void leave_get_stream(void) {
        std::lock_guard<std::mutex> lk(m_get_mu);
        m_get_active = false;
        // Don't drain the queue: another GET might be opened (with
        // resumability). This build ships without resumability so in
        // practice we simply lose the queue's contents on disconnect.
    }
    std::optional<QByteArray> try_get_frame(void) {
        std::lock_guard<std::mutex> lk(m_get_mu);
        if (m_get_queue.empty())
            return std::nullopt;
        QByteArray out = std::move(m_get_queue.front());
        m_get_queue.pop_front();
        return out;
    }

    [[nodiscard]] bool is_open(void) const noexcept override {
        // Open from construction until close(); see start() comment.
        return !m_closed.load(std::memory_order_acquire);
    }

    // Host-facing helpers ----------------------------------------------------

    /// Hand a frame received from the peer up to the Session. If the
    /// frame is a *request*, returns a future that resolves to the
    /// outbound response string. If it's a notification or response,
    /// returns std::nullopt (POST should reply 202).
    std::optional<std::future<QByteArray>>
    deliver_request_or_notification(QByteArray raw) {
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        const bool parsed =
            (perr.error == QJsonParseError::NoError) && doc.isObject();
        const QJsonObject j = parsed ? doc.object() : QJsonObject{};
        const bool is_request = parsed && j.contains("id") &&
                                !j.value("id").isNull() && j.contains("method");

        std::optional<std::future<QByteArray>> reply;
        if (is_request) {
            QString key = canonical_id(j.value("id"));
            std::promise<QByteArray> p;
            reply = p.get_future();
            std::lock_guard<std::mutex> lk(m_pending_mu);
            m_pending.emplace(std::move(key), std::move(p));
        }
        // Deliver under m_buffered_mu so the m_started/m_buffered/m_on_message
        // triple stays consistent against start() and on_message().
        // m_started alone is the gate, not m_on_message: the latter is
        // wired by Session::Session before set_request_handler calls
        // run, and dispatching during that window produces spurious
        // "method not found" errors.
        std::lock_guard<std::mutex> lk(m_buffered_mu);
        if (m_started && m_on_message) {
            m_on_message(std::move(raw));
        } else {
            m_buffered.emplace_back(std::move(raw));
        }
        return reply;
    }

private:
    static QString canonical_id(const QJsonValue& id) {
        if (id.isString())
            return "s:" + id.toString();
        if (id.isDouble())
            return "i:" + QString::number(id.toInteger());
        return QString{};
    }

    std::atomic<bool> m_closed{false};

    // Pre-start buffer, the callback, and the m_started gate share a
    // mutex so their three-way consistency is maintained across the
    // setter (on_message), the gate flip (start), and the producer
    // (deliver_request_or_notification).
    std::mutex m_buffered_mu;
    bool m_started = false;
    MessageCallback m_on_message;
    std::deque<QByteArray> m_buffered;

    ErrorCallback m_on_error;
    CloseCallback m_on_close;

    std::mutex m_pending_mu;
    std::map<QString, std::promise<QByteArray>> m_pending;

    // GET-stream queue (server-initiated outbound traffic). Frames
    // produced by `send()` while a GET stream is active are pushed
    // here and consumed by the GET handler.
    std::mutex m_get_mu;
    std::condition_variable m_get_cv;
    std::deque<QByteArray> m_get_queue;
    bool m_get_active = false;
};

// =====================================================================
// Adapter so a shared_ptr<HttpSessionTransport> can be handed to
// Server::run, which insists on std::unique_ptr<Transport>.
// =====================================================================

class SharedTransportShim final : public Transport {
public:
    explicit SharedTransportShim(std::shared_ptr<HttpSessionTransport> inner)
        : m_inner(std::move(inner)) {}

    void on_message(MessageCallback cb) override {
        m_inner->on_message(std::move(cb));
    }
    void on_error(ErrorCallback cb) override {
        m_inner->on_error(std::move(cb));
    }
    void on_close(CloseCallback cb) override {
        m_inner->on_close(std::move(cb));
    }
    void start(void) override { m_inner->start(); }
    void close(void) override { m_inner->close(); }
    [[nodiscard]] std::error_code send(QByteArrayView f) override {
        return m_inner->send(f);
    }
    [[nodiscard]] bool is_open(void) const noexcept override {
        return m_inner->is_open();
    }

private:
    std::shared_ptr<HttpSessionTransport> m_inner;
};

// =====================================================================
// Session-id minting
// =====================================================================

// Fire the user's on_session_closed hook for a session about to be torn
// down, with the Server still fully alive. Exception-contained: the hook
// is embedder code and must not abort a teardown or unwind through the
// HTTP worker / stop() caller.
void invoke_session_closed(const HttpServerHost::Options& opts,
                           Server& server) {
    if (!opts.on_session_closed)
        return;
    try {
        opts.on_session_closed(server);
    } catch (const std::exception& e) {
        MCP_LOG_ERROR(QString{"HttpServerHost: on_session_closed threw: "} +
                      e.what());
    } catch (...) {
        MCP_LOG_ERROR("HttpServerHost: on_session_closed threw a non-std "
                      "exception");
    }
}

QString make_session_id(void) {
    // 128-bit hex string from a per-call seeded RNG. Cryptographic
    // strength isn't promised here (std::random_device quality varies
    // per platform) — this is "unguessable enough" for non-adversarial
    // single-tenant hosts.
    std::random_device rd;
    std::mt19937_64 rng{(static_cast<std::uint64_t>(rd()) << 32) ^ rd()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream os;
    os << std::hex << dist(rng) << dist(rng);
    return QString::fromStdString(os.str());
}

// =====================================================================
// HTTP request helpers (Qt-typed)
// =====================================================================

// Fetch the first value for a header, case-insensitively. std::nullopt
// if the header is absent.
inline std::optional<QByteArray> header_value(const QHttpServerRequest& req,
                                              const char* name) {
    const QByteArray wanted{name};
    const auto headers = req.headers();
    for (const auto& kv : headers) {
        if (kv.first.compare(wanted, Qt::CaseInsensitive) == 0) {
            return kv.second;
        }
    }
    return std::nullopt;
}

// Count occurrences of a header (used to reject duplicate Authorization).
inline int header_count(const QHttpServerRequest& req, const char* name) {
    const QByteArray wanted{name};
    int n = 0;
    const auto headers = req.headers();
    for (const auto& kv : headers) {
        if (kv.first.compare(wanted, Qt::CaseInsensitive) == 0)
            ++n;
    }
    return n;
}

inline QHttpServerResponder::StatusCode status_from_int(int s) {
    using S = QHttpServerResponder::StatusCode;
    switch (s) {
    case 200:
        return S::Ok;
    case 201:
        return S::Created;
    case 202:
        return S::Accepted;
    case 204:
        return S::NoContent;
    case 400:
        return S::BadRequest;
    case 401:
        return S::Unauthorized;
    case 403:
        return S::Forbidden;
    case 404:
        return S::NotFound;
    case 405:
        return S::MethodNotAllowed;
    case 415:
        return S::UnsupportedMediaType;
    case 500:
        return S::InternalServerError;
    case 504:
        return S::GatewayTimeout;
    default:
        return S::InternalServerError;
    }
}

inline QHttpServerResponse
text_response(const QString& body, QHttpServerResponder::StatusCode code) {
    return QHttpServerResponse(QByteArrayLiteral("text/plain"), body.toUtf8(),
                               code);
}

inline bool origin_allowed(const QHttpServerRequest& req,
                           const QList<QString>& allow) {
    const auto origin = header_value(req, "Origin");
    if (!origin)
        return true;  // no Origin header -> not a browser XS request
    if (allow.isEmpty())
        return false;
    for (const auto& a : allow) {
        if (a == "*" || a == QString::fromUtf8(*origin))
            return true;
    }
    return false;
}

// Reject control chars + the two characters that need escaping in an
// RFC 7235 quoted-string. Using "reject" rather than "escape" because
// the only legitimate values for realm and metadata_url shouldn't
// contain any of these — and a malformed input is far more likely
// to be an attack than a legitimate exotic value.
inline bool valid_quoted_param(QStringView s) noexcept {
    for (const QChar ch : s) {
        const ushort c = ch.unicode();
        if (c == '"' || c == '\\' || c == '\r' || c == '\n' || c < 0x20) {
            return false;
        }
    }
    return true;
}

// Resolve the Access-Control-Allow-Origin value to echo back for a
// given request. If the host's allowed_origins list contains "*"
// the response advertises "*"; otherwise it echoes the request's
// Origin verbatim ONLY when the origin passes the same allowlist
// check that POST/GET/DELETE go through. Returns empty string if
// the origin would be rejected — caller skips the ACAO header.
inline QString resolve_cors_origin(const QHttpServerRequest& req,
                                   const QList<QString>& allow) {
    const auto it = header_value(req, "Origin");
    if (!it || it->isEmpty())
        return {};
    const QString origin = QString::fromUtf8(*it);
    for (const auto& a : allow) {
        if (a == "*")
            return "*";
        if (a == origin)
            return origin;
    }
    return {};
}

// Build the WWW-Authenticate Bearer challenge value for 401/403
// responses per RFC 6750 §3. error_code is "invalid_request" |
// "invalid_token" | "insufficient_scope" (or empty to omit). scope is
// the space-separated scope hint for insufficient_scope.
inline QString build_bearer_challenge(const QString& realm,
                                      const QString& metadata_url,
                                      const QString& error_code,
                                      const QString& scope) {
    // valid_quoted_param() at start() guarantees realm + metadata_url
    // never contain unsafe characters; we just splice them.
    QString out = "Bearer realm=\"";
    out += realm;
    out += "\"";
    if (!error_code.isEmpty()) {
        out += ", error=\"";
        out += error_code;
        out += "\"";
    }
    if (!scope.isEmpty()) {
        out += ", scope=\"";
        out += scope;
        out += "\"";
    }
    if (!metadata_url.isEmpty()) {
        out += ", resource_metadata=\"";
        out += metadata_url;
        out += "\"";
    }
    return out;
}

// Case-insensitive prefix match. HTTP scheme tokens are case-insensitive
// (RFC 7235 §2.1).
inline bool starts_with_bearer_ci(const QString& s) noexcept {
    return s.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive);
}

// Trim leading/trailing OWS from a token68 candidate.
inline QString trim_ows(QString s) noexcept {
    return s.trimmed();
}

// Per the 2025-11-25 spec: a request whose `MCP-Protocol-Version`
// header is present but unsupported MUST be rejected with 400.
inline bool valid_protocol_version_header(const QByteArray& v) noexcept {
    return v == "2025-03-26" || v == "2025-06-18" || v == "2025-11-25";
}

// The outcome of bearer-token validation, expressed toolkit-neutrally
// so it can be rendered as either a QHttpServerResponse (POST/DELETE)
// or a raw HTTP error (SSE GET).
struct AuthOutcome {
    bool allowed = true;
    int status = 200;
    QString challenge;  // WWW-Authenticate value, when non-empty
    QString body;       // plain-text body for the error
};

// Validate the Authorization header against the host's bearer
// validator (if configured). Returns allowed=true when the request may
// proceed; otherwise fills the outcome with a 400/401/403 response.
inline AuthOutcome authorize(const QHttpServerRequest& req,
                             const HttpServerHost::Options& opts) {
    AuthOutcome out;
    if (!opts.bearer_validator)
        return out;

    const int count_auth = header_count(req, "Authorization");
    if (count_auth == 0) {
        out.allowed = false;
        out.status = 401;
        out.challenge = build_bearer_challenge(
            opts.auth_realm, opts.resource_metadata_url, "invalid_request", {});
        out.body = "missing Authorization header";
        return out;
    }
    // RFC 7230: a request with multiple Authorization headers is
    // syntactically invalid — refuse rather than pick one arbitrarily.
    if (count_auth > 1) {
        out.allowed = false;
        out.status = 400;
        out.body = "multiple Authorization headers";
        return out;
    }

    const QString auth = QString::fromUtf8(*header_value(req, "Authorization"));
    if (!starts_with_bearer_ci(auth)) {
        out.allowed = false;
        out.status = 401;
        out.challenge = build_bearer_challenge(
            opts.auth_realm, opts.resource_metadata_url, "invalid_request", {});
        out.body = "expected Bearer scheme";
        return out;
    }
    const QString token = trim_ows(auth.mid(7));  // strip "Bearer "
    if (token.isEmpty()) {
        out.allowed = false;
        out.status = 401;
        out.challenge = build_bearer_challenge(
            opts.auth_realm, opts.resource_metadata_url, "invalid_request", {});
        out.body = "empty bearer token";
        return out;
    }

    const auto outcome = opts.bearer_validator(token);
    switch (outcome.status) {
    case HttpServerHost::Options::BearerStatus::allow:
        return out;
    case HttpServerHost::Options::BearerStatus::invalid_token:
        out.allowed = false;
        out.status = 401;
        out.challenge = build_bearer_challenge(
            opts.auth_realm, opts.resource_metadata_url, "invalid_token", {});
        out.body = "invalid bearer token";
        return out;
    case HttpServerHost::Options::BearerStatus::insufficient_scope:
        // Scope strings have the same control-char concern as the
        // realm. valid_quoted_param() is checked at start() but
        // validator output is fresh per request — drop unsafe
        // strings here rather than splicing them blindly.
        out.allowed = false;
        out.status = 403;
        out.challenge = build_bearer_challenge(
            opts.auth_realm, opts.resource_metadata_url, "insufficient_scope",
            valid_quoted_param(outcome.required_scopes)
                ? outcome.required_scopes
                : QString());
        out.body = "insufficient scope";
        return out;
    }
    return out;
}

// =====================================================================
// Raw-socket SSE writing (chunked transfer encoding)
// =====================================================================
//
// Qt 6.4's QHttpServer has no public streaming/chunked API, so the GET
// SSE endpoint is served by a custom QHttpServerRouterRule that writes
// HTTP/1.1 chunked framing straight to the connection's QTcpSocket.

inline void write_sse_chunk(QTcpSocket* socket, const QByteArray& payload) {
    QByteArray data = "data: ";
    data += payload;
    data += "\n\n";
    QByteArray chunk = QByteArray::number(data.size(), 16).toLower();
    chunk += "\r\n";
    chunk += data;
    chunk += "\r\n";
    socket->write(chunk);
}

inline void write_sse_comment(QTcpSocket* socket) {
    const QByteArray data = ":\n\n";
    QByteArray chunk = QByteArray::number(data.size(), 16).toLower();
    chunk += "\r\n";
    chunk += data;
    chunk += "\r\n";
    socket->write(chunk);
}

inline QByteArray http_reason_phrase(int status) {
    switch (status) {
    case 200:
        return QByteArrayLiteral("OK");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 401:
        return QByteArrayLiteral("Unauthorized");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 404:
        return QByteArrayLiteral("Not Found");
    case 405:
        return QByteArrayLiteral("Method Not Allowed");
    case 500:
        return QByteArrayLiteral("Internal Server Error");
    default:
        return QByteArrayLiteral("Error");
    }
}

inline void write_raw_http_error(QTcpSocket* socket, int status,
                                 const QByteArray& body,
                                 const QByteArray& www_authenticate = {}) {
    QByteArray out = QByteArrayLiteral("HTTP/1.1 ");
    out += QByteArray::number(status);
    out += ' ';
    out += http_reason_phrase(status);
    out += QByteArrayLiteral("\r\n");
    if (!www_authenticate.isEmpty()) {
        out += QByteArrayLiteral("WWW-Authenticate: ");
        out += www_authenticate;
        out += QByteArrayLiteral("\r\n");
    }
    out += QByteArrayLiteral("Content-Type: text/plain\r\n");
    out += QByteArrayLiteral("Content-Length: ");
    out += QByteArray::number(static_cast<int>(body.size()));
    out += QByteArrayLiteral("\r\n");
    out += QByteArrayLiteral("Connection: close\r\n\r\n");
    socket->write(out);
    if (!body.isEmpty())
        socket->write(body);
    socket->disconnectFromHost();
}

// Full raw HTTP/1.1 response (status line + headers + framed body) written
// straight to the connection's socket, then closed. Used by the POST/CORS
// paths so a request can be answered asynchronously from a QTimer without
// going through QHttpServer's QFuture continuation machinery (which is
// crash-prone in Qt 6.4 when an unfinished QPromise is destroyed).
struct RawResponseSpec {
    int status = 200;
    QByteArray content_type = QByteArrayLiteral("application/json");
    QByteArray body;
    QList<std::pair<QByteArray, QByteArray>> headers;
};

inline void write_raw_http_response(QTcpSocket* socket,
                                    const RawResponseSpec& spec) {
    QByteArray out = QByteArrayLiteral("HTTP/1.1 ");
    out += QByteArray::number(spec.status);
    out += ' ';
    out += http_reason_phrase(spec.status);
    out += QByteArrayLiteral("\r\n");
    out += QByteArrayLiteral("Content-Type: ");
    out += spec.content_type;
    out += QByteArrayLiteral("\r\n");
    for (const auto& header : spec.headers) {
        out += header.first;
        out += QByteArrayLiteral(": ");
        out += header.second;
        out += QByteArrayLiteral("\r\n");
    }
    out += QByteArrayLiteral("Content-Length: ");
    out += QByteArray::number(spec.body.size());
    out += QByteArrayLiteral("\r\n");
    out += QByteArrayLiteral("Connection: close\r\n\r\n");
    out += spec.body;
    socket->write(out);
    socket->disconnectFromHost();
}

}  // namespace

// =====================================================================
// Per-session context owned by the host
// =====================================================================

struct HttpServerHost::SessionContext {
    QString id;
    std::shared_ptr<HttpSessionTransport> transport;
    std::unique_ptr<Server> server;
    std::thread run_thread;
    std::chrono::steady_clock::time_point last_seen;
};

// =====================================================================
// Host impl (the QHttpServer lives on a dedicated QThread)
// =====================================================================

struct HttpServerHost::Impl {
    QThread* thread = nullptr;
    QHttpServer* http = nullptr;

    std::atomic<int> port{0};

    std::mutex sessions_mu;
    QHash<QString, std::shared_ptr<HttpServerHost::SessionContext>> sessions;
};

// =====================================================================
// Construction / destruction
// =====================================================================

HttpServerHost::HttpServerHost(Implementation server_info, Options opts,
                               SessionFactory factory)
    : m_server_info(std::move(server_info)), m_opts(std::move(opts)),
      m_factory(std::move(factory)), m_impl(std::make_unique<Impl>()) {}

HttpServerHost::~HttpServerHost() {
    stop();
}

int HttpServerHost::port(void) const noexcept {
    return m_impl ? m_impl->port.load(std::memory_order_acquire) : 0;
}

std::size_t HttpServerHost::active_sessions(void) const {
    if (!m_impl)
        return 0;
    std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
    return static_cast<std::size_t>(m_impl->sessions.size());
}

// =====================================================================
// Lifecycle
// =====================================================================

void HttpServerHost::start(void) {
    if (m_started.exchange(true, std::memory_order_acq_rel))
        return;

    // Inputs that get spliced into HTTP response headers have to be
    // sanitised — a CR/LF or `"` would smuggle a header. We don't try
    // to escape; we reject. Legitimate realm names and URLs don't
    // contain these.
    if (!valid_quoted_param(m_opts.auth_realm)) {
        m_started.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: auth_realm contains a control or quoting "
            "character — refusing to splice into the WWW-Authenticate "
            "header");
    }
    if (!valid_quoted_param(m_opts.resource_metadata_url)) {
        m_started.store(false, std::memory_order_release);
        throw std::invalid_argument(
            "HttpServerHost: resource_metadata_url contains a control "
            "or quoting character — refusing to splice into the "
            "WWW-Authenticate header");
    }
    // path is what we register routes under; allowing weird values
    // would conflict with the well-known endpoints below.
    if (m_opts.path.isEmpty() || m_opts.path.front() != '/') {
        m_started.store(false, std::memory_order_release);
        throw std::invalid_argument(
            QStringLiteral("HttpServerHost: path must begin with '/'")
                .toStdString());
    }

    // Bearer auth over plain HTTP from a non-loopback address ships
    // tokens in the clear. The spec mandates HTTPS in production;
    // we let users opt into "I have a TLS terminator in front of
    // me" via allow_insecure_http rather than guess.
    const bool is_loopback =
        (m_opts.host == "127.0.0.1" || m_opts.host == "::1" ||
         m_opts.host == "localhost");
    if (m_opts.bearer_validator && !is_loopback &&
        !m_opts.allow_insecure_http) {
        m_started.store(false, std::memory_order_release);
        throw std::invalid_argument(
            (QStringLiteral("HttpServerHost: bearer authentication is "
                            "configured but the host is binding to a "
                            "non-loopback address (\"") +
             m_opts.host +
             QStringLiteral("\") and the server is on plain HTTP. Set "
                            "Options::allow_insecure_http=true to acknowledge "
                            "that TLS termination happens elsewhere (typically "
                            "a reverse proxy), or bind to 127.0.0.1/::1 for "
                            "development."))
                .toStdString());
    }

    m_impl->http = new QHttpServer();
    m_impl->thread = new QThread();
    m_impl->thread->setObjectName(QStringLiteral("mcp-http-server"));

    auto* http = m_impl->http;

    // ---- POST: a client frame -> JSON response or 202 Accepted ----
    //
    // Served as a raw router rule (like the GET SSE endpoint) rather
    // than a QFuture-returning QHttpServer route: Qt 6.4 crashes if an
    // unfinished QPromise<QHttpServerResponse> is destroyed while
    // QHttpServer still holds a continuation on its future. Writing the
    // response (JSON, or a 202/4xx) straight to the socket keeps the
    // async reply on a QTimer and out of that machinery entirely.
    http->router()->addRule<std::function<void()>>(std::make_unique<
                                                   QHttpServerRouterRule>(
        m_opts.path, QHttpServerRequest::Method::Post,
        [this](const QRegularExpressionMatch&, const QHttpServerRequest& req,
               QTcpSocket* socket) {
            auto send = [socket](RawResponseSpec spec) {
                write_raw_http_response(socket, spec);
            };

            if (!origin_allowed(req, m_opts.allowed_origins)) {
                send({403,
                      QByteArrayLiteral("text/plain"),
                      QByteArrayLiteral("Origin not allowed"),
                      {}});
                return;
            }
            // Spec MUST: a present-but-unsupported MCP-Protocol-Version
            // is a 400. Header is allowed to be absent.
            if (const auto v = header_value(req, "MCP-Protocol-Version");
                v && !valid_protocol_version_header(*v)) {
                send({400,
                      QByteArrayLiteral("text/plain"),
                      QByteArray("unsupported MCP-Protocol-Version: ") + *v,
                      {}});
                return;
            }
            {
                const auto auth = authorize(req, m_opts);
                if (!auth.allowed) {
                    QList<std::pair<QByteArray, QByteArray>> h;
                    if (!auth.challenge.isEmpty()) {
                        h.emplace_back(QByteArrayLiteral("WWW-Authenticate"),
                                       auth.challenge.toUtf8());
                    }
                    send({auth.status, QByteArrayLiteral("text/plain"),
                          auth.body.toUtf8(), std::move(h)});
                    return;
                }
            }

            // Resolve or mint session id.
            QString sid;
            if (const auto h = header_value(req, "Mcp-Session-Id"))
                sid = *h;
            const QString body = req.body();
            const bool is_initialize_body = body.contains("\"initialize\"");
            if (sid.isEmpty() && is_initialize_body)
                sid = make_session_id();

            std::shared_ptr<SessionContext> ctx;
            if (!sid.isEmpty()) {
                std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
                auto it = m_impl->sessions.find(sid);
                if (it != m_impl->sessions.end()) {
                    ctx = it.value();
                    ctx->last_seen = std::chrono::steady_clock::now();
                } else if (is_initialize_body) {
                    ctx = std::make_shared<SessionContext>();
                    ctx->id = sid;
                    ctx->last_seen = std::chrono::steady_clock::now();
                    ctx->transport = std::make_shared<HttpSessionTransport>();
                    ctx->server = std::make_unique<Server>(m_server_info);
                    m_factory(*ctx->server);

                    // Run the Server on its own thread, wrapping our
                    // shared_ptr transport in a unique_ptr-shaped shim so
                    // Server::run can take ownership without disturbing
                    // the host's own reference.
                    auto* server_ptr = ctx->server.get();
                    auto transport = ctx->transport;
                    ctx->run_thread = std::thread([server_ptr, transport]() {
                        server_ptr->run(
                            std::make_unique<SharedTransportShim>(transport));
                    });
                    m_impl->sessions.emplace(sid, ctx);
                }
            }

            if (!ctx) {
                if (!sid.isEmpty()) {
                    // Session id refers to a session we no longer know
                    // (expired, deleted, or bogus). Spec: the server
                    // MUST respond 404 so the client knows to start a
                    // fresh session with a new InitializeRequest.
                    send({404,
                          QByteArrayLiteral("text/plain"),
                          QByteArrayLiteral("unknown or expired session id"),
                          {}});
                    return;
                }
                // No session id and not an initialize — malformed usage.
                send({400,
                      QByteArrayLiteral("text/plain"),
                      QByteArrayLiteral(
                          "missing session id; send initialize first"),
                      {}});
                return;
            }

            // Hand the body to the per-session transport. If it was a
            // request, we'll get a future for the response; otherwise
            // we reply 202.
            auto reply =
                ctx->transport->deliver_request_or_notification(body.toUtf8());
            if (!reply.has_value()) {
                QList<std::pair<QByteArray, QByteArray>> h{
                    {QByteArrayLiteral("Mcp-Session-Id"), ctx->id.toUtf8()}};
                send({202,
                      QByteArrayLiteral("application/json"),
                      {},
                      std::move(h)});
                return;
            }

            // Async: park the reply future and poll it from a QTimer that
            // lives on this (server) thread, so the response is delivered
            // without ever blocking a worker. This also lets the handler
            // issue server-initiated requests (sampling/roots/...) whose
            // answers arrive as additional POSTs on other connections.
            auto reply_shared =
                std::make_shared<std::future<QByteArray>>(std::move(*reply));
            auto budget =
                std::make_shared<std::chrono::steady_clock::time_point>(
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds{30});
            const QString session_id = ctx->id;
            QPointer<QTcpSocket> sock = socket;

            auto* timer = new QTimer(m_impl->http);
            timer->setInterval(10);
            QObject::connect(
                timer, &QTimer::timeout, timer,
                [reply_shared, budget, session_id, sock, timer]() {
                    QList<std::pair<QByteArray, QByteArray>> h{
                        {QByteArrayLiteral("Mcp-Session-Id"),
                         session_id.toUtf8()}};
                    if (!sock ||
                        sock->state() != QAbstractSocket::ConnectedState) {
                        timer->stop();
                        timer->deleteLater();
                        return;
                    }
                    if (reply_shared->wait_for(std::chrono::milliseconds{0}) ==
                        std::future_status::ready) {
                        timer->stop();
                        timer->deleteLater();
                        try {
                            QByteArray rbody = reply_shared->get();
                            write_raw_http_response(
                                sock,
                                {200, QByteArrayLiteral("application/json"),
                                 rbody, std::move(h)});
                        } catch (const std::exception& e) {
                            write_raw_http_response(
                                sock, {500, QByteArrayLiteral("text/plain"),
                                       QByteArray("handler error: ") + e.what(),
                                       std::move(h)});
                        } catch (...) {
                            write_raw_http_response(
                                sock, {500, QByteArrayLiteral("text/plain"),
                                       QByteArrayLiteral("handler error"),
                                       std::move(h)});
                        }
                        return;
                    }
                    if (std::chrono::steady_clock::now() >= *budget) {
                        timer->stop();
                        timer->deleteLater();
                        write_raw_http_response(
                            sock, {504, QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("handler timed out"),
                                   std::move(h)});
                    }
                });
            QObject::connect(socket, &QAbstractSocket::disconnected, timer,
                             [timer]() {
                                 timer->stop();
                                 timer->deleteLater();
                             });
            timer->start();
        }));

    // ---- GET: long-lived SSE stream for server-initiated traffic ----
    // Registered as a raw router rule because Qt 6.4 has no public
    // streaming API; we write HTTP/1.1 chunked framing directly.
    http->router()->addRule<std::function<void()>>(
        std::make_unique<QHttpServerRouterRule>(
            m_opts.path, QHttpServerRequest::Method::Get,
            [this](const QRegularExpressionMatch&,
                   const QHttpServerRequest& req, QTcpSocket* socket) {
                if (!origin_allowed(req, m_opts.allowed_origins)) {
                    write_raw_http_error(socket, 403, "Origin not allowed");
                    return;
                }
                const auto auth = authorize(req, m_opts);
                if (!auth.allowed) {
                    write_raw_http_error(socket, auth.status,
                                         auth.body.toUtf8(),
                                         auth.challenge.toUtf8());
                    return;
                }
                const auto sid = header_value(req, "Mcp-Session-Id");
                if (!sid || sid->isEmpty()) {
                    // No session -> we have no business holding a stream.
                    write_raw_http_error(socket, 405, "");
                    return;
                }
                std::shared_ptr<SessionContext> ctx;
                {
                    std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
                    auto sit = m_impl->sessions.find(QString::fromUtf8(*sid));
                    if (sit != m_impl->sessions.end())
                        ctx = sit.value();
                }
                if (!ctx) {
                    write_raw_http_error(socket, 404, "");
                    return;
                }

                auto transport = ctx->transport;
                transport->enter_get_stream();

                QByteArray head = QByteArrayLiteral("HTTP/1.1 200 OK\r\n");
                head +=
                    QByteArrayLiteral("Content-Type: text/event-stream\r\n");
                head += QByteArrayLiteral("Cache-Control: no-cache\r\n");
                head += QByteArrayLiteral("Connection: keep-alive\r\n");
                head += QByteArrayLiteral("Transfer-Encoding: chunked\r\n");
                head += QByteArrayLiteral("\r\n");
                socket->write(head);

                QPointer<QTcpSocket> sock = socket;
                auto* timer = new QTimer(m_impl->http);
                timer->setInterval(10);
                auto last_beat =
                    std::make_shared<std::chrono::steady_clock::time_point>(
                        std::chrono::steady_clock::now());

                QObject::connect(
                    timer, &QTimer::timeout, timer,
                    [transport, sock, timer, last_beat]() {
                        if (!sock || transport->is_closed() ||
                            sock->state() != QAbstractSocket::ConnectedState) {
                            transport->leave_get_stream();
                            // Close the TCP connection so the client's GET
                            // reply observes EOF and its read loop returns.
                            // Without this the peer can hang forever waiting
                            // on a stream the server has already abandoned.
                            if (QTcpSocket* s = sock.data())
                                s->disconnectFromHost();
                            timer->stop();
                            timer->deleteLater();
                            return;
                        }
                        while (auto frame = transport->try_get_frame()) {
                            write_sse_chunk(sock, *frame);
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now - *last_beat >= std::chrono::seconds{15}) {
                            *last_beat = now;
                            write_sse_comment(sock);
                        }
                    });
                QObject::connect(socket, &QAbstractSocket::disconnected, timer,
                                 [transport, timer]() {
                                     transport->leave_get_stream();
                                     timer->stop();
                                     timer->deleteLater();
                                 });
                timer->start();
            }));

    // ---- DELETE: terminate session ----
    http->route(
        m_opts.path, QHttpServerRequest::Method::Delete,
        [this](const QHttpServerRequest& req) -> QHttpServerResponse {
            if (!origin_allowed(req, m_opts.allowed_origins)) {
                return text_response(
                    "Origin not allowed",
                    QHttpServerResponder::StatusCode::Forbidden);
            }
            const auto auth = authorize(req, m_opts);
            if (!auth.allowed) {
                auto r = text_response(auth.body, status_from_int(auth.status));
                if (!auth.challenge.isEmpty()) {
                    r.setHeader(QByteArrayLiteral("WWW-Authenticate"),
                                auth.challenge.toUtf8());
                }
                return r;
            }
            const auto it = header_value(req, "Mcp-Session-Id");
            if (!it) {
                return QHttpServerResponse(
                    QHttpServerResponder::StatusCode::BadRequest);
            }
            std::shared_ptr<SessionContext> ctx;
            {
                std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
                auto sit = m_impl->sessions.find(QString::fromUtf8(*it));
                if (sit != m_impl->sessions.end()) {
                    ctx = sit.value();
                    m_impl->sessions.erase(sit);
                }
            }
            if (ctx) {
                // Notify the embedder while the Server is still fully alive
                // (before close/join/destruction) so it can drop any raw
                // Server* it holds. The sessions_mu is already released.
                invoke_session_closed(m_opts, *ctx->server);
                // Close the transport first, let its on_close cascade
                // unblock the server's run loop, then join.
                ctx->transport->close();
                if (ctx->run_thread.joinable())
                    ctx->run_thread.join();
            }
            return QHttpServerResponse(
                QHttpServerResponder::StatusCode::NoContent);
        });

    // ---- OPTIONS preflight ----
    if (m_opts.enable_cors_preflight) {
        http->route(
            m_opts.path, QHttpServerRequest::Method::Options,
            [this](const QHttpServerRequest& req) -> QHttpServerResponse {
                const auto acao =
                    resolve_cors_origin(req, m_opts.allowed_origins);
                if (acao.isEmpty()) {
                    return QHttpServerResponse(
                        QHttpServerResponder::StatusCode::Forbidden);
                }
                auto r = QHttpServerResponse(
                    QHttpServerResponder::StatusCode::NoContent);
                r.setHeader(QByteArrayLiteral("Access-Control-Allow-Origin"),
                            acao.toUtf8());
                r.setHeader(QByteArrayLiteral("Access-Control-Allow-Methods"),
                            QByteArrayLiteral("GET, POST, DELETE, OPTIONS"));
                r.setHeader(QByteArrayLiteral("Access-Control-Allow-Headers"),
                            QByteArrayLiteral("Authorization, Content-Type, "
                                              "Mcp-Session-Id, "
                                              "MCP-Protocol-Version, "
                                              "Last-Event-ID, Accept"));
                r.setHeader(
                    QByteArrayLiteral("Access-Control-Expose-Headers"),
                    QByteArrayLiteral("Mcp-Session-Id, WWW-Authenticate"));
                r.setHeader(QByteArrayLiteral("Access-Control-Max-Age"),
                            QByteArrayLiteral("600"));
                return r;
            });
    }

    // ---- RFC 9728 Protected Resource Metadata ----
    if (!m_opts.resource_metadata.isNull()) {
        const QByteArray body = dump(m_opts.resource_metadata).toUtf8();
        const auto handler =
            [body](const QHttpServerRequest&) -> QHttpServerResponse {
            auto r =
                QHttpServerResponse(QByteArrayLiteral("application/json"), body,
                                    QHttpServerResponder::StatusCode::Ok);
            r.setHeader(QByteArrayLiteral("Access-Control-Allow-Origin"),
                        QByteArrayLiteral("*"));
            r.setHeader(QByteArrayLiteral("Cache-Control"),
                        QByteArrayLiteral("public, max-age=3600"));
            return r;
        };
        http->route(QStringLiteral("/.well-known/oauth-protected-resource"),
                    QHttpServerRequest::Method::Get, handler);
        if (m_opts.path.size() > 1) {
            http->route(
                QStringLiteral("/.well-known/oauth-protected-resource") +
                    m_opts.path,
                QHttpServerRequest::Method::Get, handler);
        }
    }

    // ---- Bind + run on the dedicated thread ----
    http->moveToThread(m_impl->thread);
    QObject::connect(m_impl->thread, &QThread::finished, http,
                     &QObject::deleteLater);

    auto bound = std::make_shared<std::promise<int>>();
    auto bound_fut = bound->get_future();
    QObject::connect(m_impl->thread, &QThread::started, http, [this, bound]() {
        QHostAddress addr;
        if (m_opts.host == "localhost") {
            addr = QHostAddress::LocalHost;
        } else {
            addr = QHostAddress(m_opts.host);
        }
        const quint16 requested = static_cast<quint16>(m_opts.port);
        const quint16 actual = m_impl->http->listen(addr, requested);
        const int result = actual != 0 ? static_cast<int>(actual) : -1;
        m_impl->port.store(result, std::memory_order_release);
        bound->set_value(result);
    });

    m_impl->thread->start();
    const int actual_port = bound_fut.get();
    if (actual_port <= 0) {
        m_started.store(false, std::memory_order_release);
        // Tear the half-started thread down before throwing.
        m_impl->thread->quit();
        m_impl->thread->wait();
        m_impl->http = nullptr;  // deleted via the finished->deleteLater link
        delete m_impl->thread;
        m_impl->thread = nullptr;
        throw std::runtime_error(
            (QStringLiteral("HttpServerHost: bind failed for ") + m_opts.host +
             QLatin1Char(':') + QString::number(m_opts.port))
                .toStdString());
    }
}

void HttpServerHost::stop(void) {
    if (m_stopping.exchange(true, std::memory_order_acq_rel))
        return;
    if (!m_started.load(std::memory_order_acquire))
        return;

    if (m_impl) {
        // Close transports first so any in-flight handler (a POST
        // awaiting a sampling reply that will never arrive) can
        // return. If we stopped the server before this, those
        // handlers would be left waiting on their timers.
        //
        // We deliberately do NOT call ctx->server->stop() here: both
        // stop() and transport->close() race to wake the run thread,
        // but transport->close() also runs the Session's on_close hook
        // (which touches Session members). Letting the transport's
        // on_close -> user-on_closed cascade be the sole wake-up path
        // keeps "Session destruction" properly happens-after
        // "on_close finished".
        QHash<QString, std::shared_ptr<SessionContext>> drained;
        {
            std::lock_guard<std::mutex> lk(m_impl->sessions_mu);
            drained.swap(m_impl->sessions);
        }
        // Notify the embedder for every surviving session while its
        // Server is still fully alive, before any close/join/destroy.
        for (auto it = drained.begin(); it != drained.end(); ++it) {
            invoke_session_closed(m_opts, *it.value()->server);
        }
        for (auto it = drained.begin(); it != drained.end(); ++it) {
            it.value()->transport->close();
        }

        // Drain the server's event loop on its own thread and join it.
        // QHttpServer (Qt 6.4) has no stop(); tearing the thread down
        // and letting the finished->deleteLater link destroy the
        // server closes its listening sockets.
        if (m_impl->thread) {
            m_impl->thread->quit();
            m_impl->thread->wait();
        }
        m_impl->http = nullptr;
        delete m_impl->thread;
        m_impl->thread = nullptr;

        for (auto it = drained.begin(); it != drained.end(); ++it) {
            if (it.value()->run_thread.joinable())
                it.value()->run_thread.join();
        }
    }
}

}  // namespace mcp
