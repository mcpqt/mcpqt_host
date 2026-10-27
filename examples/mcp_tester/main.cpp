// SPDX-License-Identifier: AGPL-3.0-only
//
// mcp_tester: an interactive MCP client built entirely on Qt.
//
// Connects to an MCP server over either Streamable HTTP or stdio (by
// spawning a child process with QProcess), initializes the session, and
// presents a command menu for inspecting and calling the server's tools.
//
//   mcp_tester --http http://127.0.0.1:8848/mcp
//   mcp_tester --stdio ./mcp_stdio_server -- ./plugins
//
// See `mcp_tester --help` for the complete option list.

#include "mcp.h"

#if defined(MCP_ENABLE_HTTP)
#include "http_client_transport.h"
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace
{

  using OutStream = QTextStream;
  using InStream = QTextStream;

  // -------------------------------------------------------------------------
  // QProcess-backed stdio transport
  //
  // QProcess lives on the application's main thread and is driven by the Qt
  // event loop. mcp::Client may call send()/close() from the interactive
  // worker thread, so those operations are marshalled onto the main thread.
  // Inbound frames are delivered from the main thread, which is safe for
  // Session's callback contract.
  // -------------------------------------------------------------------------

  class ProcessTransport final : public QObject, public mcp::Transport
  {
  public:
    struct Options
    {
      QString program;
      QStringList arguments;
    };

    explicit ProcessTransport(Options opts, QObject* parent = nullptr)
        : QObject(parent), m_opts(std::move(opts))
    {
    }

    ~ProcessTransport() override { close(); }

    void on_message(MessageCallback cb) override
    {
      m_on_message = std::move(cb);
    }

    void on_error(ErrorCallback cb) override { m_on_error = std::move(cb); }

    void on_close(CloseCallback cb) override { m_on_close = std::move(cb); }

    void start(void) override
    {
      bool expected = false;
      if(!m_started.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel))
      {
        return;
      }
      run_on_owner([this] { do_start(); });
    }

    void close(void) override
    {
      if(m_closed.load(std::memory_order_acquire))
      {
        return;
      }
      run_on_owner([this] { do_close(); });
    }

    [[nodiscard]] std::error_code send(QByteArrayView frame) override
    {
      if(!is_open())
      {
        return std::make_error_code(std::errc::not_connected);
      }
      auto data = QByteArray(frame.data(), static_cast<qsizetype>(frame.size()));
      QMetaObject::invokeMethod(
          this, [this, data = std::move(data)] { do_send(data); },
          Qt::QueuedConnection);
      return {};
    }

    [[nodiscard]] bool is_open(void) const noexcept override
    {
      return m_open.load(std::memory_order_acquire)
             && !m_closed.load(std::memory_order_acquire);
    }

  private:
    bool on_owner_thread(void) const
    {
      return QThread::currentThread() == thread();
    }

    template<typename Fn>
    void run_on_owner(Fn&& fn)
    {
      if(on_owner_thread())
      {
        fn();
      }
      else
      {
        QMetaObject::invokeMethod(this, std::forward<Fn>(fn),
                                  Qt::BlockingQueuedConnection);
      }
    }

    void do_start(void)
    {
      m_proc = new QProcess(this);
      m_proc->setProgram(m_opts.program);
      m_proc->setArguments(m_opts.arguments);
      m_proc->setProcessChannelMode(QProcess::SeparateChannels);

      connect(m_proc, &QProcess::readyReadStandardOutput, this,
              [this] { do_read_stdout(); });
      connect(m_proc, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray data = m_proc->readAllStandardError();
        if(data.isEmpty())
        {
          return;
        }
        QTextStream err(stderr);
        err << "[server stderr] " << QString::fromUtf8(data);
        if(!data.endsWith('\n'))
        {
          err << '\n';
        }
        err.flush();
      });
      connect(m_proc, &QProcess::errorOccurred, this,
              [this](QProcess::ProcessError error) {
                if(error == QProcess::FailedToStart)
                {
                  m_open.store(false, std::memory_order_release);
                  report_error(std::make_error_code(std::errc::io_error));
                  fire_close();
                }
              });
      connect(m_proc, &QProcess::finished, this,
              [this](int, QProcess::ExitStatus) {
                do_read_stdout();
                m_open.store(false, std::memory_order_release);
                fire_close();
              });

      m_proc->start();
      if(!m_proc->waitForStarted(10'000))
      {
        m_open.store(false, std::memory_order_release);
        report_error(std::make_error_code(std::errc::io_error));
        fire_close();
        return;
      }
      m_open.store(true, std::memory_order_release);
    }

    void do_send(const QByteArray& data)
    {
      if(!m_proc || m_proc->state() != QProcess::Running)
      {
        report_error(std::make_error_code(std::errc::not_connected));
        return;
      }
      QByteArray line = data;
      line.append('\n');
      const qint64 written = m_proc->write(line);
      if(written != line.size())
      {
        report_error(std::make_error_code(std::errc::io_error));
      }
    }

    void do_read_stdout(void)
    {
      if(!m_proc)
      {
        return;
      }
      m_rx.append(m_proc->readAllStandardOutput());
      while(true)
      {
        const qsizetype newline = m_rx.indexOf('\n');
        if(newline < 0)
        {
          break;
        }
        QByteArray line = m_rx.left(newline);
        m_rx.remove(0, newline + 1);
        if(line.endsWith('\r'))
        {
          line.chop(1);
        }
        if(line.isEmpty())
        {
          continue;
        }
        if(m_on_message)
        {
          m_on_message(std::move(line));
        }
      }
    }

    void do_close(void)
    {
      if(m_closed.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }
      m_open.store(false, std::memory_order_release);
      if(m_proc && m_proc->state() != QProcess::NotRunning)
      {
        m_proc->terminate();
        if(!m_proc->waitForFinished(2'000))
        {
          m_proc->kill();
          (void)m_proc->waitForFinished(1'000);
        }
      }
      fire_close();
    }

    void report_error(std::error_code ec) noexcept
    {
      if(m_on_error)
      {
        m_on_error(ec);
      }
    }

    void fire_close(void) noexcept
    {
      if(m_close_fired.exchange(true, std::memory_order_acq_rel))
      {
        return;
      }
      if(m_on_close)
      {
        m_on_close();
      }
    }

    Options m_opts;
    QProcess* m_proc = nullptr;
    QByteArray m_rx;

    std::atomic<bool> m_started = false;
    std::atomic<bool> m_open = false;
    std::atomic<bool> m_closed = false;
    std::atomic<bool> m_close_fired = false;

    MessageCallback m_on_message;
    ErrorCallback m_on_error;
    CloseCallback m_on_close;
  };

  // -------------------------------------------------------------------------
  // Output helpers
  // -------------------------------------------------------------------------

  void print_help(OutStream& out)
  {
    out << "commands:\n"
        << "  list, tools, ls   list the server's tools\n"
        << "  call              call a tool with JSON arguments\n"
        << "  info              show server info and instructions\n"
        << "  ping              send a ping\n"
        << "  help, ?           show this help\n"
        << "  quit, exit        disconnect and exit\n";
  }

  void print_content(OutStream& out, const mcp::ContentBlock& block)
  {
    if(const auto* text = std::get_if<mcp::TextContent>(&block))
    {
      out << text->text;
      return;
    }
    if(const auto* image = std::get_if<mcp::ImageContent>(&block))
    {
      out << "[image " << image->mime_type << ", " << image->data.size()
          << " base64 bytes]";
      return;
    }
    if(const auto* audio = std::get_if<mcp::AudioContent>(&block))
    {
      out << "[audio " << audio->mime_type << ", " << audio->data.size()
          << " base64 bytes]";
      return;
    }
    out << mcp::dump(mcp::to_json(block));
  }

  void print_result(OutStream& out, const mcp::CallToolResult& result)
  {
    if(result.is_error.value_or(false))
    {
      out << "error: ";
    }
    if(result.content.isEmpty())
    {
      out << "(no content)";
    }
    else
    {
      for(qsizetype i = 0; i < result.content.size(); ++i)
      {
        if(i != 0)
        {
          out << "\n  ";
        }
        print_content(out, result.content[i]);
      }
    }
    out << "\n";
    if(!result.structured_content.isNull())
    {
      out << "structured: " << mcp::dump(result.structured_content) << "\n";
    }
  }

  void list_tools(OutStream& out, mcp::Client& client)
  {
    const mcp::ListToolsResult tools = client.list_tools().get();
    if(tools.tools.isEmpty())
    {
      out << "(server exposes no tools)\n";
      return;
    }
    for(const mcp::Tool& tool : tools.tools)
    {
      out << "  - " << tool.name;
      if(!tool.description.isNull() && !tool.description.isEmpty())
      {
        out << " — " << tool.description;
      }
      out << "\n";
    }
  }

  void show_info(OutStream& out, const mcp::Client& client)
  {
    const std::optional<mcp::InitializeResult> info = client.server();
    if(!info.has_value())
    {
      out << "(not initialized)\n";
      return;
    }
    out << "server: " << info->server_info.name << " "
        << info->server_info.version << "\n";
    if(!info->instructions.isNull())
    {
      out << "instructions: " << info->instructions << "\n";
    }
  }

  void call_tool(OutStream& out, InStream& in, mcp::Client& client)
  {
    out << "tool name: ";
    out.flush();
    QString name = in.readLine();
    if(name.isNull() || name.trimmed().isEmpty())
    {
      out << "no tool selected\n";
      return;
    }
    name = name.trimmed();

    out << "arguments JSON (blank for {}): ";
    out.flush();
    QString args_text = in.readLine();
    if(args_text.isNull())
    {
      return;
    }
    args_text = args_text.trimmed();

    QJsonValue args = QJsonObject{};
    if(!args_text.isEmpty())
    {
      try
      {
        args = mcp::parse_json(args_text.toUtf8());
      }
      catch(const std::exception& e)
      {
        out << "invalid JSON: " << e.what() << "\n";
        return;
      }
      if(!args.isObject())
      {
        out << "arguments must be a JSON object\n";
        return;
      }
    }

    try
    {
      const mcp::CallToolResult result = client.call_tool(name, args).get();
      print_result(out, result);
    }
    catch(const std::exception& e)
    {
      out << "call failed: " << e.what() << "\n";
    }
  }

  void run_interactive(mcp::Client& client, const QString& endpoint,
                       QCoreApplication& app)
  {
    QFile input;
    if(!input.open(stdin, QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream(stderr) << "mcp_tester: failed to open stdin\n";
      QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
      return;
    }
    InStream in(&input);
    OutStream out(stdout);

    try
    {
      const mcp::InitializeResult info = client.initialize().get();
      out << "connected to " << info.server_info.name << " "
          << info.server_info.version << "\n";
    }
    catch(const std::exception& e)
    {
      QTextStream(stderr) << "mcp_tester: initialize failed: " << e.what()
                          << "\n";
      client.disconnect();
      QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
      return;
    }

    out << "mcp_tester — server: " << endpoint << "\n";
    print_help(out);
    out.flush();

    while(true)
    {
      out << "mcp_tester> ";
      out.flush();
      QString line = in.readLine();
      if(line.isNull())
      {
        break;
      }
      line = line.trimmed();
      if(line.isEmpty())
      {
        continue;
      }

      if(line == "quit" || line == "exit")
      {
        break;
      }
      if(line == "help" || line == "?")
      {
        print_help(out);
      }
      else if(line == "list" || line == "tools" || line == "ls")
      {
        try
        {
          list_tools(out, client);
        }
        catch(const std::exception& e)
        {
          out << "list failed: " << e.what() << "\n";
        }
      }
      else if(line == "call")
      {
        call_tool(out, in, client);
      }
      else if(line == "info")
      {
        show_info(out, client);
      }
      else if(line == "ping")
      {
        try
        {
          client.ping().get();
          out << "pong\n";
        }
        catch(const std::exception& e)
        {
          out << "ping failed: " << e.what() << "\n";
        }
      }
      else
      {
        out << "unknown command '" << line << "'; type help\n";
      }
      out.flush();
    }

    client.disconnect();
    out << "bye\n";
    out.flush();
    QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
  }

  QString resolve_program(const QString& program)
  {
    if(program.contains('/'))
    {
      const QFileInfo info(program);
      if(info.exists() && info.isExecutable())
      {
        return info.absoluteFilePath();
      }
      return {};
    }
    return QStandardPaths::findExecutable(program);
  }

} // namespace

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("mcp_tester");
  QCoreApplication::setApplicationVersion("0.1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription(
      "Interactive MCP client. Connects to an MCP server over Streamable HTTP "
      "or stdio, then lets you inspect and call its tools.\n\n"
      "Interactive commands:\n"
      "  list, tools, ls   list the server's tools\n"
      "  call              call a tool with JSON arguments\n"
      "  info              show server info and instructions\n"
      "  ping              send a ping\n"
      "  help, ?           show the command list\n"
      "  quit, exit        disconnect and exit\n\n"
      "Examples:\n"
      "  mcp_tester --http http://127.0.0.1:8848/mcp\n"
      "  mcp_tester --stdio ./build/server/mcp_stdio_server -- ./plugins");
  parser.addHelpOption();
  parser.addVersionOption();

  const QCommandLineOption http_option(
      "http",
      "Connect to an MCP server over Streamable HTTP. The value is the "
      "endpoint URL, e.g. http://127.0.0.1:8848/mcp.",
      "URL");
  const QCommandLineOption stdio_option(
      "stdio",
      "Spawn an MCP server and connect over its stdin/stdout. Any positional "
      "arguments are passed to the program.",
      "PROGRAM");

  parser.addOption(http_option);
  parser.addOption(stdio_option);
  parser.addPositionalArgument(
      "server-args",
      "Arguments passed to the program given with --stdio.",
      "[-- server-args...]");

  parser.process(app);

  const bool use_http = parser.isSet(http_option);
  const bool use_stdio = parser.isSet(stdio_option);
  const QStringList server_args = parser.positionalArguments();

  OutStream err(stderr);

  if(use_http == use_stdio)
  {
    err << "mcp_tester: choose exactly one transport: --http <URL> or "
           "--stdio <PROGRAM>\n\n";
    err.flush();
    parser.showHelp(2);
  }

  if(!use_stdio && !server_args.isEmpty())
  {
    err << "mcp_tester: positional arguments are only valid with "
           "--stdio\n\n";
    err.flush();
    parser.showHelp(2);
  }

  mcp::Client client{ mcp::Implementation{
      .name = "mcp_tester",
      .version = "0.1.0",
  } };

  QString endpoint;

  if(use_http)
  {
    const QString url = parser.value(http_option);
    const QUrl parsed(url);
    if(!parsed.isValid() || parsed.scheme().isEmpty()
       || (parsed.scheme() != "http" && parsed.scheme() != "https")
       || parsed.host().isEmpty())
    {
      err << "mcp_tester: invalid --http URL: " << url << "\n";
      err.flush();
      return 2;
    }
#if defined(MCP_ENABLE_HTTP)
    mcp::HttpClientTransport::Options opts;
    opts.url = url;
    client.connect(std::make_unique<mcp::HttpClientTransport>(std::move(opts)));
    endpoint = url;
#else
    err << "mcp_tester: this build has no HTTP support (MCP_ENABLE_HTTP=OFF)\n";
    err.flush();
    return 2;
#endif
  }
  else
  {
    const QString requested = parser.value(stdio_option);
    const QString program = resolve_program(requested);
    if(program.isEmpty())
    {
      err << "mcp_tester: could not find or execute --stdio program: "
          << requested << "\n";
      err.flush();
      return 2;
    }

    ProcessTransport::Options opts;
    opts.program = program;
    opts.arguments = server_args;
    client.connect(std::make_unique<ProcessTransport>(std::move(opts)));
    endpoint = program;
    if(!server_args.isEmpty())
    {
      endpoint += ' ' + server_args.join(' ');
    }
  }

  std::thread interactive(
      [&client, &endpoint, &app] { run_interactive(client, endpoint, app); });
  const int status = app.exec();
  if(interactive.joinable())
  {
    interactive.join();
  }
  return status;
}
