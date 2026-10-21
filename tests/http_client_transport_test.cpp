// SPDX-License-Identifier: AGPL-3.0-only
//
// Tests for HttpClientTransport. Each test stands up an in-process
// FakeHttpServer as a stand-in for a real MCP server and points the
// transport at it. We're testing the *transport* here, not the
// protocol.

#if !defined(MCP_ENABLE_HTTP)
// MCP_ENABLE_HTTP is required for these tests; the test target is
// only added to the build when it's on.
#error "MCP_ENABLE_HTTP must be defined for http_client_transport_test"
#endif

#include "http_client_transport.h"

#include "fake_http_server.h"
#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
  using namespace std::chrono_literals;
} // namespace

class HttpClientTransport : public QObject
{
  Q_OBJECT
private slots:
  void PostSingleJsonResponseIsDelivered(void);
  void PostSseResponseDeliversMultipleFrames(void);
  void NotificationGets202AbsorbedSilently(void);
  void McpSessionIdIsCapturedAndEchoed(void);
  void McpProtocolVersionHeaderIsSent(void);
  void GetStreamReceivesServerInitiatedFrames(void);
  void SendBeforeStartFails(void);
  void IdempotentClose(void);
};

void HttpClientTransport::PostSingleJsonResponseIsDelivered(void)
{
  std::mutex mu;
  QString observed_body;
  mcp::test::FakeHttpServer fake(
      [&](const mcp::test::FakeHttpServer::Request& req) {
        std::lock_guard<std::mutex> lk(mu);
        observed_body = req.body;
        return mcp::test::json_response(R"({"ok":true})");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.open_get_stream = false; // simpler for this test
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::promise<QString> got;
  auto fut = got.get_future();
  transport->on_message([&](QString s) {
    try
    {
      got.set_value(std::move(s));
    }
    catch(...)
    {
    }
  });
  transport->start();

  QVERIFY(!transport->send(R"({"hello":1})"));
  QCOMPARE(fut.wait_for(2s), std::future_status::ready);
  QCOMPARE(QString(fut.get()), QString::fromUtf8(R"({"ok":true})"));
  {
    std::lock_guard<std::mutex> lk(mu);
    QCOMPARE(QString(observed_body), QString::fromUtf8(R"({"hello":1})"));
  }
  transport->close();
}

void HttpClientTransport::PostSseResponseDeliversMultipleFrames(void)
{
  mcp::test::FakeHttpServer fake(
      [](const mcp::test::FakeHttpServer::Request&) {
        return mcp::test::sse_response(
            "data: {\"a\":1}\n\ndata: {\"b\":2}\n\n");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::mutex mu;
  std::condition_variable cv;
  QList<QString> frames;
  transport->on_message([&](QString s) {
    std::lock_guard<std::mutex> lk(mu);
    frames.push_back(std::move(s));
    cv.notify_all();
  });
  transport->start();
  QVERIFY(!transport->send("{}"));

  {
    std::unique_lock<std::mutex> lk(mu);
    QVERIFY(cv.wait_for(lk, 2s, [&] { return frames.size() >= 2; }));
  }
  QCOMPARE(frames.size(), std::size_t{ 2 });
  QCOMPARE(QString(frames[0]), QString::fromUtf8(R"({"a":1})"));
  QCOMPARE(QString(frames[1]), QString::fromUtf8(R"({"b":2})"));
  transport->close();
}

void HttpClientTransport::NotificationGets202AbsorbedSilently(void)
{
  std::atomic<int> hits{ 0 };
  mcp::test::FakeHttpServer fake(
      [&](const mcp::test::FakeHttpServer::Request&) {
        ++hits;
        return mcp::test::empty_response(202, "Accepted");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::atomic<int> messages{ 0 };
  transport->on_message([&](QString) { ++messages; });
  transport->start();
  QVERIFY(!transport->send(R"({"jsonrpc":"2.0","method":"x"})"));
  std::this_thread::sleep_for(150ms);

  QCOMPARE(hits.load(), 1);
  QCOMPARE(messages.load(), 0);
  transport->close();
}

void HttpClientTransport::McpSessionIdIsCapturedAndEchoed(void)
{
  std::atomic<int> first_call{ 0 };
  std::mutex mu;
  QString captured_id_on_second_call;
  mcp::test::FakeHttpServer fake(
      [&](const mcp::test::FakeHttpServer::Request& req) {
        const int n = ++first_call;
        if(n == 1)
        {
          return mcp::test::json_response(
              "{}", 200, "OK", { { "Mcp-Session-Id", "deadbeef" } });
        }
        const QByteArray id = req.header("Mcp-Session-Id");
        if(!id.isEmpty())
        {
          std::lock_guard<std::mutex> lk(mu);
          captured_id_on_second_call = id;
        }
        return mcp::test::json_response("{}");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::atomic<int> messages{ 0 };
  transport->on_message([&](QString) { ++messages; });
  transport->start();

  QVERIFY(!transport->send("{}"));
  while(messages.load() < 1)
    std::this_thread::sleep_for(5ms);
  QCOMPARE(transport->session_id(), QString{ "deadbeef" });

  QVERIFY(!transport->send("{}"));
  while(messages.load() < 2)
    std::this_thread::sleep_for(5ms);
  {
    std::lock_guard<std::mutex> lk(mu);
    QCOMPARE(QString(captured_id_on_second_call), QStringLiteral("deadbeef"));
  }

  transport->close();
}

void HttpClientTransport::McpProtocolVersionHeaderIsSent(void)
{
  std::mutex mu;
  QString captured_pv;
  mcp::test::FakeHttpServer fake(
      [&](const mcp::test::FakeHttpServer::Request& req) {
        const QByteArray pv = req.header("MCP-Protocol-Version");
        if(!pv.isEmpty())
        {
          std::lock_guard<std::mutex> lk(mu);
          captured_pv = pv;
        }
        return mcp::test::json_response("{}");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.protocol_version = "2025-11-25";
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::atomic<int> messages{ 0 };
  transport->on_message([&](QString) { ++messages; });
  transport->start();
  QVERIFY(!transport->send("{}"));
  while(messages.load() < 1)
    std::this_thread::sleep_for(5ms);
  {
    std::lock_guard<std::mutex> lk(mu);
    QCOMPARE(QString(captured_pv), QStringLiteral("2025-11-25"));
  }
  transport->close();
}

void HttpClientTransport::GetStreamReceivesServerInitiatedFrames(void)
{
  mcp::test::FakeHttpServer fake(
      [](const mcp::test::FakeHttpServer::Request& req) {
        if(req.method == "GET")
        {
          return mcp::test::sse_response("data: {\"server-said\":\"hi\"}\n\n");
        }
        return mcp::test::empty_response(202, "Accepted");
      });

  mcp::HttpClientTransport::Options opts;
  opts.url = fake.url();
  opts.open_get_stream = true;
  // The transport's GET worker waits for a session id before
  // issuing the request (real servers want one); seed it for this
  // unit test, where there is no initialize flow to negotiate one.
  opts.session_id = "test-session";
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);

  std::promise<QString> got;
  auto fut = got.get_future();
  transport->on_message([&](QString s) {
    try
    {
      got.set_value(std::move(s));
    }
    catch(...)
    {
    }
  });
  transport->start();

  QCOMPARE(fut.wait_for(2s), std::future_status::ready);
  QCOMPARE(QString(fut.get()), QString::fromUtf8(R"({"server-said":"hi"})"));
  transport->close();
}

void HttpClientTransport::SendBeforeStartFails(void)
{
  mcp::HttpClientTransport::Options opts;
  opts.url = "http://127.0.0.1:1";
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);
  auto ec = transport->send("x");
  QVERIFY(ec);
  QVERIFY(ec == std::make_error_code(std::errc::not_connected));
}

void HttpClientTransport::IdempotentClose(void)
{
  mcp::HttpClientTransport::Options opts;
  opts.url = "http://127.0.0.1:1";
  opts.open_get_stream = false;
  auto transport = std::make_unique<mcp::HttpClientTransport>(opts);
  transport->start();
  transport->close();
  transport->close(); // must be a no-op
}

QTEST_GUILESS_MAIN(HttpClientTransport)
#include "http_client_transport_test.moc"
#include <QList>
#include <QString>
