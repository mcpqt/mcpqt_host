// SPDX-License-Identifier: AGPL-3.0-only
//
// Phase 4a tests: elicitation. Cover both wire-shape round-trips and
// the server-initiated-with-handler integration path.

#include "in_memory_transport.h"

#include "client.h"
#include "protocol.h"
#include "server.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <variant>

namespace {

using mcp::Json;
using namespace std::chrono_literals;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "elicit-srv",
              .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread(
            [s = server, t = std::move(t)]() mutable { s->run(std::move(t)); });
    }
    ~ServerThread() {
        server->stop();
        if (thread.joinable())
            thread.join();
    }
};

}  // namespace

// -------------------------------------------------------------------------
// Wire-shape round trips
// -------------------------------------------------------------------------

class Elicitation : public QObject {
    Q_OBJECT
private slots:
    void FormParamsRoundTrip(void);
    void UrlParamsRoundTrip(void);
    void VariantTagDispatch(void);
    void ResultRoundTrip(void);
    void CompleteNotificationParamsRoundTrip(void);
    void ModeNullRejected(void);
    void RequestedSchemaMustBeObject(void);
};

void Elicitation::FormParamsRoundTrip(void) {
    mcp::ElicitFormRequestParams p{
        .message = "what's your name?",
        .requested_schema =
            QJsonObject{
                {"type", "object"},
                {"properties",
                 QJsonObject{{"name", QJsonObject{{"type", "string"}}}}},
                {"required", QJsonArray{"name"}},
            },
    };
    Json j = mcp::to_json(p);
    QCOMPARE(j["message"], mcp::test::jv("what's your name?"));
    QVERIFY(!j.toObject().contains(
        QString::fromUtf8("mode")));  // form is the omitted-default
    auto back = mcp::from_json<mcp::ElicitFormRequestParams>(j);
    QCOMPARE(back.message, p.message);
    QCOMPARE(back.requested_schema["properties"]["name"]["type"],
             mcp::test::jv("string"));
}

void Elicitation::UrlParamsRoundTrip(void) {
    mcp::ElicitUrlRequestParams p{
        .message = "please authorise",
        .url = "https://example.com/auth?session=xyz",
        .elicitation_id = "abc-123",
    };
    Json j = mcp::to_json(p);
    QCOMPARE(j["mode"], mcp::test::jv("url"));
    QCOMPARE(j["url"], mcp::test::jv("https://example.com/auth?session=xyz"));
    auto back = mcp::from_json<mcp::ElicitUrlRequestParams>(j);
    QCOMPARE(back.elicitation_id, std::string{"abc-123"});
}

void Elicitation::VariantTagDispatch(void) {
    // Form mode: omitted "mode"
    Json form_wire = QJsonObject{
        {"message", "msg"},
        {"requestedSchema", QJsonObject{{"type", "object"}}},
    };
    auto v = mcp::from_json<mcp::ElicitRequestParams>(form_wire);
    QVERIFY(std::holds_alternative<mcp::ElicitFormRequestParams>(v));

    // URL mode
    Json url_wire = QJsonObject{
        {"mode", "url"},
        {"message", "go"},
        {"url", "https://example.com"},
        {"elicitationId", "id-1"},
    };
    auto v2 = mcp::from_json<mcp::ElicitRequestParams>(url_wire);
    QVERIFY(std::holds_alternative<mcp::ElicitUrlRequestParams>(v2));

    // Unknown mode is rejected (so a future "mode": "voice" doesn't
    // get silently parsed as a form).
    Json bad = QJsonObject{{"mode", "voice"}, {"message", "?"}};
    QVERIFY_EXCEPTION_THROWN(
        (void)mcp::from_json<mcp::ElicitRequestParams>(bad), mcp::Error);
}

void Elicitation::ResultRoundTrip(void) {
    mcp::ElicitResult r{
        .action = mcp::ElicitAction::accept,
        .content = QJsonObject{{"name", "Alice"}, {"age", 30}},
    };
    Json j = mcp::to_json(r);
    QCOMPARE(j["action"], mcp::test::jv("accept"));
    QCOMPARE(j["content"]["name"], mcp::test::jv("Alice"));
    auto back = mcp::from_json<mcp::ElicitResult>(j);
    QCOMPARE(back.action, mcp::ElicitAction::accept);
    QVERIFY(back.content.has_value());
    QCOMPARE((*back.content)["age"], mcp::test::jv(30));

    // Decline / cancel: content omitted on the wire.
    mcp::ElicitResult r2{.action = mcp::ElicitAction::decline};
    Json j2 = mcp::to_json(r2);
    QVERIFY(!j2.toObject().contains(QString::fromUtf8("content")));
    QCOMPARE(j2["action"], mcp::test::jv("decline"));
    auto back2 = mcp::from_json<mcp::ElicitResult>(j2);
    QCOMPARE(back2.action, mcp::ElicitAction::decline);
    QVERIFY(!back2.content.has_value());

    // Unknown action string ⇒ Error.
    Json bad = QJsonObject{{"action", "nope"}};
    QVERIFY_EXCEPTION_THROWN((void)mcp::from_json<mcp::ElicitResult>(bad),
                             mcp::Error);
}

void Elicitation::CompleteNotificationParamsRoundTrip(void) {
    mcp::ElicitationCompleteNotificationParams p{
        .elicitation_id = "abc",
    };
    Json j = mcp::to_json(p);
    QCOMPARE(j["elicitationId"], mcp::test::jv("abc"));
    QCOMPARE(mcp::from_json<mcp::ElicitationCompleteNotificationParams>(j)
                 .elicitation_id,
             std::string{"abc"});
}

// -------------------------------------------------------------------------
// Integration: tool handler issues an elicitation, client responds
// -------------------------------------------------------------------------

class ElicitationIntegration : public QObject {
    Q_OBJECT
private slots:
    void FormModeRoundTripThroughClient(void);
    void DeclineCarriesThrough(void);
    void MissingHandlerYieldsError(void);
    void UrlModeNotificationDeliversToClient(void);
    void UrlModeFullRoundTripWithCompletion(void);
    void ServerReceivesElicitationComplete(void);
    void CapabilityActuallyAdvertisedOnInitialize(void);
    void NarrowedCapabilityOverrideKeepsOtherDerivedCaps(void);
};

void ElicitationIntegration::FormModeRoundTripThroughClient(void) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("ask_name", QJsonObject{{"type", "object"}},
               [&s](const Json&) -> mcp::CallToolResult {
                   auto fut = s.elicit(mcp::ElicitFormRequestParams{
                       .message = "name?",
                       .requested_schema =
                           QJsonObject{
                               {"type", "object"},
                               {"properties",
                                QJsonObject{
                                    {"name", QJsonObject{{"type", "string"}}}}},
                           },
                   });
                   auto er = fut.get();
                   if (er.action != mcp::ElicitAction::accept ||
                       !er.content.has_value()) {
                       return mcp::CallToolResult{
                           .content = {mcp::TextContent{.text = "no answer"}},
                           .is_error = true,
                       };
                   }
                   const QJsonObject co = er.content->toObject();
                   const QJsonValue nv = co.value(QStringLiteral("name"));
                   const std::string nm = nv.isString()
                                              ? nv.toString().toStdString()
                                              : std::string{"<none>"};
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "hi " + nm}},
                   };
               });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    bool held_form = false;
    std::string got_message;
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [&](const mcp::ElicitRequestParams& req) -> mcp::ElicitResult {
            // Should be form mode in this test.
            held_form =
                std::holds_alternative<mcp::ElicitFormRequestParams>(req);
            if (held_form) {
                got_message =
                    std::get<mcp::ElicitFormRequestParams>(req).message;
            }
            return mcp::ElicitResult{
                .action = mcp::ElicitAction::accept,
                .content = QJsonObject{{"name", "Ada"}},
            };
        });
    auto info = client.initialize().get();
    QVERIFY(info.capabilities.tools.has_value());
    // The capability we declared should propagate through initialize.
    // (The server reflects ours back to us via... no, it doesn't —
    //  but our local advertise was set under m_handlers_mu, so the
    //  initialize payload included `elicitation: {form:{}, url:{}}`.)

    auto out = client.call_tool("ask_name").get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(out.content.size(), std::size_t{1});
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             std::string{"hi Ada"});
    QVERIFY(held_form);
    QCOMPARE(got_message, std::string{"name?"});

    client.disconnect();
}

void ElicitationIntegration::DeclineCarriesThrough(void) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("touch", QJsonObject{{"type", "object"}},
               [&s](const Json&) -> mcp::CallToolResult {
                   auto er = s.elicit(mcp::ElicitFormRequestParams{
                                          .message = "?",
                                          .requested_schema =
                                              QJsonObject{{"type", "object"}},
                                      })
                                 .get();
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{
                           .text = (er.action == mcp::ElicitAction::decline)
                                       ? "declined"
                                       : "other",
                       }},
                   };
               });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::decline};
        });
    (void)client.initialize().get();

    auto out = client.call_tool("touch").get();
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             std::string{"declined"});

    client.disconnect();
}

void ElicitationIntegration::MissingHandlerYieldsError(void) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool(
            "touch", QJsonObject{{"type", "object"}},
            [&s](const Json&) -> mcp::CallToolResult {
                try {
                    (void)s
                        .elicit(mcp::ElicitFormRequestParams{
                            .message = "?",
                            .requested_schema = QJsonObject{{"type", "object"}},
                        })
                        .get();
                } catch (const mcp::Error&) {
                    return mcp::CallToolResult{
                        .content = {mcp::TextContent{.text = "no-elicit"}},
                        .is_error = true,
                    };
                }
                return mcp::CallToolResult{
                    .content = {mcp::TextContent{.text = "unexpected-success"}},
                };
            });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));  // no elicitation handler
    (void)client.initialize().get();

    auto out = client.call_tool("touch").get();
    QVERIFY(out.is_error.value_or(false));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             std::string{"no-elicit"});

    client.disconnect();
}

void ElicitationIntegration::UrlModeNotificationDeliversToClient(void) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("kick", QJsonObject{{"type", "object"}},
               [&s](const Json&) -> mcp::CallToolResult {
                   const auto ec = s.notify_elicitation_complete("eid-77");
                   if (ec) {
                       return mcp::CallToolResult{
                           .content = {mcp::TextContent{
                               .text = std::string{"notify failed: "} +
                                       ec.message(),
                           }},
                           .is_error = true,
                       };
                   }
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "kicked"}},
                   };
               });
    });

    std::promise<std::string> got;
    auto fut = got.get_future();
    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_elicitation_complete_handler([&](std::string id) {
        try {
            got.set_value(std::move(id));
        } catch (...) {
        }
    });
    (void)client.initialize().get();

    auto out = client.call_tool("kick").get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             std::string{"kicked"});

    QCOMPARE(fut.wait_for(2s), std::future_status::ready);
    QCOMPARE(fut.get(), std::string{"eid-77"});

    client.disconnect();
}

void ElicitationIntegration::UrlModeFullRoundTripWithCompletion(void) {
    // Audit fix: the previous URL-mode test only fired a stand-alone
    // notification; it never issued an actual URL-mode elicitation
    // through Server::elicit. This test does the real thing:
    //   server.elicit(URL params) → client returns accept →
    //   server emits notifications/elicitation/complete →
    //   client receives the completion.
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("oauth_kickoff", QJsonObject{{"type", "object"}},
               [&s](const Json&) -> mcp::CallToolResult {
                   auto er =
                       s.elicit(mcp::ElicitUrlRequestParams{
                                    .message = "Authorise via the linked URL",
                                    .url = "https://idp.example/auth?code=xyz",
                                    .elicitation_id = "eid-url-1",
                                })
                           .get();
                   if (er.action != mcp::ElicitAction::accept) {
                       return mcp::CallToolResult{
                           .content = {mcp::TextContent{.text = "no"}},
                           .is_error = true,
                       };
                   }
                   (void)s.notify_elicitation_complete("eid-url-1");
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "ok"}},
                   };
               });
    });

    std::promise<std::string> completion_p;
    auto completion_fut = completion_p.get_future();

    bool got_url_mode_request = false;
    std::string got_elicitation_id;
    std::string got_url;
    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [&](const mcp::ElicitRequestParams& req) -> mcp::ElicitResult {
            if (std::holds_alternative<mcp::ElicitUrlRequestParams>(req)) {
                const auto& u = std::get<mcp::ElicitUrlRequestParams>(req);
                got_elicitation_id = u.elicitation_id;
                got_url = u.url;
                got_url_mode_request = true;
            }
            return mcp::ElicitResult{.action = mcp::ElicitAction::accept};
        });
    client.set_elicitation_complete_handler([&](std::string id) {
        try {
            completion_p.set_value(std::move(id));
        } catch (...) {
        }
    });
    (void)client.initialize().get();

    auto out = client.call_tool("oauth_kickoff").get();
    QVERIFY(!out.is_error.value_or(false));
    QVERIFY(got_url_mode_request);
    QCOMPARE(got_elicitation_id, std::string{"eid-url-1"});
    QVERIFY(got_url.find("idp.example") != std::string::npos);

    QCOMPARE(completion_fut.wait_for(2s), std::future_status::ready);
    QCOMPARE(completion_fut.get(), std::string{"eid-url-1"});

    client.disconnect();
}

void ElicitationIntegration::ServerReceivesElicitationComplete(void) {
    // Audit fix: the spec lets EITHER side emit
    // notifications/elicitation/complete. The server side previously
    // had no API to receive it. Verify the full round-trip:
    //   client.notify_elicitation_complete(id) →
    //   server's set_elicitation_complete_handler fires with id.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<std::string> server_got;
    auto server_fut = server_got.get_future();

    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.set_elicitation_complete_handler([&](std::string id) {
            try {
                server_got.set_value(std::move(id));
            } catch (...) {
            }
        });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    const auto ec = client.notify_elicitation_complete("eid-from-client");
    QVERIFY2(!ec, qPrintable(QStringLiteral("send_notification failed: ") +
                             QString::fromStdString(ec.message())));

    QCOMPARE(server_fut.wait_for(2s), std::future_status::ready);
    QCOMPARE(server_fut.get(), std::string{"eid-from-client"});

    client.disconnect();
}

void ElicitationIntegration::CapabilityActuallyAdvertisedOnInitialize(void) {
    // Audit fix: the FormModeRoundTripThroughClient test admitted
    // it never verified the capability was advertised. This test
    // captures the inbound InitializeRequestParams server-side
    // and asserts the elicitation capability was sent.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ClientCapabilities> caps_p;
    auto caps_fut = caps_p.get_future();

    // We need access to the request params before the server's
    // built-in initialize handler consumes them. Use a raw Session
    // on the b-side instead of a Server, so we can install our own
    // initialize handler that captures + responds.
    auto b_session = std::make_unique<mcp::Session>(std::move(p.b));
    b_session->set_request_handler(
        "initialize", [&](const mcp::Json& params) -> mcp::Json {
            auto parsed = mcp::from_json<mcp::InitializeRequestParams>(params);
            try {
                caps_p.set_value(parsed.capabilities);
            } catch (...) {
            }
            return mcp::to_json(mcp::InitializeResult{
                .protocol_version = std::string{mcp::kLatestProtocolVersion},
                .capabilities = mcp::ServerCapabilities{},
                .server_info =
                    mcp::Implementation{
                        .name = "x",
                        .version = "0",
                    },
            });
        });
    b_session->start();

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::accept};
        });

    (void)client.initialize().get();

    QCOMPARE(caps_fut.wait_for(2s), std::future_status::ready);
    auto caps = caps_fut.get();
    QVERIFY(caps.elicitation.has_value());
    QVERIFY(caps.elicitation->form.has_value());
    QVERIFY(caps.elicitation->url.has_value());

    client.disconnect();
    b_session->close();
}

void ElicitationIntegration::NarrowedCapabilityOverrideKeepsOtherDerivedCaps(
    void) {
    // Audit fix: set_client_capabilities() was a *full replacement*,
    // dropping handler-derived sampling/roots/elicitation. Now it's
    // a per-field override — engaged fields replace, unset fields
    // keep their derived values.
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ClientCapabilities> caps_p;
    auto caps_fut = caps_p.get_future();

    auto b_session = std::make_unique<mcp::Session>(std::move(p.b));
    b_session->set_request_handler(
        "initialize", [&](const mcp::Json& params) -> mcp::Json {
            auto parsed = mcp::from_json<mcp::InitializeRequestParams>(params);
            try {
                caps_p.set_value(parsed.capabilities);
            } catch (...) {
            }
            return mcp::to_json(mcp::InitializeResult{
                .protocol_version = std::string{mcp::kLatestProtocolVersion},
                .capabilities = mcp::ServerCapabilities{},
                .server_info =
                    mcp::Implementation{
                        .name = "x",
                        .version = "0",
                    },
            });
        });
    b_session->start();

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_sampling_handler(
        [](const mcp::CreateMessageRequestParams&) -> mcp::CreateMessageResult {
            return {.role = mcp::Role::assistant, .model = "m"};
        });
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return {.action = mcp::ElicitAction::accept};
        });
    // Override: narrow elicitation to form-only. Sampling override
    // not set — should keep the handler-derived value.
    mcp::ClientCapabilities override_caps;
    mcp::ElicitationCapability ec;
    ec.form = QJsonObject{};
    // ec.url left absent ⇒ form-only.
    override_caps.elicitation = std::move(ec);
    client.set_client_capabilities(std::move(override_caps));

    (void)client.initialize().get();
    QCOMPARE(caps_fut.wait_for(2s), std::future_status::ready);
    auto caps = caps_fut.get();
    QVERIFY(caps.elicitation.has_value());
    QVERIFY(caps.elicitation->form.has_value());
    QVERIFY(!caps.elicitation->url.has_value());
    // The sampling capability survived because we didn't override it.
    QVERIFY(caps.sampling.has_value());

    client.disconnect();
    b_session->close();
}

void Elicitation::ModeNullRejected(void) {
    Json j =
        QJsonObject{{"mode", QJsonValue(QJsonValue::Null)}, {"message", "?"}};
    QVERIFY_EXCEPTION_THROWN((void)mcp::from_json<mcp::ElicitRequestParams>(j),
                             mcp::Error);
}

void Elicitation::RequestedSchemaMustBeObject(void) {
    Json j = QJsonObject{
        {"message", "?"},
        {"requestedSchema", "not-an-object"},
    };
    QVERIFY_EXCEPTION_THROWN(
        (void)mcp::from_json<mcp::ElicitFormRequestParams>(j), mcp::Error);

    Json j2 = QJsonObject{
        {"message", "?"},
        {"requestedSchema", QJsonValue(QJsonValue::Null)},
    };
    QVERIFY_EXCEPTION_THROWN(
        (void)mcp::from_json<mcp::ElicitFormRequestParams>(j2), mcp::Error);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Elicitation suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        ElicitationIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "elicitation_test.moc"
