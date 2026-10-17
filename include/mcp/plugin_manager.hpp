// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginManager: loads Qt plugins implementing PluginInterface from disk and
// registers each plugin command as an MCP tool on a Server.
//
//   mcp::Server server{ mcp::Implementation{ .name = "host", .version = "1.0" } };
//   mcp::PluginManager plugins{ server };
//   plugins.load_directory("/path/to/plugins");   // tools now on `server`
//   server.run(std::make_unique<mcp::StdioTransport>());
//
// Lifetime: the referenced Server must outlive the PluginManager. Registered
// tool handlers hold a weak reference to the loaded plugin, so once a plugin
// is unloaded (or the manager destroyed) calling its tool returns an error
// result rather than touching detached code. Loading/unloading must not race
// with in-flight tool calls: load plugins before run(), unload after it stops.
//
// This header lives in the core `mcp` library and depends only on Qt6::Core
// (QPluginLoader). Plugins themselves are separate CMake targets (MODULE
// libraries) building against `mcp::mcp`.

#pragma once

#include "mcp/plugin.hpp"

#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class QPluginLoader;

namespace mcp {

class Server;

class PluginManager {
public:
    /// Manage plugins for `server`. The Server is referenced, not owned.
    explicit PluginManager(Server& server);
    ~PluginManager();

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Load a single plugin shared library. Returns true on success. On
    /// failure (not a shared library, wrong interface, or initialize() threw)
    /// the loader is cleaned up and false is returned. Commands whose name is
    /// already registered are skipped (with a warning) but do not fail the
    /// load, so a successful load may register zero tools.
    bool load_file(const QString& file);

    /// Load every plugin library in `directory`. Returns the number of
    /// plugins successfully loaded (0 if the directory is missing/empty).
    std::size_t load_directory(const QString& directory);

    /// Call shutdown() on every plugin and detach its shared library. Safe to
    /// call more than once. Must not be called while tool calls are in flight.
    void unload_all();

    /// Number of currently loaded plugins.
    [[nodiscard]] std::size_t plugin_count() const;

    /// Identifiers (`PluginInterface::id()`) of the loaded plugins.
    [[nodiscard]] QStringList plugin_ids() const;

    /// Names of the MCP tools registered from plugins.
    [[nodiscard]] std::vector<std::string> registered_tools() const;

    /// True when a tool with `name` was registered by a plugin.
    [[nodiscard]] bool has_tool(const std::string& name) const;

private:
    struct Instance;

    /// Register the commands of an already-initialised plugin on the server.
    /// Returns the number of tools newly registered.
    std::size_t register_commands(const std::shared_ptr<Instance>& inst);

    Server&                                 server_;
    std::vector<std::shared_ptr<Instance>>  plugins_;
    std::vector<std::string>                tools_;
};

}  // namespace mcp
