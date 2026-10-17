// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginManager implementation: QPluginLoader discovery + command → tool
// registration.

#include "plugin_manager.hpp"

#include "error.hpp"
#include "log.hpp"
#include "server.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPluginLoader>

#include <exception>
#include <optional>
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

}  // namespace

struct PluginManager::Instance {
    QPluginLoader*             loader  = nullptr;
    PluginInterface*           iface   = nullptr;
    QString                    path;
    std::string                id;
    std::vector<PluginCommand> commands;
};

PluginManager::PluginManager(Server& server) : server_(server) {}

PluginManager::~PluginManager() { unload_all(); }

std::size_t PluginManager::register_commands(const std::shared_ptr<Instance>& inst) {
    std::size_t             registered = 0;
    std::weak_ptr<Instance> weak       = inst;

    for (std::size_t i = 0; i < inst->commands.size(); ++i) {
        const PluginCommand& cmd = inst->commands[i];

        if (cmd.name.empty()) {
            MCP_LOG_WARN("PluginManager: plugin '" + inst->id +
                         "' declared a command with an empty name; skipping");
            continue;
        }
        if (has_tool(cmd.name)) {
            MCP_LOG_WARN("PluginManager: tool '" + cmd.name +
                         "' is already registered; skipping duplicate from plugin '" +
                         inst->id + "'");
            continue;
        }

        const Json   schema = cmd.input_schema.isObject() ? cmd.input_schema : permissive_schema();
        const std::string name = cmd.name;
        const std::optional<std::string> title =
            cmd.title.empty() ? std::nullopt : std::optional<std::string>(cmd.title);
        const std::optional<std::string> description =
            cmd.description.empty() ? std::nullopt : std::optional<std::string>(cmd.description);

        server_.tool(
            name, schema,
            [weak, i, name](const Json& args) -> CallToolResult {
                const auto plugin = weak.lock();
                if (!plugin || i >= plugin->commands.size()) {
                    // The owning plugin was unloaded while the tool stayed
                    // registered; fail softly instead of calling detached code.
                    return error_result("plugin command '" + name +
                                        "' is no longer available");
                }
                try {
                    return plugin->commands[i].handler(args);
                } catch (const Error&) {
                    throw;  // protocol-level error, propagate to the client
                } catch (const std::exception& e) {
                    return error_result("plugin command '" + name + "' failed: " + e.what());
                }
            },
            title, description);

        tools_.push_back(name);
        ++registered;
    }
    return registered;
}

bool PluginManager::load_file(const QString& file) {
    auto* loader = new QPluginLoader(file);

    QObject* obj = loader->instance();
    if (!obj) {
        MCP_LOG_WARN("PluginManager: failed to load '" + file.toStdString() +
                     "': " + loader->errorString().toStdString());
        delete loader;
        return false;
    }

    auto* iface = qobject_cast<PluginInterface*>(obj);
    if (!iface) {
        MCP_LOG_WARN("PluginManager: '" + file.toStdString() +
                     "' is not an MCP plugin (interface mismatch)");
        loader->unload();
        delete loader;
        return false;
    }

    auto inst = std::make_shared<Instance>();
    inst->loader = loader;
    inst->iface  = iface;
    inst->path   = file;
    inst->id     = iface->id();

    try {
        iface->initialize();
        inst->commands = iface->commands();
    } catch (const std::exception& e) {
        MCP_LOG_WARN("PluginManager: initialising '" + file.toStdString() +
                     "' threw: " + e.what());
        loader->unload();
        delete loader;
        return false;
    }

    const std::size_t registered = register_commands(inst);
    plugins_.push_back(std::move(inst));

    MCP_LOG_INFO("PluginManager: loaded plugin '" + plugins_.back()->id + "' (" +
                 file.toStdString() + "), " + std::to_string(registered) +
                 " tool(s) registered");
    return true;
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
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo& file : files) {
        if (load_file(file.absoluteFilePath())) {
            ++loaded;
        }
    }
    return loaded;
}

void PluginManager::unload_all() {
    for (auto& inst : plugins_) {
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
        inst->iface = nullptr;
    }
    plugins_.clear();
    tools_.clear();
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

}  // namespace mcp
