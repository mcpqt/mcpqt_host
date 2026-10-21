// SPDX-License-Identifier: AGPL-3.0-only
//
// Example plugin host. It loads every plugin in a directory (argv[1], or
// $MCP_PLUGIN_DIR, or ./plugins), reflects each plugin's public slots into the
// cross-plugin function registry, and prints the registered signatures.
//
//   ./plugin_host /path/to/plugins
//
// Building examples/hello_plugin and examples/cross_caller produces
// hello_plugin.so and cross_caller.so; point the host at the directory
// containing them to try it out. Loading both demonstrates cross-plugin
// calls: CrossCallerPlugin::greet_via_host forwards to HelloPlugin::greet.
//
// NOTE: functions are currently exposed only to other plugins, not as MCP
// tools; wiring the registry to a Server's tool list is a later step.

#include "mcp.h"

#include <QString>

#include <cstddef>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
  mcp::set_log_level(mcp::LogLevel::info);

  const QString directory
      = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                   : qEnvironmentVariable("MCP_PLUGIN_DIR",
                                          QStringLiteral("plugins"));

  mcp::PluginManager plugins;

  const std::size_t loaded = plugins.load_directory(directory);
  std::cerr << "Loaded " << loaded << " plugin(s) from "
            << directory.toStdString() << "\n";
  for(const QString& signature : plugins.functions())
  {
    std::cerr << "  function: " << signature.toStdString() << "\n";
  }

  plugins.unload_all();
  return EXIT_SUCCESS;
}