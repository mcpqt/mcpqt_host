// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Demo MCP plugin. Built as a standalone MODULE library (hello_plugin.so);
// the host (examples/plugin_host) loads it and exposes its commands as tools.
//
// The plugin links only mcp::mcp and Qt Core — it never sees the host binary.

#include "plugin.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <string>
#include <vector>

class HelloPlugin : public QObject, public mcp::PluginInterface {
    Q_OBJECT
    // The IID string must match MCP_PLUGIN_IID in <plugin.hpp>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/1.0" FILE "hello_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    std::string id() const override { return "hello"; }
    std::string version() const override { return "1.0.0"; }
    std::string description() const override {
        return "Demo plugin contributing a greeting and a sum tool.";
    }

    std::vector<mcp::PluginCommand> commands() const override {
        std::vector<mcp::PluginCommand> cmds;

        cmds.push_back(mcp::PluginCommand{
            .name        = "hello_greet",
            .title       = "Greet",
            .description = "Return a friendly greeting for the given name.",
            .input_schema =
                QJsonObject{
                    { "type", "object" },
                    { "properties",
                      QJsonObject{
                          { "name",
                            QJsonObject{ { "type", "string" },
                                         { "description", "Who to greet." } } },
                      } },
                    { "required", QJsonArray{ "name" } },
                },
            .handler =
                [](const mcp::Json& args) -> mcp::CallToolResult {
                    const QString name = args.toObject().value("name").toString();
                    const QString who  = name.isEmpty() ? QStringLiteral("world") : name;
                    return mcp::CallToolResult{
                        .content = { mcp::TextContent{
                            .text = QStringLiteral("Hello, %1!").arg(who).toStdString() } },
                    };
                },
        });

        cmds.push_back(mcp::PluginCommand{
            .name        = "hello_sum",
            .title       = "Sum",
            .description = "Add two numbers (a + b).",
            .input_schema =
                QJsonObject{
                    { "type", "object" },
                    { "properties",
                      QJsonObject{
                          { "a", QJsonObject{ { "type", "number" } } },
                          { "b", QJsonObject{ { "type", "number" } } },
                      } },
                    { "required", QJsonArray{ "a", "b" } },
                },
            .handler =
                [](const mcp::Json& args) -> mcp::CallToolResult {
                    const QJsonObject o = args.toObject();
                    const double      a = o.value("a").toDouble();
                    const double      b = o.value("b").toDouble();
                    return mcp::CallToolResult{ .content = { mcp::TextContent{
                        .text = QString::number(a + b).toStdString() } } };
                },
        });

        return cmds;
    }
};

#include "hello_plugin.moc"
