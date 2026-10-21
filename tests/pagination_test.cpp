// SPDX-License-Identifier: AGPL-3.0-only
//
// Pagination wiring tests. The server caps responses at `page_size`,
// returning a `nextCursor` so the client can fetch the remainder.

#include "in_memory_transport.h"

#include "client.h"
#include "protocol.h"
#include "server.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <memory>
#include <set>
#include <string>
#include <thread>

namespace {

struct ServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "pagi",
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

class Pagination : public QObject {
    Q_OBJECT
private slots:
    void ToolsListReturnsPagesWithCursor(void);
    void NoPaginationByDefault(void);
    void ResourcesPaginate(void);
    void MalformedCursorRejected(void);
};

void Pagination::ToolsListReturnsPagesWithCursor(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(3);
        for (int i = 0; i < 7; ++i) {
            s.tool("t" + QString::number(i), QJsonObject{{"type", "object"}},
                   [](const QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        }
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    std::set<QString> seen;
    QString cursor;
    int pages = 0;
    do {
        auto res = client.list_tools(cursor).get();
        QVERIFY(res.tools.size() <= 3u);
        for (const auto& t : res.tools)
            seen.insert(t.name);
        cursor = res.next_cursor;
        ++pages;
        QVERIFY(pages < 10);
    } while (!cursor.isNull());

    QVERIFY(pages >= 3);
    QCOMPARE(seen.size(), std::size_t{7});
    client.disconnect();
}

void Pagination::NoPaginationByDefault(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        for (int i = 0; i < 5; ++i) {
            s.tool("t" + QString::number(i), QJsonObject{{"type", "object"}},
                   [](const QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        }
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto res = client.list_tools().get();
    QCOMPARE(res.tools.size(), std::size_t{5});
    QVERIFY(res.next_cursor.isNull());
    client.disconnect();
}

void Pagination::ResourcesPaginate(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(2);
        for (int i = 0; i < 5; ++i) {
            s.resource(
                mcp::Resource{
                    .uri = "file:///x" + QString::number(i),
                    .name = "n" + QString::number(i),
                },
                [](const QString& uri) {
                    return mcp::ReadResourceResult{
                        .contents = {mcp::TextResourceContents{.uri = uri,
                                                               .text = "y"}},
                    };
                });
        }
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    int total = 0;
    QString cursor;
    int pages = 0;
    do {
        auto res = client.list_resources(cursor).get();
        total += static_cast<int>(res.resources.size());
        cursor = res.next_cursor;
        ++pages;
        QVERIFY(pages < 10);
    } while (!cursor.isNull());

    QCOMPARE(total, 5);
    client.disconnect();
}

void Pagination::MalformedCursorRejected(void) {
    auto p = mcp::test::make_in_memory_pair();
    ServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.set_page_size(2);
        for (int i = 0; i < 5; ++i) {
            s.tool("t" + QString::number(i), QJsonObject{{"type", "object"}},
                   [](const QJsonValue&) -> mcp::CallToolResult {
                       return {.content = {mcp::TextContent{.text = "ok"}}};
                   });
        }
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.list_tools(QString{"not-a-number"});
    try {
        (void)fut.get();
        QFAIL("expected Error for bad cursor");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::invalid_params);
    }
    client.disconnect();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Pagination suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "pagination_test.moc"
#include <QString>
