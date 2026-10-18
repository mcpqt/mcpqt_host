// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Demo MCP plugin. Built as a standalone MODULE library (hello_plugin.so);
// the host (examples/plugin_host) loads it and exposes its public slots as
// MCP tools.
//
// The plugin links only mcp::mcp and Qt Core — it never sees the host binary.
// All cross-plugin communication flows through the HostInterface handed to
// initialize(); the plugin never knows or touches the host class.

#include "plugin.hpp"

#include <QObject>
#include <QVariant>
#include <QVariantMap>

#include <string>

namespace {

class HelloPlugin : public mcp::PluginInterface {
    Q_OBJECT
    // The IID string must match MCP_PLUGIN_IID in <plugin.hpp>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "hello_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    // The plugin is a C++ object; the QVariant(QVariant) slots below are what
    // the host publishes. `id()` prefixes the function signatures.
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
    // "hello.greet(QVariant)" — a single QVariant carrying the args map.
    QVariant greet(const QVariant& args) {
        const QVariantMap m = args.toMap();
        const QString   name = m.value("name", QStringLiteral("world")).toString();
        return QStringLiteral("Hello, %1!").arg(name);
    }

    // "hello.sum(QVariant)" — packed number args, QVariant(double) back.
    QVariant sum(const QVariant& args) {
        const QVariantMap m = args.toMap();
        const double      a = m.value("a", 0.0).toDouble();
        const double      b = m.value("b", 0.0).toDouble();
        return a + b;
    }
};

}  // namespace

#include "hello_plugin.moc"

// Standard Qt plugin entry point macro family:
//   Q_PLUGIN_METADATA    → metadata + factory for QPluginLoader
//   Q_INTERFACES         → makes qobject_cast<PluginInterface*> work
// Those two, plus the class above and include <QtPlugin>, are all a plugin
// needs. (The .moc include above defines the meta-object; AUTOMOC handles it.)