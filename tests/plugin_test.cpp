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
#include <QStringList>
#include <QtTest/QtTest>

#include <string>

#if !defined(MCP_TEST_PLUGIN)
#error "MCP_TEST_PLUGIN path was not generated; the test plugin target is missing"
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
};

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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PluginManagerTest suite;
    return QTest::qExec(&suite, argc, argv);
}

#include "plugin_test.moc"