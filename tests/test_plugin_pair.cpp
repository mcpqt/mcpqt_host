// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// A second plugin used by plugin_test to exercise CROSS-PLUGIN calls. Its
// public slot `test-pair.forward(QString)` calls `test-plugin.echo(QString)`
// through the host (HostInterface::callFunc), then displays the returned value
// in its own result.
//
// The pair plugin declares a dependency on test-plugin, so the host will not
// load it until test-plugin is available.

#include "plugin.hpp"

#include "log.hpp"

#include <QObject>
#include <QString>

#include <string>

class TestPluginPair : public mcp::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "test_plugin_pair.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit TestPluginPair(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    std::string id() const override { return "test-pair"; }
    std::string version() const override { return "0.1.0"; }

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;
        // Discovery: locate test-plugin.echo's fully-qualified signature.
        const QStringList funcs = host_.getFuncs();
        for (const QString& sig : funcs) {
            if (sig.startsWith(QStringLiteral("test-plugin.echo("))) {
                target_ = sig;
                break;
            }
        }
        registerPublicSlots();
    }
    void shutdown() override {}

public slots:
    // "test-pair.forward(QString)" — remotely calls test-plugin.echo through
    // the host, then displays the value it got back: it is logged and embedded
    // in this plugin's own return value.
    QString forward(QString message) {
        if (target_.isEmpty()) {
            return QStringLiteral("target echo function not found");
        }

        // Inter-plugin call: one positional argument, routed by the host.
        const QString text = host_.callFunc(target_, { message }).toString();

        // Display the returned value (plugin-side log) and fold it into our
        // own result so it is observable by the caller too.
        MCP_LOG_INFO("test-pair: test-plugin.echo returned '" +
                     text.toStdString() + "'");
        return QStringLiteral("test-pair received: %1").arg(text);
    }

private:
    QString target_;
};

#include "test_plugin_pair.moc"