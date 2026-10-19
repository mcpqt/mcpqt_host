// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Tests for PluginManager: loading the test plugins, slot → tool publication
// via meta-object reflection (including duplicate rejection), cross-plugin
// function forwarding through the host, unloading, and an end-to-end
// tools/call through Server + Client.

#include "in_memory_transport.hpp"
#include "qt_test_support.hpp"
#include "test_binary_paths.hpp"

#include "client.hpp"
#include "plugin_manager.hpp"
#include "server.hpp"

#include <QMetaObject>
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
#if !defined(MCP_TEST_PLUGIN_PAIR)
#error "MCP_TEST_PLUGIN_PAIR path was not generated; the pair plugin target is missing"
#endif
#if !defined(MCP_TEST_LOG_PLUGIN)
#error "MCP_TEST_LOG_PLUGIN path was not generated; the log plugin target is missing"
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
    void LoadRegistersSlotsAsTools();
    void MissingFileFails();
    void MissingDirectoryLoadsNothing();
    void UnloadClearsState();
    void DependencyGatesLoad();
    void EndToEndToolCalls();
    void CrossPluginForwarding();
    void UnloadHonorsReverseDependencies();
    void LoggingPluginClaimsAndCannotBeReplaced();
};

namespace {

// Invoke a plugin slot returning QVariant, passing one (empty) QVariant arg.
QVariant invoke_slot(QObject* obj, const char* name) {
    QVariant arg;
    QVariant result;
    const bool ok = QMetaObject::invokeMethod(
        obj, name, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, arg));
    return ok ? result : QVariant();
}

}  // namespace

void PluginManagerTest::LoadRegistersSlotsAsTools() {
    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QCOMPARE(plugins.plugin_ids(), QStringList{ "test-plugin" });

    std::vector<std::string> tools = plugins.registered_tools();
    std::sort(tools.begin(), tools.end());
    QCOMPARE(tools, (std::vector<std::string>{
                        "test-plugin.add", "test-plugin.boom",
                        "test-plugin.dup", "test-plugin.echo" }));

    QVERIFY(plugins.has_tool("test-plugin.echo"));
    QVERIFY(!plugins.has_tool("test-plugin.nope"));
    QVERIFY(!plugins.has_tool("echo"));
}

void PluginManagerTest::MissingFileFails() {
    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(!plugins.load_file(QStringLiteral("/no/such/plugin.so")));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.registered_tools().empty());
}

void PluginManagerTest::MissingDirectoryLoadsNothing() {
    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QCOMPARE(plugins.load_directory(QStringLiteral("/no/such/directory")), std::size_t{0});
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::UnloadClearsState() {
    mcp::Server        server{ host_impl() };
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

void PluginManagerTest::DependencyGatesLoad() {
    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    // test_plugin_pair declares a dependency on test-plugin, so loading it
    // first must fail (the dependency is not yet satisfiable)...
    QVERIFY(!plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});

    // ...and loading the dependency first allows the dependent to load.
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});
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
        if (tool.name == "test-plugin.add") has_add = true;
    }
    QVERIFY(has_add);

    const auto sum = client.call_tool("test-plugin.add",
                                      QJsonObject{ { "a", 2.0 }, { "b", 40.0 } }).get();
    QCOMPARE(sum.content.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(std::get<mcp::TextContent>(sum.content[0]).text),
             QString("42"));

    const auto echo = client.call_tool("test-plugin.echo",
                                       QJsonObject{ { "message", "hi" } }).get();
    QCOMPARE(QString::fromStdString(std::get<mcp::TextContent>(echo.content[0]).text),
             QString("hi"));

    const auto boom = client.call_tool("test-plugin.boom", QJsonObject{}).get();
    QVERIFY(boom.is_error.value_or(false));
    QVERIFY(std::get<mcp::TextContent>(boom.content[0]).text.find("boom") != std::string::npos);
}

void PluginManagerTest::CrossPluginForwarding() {
    auto pair   = mcp::test::make_in_memory_pair();
    auto server = std::make_shared<mcp::Server>(host_impl());

    mcp::PluginManager plugins{ *server };
    // Order matters: the pair plugin records test-plugin.echo at load time,
    // so test_plugin must load first.
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});
    QCOMPARE(plugins.plugin_ids(), (QStringList{ "test-plugin", "test-pair" }));
    QVERIFY(plugins.has_tool("test-pair.forward"));
    QVERIFY(plugins.has_tool("test-plugin.echo"));

    std::thread runner([server, t = std::move(pair.b)]() mutable {
        server->run(std::move(t));
    });

    mcp::Client client{ mcp::Implementation{ .name = "tester", .version = "0.1.0" } };
    auto shutdown = qScopeGuard([&] {
        client.disconnect();
        server->stop();
        if (runner.joinable()) runner.join();
    });

    client.connect(std::move(pair.a));
    (void)client.initialize().get();

    // Cross-plugin round-trip: this tool is owned by test-pair, which
    // internally calls test-plugin.echo (a different plugin) through the host.
    // The returned value must be test-plugin's echoed message, surfaced by
    // test-pair in its own result.
    const auto fwd = client.call_tool("test-pair.forward",
                                      QJsonObject{ { "message", "cross" } }).get();
    QVERIFY(!fwd.is_error.value_or(false));
    QCOMPARE(fwd.content.size(), std::size_t{1});
    QCOMPARE(QString::fromStdString(std::get<mcp::TextContent>(fwd.content[0]).text),
             QString("test-pair received: cross"));
}

void PluginManagerTest::UnloadHonorsReverseDependencies() {
    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});

    // test-pair declares a dependency on test-plugin, so test-plugin cannot be
    // unloaded while test-pair is loaded.
    QVERIFY(!plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});

    // Unloading the dependent first is fine; then the dependency can go.
    QVERIFY(plugins.unload_plugin("test-pair"));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QVERIFY(plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.registered_tools().empty());

    // Unknown ids are rejected.
    QVERIFY(!plugins.unload_plugin("does-not-exist"));
}

void PluginManagerTest::LoggingPluginClaimsAndCannotBeReplaced() {
    // Runs last (declaration order): it claims the process-wide sink.
    QVERIFY(!mcp::log_sink_claimed());

    mcp::Server        server{ host_impl() };
    mcp::PluginManager plugins{ server };

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_LOG_PLUGIN)));
    QVERIFY(mcp::log_sink_claimed());

    QObject* logger = plugins.plugin_object("test-logger");
    QVERIFY(logger != nullptr);
    QVERIFY(invoke_slot(logger, "claimed").toBool());

    const int before = invoke_slot(logger, "count").toInt();

    // A core log event must reach the plugin's sink (the plugin decides how to
    // handle it; here it counts/stashes it).
    MCP_LOG_WARN("logging-plugin-probe-line");
    const int after = invoke_slot(logger, "count").toInt();
    QVERIFY2(after >= before + 1, "logging plugin did not receive the log event");
    QVERIFY(invoke_slot(logger, "last").toString().contains("logging-plugin-probe-line"));

    // Once claimed, nobody can displace the logging system.
    QVERIFY(!mcp::set_log_sink([](mcp::LogLevel, std::string_view) {}));
    QVERIFY(!mcp::claim_log_sink([](mcp::LogLevel, std::string_view) {}));

    // The claiming plugin is pinned: unload_all() must not detach it.
    plugins.unload_all();
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QVERIFY(plugins.plugin_object("test-logger") != nullptr);
    QVERIFY(mcp::log_sink_claimed());
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PluginManagerTest suite;
    return QTest::qExec(&suite, argc, argv);
}

#include "plugin_test.moc"