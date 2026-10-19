// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Cross-call demo plugin. Loaded alongside examples/hello_plugin, it calls
// HelloPlugin's public slot through the host — the plugin never knows the
// concrete host or plugin classes, only the HostInterface callbacks and the
// packed-QVariant convention.
//
// Discovery + call both go through the host:
//   - host.getFuncs()  → signature strings of every plugin function, e.g.
//                        "hello.greet(QVariant)"
//   - host.callFunc()  → invoke one by its signature

#include "plugin.hpp"

#include <QObject>
#include <QVariant>
#include <QVariantMap>

#include <string>

namespace {

class CrossCallerPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "cross_caller.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit CrossCallerPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    std::string id() const override { return "caller"; }
    std::string version() const override { return "1.0.0"; }
    std::string description() const override {
        return "Demonstrates calling another plugin's function via the host.";
    }

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;

        // Discover: find HelloPlugin's greet slot by signature pattern.
        const QStringList funcs = host_.getFuncs();
        for (const QString& sig : funcs) {
            if (sig.startsWith(QStringLiteral("hello.greet("))) {
                target_greet_ = sig;
                break;
            }
        }
        registerPublicSlots();
    }
    void shutdown() override {}

public slots:
    // "caller.greet_via_host(QVariant)" — forwards to hello.greet at call time
    // (handles the case where hello_plugin loads after us).
    QVariant greet_via_host(const QVariant& args) {
        if (target_greet_.isEmpty()) {
            return QStringLiteral("hello plugin's greet() is not loaded");
        }
        const QVariantMap m = args.toMap();
        return host_.callFunc(target_greet_, QVariantList{ m });
    }

private:
    QString target_greet_;
};

}  // namespace

#include "cross_caller.moc"