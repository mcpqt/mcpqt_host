// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginManager: the host side of the plugin ABI. It loads Qt plugins
// implementing PluginInterface from disk, reflects each plugin's public slots
// into a process-wide function registry, and routes cross-plugin calls.
//
//   mcp::PluginManager plugins;
//   plugins.load_directory("/path/to/plugins");
//   // ... plugins call one another through the HostInterface ...
//
// Cross-plugin calls: every function is keyed by the fully-qualified signature
// "<return_type> <class_name>::<function_name>(<param_types>)", e.g. "QString
// HelloPlugin::greet(QString)". A plugin calls any other plugin's function
// through the HostInterface handed to it at initialize()
// (HostInterface::callFunc), passing one QVariant per parameter. The host
// builds the QGenericArguments and invokes the typed slot; plugins never pack
// their parameters into a single QVariant.
//
// NOTE: tools declared in a plugin's metadata "Tools" object are
// auto-published: the host registers the matching public slot even when the
// plugin did not call registerPublicSlots() itself.
//
// Lifetime: registered calls keep a weak reference to the loaded plugin, so
// once a plugin is unloaded (or the manager destroyed) calling one of its
// functions reports an error rather than touching detached code.
// Loading/unloading must not race with in-flight calls.
//
// Logging: a plugin claims the process-wide diagnostic sink through the host
// function "bool mcp::Host::registerLogger(QString)" (listed by getFuncs()),
// passing the canonical signature of its own void log(int, QString) slot. The
// first claim wins; afterwards the core rejects replacement, and the claiming
// plugin is pinned — unload_all() leaves it loaded (its sink runs code in the
// plugin's library, so detaching it would be unsafe). The core keeps its
// minimal default stderr sink as a fallback.

#pragma once

#include "plugin.h"
#include "protocol.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <QList>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QMetaMethod;
class QObject;
class QPluginLoader;

namespace mcp
{

  class Server;

  /// One tool declaration from a plugin's metadata: a published function
  /// signature paired with the MCP Tool descriptor. Declared in the
  /// plugin's Q_PLUGIN_METADATA JSON as a signature-keyed object:
  ///
  ///   "Tools": {
  ///     "QString MyPlugin::reverse(QString)": {
  ///       "name": "reverse",
  ///       "description": "Reverse a string.",
  ///       "inputSchema": {
  ///         "type": "object",
  ///         "properties": { "text": { "type": "string" } }
  ///       }
  ///     }
  ///   }
  ///
  /// Only entries whose signature names a function the plugin actually
  /// publishes are retained.
  struct PluginTool
  {
    QString signature;
    Tool tool;
  };

  /// One resource declaration from a plugin's metadata: the resource
  /// descriptor plus the signature of the published function that renders it.
  /// Declared as a URI-keyed object:
  ///
  ///   "Resources": {
  ///     "notes://all": {
  ///       "name": "all-notes",
  ///       "description": "Every note as JSON",
  ///       "mimeType": "application/json",
  ///       "signature": "QString NotesPlugin::readAll(QString)"
  ///     }
  ///   }
  ///
  /// The reader takes an optional single QString (the URI) and returns the
  /// resource text (QString, or any value converted to JSON text).
  struct PluginResource
  {
    QString uri;
    Resource resource;
    QString signature;
  };

  class PluginManager
  {
  public:
    PluginManager(void);
    ~PluginManager(void);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Load a single plugin shared library. Returns true on success. On
    /// failure (not a shared library, wrong interface, duplicate plugin id,
    /// a declared dependency that is not already satisfied, or
    /// initialize()/slot registration threw) the loader is cleaned up and
    /// false is returned.
    ///
    /// Dependencies: a plugin may declare, in its metadata JSON, a
    /// `dependencies` array of fully-qualified function signatures (and/or
    /// plugin ids) that must be available before it is initialized — e.g.
    /// `"dependencies": ["QString HelloPlugin::greet(QString)"]`. Each entry
    /// is satisfied if a previously-loaded plugin already published that
    /// signature, or if a plugin with that id is already loaded. A plugin
    /// whose dependencies are not yet satisfiable fails to load (false);
    /// `load_directory()` retries such plugins automatically.
    bool load_file(const QString& file);

    /// Load every plugin library in `directory`, honoring plugin dependencies.
    /// Candidates whose dependencies are not yet satisfiable are retried on
    /// later passes, so the directory may be scanned in any order. Returns the
    /// number of plugins successfully loaded (0 if the directory is
    /// missing/empty).
    std::size_t load_directory(const QString& directory);

    /// Unload a single plugin by its id. Returns false if no such plugin is
    /// loaded, if it is pinned (owns the log sink), or if another loaded
    /// plugin declares a dependency on it or on one of its published functions
    /// — reverse dependencies are honored so a dependent is never left
    /// dangling. Must not be called while that plugin's calls are in flight.
    bool unload_plugin(const QString& id);

    /// Call shutdown() on every unloadable plugin and detach its shared
    /// library. Pinned plugins (those that own the log sink) are kept loaded.
    /// Safe to call more than once. Must not be called while calls are in
    /// flight.
    void unload_all(void);

    /// Number of currently loaded plugins.
    [[nodiscard]] std::size_t plugin_count(void) const;

    /// Identifiers of the loaded plugins (their Q_CLASSINFO("id")).
    [[nodiscard]] QStringList plugin_ids(void) const;

    /// The QObject of the loaded plugin with `id`, or nullptr.
    [[nodiscard]] QObject* plugin_object(const QString& id) const;

    /// Every published function signature, across all loaded plugins.
    [[nodiscard]] QStringList functions(void) const;

    /// True when `signature` is currently published.
    [[nodiscard]] bool has_function(const QString& signature) const;

    /// Tool declarations contributed by loaded plugins (from each plugin's
    /// metadata "Tools" object), sorted by signature. Declared signatures are
    /// auto-published as the plugin's matching public slots; entries that do
    /// not name a published function are dropped at load time with a warning.
    [[nodiscard]] QList<PluginTool> tools() const;

    /// The Tool declared for `signature`, if any loaded plugin
    /// declared one for a published function.
    [[nodiscard]] std::optional<Tool> tool(const QString& signature) const;

    /// Resource declarations contributed by loaded plugins.
    [[nodiscard]] QList<PluginResource> resources() const;

    /// The resource descriptor declared for `uri`, if any.
    [[nodiscard]] std::optional<PluginResource>
    resource(const QString& uri) const;

    /// Register every declared tool and resource on `server`, mapping incoming
    /// JSON arguments to function parameters by name. May be called once per
    /// Server (e.g. per HTTP session); returns the number of registrations.
    std::size_t register_tools(Server& server) const;

    /// Invoke a published function. `args` holds one QVariant per declared
    /// parameter, in order. Returns the QVariant result (invalid for a void
    /// function). Throws mcp::Error on unknown functions, unloaded plugins,
    /// arity/type mismatches, or exceptions escaping the slot.
    [[nodiscard]] QVariant call_function(const QString& signature,
                                         const QVariantList& args = {}) const;

  private:
    QVariant registerLogger(const QVariantList& args);
    static QVariant elicitPrompt(const QVariantList& args);
    struct Instance;
    struct FunctionEntry;

    enum class LoadOutcome
    {
      Loaded,
      UnsatisfiedDependencies, ///< declared deps not all available yet
      Failed,                  ///< any other failure
    };

    /// Parse the `dependencies` array from a plugin's metadata JSON.
    [[nodiscard]] QStringList
    plugin_dependencies(const QPluginLoader* loader) const;

    /// True when every entry of `dependencies` is satisfied.
    [[nodiscard]] bool
    dependencies_satisfied(const QStringList& dependencies) const;

    LoadOutcome load_file_impl(const QString& file, bool quiet);

    /// Invoke `method` on `object` with positional QVariant arguments.
    [[nodiscard]] static QVariant invoke_method(QObject* object,
                                                const QMetaMethod& method,
                                                const QVariantList& args);

    /// A host-provided function (e.g. registerLogger/elicit) in the registry.
    using HostFunction = std::function<QVariant(const QVariantList& args)>;

    /// Register the built-in host services (registerLogger, elicit).
    void install_host_functions(void);
    void register_host_function(const QString& signature, HostFunction fn);

    bool host_reg_func(const std::shared_ptr<Instance>& inst,
                       const QString& signature);
    void host_unload_self(const std::shared_ptr<Instance>& inst,
                          const QString& reason);

    /// Unload a single plugin (shutdown + detach + erase from the registry).
    void unload_instance(const std::shared_ptr<Instance>& inst);

    QHash<QString, FunctionEntry> m_registry; // signature -> entry
    QList<std::shared_ptr<Instance>> m_plugins;
  };

} // namespace mcp
