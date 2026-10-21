// SPDX-License-Identifier: AGPL-3.0-only
//
// Tests for ping, cancellation, progress, and protocol-level logging
// (the "in-band utilities" added in Phase 2).

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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct UtilServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    UtilServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "util-test",
              .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread(
            [s = server, t = std::move(t)]() mutable { s->run(std::move(t)); });
    }
    ~UtilServerThread() {
        server->stop();
        if (thread.joinable())
            thread.join();
    }
};

}  // namespace

class Cancelled : public QObject {
    Q_OBJECT
private slots:
    void RoundTrip(void);
};

class Progress : public QObject {
    Q_OBJECT
private slots:
    void RoundTrip(void);
};

class Logging : public QObject {
    Q_OBJECT
private slots:
    void LevelStrings(void);
    void MessageNotificationRoundTrip(void);
};

class Ping : public QObject {
    Q_OBJECT
private slots:
    void ClientPingResolves(void);
};

class LoggingIntegration : public QObject {
    Q_OBJECT
private slots:
    void ServerLogReachesClient(void);
    void SetLevelFiltersServerEmissions(void);
};

class ProgressIntegration : public QObject {
    Q_OBJECT
private slots:
    void ServerProgressReachesClient(void);
};

class CancellationIntegration : public QObject {
    Q_OBJECT
private slots:
    void ClientCanSendCancelledNotification(void);
};

// -------------------------------------------------------------------------
// Round-trip tests
// -------------------------------------------------------------------------

void Cancelled::RoundTrip(void) {
    mcp::CancelledNotificationParams p{
        .request_id = mcp::RequestId{42},
        .reason = "user requested",
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["requestId"], mcp::test::jv(42));
    QCOMPARE(j["reason"], mcp::test::jv("user requested"));
    auto back = mcp::from_json<mcp::CancelledNotificationParams>(j);
    QVERIFY(!back.request_id.isNull());
    QCOMPARE(back.request_id.as_integer(), std::int64_t{42});
}

void Progress::RoundTrip(void) {
    mcp::ProgressNotificationParams p{
        .progress_token = mcp::ProgressToken{"tok"},
        .progress = 0.5,
        .total = 1.0,
        .message = "halfway",
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["progressToken"], mcp::test::jv("tok"));
    QCOMPARE(j["progress"], mcp::test::jv(0.5));
    auto back = mcp::from_json<mcp::ProgressNotificationParams>(j);
    QCOMPARE(back.progress, 0.5);
    QCOMPARE(back.total, 1.0);
}

void Logging::LevelStrings(void) {
    QCOMPARE(mcp::to_string(mcp::LoggingLevel::debug), "debug");
    QCOMPARE(mcp::to_string(mcp::LoggingLevel::emergency), "emergency");
    QJsonValue j = mcp::to_json(mcp::LoggingLevel::warning);
    QCOMPARE(j, mcp::test::jv("warning"));
    auto back = mcp::from_json<mcp::LoggingLevel>(j);
    QCOMPARE(back, mcp::LoggingLevel::warning);
}

void Logging::MessageNotificationRoundTrip(void) {
    mcp::LoggingMessageNotificationParams p{
        .level = mcp::LoggingLevel::info,
        .logger = "scheduler",
        .data = QJsonObject{{"event", "tick"}},
    };
    QJsonValue j = mcp::to_json(p);
    QCOMPARE(j["level"], mcp::test::jv("info"));
    QCOMPARE(j["logger"], mcp::test::jv("scheduler"));
    QCOMPARE(j["data"]["event"], mcp::test::jv("tick"));
    auto back = mcp::from_json<mcp::LoggingMessageNotificationParams>(j);
    QCOMPARE(back.level, mcp::LoggingLevel::info);
}

// -------------------------------------------------------------------------
// Ping
// -------------------------------------------------------------------------

void Ping::ClientPingResolves(void) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server&) {});

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    try {
        client.ping().get();
        QVERIFY(true);
    } catch (...) {
        QFAIL("unexpected exception");
    }

    client.disconnect();
}

// -------------------------------------------------------------------------
// Logging
// -------------------------------------------------------------------------

void LoggingIntegration::ServerLogReachesClient(void) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_logging(mcp::LoggingLevel::debug);
        s.tool("emit_log", QJsonObject{{"type", "object"}},
               [&s](const QJsonValue&) {
                   (void)s.log(mcp::LoggingLevel::warning,
                               QJsonObject{{"msg", "hi"}}, "logger-1");
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "ok"}},
                   };
               });
    });

    std::promise<mcp::LoggingMessageNotificationParams> got;
    auto fut = got.get_future();

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_log_message_handler(
        [&](const mcp::LoggingMessageNotificationParams& params) {
            try {
                got.set_value(params);
            } catch (...) {
            }
        });
    auto info = client.initialize().get();
    QVERIFY(!info.capabilities.logging.isNull());

    auto res = client.call_tool("emit_log").get();
    QVERIFY(!(res.is_error.value_or(false)));

    auto received = fut.get();
    QCOMPARE(received.level, mcp::LoggingLevel::warning);
    QCOMPARE(received.logger, QString{"logger-1"});
    QCOMPARE(received.data["msg"], mcp::test::jv("hi"));

    client.disconnect();
}

void LoggingIntegration::SetLevelFiltersServerEmissions(void) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_logging(mcp::LoggingLevel::debug);
        s.tool("emit_debug", QJsonObject{{"type", "object"}},
               [&s](const QJsonValue&) {
                   (void)s.log(mcp::LoggingLevel::debug, "should-be-filtered");
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "ok"}},
                   };
               });
    });

    std::mutex mu;
    int received_count = 0;

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_log_message_handler(
        [&](const mcp::LoggingMessageNotificationParams&) {
            std::lock_guard<std::mutex> lk(mu);
            ++received_count;
        });
    (void)client.initialize().get();

    // Raise the server's level above debug, so debug emissions drop.
    (void)client.set_log_level(mcp::LoggingLevel::error).get();
    (void)client.call_tool("emit_debug").get();
    std::this_thread::sleep_for(50ms);

    {
        std::lock_guard<std::mutex> lk(mu);
        QCOMPARE(received_count, 0);
    }

    client.disconnect();
}

// -------------------------------------------------------------------------
// Progress + cancellation (limited Phase 2 scope)
// -------------------------------------------------------------------------

void ProgressIntegration::ServerProgressReachesClient(void) {
    auto p = mcp::test::make_in_memory_pair();
    UtilServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.tool("loud_op", QJsonObject{{"type", "object"}},
               [&s](const QJsonValue&) {
                   s.report_progress(mcp::ProgressToken{"job-1"}, 0.25);
                   s.report_progress(mcp::ProgressToken{"job-1"}, 0.75, 1.0,
                                     "almost");
                   return mcp::CallToolResult{
                       .content = {mcp::TextContent{.text = "done"}},
                   };
               });
    });

    std::mutex mu;
    QList<double> progresses;

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_progress_handler([&](const mcp::ProgressNotificationParams& pn) {
        std::lock_guard<std::mutex> lk(mu);
        progresses.push_back(pn.progress);
    });
    (void)client.initialize().get();

    (void)client.call_tool("loud_op").get();
    std::this_thread::sleep_for(50ms);

    std::lock_guard<std::mutex> lk(mu);
    QVERIFY(progresses.size() >= 2u);
    QCOMPARE(progresses[0], 0.25);
    QCOMPARE(progresses[1], 0.75);

    client.disconnect();
}

void CancellationIntegration::ClientCanSendCancelledNotification(void) {
    auto p = mcp::test::make_in_memory_pair();
    std::promise<mcp::CancelledNotificationParams> got;
    auto fut = got.get_future();

    auto pair = mcp::test::make_in_memory_pair();
    auto client_session = std::make_unique<mcp::Session>(std::move(pair.a));
    auto server_session = std::make_unique<mcp::Session>(std::move(pair.b));
    server_session->set_notification_handler(
        QString{mcp::method_notifications_cancelled},
        [&](const QJsonValue& params) {
            try {
                got.set_value(
                    mcp::from_json<mcp::CancelledNotificationParams>(params));
            } catch (...) {
            }
        });
    client_session->start();
    server_session->start();

    QVERIFY(!(client_session->send_notification(
        QString{mcp::method_notifications_cancelled},
        mcp::to_json(mcp::CancelledNotificationParams{
            .request_id = mcp::RequestId{7},
            .reason = "stopped",
        }))));

    auto received = fut.get();
    QVERIFY(!received.request_id.isNull());
    QCOMPARE(received.request_id.as_integer(), std::int64_t{7});

    client_session->close();
    server_session->close();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Cancelled suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        Progress suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        Logging suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        Ping suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        LoggingIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        ProgressIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        CancellationIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "utilities_test.moc"
#include <QList>
#include <QString>
