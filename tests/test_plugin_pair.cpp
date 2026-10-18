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
    // "test-pair.forward(QVariant)" — forwards its `message` to
    // test-plugin.echo through the host, returning the echoed text.
    QVariant forward(const QVariant& args) {
        if (target_.isEmpty()) {
            return QStringLiteral("target echo function not found");
        }
        const QVariantMap m = args.toMap();
        return host_.callFunc(target_, m);
    }

private:
    QString target_;
};

#include "test_plugin_pair.moc"