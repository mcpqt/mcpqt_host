// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Tests for the typed cross-plugin registry: loading a plugin, reflecting its
// public slots into functions, invoking them with positional QVariant
// arguments (void or QVariant results), and unloading.
//
// This first test covers the simple plugin (test_plugin). Additional plugins
// (pair, logger, ...) are covered by tests added alongside their updates.

#include "test_binary_paths.hpp"

#include "error.hpp"
#include "plugin_manager.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QStringList>
#include <QtTest/QtTest>

#include <string>

#if !defined(MCP_TEST_PLUGIN)
#error "MCP_TEST_PLUGIN path was not generated; the test plugin target is missing"
#endif
#if !defined(MCP_TEST_PLUGIN_PAIR)
#error "MCP_TEST_PLUGIN_PAIR path was not generated; the pair plugin target is missing"
#endif
#if !defined(MCP_TEST_LOG_PLUGIN)
#error "MCP_TEST_LOG_PLUGIN path was not generated; the log plugin target is missing"
#endif

class PluginManagerTest : public QObject {
    Q_OBJECT
private slots:
    void LoadRegistersTypedFunctions();
    void MissingFileFails();
    void MissingDirectoryLoadsNothing();
    void UnloadClearsState();
    void CallsTypedFunctions();
    void VoidAndThrowingFunctions();
    void InvalidCallsThrow();
    void DependencyGatesLoad();
    void CrossPluginForwarding();
    void UnloadHonorsReverseDependencies();
    void LoggingPluginClaimsAndCannotBeReplaced();
};

namespace {

// Invoke a no-argument plugin slot that returns QVariant.
QVariant invoke_slot(QObject* obj, const char* name) {
    QVariant result;
    const bool ok = QMetaObject::invokeMethod(
        obj, name, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result));
    return ok ? result : QVariant();
}

}  // namespace

void PluginManagerTest::LoadRegistersTypedFunctions() {
    mcp::PluginManager plugins;

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QCOMPARE(plugins.plugin_ids(), QStringList{ "test-plugin" });

    const QStringList funcs = plugins.functions();
    for (const char* expected : { "test-plugin.echo(QString)",
                                  "test-plugin.add(double,double)",
                                  "test-plugin.boom()",
                                  "test-plugin.reset()",
                                  "test-plugin.dup()" }) {
        QVERIFY2(funcs.contains(QString::fromLatin1(expected)), expected);
        QVERIFY(plugins.has_function(QString::fromLatin1(expected)));
    }
    // The old packed convention is gone.
    QVERIFY(!plugins.has_function("test-plugin.echo(QVariant)"));
}

void PluginManagerTest::MissingFileFails() {
    mcp::PluginManager plugins;
    QVERIFY(!plugins.load_file(QStringLiteral("/no/such/plugin.so")));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.functions().isEmpty());
}

void PluginManagerTest::MissingDirectoryLoadsNothing() {
    mcp::PluginManager plugins;
    QCOMPARE(plugins.load_directory(QStringLiteral("/no/such/directory")), std::size_t{0});
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::UnloadClearsState() {
    mcp::PluginManager plugins;

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QVERIFY(plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.functions().isEmpty());

    // Idempotent, and unknown ids are rejected.
    QVERIFY(!plugins.unload_plugin("test-plugin"));
    plugins.unload_all();
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::CallsTypedFunctions() {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // QVariant echo(QString)
    const QVariant echoed = plugins.call_function(
        QStringLiteral("test-plugin.echo(QString)"), { QStringLiteral("hi") });
    QCOMPARE(echoed.toString(), QStringLiteral("hi"));

    // QVariant add(double, double)
    const QVariant sum = plugins.call_function(
        QStringLiteral("test-plugin.add(double,double)"), { 2.0, 40.0 });
    QCOMPARE(sum.toDouble(), 42.0);

    // A convertible argument (int → QString) is accepted.
    const QVariant coerced = plugins.call_function(
        QStringLiteral("test-plugin.echo(QString)"), { 7 });
    QCOMPARE(coerced.toString(), QStringLiteral("7"));

    // QVariant dup()
    QCOMPARE(plugins.call_function(QStringLiteral("test-plugin.dup()"), {}).toString(),
             QStringLiteral("dup"));
}

void PluginManagerTest::VoidAndThrowingFunctions() {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // void reset() → invalid QVariant, no error.
    const QVariant result =
        plugins.call_function(QStringLiteral("test-plugin.reset()"), {});
    QVERIFY(!result.isValid());

    // void boom() throws; the host surfaces it as mcp::Error.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(QStringLiteral("test-plugin.boom()"), {}),
        mcp::Error);
}

void PluginManagerTest::InvalidCallsThrow() {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // Unknown function.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(QStringLiteral("test-plugin.nope()"), {}),
        mcp::Error);

    // Wrong arity.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(QStringLiteral("test-plugin.echo(QString)"), {}),
        mcp::Error);
}

void PluginManagerTest::DependencyGatesLoad() {
    mcp::PluginManager plugins;

    // test-pair declares a dependency on test-plugin, so loading it first must
    // fail (the dependency is not yet satisfiable)...
    QVERIFY(!plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});

    // ...and loading the dependency first allows the dependent to load.
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});
}

void PluginManagerTest::CrossPluginForwarding() {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));

    QCOMPARE(plugins.plugin_ids(), (QStringList{ "test-plugin", "test-pair" }));
    QVERIFY(plugins.has_function("test-pair.forward(QString)"));
    QVERIFY(plugins.has_function("test-plugin.echo(QString)"));

    // test-pair.forward calls test-plugin.echo through the host and surfaces
    // the other plugin's returned value in its own result.
    const QVariant forwarded = plugins.call_function(
        QStringLiteral("test-pair.forward(QString)"), { QStringLiteral("cross") });
    QCOMPARE(forwarded.toString(), QStringLiteral("test-pair received: cross"));
}

void PluginManagerTest::UnloadHonorsReverseDependencies() {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});

    // test-plugin cannot be unloaded while test-pair (which depends on it) is
    // loaded; unloading the dependent first is fine.
    QVERIFY(!plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{2});
    QVERIFY(plugins.unload_plugin("test-pair"));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QVERIFY(plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QVERIFY(plugins.functions().isEmpty());
}

void PluginManagerTest::LoggingPluginClaimsAndCannotBeReplaced() {
    // Runs last (declaration order): it claims the process-wide sink.
    QVERIFY(!mcp::log_sink_claimed());

    mcp::PluginManager plugins;
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