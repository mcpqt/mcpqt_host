// SPDX-License-Identifier: AGPL-3.0-only
//
// Plugin ABI for the MCP host.
//
// A plugin is an ordinary Qt plugin: a QObject implementing
// mcp::PluginInterface, marked with Q_PLUGIN_METADATA / Q_INTERFACES. The
// host's PluginManager loads such plugins with QPluginLoader and registers
// every public slot with the host:
//
//     void      myFunction(int a, QString b);   // 0..10 parameters
//     QString   myFunction(double x);           // any registered QMetaType
//                                               // return (void included)
//
// Parameters use ordinary types (anything with a registered QMetaType that
// QGenericArgument can carry); they are NOT packed into a single QVariant.
// The host invokes the slot reflectively with the caller-supplied arguments,
// so the plugin writes normal typed functions. A function's address is its
// fully-qualified signature, e.g. "QString MyPlugin::reverse(QString)".
//
// The plugin never talks to other plugins directly; everything goes through
// the HostInterface the host hands it once at initialize():
//
//     - getFuncs  enumerate every function the host currently knows
//     - callFunc  invoke any function by signature, passing one QVariant per
//                 parameter (a positional QVariantList)
//     - regFunc   publish a function (used by registerPublicSlots())
//     - unloadSelf ask the host to tear this plugin down
//
// Because the host reflects over each plugin's QMetaObject and routes calls by
// signature string, a plugin can call functions owned by OTHER plugins without
// linking against them or knowing their classes.
//
// Writing a plugin:
//
//   #include <mcp/plugin.h>
//
//   class MyPlugin : public mcp::PluginInterface {
//       Q_OBJECT
//       // Plugin metadata lives in Q_CLASSINFO; "id" is required and prefixes
//       // every function signature. "version"/"description" are optional.
//       Q_CLASSINFO("id", "myplugin")
//       Q_CLASSINFO("version", "1.0.0")
//       Q_CLASSINFO("description", "What this plugin does.")
//       // The IID literal must match MCP_PLUGIN_IID below (moc does not
//       // expand the macro inside Q_PLUGIN_METADATA).
//       Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "myplugin.json")
//       Q_INTERFACES(mcp::PluginInterface)
//   public:
//       void initialize(const mcp::HostInterface& host) override {
//           m_host = host;
//           registerPublicSlots();
//       }
//   public slots:
//       QString reverse(QString text) { return text.reversed(); }
//   };
//
// The JSON metadata file (the FILE argument above) must be valid JSON. It is
// used by the host for declared dependencies, and may declare a "Tools" object
// mapping a published function signature to a Tool JSON value (the
// host validates the signature and exposes the mapping; it does not register
// MCP tools yet):
//
//   "Tools": {
//     "QString MyPlugin::reverse(QString)": {
//       "name": "reverse",
//       "description": "Reverse a string.",
//       "inputSchema": {
//         "type": "object",
//         "properties": { "text": { "type": "string" } }
//       }
//     }
//   }
//
// A minimal metadata file is `{}`.

#pragma once

#include "json.h"
#include "log.h"
#include "protocol.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QtPlugin>

#include <functional>
#include <string>

namespace mcp {

/// The handle a plugin receives once at initialize(). All plugin↔host
/// interaction — including cross-plugin function calls — flows through these
/// callbacks. Copy it into a member (as `m_host`) if you need it later.
struct HostInterface {
    /// Enumerate every function currently registered with the host, across all
    /// loaded plugins. Each entry is a fully-qualified signature of the form
    /// "<return_type> <class_name>::<function_name>(<param_types>)", e.g.
    /// "QString HelloPlugin::greet(QString)".
    std::function<QStringList()> getFuncs;

    /// Invoke a function by its fully-qualified signature. `args` holds one
    /// QVariant per declared parameter, in order (positional); the host builds
    /// the QGenericArguments and invokes the slot. Returns the slot's QVariant
    /// result, or an invalid QVariant for a void function. Throws mcp::Error if
    /// the function is unknown, the plugin has been unloaded, or the argument
    /// count/types cannot be matched. Runs synchronously on the caller's
    /// thread.
    std::function<QVariant(const QString& signature, const QVariantList& args)>
        callFunc;

    /// Publish a function under a fully-qualified signature. The host validates
    /// the signature against the calling plugin's meta-object, so this must be
    /// called with the canonical "<return_type>
    /// <class_name>::<function_name>(<param_types>)" form. Returns true if the
    /// function was newly published, false if it is unknown or already
    /// registered. `registerPublicSlots()` drives this automatically.
    std::function<bool(const QString& signature)> regFunc;

    /// Ask the connected client to elicit user input. Blocks until the client
    /// answers and returns its result. Only valid while a plugin function is
    /// being invoked by a server session (the host routes the request to that
    /// session); throws mcp::Error otherwise.
    std::function<ElicitResult(const ElicitRequestParams&)> elicit;

    /// Ask the host to tear this plugin down (shutdown + unload). Optional.
    std::function<void(const QString& reason)> unloadSelf;

    /// Claim process-wide diagnostic logging: install `sink` (which decides how
    /// each `MCP_LOG_*` event is rendered and where it goes) and take
    /// exclusive ownership of it. Returns false if logging was already claimed.
    /// Once claimed, no other plugin (nor `set_log_sink`) can replace the sink.
    ///
    /// A plugin that claims logging is "pinned": the host will not unload it,
    /// because the installed sink runs code owned by the plugin. Call this last
    /// in initialize(), after any setup that may fail.
    std::function<bool(LogSink)> claimLogging;
};

/// Interface every plugin implements. The concrete plugin is a QObject (for
/// Qt's plugin machinery and meta-object metadata) that also derives from this.
///
/// Deliberately NOT given a Q_OBJECT of its own: the header is shared between
/// the `mcp` library and every plugin MODULE target, and a Q_OBJECT here would
/// be moc'ed once per target (duplicate `staticMetaObject`). The concrete
/// plugin's Q_OBJECT overrides the virtual `metaObject()`, so scanning
/// `this->metaObject()` from the interface methods still returns the derived
/// class's full meta-object.
class PluginInterface : public QObject {
public:
    PluginInterface(QObject* parent = nullptr);
    PluginInterface(const PluginInterface&) = delete;
    PluginInterface& operator=(const PluginInterface&) = delete;
    ~PluginInterface() override = default;

    /// Called once after the plugin is loaded and cast, before any of its
    /// functions becomes visible to other plugins. Call registerPublicSlots()
    /// (or host.regFunc directly) from here. Keep it cheap and exception-safe:
    /// a throw aborts the load.
    virtual void initialize(const HostInterface& host) = 0;

    /// Called once when the host unloads the plugin, before the shared library
    /// is detached. Must not throw.
    virtual void shutdown(void) {}

protected:
    /// Publish every public slot on this plugin to the host. This does not
    /// validate or reject anything: the host decides which slots are acceptable
    /// functions (0..10 parameters, a registered QMetaType return including
    /// `void`, declared by the plugin, not duplicated). Call it from
    /// initialize() after storing `host` into `m_host`.
    void registerPublicSlots(void);

    /// Stored copy of the host handle, for use after initialize().
    HostInterface m_host;
};

/// Read a Q_CLASSINFO value (e.g. "id", "version", "description") from a
/// plugin's meta-object. Returns an empty QString when the key is absent.
[[nodiscard]] QString plugin_class_info(const QMetaObject* meta_object,
                                        const char* key);

}  // namespace mcp

/// Unique interface identifier (reverse-DNS). Used by both Q_PLUGIN_METADATA
/// and qobject_cast in the host. Bumped to /2.0 for the reflection-based ABI.
#define MCP_PLUGIN_IID "ai.mcp.qt.mcp-plugin/2.0"

Q_DECLARE_INTERFACE(mcp::PluginInterface, MCP_PLUGIN_IID)