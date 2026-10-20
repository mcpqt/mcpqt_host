// SPDX-License-Identifier: AGPL-3.0-only
//
// Tests for the typed cross-plugin registry: loading a plugin, reflecting its
// public slots into functions, invoking them with positional QVariant
// arguments (void or QVariant results), and unloading.
//
// This first test covers the simple plugin (test_plugin). Additional plugins
// (pair, logger, ...) are covered by tests added alongside their updates.

#include "test_binary_paths.h"

#include "error.h"
#include "plugin_manager.h"

#include <QCoreApplication>
#include <QMetaType>
#include <QStringList>
#include <QtTest/QtTest>

#include <string>

#if !defined(MCP_TEST_PLUGIN)
#error                                                                         \
    "MCP_TEST_PLUGIN path was not generated; the test plugin target is missing"
#endif
#if !defined(MCP_TEST_PLUGIN_PAIR)
#error                                                                         \
    "MCP_TEST_PLUGIN_PAIR path was not generated; the pair plugin target is missing"
#endif
#if !defined(MCP_TEST_LOG_PLUGIN)
#error                                                                         \
    "MCP_TEST_LOG_PLUGIN path was not generated; the log plugin target is missing"
#endif

class PluginManagerTest : public QObject {
    Q_OBJECT
private slots:
    void LoadRegistersTypedFunctions(void);
    void ToolsMetadataExposed(void);
    void MissingFileFails(void);
    void MissingDirectoryLoadsNothing(void);
    void UnloadClearsState(void);
    void CallsTypedFunctions(void);
    void VoidAndThrowingFunctions(void);
    void InvalidCallsThrow(void);
    void DependencyGatesLoad(void);
    void CrossPluginForwarding(void);
    void UnloadHonorsReverseDependencies(void);
    void LoggingPluginClaimsAndCannotBeReplaced(void);
};

void PluginManagerTest::LoadRegistersTypedFunctions(void) {
    mcp::PluginManager plugins;

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QCOMPARE(plugins.plugin_ids(), QStringList{"test-plugin"});

    const QStringList funcs = plugins.functions();
    for (const char* expected :
         {"QString TestPlugin::echo(QString)",
          "double TestPlugin::add(double,double)", "void TestPlugin::boom()",
          "void TestPlugin::reset()", "QString TestPlugin::dup()",
          "int TestPlugin::meaning()", "QString TestPlugin::upper(QString)"}) {
        QVERIFY2(funcs.contains(QString::fromLatin1(expected)), expected);
        QVERIFY(plugins.has_function(QString::fromLatin1(expected)));
    }
    // The old packed convention is gone.
    QVERIFY(!plugins.has_function("QVariant TestPlugin::echo(QVariant)"));
    // Inherited QObject slots (deleteLater is a public void() slot) are not
    // published as plugin functions.
    QVERIFY(!plugins.has_function("void QObject::deleteLater()"));
    QCOMPARE(funcs.size(), 9);
    // The host's own services are part of the function list.
    QVERIFY(plugins.has_function(
        QStringLiteral("bool mcp::Host::claimLogging(QString)")));
    QVERIFY(plugins.has_function(
        QStringLiteral("QVariant mcp::Host::elicit(QVariant)")));
}

void PluginManagerTest::ToolsMetadataExposed(void) {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // The plugin's metadata declares Tools keyed by function signature. Only
    // the entry naming a published function is retained; the bogus one is
    // dropped at load time.
    const std::vector<mcp::PluginTool> tools = plugins.tools();
    QCOMPARE(tools.size(), std::size_t{1});
    QCOMPARE(tools[0].signature,
             QStringLiteral("QString TestPlugin::echo(QString)"));
    QCOMPARE(QString::fromStdString(tools[0].tool.name),
             QStringLiteral("echo"));
    QCOMPARE(QString::fromStdString(tools[0].tool.description.value_or("")),
             QStringLiteral("Echo back the message."));
    const QJsonObject schema = tools[0].tool.input_schema.toObject();
    QCOMPARE(schema.value(QStringLiteral("type")).toString(),
             QStringLiteral("object"));
    QCOMPARE(schema.value(QStringLiteral("properties"))
                 .toObject()
                 .value(QStringLiteral("message"))
                 .toObject()
                 .value(QStringLiteral("type"))
                 .toString(),
             QStringLiteral("string"));

    const std::optional<mcp::Tool> tool =
        plugins.tool(QStringLiteral("QString TestPlugin::echo(QString)"));
    QVERIFY(tool.has_value());
    QCOMPARE(QString::fromStdString(tool->name), QStringLiteral("echo"));

    QVERIFY(!plugins.tool(QStringLiteral("void TestPlugin::does_not_exist()"))
                 .has_value());

    // Unloading removes the plugin's declared tools.
    plugins.unload_all();
    QVERIFY(plugins.tools().empty());
}

void PluginManagerTest::MissingFileFails(void) {
    mcp::PluginManager plugins;
    QVERIFY(!plugins.load_file(QStringLiteral("/no/such/plugin.so")));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QCOMPARE(plugins.functions().size(), 2);  // host services only
}

void PluginManagerTest::MissingDirectoryLoadsNothing(void) {
    mcp::PluginManager plugins;
    QCOMPARE(plugins.load_directory(QStringLiteral("/no/such/directory")),
             std::size_t{0});
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::UnloadClearsState(void) {
    mcp::PluginManager plugins;

    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QCOMPARE(plugins.plugin_count(), std::size_t{1});
    QVERIFY(plugins.unload_plugin("test-plugin"));
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
    QCOMPARE(plugins.functions().size(), 2);  // host services only

    // Idempotent, and unknown ids are rejected.
    QVERIFY(!plugins.unload_plugin("test-plugin"));
    plugins.unload_all();
    QCOMPARE(plugins.plugin_count(), std::size_t{0});
}

void PluginManagerTest::CallsTypedFunctions(void) {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // QString echo(QString) — natural return type, not QVariant.
    const QVariant echoed = plugins.call_function(
        QStringLiteral("QString TestPlugin::echo(QString)"),
        {QStringLiteral("hi")});
    QCOMPARE(echoed.metaType(), QMetaType::fromType<QString>());
    QCOMPARE(echoed.toString(), QStringLiteral("hi"));

    // double add(double, double) — natural return type.
    const QVariant sum = plugins.call_function(
        QStringLiteral("double TestPlugin::add(double,double)"), {2.0, 40.0});
    QCOMPARE(sum.metaType(), QMetaType::fromType<double>());
    QCOMPARE(sum.toDouble(), 42.0);

    // A convertible argument (int → QString) is accepted.
    const QVariant coerced = plugins.call_function(
        QStringLiteral("QString TestPlugin::echo(QString)"), {7});
    QCOMPARE(coerced.toString(), QStringLiteral("7"));

    // QString dup()
    const QVariant dup =
        plugins.call_function(QStringLiteral("QString TestPlugin::dup()"), {});
    QCOMPARE(dup.metaType(), QMetaType::fromType<QString>());
    QCOMPARE(dup.toString(), QStringLiteral("dup"));

    // Non-QVariant return types come back as the matching QVariant type.
    const QVariant meaning =
        plugins.call_function(QStringLiteral("int TestPlugin::meaning()"), {});
    QCOMPARE(meaning.metaType(), QMetaType::fromType<int>());
    QCOMPARE(meaning.toInt(), 42);

    const QVariant upper = plugins.call_function(
        QStringLiteral("QString TestPlugin::upper(QString)"),
        {QStringLiteral("abc")});
    QCOMPARE(upper.metaType(), QMetaType::fromType<QString>());
    QCOMPARE(upper.toString(), QStringLiteral("ABC"));
}

void PluginManagerTest::VoidAndThrowingFunctions(void) {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // void reset() → invalid QVariant, no error.
    const QVariant result =
        plugins.call_function(QStringLiteral("void TestPlugin::reset()"), {});
    QVERIFY(!result.isValid());

    // void boom() throws; the host surfaces it as mcp::Error.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(QStringLiteral("void TestPlugin::boom()"), {}),
        mcp::Error);
}

void PluginManagerTest::InvalidCallsThrow(void) {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));

    // Unknown function.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(QStringLiteral("void TestPlugin::nope()"), {}),
        mcp::Error);

    // Wrong arity.
    QVERIFY_EXCEPTION_THROWN(
        plugins.call_function(
            QStringLiteral("QString TestPlugin::echo(QString)"), {}),
        mcp::Error);
}

void PluginManagerTest::DependencyGatesLoad(void) {
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

void PluginManagerTest::CrossPluginForwarding(void) {
    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN)));
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_PLUGIN_PAIR)));

    QCOMPARE(plugins.plugin_ids(), (QStringList{"test-plugin", "test-pair"}));
    QVERIFY(plugins.has_function("QString TestPluginPair::forward(QString)"));
    QVERIFY(plugins.has_function("QString TestPlugin::echo(QString)"));

    // TestPluginPair::forward calls TestPlugin::echo through the host and
    // surfaces the other plugin's returned value in its own result.
    const QVariant forwarded = plugins.call_function(
        QStringLiteral("QString TestPluginPair::forward(QString)"),
        {QStringLiteral("cross")});
    QCOMPARE(forwarded.toString(), QStringLiteral("test-pair received: cross"));
}

void PluginManagerTest::UnloadHonorsReverseDependencies(void) {
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
    QCOMPARE(plugins.functions().size(), 2);  // host services only
}

void PluginManagerTest::LoggingPluginClaimsAndCannotBeReplaced(void) {
    // Runs last (declaration order): it claims the process-wide sink.
    QVERIFY(!mcp::log_sink_claimed());

    mcp::PluginManager plugins;
    QVERIFY(plugins.load_file(QString::fromUtf8(MCP_TEST_LOG_PLUGIN)));
    QVERIFY(mcp::log_sink_claimed());

    QVERIFY(plugins.plugin_object("test-logger") != nullptr);
    QVERIFY(
        plugins
            .call_function(QStringLiteral("bool TestLogPlugin::claimed()"), {})
            .toBool());

    const int before =
        plugins.call_function(QStringLiteral("int TestLogPlugin::count()"), {})
            .toInt();

    // A core log event must reach the plugin's sink (the plugin decides how to
    // handle it; here it counts/stashes it).
    MCP_LOG_WARN("logging-plugin-probe-line");
    const int after =
        plugins.call_function(QStringLiteral("int TestLogPlugin::count()"), {})
            .toInt();
    QVERIFY2(after >= before + 1,
             "logging plugin did not receive the log event");
    QVERIFY(
        plugins
            .call_function(QStringLiteral("QString TestLogPlugin::last()"), {})
            .toString()
            .contains("logging-plugin-probe-line"));

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