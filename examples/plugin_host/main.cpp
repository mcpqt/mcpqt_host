// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Example plugin host: an MCP server whose tools come from plugins.
//
// It loads every plugin in a directory (argv[1], or $MCP_PLUGIN_DIR, or
// ./plugins), registers each plugin command as a tool, then serves over stdio.
//
//   ./plugin_host /path/to/plugins
//
// Building examples/hello_plugin produces hello_plugin.so; point the host at
// the directory containing it to try it out.

#include "mcp/mcp.hpp"

#include <QString>

#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    mcp::set_log_level(mcp::LogLevel::info);

    mcp::Server server{ mcp::Implementation{
        .name        = "mcp-plugin-host",
        .version     = "0.1.0",
        .description = "Hosts MCP tools loaded from plugins.",
    } };
    server.set_instructions(
        "Tools on this server are contributed by dynamically loaded plugins.");

    const QString directory =
        (argc > 1)
            ? QString::fromLocal8Bit(argv[1])
            : qEnvironmentVariable("MCP_PLUGIN_DIR", QStringLiteral("plugins"));

    // Declared after `server`, so it is destroyed (and unloads plugins) before
    // the server; a plugin's registered handlers then never outlive their code.
    mcp::PluginManager plugins{ server };

    const std::size_t loaded = plugins.load_directory(directory);
    std::cerr << "Loaded " << loaded << " plugin(s) from "
              << directory.toStdString() << "\n";
    for (const std::string& tool : plugins.registered_tools()) {
        std::cerr << "  tool: " << tool << "\n";
    }

    server.run(std::make_unique<mcp::StdioTransport>());
    return EXIT_SUCCESS;
}
