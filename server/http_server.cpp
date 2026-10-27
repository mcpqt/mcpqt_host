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

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QString>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <csignal>
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
  QCoreApplication::setApplicationName("mcp_http_server");
  QCoreApplication::setApplicationVersion("0.1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription("Streamable HTTP MCP server. Loads plugins and serves MCP over HTTP.");
  parser.addHelpOption();
  parser.addVersionOption();

  const QCommandLineOption host_option("host", "Bind address (default: 127.0.0.1).", "H", "127.0.0.1");
  const QCommandLineOption port_option("port", "Bind port, 0 for OS-assigned (default: 8848).", "P", "8848");
  const QCommandLineOption path_option("path", "MCP endpoint path (default: /mcp).", "PATH", "/mcp");

  parser.addOption(host_option);
  parser.addOption(port_option);
  parser.addOption(path_option);
  parser.addPositionalArgument(
      "plugin-path",
      "Plugin file or directory to load. Defaults to "
      "$MCP_PLUGIN_DIR or 'plugins'.",
      "[plugin-path ...]");

  parser.process(app);

  const QString host = parser.value(host_option);
  bool port_ok = false;
  const int port = parser.value(port_option).toInt(&port_ok);
  if(!port_ok)
  {
    qCritical().noquote()
        << "[mcp_http_server] invalid --port value:"
        << parser.value(port_option);
    return 2;
  }
  const QString path = parser.value(path_option);
  QStringList plugin_paths = parser.positionalArguments();
  if(plugin_paths.isEmpty())
  {
    plugin_paths << qEnvironmentVariable("MCP_PLUGIN_DIR", "plugins");
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

  if(QString token = qEnvironmentVariable("MCP_TOKEN"); !token.isNull() && !token.isEmpty())
  {
    opts.bearer_validator
        = [expected = token](QStringView presented) {
            using Outcome = mcp::HttpServerHost::Options::BearerOutcome;
            using Status = mcp::HttpServerHost::Options::BearerStatus;
            Outcome out;
            out.status = presented == expected ? Status::allow
                                               : Status::invalid_token;
            return out;
          };
    qInfo().noquote() << "[mcp_http_server] bearer auth ON";
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
  std::signal(SIGQUIT, on_signal);

  server_host.start();
  qInfo().noquote() << QString(
                           "[mcp_http_server] listening on http://%1:%2%3")
                           .arg(host)
                           .arg(server_host.port())
                           .arg(path);

  // Run the main-thread Qt event loop. Plugins such as webio marshal work
  // (network operations) onto the thread that created them; that thread is
  // this one, so it must process events rather than sleep.
  QTimer shutdown_timer;
  QObject::connect(&shutdown_timer, &QTimer::timeout, &app, [&app]() {
    if(g_shutdown_requested.test(std::memory_order_acquire))
    {
      app.quit();
    }
  });
  shutdown_timer.start(200);
  const int event_loop_status = app.exec();
  (void)event_loop_status;

  qInfo().noquote() << QString("[mcp_http_server] shutting down (%1 sessions)")
                           .arg(static_cast<qulonglong>(server_host.active_sessions()));
  server_host.stop();
  return 0;
}
