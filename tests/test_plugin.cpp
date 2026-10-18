// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// A plugin used by plugin_test. Built as a MODULE library the test loads at
// runtime. Its public slots are published by the host via meta-object
// reflection, so their signatures become MCP tools:
//   test-plugin.echo, test-plugin.add, test-plugin.boom, test-plugin.dup.

#include "plugin.hpp"

#include <QObject>
#include <QVariant>
#include <QVariantMap>

#include <stdexcept>
#include <string>

class TestPlugin : public mcp::PluginInterface {
    Q_OBJECT
    // IID must match MCP_PLUGIN_IID in <plugin.hpp>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "test_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit TestPlugin(QObject* parent = nullptr) : mcp::PluginInterface(parent) {}

    std::string id() const override { return "test-plugin"; }
    std::string version() const override { return "9.9.9"; }

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;
        registerPublicSlots();
    }
    void shutdown() override {}

public slots:
    // Returns whatever `message` was passed in.
    QVariant echo(const QVariant& args) {
        return args.toMap().value("message").toString();
    }

    // Adds two numbers.
    QVariant add(const QVariant& args) {
        const QVariantMap m = args.toMap();
        return m.value("a").toDouble() + m.value("b").toDouble();
    }

    // Throws; the host turns this into an is_error tool result.
    QVariant boom(const QVariant&) {
        throw std::runtime_error("boom from plugin");
    }

    QVariant dup(const QVariant&) { return QStringLiteral("dup"); }
};

#include "test_plugin.moc"