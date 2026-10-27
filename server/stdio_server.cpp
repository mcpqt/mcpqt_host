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

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QString>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

namespace
{

  std::atomic<mcp::Server*> g_server{ nullptr };
  std::atomic_flag g_shutdown_requested = ATOMIC_FLAG_INIT;

  extern "C" void on_signal(int)
  {
    g_shutdown_requested.test_and_set(std::memory_order_release);
  }

  void load_path(mcp::PluginManager& plugins, const QString& path)
  {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if(QFileInfo(absolute).isDir())
    {
      const std::size_t n = plugins.load_directory(absolute);
      qInfo().noquote() << QString("mcp_stdio_server: loaded %1 plugin(s) from %2")
                               .arg(static_cast<qulonglong>(n))
                               .arg(absolute);
    }
    else
    {
      if(plugins.load_file(absolute))
      {
        qInfo().noquote() << QString("mcp_stdio_server: loaded plugin %1")
                                 .arg(absolute);
      }
      else
      {
        qWarning().noquote() << QString("mcp_stdio_server: failed to load %1")
                                    .arg(absolute);
      }
    }
  }

} // namespace

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("mcp_stdio_server");
  QCoreApplication::setApplicationVersion("0.1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription(
      "Stdio MCP server. Loads plugins and serves MCP over stdin/stdout.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(
      "plugin-path",
      "Plugin file or directory to load. Defaults to "
      "$MCP_PLUGIN_DIR or 'plugins'.",
      "[plugin-path ...]");

  parser.process(app);

  QStringList paths = parser.positionalArguments();
  if(paths.isEmpty())
  {
    paths << qEnvironmentVariable("MCP_PLUGIN_DIR", "plugins");
  }

  // The manager must outlive the Server: tool handlers reference it.
  mcp::PluginManager plugins;
  for(const QString& path : paths)
  {
    load_path(plugins, path);
  }

  mcp::Server server{ mcp::Implementation{
      .name = "mcp-server",
      .version = "0.1.0",
      .description = "MCP server for dynamically loaded plugins.",
  } };
  server.set_instructions("Tools and resources on this server are "
                          "contributed by loaded plugins.");

  const std::size_t registered = plugins.register_tools(server);
  plugins.register_control_tool(server);
  qInfo().noquote() << QString(
                           "mcp_stdio_server: registered tool/resource entries: %1")
                           .arg(static_cast<qulonglong>(registered));

  g_server.store(&server, std::memory_order_release);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGQUIT, on_signal);

  std::atomic<bool> supervisor_done{ false };
  std::thread supervisor([&supervisor_done] {
    while(!g_shutdown_requested.test(std::memory_order_acquire))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
    }
    if(auto* s = g_server.load(std::memory_order_acquire))
    {
      s->stop();
    }
    supervisor_done.store(true, std::memory_order_release);
  });

  server.run(std::make_unique<mcp::StdioTransport>());

  g_shutdown_requested.test_and_set(std::memory_order_release);
  if(supervisor.joinable())
  {
    supervisor.join();
  }
  qInfo().noquote() << "mcp_stdio_server: stopped";
  return 0;
}
