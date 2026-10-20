// SPDX-License-Identifier: AGPL-3.0-only
//
// Echo plugin: the `echo` tool formerly provided by http_echo_server.

#include "plugin.h"

#include <QObject>
#include <QString>

namespace {

class EchoPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "echo")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description", "Echoes text back.")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "echo_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit EchoPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        m_host = host;
        registerPublicSlots();
    }
    void shutdown(void) override {}

public slots:
    QString echo(QString text) { return QStringLiteral("echo: ") + text; }
};

}  // namespace

#include "echo_plugin.moc"