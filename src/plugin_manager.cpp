// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginManager implementation: QPluginLoader discovery, meta-object
// reflection of each plugin's public slots into a cross-plugin function
// registry, and host-side routing with typed QGenericArgument invocation.

#include "plugin_manager.h"

#include "error.h"
#include "log.h"
#include "server.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaClassInfo>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPluginLoader>

#include <QHash>
#include <QList>
#include <QString>
#include <algorithm>
#include <exception>
#include <utility>

namespace mcp
{
  namespace
  {

    // The Server currently dispatching a plugin call on this thread. Set by
    // register_tools()'s handlers around call_function() so
    // HostInterface::elicit can route elicitation to the session that invoked
    // the function.
    thread_local Server* t_current_server = nullptr;

    struct CurrentServerGuard
    {
      explicit CurrentServerGuard(Server* server)
          : m_previous(t_current_server)
      {
        t_current_server = server;
      }
      ~CurrentServerGuard(void) { t_current_server = m_previous; }
      CurrentServerGuard(const CurrentServerGuard&) = delete;
      CurrentServerGuard& operator=(const CurrentServerGuard&) = delete;

    private:
      Server* m_previous;
    };

    QString class_info(const QMetaObject* meta_object, const char* key)
    {
      if(meta_object == nullptr || key == nullptr)
      {
        return {};
      }
      for(int i = 0; i < meta_object->classInfoCount(); ++i)
      {
        const QMetaClassInfo info = meta_object->classInfo(i);
        if(!qstrcmp(info.name(), key))
          return QString::fromLatin1(info.value());
      }
      return {};
    }

    Server* current_server(void) { return t_current_server; }

    CallToolResult error_result(QString text)
    {
      CallToolResult result;
      result.content = { TextContent{ .text = std::move(text) } };
      result.is_error = true;
      return result;
    }

    CallToolResult variant_to_tool_result(const QVariant& out)
    {
      CallToolResult result;
      if(!out.isValid())
      {
        result.content = { TextContent{ .text = {} } };
        return result;
      }
      const QJsonValue json = QJsonValue::fromVariant(out);
      if(json.isString())
      {
        result.content = { TextContent{ .text = json.toString() } };
      }
      else if(json.isObject() || json.isArray())
      {
        result.structured_content = json;
        result.content = { TextContent{ .text = mcp::dump(json) } };
      }
      else
      {
        result.content = { TextContent{ .text = mcp::dump(json) } };
      }
      return result;
    }

  } // namespace


  struct PluginManager::Instance
  {
    QPluginLoader* loader = nullptr;
    PluginInterface* iface = nullptr;
    QObject* object = nullptr; // iface as QObject (same address)
    QString path;
    QString id;
    QStringList signatures; // fully-qualified signatures this plugin published
    QStringList dependencies;   // declared plugin ids / function signatures
    QHash<QString, Tool> tools; // validated metadata "Tools" entries
    QHash<QString, PluginResource> resources; // validated "Resources" entries
    /// True once this plugin owns the process-wide log sink. Pinned plugins
    /// are never unloaded: the sink executes code inside the plugin's library.
    bool pinned = false;
  };

  /// One published function: the reflective method on the owning plugin.
  struct PluginManager::FunctionEntry
  {
    std::weak_ptr<Instance> owner;
    QMetaMethod method;
    HostFunction host_function; // set for host services
  };

  QVariant PluginManager::registerLogger(const QVariantList& args)
  {
    if(args.size() != 1)
    {
      throw Error{ error_code::invalid_params,
                   "registerLogger expects the sink function signature" };
    }
    const QString sink_signature = args[0].toString();
    const auto it = m_registry.constFind(sink_signature);
    if(it == m_registry.constEnd() || it->host_function)
    {
      throw Error{ error_code::invalid_params,
                   "registerLogger: '" + sink_signature
                       + "' is not a plugin function" };
    }
    const auto owner = it->owner.lock();
    if(!owner)
    {
      throw Error{ error_code::internal_error,
                   "registerLogger: the sink's plugin is not loaded" };
    }
    const QMetaMethod method = it->method;
    if(method.parameterCount() != 2
       || method.parameterMetaType(0) != QMetaType::fromType<int>()
       || method.parameterMetaType(1) != QMetaType::fromType<QString>()
       || method.returnMetaType() != QMetaType::fromType<void>())
    {
      throw Error{ error_code::invalid_params,
                   "registerLogger: sink must be "
                   "'void <Class>::log(int, QString)'" };
    }

    QObject* const object = owner->object;
    const LogSink sink = [object, method](LogLevel level,
                                          const QString& msg)
    {
      static thread_local bool in_sink = false;
      if(!in_sink)
      {
        in_sink = true;
        try
        {
          (void)invoke_method(object, method,
                              { QVariant(static_cast<int>(level)), msg });
        } catch(...) {}
        in_sink = false;
      }
    };

    if(!claim_log_sink(sink))
    {
      return false;
    }
    owner->pinned = true;
    return true;
  }

  QVariant PluginManager::elicitPrompt(const QVariantList& args)
  {
    if(args.size() != 1)
    {
      throw Error{ error_code::invalid_params,
                  "elicit expects a JSON ElicitRequestParams" };
    }
    const ElicitRequestParams params
        = from_json<ElicitRequestParams>(args[0].value<QJsonValue>());
    Server* server = current_server();
    if(server == nullptr)
    {
      throw Error{ error_code::internal_error,
                  "elicit is only available while a server is "
                  "dispatching a plugin call" };
    }
    return QVariant::fromValue(to_json(server->elicit(params).get()));
  }


  PluginManager::PluginManager(void)
  {
    register_host_function("bool Host::registerLogger(QString)",
                           [this](const QVariantList& args) -> QVariant { return registerLogger(args); });

    register_host_function("QVariant Host::elicitPrompt(QVariant)",
                           [](const QVariantList& args) -> QVariant { return PluginManager::elicitPrompt(args); });
  }

  PluginManager::~PluginManager(void)
  {
    unload_all();
  }


  void PluginManager::register_host_function(const QString& signature,
                                             HostFunction fn)
  {
    FunctionEntry entry;
    entry.host_function = std::move(fn);
    m_registry.insert(signature, std::move(entry));
  }

  QStringList PluginManager::plugin_dependencies(const QPluginLoader* loader) const
  {
    QStringList dependencies;
    const QJsonValue meta
        = loader->metaData().value("MetaData");
    if(!meta.isObject())
    {
      return dependencies;
    }
    const QJsonValue deps
        = meta.toObject().value("dependencies");
    if(!deps.isArray())
    {
      return dependencies;
    }
    const QJsonArray arr = deps.toArray();
    for(const QJsonValue& v : arr)
    {
      if(v.isString())
      {
        dependencies << v.toString();
      }
    }
    return dependencies;
  }

  bool PluginManager::dependencies_satisfied(const QStringList& dependencies) const
  {
    const QStringList loaded_ids = plugin_ids();
    const QStringList funcs = functions();
    for(const QString& dep : dependencies)
    {
      if(loaded_ids.contains(dep))
      {
        continue; // a plugin with this id is already loaded
      }
      bool found = false;
      for(const QString& sig : funcs)
      {
        // Match a fully-qualified signature exactly, or as a signature
        // without its parameter list ("QString HelloPlugin::greet" →
        // "QString HelloPlugin::greet(QString)").
        if(sig == dep || sig.startsWith(dep + '('))
        {
          found = true;
          break;
        }
      }
      if(!found)
      {
        return false;
      }
    }
    return true;
  }

  QVariant PluginManager::invoke_method(QObject* object,
                                        const QMetaMethod& method,
                                        const QVariantList& args)
  {
    const int arity = method.parameterCount();
    if(args.size() != arity)
    {
      throw Error{ error_code::invalid_params,
                   "expected " + QString::number(arity) + " argument(s) for '"
                       + QString::fromLatin1(method.methodSignature())
                       + "', got " + QString::number(args.size()) };
    }

    // Convert each supplied value to the declared parameter type, then expose
    // its storage to QGenericArgument for the reflective call.
    QVariantList converted = args;
    QGenericArgument gen[Q_METAMETHOD_INVOKE_MAX_ARGS];
    for(int i = 0; i < arity; ++i)
    {
      const QMetaType param_type = method.parameterMetaType(i);
      if(converted[i].metaType() != param_type
         && !converted[i].convert(param_type))
      {
        throw Error{ error_code::invalid_params,
                     "argument " + QString::number(i) + " for '"
                         + QString::fromLatin1(method.methodSignature())
                         + "' cannot be converted to " + param_type.name() };
      }
      gen[i] = QGenericArgument(converted[i].typeName(),
                                converted[i].constData());
    }

    const QMetaType return_type = method.returnMetaType();
    if(return_type == QMetaType::fromType<void>())
    {
      const bool ok = method.invoke(object,
                                    Qt::DirectConnection,
                                    gen[0],
                                    gen[1],
                                    gen[2],
                                    gen[3],
                                    gen[4],
                                    gen[5],
                                    gen[6],
                                    gen[7],
                                    gen[8],
                                    gen[9]);
      if(!ok)
      {
        throw Error{ error_code::internal_error,
                     "failed to invoke '"
                         + QString::fromLatin1(method.methodSignature())
                         + "'" };
      }
      return {};
    }
    else
    {

      // Any other registered QMetaType is a valid return type: allocate storage
      // for it, hand QGenericReturnArgument that storage, then wrap the result
      // in a QVariant for the caller.
      void* storage = return_type.create();
      if(storage == nullptr)
      {
        throw Error{ error_code::internal_error,
                     "cannot allocate return value of type "
                         + QString(return_type.name()) + " for '"
                         + QString::fromLatin1(method.methodSignature()) + "'" };
      }
      QGenericReturnArgument return_arg(return_type.name(), storage);
      const bool ok = method.invoke(object,
                                    Qt::DirectConnection,
                                    return_arg,
                                    gen[0],
                                    gen[1],
                                    gen[2],
                                    gen[3],
                                    gen[4],
                                    gen[5],
                                    gen[6],
                                    gen[7],
                                    gen[8],
                                    gen[9]);
      QVariant result(return_type, storage);
      if(return_type == QMetaType::fromType<QVariant>())
        result = std::move(*static_cast<QVariant*>(storage)); // move directly into result to avoid wrapping QVariant type
      else
        return_type.destroy(storage);

      if(!ok)
      {
        throw Error{ error_code::internal_error,
                     "failed to invoke '"
                         + QString::fromLatin1(method.methodSignature()) + "'" };
      }
      return result;
    }
  }

  QVariant PluginManager::call_function(const QString& signature,
                                        const QVariantList& args) const
  {
    const auto it = m_registry.constFind(signature);
    if(it == m_registry.constEnd())
    {
      throw Error{ error_code::method_not_found,
                   "unknown function: " + signature };
    }

    try
    {
      if(it->host_function)
        return it->host_function(args);

      const auto owner = it->owner.lock();
      if(!owner)
        throw Error{ error_code::internal_error,
                     "function's plugin is no longer loaded: " + signature };
      return invoke_method(owner->object, it->method, args);
    }
    catch(const Error&)
    {
      throw;
    }
    catch(const std::exception& e)
    {
      throw Error{ error_code::internal_error, e.what() };
    }
  }

  bool PluginManager::host_reg_func(const std::shared_ptr<Instance>& inst,
                                    const QString& signature)
  {
    const QMetaObject* mo = inst->object->metaObject();

    // Signatures are "return_type class_name::function_name(param_types)".
    // The part after the last "::" before the parameter list is the method
    // signature moc knows; the full string is re-derived below to validate the
    // return type and class name.
    const qsizetype open = signature.indexOf('(');
    const qsizetype sep
        = open < 0 ? qsizetype{ -1 }
                   : signature.lastIndexOf("::", open);
    if(open < 0 || sep < 0)
    {
      MCP_LOG_WARN("PluginManager: plugin '" + inst->id
                   + "' registered malformed signature '" + signature + "'");
      return false;
    }
    const QByteArray slot_sig = signature.mid(sep + 2).toUtf8();
    const int idx = mo->indexOfMethod(slot_sig.constData());
    if(idx < 0)
    {
      MCP_LOG_WARN("PluginManager: plugin '" + inst->id + "' signature '"
                   + signature + "' matches no public slot");
      return false;
    }
    const QMetaMethod method = mo->method(idx);
    if(method.methodType() != QMetaMethod::Slot
       || method.access() != QMetaMethod::Public)
    {
      MCP_LOG_WARN("PluginManager: '" + signature + "' is not a public slot");
      return false;
    }
    // Reject inherited QObject slots (e.g. deleteLater) and other base-class
    // methods: only functions declared by the plugin itself are publishable.
    if(method.enclosingMetaObject() != mo)
    {
      // Every plugin also inherits QObject's public slots (e.g.
      // deleteLater); they are expected noise, not a load problem.
      MCP_LOG_DEBUG("PluginManager: '" + signature
                    + "' is inherited, not declared by the plugin");
      return false;
    }

    // Require the canonical signature: exact return type, class name, method
    // name, and parameter types in declaration order.
    const QString expected
        = QString::fromLatin1(method.typeName()) + ' '
          + mo->className() + "::" + method.methodSignature();
    if(signature != expected)
    {
      MCP_LOG_WARN("PluginManager: plugin '" + inst->id
                   + "' registered signature '" + signature
                   + "' but the canonical form is '" + expected + "'");
      return false;
    }

    const QMetaType return_type = method.returnMetaType();
    if(!return_type.isValid())
    {
      MCP_LOG_WARN("PluginManager: '" + signature
                   + "' has an unregistered return type");
      return false;
    }
    if(method.parameterCount() > Q_METAMETHOD_INVOKE_MAX_ARGS)
    {
      MCP_LOG_WARN("PluginManager: '" + signature
                   + "' has more than 10 parameters");
      return false;
    }
    for(int p = 0; p < method.parameterCount(); ++p)
    {
      if(!method.parameterMetaType(p).isValid())
      {
        MCP_LOG_WARN("PluginManager: '" + signature + "' parameter "
                     + QString::number(p) + " has no registered QMetaType");
        return false;
      }
    }

    if(m_registry.contains(signature))
    {
      MCP_LOG_WARN("PluginManager: function '" + signature
                   + "' is already registered; skipping duplicate");
      return false;
    }

    m_registry.insert(signature, FunctionEntry{ inst, method, {} });
    inst->signatures.append(signature);
    return true;
  }

  void PluginManager::host_unload_self(const std::shared_ptr<Instance>& inst,
                                       const QString& reason)
  {
    MCP_LOG_INFO("PluginManager: unloading plugin '" + inst->id
                 + "' on its "
                   "request: "
                 + reason);
    unload_instance(inst);
  }

  void PluginManager::unload_instance(const std::shared_ptr<Instance>& inst)
  {
    if(inst->pinned)
    {
      // Pinned plugins own the log sink; detaching the library would leave
      // the sink pointing at unmapped code. Keep it loaded for the process
      // lifetime (the loader is intentionally not destroyed).
      MCP_LOG_WARN("PluginManager: refusing to unload pinned plugin '"
                   + inst->id + "' (it owns the log sink)");
      return;
    }
    for(const QString& signature : inst->signatures)
    {
      m_registry.remove(signature);
    }
    inst->signatures.clear();

    if(inst->iface)
    {
      try
      {
        inst->iface->shutdown();
      }
      catch(...)
      {
        // shutdown() is documented not to throw; never let it escape.
      }
    }
    if(inst->loader)
    {
      inst->loader->unload();
      delete inst->loader;
      inst->loader = nullptr;
    }
    inst->iface = nullptr;
    inst->object = nullptr;
  }

  PluginManager::LoadOutcome PluginManager::load_file_impl(const QString& file,
                                                           bool quiet)
  {
    auto* loader = new QPluginLoader(file);

    // Parse the metadata "Tools" and "Resources" objects. Entries are
    // validated against the plugin's published functions after initialize().
    QHash<QString, Tool> declared_tools;
    QHash<QString, PluginResource> declared_resources;
    {
      const QJsonValue meta
          = loader->metaData().value("MetaData");
      if(meta.isObject())
      {
        const QJsonObject meta_object = meta.toObject();

        const QJsonValue tools_value
            = meta_object.value("Tools");
        if(tools_value.isObject())
        {
          const QJsonObject tools_object = tools_value.toObject();
          for(auto it = tools_object.constBegin();
              it != tools_object.constEnd(); ++it)
          {
            try
            {
              declared_tools.insert(it.key(), from_json<Tool>(it.value()));
            }
            catch(const std::exception& e)
            {
              MCP_LOG_WARN("PluginManager: '" + file + "' Tools entry '"
                           + it.key() + "' is invalid: " + e.what());
            }
          }
        }

        const QJsonValue resources_value
            = meta_object.value("Resources");
        if(resources_value.isObject())
        {
          const QJsonObject resources_object = resources_value.toObject();
          for(auto it = resources_object.constBegin();
              it != resources_object.constEnd(); ++it)
          {
            if(!it.value().isObject())
            {
              MCP_LOG_WARN("PluginManager: '" + file + "' Resources entry '"
                           + it.key() + "' is not an object");
              continue;
            }
            const QJsonObject o = it.value().toObject();
            PluginResource entry;
            entry.uri = it.key();
            entry.signature = o.value(QLatin1String("signature")).toString();
            if(entry.signature.isEmpty())
            {
              MCP_LOG_WARN("PluginManager: '" + file + "' resource '"
                           + it.key() + "' has no signature; ignoring");
              continue;
            }
            entry.resource.uri = entry.uri;
            entry.resource.name
                = o.value(QLatin1String("name")).toString(entry.uri);
            if(o.contains(QLatin1String("title")))
            {
              entry.resource.title
                  = o.value(QLatin1String("title")).toString();
            }
            if(o.contains(QLatin1String("description")))
            {
              entry.resource.description
                  = o.value(QLatin1String("description")).toString();
            }
            if(o.contains(QLatin1String("mimeType")))
            {
              entry.resource.mime_type
                  = o.value(QLatin1String("mimeType")).toString();
            }
            declared_resources.insert(entry.uri, entry);
          }
        }
      }
    }

    const QStringList deps = plugin_dependencies(loader);
    if(!deps.isEmpty() && !dependencies_satisfied(deps))
    {
      if(!quiet)
      {
        MCP_LOG_WARN("PluginManager: '" + file
                     + "' declares dependencies that are not yet satisfied; "
                       "deferring load");
      }
      delete loader;
      return LoadOutcome::UnsatisfiedDependencies;
    }

    QObject* obj = loader->instance();
    if(!obj)
    {
      MCP_LOG_WARN("PluginManager: failed to load '" + file
                   + "': " + loader->errorString());
      delete loader;
      return LoadOutcome::Failed;
    }

    auto* iface = qobject_cast<PluginInterface*>(obj);
    if(!iface)
    {
      MCP_LOG_WARN("PluginManager: '" + file
                   + "' is not an MCP plugin (interface mismatch)");
      loader->unload();
      delete loader;
      return LoadOutcome::Failed;
    }

    const QString id_q = class_info(obj->metaObject(), "id");
    if(id_q.isEmpty())
    {
      MCP_LOG_WARN("PluginManager: '" + file
                   + "' has no Q_CLASSINFO(\"id\"); rejecting");
      loader->unload();
      delete loader;
      return LoadOutcome::Failed;
    }
    const QString id = id_q;
    for(const auto& existing : m_plugins)
    {
      if(existing->id == id)
      {
        MCP_LOG_WARN("PluginManager: plugin id '" + id
                     + "' already loaded from " + existing->path
                     + "; rejecting " + file);
        loader->unload();
        delete loader;
        return LoadOutcome::Failed;
      }
    }

    auto inst = std::make_shared<Instance>();
    inst->loader = loader;
    inst->iface = iface;
    inst->object = obj;
    inst->path = file;
    inst->id = id;
    inst->dependencies = deps;

    HostInterface host;
    host.getFuncs = [this] { return functions(); };
    host.callFunc = [this](const QString& sig, const QVariantList& args) {
      return call_function(sig, args);
    };
    host.regFunc = [this, inst](const QString& sig) {
      return host_reg_func(inst, sig);
    };
    host.unloadSelf = [this, inst](const QString& reason) {
      host_unload_self(inst, reason);
    };
    host.log = [](int level, const QString& message) {
      const int clamped = std::clamp(level, 0, 5);
      log_message(static_cast<LogLevel>(clamped), message);
    };

    try
    {
      iface->initialize(host);
    }
    catch(const std::exception& e)
    {
      MCP_LOG_WARN("PluginManager: initialising '" + file
                   + "' threw: " + e.what());
      if(inst->pinned)
      {
        // It already claimed logging; its sink code must stay resident.
        m_plugins.push_back(std::move(inst));
        return LoadOutcome::Failed;
      }
      unload_instance(inst);
      return LoadOutcome::Failed;
    }

    // Keep only Tools entries that name a function this plugin actually
    // published; a stale/typo'd signature is dropped with a warning.
    for(auto it = declared_tools.constBegin(); it != declared_tools.constEnd();
        ++it)
    {
      if(!inst->signatures.contains(it.key()))
      {
        MCP_LOG_WARN("PluginManager: '" + file + "' declares a Tool for '"
                     + it.key()
                     + "', which is not a published function; ignoring");
        continue;
      }
      inst->tools.insert(it.key(), it.value());
    }

    for(auto it = declared_resources.constBegin();
        it != declared_resources.constEnd(); ++it)
    {
      if(!inst->signatures.contains(it->signature))
      {
        MCP_LOG_WARN("PluginManager: '" + file + "' declares resource '"
                     + it.key() + "' with signature '" + it->signature
                     + "', which is not a published function; ignoring");
        continue;
      }
      inst->resources.insert(it.key(), it.value());
    }

    m_plugins.push_back(std::move(inst));

    MCP_LOG_INFO("PluginManager: loaded plugin '" + m_plugins.back()->id
                 + "' (" + file + "), "
                 + QString::number(m_plugins.back()->signatures.size())
                 + " function(s) registered");
    return LoadOutcome::Loaded;
  }

  bool PluginManager::load_file(const QString& file)
  {
    return load_file_impl(file, /*quiet=*/false) == LoadOutcome::Loaded;
  }

  std::size_t PluginManager::load_directory(const QString& directory)
  {
    QDir dir(directory);
    if(!dir.exists())
    {
      MCP_LOG_WARN("PluginManager: plugin directory does not exist: "
                   + directory);
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
    for(const QFileInfo& file : files)
    {
      pending << file.absoluteFilePath();
    }

    bool progress = true;
    while(!pending.isEmpty() && progress)
    {
      progress = false;
      QStringList still;
      for(const QString& file : pending)
      {
        switch(load_file_impl(file, /*quiet=*/true))
        {
        case LoadOutcome::Loaded:
          ++loaded;
          progress = true;
          break;
        case LoadOutcome::UnsatisfiedDependencies:
          still << file;
          break;
        case LoadOutcome::Failed:
          break; // already warned by load_file_impl
        }
      }
      pending = still;
    }
    for(const QString& file : pending)
    {
      MCP_LOG_WARN("PluginManager: skipping '" + file
                   + "' — dependencies were never satisfiable");
    }
    return loaded;
  }

  bool PluginManager::unload_plugin(const QString& id)
  {
    std::shared_ptr<Instance> target;
    for(const auto& inst : m_plugins)
    {
      if(inst->id == id)
      {
        target = inst;
        break;
      }
    }
    if(!target)
    {
      MCP_LOG_WARN("PluginManager: cannot unload unknown plugin '" + id + "'");
      return false;
    }
    if(target->pinned)
    {
      MCP_LOG_WARN("PluginManager: refusing to unload pinned plugin '" + id
                   + "'");
      return false;
    }

    const QString target_id = target->id;
    for(const auto& other : m_plugins)
    {
      if(other.get() == target.get())
      {
        continue;
      }
      for(const QString& dep : other->dependencies)
      {
        bool conflict = (dep == target_id);
        if(!conflict)
        {
          for(const QString& sig : target->signatures)
          {
            if(sig == dep || sig.startsWith(dep + '('))
            {
              conflict = true;
              break;
            }
          }
        }
        if(conflict)
        {
          MCP_LOG_WARN("PluginManager: refusing to unload '" + id
                       + "': plugin '" + other->id + "' depends on '" + dep
                       + "'");
          return false;
        }
      }
    }

    unload_instance(target);
    m_plugins.erase(
        std::remove_if(m_plugins.begin(), m_plugins.end(),
                       [&target](const std::shared_ptr<Instance>& p) {
                         return p.get() == target.get();
                       }),
        m_plugins.end());
    MCP_LOG_INFO("PluginManager: unloaded plugin '" + id + "'");
    return true;
  }

  void PluginManager::unload_all(void)
  {
    for(auto it = m_plugins.begin(); it != m_plugins.end();)
    {
      if((*it)->pinned)
      {
        ++it;
        continue;
      }
      unload_instance(*it);
      it = m_plugins.erase(it);
    }
  }

  std::size_t PluginManager::plugin_count(void) const
  {
    return static_cast<std::size_t>(m_plugins.size());
  }

  QStringList PluginManager::plugin_ids(void) const
  {
    QStringList ids;
    for(const auto& inst : m_plugins)
    {
      ids << inst->id;
    }
    return ids;
  }

  QObject* PluginManager::plugin_object(const QString& id) const
  {
    for(const auto& inst : m_plugins)
    {
      if(inst->id == id)
        return inst->object;
    }
    return nullptr;
  }

  QStringList PluginManager::functions(void) const
  {
    QStringList sigs = m_registry.keys();
    sigs.sort();
    return sigs;
  }

  bool PluginManager::has_function(const QString& signature) const
  {
    return m_registry.contains(signature);
  }

  QList<PluginTool> PluginManager::tools() const
  {
    QList<PluginTool> out;
    for(const auto& inst : m_plugins)
    {
      for(auto it = inst->tools.constBegin(); it != inst->tools.constEnd();
          ++it)
      {
        out.push_back(PluginTool{ it.key(), it.value() });
      }
    }
    std::sort(out.begin(), out.end(),
              [](const PluginTool& a, const PluginTool& b) {
                return a.signature < b.signature;
              });
    return out;
  }

  std::optional<Tool> PluginManager::tool(const QString& signature) const
  {
    for(const auto& inst : m_plugins)
    {
      const auto it = inst->tools.constFind(signature);
      if(it != inst->tools.constEnd())
      {
        return it.value();
      }
    }
    return std::nullopt;
  }

  QList<PluginResource> PluginManager::resources(void) const
  {
    QList<PluginResource> out;
    for(const auto& inst : m_plugins)
    {
      for(auto it = inst->resources.constBegin();
          it != inst->resources.constEnd(); ++it)
      {
        out.push_back(it.value());
      }
    }
    std::sort(out.begin(), out.end(),
              [](const PluginResource& a, const PluginResource& b) {
                return a.uri < b.uri;
              });
    return out;
  }

  std::optional<PluginResource>
  PluginManager::resource(const QString& uri) const
  {
    for(const auto& inst : m_plugins)
    {
      const auto it = inst->resources.constFind(uri);
      if(it != inst->resources.constEnd())
      {
        return it.value();
      }
    }
    return std::nullopt;
  }

  std::size_t PluginManager::register_tools(Server& server) const
  {
    std::size_t registered = 0;

    for(const auto& inst : m_plugins)
    {
      for(auto it = inst->tools.constBegin(); it != inst->tools.constEnd();
          ++it)
      {
        const QString signature = it.key();
        const Tool tool = it.value();
        const auto entry = m_registry.constFind(signature);
        if(entry == m_registry.constEnd() || entry->owner.expired())
        {
          continue;
        }
        const QMetaMethod method = entry->method;
        const QList<QByteArray> parameter_names = method.parameterNames();

        auto handler = [this, &server, signature, method, parameter_names](
                           const QJsonValue& args) -> CallToolResult {
          const QJsonObject object
              = args.isObject() ? args.toObject() : QJsonObject{};

          QVariantList positional;
          positional.reserve(method.parameterCount());
          for(int i = 0; i < method.parameterCount(); ++i)
          {
            const QMetaType type = method.parameterMetaType(i);
            const QString name = QString::fromLatin1(parameter_names.value(i));
            QVariant value;
            if(!name.isEmpty() && object.contains(name))
            {
              value = object.value(name).toVariant();
            }
            if(!value.isValid())
            {
              value = QVariant(type);
            }
            if(value.metaType() != type && !value.convert(type))
            {
              return error_result("argument '" + name + "' for " + signature
                                  + " cannot be converted to "
                                  + QString(type.name()));
            }
            positional.append(value);
          }

          try
          {
            CurrentServerGuard guard(&server);
            return variant_to_tool_result(
                call_function(signature, positional));
          }
          catch(const std::exception& e)
          {
            return error_result(e.what());
          }
        };

        server.tool(tool.name, tool.input_schema, handler, tool.title,
                    tool.description, tool.annotations, tool.output_schema);
        ++registered;
      }

      for(auto it = inst->resources.constBegin();
          it != inst->resources.constEnd(); ++it)
      {
        const PluginResource resource = it.value();
        const auto entry = m_registry.constFind(resource.signature);
        if(entry == m_registry.constEnd() || entry->owner.expired())
        {
          continue;
        }
        const QMetaMethod method = entry->method;
        if(method.parameterCount() > 1)
        {
          MCP_LOG_WARN("PluginManager: resource '" + resource.uri
                       + "' reader takes more than one parameter; skipping");
          continue;
        }
        if(method.parameterCount() == 1
           && method.parameterMetaType(0) != QMetaType::fromType<QString>())
        {
          MCP_LOG_WARN("PluginManager: resource '" + resource.uri
                       + "' reader parameter must be QString; skipping");
          continue;
        }

        auto handler = [this, &server, resource,
                        method](const QString& uri) -> ReadResourceResult {
          QVariantList args;
          if(method.parameterCount() == 1)
          {
            args.append(uri);
          }
          QString text;
          try
          {
            CurrentServerGuard guard(&server);
            const QVariant out = call_function(resource.signature, args);
            if(out.metaType() == QMetaType::fromType<QString>())
            {
              text = out.toString();
            }
            else
            {
              text = mcp::dump(QJsonValue::fromVariant(out));
            }
          }
          catch(const std::exception& e)
          {
            text = QString::fromUtf8(e.what());
          }
          ReadResourceResult result;
          result.contents.push_back(
              TextResourceContents{ uri, resource.resource.mime_type, text });
          return result;
        };

        server.resource(resource.resource, handler);
        ++registered;
      }
    }

    return registered;
  }

} // namespace mcp
