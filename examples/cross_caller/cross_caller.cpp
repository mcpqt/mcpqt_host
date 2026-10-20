// SPDX-License-Identifier: AGPL-3.0-only
//
// Cross-call demo plugin. Loaded alongside examples/hello_plugin, it calls
// HelloPlugin's function through the host — the plugin never knows the
// concrete host or plugin classes, only the HostInterface callbacks and the
// target's signature.
//
// Discovery + call both go through the host:
//   - host.getFuncs()  → signature strings of every plugin function, e.g.
//                        "QString HelloPlugin::greet(QString)"
//   - host.callFunc()  → invoke one by signature, one QVariant per parameter

#include "plugin.h"

#include <QObject>
#include <QString>

#include <string>

namespace {

class CrossCallerPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "caller")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description",
                "Demonstrates calling another plugin's function via the host.")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "cross_caller.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit CrossCallerPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        m_host = host;

        // Discover: find HelloPlugin's greet function by signature pattern.
        const QStringList funcs = m_host.getFuncs();
        for (const QString& sig : funcs) {
            if (sig.startsWith(QStringLiteral("QString HelloPlugin::greet("))) {
                m_target_greet = sig;
                break;
            }
        }
        registerPublicSlots();
    }
    void shutdown(void) override {}

public slots:
    // "QString CrossCallerPlugin::greet_via_host(QString)" — forwards to
    // "QString HelloPlugin::greet(QString)", passing the name as the single
    // positional argument.
    QString greet_via_host(QString name) {
        if (m_target_greet.isEmpty()) {
            return QStringLiteral("hello plugin's greet() is not loaded");
        }
        return m_host.callFunc(m_target_greet, {name}).toString();
    }

private:
    QString m_target_greet;
};

}  // namespace

#include "cross_caller.moc"