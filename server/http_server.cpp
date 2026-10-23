// SPDX-License-Identifier: AGPL-3.0-only
//
// mcp_http_server: the project's Streamable HTTP MCP server.
//
// Loads one or more plugins (files or directories), registers their declared
// tools and resources on each session's Server, and serves MCP over HTTP.
//
//   mcp_http_server [--host H] [--port P] [--path PATH] [plugin-path ...]
//
// Defaults: 127.0.0.1:8848 /mcp. Set MCP_TOKEN to require
// `Authorization: Bearer <token>` on every request.

#include "http_server_host.h"
#include "mcp.h"
#include "plugin_manager.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{

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
      (void)plugins.load_directory(path);
    }
    else
    {
      (void)plugins.load_file(path);
    }
  }

} // namespace

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  mcp::set_log_level(mcp::LogLevel::info);

  QString host = "127.0.0.1";
  int port = 8848;
  QString path = "/mcp";
  QStringList plugin_paths;

  for(int i = 1; i < argc; ++i)
  {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if(arg == "--host" && i + 1 < argc)
    {
      host = QString::fromLocal8Bit(argv[++i]);
    }
    else if(arg == "--port" && i + 1 < argc)
    {
      port = std::atoi(argv[++i]);
    }
    else if(arg == "--path" && i + 1 < argc)
    {
      path = QString::fromLocal8Bit(argv[++i]);
    }
    else
    {
      plugin_paths << QString::fromLocal8Bit(argv[i]);
    }
  }
  if(plugin_paths.isEmpty())
  {
    plugin_paths << qEnvironmentVariable("MCP_PLUGIN_DIR",
                                         QStringLiteral("plugins"));
  }

  // The manager must outlive the host: per-session handlers reference it.
  mcp::PluginManager plugins;
  for(const QString& plugin_path : plugin_paths)
  {
    load_path(plugins, plugin_path);
  }

  mcp::HttpServerHost::Options opts;
  opts.host = host;
  opts.port = port;
  opts.path = path;

  if(const char* token = std::getenv("MCP_TOKEN"); token && *token)
  {
    opts.bearer_validator
        = [expected = QString::fromLocal8Bit(token)](QStringView presented) {
            using Outcome = mcp::HttpServerHost::Options::BearerOutcome;
            using Status = mcp::HttpServerHost::Options::BearerStatus;
            Outcome out;
            out.status = presented == expected ? Status::allow
                                               : Status::invalid_token;
            return out;
          };
    std::fprintf(stderr, "[mcp_http_server] bearer auth ON\n");
  }

  mcp::HttpServerHost server_host{
    mcp::Implementation{
        .name = "mcp-server",
        .version = "0.1.0",
        .description = "MCP server for dynamically loaded plugins.",
    },
    opts,
    [&plugins](mcp::Server& server) {
      plugins.register_tools(server);
      plugins.register_control_tool(server);
    },
  };

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  server_host.start();
  std::fprintf(stderr, "[mcp_http_server] listening on http://%s:%d%s\n",
               host.toUtf8().constData(), server_host.port(),
               path.toUtf8().constData());

  while(!g_shutdown_requested.test(std::memory_order_acquire))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{ 200 });
  }

  std::fprintf(stderr, "[mcp_http_server] shutting down (%zu sessions)\n",
               server_host.active_sessions());
  server_host.stop();
  return 0;
}
