// SPDX-License-Identifier: AGPL-3.0-only
//
// End-to-end tests using Server + Client over an in-memory transport pair.
// These exercise the full path: Client → Session → Transport → Session →
// Server → handler → reverse path back to the Client's future.

#include "in_memory_transport.h"

#include "client.h"
#include "error.h"
#include "protocol.h"
#include "server.h"
#include "transport.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{

  using namespace std::chrono_literals;

  // Run a Server in a background thread with the supplied transport, and
  // return a handle that joins on destruction. Lets the test drive the
  // Client side from the foreground.
  struct ServerThread
  {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ServerThread(mcp::Implementation info,
                 std::unique_ptr<mcp::Transport> transport,
                 Configure configure)
        : server(std::make_shared<mcp::Server>(std::move(info)))
    {
      configure(*server);
      thread = std::thread([s = server, t = std::move(transport)]() mutable {
        s->run(std::move(t));
      });
    }

    ~ServerThread()
    {
      server->stop();
      if(thread.joinable())
        thread.join();
    }
  };

  // Build the canonical "calculator" server with two tools.
  std::unique_ptr<ServerThread>
  calc_server(std::unique_ptr<mcp::Transport> transport)
  {
    return std::make_unique<ServerThread>(
        mcp::Implementation{
            .name = "calc",
            .version = "1.0.0",
        },
        std::move(transport), [](mcp::Server& s) {
          s.set_instructions("A tiny arithmetic server.");
          s.tool("add",
                 QJsonObject{
                     { "type", "object" },
                     { "properties",
                       QJsonObject{
                           { "a", QJsonObject{ { "type", "number" } } },
                           { "b", QJsonObject{ { "type", "number" } } } } },
                     { "required", QJsonArray{ "a", "b" } } },
                 [](const QJsonValue& args) -> mcp::CallToolResult {
                   const double a = args["a"].toDouble();
                   const double b = args["b"].toDouble();
                   return mcp::CallToolResult{
                     .content = { mcp::TextContent{
                         .text = QString::number(a + b),
                     } },
                   };
                 });
          s.tool("divide",
                 QJsonObject{
                     { "type", "object" },
                     { "properties",
                       QJsonObject{
                           { "a", QJsonObject{ { "type", "number" } } },
                           { "b", QJsonObject{ { "type", "number" } } } } },
                     { "required", QJsonArray{ "a", "b" } } },
                 [](const QJsonValue& args) -> mcp::CallToolResult {
                   const double b = args["b"].toDouble();
                   if(b == 0.0)
                   {
                     // Reportable error: visible to the LLM via isError.
                     return mcp::CallToolResult{
                       .content = { mcp::TextContent{
                           .text = "division by zero",
                       } },
                       .is_error = true,
                     };
                   }
                   const double a = args["a"].toDouble();
                   return mcp::CallToolResult{
                     .content = { mcp::TextContent{
                         .text = QString::number(a / b),
                     } },
                   };
                 });
        });
  }

} // namespace

// -------------------------------------------------------------------------

class Integration : public QObject
{
  Q_OBJECT
private slots:
  void InitializeReturnsExpectedShape(void);
  void ListToolsReturnsRegisteredTools(void);
  void CallToolReturnsTextContent(void);
  void ToolErrorIsReportedViaIsError(void);
  void MissingToolMapsToMethodNotFound(void);
  void RequestsBeforeInitializeAreRejected(void);
  void ConcurrentToolCallsAllResolve(void);
  void ServerInfoCachedOnInitialize(void);
};

void Integration::InitializeReturnsExpectedShape(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));

  auto info = client.initialize().get();
  QCOMPARE(info.protocol_version, QString{ mcp::kLatestProtocolVersion });
  QCOMPARE(info.server_info.name, QString{ "calc" });
  QVERIFY(info.capabilities.tools.has_value());
  QVERIFY(!info.instructions.isNull());
  QCOMPARE(info.instructions, QString{ "A tiny arithmetic server." });

  client.disconnect();
}

void Integration::ListToolsReturnsRegisteredTools(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto list = client.list_tools().get();
  QCOMPARE(list.tools.size(), std::size_t{ 2 });
  QList<QString> names;
  for(const auto& t : list.tools)
    names.push_back(t.name);
  std::sort(names.begin(), names.end());
  QCOMPARE(names, (QList<QString>{ "add", "divide" }));

  client.disconnect();
}

void Integration::CallToolReturnsTextContent(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto out = client.call_tool("add", QJsonObject{ { "a", 2.0 }, { "b", 3.5 } })
                 .get();
  QCOMPARE(out.content.size(), std::size_t{ 1 });
  QVERIFY(std::holds_alternative<mcp::TextContent>(out.content[0]));
  const auto& text = std::get<mcp::TextContent>(out.content[0]).text;
  QVERIFY(text.contains("5.5"));
  QVERIFY(!out.is_error.value_or(false));

  client.disconnect();
}

void Integration::ToolErrorIsReportedViaIsError(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto out
      = client.call_tool("divide", QJsonObject{ { "a", 1.0 }, { "b", 0.0 } })
            .get();
  QVERIFY(out.is_error.has_value());
  QVERIFY(*out.is_error);
  QCOMPARE(out.content.size(), std::size_t{ 1 });
  QVERIFY(std::get<mcp::TextContent>(out.content[0])
              .text.contains("division by zero"));

  client.disconnect();
}

void Integration::MissingToolMapsToMethodNotFound(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  auto fut = client.call_tool("does-not-exist");
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

void Integration::RequestsBeforeInitializeAreRejected(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));

  auto fut = client.list_tools();
  try
  {
    (void)fut.get();
    QFAIL("expected Error before initialize");
  }
  catch(const mcp::Error& e)
  {
    QCOMPARE(e.code(), mcp::error_code::invalid_request);
  }

  client.disconnect();
}

void Integration::ConcurrentToolCallsAllResolve(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  (void)client.initialize().get();

  constexpr int N = 50;
  std::vector<std::future<mcp::CallToolResult>> futs;
  futs.reserve(N);
  for(int i = 0; i < N; ++i)
  {
    futs.push_back(
        client.call_tool("add", QJsonObject{ { "a", i }, { "b", 1 } }));
  }
  for(int i = 0; i < N; ++i)
  {
    auto r = futs[i].get();
    QVERIFY(!r.is_error.value_or(false));
  }

  client.disconnect();
}

void Integration::ServerInfoCachedOnInitialize(void)
{
  auto p = mcp::test::make_in_memory_pair();
  auto srv = calc_server(std::move(p.b));

  mcp::Client client{ mcp::Implementation{ .name = "tester",
                                           .version = "0.1.0" } };
  client.connect(std::move(p.a));
  QVERIFY(!client.server().has_value());
  (void)client.initialize().get();
  auto cached = client.server();
  QVERIFY(cached.has_value());
  QCOMPARE(cached->server_info.name, QString{ "calc" });
  client.disconnect();
  QVERIFY(!client.server().has_value());
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int status = 0;
  {
    Integration suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  return status;
}

#include "server_client_integration_test.moc"
#include <QList>
#include <QString>
