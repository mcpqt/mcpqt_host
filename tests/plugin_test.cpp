// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Tests for PluginManager: loading the test plugin, command → tool
// registration (including duplicate de-duplication), unloading, and an
// end-to-end tools/call through Server + Client.

#include "in_memory_transport.hpp"
#include "qt_test_support.hpp"
#include "test_binary_paths.hpp"

#include "client.hpp"
#include "plugin_manager.hpp"
#include "server.hpp"

#include <QScopeGuard>
#include <QtTest/QtTest>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(MCP_TEST_PLUGIN)
#error "MCP_TEST_PLUGIN path was not generated; the test plugin target is missing"
#endif

namespace {

// Server is neither copyable nor movable, so hand a Server the Implementation
// (or construct it in place) rather than returning a Server by value.
mcp::Implementation host_impl() {
    return mcp::Implementation{ .name = "plugin-host", .version = "0.1.0" };
}

}  // namespace

class PluginManagerTest : public QObject {
    Q_OBJECT
private slots:
    void LoadRegistersUniqueTools();
    void MissingFileFails();
    void MissingDirectoryLoadsNothing();
    void UnloadClearsState();
    void EndToEndToolCalls();
};

void PluginManagerTest::LoadRegistersUniqueTools() {
    mcp::Server       server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QCOMPARE(plugins.plugin_ids(), QStringList{ "test-plugin" });

    std::vector<std::string> tools = plugins.registered_tools();
    std::sort(tools.begin(), tools.end());
    QCOMPARE(tools, (std::vector<std::string>{
                        "plugin_add", "plugin_boom", "plugin_dup", "plugin_echo" }));

    QVERIFY(plugins.has_tool("plugin_echo"));
    QVERIFY(!plugins.has_tool("does_not_exist"));
}

void PluginManagerTest::MissingFileFails() {
    mcp::Server       server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(!plugins.load_file(QStringLiteral("/no/such/plugin.so")));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.registered_tools().empty());
}

void PluginManagerTest::MissingDirectoryLoadsNothing() {
    mcp::Server       server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QCOMPARE(plugins.load_directory(QStringLiteral("/no/such/directory")), std::size_t{0});
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::UnloadClearsState() {
    mcp::Server       server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});

    plugins.unload_all();
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.registered_tools().empty());

    // Idempotent.
    plugins.unload_all();
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::EndToEndToolCalls() {
    auto pair   = mcp::test::make_in_memory_pair();
    auto server = std::make_shared<mcp::Server>(host_impl());

    mcp::PluginManager plugins{ *server };
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    std::thread runner([server, t = std::move(pair.b)]() mutable {
        server->run(std::move(t));
    });

    mcp::Client client{ mcp::Implementation{ .name = "tester", .version = "0.1.0" } };

    // Ensure the server thread is always stopped and joined, even when a
    // QVERIFY below returns early — otherwise std::thread's destructor aborts.
    auto shutdown = qScopeGuard([&] {
        client.disconnect();
        server->stop();
        if (runner.joinable()) runner.join();
    });

    client.connect(std::move(pair.a));
    (void)client.initialize().get();

    const auto list = client.list_tools().get();
    bool       has_add = false;
    for (const auto& tool : list.tools) {
        if (tool.name == "plugin_add") has_add = true;
    }
    QVERIFY(has_add);

    const auto sum = client.call_tool("plugin_add", QJsonObject{ { "a", 2.0 }, { "b", 40.0 } }).get();
    QCOMPARE(sum.content.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(std::get<mcp::TextContent>(sum.content[0]).text),
             QString("42"));

    const auto echo = client.call_tool("plugin_echo", QJsonObject{ { "message", "hi" } }).get();
    QCOMPARE(QString::fromStdString(std::get<mcp::TextContent>(echo.content[0]).text),
             QString("hi"));

    const auto boom = client.call_tool("plugin_boom", QJsonObject{}).get();
    QVERIFY(boom.is_error.value_or(false));
    QVERIFY(std::get<mcp::TextContent>(boom.content[0]).text.find("boom") != std::string::npos);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PluginManagerTest suite;
    return QTest::qExec(&suite, argc, argv);
}

#include "plugin_test.moc"
