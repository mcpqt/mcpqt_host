// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// A plugin used by plugin_test. Built as a MODULE library the test loads at
// runtime. It deliberately declares a duplicate command name so the manager's
// de-duplication can be asserted.

#include "plugin.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

#include <stdexcept>
#include <string>
#include <vector>

class TestPlugin : public QObject, public mcp::PluginInterface {
    Q_OBJECT
    // IID must match MCP_PLUGIN_IID in <plugin.hpp>.
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/1.0" FILE "test_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    std::string id() const override { return "test-plugin"; }
    std::string version() const override { return "9.9.9"; }

    std::vector<mcp::PluginCommand> commands() const override {
        auto string_arg = [](const char* key) {
            return QJsonObject{
                { "type", "object" },
                { "properties",
                  QJsonObject{ { key, QJsonObject{ { "type", "string" } } } } },
                { "required", QJsonArray{ key } },
            };
        };

        std::vector<mcp::PluginCommand> cmds;

        cmds.push_back(mcp::PluginCommand{
            .name         = "plugin_echo",
            .description  = "Echo back the `message` argument.",
            .input_schema = string_arg("message"),
            .handler =
                [](const mcp::Json& args) -> mcp::CallToolResult {
                    return mcp::CallToolResult{ .content = { mcp::TextContent{
                        .text = args.toObject().value("message").toString().toStdString() } } };
                },
        });

        cmds.push_back(mcp::PluginCommand{
            .name         = "plugin_add",
            .description  = "Add two numbers.",
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
                    const double      s = o.value("a").toDouble() + o.value("b").toDouble();
                    return mcp::CallToolResult{ .content = { mcp::TextContent{
                        .text = QString::number(s).toStdString() } } };
                },
        });

        // Throws a non-mcp exception; the manager should turn this into an
        // is_error result rather than letting it escape.
        cmds.push_back(mcp::PluginCommand{
            .name         = "plugin_boom",
            .description  = "Always fails.",
            .input_schema = QJsonObject{ { "type", "object" } },
            .handler =
                [](const mcp::Json&) -> mcp::CallToolResult {
                    throw std::runtime_error("boom from plugin");
                },
        });

        // Duplicate name (declared twice on purpose). The second must be
        // skipped by the manager.
        for (int i = 0; i < 2; ++i) {
            cmds.push_back(mcp::PluginCommand{
                .name         = "plugin_dup",
                .description  = "Duplicate command.",
                .input_schema = QJsonObject{ { "type", "object" } },
                .handler =
                    [](const mcp::Json&) -> mcp::CallToolResult {
                        return mcp::CallToolResult{
                            .content = { mcp::TextContent{ .text = "dup" } } };
                    },
            });
        }

        return cmds;
    }
};

#include "test_plugin.moc"
