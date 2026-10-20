// SPDX-License-Identifier: AGPL-3.0-only
//
// A second plugin used by plugin_test to exercise CROSS-PLUGIN calls. Its
// public slot `QString TestPluginPair::forward(QString)` calls `QString
// TestPlugin::echo(QString)` through the host (HostInterface::callFunc), then
// displays the returned value in its own result.
//
// The pair plugin declares a dependency on test-plugin, so the host will not
// load it until test-plugin is available.

#include "plugin.h"

#include "log.h"

#include <QObject>
#include <QString>

#include <string>

class TestPluginPair : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "test-pair")
    Q_CLASSINFO("version", "0.1.0")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE
                          "test_plugin_pair.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit TestPluginPair(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        m_host = host;
        // Discovery: locate TestPlugin::echo's fully-qualified signature.
        const QStringList funcs = m_host.getFuncs();
        for (const QString& sig : funcs) {
            if (sig.startsWith(QStringLiteral("QString TestPlugin::echo("))) {
                m_target = sig;
                break;
            }
        }
        registerPublicSlots();
    }
    void shutdown(void) override {}

public slots:
    // "QString TestPluginPair::forward(QString)" — remotely calls
    // TestPlugin::echo through the host, then displays the value it got back:
    // it is logged and embedded in this plugin's own return value.
    QString forward(QString message) {
        if (m_target.isEmpty()) {
            return QStringLiteral("target echo function not found");
        }

        // Inter-plugin call: one positional argument, routed by the host.
        const QString text = m_host.callFunc(m_target, {message}).toString();

        // Display the returned value (plugin-side log) and fold it into our
        // own result so it is observable by the caller too.
        MCP_LOG_INFO("test-pair: TestPlugin::echo returned '" +
                     text.toStdString() + "'");
        return QStringLiteral("test-pair received: %1").arg(text);
    }

private:
    QString m_target;
};

#include "test_plugin_pair.moc"