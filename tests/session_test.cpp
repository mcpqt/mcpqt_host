// SPDX-License-Identifier: AGPL-3.0-only
//
// Tests for the Session dispatcher. Each test stands up a paired
// in-memory transport, drives one Session as the "client" and either
// the other Session or a hand-rolled handler as the "server", and
// asserts the wire-level effect.

#include "in_memory_transport.h"

#include "error.h"
#include "protocol.h"
#include "session.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {

using namespace std::chrono_literals;

// Helper that builds two Sessions back-to-back via in-memory pair.
struct Pair {
    std::unique_ptr<mcp::Session> a;
    std::unique_ptr<mcp::Session> b;
};

Pair make_session_pair(mcp::Session::Options opts = {}) {
    auto p = mcp::test::make_in_memory_pair();
    Pair sp{
        std::make_unique<mcp::Session>(std::move(p.a), opts),
        std::make_unique<mcp::Session>(std::move(p.b), opts),
    };
    sp.a->start();
    sp.b->start();
    return sp;
}

}  // namespace

// -------------------------------------------------------------------------

class Session : public QObject {
    Q_OBJECT
private slots:
    void RequestRoundTripThroughHandler(void);
    void NotificationDeliveredToHandler(void);
    void MissingHandlerYieldsMethodNotFound(void);
    void HandlerThrowingMcpErrorPropagates(void);
    void HandlerThrowingStdExceptionMapsToInternal(void);
    void FallbackHandlerCatchesUnknownMethods(void);
    void ManyConcurrentRequestsAllResolveCorrectly(void);
    void RequestTimeoutFiresErrorOnFuture(void);
    void ClosingCancelsPendingRequests(void);
    void IdsForDifferentRequestsAreDistinct(void);
    void IsOpenReflectsLifecycle(void);
    void DoubleCloseDoesNotLeaveJoinableThread(void);
    void OnClosedHookFiresWhenPeerCloses(void);
    void MalformedFrameIsDropped(void);
};

void Session::RequestRoundTripThroughHandler(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("echo",
                              [](const QJsonValue& params) -> QJsonValue {
                                  return QJsonObject{{"echoed", params}};
                              });

    auto fut = sp.a->send_request("echo", QJsonObject{{"hi", 1}}, 2s);
    auto result = fut.get();
    QCOMPARE(result["echoed"]["hi"], mcp::test::jv(1));

    sp.a->close();
    sp.b->close();
}

void Session::NotificationDeliveredToHandler(void) {
    auto sp = make_session_pair();

    std::promise<QJsonValue> got;
    auto fut = got.get_future();

    sp.b->set_notification_handler(
        "ping", [&](const QJsonValue& params) { got.set_value(params); });

    QVERIFY(!sp.a->send_notification("ping", QJsonObject{{"k", "v"}}));

    auto p = fut.get();
    QCOMPARE(p["k"], mcp::test::jv("v"));

    sp.a->close();
    sp.b->close();
}

void Session::MissingHandlerYieldsMethodNotFound(void) {
    auto sp = make_session_pair();
    auto fut = sp.a->send_request("nope", QJsonValue{}, 2s);
    QVERIFY_EXCEPTION_THROWN(
        ([&] {
            try {
                (void)fut.get();
            } catch (const mcp::Error& e) {
                QCOMPARE(e.code(), mcp::error_code::method_not_found);
                throw;
            }
        }()),
        mcp::Error);

    sp.a->close();
    sp.b->close();
}

void Session::HandlerThrowingMcpErrorPropagates(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("explode", [](const QJsonValue&) -> QJsonValue {
        throw mcp::Error{mcp::error_code::invalid_params, "bad",
                         QJsonObject{{"why", "missing"}}};
    });

    auto fut = sp.a->send_request("explode", QJsonValue{}, 2s);
    try {
        (void)fut.get();
        QFAIL("expected Error");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::invalid_params);
        QCOMPARE(e.message(), QString{"bad"});
        QCOMPARE(e.data()["why"], mcp::test::jv("missing"));
    }

    sp.a->close();
    sp.b->close();
}

void Session::HandlerThrowingStdExceptionMapsToInternal(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("explode", [](const QJsonValue&) -> QJsonValue {
        throw std::runtime_error{"boom"};
    });

    auto fut = sp.a->send_request("explode", QJsonValue{}, 2s);
    try {
        (void)fut.get();
        QFAIL("expected Error");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::internal_error);
    }

    sp.a->close();
    sp.b->close();
}

void Session::FallbackHandlerCatchesUnknownMethods(void) {
    auto sp = make_session_pair();
    sp.b->set_fallback_request_handler([](const QJsonValue&) -> QJsonValue {
        return QJsonObject{{"caught_by", "fallback"}};
    });

    auto fut = sp.a->send_request("anything", QJsonValue{}, 2s);
    auto r = fut.get();
    QCOMPARE(r["caught_by"], mcp::test::jv("fallback"));

    sp.a->close();
    sp.b->close();
}

void Session::ManyConcurrentRequestsAllResolveCorrectly(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("double", [](const QJsonValue& p) -> QJsonValue {
        return QJsonObject{{"r", mcp::from_json<int>(p["x"]) * 2}};
    });

    constexpr int N = 200;
    std::vector<std::future<QJsonValue>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(sp.a->send_request("double", QJsonObject{{"x", i}}, 5s));
    }
    for (int i = 0; i < N; ++i) {
        auto r = futs[i].get();
        QCOMPARE(r["r"], mcp::test::jv(i * 2));
    }

    sp.a->close();
    sp.b->close();
}

void Session::RequestTimeoutFiresErrorOnFuture(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("slow", [](const QJsonValue&) -> QJsonValue {
        std::this_thread::sleep_for(2s);
        return {};
    });

    auto fut = sp.a->send_request("slow", QJsonValue{}, 100ms);
    try {
        (void)fut.get();
        QFAIL("expected timeout error");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::internal_error);
        QVERIFY(e.message().contains("timed out"));
    }

    sp.a->close();
    sp.b->close();
}

void Session::ClosingCancelsPendingRequests(void) {
    auto sp = make_session_pair();
    sp.b->set_request_handler("hang", [](const QJsonValue&) -> QJsonValue {
        std::this_thread::sleep_for(300ms);
        return {};
    });

    auto fut = sp.a->send_request("hang", QJsonValue{}, 30s);
    sp.a->close();
    try {
        (void)fut.get();
        QFAIL("expected error after close");
    } catch (const mcp::Error&) {
        // expected: closing the session rejects pending requests
    }

    sp.b->close();
}

void Session::IdsForDifferentRequestsAreDistinct(void) {
    auto sp = make_session_pair();
    // Two requests are dispatched on detached workers, so the handler
    // can run on either thread first — the bookkeeping must be
    // thread-safe.
    std::mutex ids_mu;
    QList<QString> ids;
    sp.b->set_request_handler("tag", [&](const QJsonValue& p) -> QJsonValue {
        std::lock_guard<std::mutex> lk(ids_mu);
        ids.push_back(mcp::from_json<QString>(p["id"]));
        return {};
    });
    auto f1 = sp.a->send_request("tag", QJsonObject{{"id", "x"}}, 2s);
    auto f2 = sp.a->send_request("tag", QJsonObject{{"id", "y"}}, 2s);
    f1.get();
    f2.get();
    {
        std::lock_guard<std::mutex> lk(ids_mu);
        QCOMPARE(ids.size(), std::size_t{2});
    }

    sp.a->close();
    sp.b->close();
}

void Session::IsOpenReflectsLifecycle(void) {
    auto sp = make_session_pair();
    QVERIFY(sp.a->is_open());
    sp.a->close();
    QVERIFY(!sp.a->is_open());
    sp.b->close();
}

void Session::DoubleCloseDoesNotLeaveJoinableThread(void) {
    // Regression: close() short-circuiting once m_closed was true left the
    // timeout thread joinable, so ~Session called std::terminate from
    // ~thread. Both close() callers must run to completion.
    auto sp = make_session_pair();
    sp.a->close();
    sp.a->close();  // explicit double close
    // arriving here without terminate is the test
    sp.b->close();
}

void Session::OnClosedHookFiresWhenPeerCloses(void) {
    auto p = mcp::test::make_in_memory_pair();
    auto a = std::make_unique<mcp::Session>(std::move(p.a));
    auto b = std::make_unique<mcp::Session>(std::move(p.b));

    std::promise<void> got_close;
    a->set_on_closed([&] { got_close.set_value(); });
    a->start();
    b->start();

    b->close();
    QCOMPARE(got_close.get_future().wait_for(2s), std::future_status::ready);

    a->close();
}

void Session::MalformedFrameIsDropped(void) {
    auto p = mcp::test::make_in_memory_pair();
    auto a = std::make_unique<mcp::Session>(std::move(p.a));
    // Sneak the raw end onto the b-side so we can poke garbage into a's
    // dispatcher: we send a frame from b that isn't valid JSON.
    auto& b_transport = *p.b;
    b_transport.start();
    a->start();

    auto invalid = "{not json";
    QVERIFY(!b_transport.send(invalid));

    // Subsequent valid frames still get through.
    std::promise<QJsonValue> got;
    auto f = got.get_future();
    a->set_notification_handler(
        "hi", [&](const QJsonValue& params) { got.set_value(params); });
    QVERIFY(!b_transport.send(
        R"({"jsonrpc":"2.0","method":"hi","params":{"k":1}})"));
    QCOMPARE(f.get()["k"], mcp::test::jv(1));

    a->close();
    b_transport.close();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Session suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "session_test.moc"
#include <QList>
#include <QString>
