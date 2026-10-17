// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Plugin ABI for the MCP host.
//
// A plugin is an ordinary Qt plugin (a QObject implementing PluginInterface,
// marked with Q_PLUGIN_METADATA / Q_INTERFACES) that advertises a list of
// named commands. The host's PluginManager loads such plugins and registers
// one MCP tool per command on a Server — i.e. a plugin can implement MCP
// tools without linking against anything beyond this header and `mcp::mcp`.
//
// Writing a plugin:
//
//   #include <mcp/plugin.hpp>
//   #include <QObject>
//
//   class MyPlugin : public QObject, public mcp::PluginInterface {
//       Q_OBJECT
//       // The IID literal must match MCP_PLUGIN_IID below (moc does not
//       // expand the macro here).
//       Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/1.0" FILE "myplugin.json")
//       Q_INTERFACES(mcp::PluginInterface)
//   public:
//       std::string id() const override { return "myplugin"; }
//       std::vector<mcp::PluginCommand> commands() const override {
//           return {{
//               .name         = "myplugin.reverse",
//               .description  = "Reverse a string.",
//               .input_schema = QJsonObject{ ... },
//               .handler      = [](const mcp::Json& args) -> mcp::CallToolResult {
//                   ...
//               },
//           }};
//       }
//   };
//
// The JSON metadata file (the FILE argument above) must be valid JSON; a
// minimal one is `{ "id": "myplugin", "version": "1.0.0" }`.

#pragma once

#include "mcp/json.hpp"
#include "mcp/protocol.hpp"

#include <QtPlugin>

#include <functional>
#include <string>
#include <vector>

namespace mcp {

/// One command a plugin exposes. The host registers it as an MCP tool named
/// `name` with `input_schema`; when the tool is called, `handler` runs with
/// the inbound `arguments` (or JSON null when the client omitted them).
///
/// `handler` follows the same contract as Server::ToolHandler: return a
/// CallToolResult (set `is_error` for an in-protocol failure) or throw
/// `mcp::Error` to report a JSON-RPC error.
struct PluginCommand {
    /// Tool name. Must be non-empty and unique across all loaded plugins.
    std::string name;

    /// Optional human-readable title and description shown to clients.
    std::string title;
    std::string description;

    /// JSON Schema object for the tool input. When not an object, the host
    /// substitutes a permissive `{"type": "object"}` schema.
    Json input_schema;

    /// The command implementation.
    std::function<CallToolResult(const Json& arguments)> handler;
};

/// Interface every plugin implements. Kept deliberately small; the concrete
/// plugin is a QObject (for Qt's plugin machinery and meta-object metadata)
/// that also derives from this.
class PluginInterface {
public:
    PluginInterface()                                  = default;
    PluginInterface(const PluginInterface&)            = delete;
    PluginInterface& operator=(const PluginInterface&) = delete;
    virtual ~PluginInterface()                         = default;

    /// Stable, non-empty identifier for the plugin (e.g. "myplugin").
    virtual std::string id() const = 0;

    /// Optional version string.
    virtual std::string version() const { return {}; }

    /// Optional one-line description.
    virtual std::string description() const { return {}; }

    /// Called once after the plugin is loaded and cast, before its commands
    /// are enumerated/registered. May be used for setup. Keep it cheap and
    /// exception-safe: a throw aborts the load.
    virtual void initialize() {}

    /// Called once when the host unloads the plugin, before the shared
    /// library is detached. Must not throw.
    virtual void shutdown() {}

    /// The commands this plugin contributes. Called once at load time.
    virtual std::vector<PluginCommand> commands() const = 0;
};

}  // namespace mcp

/// Unique interface identifier (reverse-DNS). Used by both Q_PLUGIN_METADATA
/// and qobject_cast in the host.
#define MCP_PLUGIN_IID "ai.mcp.qt.mcp-plugin/1.0"

Q_DECLARE_INTERFACE(mcp::PluginInterface, MCP_PLUGIN_IID)
