// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Demo plugin. Built as a standalone MODULE library (hello_plugin.so); the
// host (examples/plugin_host) loads it and publishes its public slots as
// typed cross-plugin functions.
//
// The plugin links only mcp::mcp and Qt Core — it never sees the host binary.
// All cross-plugin communication flows through the HostInterface handed to
// initialize(); the plugin never knows or touches the host class.

#include "plugin.hpp"

#include <QObject>
#include <QString>

#include <string>

namespace {

class HelloPlugin : public mcp::PluginInterface {
    Q_OBJECT
    // The IID string must match MCP_PLUGIN_IID in <plugin.hpp>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "hello_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    // The public slots below are ordinary typed functions; the host reflects
    // them. `id()` prefixes the function signatures.
    explicit HelloPlugin(QObject* parent = nullptr) : mcp::PluginInterface(parent) {}

    std::string id() const override { return "hello"; }
    std::string version() const override { return "1.0.0"; }
    std::string description() const override {
        return "Demo plugin contributing a greeting and a sum function.";
    }

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;
        registerPublicSlots();
    }
    void shutdown() override {}

public slots:
    // "hello.greet(QString)"
    QString greet(QString name) {
        if (name.isEmpty()) {
            name = QStringLiteral("world");
        }
        return QStringLiteral("Hello, %1!").arg(name);
    }

    // "hello.sum(double,double)"
    double sum(double a, double b) { return a + b; }
};

}  // namespace

#include "hello_plugin.moc"