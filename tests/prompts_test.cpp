// SPDX-License-Identifier: AGPL-3.0-only
//
// Tests for the prompts surface added in Phase 2: list_prompts,
// get_prompt, plus the protocol types PromptArgument, Prompt,
// PromptMessage.

#include "in_memory_transport.h"

#include "client.h"
#include "protocol.h"
#include "server.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace
{

  using namespace std::chrono_literals;

  // -------------------------------------------------------------------------
  // Server + Client integration harness
  // -------------------------------------------------------------------------

  struct PromptsServerThread
  {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    PromptsServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "prompts-test",
              .version = "1.0",
          }))
    {
      cfg(*server);
      thread = std::thread(
          [s = server, t = std::move(t)]() mutable { s->run(std::move(t)); });
    }
    ~PromptsServerThread()
    {
      server->stop();
      if(thread.joinable())
        thread.join();
    }
  };

} // namespace

// -------------------------------------------------------------------------
// Round-trip type tests
// -------------------------------------------------------------------------

class Prompts : public QObject
{
  Q_OBJECT
private slots:
  void PromptArgumentRoundTrip(void);
  void PromptRoundTripWithArguments(void);
  void PromptMessageRoundTrip(void);
  void GetPromptResultRoundTrip(void);
  void GetPromptParamsArgumentsAreStringMap(void);
};

void Prompts::PromptArgumentRoundTrip(void)
{
  mcp::PromptArgument a{
    .name = "topic",
    .title = "Topic",
    .description = "What to write about",
    .required = true,
  };
  QJsonValue j = mcp::to_json(a);
  QCOMPARE(j["name"], mcp::test::jv("topic"));
  QCOMPARE(j["required"], mcp::test::jv(true));
  auto back = mcp::from_json<mcp::PromptArgument>(j);
  QCOMPARE(back.name, a.name);
  QCOMPARE(back.required, a.required);
}

void Prompts::PromptRoundTripWithArguments(void)
{
  mcp::Prompt p{
        .name = "summarize",
        .description = "Summarize text",
        .arguments =
            QList<mcp::PromptArgument>{
                {.name = "text", .required = true},
                {.name = "max_words"},
            },
    };
  QJsonValue j = mcp::to_json(p);
  QVERIFY(j.toObject().contains(QString::fromUtf8("arguments")));
  QCOMPARE(j["arguments"].toArray().size(), qsizetype{ 2 });
  auto back = mcp::from_json<mcp::Prompt>(j);
  QVERIFY(back.arguments.has_value());
  QCOMPARE(back.arguments->size(), std::size_t{ 2 });
  QCOMPARE((*back.arguments)[0].name, QString{ "text" });
}

void Prompts::PromptMessageRoundTrip(void)
{
  mcp::PromptMessage m{
    .role = mcp::Role::user,
    .content = mcp::TextContent{ .text = "hello" },
  };
  QJsonValue j = mcp::to_json(m);
  QCOMPARE(j["role"], mcp::test::jv("user"));
  QCOMPARE(j["content"]["type"], mcp::test::jv("text"));
  auto back = mcp::from_json<mcp::PromptMessage>(j);
  QCOMPARE(back.role, mcp::Role::user);
  QVERIFY(std::holds_alternative<mcp::TextContent>(back.content));
}

void Prompts::GetPromptResultRoundTrip(void)
{
  mcp::GetPromptResult r{
        .description = "A summary prompt",
        .messages =
            {
                mcp::PromptMessage{.role = mcp::Role::user,
                                   .content =
                                       mcp::TextContent{.text = "Summarize:"}},
                mcp::PromptMessage{.role = mcp::Role::assistant,
                                   .content = mcp::TextContent{.text = "OK."}},
            },
    };
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["description"], mcp::test::jv("A summary prompt"));
  QCOMPARE(j["messages"].toArray().size(), qsizetype{ 2 });
  auto back = mcp::from_json<mcp::GetPromptResult>(j);
  QCOMPARE(back.messages.size(), std::size_t{ 2 });
  QCOMPARE(back.messages[0].role, mcp::Role::user);
}

void Prompts::GetPromptParamsArgumentsAreStringMap(void)
{
  mcp::GetPromptRequestParams p{
        .name = "x",
        .arguments =
            QHash<QString, QString>{
                {"a", "1"},
                {"b", "two"},
            },
    };
  QJsonValue j = mcp::to_json(p);
  QCOMPARE(j["name"], mcp::test::jv("x"));
  QCOMPARE(j["arguments"]["a"], mcp::test::jv("1"));
  QCOMPARE(j["arguments"]["b"], mcp::test::jv("two"));
  auto back = mcp::from_json<mcp::GetPromptRequestParams>(j);
  QVERIFY(back.arguments.has_value());
  QCOMPARE((*back.arguments)["a"], QString{ "1" });
}

// -------------------------------------------------------------------------
// Server + Client integration
// -------------------------------------------------------------------------

class PromptsIntegration : public QObject
{
  Q_OBJECT
private slots:
  void ListPromptsAndGetPrompt(void);
  void MissingPromptMapsToError(void);
  void EmptyArgumentsAreOkay(void);
};

void PromptsIntegration::ListPromptsAndGetPrompt(void)
{
  auto p = mcp::test::make_in_memory_pair();
  PromptsServerThread srv(std::move(p.b), [](mcp::Server& s) {
    s.prompt(
            mcp::Prompt{
                .name = "greet",
                .title = "Greet",
                .description = "Greet someone by name",
                .arguments =
                    QList<mcp::PromptArgument>{
                        {.name = "name", .required = true},
                    },
            },
            [](const QHash<QString, QString>& args) {
                const QString name =
                    args.contains("name") ? args.value("name") : "world";
                return mcp::GetPromptResult{
                    .description = "greeting",
                    .messages =
                        {
                            mcp::PromptMessage{
                                .role = mcp::Role::user,
                                .content = mcp::TextContent{.text = "Hello, " +
                                                                    name + "!"},
                            },
                        },
                };
            });
  });

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0" } };
  client.connect(std::move(p.a));
  auto info = client.initialize().get();
  QVERIFY(info.capabilities.prompts.has_value());

  auto list = client.list_prompts().get();
  QCOMPARE(list.prompts.size(), std::size_t{ 1 });
  QCOMPARE(list.prompts[0].name, QString{ "greet" });
  QVERIFY(list.prompts[0].arguments.has_value());

  auto out = client
                 .get_prompt("greet",
                             QHash<QString, QString>{ { "name", "Alice" } })
                 .get();
  QCOMPARE(out.messages.size(), std::size_t{ 1 });
  QVERIFY(std::holds_alternative<mcp::TextContent>(out.messages[0].content));
  QVERIFY(std::get<mcp::TextContent>(out.messages[0].content)
              .text.contains("Alice"));

  client.disconnect();
}

void PromptsIntegration::MissingPromptMapsToError(void)
{
  auto p = mcp::test::make_in_memory_pair();
  PromptsServerThread srv(std::move(p.b), [](mcp::Server& s) {
    s.prompt(mcp::Prompt{ .name = "x" }, [](const QHash<QString, QString>&) {
      return mcp::GetPromptResult{};
    });
  });

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto fut = client.get_prompt("nope");
  try
  {
    (void)fut.get();
    QFAIL("expected Error");
  }
  catch(const mcp::Error& e)
  {
    QCOMPARE(e.code(), mcp::error_code::method_not_found);
  }
  client.disconnect();
}

void PromptsIntegration::EmptyArgumentsAreOkay(void)
{
  std::atomic<bool> handler_saw_empty_args = false;
  auto p = mcp::test::make_in_memory_pair();
  PromptsServerThread srv(std::move(p.b), [&](mcp::Server& s) {
    s.prompt(mcp::Prompt{ .name = "noargs" },
             [&](const QHash<QString, QString>& args) {
               handler_saw_empty_args = args.empty();
               return mcp::GetPromptResult{
                         .messages =
                             {
                                 mcp::PromptMessage{
                                     .role = mcp::Role::user,
                                     .content = mcp::TextContent{.text = "ok"},
                                 },
                             },
                     };
             });
  });

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto out = client.get_prompt("noargs").get();
  QCOMPARE(out.messages.size(), std::size_t{ 1 });
  QVERIFY(handler_saw_empty_args.load());
  client.disconnect();
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int status = 0;
  {
    Prompts suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    PromptsIntegration suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  return status;
}

#include "prompts_test.moc"
#include <QHash>
#include <QList>
#include <QString>
