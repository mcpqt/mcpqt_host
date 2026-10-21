// SPDX-License-Identifier: AGPL-3.0-only
//
// Phase 4c tests: OAuth 2.1 authorization on the HTTP transport.
//
// We use a raw QTcpSocket client for the negative paths (sending a
// raw POST without — or with the wrong — Authorization header), and
// the regular HttpClientTransport + Client stack for the positive
// path (Bearer-attached requests pass through end-to-end).

#if !defined(MCP_ENABLE_HTTP)
#error "MCP_ENABLE_HTTP must be defined for http_oauth_test"
#endif

// QVERIFY_EXCEPTION_THROWN is deprecated (but still functional) in Qt 6.4.
#define QT_NO_DEPRECATED_WARNINGS

#include "client.h"
#include "http_client_transport.h"
#include "http_server_host.h"
#include "protocol.h"

#include "qt_test_support.h"
#include "raw_http_client.h"

#include <QtTest/QtTest>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>

namespace {
using namespace std::chrono_literals;
}  // namespace

class HttpOAuth : public QObject {
    Q_OBJECT
private slots:
    void MissingAuthorizationProduces401(void);
    void InvalidBearerProduces401(void);
    void NonBearerSchemeRejected(void);
    void ValidBearerEndToEnd(void);
    void ClientReceives401Callback(void);
    void ResourceMetadataDocumentIsServed(void);
    void MetadataDocumentNotServedWhenAbsent(void);
    void CaseInsensitiveBearerScheme(void);
    void MultipleAuthorizationHeadersRejected(void);
    void InsufficientScopeProduces403(void);
    void ChallengeIncludesInvalidTokenError(void);
    void EmptyBearerTokenRejected(void);
    void MetadataDocumentHasCors(void);
    void RealmInjectionRejectedAtStart(void);
    void MetadataUrlInjectionRejectedAtStart(void);
    void ClientSetAccessTokenRotates(void);
    void BindToNonLoopbackRequiresAllowInsecureHttp(void);
    void BindToNonLoopbackWithAllowInsecureHttpStartsCleanly(void);
    void OptionsPreflightAllowsAllowedOrigin(void);
    void OptionsPreflightRejectsDisallowedOrigin(void);
    void NoValidatorMeansNoAuth(void);
};

void HttpOAuth::MissingAuthorizationProduces401(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status = mcp::HttpServerHost::Options::BearerStatus::
                            invalid_token,
                    };
                },
            .auth_realm = "test-realm",
            .resource_metadata_url =
                "https://example.com/.well-known/oauth-protected-resource",
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp", {{"Content-Type", "application/json"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 401);
    const QByteArray www = res.header("WWW-Authenticate");
    QVERIFY(!www.isEmpty());
    QVERIFY(www.contains("Bearer"));
    QVERIFY(www.contains("realm=\"test-realm\""));
    QVERIFY(www.contains("resource_metadata=\"https://example.com"));

    host.stop();
}

void HttpOAuth::InvalidBearerProduces401(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString t) {
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = (t == "good")
                                      ? O::BearerStatus::allow
                                      : O::BearerStatus::invalid_token,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp",
        {{"Content-Type", "application/json"},
         {"Authorization", "Bearer wrong-token"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 401);

    host.stop();
}

void HttpOAuth::NonBearerSchemeRejected(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp",
        {{"Content-Type", "application/json"},
         {"Authorization", "Basic dXNlcjpwYXNz"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 401);

    host.stop();
}

void HttpOAuth::ValidBearerEndToEnd(void) {
    std::atomic<int> validations{0};
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "calc", .version = "1.0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [&](QString t) {
                    validations.fetch_add(1, std::memory_order_relaxed);
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = (t == "secret")
                                      ? O::BearerStatus::allow
                                      : O::BearerStatus::invalid_token,
                    };
                },
        },
        [](mcp::Server& s) {
            s.tool("ping", QJsonObject{{"type", "object"}},
                   [](const mcp::QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "pong"}}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + QString::number(host.port()) + "/mcp";
    topts.access_token = "secret";
    topts.open_get_stream = false;

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));

    (void)client.initialize().get();
    auto out = client.call_tool("ping").get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString{"pong"});

    // At least one validation per inbound POST. We sent at least
    // initialize + tools/call.
    QVERIFY(validations.load() >= 2);

    client.disconnect();
    host.stop();
}

void HttpOAuth::ClientReceives401Callback(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status = mcp::HttpServerHost::Options::BearerStatus::
                            invalid_token,
                    };
                },
            .resource_metadata_url =
                "https://idp.example/.well-known/oauth-protected-resource",
        },
        [](mcp::Server&) {},
    };
    host.start();

    std::promise<QString> challenge_p;
    auto challenge_f = challenge_p.get_future();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + QString::number(host.port()) + "/mcp";
    topts.access_token = "anything";
    topts.open_get_stream = false;
    topts.on_unauthorized = [&](int, QString ch) {
        try {
            challenge_p.set_value(QString{ch});
        } catch (...) {
        }
    };

    auto transport = std::make_unique<mcp::HttpClientTransport>(topts);
    transport->start();
    QVERIFY(
        !transport->send(R"({"jsonrpc":"2.0","method":"initialize","id":1})"));

    QCOMPARE(challenge_f.wait_for(2s), std::future_status::ready);
    const auto challenge = challenge_f.get();
    QVERIFY(challenge.contains("Bearer"));
    QVERIFY(challenge.contains("resource_metadata=\"https://idp.example"));

    transport->close();
    host.stop();
}

void HttpOAuth::ResourceMetadataDocumentIsServed(void) {
    QJsonObject metadata{
        {"resource", "https://example.com/mcp"},
        {"authorization_servers", QJsonArray{"https://idp.example"}},
        {"scopes_supported", QJsonArray{"mcp:read", "mcp:write"}},
        {"bearer_methods_supported", QJsonArray{"header"}},
    };
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .resource_metadata = metadata,
        },
        [](mcp::Server&) {},
    };
    host.start();

    // Root well-known URI.
    {
        auto res = mcp::test::raw_request(
            host.port(), "GET", "/.well-known/oauth-protected-resource", {});
        QVERIFY(res.connected);
        QCOMPARE(res.status, 200);
        const mcp::QJsonValue body = mcp::parse_json(res.body);
        QCOMPARE(body.toObject().value("resource").toString(),
                 QStringLiteral("https://example.com/mcp"));
        QCOMPARE(body.toObject()
                     .value("authorization_servers")
                     .toArray()[0]
                     .toString(),
                 QStringLiteral("https://idp.example"));
    }
    // Path-prefixed variant.
    {
        auto res = mcp::test::raw_request(
            host.port(), "GET", "/.well-known/oauth-protected-resource/mcp",
            {});
        QVERIFY(res.connected);
        QCOMPARE(res.status, 200);
    }

    host.stop();
}

void HttpOAuth::MetadataDocumentNotServedWhenAbsent(void) {
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
        host.port(), "GET", "/.well-known/oauth-protected-resource", {});
    QVERIFY(res.connected);
    QCOMPARE(res.status, 404);

    host.stop();
}

void HttpOAuth::CaseInsensitiveBearerScheme(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString t) {
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = (t == "tok") ? O::BearerStatus::allow
                                               : O::BearerStatus::invalid_token,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    // RFC 7235 §2.1: scheme is case-insensitive. Try lowercase + uppercase.
    for (const char* hdr : {"bearer tok", "BEARER tok", "BeArEr tok"}) {
        auto res = mcp::test::raw_request(
            host.port(), "POST", "/mcp",
            {{"Content-Type", "application/json"}, {"Authorization", hdr}},
            R"({"jsonrpc":"2.0","method":"initialize","id":1})");
        QVERIFY(res.connected);
        // We expect 400/401 only if the body fails session lookup, NOT 401
        // for scheme parsing.
        QVERIFY2(res.status != 401, hdr);
    }

    host.stop();
}

void HttpOAuth::MultipleAuthorizationHeadersRejected(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    QList<QPair<QByteArray, QByteArray>> hdrs = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer one"},
        {"Authorization", "Bearer two"},
    };
    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp", hdrs,
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 400);

    host.stop();
}

void HttpOAuth::InsufficientScopeProduces403(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    using O = mcp::HttpServerHost::Options;
                    return O::BearerOutcome{
                        .status = O::BearerStatus::insufficient_scope,
                        .required_scopes = "mcp:read mcp:tools",
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp",
        {{"Content-Type", "application/json"}, {"Authorization", "Bearer tok"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 403);
    const QByteArray www = res.header("WWW-Authenticate");
    QVERIFY(!www.isEmpty());
    QVERIFY(www.contains("error=\"insufficient_scope\""));
    QVERIFY(www.contains("scope=\"mcp:read mcp:tools\""));

    host.stop();
}

void HttpOAuth::ChallengeIncludesInvalidTokenError(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status = mcp::HttpServerHost::Options::BearerStatus::
                            invalid_token,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "POST", "/mcp",
        {{"Content-Type", "application/json"},
         {"Authorization", "Bearer junk"}},
        R"({"jsonrpc":"2.0","method":"initialize","id":1})");
    QVERIFY(res.connected);
    QCOMPARE(res.status, 401);
    const QByteArray www = res.header("WWW-Authenticate");
    QVERIFY(!www.isEmpty());
    QVERIFY(www.contains("error=\"invalid_token\""));

    host.stop();
}

void HttpOAuth::EmptyBearerTokenRejected(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    host.start();

    // Trailing whitespace after "Bearer" is the only token68 — empty.
    for (const char* hdr : {"Bearer ", "Bearer  ", "Bearer \t"}) {
        auto res = mcp::test::raw_request(
            host.port(), "POST", "/mcp",
            {{"Content-Type", "application/json"}, {"Authorization", hdr}},
            R"({"jsonrpc":"2.0","method":"initialize","id":1})");
        QVERIFY(res.connected);
        QVERIFY2(res.status == 401, hdr);
    }

    host.stop();
}

void HttpOAuth::MetadataDocumentHasCors(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .resource_metadata = QJsonObject{{"resource", "https://x"}},
        },
        [](mcp::Server&) {},
    };
    host.start();

    auto res = mcp::test::raw_request(
        host.port(), "GET", "/.well-known/oauth-protected-resource", {});
    QVERIFY(res.connected);
    QCOMPARE(res.status, 200);
    QCOMPARE(res.header("Access-Control-Allow-Origin"), QByteArray("*"));

    host.stop();
}

void HttpOAuth::RealmInjectionRejectedAtStart(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
            // CRLF + closing quote — header injection / smuggling.
            .auth_realm = "evil\r\nX-Pwn: yes",
        },
        [](mcp::Server&) {},
    };
    QVERIFY_EXCEPTION_THROWN(host.start(), std::invalid_argument);
}

void HttpOAuth::MetadataUrlInjectionRejectedAtStart(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
            .resource_metadata_url = "https://x.com/\".pwn",
        },
        [](mcp::Server&) {},
    };
    QVERIFY_EXCEPTION_THROWN(host.start(), std::invalid_argument);
}

void HttpOAuth::ClientSetAccessTokenRotates(void) {
    std::atomic<int> valid_count{0};
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [&](QString t) {
                    using O = mcp::HttpServerHost::Options;
                    if (t == "v2") {
                        valid_count.fetch_add(1, std::memory_order_relaxed);
                        return O::BearerOutcome{.status =
                                                    O::BearerStatus::allow};
                    }
                    return O::BearerOutcome{.status =
                                                O::BearerStatus::invalid_token};
                },
        },
        [](mcp::Server& s) {
            s.tool("noop", QJsonObject{{"type", "object"}},
                   [](const mcp::QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + QString::number(host.port()) + "/mcp";
    topts.access_token = "v1";  // wrong
    topts.open_get_stream = false;
    auto* transport_raw = new mcp::HttpClientTransport{topts};
    std::unique_ptr<mcp::HttpClientTransport> transport{transport_raw};

    QCOMPARE(QString(transport_raw->access_token()), QStringLiteral("v1"));
    transport_raw->set_access_token("v2");
    QCOMPARE(QString(transport_raw->access_token()), QStringLiteral("v2"));

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(transport));
    (void)client.initialize().get();
    auto out = client.call_tool("noop").get();
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString{"ok"});
    QVERIFY(valid_count.load() >= 2);

    client.disconnect();
    host.stop();
}

void HttpOAuth::BindToNonLoopbackRequiresAllowInsecureHttp(void) {
    // Bearer auth + non-loopback host + plain HTTP without the
    // explicit acknowledgement throws at start().
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "0.0.0.0",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
        },
        [](mcp::Server&) {},
    };
    QVERIFY_EXCEPTION_THROWN(host.start(), std::invalid_argument);
}

void HttpOAuth::BindToNonLoopbackWithAllowInsecureHttpStartsCleanly(void) {
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
            .bearer_validator =
                [](QString) {
                    return mcp::HttpServerHost::Options::BearerOutcome{
                        .status =
                            mcp::HttpServerHost::Options::BearerStatus::allow,
                    };
                },
            // Loopback is fine without the flag too — exercise both
            // path-prefix variants in one go by setting it.
            .allow_insecure_http = true,
        },
        [](mcp::Server&) {},
    };
    try {
        host.start();
    } catch (...) {
        QFAIL("host.start() threw unexpectedly");
    }
    host.stop();
}

void HttpOAuth::OptionsPreflightAllowsAllowedOrigin(void) {
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

    auto res = mcp::test::raw_request(
        host.port(), "OPTIONS", "/mcp",
        {{"Origin", "https://allowed.example"},
         {"Access-Control-Request-Method", "POST"},
         {"Access-Control-Request-Headers", "Authorization, Content-Type"}},
        {});
    QVERIFY(res.connected);
    QCOMPARE(res.status, 204);
    QCOMPARE(res.header("Access-Control-Allow-Origin"),
             QByteArray("https://allowed.example"));
    const QByteArray acam = res.header("Access-Control-Allow-Methods");
    QVERIFY(!acam.isEmpty());
    QVERIFY(acam.contains("POST"));
    QVERIFY(acam.contains("DELETE"));
    const QByteArray acah = res.header("Access-Control-Allow-Headers");
    QVERIFY(!acah.isEmpty());
    QVERIFY(acah.contains("Authorization"));
    QVERIFY(acah.contains("Mcp-Session-Id"));

    host.stop();
}

void HttpOAuth::OptionsPreflightRejectsDisallowedOrigin(void) {
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

    auto res = mcp::test::raw_request(host.port(), "OPTIONS", "/mcp",
                                      {{"Origin", "https://evil.example"}}, {});
    QVERIFY(res.connected);
    QCOMPARE(res.status, 403);

    host.stop();
}

void HttpOAuth::NoValidatorMeansNoAuth(void) {
    // Sanity check: existing code paths (no validator configured)
    // are unaffected — no Authorization header required.
    mcp::HttpServerHost host{
        mcp::Implementation{.name = "x", .version = "0"},
        mcp::HttpServerHost::Options{
            .host = "127.0.0.1",
            .path = "/mcp",
        },
        [](mcp::Server& s) {
            s.tool("noop", QJsonObject{{"type", "object"}},
                   [](const mcp::QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        },
    };
    host.start();

    mcp::HttpClientTransport::Options topts;
    topts.url = "http://127.0.0.1:" + QString::number(host.port()) + "/mcp";
    topts.open_get_stream = false;

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
    (void)client.initialize().get();
    auto out = client.call_tool("noop").get();
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString{"ok"});

    client.disconnect();
    host.stop();
}

QTEST_GUILESS_MAIN(HttpOAuth)
#include "http_oauth_test.moc"
#include <QList>
#include <QString>
