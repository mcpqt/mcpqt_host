// SPDX-License-Identifier: AGPL-3.0-only
//
// plugintester: an interactive, standalone plugin loader.
//
// Scans a plugin directory and lets you load/unload plugins by name while
// showing what is loaded. Loading goes through mcp::PluginManager, so the
// dependency system applies: a plugin whose declared dependencies are not yet
// available fails to load, and unloading a plugin that another loaded plugin
// depends on is refused.
//
// Logging: this tool does NOT use a logging plugin. It claims the process-wide
// sink at startup with a plain stderr dumper, so every SDK log request goes to
// stderr and no plugin can take logging over.
//
//   ./plugintester [/path/to/plugins]
//
// Commands (also see `help`):
//   list                 list shared libraries found in the plugin directory
//   load <name|path>     load one plugin (dependency-aware)
//   loadall              load every plugin in the directory (dependency-aware)
//   unload <plugin-id>   unload one plugin (respects reverse dependencies)
//   loaded               show loaded plugin ids and their tools
//   help                 show this help
//   quit                 exit

#include "mcp.h"
#include "plugin_manager.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

  QStringList library_filters(void)
  {
    QStringList filters;
#if defined(Q_OS_WIN)
    filters << "*.dll";
#elif defined(Q_OS_MAC)
    filters << "*.dylib" << "*.so";
#else
    filters << "*.so";
#endif
    return filters;
  }

  QStringList discover(const QString& dir)
  {
    QDir d(dir);
    if(!d.exists())
    {
      return {};
    }
    QStringList names;
    const QFileInfoList files
        = d.entryInfoList(library_filters(), QDir::Files);
    for(const QFileInfo& f : files)
    {
      names << f.fileName();
    }
    names.sort();
    return names;
  }

  /// Resolve a user-supplied name to a plugin file in `dir`.
  QString resolve_plugin_path(const QString& dir, const QString& name)
  {
    if(name.isEmpty())
    {
      return {};
    }
    // Treat anything with a path separator or a library suffix as a path.
    if(name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
       || name.endsWith(QLatin1String(".so"))
       || name.endsWith(QLatin1String(".dll"))
       || name.endsWith(QLatin1String(".dylib")))
    {
      return name;
    }
    const QDir d(dir);
    for(const QString& filter : library_filters())
    {
      QString ext = filter;
      ext.remove(QLatin1Char('*')); // "*.so" -> ".so"
      const QString candidate = d.filePath(name + ext);
      if(QFileInfo::exists(candidate))
      {
        return candidate;
      }
    }
    return {};
  }

  void print_help(void)
  {
    std::cout
        << "commands:\n"
        << "  list                 list shared libraries in the plugin "
           "directory\n"
        << "  load <name|path>     load one plugin (dependency-aware)\n"
        << "  loadall              load every plugin in the directory\n"
        << "  unload <plugin-id>   unload one plugin (respects reverse deps)\n"
        << "  loaded               show loaded plugin ids and functions\n"
        << "  help                 show this help\n"
        << "  quit                 exit\n";
  }

  void print_loaded(const mcp::PluginManager& plugins)
  {
    const QStringList ids = plugins.plugin_ids();
    if(ids.isEmpty())
    {
      std::cout << "(nothing loaded)\n";
      return;
    }
    std::cout << "loaded plugins: "
              << ids.join(", ").toStdString() << "\n";
    std::cout << "functions:\n";
    for(const QString& signature : plugins.functions())
    {
      std::cout << "  " << signature.toStdString() << "\n";
    }
  }

  QString plugin_directory(int argc, char** argv)
  {
    if(argc > 1)
    {
      return QString::fromLocal8Bit(argv[1]);
    }
    return qEnvironmentVariable("MCP_PLUGIN_DIR", QStringLiteral("plugins"));
  }

} // namespace

int main(int argc, char** argv)
{
  // All SDK diagnostics go to stderr, and a plugin must not be able to take
  // logging over. Claiming the sink here (before any plugin loads) does both.
  const bool claimed
      = mcp::claim_log_sink([](mcp::LogLevel level, const QString& msg) {
          const QByteArray level_text = mcp::to_string(level).toUtf8();
          const QByteArray text = msg.toUtf8();
          std::fprintf(stderr, "[%-5s] %.*s\n", level_text.constData(),
                       static_cast<int>(text.size()), text.constData());
          std::fflush(stderr);
        });
  if(!claimed)
  {
    std::fprintf(stderr, "warning: process log sink was already claimed\n");
  }

  mcp::PluginManager plugins;

  const QString dir = plugin_directory(argc, argv);
  std::cout << "plugintester — plugin directory: " << dir.toStdString()
            << "\n";
  print_help();

  std::string line;
  while(true)
  {
    std::cout << "plugintester> " << std::flush;
    if(!std::getline(std::cin, line))
    {
      break;
    }

    std::istringstream iss(line);
    std::string command;
    iss >> command;
    if(command.empty())
    {
      continue;
    }

    if(command == "quit" || command == "exit")
    {
      break;
    }
    else if(command == "help" || command == "?")
    {
      print_help();
    }
    else if(command == "list" || command == "ls")
    {
      const QStringList files = discover(dir);
      if(files.isEmpty())
      {
        std::cout << "(no plugins found in " << dir.toStdString() << ")\n";
      }
      else
      {
        for(const QString& f : files)
        {
          std::cout << "  " << f.toStdString() << "\n";
        }
      }
    }
    else if(command == "loaded")
    {
      print_loaded(plugins);
    }
    else if(command == "load")
    {
      std::string name;
      iss >> name;
      const QString path
          = resolve_plugin_path(dir, QString::fromStdString(name));
      if(path.isEmpty())
      {
        std::cout << "not found: " << name << "\n";
      }
      else if(plugins.load_file(path))
      {
        std::cout << "loaded: " << name << "\n";
      }
      else
      {
        std::cout << "load failed: " << name
                  << " (see stderr; dependencies may be unmet)\n";
      }
    }
    else if(command == "loadall")
    {
      const std::size_t n = plugins.load_directory(dir);
      std::cout << "loaded " << n << " plugin(s) from " << dir.toStdString()
                << "\n";
    }
    else if(command == "unload")
    {
      std::string id_str;
      iss >> id_str;
      const QString id = QString::fromStdString(id_str);
      if(id.isEmpty())
      {
        std::cout << "usage: unload <plugin-id>\n";
      }
      else if(plugins.unload_plugin(id))
      {
        std::cout << "unloaded: " << id.toStdString() << "\n";
      }
      else
      {
        std::cout << "unload refused: " << id.toStdString()
                  << " (unknown, pinned, or required by another plugin)\n";
      }
    }
    else
    {
      std::cout << "unknown command '" << command << "'; type help\n";
    }
  }

  plugins.unload_all();
  std::cout << "bye\n";
  return 0;
}
