// SPDX-License-Identifier: AGPL-3.0-only
//
// End-to-end test: an in-process HttpServerHost paired with a Client
// driving it through HttpClientTransport. Proves the full Streamable
// HTTP path works for a non-streaming server: initialize handshake,
// session-id round-tripping, tools/list, and tools/call.

#if !defined(MCP_ENABLE_HTTP)
#error "MCP_ENABLE_HTTP must be defined for http_e2e_test"
#endif

#include "client.h"
#include "http_client_transport.h"
#include "http_server_host.h"
#include "log.h"
#include "protocol.h"

#include "qt_test_support.h"
#include "raw_http_client.h"

#include <QtTest/QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct EnableTraceLogging {
    EnableTraceLogging(void) {
        if (std::getenv("MCP_TRACE")) {
            mcp::set_log_level(mcp::LogLevel::trace);
        }
    }
};
EnableTraceLogging g_enable_trace;

using Headers = QList<QPair<QByteArray, QByteArray>>;

const char* kInitBody =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
    R"({"protocolVersion":"2025-11-25","capabilities":{},)"
    R"("clientInfo":{"name":"t","version":"0"}}})";

Headers base_headers(void) {
    return {{"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"}};
}

}  // namespace

class HttpEndToEnd : public QObject {
    Q_OBJECT
private slots:
    void InitializeAndToolCall(void);
    void OriginAllowlistRejectsCrossOrigin(void);
    void ServerInitiatedSamplingThroughGetStream(void);
    void SessionIdReusedAcrossCalls(void);
    void UnsupportedMcpProtocolVersionIs400(void);
    void ClientSendsDeleteOnGracefulClose(void);
    void FixedPortIsHonored(void);
    void PostToTerminatedSessionIs404(void);
    void OnSessionClosedFiresOnceOnDelete(void);
    void OnSessionClosedFiresOnStop(void);
    void ClientCapabilitiesCapturedAtInitialize(void);
};

void HttpEndToEnd::InitializeAndToolCall(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "calc-http", .version = "1.0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .port = 0,  // OS-assigned
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("add",
                   QJsonObject{
                       {"type", "object"},
                       {"properties",
                        QJsonObject{{"a", QJsonObject{{"type", "number"}}},
                                    {"b", QJsonObject{{"type", "number"}}}}},
                       {"required", QJsonArray{"a", "b"}}},
                   [](const mcp::Json& args) -> mcp::CallToolResult {
                       const QJsonObject o = args.toObject();
                       const double a = mcp::require<double>(o, "a");
                       const double b = mcp::require<double>(o, "b");
                       return {.content = {mcp::TextContent{
                                   .text = std::to_string(a + b),
                               }}};
                   });
        },
    };
    host.start();
    QVERIFY(host.port() > 0);

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;  // server doesn't offer one in 3e/2

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));

    auto info = client.initialize().get();
    QCOMPARE(info.protocol_version, std::string{mcp::kLatestProtocolVersion});
    QCOMPARE(info.server_info.name, std::string{"calc-http"});
    QVERIFY(info.capabilities.tools.has_value());

    auto tools = client.list_tools().get();
    QCOMPARE(tools.tools.size(), std::size_t{1});
    QCOMPARE(tools.tools[0].name, std::string{"add"});

    auto out =
        client.call_tool("add", QJsonObject{{"a", 2.5}, {"b", 1.5}}).get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(out.content.size(), std::size_t{1});
    QVERIFY(std::holds_alternative<mcp::TextContent>(out.content[0]));
    QVERIFY(std::get<mcp::TextContent>(out.content[0]).text.find("4.") !=
            std::string::npos);

    client.disconnect();
    host.stop();
}

void HttpEndToEnd::OriginAllowlistRejectsCrossOrigin(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .allowed_origins = {"https://allowed.example"},
        },
        [](mcp::Server&) {},
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;
    topts.extra_headers["Origin"] = "https://evil.example";

    auto transport = std::make_unique<mcp::HttpClientTransport>(topts);
    std::promise<std::error_code> got_error;
    auto fut = got_error.get_future();
    transport->on_error([&](std::error_code ec) {
        try {
            got_error.set_value(ec);
        } catch (...) {
        }
    });
    transport->start();
    QVERIFY(!transport->send(R"({"jsonrpc":"2.0","method":"x"})"));

    QCOMPARE(fut.wait_for(2s), std::future_status::ready);
    transport->close();
    host.stop();
}

void HttpEndToEnd::ServerInitiatedSamplingThroughGetStream(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "agent", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            // Tool that itself asks the client to sample. Exercises
            // the full server-initiated → GET-stream → client →
            // back-through-POST round trip.
            s.tool(
                "ask",
                QJsonObject{
                    {"type", "object"},
                    {"properties",
                     QJsonObject{{"q", QJsonObject{{"type", "string"}}}}}},
                [&s](const mcp::Json& args) -> mcp::CallToolResult {
                    const std::string q =
                        args.toObject().value("q").toString("?").toStdString();
                    auto resp =
                        s
                            .sample(mcp::CreateMessageRequestParams{
                                .messages = {mcp::SamplingMessage{
                                    .role = mcp::Role::user,
                                    .content = {mcp::TextContent{.text = q}},
                                }},
                                .max_tokens = 100,
                            })
                            .get();
                    return {.content = {mcp::TextContent{
                                .text = "got: " + std::get<mcp::TextContent>(
                                                      resp.content.at(0))
                                                      .text,
                            }}};
                });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = true;  // required for sampling round-trip

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
    client.set_sampling_handler([](const mcp::CreateMessageRequestParams& req) {
        const std::string in =
            std::get<mcp::TextContent>(req.messages.at(0).content.at(0)).text;
        return mcp::CreateMessageResult{
            .role = mcp::Role::assistant,
            .content = {mcp::TextContent{.text = "echo:" + in}},
            .model = "test-model",
        };
    });
    (void)client.initialize().get();

    auto out = client.call_tool("ask", QJsonObject{{"q", "hello"}}).get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             std::string{"got: echo:hello"});

    client.disconnect();
    host.stop();
}

void HttpEndToEnd::SessionIdReusedAcrossCalls(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("echo", QJsonObject{{"type", "object"}},
                   [](const mcp::Json& args) -> mcp::CallToolResult {
                       (void)args;
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;
    auto transport_ptr = new mcp::HttpClientTransport{topts};
    std::unique_ptr<mcp::HttpClientTransport> transport{transport_ptr};

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(transport));

    (void)client.initialize().get();
    const auto sid = transport_ptr->session_id();
    QVERIFY(sid.has_value());
    QVERIFY(!sid->empty());

    QCOMPARE(host.active_sessions(), std::size_t{1});

    // Multiple call_tool requests should all reuse this session.
    for (int i = 0; i < 3; ++i) {
        auto out = client.call_tool("echo").get();
        QVERIFY(!out.is_error.value_or(false));
    }
    QVERIFY(transport_ptr->session_id() == sid);
    QCOMPARE(host.active_sessions(), std::size_t{1});

    client.disconnect();
    host.stop();
}

void HttpEndToEnd::UnsupportedMcpProtocolVersionIs400(void) {
    // Spec MUST: a request with a present-but-unsupported
    // MCP-Protocol-Version header is rejected with 400.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp",
        {{"Content-Type", "application/json"},
         {"MCP-Protocol-Version", "1999-01-01"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 400);
    QVERIFY(res.body.contains("unsupported MCP-Protocol-Version"));

    host.stop();
}

void HttpEndToEnd::ClientSendsDeleteOnGracefulClose(void) {
    // Spec: a client that no longer needs a session SHOULD send an
    // HTTP DELETE on the MCP path. Verify our transport does it,
    // and that the server's session table reflects the removal.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("noop", QJsonObject{{"type", "object"}},
                   [](const mcp::Json&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + std::to_string(host.port()) + "/mcp";
    topts.open_get_stream = false;

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
    (void)client.initialize().get();
    QCOMPARE(host.active_sessions(), std::size_t{1});

    // Disconnect should drive the transport's close(), which fires
    // the spec-mandated DELETE before stopping the HTTP clients.
    client.disconnect();

    // Allow a brief window for the DELETE to land.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (host.active_sessions() != 0u &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    QCOMPARE(host.active_sessions(), std::size_t{0});

    host.stop();
}

void HttpEndToEnd::FixedPortIsHonored(void) {
    // Regression: start() used to bind to an any-port
    // unconditionally, silently ignoring Options::port. Grab an
    // OS-assigned port with a throwaway host, then demand it back
    // explicitly with a second one.
    int chosen = 0;
    {
        mcp::HttpServerHost probe{
            mcp::Implementation{.name = "probe", .version = "0"},
            mcp::HttpServerHost::Options{.host = "127.0.0.1", .path = "/mcp"},
            [](mcp::Server&) {},
        };
        probe.start();
        chosen = probe.port();
        probe.stop();
    }
    QVERIFY(chosen > 0);

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "fixed", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .port = chosen,
            .path = "/mcp",
        },
        [](mcp::Server&) {},
    };
    host.start();
    QCOMPARE(host.port(), chosen);

    // And it actually answers there.
    auto res = mcp::test::raw_request(host.port(), "POST", "/mcp",
                                      base_headers(), kInitBody);
    QVERIFY(res.connected);
    QCOMPARE(res.status, 200);

    host.stop();
}

void HttpEndToEnd::PostToTerminatedSessionIs404(void) {
    // Spec: a request carrying an Mcp-Session-Id the server no longer
    // recognizes MUST get 404 (the client's cue to re-initialize).
    // A session-less non-initialize POST stays a plain 400.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{.host = "127.0.0.1", .path = "/mcp"},
        [](mcp::Server&) {},
    };
    host.start();

    const Headers base = base_headers();

    auto init =
        mcp::test::raw_request(host.port(), "POST", "/mcp", base, kInitBody);
    QVERIFY(init.connected);
    QCOMPARE(init.status, 200);
    const QByteArray sid = init.header("Mcp-Session-Id");
    QVERIFY(!sid.isEmpty());

    auto del = mcp::test::raw_request(host.port(), "DELETE", "/mcp",
                                      {{"Mcp-Session-Id", sid}});
    QVERIFY(del.connected);
    QCOMPARE(del.status, 204);

    Headers stale = base;
    stale.emplace_back("Mcp-Session-Id", sid);
    auto reuse =
        mcp::test::raw_request(host.port(), "POST", "/mcp", stale,
                               R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    QVERIFY(reuse.connected);
    QCOMPARE(reuse.status, 404);

    auto no_sid =
        mcp::test::raw_request(host.port(), "POST", "/mcp", base,
                               R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
    QVERIFY(no_sid.connected);
    QCOMPARE(no_sid.status, 400);

    host.stop();
}

void HttpEndToEnd::OnSessionClosedFiresOnceOnDelete(void) {
    // The pre-destruction hook must fire exactly once per session on the
    // client-DELETE teardown path, with the Server still alive. Track the
    // Server* seen so we can assert identity, and count invocations.
    std::mutex seen_mu;
    std::vector<mcp::Server*> seen;
    std::atomic<int> calls{0};

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server& s) {
        // The Server is alive here: an any-thread call must succeed.
        (void)s.client_capabilities();
        {
            std::lock_guard<std::mutex> lk(seen_mu);
            seen.push_back(&s);
        }
        calls.fetch_add(1, std::memory_order_relaxed);
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    const Headers base = base_headers();
    auto init =
        mcp::test::raw_request(host.port(), "POST", "/mcp", base, kInitBody);
    QVERIFY(init.connected);
    QCOMPARE(init.status, 200);
    const QByteArray sid = init.header("Mcp-Session-Id");
    QVERIFY(!sid.isEmpty());
    QCOMPARE(calls.load(), 0);  // not yet — session is live

    auto del = mcp::test::raw_request(host.port(), "DELETE", "/mcp",
                                      {{"Mcp-Session-Id", sid}});
    QVERIFY(del.connected);
    QCOMPARE(del.status, 204);

    // Fired exactly once, on the DELETE.
    QCOMPARE(calls.load(), 1);
    {
        std::lock_guard<std::mutex> lk(seen_mu);
        QCOMPARE(seen.size(), std::size_t{1});
        QVERIFY(seen[0] != nullptr);
    }

    // stop() must not re-fire for an already-torn-down session.
    host.stop();
    QCOMPARE(calls.load(), 1);
}

void HttpEndToEnd::OnSessionClosedFiresOnStop(void) {
    // stop() tears down every surviving session; the hook must fire once
    // for each, before any close/join/destroy.
    std::atomic<int> calls{0};

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server&) {
        calls.fetch_add(1, std::memory_order_relaxed);
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    const Headers base = base_headers();
    // Two independent sessions (each a fresh initialize mints its own id).
    for (int i = 0; i < 2; ++i) {
        auto init = mcp::test::raw_request(host.port(), "POST", "/mcp", base,
                                           kInitBody);
        QVERIFY(init.connected);
        QCOMPARE(init.status, 200);
    }
    QCOMPARE(host.active_sessions(), std::size_t{2});
    QCOMPARE(calls.load(), 0);

    host.stop();
    QCOMPARE(calls.load(), 2);

    // Idempotent: a second stop() fires nothing more.
    host.stop();
    QCOMPARE(calls.load(), 2);
}

void HttpEndToEnd::ClientCapabilitiesCapturedAtInitialize(void) {
    // Before initialize: nullopt. After an initialize advertising known
    // capabilities: the accessor returns them, readable from the factory
    // thread via the session-closed hook (any-thread contract).
    mcp::Server fresh{mcp::Implementation{.name = "fresh", .version = "0"}};
    QVERIFY(!fresh.client_capabilities().has_value());

    std::promise<mcp::ClientCapabilities> captured;
    auto captured_fut = captured.get_future();

    mcp::HttpServerHost::Options opts;
    opts.host = "127.0.0.1";
    opts.path = "/mcp";
    opts.on_session_closed = [&](mcp::Server& s) {
        auto caps = s.client_capabilities();
        if (caps) {
            try {
                captured.set_value(*caps);
            } catch (...) {
            }
        }
    };

    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        opts,
        [](mcp::Server&) {},
    };
    host.start();

    const Headers base = base_headers();
    // A client advertising sampling + elicitation.
    auto init = mcp::test::raw_request(
        host.port(), "POST", "/mcp", base,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
        R"({"protocolVersion":"2025-11-25",)"
        R"("capabilities":{"sampling":{},"elicitation":{}},)"
        R"("clientInfo":{"name":"t","version":"0"}}})");
    QVERIFY(init.connected);
    QCOMPARE(init.status, 200);
    const QByteArray sid = init.header("Mcp-Session-Id");
    QVERIFY(!sid.isEmpty());

    auto del = mcp::test::raw_request(host.port(), "DELETE", "/mcp",
                                      {{"Mcp-Session-Id", sid}});
    QVERIFY(del.connected);
    QCOMPARE(del.status, 204);

    QCOMPARE(captured_fut.wait_for(2s), std::future_status::ready);
    const mcp::ClientCapabilities caps = captured_fut.get();
    QVERIFY(caps.sampling.has_value());
    QVERIFY(caps.elicitation.has_value());
    QVERIFY(!caps.roots.has_value());

    host.stop();
}

QTEST_GUILESS_MAIN(HttpEndToEnd)
#include "http_e2e_test.moc"
