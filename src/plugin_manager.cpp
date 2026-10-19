// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginManager implementation: QPluginLoader discovery, meta-object
// reflection of each plugin's public slots into a cross-plugin function
// registry, and host-side routing with typed QGenericArgument invocation.

#include "plugin_manager.hpp"

#include "error.hpp"
#include "log.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPluginLoader>

#include <algorithm>
#include <exception>
#include <utility>

namespace mcp {

struct PluginManager::Instance {
    QPluginLoader*    loader = nullptr;
    PluginInterface*  iface  = nullptr;
    QObject*          object = nullptr;   // iface as QObject (same address)
    QString           path;
    std::string       id;
    QStringList       signatures;         // fully-qualified signatures this plugin published
    QStringList       dependencies;       // declared plugin ids / function signatures
    /// True once this plugin owns the process-wide log sink. Pinned plugins are
    /// never unloaded: the sink executes code inside the plugin's library.
    bool              pinned = false;
};

/// One published function: the reflective method on the owning plugin.
struct PluginManager::FunctionEntry {
    std::weak_ptr<Instance> owner;
    QMetaMethod             method;
};

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() { unload_all(); }

QStringList PluginManager::host_get_funcs() const {
    return functions();
}

QVariant PluginManager::host_call_func(const QString& signature,
                                       const QVariantList& args) const {
    return call_function(signature, args);
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
    const QStringList funcs      = functions();
    for (const QString& dep : dependencies) {
        if (loaded_ids.contains(dep)) {
            continue;  // a plugin with this id is already loaded
        }
        bool found = false;
        for (const QString& sig : funcs) {
            // Match a fully-qualified signature exactly, or as a bare
            // "plugin.function" prefix of one ("hello.greet" → hello.greet(QString)).
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

QVariant PluginManager::invoke_method(QObject* object,
                                      const QMetaMethod& method,
                                      const QVariantList& args) {
    const int arity = method.parameterCount();
    if (args.size() != arity) {
        throw Error{ error_code::invalid_params,
                     "expected " + std::to_string(arity) + " argument(s) for '" +
                         method.methodSignature().toStdString() + "', got " +
                         std::to_string(args.size()) };
    }

    // Convert each supplied value to the declared parameter type, then expose
    // its storage to QGenericArgument for the reflective call.
    QVariantList converted = args;
    QGenericArgument gen[Q_METAMETHOD_INVOKE_MAX_ARGS];
    for (int i = 0; i < arity; ++i) {
        const QMetaType param_type = method.parameterMetaType(i);
        if (converted[i].metaType() != param_type && !converted[i].convert(param_type)) {
            throw Error{ error_code::invalid_params,
                         "argument " + std::to_string(i) + " for '" +
                             method.methodSignature().toStdString() +
                             "' cannot be converted to " +
                             param_type.name() };
        }
        gen[i] = QGenericArgument(converted[i].typeName(), converted[i].constData());
    }

    const QMetaType return_type = method.returnMetaType();
    if (return_type == QMetaType::fromType<void>()) {
        const bool ok = method.invoke(
            object, Qt::DirectConnection,
            gen[0], gen[1], gen[2], gen[3], gen[4],
            gen[5], gen[6], gen[7], gen[8], gen[9]);
        if (!ok) {
            throw Error{ error_code::internal_error,
                         "failed to invoke '" +
                             method.methodSignature().toStdString() + "'" };
        }
        return {};
    }

    // A QVariant return is already the value we want to hand back — wrapping
    // it in another QVariant would double-wrap it. Let invoke write it directly.
    if (return_type == QMetaType::fromType<QVariant>()) {
        QVariant result;
        const bool ok = method.invoke(
            object, Qt::DirectConnection, Q_RETURN_ARG(QVariant, result),
            gen[0], gen[1], gen[2], gen[3], gen[4],
            gen[5], gen[6], gen[7], gen[8], gen[9]);
        if (!ok) {
            throw Error{ error_code::internal_error,
                         "failed to invoke '" +
                             method.methodSignature().toStdString() + "'" };
        }
        return result;
    }

    // Any other registered QMetaType is a valid return type: allocate storage
    // for it, hand QGenericReturnArgument that storage, then wrap the result in
    // a QVariant for the caller.
    void* storage = return_type.create();
    if (storage == nullptr) {
        throw Error{ error_code::internal_error,
                     "cannot allocate return value of type " +
                         std::string(return_type.name()) + " for '" +
                         method.methodSignature().toStdString() + "'" };
    }
    QGenericReturnArgument return_arg(return_type.name(), storage);
    const bool ok = method.invoke(
        object, Qt::DirectConnection, return_arg,
        gen[0], gen[1], gen[2], gen[3], gen[4],
        gen[5], gen[6], gen[7], gen[8], gen[9]);
    QVariant result(return_type, storage);
    return_type.destroy(storage);
    if (!ok) {
        throw Error{ error_code::internal_error,
                     "failed to invoke '" +
                         method.methodSignature().toStdString() + "'" };
    }
    return result;
}

QVariant PluginManager::call_function(const QString& signature,
                                      const QVariantList& args) const {
    const auto it = registry_.constFind(signature);
    if (it == registry_.constEnd()) {
        throw Error{ error_code::method_not_found,
                     "unknown function: " + signature.toStdString() };
    }
    const auto owner = it->owner.lock();
    if (!owner) {
        throw Error{ error_code::internal_error,
                     "function's plugin is no longer loaded: " +
                         signature.toStdString() };
    }
    try {
        return invoke_method(owner->object, it->method, args);
    } catch (const Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Error{ error_code::internal_error, e.what() };
    }
}

bool PluginManager::host_reg_func(const std::shared_ptr<Instance>& inst,
                                  const QString& signature) {
    const QString prefix = QString::fromStdString(inst->id) + ".";
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
    if (method.methodType() != QMetaMethod::Slot ||
        method.access() != QMetaMethod::Public) {
        MCP_LOG_WARN("PluginManager: '" + signature.toStdString() +
                     "' is not a public slot");
        return false;
    }
    // Reject inherited QObject slots (e.g. deleteLater) and other base-class
    // methods: only functions declared by the plugin itself are publishable.
    if (method.enclosingMetaObject() != mo) {
        // Every plugin also inherits QObject's public slots (e.g.
        // deleteLater); they are expected noise, not a load problem.
        MCP_LOG_DEBUG("PluginManager: '" + signature.toStdString() +
                      "' is inherited, not declared by the plugin");
        return false;
    }

    const QMetaType return_type = method.returnMetaType();
    if (!return_type.isValid()) {
        MCP_LOG_WARN("PluginManager: '" + signature.toStdString() +
                     "' has an unregistered return type");
        return false;
    }
    if (method.parameterCount() > Q_METAMETHOD_INVOKE_MAX_ARGS) {
        MCP_LOG_WARN("PluginManager: '" + signature.toStdString() +
                     "' has more than 10 parameters");
        return false;
    }
    for (int p = 0; p < method.parameterCount(); ++p) {
        if (!method.parameterMetaType(p).isValid()) {
            MCP_LOG_WARN("PluginManager: '" + signature.toStdString() +
                         "' parameter " + std::to_string(p) +
                         " has no registered QMetaType");
            return false;
        }
    }

    if (registry_.contains(signature)) {
        MCP_LOG_WARN("PluginManager: function '" + signature.toStdString() +
                     "' is already registered; skipping duplicate");
        return false;
    }

    registry_.insert(signature, FunctionEntry{ inst, method });
    inst->signatures.append(signature);
    return true;
}

void PluginManager::host_unload_self(const std::shared_ptr<Instance>& inst,
                                     const QString& reason) {
    MCP_LOG_INFO("PluginManager: unloading plugin '" + inst->id + "' on its "
                 "request: " + reason.toStdString());
    unload_instance(inst);
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
        registry_.remove(signature);
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
    inst->iface  = nullptr;
    inst->object = nullptr;
}

PluginManager::LoadOutcome PluginManager::load_file_impl(const QString& file,
                                                         bool quiet) {
    auto* loader = new QPluginLoader(file);

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
    inst->dependencies = deps;

    HostInterface host;
    host.getFuncs    = [this] { return host_get_funcs(); };
    host.callFunc    = [this](const QString& sig, const QVariantList& args) {
        return host_call_func(sig, args);
    };
    host.regFunc     = [this, inst](const QString& sig) { return host_reg_func(inst, sig); };
    host.unloadSelf  = [this, inst](const QString& reason) { host_unload_self(inst, reason); };
    host.claimLogging = [inst](LogSink sink) {
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

bool PluginManager::load_file(const QString& file) {
    return load_file_impl(file, /*quiet=*/false) == LoadOutcome::Loaded;
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

bool PluginManager::unload_plugin(const std::string& id) {
    std::shared_ptr<Instance> target;
    for (const auto& inst : plugins_) {
        if (inst->id == id) {
            target = inst;
            break;
        }
    }
    if (!target) {
        MCP_LOG_WARN("PluginManager: cannot unload unknown plugin '" + id + "'");
        return false;
    }
    if (target->pinned) {
        MCP_LOG_WARN("PluginManager: refusing to unload pinned plugin '" + id + "'");
        return false;
    }

    const QString target_id = QString::fromStdString(target->id);
    for (const auto& other : plugins_) {
        if (other.get() == target.get()) {
            continue;
        }
        for (const QString& dep : other->dependencies) {
            bool conflict = (dep == target_id);
            if (!conflict) {
                for (const QString& sig : target->signatures) {
                    if (sig == dep || sig.startsWith(dep + QLatin1Char('('))) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (conflict) {
                MCP_LOG_WARN("PluginManager: refusing to unload '" + id +
                             "': plugin '" + other->id + "' depends on '" +
                             dep.toStdString() + "'");
                return false;
            }
        }
    }

    unload_instance(target);
    plugins_.erase(std::remove_if(plugins_.begin(), plugins_.end(),
                                  [&target](const std::shared_ptr<Instance>& p) {
                                      return p.get() == target.get();
                                  }),
                   plugins_.end());
    MCP_LOG_INFO("PluginManager: unloaded plugin '" + id + "'");
    return true;
}

void PluginManager::unload_all() {
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

QObject* PluginManager::plugin_object(const std::string& id) const {
    for (const auto& inst : plugins_) {
        if (inst->id == id) return inst->object;
    }
    return nullptr;
}

QStringList PluginManager::functions() const {
    QStringList sigs = registry_.keys();
    sigs.sort();
    return sigs;
}

bool PluginManager::has_function(const QString& signature) const {
    return registry_.contains(signature);
}

}  // namespace mcp