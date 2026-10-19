// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginManager implementation: QPluginLoader discovery, meta-object
// reflection of each plugin's public slots into a cross-plugin function
// registry, host-side routing, and MCP tool registration.

#include "plugin_manager.hpp"

#include "error.hpp"
#include "log.hpp"
#include "server.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaMethod>
#include <QMetaObject>
#include <QObject>
#include <QPluginLoader>

#include <algorithm>
#include <exception>
#include <utility>

namespace mcp {

namespace {

CallToolResult error_result(std::string text) {
    CallToolResult result;
    result.content  = { TextContent{ .text = std::move(text), .annotations = std::nullopt } };
    result.is_error = true;
    return result;
}

Json permissive_schema() {
    return QJsonObject{ { "type", "object" }, { "properties", QJsonObject{} } };
}

/// The MCP tool name for a function whose full signature is
/// "<pluginId>.<slotName>(...)" — the qualified slot name without the params.
/// e.g. "hello.greet(QVariant)" → "hello.greet".
QString tool_name_for(const QString& signature) {
    const qsizetype open = signature.indexOf(QLatin1Char('('));
    return open > 0 ? signature.left(open) : signature;
}

/// Convert a slot's QVariant result into an MCP tool result. Strings become
/// TextContent; objects/arrays become structured_content (plus a JSON text
/// dump so text-only clients still see something).
CallToolResult variant_to_tool_result(const QVariant& out) {
    CallToolResult result;
    if (!out.isValid()) {
        result.content = { TextContent{ .text = {}, .annotations = std::nullopt } };
        return result;
    }
    const QJsonValue jv = QJsonValue::fromVariant(out);
    if (jv.isString()) {
        result.content = { TextContent{ .text = jv.toString().toStdString(),
                                        .annotations = std::nullopt } };
    } else if (jv.isObject() || jv.isArray()) {
        result.structured_content = jv;
        const QByteArray utf8 = jv.isObject()
                                    ? QJsonDocument(jv.toObject()).toJson(QJsonDocument::Compact)
                                    : QJsonDocument(jv.toArray()).toJson(QJsonDocument::Compact);
        result.content = { TextContent{ .text = utf8.toStdString(),
                                        .annotations = std::nullopt } };
    } else {
        // bool / number / null → compact scalar text.
        const QByteArray utf8 = QJsonValue::fromVariant(out).toVariant().toString().toUtf8();
        result.content = { TextContent{ .text = utf8.toStdString(),
                                        .annotations = std::nullopt } };
    }
    return result;
}

}  // namespace

struct PluginManager::Instance {
    QPluginLoader*    loader = nullptr;
    PluginInterface*  iface  = nullptr;
    QObject*          object = nullptr;   // iface as QObject (same address)
    QString           path;
    std::string       id;
    QStringList       signatures;         // fully-qualified signatures this plugin published
    /// True once this plugin owns the process-wide log sink. Pinned plugins are
    /// never unloaded: the sink executes code inside the plugin's library.
    bool              pinned = false;
};

/// One published function: the method on the owning plugin plus the metadata
/// used for the MCP tool surface. `owner` guards unload safety.
struct PluginManager::FunctionEntry {
    std::weak_ptr<Instance> owner;
    QMetaMethod             method;
    QString                 signature;
    QString                 description;
};

PluginManager::PluginManager(Server& server) : server_(server) {}

PluginManager::~PluginManager() { unload_all(); }

QStringList PluginManager::host_get_funcs() const {
    QStringList funcs;
    for (const auto& instance : plugins_) {
        funcs << instance->signatures;
    }
    return funcs;
}

QVariant PluginManager::host_call_func(const QString& signature,
                                       const QVariant& packed_args) {
    // Locate the entry by scanning loaded plugins (the registry is a flat list
    // of signatures per instance, so an owner must be found first).
    std::shared_ptr<Instance> owner;
    QMetaMethod               method;
    for (const auto& instance : plugins_) {
        if (instance->signatures.contains(signature)) {
            // Re-derive the method: iterate the meta-object for the signature.
            const QByteArray sig = signature.mid(signature.indexOf('.') + 1).toUtf8();
            const int idx = instance->object->metaObject()->indexOfMethod(sig.constData());
            if (idx < 0) {
                throw Error{ error_code::method_not_found,
                             "function signature no longer resolvable: " +
                                 signature.toStdString() };
            }
            owner  = instance;
            method = instance->object->metaObject()->method(idx);
            break;
        }
    }
    if (!owner) {
        throw Error{ error_code::method_not_found,
                     "unknown function: " + signature.toStdString() };
    }

    QVariant local_args = packed_args;   // Q_ARG needs an lvalue
    QVariant result;
    if (!method.invoke(owner->object, Qt::DirectConnection,
                       Q_RETURN_ARG(QVariant, result),
                       Q_ARG(QVariant, local_args))) {
        throw Error{ error_code::internal_error,
                     "failed to invoke '" + signature.toStdString() + "'" };
    }
    return result;
}

bool PluginManager::host_reg_func(const std::shared_ptr<Instance>& inst,
                                  const QString& signature) {
    // Validate shape "<pluginId>.<slotSignature>".
    const QString prefix  = QString::fromStdString(inst->id) + ".";
    if (!signature.startsWith(prefix)) {
        MCP_LOG_WARN("PluginManager: plugin '" + inst->id +
                     "' tried to register foreign signature '" +
                     signature.toStdString() + "'");
        return false;
    }
    const QByteArray slot_sig = signature.mid(prefix.size()).toUtf8();
    const QMetaObject* mo = inst->object->metaObject();
    const int idx = mo->indexOfMethod(slot_sig.constData());
    if (idx < 0) {
        MCP_LOG_WARN("PluginManager: plugin '" + inst->id +
                     "' signature '" + signature.toStdString() +
                     "' matches no public slot");
        return false;
    }
    const QMetaMethod method = mo->method(idx);
    // Enforce the uniform convention at the host too (defense in depth): one
    // QVariant in, QVariant out.
    if (method.parameterCount() != 1 ||
        method.parameterMetaType(0) != QMetaType::fromType<QVariant>() ||
        method.returnMetaType() != QMetaType::fromType<QVariant>()) {
        MCP_LOG_WARN("PluginManager: plugin '" + inst->id +
                     "' signature '" + signature.toStdString() +
                     "' does not follow the QVariant(const QVariant&) convention");
        return false;
    }

    if (inst->signatures.contains(signature)) {
        MCP_LOG_WARN("PluginManager: function '" + signature.toStdString() +
                     "' is already registered; skipping duplicate");
        return false;
    }

    const QString tool_name = tool_name_for(signature);
    if (has_tool(tool_name.toStdString())) {
        MCP_LOG_WARN("PluginManager: tool '" + tool_name.toStdString() +
                     "' is already registered; skipping duplicate from plugin '" +
                     inst->id + "'");
        return false;
    }

    inst->signatures.append(signature);

    const std::weak_ptr<Instance> weak     = inst;
    const QString                 ctx_desc = signature;
    const Json                    schema   = permissive_schema();

    server_.tool(
        tool_name.toStdString(), schema,
        [weak, signature, ctx_desc](const Json& args) -> CallToolResult {
            const auto plugin = weak.lock();
            if (!plugin) {
                return error_result("plugin function '" +
                                    signature.toStdString() +
                                    "' is no longer available");
            }
            // Resolve the method afresh: the registry may have been rebuilt.
            const QByteArray tool_slot_sig =
                signature.mid(signature.indexOf('.') + 1).toUtf8();
            const int tool_idx =
                plugin->object->metaObject()->indexOfMethod(tool_slot_sig.constData());
            if (tool_idx < 0) {
                return error_result("plugin function '" +
                                    signature.toStdString() +
                                    "' is no longer resolvable");
            }
            const QMetaMethod tool_method =
                plugin->object->metaObject()->method(tool_idx);
            try {
                QVariant local_args = args.toVariant();   // Json → packed QVariant
                QVariant result;
                if (!tool_method.invoke(plugin->object, Qt::DirectConnection,
                                        Q_RETURN_ARG(QVariant, result),
                                        Q_ARG(QVariant, local_args))) {
                    return error_result("plugin function '" +
                                        signature.toStdString() +
                                        "' failed to invoke");
                }
                return variant_to_tool_result(result);
            } catch (const Error&) {
                throw;  // protocol-level error, propagate to the client
            } catch (const std::exception& e) {
                return error_result("plugin function '" +
                                    signature.toStdString() + "' failed: " + e.what());
            }
        },
        std::optional<std::string>{}, std::optional<std::string>{ ctx_desc.toStdString() });

    tools_.push_back(tool_name.toStdString());
    return true;
}

void PluginManager::host_unload_self(const std::shared_ptr<Instance>& inst,
                                     const QString& reason) {
    MCP_LOG_INFO("PluginManager: unloading plugin '" + inst->id + "' on its "
                 "request: " + reason.toStdString());
    unload_instance(inst);
}

void PluginManager::drop_function(const QString& signature) {
    for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
        auto& inst = *it;
        if (inst->signatures.removeOne(signature)) {
            const QString tool_name = tool_name_for(signature);
            tools_.erase(std::remove(tools_.begin(), tools_.end(),
                                     tool_name.toStdString()),
                         tools_.end());
            return;
        }
    }
}

void PluginManager::unload_instance(const std::shared_ptr<Instance>& inst) {
    if (inst->pinned) {
        // Pinned plugins own the log sink; detaching the library would leave
        // the sink pointing at unmapped code. Keep it loaded for the process
        // lifetime (the loader is intentionally not destroyed).
        MCP_LOG_WARN("PluginManager: refusing to unload pinned plugin '" +
                     inst->id + "' (it owns the log sink)");
        return;
    }
    for (const QString& signature : inst->signatures) {
        const QString tool_name = tool_name_for(signature);
        tools_.erase(std::remove(tools_.begin(), tools_.end(),
                                 tool_name.toStdString()),
                     tools_.end());
    }
    inst->signatures.clear();

    if (inst->iface) {
        try {
            inst->iface->shutdown();
        } catch (...) {
            // shutdown() is documented not to throw; never let it escape.
        }
    }
    if (inst->loader) {
        inst->loader->unload();
        delete inst->loader;
        inst->loader = nullptr;
    }
    inst->iface   = nullptr;
    inst->object  = nullptr;
}

QStringList PluginManager::plugin_dependencies(const QPluginLoader* loader) const {
    QStringList      dependencies;
    const QJsonValue meta = loader->metaData().value(QLatin1String("MetaData"));
    if (!meta.isObject()) {
        return dependencies;
    }
    const QJsonValue deps = meta.toObject().value(QLatin1String("dependencies"));
    if (!deps.isArray()) {
        return dependencies;
    }
    const QJsonArray arr = deps.toArray();
    for (const QJsonValue& v : arr) {
        if (v.isString()) {
            dependencies << v.toString();
        }
    }
    return dependencies;
}

bool PluginManager::dependencies_satisfied(const QStringList& dependencies) const {
    const QStringList loaded_ids = plugin_ids();
    const QStringList funcs      = host_get_funcs();
    for (const QString& dep : dependencies) {
        if (loaded_ids.contains(dep)) {
            continue;  // a plugin with this id is already loaded
        }
        bool found = false;
        for (const QString& sig : funcs) {
            // Match a fully-qualified signature exactly, or as a bare
            // "plugin.function" prefix of one ("hello.greet" → hello.greet(QVariant)).
            if (sig == dep || sig.startsWith(dep + QLatin1Char('('))) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool PluginManager::load_file(const QString& file) {
    return load_file_impl(file, /*quiet=*/false) == LoadOutcome::Loaded;
}

PluginManager::LoadOutcome PluginManager::load_file_impl(const QString& file,
                                                         bool quiet) {
    auto* loader = new QPluginLoader(file);

    // Read declared dependencies from the embedded metadata (no load needed)
    // and defer the plugin until they are all satisfiable. This prevents a
    // dependent plugin from being initialized before the plugins (and their
    // functions) it relies on exist, so its initialize() can call them safely.
    const QStringList deps = plugin_dependencies(loader);
    if (!deps.isEmpty() && !dependencies_satisfied(deps)) {
        if (!quiet) {
            MCP_LOG_WARN("PluginManager: '" + file.toStdString() +
                         "' declares dependencies that are not yet satisfied; "
                         "deferring load");
        }
        delete loader;
        return LoadOutcome::UnsatisfiedDependencies;
    }

    QObject* obj = loader->instance();
    if (!obj) {
        MCP_LOG_WARN("PluginManager: failed to load '" + file.toStdString() +
                     "': " + loader->errorString().toStdString());
        delete loader;
        return LoadOutcome::Failed;
    }

    auto* iface = qobject_cast<PluginInterface*>(obj);
    if (!iface) {
        MCP_LOG_WARN("PluginManager: '" + file.toStdString() +
                     "' is not an MCP plugin (interface mismatch)");
        loader->unload();
        delete loader;
        return LoadOutcome::Failed;
    }

    const std::string id = iface->id();
    for (const auto& existing : plugins_) {
        if (existing->id == id) {
            MCP_LOG_WARN("PluginManager: plugin id '" + id +
                         "' already loaded from " +
                         existing->path.toStdString() + "; rejecting " +
                         file.toStdString());
            loader->unload();
            delete loader;
            return LoadOutcome::Failed;
        }
    }

    auto inst          = std::make_shared<Instance>();
    inst->loader       = loader;
    inst->iface        = iface;
    inst->object       = obj;
    inst->path         = file;
    inst->id           = id;

    HostInterface host;
    host.getFuncs  = [this] { return host_get_funcs(); };
    host.callFunc  = [this](const QString& sig, const QVariant& args) { return host_call_func(sig, args); };
    host.regFunc   = [this, inst](const QString& sig) { return host_reg_func(inst, sig); };
    host.unloadSelf = [this, inst](const QString& reason) { host_unload_self(inst, reason); };
    host.claimLogging = [inst](LogSink sink) {
        // Claim the process-wide sink; on success the plugin becomes pinned so
        // its sink code cannot be detached while in use.
        if (!claim_log_sink(std::move(sink))) {
            return false;
        }
        inst->pinned = true;
        return true;
    };

    try {
        iface->initialize(host);
    } catch (const std::exception& e) {
        MCP_LOG_WARN("PluginManager: initialising '" + file.toStdString() +
                     "' threw: " + e.what());
        if (inst->pinned) {
            // It already claimed logging; its sink code must stay resident.
            // Keep the instance rather than unloading (which unload_instance
            // would refuse anyway) so this->signatures/tools stay consistent.
            plugins_.push_back(std::move(inst));
            return LoadOutcome::Failed;
        }
        unload_instance(inst);
        return LoadOutcome::Failed;
    }

    plugins_.push_back(std::move(inst));

    MCP_LOG_INFO("PluginManager: loaded plugin '" + plugins_.back()->id + "' (" +
                 file.toStdString() + "), " +
                 std::to_string(plugins_.back()->signatures.size()) +
                 " function(s) registered");
    return LoadOutcome::Loaded;
}

std::size_t PluginManager::load_directory(const QString& directory) {
    QDir dir(directory);
    if (!dir.exists()) {
        MCP_LOG_WARN("PluginManager: plugin directory does not exist: " +
                     directory.toStdString());
        return 0;
    }

    QStringList filters;
#if defined(Q_OS_WIN)
    filters << "*.dll";
#elif defined(Q_OS_MAC)
    filters << "*.dylib" << "*.so";
#else
    filters << "*.so";
#endif

    std::size_t loaded = 0;
    QStringList pending;
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& file : files) {
        pending << file.absoluteFilePath();
    }

    // Multi-pass load honoring dependencies. A plugin is only attempted once
    // the plugins/functions it requires are available; unresolved files are
    // retried on later passes until no progress can be made.
    bool progress = true;
    while (!pending.isEmpty() && progress) {
        progress = false;
        QStringList still;
        for (const QString& file : pending) {
            switch (load_file_impl(file, /*quiet=*/true)) {
                case LoadOutcome::Loaded:
                    ++loaded;
                    progress = true;
                    break;
                case LoadOutcome::UnsatisfiedDependencies:
                    still << file;
                    break;
                case LoadOutcome::Failed:
                    break;  // already warned by load_file_impl
            }
        }
        pending = still;
    }
    for (const QString& file : pending) {
        MCP_LOG_WARN("PluginManager: skipping '" + file.toStdString() +
                     "' — dependencies were never satisfiable");
    }
    return loaded;
}

void PluginManager::unload_all() {
    // Unload everything except pinned instances (which own the log sink and
    // must stay resident). Their registered tools stay registered too.
    for (auto it = plugins_.begin(); it != plugins_.end();) {
        if ((*it)->pinned) {
            ++it;
            continue;
        }
        unload_instance(*it);
        it = plugins_.erase(it);
    }
}

std::size_t PluginManager::plugin_count() const { return plugins_.size(); }

QStringList PluginManager::plugin_ids() const {
    QStringList ids;
    for (const auto& inst : plugins_) {
        ids << QString::fromStdString(inst->id);
    }
    return ids;
}

std::vector<std::string> PluginManager::registered_tools() const { return tools_; }

bool PluginManager::has_tool(const std::string& name) const {
    for (const auto& tool : tools_) {
        if (tool == name) return true;
    }
    return false;
}

QObject* PluginManager::plugin_object(const std::string& id) const {
    for (const auto& inst : plugins_) {
        if (inst->id == id) return inst->object;
    }
    return nullptr;
}

}  // namespace mcp