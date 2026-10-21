// SPDX-License-Identifier: AGPL-3.0-only
//
// Streamable HTTP client transport.
//
// Implements the client side of the MCP "Streamable HTTP" transport
// (spec 2025-11-25). One transport instance corresponds to one logical
// MCP session against a single HTTP endpoint:
//
//   * `send()` issues an HTTP POST to the endpoint with the JSON-RPC
//     frame as the body. The response Content-Type determines the
//     fan-out: `application/json` is delivered as a single frame to
//     the on_message callback; `text/event-stream` is parsed as SSE
//     and each `data:` event payload is delivered as its own frame.
//     A 202 Accepted response (sent for inbound notifications and
//     responses) is silently absorbed.
//   * On `start()`, an optional GET stream is opened to the same
//     endpoint to receive server-initiated requests / notifications
//     (sampling, log messages, list-changed, etc.). The stream is
//     parsed as SSE.
//   * The optional `Mcp-Session-Id` header is round-tripped: any
//     server response that sets it is captured, and every subsequent
//     request includes it. The negotiated id is exposed via
//     `session_id()`.
//
// Threading: an internal worker thread pumps both POST replies and
// the GET stream. Calls to `send()` are dispatched onto the worker so
// the caller does not block on the HTTP round-trip.

#pragma once

#if !defined(MCP_ENABLE_HTTP)
#error                                                                        \
    "mcp::HttpClientTransport requires MCP_ENABLE_HTTP=1 (CMake option MCP_ENABLE_HTTP=ON)"
#endif

#include "transport.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QString>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace mcp
{

  class HttpClientTransport final : public Transport
  {
  public:
    struct Options
    {
      /// Endpoint URL, e.g. "http://localhost:8080/mcp".
      QString url;

      /// MCP protocol version sent on every request via the
      /// `MCP-Protocol-Version` header. Defaults to the latest spec
      /// version this SDK supports.
      QString protocol_version = "2025-11-25";

      /// Optional pre-existing session id (resume case). Leave empty
      /// for a fresh session — the server will assign one on the
      /// initialize response, and we'll capture it.
      QString session_id;

      /// Connect timeout (TCP handshake + TLS).
      std::chrono::milliseconds connect_timeout{ 10'000 };

      /// Per-request total timeout.
      std::chrono::milliseconds request_timeout{ 30'000 };

      /// Extra HTTP headers attached to every outbound request.
      QHash<QString, QString> extra_headers;

      /// If true, open a long-lived GET stream on `start()` so the
      /// server can push requests and notifications. If false, the
      /// transport is request/response-only (no server-initiated
      /// traffic).
      bool open_get_stream = true;

      // -------- 2025-11-25 OAuth 2.1 authorization (optional) --------
      //
      // When `access_token` is non-empty, every outbound POST and
      // GET carries `Authorization: Bearer <access_token>` per
      // RFC 6750. The SDK does NOT itself perform the OAuth flow
      // (PKCE, dynamic client registration, code exchange) — the
      // application is expected to obtain a token (via Protected
      // Resource Metadata discovery + AS interaction) and supply
      // it here.
      QString access_token;

      /// Hook fired when the server replies 401/403 to a request.
      /// Receives the raw `WWW-Authenticate` header value (may be
      /// empty if the server omits it). The application can parse
      /// it for `resource_metadata="<url>"` to drive discovery.
      /// After this fires, the offending POST has already failed
      /// and the transport delivers an error to the caller; the
      /// callback is purely a side-channel notification.
      std::function<void(int status, const QString& www_authenticate)>
          on_unauthorized;
    };

    explicit HttpClientTransport(Options opts);
    ~HttpClientTransport() override;

    // Transport interface ---------------------------------------------------

    void on_message(MessageCallback cb) override;
    void on_error(ErrorCallback cb) override;
    void on_close(CloseCallback cb) override;

    void start(void) override;
    void close(void) override;

    [[nodiscard]] std::error_code send(QByteArrayView frame) override;
    [[nodiscard]] bool is_open(void) const noexcept override
    {
      return m_started.load(std::memory_order_acquire)
             && !m_closed.load(std::memory_order_acquire);
    }

    // HTTP-specific ---------------------------------------------------------

    /// The negotiated `Mcp-Session-Id`, or empty if none has been
    /// established yet (the server may opt out).
    [[nodiscard]] QString session_id(void) const;

    /// Update the `Authorization: Bearer` token used on every
    /// subsequent request — the obvious recovery path from an
    /// `on_unauthorized` callback. Pass an empty string to drop the
    /// header entirely. Thread-safe.
    void set_access_token(QString token);

    /// Read the current access token. Thread-safe.
    [[nodiscard]] QString access_token(void) const;

  private:
    void worker_loop(void);
    void process_send(QByteArray frame);
    void deliver_frame(QByteArray raw);
    void deliver_error(std::error_code ec) noexcept;
    void fire_close(void) noexcept;
    void run_get_stream(void) noexcept;

    Options m_opts;

    std::atomic<bool> m_started{ false };
    std::atomic<bool> m_closed{ false };
    std::atomic<bool> m_close_fired{ false };

    // Outbound queue: frames the application has handed us that are
    // waiting to go out as POSTs. The worker thread drains, then
    // dispatches each frame to its own short-lived sender thread so
    // many POSTs can be in flight simultaneously without serialising
    // (a long-blocked POST awaiting a server-initiated sampling
    // round-trip would otherwise wedge later sends).
    std::mutex m_out_mu;
    std::condition_variable m_out_cv;
    std::deque<QByteArray> m_out;
    std::thread m_worker;

    // Track in-flight per-send threads so close() can drain.
    std::mutex m_inflight_mu;
    std::condition_variable m_inflight_cv;
    int m_inflight = 0;

    // GET-stream worker (optional).
    std::thread m_get_thread;

    // Negotiated session id, captured from any inbound
    // `Mcp-Session-Id` response header. The GET worker waits on this
    // cv until a session id has been negotiated (or close fires).
    mutable std::mutex m_session_id_mu;
    std::condition_variable m_session_id_cv;
    QString m_session_id;

    // SSE resumability state (spec: server emits `id:` lines, client
    // sends `Last-Event-ID` on reconnect and honours `retry:` for
    // backoff). Updated by drain_sse on every event; read by the GET
    // worker on reconnect.
    mutable std::mutex m_get_mu_state;
    QString m_last_event_id;
    std::optional<int> m_next_retry_ms;

    // Mutable access token. The constructor seeds this from
    // Options::access_token; set_access_token() rotates it. Reads are
    // taken on every outbound request, so this is hot-ish — but
    // contention is bounded by the number of in-flight POSTs.
    mutable std::mutex m_auth_mu;
    QString m_access_token;

    // on_unauthorized firing-flag. Multiple in-flight POSTs that all
    // hit a 401/403 would otherwise each fire the callback,
    // racing the application's token-refresh logic. Fire ONCE per
    // "auth-fail batch" — re-arm on the next successful 2xx. Reads
    // are atomic-CAS so dedup decisions don't need the m_auth_mu.
    std::atomic<bool> m_auth_callback_armed{ true };

    MessageCallback m_on_message;
    ErrorCallback m_on_error;
    CloseCallback m_on_close;

    // pImpl-style holder for the Qt network state (parsed QUrl, etc.),
    // kept opaque from the public header so consumers don't need
    // QtNetwork's include path on their own translation units.
    struct ClientImpl;
    std::unique_ptr<ClientImpl> m_impl;
  };

} // namespace mcp
