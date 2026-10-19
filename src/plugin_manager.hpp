// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginManager: the host side of the plugin ABI. It loads Qt plugins
// implementing PluginInterface from disk, reflects each plugin's public slots
// into a process-wide function registry, routes cross-plugin calls, and
// surfaces every published function as an MCP tool on a Server.
//
//   mcp::Server server{ mcp::Implementation{ .name = "host", .version = "1.0" } };
//   mcp::PluginManager plugins{ server };
//   plugins.load_directory("/path/to/plugins");   // tools now on `server`
//   server.run(std::make_unique<mcp::StdioTransport>());
//
// Cross-plugin calls: every function is keyed by the fully-qualified signature
// "<pluginId>.<slotSignature>", e.g. "hello.greet(QVariant)". A plugin calls
// any other plugin's function through the HostInterface handed to it at
// initialize() (HostInterface::callFunc). The manager owns the registry, so
// plugins never discover or reference each other directly.
//
// Lifetime: the referenced Server must outlive the PluginManager. Registered
// tool handlers keep a weak reference to the loaded plugin, so once a plugin
// is unloaded (or the manager destroyed) calling its tool returns an error
// result rather than touching detached code. Loading/unloading must not race
// with in-flight tool calls: load plugins before run(), unload after it stops.
//
// Logging: a plugin can claim the process-wide diagnostic sink through
// HostInterface::claimLogging(). The first claim wins; afterwards the core
// rejects replacement, and the claiming plugin is pinned — unload_all() leaves
// it loaded (its sink runs code in the plugin's library, so detaching it would
// be unsafe). The core keeps its minimal default stderr sink as a fallback for
// before the claim and when no logging plugin is present.
//
// This header lives in the core `mcp` library and depends only on Qt6::Core
// (QPluginLoader). Plugins themselves are separate CMake targets (MODULE
// libraries) building against `mcp::mcp`.

#pragma once

#include "plugin.hpp"

#include <QString>
#include <QStringList>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class QMetaMethod;
class QObject;
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
    /// failure (not a shared library, wrong interface, duplicate plugin id,
    /// a declared dependency that is not already satisfied, or
    /// initialize()/slot registration threw) the loader is cleaned up and
    /// false is returned. Functions whose signature is already registered are
    /// skipped (with a warning) but do not fail the load.
    ///
    /// Dependencies: a plugin may declare, in its metadata JSON, a
    /// `dependencies` array of fully-qualified function signatures (and/or
    /// plugin ids) that must be available before it is initialized — e.g.
    /// `"dependencies": ["hello.greet(QVariant)"]`. Each entry is satisfied if
    /// a previously-loaded plugin already published that signature, or if a
    /// plugin with that id is already loaded. A plugin whose dependencies are
    /// not yet satisfiable fails to load (false), so a dependent plugin must
    /// be loaded after the plugins it depends on. `load_directory()` retries
    /// such plugins automatically.
    bool load_file(const QString& file);

    /// Load every plugin library in `directory`, honoring plugin dependencies.
    /// Candidates whose dependencies are not yet satisfiable are retried on
    /// later passes, so the directory may be scanned in any order. Returns the
    /// number of plugins successfully loaded (0 if the directory is
    /// missing/empty).
    std::size_t load_directory(const QString& directory);

    /// Call shutdown() on every plugin and detach its shared library. Safe to
    /// call more than once. Must not be called while tool calls are in flight.
    void unload_all();

    /// Number of currently loaded plugins.
    [[nodiscard]] std::size_t plugin_count() const;

    /// Identifiers (`PluginInterface::id()`) of the loaded plugins.
    [[nodiscard]] QStringList plugin_ids() const;

    /// Names of the MCP tools registered from published functions.
    [[nodiscard]] std::vector<std::string> registered_tools() const;

    /// True when a tool with `name` was registered by a plugin function.
    [[nodiscard]] bool has_tool(const std::string& name) const;

    /// The QObject of the loaded plugin with `id`, or nullptr. Lets embedders
    /// reach through to a plugin (e.g. to call slot-introspection or a test
    /// plugin's custom API past the host interface).
    [[nodiscard]] QObject* plugin_object(const std::string& id) const;

private:
    struct Instance;
    struct FunctionEntry;

    /// Outcome of an attempt to load a single plugin file.
    enum class LoadOutcome {
        Loaded,
        UnsatisfiedDependencies,   ///< declared deps not all available yet
        Failed,                    ///< any other failure
    };

    /// Parse the `dependencies` array from a plugin's metadata JSON, or return
    /// an empty list when absent/invalid. `loader` may or may not be loaded;
    /// metaData() works without loading the library.
    [[nodiscard]] QStringList plugin_dependencies(const QPluginLoader* loader) const;

    /// True when every entry of `dependencies` is satisfied by a loaded plugin
    /// id or a registered function signature.
    [[nodiscard]] bool dependencies_satisfied(const QStringList& dependencies) const;

    /// Read a plugin's declared dependencies and, if satisfied, load it.
    LoadOutcome load_file_impl(const QString& file, bool quiet);

    /// Host-side callbacks bound into the HostInterface handed to plugins.
    QStringList                  host_get_funcs() const;
    QVariant                     host_call_func(const QString& signature,
                                                const QVariant& packed_args);
    bool                         host_reg_func(const std::shared_ptr<Instance>& inst,
                                               const QString& signature);
    void                         host_unload_self(const std::shared_ptr<Instance>& inst,
                                                  const QString& reason);

    /// Remove a function's registry entry and (if this plugin owned it) the
    /// corresponding MCP tool from `tools_`.
    void drop_function(const QString& signature);

    /// Unload a single plugin (shutdown + detach + erase from the registry).
    void unload_instance(const std::shared_ptr<Instance>& inst);

    Server&                                  server_;
    std::vector<std::shared_ptr<Instance>>   plugins_;
    std::vector<std::string>                 tools_;
};

}  // namespace mcp