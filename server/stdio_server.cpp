// SPDX-License-Identifier: AGPL-3.0-only
//
// mcp_stdio_server: the project's stdio MCP server.
//
// Loads one or more plugins (files or directories), registers their declared
// tools and resources on a Server, and serves MCP over stdin/stdout.
//
//   mcp_stdio_server [plugin-path ...]
//
// With no paths it uses $MCP_PLUGIN_DIR, falling back to ./plugins.

#include "mcp.h"
#include "plugin_manager.h"

#include <QFileInfo>
#include <QString>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<mcp::Server*> g_server{nullptr};
std::atomic_flag g_shutdown_requested = ATOMIC_FLAG_INIT;

extern "C" void on_signal(int) {
    g_shutdown_requested.test_and_set(std::memory_order_release);
}

void load_path(mcp::PluginManager& plugins, const QString& path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (QFileInfo(absolute).isDir()) {
        const std::size_t n = plugins.load_directory(absolute);
        MCP_LOG_INFO("mcp_stdio_server: loaded " + std::to_string(n) +
                     " plugin(s) from " + absolute.toStdString());
    } else {
        if (plugins.load_file(absolute)) {
            MCP_LOG_INFO("mcp_stdio_server: loaded plugin " +
                         absolute.toStdString());
        } else {
            MCP_LOG_WARN("mcp_stdio_server: failed to load " +
                         absolute.toStdString());
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    mcp::set_log_level(mcp::LogLevel::info);

    QStringList paths;
    for (int i = 1; i < argc; ++i) {
        paths << QString::fromLocal8Bit(argv[i]);
    }
    if (paths.isEmpty()) {
        paths << qEnvironmentVariable("MCP_PLUGIN_DIR",
                                      QStringLiteral("plugins"));
    }

    // The manager must outlive the Server: tool handlers reference it.
    mcp::PluginManager plugins;
    for (const QString& path : paths) {
        load_path(plugins, path);
    }

    mcp::Server server{mcp::Implementation{
        .name = "mcp-server",
        .version = "0.1.0",
        .description = "MCP server for dynamically loaded plugins.",
    }};
    server.set_instructions("Tools and resources on this server are "
                            "contributed by loaded plugins.");

    const std::size_t registered = plugins.register_tools(server);
    MCP_LOG_INFO("mcp_stdio_server: registered " + std::to_string(registered) +
                 " tool/resource entr(ies)");

    g_server.store(&server, std::memory_order_release);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::atomic<bool> supervisor_done{false};
    std::thread supervisor([&supervisor_done] {
        while (!g_shutdown_requested.test(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (auto* s = g_server.load(std::memory_order_acquire)) {
            s->stop();
        }
        supervisor_done.store(true, std::memory_order_release);
    });

    server.run(std::make_unique<mcp::StdioTransport>());

    g_shutdown_requested.test_and_set(std::memory_order_release);
    if (supervisor.joinable()) {
        supervisor.join();
    }
    MCP_LOG_INFO("mcp_stdio_server: stopped");
    return EXIT_SUCCESS;
}