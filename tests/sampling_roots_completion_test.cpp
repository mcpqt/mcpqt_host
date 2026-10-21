// SPDX-License-Identifier: AGPL-3.0-only
//
// Phase 3 integration tests: server-initiated sampling, roots,
// completion. The interesting one is sampling — it exercises the
// session's worker dispatch (a tool handler in the server thread
// must be able to await a request it issued back to the client).

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

namespace {

using namespace std::chrono_literals;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "phase3",
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
// Round-trip type tests
// -------------------------------------------------------------------------

class Sampling : public QObject {
    Q_OBJECT
private slots:
    void CreateMessageRoundTrip(void);
    void IncludeContextRoundTrip(void);
    void CreateMessageResultRoundTrip(void);
};

void Sampling::CreateMessageRoundTrip(void) {
    mcp::CreateMessageRequestParams p{
        .messages = {mcp::SamplingMessage{
            .role = mcp::Role::user,
            .content = {mcp::TextContent{.text = "hi"}},
        }},
        .model_preferences =
            mcp::ModelPreferences{
                .hints = QList<mcp::ModelHint>{{.name = "claude-3-5-sonnet"}},
                .intelligence_priority = 0.9,
            },
        .max_tokens = 256,
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["maxTokens"], mcp::test::jv(256));
    QCOMPARE(j["modelPreferences"]["hints"][0]["name"],
             mcp::test::jv("claude-3-5-sonnet"));
    auto back = mcp::from_json<mcp::CreateMessageRequestParams>(j);
    QCOMPARE(back.max_tokens, std::int64_t{256});
    QCOMPARE(back.messages.size(), std::size_t{1});
}

void Sampling::IncludeContextRoundTrip(void) {
    mcp::CreateMessageRequestParams p{
        .messages = {mcp::SamplingMessage{
            .role = mcp::Role::user,
            .content = {mcp::TextContent{.text = "x"}},
        }},
        .include_context = mcp::IncludeContext::allServers,
        .max_tokens = 1,
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["includeContext"], mcp::test::jv("allServers"));
    auto back = mcp::from_json<mcp::CreateMessageRequestParams>(j);
    QCOMPARE(back.include_context, mcp::IncludeContext::allServers);
}

void Sampling::CreateMessageResultRoundTrip(void) {
    mcp::CreateMessageResult r{
        .role = mcp::Role::assistant,
        .content = {mcp::TextContent{.text = "answer"}},
        .model = "claude-3-5-sonnet-20241022",
        .stop_reason = "endTurn",
    };
    QJsonValue j = mcp::to_json(r);
    QCOMPARE(j["role"], mcp::test::jv("assistant"));
    QCOMPARE(j["model"], mcp::test::jv("claude-3-5-sonnet-20241022"));
    QCOMPARE(j["stopReason"], mcp::test::jv("endTurn"));
    auto back = mcp::from_json<mcp::CreateMessageResult>(j);
    QCOMPARE(back.model, r.model);
}

class Roots : public QObject {
    Q_OBJECT
private slots:
    void RoundTrip(void);
};

void Roots::RoundTrip(void) {
    mcp::ListRootsResult r{
        .roots =
            {
                mcp::Root{.uri = "file:///workspace", .name = "workspace"},
                mcp::Root{.uri = "https://example.com/data"},
            },
    };
    QJsonValue j = mcp::to_json(r);
    QCOMPARE(j["roots"][0]["name"], mcp::test::jv("workspace"));
    auto back = mcp::from_json<mcp::ListRootsResult>(j);
    QCOMPARE(back.roots.size(), std::size_t{2});
}

class Completion : public QObject {
    Q_OBJECT
private slots:
    void ReferenceVariantRoundTrip(void);
    void RequestRoundTrip(void);
};

void Completion::ReferenceVariantRoundTrip(void) {
    mcp::CompletionReference ref = mcp::PromptReference{.name = "p1"};
    QJsonValue j = mcp::to_json(ref);
    QCOMPARE(j["type"], mcp::test::jv("ref/prompt"));
    auto back = mcp::from_json<mcp::CompletionReference>(j);
    QVERIFY(back.is_prompt());
    QCOMPARE(static_cast<mcp::PromptReference>(back).name, QString{"p1"});

    ref = mcp::ResourceTemplateReference{.uri_template = "file:///{x}"};
    j = mcp::to_json(ref);
    QCOMPARE(j["type"], mcp::test::jv("ref/resource"));
    QCOMPARE(j["uri"], mcp::test::jv("file:///{x}"));
}

void Completion::RequestRoundTrip(void) {
    mcp::CompleteRequestParams p{
        .reference = mcp::PromptReference{.name = "greet"},
        .argument = mcp::CompleteArgument{.name = "name", .value = "Al"},
        .context_arguments =
            QHash<QString, QString>{
                {"locale", "en-US"},
            },
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["argument"]["name"], mcp::test::jv("name"));
    QCOMPARE(j["argument"]["value"], mcp::test::jv("Al"));
    QCOMPARE(j["context"]["arguments"]["locale"], mcp::test::jv("en-US"));
    auto back = mcp::from_json<mcp::CompleteRequestParams>(j);
    QCOMPARE(back.argument.value, QString{"Al"});
    QVERIFY(back.context_arguments.has_value());
}

// -------------------------------------------------------------------------
// Sampling integration: server tool calls server.sample()
// -------------------------------------------------------------------------

class SamplingIntegration : public QObject {
    Q_OBJECT
private slots:
    void ToolHandlerCanRoundTripThroughClient(void);
    void MissingHandlerYieldsError(void);
};

void SamplingIntegration::ToolHandlerCanRoundTripThroughClient(void) {
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool(
            "ask",
            QJsonObject{{"type", "object"},
                        {"properties",
                         QJsonObject{{"q", QJsonObject{{"type", "string"}}}}}},
            [&s](const QJsonValue& args) -> mcp::CallToolResult {
                const QJsonObject ao = args.toObject();
                const QJsonValue qv = ao.value(QStringLiteral("q"));
                const QString q = qv.isString() ? qv.toString() : QString{"?"};
                mcp::CreateMessageRequestParams params{
                    .messages = {mcp::SamplingMessage{
                        .role = mcp::Role::user,
                        .content = {mcp::TextContent{.text = q}},
                    }},
                    .max_tokens = 100,
                };
                auto fut = s.sample(std::move(params));
                auto res = fut.get();
                return mcp::CallToolResult{
                    .content = {mcp::TextContent{
                        .text =
                            "got: " +
                            std::get<mcp::TextContent>(res.content.at(0)).text,
                    }},
                };
            });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_sampling_handler([](const mcp::CreateMessageRequestParams& req)
                                    -> mcp::CreateMessageResult {
        const QString in =
            std::get<mcp::TextContent>(req.messages.at(0).content.at(0)).text;
        return mcp::CreateMessageResult{
            .role = mcp::Role::assistant,
            .content = {mcp::TextContent{.text = "echo:" + in}},
            .model = "test-model",
        };
    });

    auto info = client.initialize().get();
    (void)info;

    auto out = client.call_tool("ask", QJsonObject{{"q", "hello"}}).get();
    QVERIFY(!out.is_error.value_or(false));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
             QString{"got: echo:hello"});

    client.disconnect();
}

void SamplingIntegration::MissingHandlerYieldsError(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool(
            "ask", QJsonObject{{"type", "object"}},
            [&s](const QJsonValue&) -> mcp::CallToolResult {
                try {
                    (void)s
                        .sample(mcp::CreateMessageRequestParams{
                            .messages = {mcp::SamplingMessage{
                                .role = mcp::Role::user,
                                .content = {mcp::TextContent{.text = "x"}},
                            }},
                            .max_tokens = 1,
                        })
                        .get();
                } catch (const mcp::Error&) {
                    return mcp::CallToolResult{
                        .content = {mcp::TextContent{.text = "no-sampling"}},
                        .is_error = true,
                    };
                }
                return mcp::CallToolResult{
                    .content = {mcp::TextContent{.text = "unexpected-success"}},
                };
            });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));  // no sampling handler
    (void)client.initialize().get();

    auto out = client.call_tool("ask").get();
    QVERIFY(out.is_error.value_or(false));

    client.disconnect();
}

// -------------------------------------------------------------------------
// Roots integration
// -------------------------------------------------------------------------

class RootsIntegration : public QObject {
    Q_OBJECT
private slots:
    void ServerCanQueryClientRoots(void);
};

void RootsIntegration::ServerCanQueryClientRoots(void) {
    auto p = mcp::test::make_in_memory_pair();

    std::promise<mcp::ListRootsResult> roots_received;
    auto roots_future = roots_received.get_future();

    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.tool("read_roots", QJsonObject{{"type", "object"}},
               [&s, &roots_received](const QJsonValue&) -> mcp::CallToolResult {
                   auto roots = s.list_roots().get();
                   try {
                       roots_received.set_value(roots);
                   } catch (...) {
                   }
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{
                           .text = QString::number(roots.roots.size())}},
                   };
               });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_roots_list_handler([]() -> mcp::ListRootsResult {
        return {
            .roots =
                {
                    mcp::Root{.uri = "file:///a", .name = "A"},
                    mcp::Root{.uri = "file:///b"},
                },
        };
    });
    auto info = client.initialize().get();
    QVERIFY(info.capabilities.tools.has_value());

    auto out = client.call_tool("read_roots").get();
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString{"2"});

    auto roots = roots_future.get();
    QCOMPARE(roots.roots.size(), std::size_t{2});
    client.disconnect();
}

// -------------------------------------------------------------------------
// Completion integration
// -------------------------------------------------------------------------

class CompletionIntegration : public QObject {
    Q_OBJECT
private slots:
    void ServerProvidesCompletions(void);
};

void CompletionIntegration::ServerProvidesCompletions(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_completion([](const mcp::CompleteRequestParams& req)
                                -> mcp::CompletionValues {
            if (req.reference.is_prompt()) {
                return mcp::CompletionValues{
                    .values = {req.argument.value + "_a",
                               req.argument.value + "_b"},
                };
            }
            return {};
        });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    QVERIFY(!info.capabilities.completions.isNull());

    auto out =
        client
            .complete(mcp::PromptReference{.name = "greet"},
                      mcp::CompleteArgument{.name = "name", .value = "Al"})
            .get();
    QCOMPARE(out.completion.values.size(), std::size_t{2});
    QCOMPARE(out.completion.values[0], QString{"Al_a"});

    client.disconnect();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Sampling suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        Roots suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        Completion suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        SamplingIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        RootsIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        CompletionIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "sampling_roots_completion_test.moc"
#include <QHash>
#include <QList>
#include <QString>
