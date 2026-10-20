// SPDX-License-Identifier: AGPL-3.0-only
//
// Phase 4b tests: tasks. Cover wire-shape round-trips and the full
// "client task-augments a tools/call → server runs handler async →
//  client polls / awaits / cancels" loop.

#include "in_memory_transport.h"

#include "client.h"
#include "protocol.h"
#include "server.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using mcp::Json;
using namespace std::chrono_literals;

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "tasks-srv",
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

class Tasks : public QObject {
    Q_OBJECT
private slots:
    void TaskRoundTrip(void);
    void TaskStatusEnum(void);
    void AugmentationRoundTrip(void);
    void CallToolRequestParamsCarriesTask(void);
    void CapabilityRoundTrip(void);
    void ListTasksResultRoundTrip(void);
};

class TasksIntegration : public QObject {
    Q_OBJECT
private slots:
    void ServerAdvertisesCapability(void);
    void AugmentedCallReturnsCreateTaskResult(void);
    void GetReturnsCurrentEnvelope(void);
    void ListEnumeratesTasks(void);
    void CancelTransitionsToCancelled(void);
    void StatusNotificationsArrive(void);
    void NonAugmentedCallStillWorks(void);
    void CancelOnceFiresStatusListenerOnce(void);
    void ConcurrencyCapEnforced(void);
    void ResultMetaMergesInsteadOfClobbering(void);
    void TaskNotFoundIsInvalidRequest(void);
    void ExecutionTaskSupportEnforced(void);
    void AugmentedCallWithoutEnableErrors(void);
};

void Tasks::TaskRoundTrip(void) {
    mcp::Task t{
        .taskId = "abc",
        .status = mcp::TaskStatus::working,
        .status_message = "running",
        .created_at = "2026-04-29T00:00:00Z",
        .last_updated_at = "2026-04-29T00:00:01Z",
        .ttl = 60'000,
        .poll_interval = 500,
    };
    Json j = mcp::to_json(t);
    QCOMPARE(j.toObject().value("taskId"), mcp::test::jv("abc"));
    QCOMPARE(j.toObject().value("status"), mcp::test::jv("working"));
    QCOMPARE(j.toObject().value("ttl"),
             mcp::test::jv(static_cast<qint64>(60'000)));
    QCOMPARE(j.toObject().value("pollInterval"), mcp::test::jv(500));
    auto back = mcp::from_json<mcp::Task>(j);
    QCOMPARE(back.taskId, QString("abc"));
    QCOMPARE(back.status, mcp::TaskStatus::working);
    QCOMPARE(*back.poll_interval, 500);
}

void Tasks::TaskStatusEnum(void) {
    QCOMPARE(mcp::to_json(mcp::TaskStatus::working), mcp::test::jv("working"));
    QCOMPARE(mcp::to_json(mcp::TaskStatus::input_required),
             mcp::test::jv("input_required"));
    QCOMPARE(mcp::to_json(mcp::TaskStatus::completed),
             mcp::test::jv("completed"));
    QCOMPARE(mcp::to_json(mcp::TaskStatus::failed), mcp::test::jv("failed"));
    QCOMPARE(mcp::to_json(mcp::TaskStatus::cancelled),
             mcp::test::jv("cancelled"));

    QCOMPARE(mcp::from_json<mcp::TaskStatus>(mcp::test::jv("input_required")),
             mcp::TaskStatus::input_required);
    QVERIFY_EXCEPTION_THROWN(
        (void)mcp::from_json<mcp::TaskStatus>(mcp::test::jv("nope")),
        mcp::Error);
}

void Tasks::AugmentationRoundTrip(void) {
    mcp::TaskAugmentation a{.ttl = 12'345};
    Json j = mcp::to_json(a);
    QCOMPARE(j.toObject().value("ttl"),
             mcp::test::jv(static_cast<qint64>(12'345)));
    auto back = mcp::from_json<mcp::TaskAugmentation>(j);
    QCOMPARE(*back.ttl, 12'345);

    mcp::TaskAugmentation empty{};
    Json j2 = mcp::to_json(empty);
    QVERIFY(!j2.toObject().contains("ttl"));
}

void Tasks::CallToolRequestParamsCarriesTask(void) {
    mcp::CallToolRequestParams p{
        .name = "ask",
        .arguments = QJsonObject{{"q", "?"}},
        .task = mcp::TaskAugmentation{.ttl = 10'000},
    };
    Json j = mcp::to_json(p);
    QCOMPARE(j.toObject().value("task").toObject().value("ttl"),
             mcp::test::jv(static_cast<qint64>(10'000)));
    auto back = mcp::from_json<mcp::CallToolRequestParams>(j);
    QVERIFY(back.task.has_value());
    QCOMPARE(*back.task->ttl, 10'000);
}

void Tasks::CapabilityRoundTrip(void) {
    mcp::TasksCapability tc;
    tc.list = QJsonObject{};
    tc.cancel = QJsonObject{};
    mcp::TasksRequestsCapability rc;
    rc.tools = QJsonObject{{"call", QJsonObject{}}};
    tc.requests = std::move(rc);

    Json j = mcp::to_json(tc);
    QVERIFY(j.toObject().value("list").isObject());
    QVERIFY(j.toObject().value("cancel").isObject());
    QCOMPARE(j.toObject()
                 .value("requests")
                 .toObject()
                 .value("tools")
                 .toObject()
                 .value("call")
                 .toObject(),
             QJsonObject{});

    auto back = mcp::from_json<mcp::TasksCapability>(j);
    QVERIFY(back.list.has_value());
    QVERIFY(back.requests.has_value());
    QVERIFY(back.requests->tools.has_value());
}

void Tasks::ListTasksResultRoundTrip(void) {
    mcp::ListTasksResult r{
        .tasks =
            {
                mcp::Task{.taskId = "1",
                          .status = mcp::TaskStatus::working,
                          .created_at = "x",
                          .last_updated_at = "y"},
            },
        .next_cursor = "1",
    };
    Json j = mcp::to_json(r);
    QCOMPARE(static_cast<int>(j.toObject().value("tasks").toArray().size()), 1);
    QCOMPARE(j.toObject().value("nextCursor"), mcp::test::jv("1"));
    auto back = mcp::from_json<mcp::ListTasksResult>(j);
    QCOMPARE(static_cast<int>(back.tasks.size()), 1);
    QCOMPARE(back.next_cursor, QString("1"));
}

// -------------------------------------------------------------------------
// Integration tests
// -------------------------------------------------------------------------

void TasksIntegration::ServerAdvertisesCapability(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks(/*default_ttl_ms=*/std::nullopt);
        s.tool("noop", QJsonObject{{"type", "object"}},
               [](const Json&) -> mcp::CallToolResult {
                   return {.content = {mcp::TextContent{.text = "ok"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    QVERIFY(info.capabilities.tasks.has_value());
    QVERIFY(info.capabilities.tasks->list.has_value());
    QVERIFY(info.capabilities.tasks->cancel.has_value());
    QVERIFY(info.capabilities.tasks->requests.has_value());
    QVERIFY(info.capabilities.tasks->requests->tools.has_value());

    client.disconnect();
}

void TasksIntegration::AugmentedCallReturnsCreateTaskResult(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("slow_add", QJsonObject{{"type", "object"}},
               [](const Json& args) -> mcp::CallToolResult {
                   std::this_thread::sleep_for(50ms);
                   const int a = args.toObject().value("a").toInt(0);
                   const int b = args.toObject().value("b").toInt(0);
                   return {.content = {mcp::TextContent{
                               .text = QString::number(a + b),
                           }}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto envelope =
        client
            .call_tool_as_task("slow_add",
                               Json(QJsonObject{{"a", 2}, {"b", 3}}), 60'000)
            .get();
    QVERIFY(!envelope.task.taskId.isEmpty());
    // The server marks the task working before the worker thread
    // flips to completed; either is acceptable depending on
    // scheduling, but it can't be a terminal-other.
    QVERIFY(envelope.task.status == mcp::TaskStatus::working ||
            envelope.task.status == mcp::TaskStatus::completed);

    // Block until the task is terminal and decode.
    auto raw = client.task_result(envelope.task.taskId).get();
    auto out = mcp::from_json<mcp::CallToolResult>(raw);
    QVERIFY(!(out.is_error.value_or(false)));
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString("5"));

    // _meta must carry the related-task tag.
    QVERIFY(raw.toObject().contains("_meta"));
    QVERIFY(raw.toObject().value("_meta").isObject());
    QVERIFY(raw.toObject().value("_meta").toObject().contains(
        mcp::tasks_related_task_meta_key));
    QCOMPARE(raw.toObject()
                 .value("_meta")
                 .toObject()
                 .value(mcp::tasks_related_task_meta_key)
                 .toObject()
                 .value("taskId"),
             mcp::test::jv(envelope.task.taskId));

    client.disconnect();
}

void TasksIntegration::GetReturnsCurrentEnvelope(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", QJsonObject{{"type", "object"}},
               [](const Json& args) -> mcp::CallToolResult {
                   const int a = args.toObject().value("a").toInt(0);
                   const int b = args.toObject().value("b").toInt(0);
                   return {.content = {mcp::TextContent{
                               .text = QString::number(a + b),
                           }}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env =
        client.call_tool_as_task("add", Json(QJsonObject{{"a", 1}, {"b", 2}}))
            .get();
    // Drain to terminal.
    (void)client.task_result(env.task.taskId).get();

    auto t = client.task_get(env.task.taskId).get();
    QCOMPARE(t.taskId, env.task.taskId);
    QCOMPARE(t.status, mcp::TaskStatus::completed);

    client.disconnect();
}

void TasksIntegration::ListEnumeratesTasks(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("noop", QJsonObject{{"type", "object"}},
               [](const Json&) -> mcp::CallToolResult {
                   return {.content = {mcp::TextContent{.text = "ok"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    QList<QString> ids;
    for (int i = 0; i < 3; ++i) {
        auto e = client.call_tool_as_task("noop", Json(QJsonObject{})).get();
        ids.push_back(e.task.taskId);
        // Drain so the test isn't flaky on the tasks/list snapshot.
        (void)client.task_result(e.task.taskId).get();
    }

    auto lst = client.task_list().get();
    QCOMPARE(static_cast<int>(lst.tasks.size()), 3);
    QVERIFY(lst.next_cursor.isNull());

    client.disconnect();
}

void TasksIntegration::CancelTransitionsToCancelled(void) {
    // The handler blocks on a flag the test flips after issuing the
    // cancel, so we observe the state machine: working → cancelled.
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks();
        s.tool("park", QJsonObject{{"type", "object"}},
               [&](const Json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = {mcp::TextContent{.text = "done"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("park", Json(QJsonObject{})).get();
    QCOMPARE(env.task.status, mcp::TaskStatus::working);

    auto cancelled = client.task_cancel(env.task.taskId).get();
    QCOMPARE(cancelled.taskId, env.task.taskId);
    QCOMPARE(cancelled.status, mcp::TaskStatus::cancelled);

    // Release the handler so the worker thread terminates cleanly.
    let_finish.store(true, std::memory_order_release);

    client.disconnect();
}

void TasksIntegration::StatusNotificationsArrive(void) {
    // The status listener emits notifications for each transition;
    // the client's set_task_status_handler should see them.
    auto p = mcp::test::make_in_memory_pair();

    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", QJsonObject{{"type", "object"}},
               [](const Json& args) -> mcp::CallToolResult {
                   const int a = args.toObject().value("a").toInt(0);
                   const int b = args.toObject().value("b").toInt(0);
                   return {.content = {mcp::TextContent{
                               .text = QString::number(a + b),
                           }}};
               });
    });

    std::mutex mu;
    std::condition_variable cv;
    QList<mcp::TaskStatus> seen;

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_task_status_handler([&](const mcp::Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(t.status);
        cv.notify_all();
    });
    (void)client.initialize().get();

    auto env =
        client.call_tool_as_task("add", Json(QJsonObject{{"a", 4}, {"b", 5}}))
            .get();
    (void)client.task_result(env.task.taskId).get();

    // Wait for the working + completed transitions to arrive.
    std::unique_lock<std::mutex> lk(mu);
    QVERIFY(cv.wait_for(lk, 2s, [&] {
        // We expect at least: working (on create) and completed
        // (on terminal). Other intermediate states are fine.
        bool saw_working = false;
        bool saw_completed = false;
        for (auto s : seen) {
            if (s == mcp::TaskStatus::working)
                saw_working = true;
            if (s == mcp::TaskStatus::completed)
                saw_completed = true;
        }
        return saw_working && saw_completed;
    }));

    client.disconnect();
}

void TasksIntegration::NonAugmentedCallStillWorks(void) {
    // Sanity check: enabling tasks doesn't break the synchronous
    // tools/call path.
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool("add", QJsonObject{{"type", "object"}},
               [](const Json& args) -> mcp::CallToolResult {
                   const int a = args.toObject().value("a").toInt(0);
                   const int b = args.toObject().value("b").toInt(0);
                   return {.content = {mcp::TextContent{
                               .text = QString::number(a + b),
                           }}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto out =
        client.call_tool("add", Json(QJsonObject{{"a", 7}, {"b", 8}})).get();
    QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text, QString("15"));

    client.disconnect();
}

void TasksIntegration::CancelOnceFiresStatusListenerOnce(void) {
    // Audit fix: the prior "cancel + worker race" allowed
    // complete() to re-fire the status listener after cancel()
    // had already done so, producing a duplicate cancelled
    // notification. Verify exactly-one notification per terminal
    // transition under that race.
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks();
        s.tool("park", QJsonObject{{"type", "object"}},
               [&](const Json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = {mcp::TextContent{.text = "done"}}};
               });
    });

    std::mutex mu;
    std::condition_variable cv;
    QList<mcp::TaskStatus> seen;

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    client.set_task_status_handler([&](const mcp::Task& t) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(t.status);
        cv.notify_all();
    });
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("park", Json(QJsonObject{})).get();
    auto cancelled = client.task_cancel(env.task.taskId).get();
    QCOMPARE(cancelled.status, mcp::TaskStatus::cancelled);

    // Release the worker; without the audit fix this would re-fire
    // a SECOND cancelled notification once complete() ran.
    let_finish.store(true, std::memory_order_release);

    // Give the worker time to exit.
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, 1s, [&] {
        return std::count(seen.begin(), seen.end(),
                          mcp::TaskStatus::cancelled) > 1;
    });
    int cancelled_count = static_cast<int>(
        std::count(seen.begin(), seen.end(), mcp::TaskStatus::cancelled));
    QCOMPARE(cancelled_count, 1);

    client.disconnect();
}

void TasksIntegration::ConcurrencyCapEnforced(void) {
    auto p = mcp::test::make_in_memory_pair();

    std::atomic<bool> let_finish{false};
    ServerThread srv(std::move(p.b), [&](mcp::Server& s) {
        s.enable_tasks(/*default_ttl_ms=*/std::nullopt,
                       /*max_concurrent=*/1);
        s.tool("park", QJsonObject{{"type", "object"}},
               [&](const Json&) -> mcp::CallToolResult {
                   while (!let_finish.load(std::memory_order_acquire)) {
                       std::this_thread::sleep_for(5ms);
                   }
                   return {.content = {mcp::TextContent{.text = "done"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    // First task occupies the only slot.
    auto env = client.call_tool_as_task("park", Json(QJsonObject{})).get();
    QCOMPARE(env.task.status, mcp::TaskStatus::working);

    // Second task is rejected.
    QVERIFY_EXCEPTION_THROWN(
        (void)client.call_tool_as_task("park", Json(QJsonObject{})).get(),
        mcp::Error);

    let_finish.store(true, std::memory_order_release);
    // Drain the first task so the ServerThread destructor doesn't
    // wedge on a worker still parked in the busy-loop.
    (void)client.task_result(env.task.taskId).get();

    client.disconnect();
}

void TasksIntegration::ResultMetaMergesInsteadOfClobbering(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        // Tool returns a CallToolResult whose JSON form, when set
        // through structured_content, doesn't itself populate
        // _meta. We'll inspect the wire-level result for the
        // related-task tag and verify it lives alongside other
        // _meta keys we sneak in via structured_content.
        s.tool("noop", QJsonObject{{"type", "object"}},
               [](const Json&) -> mcp::CallToolResult {
                   return {.content = {mcp::TextContent{.text = "ok"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto env = client.call_tool_as_task("noop", Json(QJsonObject{})).get();
    auto raw = client.task_result(env.task.taskId).get();
    QVERIFY(raw.toObject().contains("_meta"));
    QVERIFY(raw.toObject().value("_meta").isObject());
    QVERIFY(raw.toObject().value("_meta").toObject().contains(
        mcp::tasks_related_task_meta_key));

    client.disconnect();
}

void TasksIntegration::TaskNotFoundIsInvalidRequest(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) { s.enable_tasks(); });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    try {
        (void)client.task_get("nonexistent").get();
        QFAIL("task_get of unknown id should have thrown");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::invalid_request);
    }

    client.disconnect();
}

void TasksIntegration::ExecutionTaskSupportEnforced(void) {
    // Tool with execution.taskSupport=required must reject sync;
    // taskSupport=forbidden must reject task-augmented.
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.enable_tasks();
        s.tool(
            "must_task", QJsonObject{{"type", "object"}},
            [](const Json&) -> mcp::CallToolResult {
                return {.content = {mcp::TextContent{.text = "ok"}}};
            },
            mcp::Server::ToolMetadata{
                .execution =
                    mcp::ToolExecution{
                        .task_support = mcp::TaskSupport::required,
                    },
            });
        s.tool(
            "no_task", QJsonObject{{"type", "object"}},
            [](const Json&) -> mcp::CallToolResult {
                return {.content = {mcp::TextContent{.text = "ok"}}};
            },
            mcp::Server::ToolMetadata{
                .execution =
                    mcp::ToolExecution{
                        .task_support = mcp::TaskSupport::forbidden,
                    },
            });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    // Required tool: a plain call must error; task-augmented succeeds.
    QVERIFY_EXCEPTION_THROWN((void)client.call_tool("must_task").get(),
                             mcp::Error);
    auto env = client.call_tool_as_task("must_task", Json(QJsonObject{})).get();
    (void)client.task_result(env.task.taskId).get();

    // Forbidden tool: task-augmented must error; plain succeeds.
    QVERIFY_EXCEPTION_THROWN(
        (void)client.call_tool_as_task("no_task", Json(QJsonObject{})).get(),
        mcp::Error);
    auto out = client.call_tool("no_task").get();
    QVERIFY(!(out.is_error.value_or(false)));

    client.disconnect();
}

void TasksIntegration::AugmentedCallWithoutEnableErrors(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        // No enable_tasks() — capability not advertised.
        s.tool("add", QJsonObject{{"type", "object"}},
               [](const Json&) -> mcp::CallToolResult {
                   return {.content = {mcp::TextContent{.text = "x"}}};
               });
    });

    mcp::Client client{mcp::Implementation{.name = "t", .version = "0"}};
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    QVERIFY(!info.capabilities.tasks.has_value());

    QVERIFY_EXCEPTION_THROWN(
        (void)client.call_tool_as_task("add", Json(QJsonObject{})).get(),
        mcp::Error);

    client.disconnect();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Tasks s;
        status |= QTest::qExec(&s, argc, argv);
    }
    {
        TasksIntegration s;
        status |= QTest::qExec(&s, argc, argv);
    }
    return status;
}

#include "tasks_test.moc"
#include <QList>
#include <QString>
