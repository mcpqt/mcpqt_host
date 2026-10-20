// SPDX-License-Identifier: AGPL-3.0-only
//
// Demo plugin. Built as a standalone MODULE library (hello_plugin.so); the
// host (examples/plugin_host) loads it and publishes its public slots as
// typed cross-plugin functions.
//
// The plugin links only mcp::mcp and Qt Core — it never sees the host binary.
// All cross-plugin communication flows through the HostInterface handed to
// initialize(); the plugin never knows or touches the host class.

#include "plugin.h"

#include <QObject>
#include <QString>

#include <string>

namespace {

class HelloPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "hello")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description",
                "Demo plugin contributing a greeting and a sum function.")
    // The IID string must match MCP_PLUGIN_IID in <plugin.h>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "hello_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    // The public slots below are ordinary typed functions; the host reflects
    // them. The Q_CLASSINFO("id") prefixes the function signatures.
    explicit HelloPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        m_host = host;
        registerPublicSlots();
    }
    void shutdown(void) override {}

public slots:
    // "QString HelloPlugin::greet(QString)"
    QString greet(QString name) {
        if (name.isEmpty()) {
            name = QStringLiteral("world");
        }
        return QStringLiteral("Hello, %1!").arg(name);
    }

    // "double HelloPlugin::sum(double,double)"
    double sum(double a, double b) { return a + b; }
};

}  // namespace

#include "hello_plugin.moc"