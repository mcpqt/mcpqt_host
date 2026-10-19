// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// A second plugin used by plugin_test to exercise CROSS-PLUGIN calls. It
// publishes its own public slot `pair.forward`, whose handler calls the
// `test-plugin.echo` function through the host (HostInterface::callFunc).
//
// The pair plugin must be loaded AFTER test_plugin, since its initialize()
// records the target signature eagerly. plugin_test loads them in order.

#include "plugin.hpp"

#include "log.hpp"

#include <QObject>
#include <QVariant>
#include <QVariantMap>

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
    // "test-pair.forward(QVariant)" — remotely calls test-plugin.echo through
    // the host, then *displays* the value it got back: it is logged and
    // embedded in this plugin's own return value so the caller (and the test)
    // can see that the round-trip actually returned test-plugin's data.
    QVariant forward(const QVariant& args) {
        if (target_.isEmpty()) {
            return QStringLiteral("target echo function not found");
        }
        const QVariantMap m = args.toMap();

        // Inter-plugin call: routed by the host to the other plugin.
        const QVariant returned = host_.callFunc(target_, QVariantList{ m });

        // Display the returned value (plugin-side log) and fold it into our
        // own result so it is observable by the MCP caller too.
        const QString text = returned.toString();
        MCP_LOG_INFO("test-pair: test-plugin.echo returned '" +
                     text.toStdString() + "'");
        return QStringLiteral("test-pair received: %1").arg(text);
    }

private:
    QString target_;
};

#include "test_plugin_pair.moc"