// SPDX-License-Identifier: AGPL-3.0-only
//
// mcp_tester: an interactive MCP client built on Qt and libqjsonrpc.
//
// The command interface is discovered at runtime from the server's
// initialize result and the entities it returns from tools/list,
// resources/list, resources/templates/list, and prompts/list. Tool and
// prompt arguments are prompted for using the schemas the server supplies.
//
//   mcp_tester --http http://127.0.0.1:8848/mcp
//   mcp_tester --stdio ./mcp_stdio_server -- ./plugins

#if defined(MCP_ENABLE_HTTP)
#include "mcp/http_client_transport.h"
#endif

#include "src/jsonrpc.h"
#include "src/session.h"
#include "src/transport.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace
{

  using OutStream = QTextStream;
  using InStream = QTextStream;

  std::atomic<bool> g_refresh_requested{ false };

  QString pretty_json(const QJsonValue& value)
  {
    if(value.isObject())
    {
      return QString::fromUtf8(
          QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
    }
    if(value.isArray())
    {
      return QString::fromUtf8(
          QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
    }
    if(value.isString())
    {
      return value.toString();
    }
    if(value.isNull() || value.isUndefined())
    {
      return "null";
    }
    if(value.isBool())
    {
      return value.toBool() ? "true" : "false";
    }
    if(value.isDouble())
    {
      const qint64 integer = value.toInteger();
      if(static_cast<double>(integer) == value.toDouble())
      {
        return QString::number(integer);
      }
      return QString::number(value.toDouble());
    }
    return {};
  }

  QString compact_json(const QJsonValue& value)
  {
    if(value.isObject())
    {
      return QString::fromUtf8(
          QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if(value.isArray())
    {
      return QString::fromUtf8(
          QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return pretty_json(value);
  }

  void print_notification(const QString& kind, const QJsonValue& value)
  {
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    QTextStream err(stderr);
    err << "[notification] " << kind << ": " << pretty_json(value) << "\n";
    err.flush();
  }

  // -------------------------------------------------------------------------
  // QProcess-backed stdio transport for libqjsonrpc
  // -------------------------------------------------------------------------

  class ProcessTransport final : public QObject, public jsonrpc::Transport
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

#if defined(MCP_ENABLE_HTTP)
  class HttpTransportAdapter final : public jsonrpc::Transport
  {
  public:
    explicit HttpTransportAdapter(std::unique_ptr<mcp::Transport> inner)
        : m_inner(std::move(inner))
    {
    }

    void on_message(MessageCallback cb) override
    {
      m_inner->on_message(std::move(cb));
    }
    void on_error(ErrorCallback cb) override { m_inner->on_error(std::move(cb)); }
    void on_close(CloseCallback cb) override { m_inner->on_close(std::move(cb)); }

    void start(void) override { m_inner->start(); }
    void close(void) override { m_inner->close(); }

    [[nodiscard]] std::error_code send(QByteArrayView frame) override
    {
      return m_inner->send(frame);
    }

    [[nodiscard]] bool is_open(void) const noexcept override
    {
      return m_inner->is_open();
    }

  private:
    std::unique_ptr<mcp::Transport> m_inner;
  };
#endif

  // -------------------------------------------------------------------------
  // Schema-driven argument handling
  // -------------------------------------------------------------------------

  QString schema_type(const QJsonObject& schema)
  {
    const QJsonValue type = schema.value("type");
    if(type.isString())
    {
      return type.toString();
    }
    if(type.isArray())
    {
      QStringList parts;
      for(const QJsonValue& item : type.toArray())
      {
        if(item.isString())
        {
          parts << item.toString();
        }
      }
      if(!parts.isEmpty())
      {
        return parts.join("|");
      }
    }
    return {};
  }

  QJsonValue parse_json_value(const QString& text, const QString& context)
  {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        text.toUtf8(), &error);
    if(error.error != QJsonParseError::NoError)
    {
      throw std::runtime_error(
          context.toStdString() + ": " + error.errorString().toStdString());
    }
    if(doc.isObject())
    {
      return doc.object();
    }
    if(doc.isArray())
    {
      return doc.array();
    }
    throw std::runtime_error(
        context.toStdString() + ": expected a JSON object or array");
  }

  QJsonValue convert_value(const QString& text, const QJsonObject& schema)
  {
    const QString type = schema_type(schema);
    if(type == "string" || type.isEmpty())
    {
      return text;
    }
    if(type == "integer")
    {
      bool ok = false;
      const qint64 value = text.toLongLong(&ok);
      if(!ok)
      {
        throw std::runtime_error("expected an integer");
      }
      return value;
    }
    if(type == "number")
    {
      bool ok = false;
      const double value = text.toDouble(&ok);
      if(!ok)
      {
        throw std::runtime_error("expected a number");
      }
      return value;
    }
    if(type == "boolean")
    {
      const QString v = text.toLower();
      if(v == "true" || v == "1")
      {
        return true;
      }
      if(v == "false" || v == "0")
      {
        return false;
      }
      throw std::runtime_error("expected true or false");
    }
    if(type == "object" || type == "array")
    {
      return parse_json_value(text, type);
    }
    if(type == "null")
    {
      return QJsonValue(QJsonValue::Null);
    }
    return text;
  }

  QHash<QString, QString> parse_inline_arguments(const QString& text)
  {
    QHash<QString, QString> values;
    if(text.trimmed().isEmpty())
    {
      return values;
    }
    const QStringList tokens = text.split(' ', Qt::SkipEmptyParts);
    for(const QString& token : tokens)
    {
      const qsizetype equals = token.indexOf('=');
      if(equals <= 0)
      {
        throw std::runtime_error(
            "expected key=value arguments, got '" + token.toStdString() + "'");
      }
      values.insert(token.left(equals), token.mid(equals + 1));
    }
    return values;
  }

  QString schema_usage(const QJsonObject& schema)
  {
    const QJsonObject properties = schema.value("properties").toObject();
    if(properties.isEmpty())
    {
      return "[json]";
    }

    QSet<QString> required;
    for(const QJsonValue& value : schema.value("required").toArray())
    {
      if(value.isString())
      {
        required.insert(value.toString());
      }
    }

    QStringList parts;
    for(auto it = properties.constBegin(); it != properties.constEnd(); ++it)
    {
      const QJsonObject property = it.value().toObject();
      QString part = it.key();
      const QString type = schema_type(property);
      if(!type.isEmpty())
      {
        part += "=<" + type + ">";
      }
      if(!required.contains(it.key()))
      {
        part = "[" + part + "]";
      }
      parts << part;
    }
    return parts.join(' ');
  }

  QStringList uri_template_variables(const QString& uri)
  {
    static const QRegularExpression variable(R"(\{([^}]+)\})");
    QStringList names;
    QRegularExpressionMatchIterator it = variable.globalMatch(uri);
    while(it.hasNext())
    {
      const QRegularExpressionMatch match = it.next();
      QString name = match.captured(1);
      if(!name.isEmpty() && (name.startsWith('+') || name.startsWith('#')))
      {
        name.remove(0, 1);
      }
      if(!names.contains(name))
      {
        names << name;
      }
    }
    return names;
  }

  // -------------------------------------------------------------------------
  // Interactive, data-driven client
  // -------------------------------------------------------------------------

  class Tester
  {
  public:
    Tester(jsonrpc::Session& session, InStream& in, OutStream& out,
           QString endpoint)
        : m_session(session), m_in(in), m_out(out),
          m_endpoint(std::move(endpoint))
    {
    }

    void run(void)
    {
      initialize();
      discover();
      print_help();
      loop();
    }

    void refresh(void)
    {
      discover();
      print_help();
    }

  private:
    struct Command
    {
      QString name;
      QString usage;
      QString description;
      QJsonValue details;
      std::function<void(const QString&)> run;
    };

    void initialize(void)
    {
      QJsonObject capabilities{
        { "roots", QJsonObject{ { "listChanged", false } } },
      };
      QJsonObject params{
        { "protocolVersion", "2025-11-25" },
        { "capabilities", capabilities },
        { "clientInfo",
          QJsonObject{ { "name", "mcp_tester" },
                       { "version", "0.1.0" } } },
      };

      const QJsonObject result = request("initialize", params).toObject();
      m_server = result;

      const std::error_code ec
          = m_session.send_notification("notifications/initialized");
      if(ec)
      {
        throw std::runtime_error("failed to send notifications/initialized: "
                                 + ec.message());
      }

      m_out << "connected to "
            << result.value("serverInfo").toObject().value("name").toString()
            << " "
            << result.value("serverInfo")
                   .toObject()
                   .value("version")
                   .toString()
            << "\n";
    }

    QJsonValue request(const QString& method, const QJsonValue& params = {})
    {
      return m_session.send_request(method, params).get();
    }

    void print_json(const QJsonValue& value)
    {
      const QString text = pretty_json(value);
      m_out << text;
      if(!text.endsWith('\n'))
      {
        m_out << "\n";
      }
    }

    QString read_line(void)
    {
      const QString line = m_in.readLine();
      if(line.isNull())
      {
        throw std::runtime_error("input closed");
      }
      return line.trimmed();
    }

    QJsonObject collect_arguments(const QJsonObject& schema,
                                  const QString& inline_arguments)
    {
      const QJsonObject properties = schema.value("properties").toObject();
      const bool interactive = inline_arguments.trimmed().isEmpty();
      const QHash<QString, QString> inline_values
          = parse_inline_arguments(inline_arguments);

      if(properties.isEmpty())
      {
        if(!interactive)
        {
          return parse_json_value(inline_arguments, "arguments").toObject();
        }
        m_out << "arguments JSON (blank for {}): ";
        m_out.flush();
        const QString text = read_line();
        if(text.isEmpty())
        {
          return {};
        }
        return parse_json_value(text, "arguments").toObject();
      }

      QSet<QString> required;
      for(const QJsonValue& value : schema.value("required").toArray())
      {
        if(value.isString())
        {
          required.insert(value.toString());
        }
      }

      QJsonObject arguments;
      for(auto it = properties.constBegin(); it != properties.constEnd(); ++it)
      {
        const QString name = it.key();
        const QJsonObject property = it.value().toObject();
        const QString type = schema_type(property);
        const bool is_required = required.contains(name);

        QString text;
        bool supplied = false;

        if(inline_values.contains(name))
        {
          text = inline_values.value(name);
          supplied = true;
        }
        else if(interactive)
        {
          m_out << name;
          if(!type.isEmpty())
          {
            m_out << " (" << type << ")";
          }
          if(is_required)
          {
            m_out << " [required]";
          }
          const QString description
              = property.value("description").toString();
          if(!description.isEmpty())
          {
            m_out << " — " << description;
          }
          if(property.contains("default"))
          {
            m_out << " [default: "
                  << compact_json(property.value("default")) << "]";
          }
          m_out << ": ";
          m_out.flush();
          text = read_line();
          supplied = !text.isEmpty();
        }

        if(!supplied)
        {
          if(is_required)
          {
            throw std::runtime_error("missing required argument '" +
                                     name.toStdString() + "'");
          }
          continue;
        }

        arguments.insert(name, convert_value(text, property));
      }

      for(auto it = inline_values.constBegin();
          it != inline_values.constEnd(); ++it)
      {
        if(properties.contains(it.key()))
        {
          continue;
        }
        QJsonValue value(it.value());
        if(it.value().startsWith('{') || it.value().startsWith('['))
        {
          value = parse_json_value(it.value(), it.key());
        }
        arguments.insert(it.key(), value);
      }

      return arguments;
    }

    QJsonObject collect_prompt_arguments(const QJsonObject& prompt,
                                         const QString& inline_arguments)
    {
      const QJsonArray arguments = prompt.value("arguments").toArray();
      const bool interactive = inline_arguments.trimmed().isEmpty();
      const QHash<QString, QString> inline_values
          = parse_inline_arguments(inline_arguments);

      if(arguments.isEmpty())
      {
        if(!interactive)
        {
          return parse_json_value(inline_arguments, "arguments").toObject();
        }
        return {};
      }

      QJsonObject values;
      for(const QJsonValue& value : arguments)
      {
        const QJsonObject argument = value.toObject();
        const QString name = argument.value("name").toString();
        if(name.isEmpty())
        {
          continue;
        }
        const bool required = argument.value("required").toBool(false);

        QString text;
        bool supplied = false;
        if(inline_values.contains(name))
        {
          text = inline_values.value(name);
          supplied = true;
        }
        else if(interactive)
        {
          m_out << name;
          if(required)
          {
            m_out << " [required]";
          }
          const QString description
              = argument.value("description").toString();
          if(!description.isEmpty())
          {
            m_out << " — " << description;
          }
          m_out << ": ";
          m_out.flush();
          text = read_line();
          supplied = !text.isEmpty();
        }

        if(!supplied)
        {
          if(required)
          {
            throw std::runtime_error("missing required argument '" +
                                     name.toStdString() + "'");
          }
          continue;
        }
        values.insert(name, text);
      }

      for(auto it = inline_values.constBegin();
          it != inline_values.constEnd(); ++it)
      {
        if(!values.contains(it.key()))
        {
          values.insert(it.key(), it.value());
        }
      }
      return values;
    }

    void run_tool(const QJsonObject& tool, const QString& inline_arguments)
    {
      const QJsonObject schema = tool.value("inputSchema").toObject();
      QJsonObject params{
        { "name", tool.value("name") },
        { "arguments", collect_arguments(schema, inline_arguments) },
      };
      print_json(request("tools/call", params));
    }

    void run_prompt(const QJsonObject& prompt, const QString& inline_arguments)
    {
      QJsonObject params{
        { "name", prompt.value("name") },
        { "arguments", collect_prompt_arguments(prompt, inline_arguments) },
      };
      print_json(request("prompts/get", params));
    }

    void run_template(const QJsonObject& resource_template,
                      const QString& inline_arguments)
    {
      QString uri = resource_template.value("uriTemplate").toString();
      const QHash<QString, QString> inline_values
          = parse_inline_arguments(inline_arguments);

      for(const QString& name : uri_template_variables(uri))
      {
        QString value;
        if(inline_values.contains(name))
        {
          value = inline_values.value(name);
        }
        else
        {
          m_out << name << " (URI variable): ";
          m_out.flush();
          value = read_line();
        }
        const QString encoded
            = QString::fromLatin1(QUrl::toPercentEncoding(value));
        uri.replace("{" + name + "}", encoded);
        uri.replace("{+" + name + "}", value);
      }

      print_json(request("resources/read", QJsonObject{ { "uri", uri } }));
    }

    QString unique_name(QString name)
    {
      if(name == "quit" || name == "exit" || name == "help" || name == "?")
      {
        name.prepend("tool:");
      }
      QString candidate = name;
      int suffix = 2;
      while(m_index.contains(candidate))
      {
        candidate = name + "#" + QString::number(suffix++);
      }
      return candidate;
    }

    void add_command(QString name, QString usage, QString description,
                     QJsonValue details,
                     std::function<void(const QString&)> run)
    {
      Command command;
      command.name = unique_name(std::move(name));
      command.usage = std::move(usage);
      command.description = std::move(description);
      command.details = std::move(details);
      command.run = std::move(run);
      m_index.insert(command.name, m_commands.size());
      m_commands.append(std::move(command));
    }

    QList<QJsonObject> fetch_all(const QString& method, const QString& key)
    {
      QList<QJsonObject> items;
      QString cursor;
      do
      {
        QJsonObject params;
        if(!cursor.isEmpty())
        {
          params.insert("cursor", cursor);
        }
        const QJsonObject result = request(method, params).toObject();
        for(const QJsonValue& value : result.value(key).toArray())
        {
          items.append(value.toObject());
        }
        cursor = result.value("nextCursor").toString();
      } while(!cursor.isEmpty());
      return items;
    }

    QJsonObject cursor_params(const QString& tail)
    {
      if(tail.isEmpty())
      {
        return {};
      }
      if(tail.startsWith('{'))
      {
        return parse_json_value(tail, "params").toObject();
      }
      return { { "cursor", tail } };
    }

    QJsonObject single_param(const QString& tail, const QString& key)
    {
      if(tail.startsWith('{'))
      {
        return parse_json_value(tail, "params").toObject();
      }
      return { { key, tail } };
    }

    void add_protocol_commands(const QJsonObject& capabilities)
    {
      add_command(
          "ping", "", "Send a ping.", {},
          [this](const QString&) { print_json(request("ping")); });

      if(capabilities.contains("tools"))
      {
        add_command(
            "tools/list", "[cursor]",
            "List tools as returned by the server.", {},
            [this](const QString& tail) {
              print_json(request("tools/list", cursor_params(tail)));
            });
        add_command(
            "tools/call", "<name> [key=value ...] | <json>",
            "Call a tool using its schema or raw tools/call parameters.", {},
            [this](const QString& tail) {
              if(tail.trimmed().isEmpty())
              {
                throw std::runtime_error(
                    "usage: tools/call <name> [key=value ...] | <json>");
              }
              if(tail.startsWith('{'))
              {
                print_json(
                    request("tools/call", parse_json_value(tail, "params")));
                return;
              }
              const QString name = tail.split(' ').value(0);
              auto it = m_tools.find(name);
              if(it == m_tools.end())
              {
                throw std::runtime_error("unknown tool: " + name.toStdString());
              }
              qsizetype space = tail.indexOf(' ');
              const QString rest
                  = space < 0 ? QString{} : tail.mid(space + 1).trimmed();
              run_tool(it.value(), rest);
            });
      }

      if(capabilities.contains("resources"))
      {
        add_command(
            "resources/list", "[cursor]",
            "List resources as returned by the server.", {},
            [this](const QString& tail) {
              print_json(request("resources/list", cursor_params(tail)));
            });
        add_command(
            "resources/templates/list", "[cursor]",
            "List resource templates as returned by the server.", {},
            [this](const QString& tail) {
              print_json(
                  request("resources/templates/list", cursor_params(tail)));
            });
        add_command(
            "resources/read", "<uri>",
            "Read a resource by URI.", {},
            [this](const QString& tail) {
              print_json(request("resources/read",
                                 single_param(tail, "uri")));
            });
        add_command(
            "resources/subscribe", "<uri>",
            "Subscribe to updates for a resource URI.", {},
            [this](const QString& tail) {
              print_json(request("resources/subscribe",
                                 single_param(tail, "uri")));
            });
        add_command(
            "resources/unsubscribe", "<uri>",
            "Unsubscribe from updates for a resource URI.", {},
            [this](const QString& tail) {
              print_json(request("resources/unsubscribe",
                                 single_param(tail, "uri")));
            });
      }

      if(capabilities.contains("prompts"))
      {
        add_command(
            "prompts/list", "[cursor]",
            "List prompts as returned by the server.", {},
            [this](const QString& tail) {
              print_json(request("prompts/list", cursor_params(tail)));
            });
        add_command(
            "prompts/get", "<name> <json>",
            "Get a prompt using raw prompts/get parameters.", {},
            [this](const QString& tail) {
              if(tail.startsWith('{'))
              {
                print_json(
                    request("prompts/get", parse_json_value(tail, "params")));
                return;
              }
              const qsizetype space = tail.indexOf(' ');
              if(space < 0)
              {
                throw std::runtime_error(
                    "usage: prompts/get <name> <json>");
              }
              QJsonObject params{
                { "name", tail.left(space) },
                { "arguments",
                  parse_json_value(tail.mid(space + 1).trimmed(), "arguments") },
              };
              print_json(request("prompts/get", params));
            });
      }

      if(capabilities.contains("logging"))
      {
        add_command(
            "logging/setLevel", "<level>",
            "Set the server log level using an RFC 5424 level name.", {},
            [this](const QString& tail) {
              print_json(request("logging/setLevel",
                                 QJsonObject{ { "level", tail } }));
            });
      }

      if(capabilities.contains("completions"))
      {
        add_command(
            "completion/complete", "<json>",
            "Request completion suggestions using raw "
            "completion/complete parameters.", {},
            [this](const QString& tail) {
              print_json(request(
                  "completion/complete",
                  parse_json_value(tail, "params")));
            });
      }

      if(capabilities.contains("tasks"))
      {
        add_command(
            "tasks/list", "[cursor]",
            "List tasks as returned by the server.", {},
            [this](const QString& tail) {
              print_json(request("tasks/list", cursor_params(tail)));
            });
        add_command(
            "tasks/get", "<task-id>", "Get a task envelope.", {},
            [this](const QString& tail) {
              print_json(request("tasks/get",
                                 single_param(tail, "taskId")));
            });
        add_command(
            "tasks/result", "<task-id>",
            "Wait for and return a task's result.", {},
            [this](const QString& tail) {
              print_json(request("tasks/result",
                                 single_param(tail, "taskId")));
            });
        add_command(
            "tasks/cancel", "<task-id>", "Cancel a task.", {},
            [this](const QString& tail) {
              print_json(request("tasks/cancel",
                                 single_param(tail, "taskId")));
            });
      }
    }

    void discover(void)
    {
      m_commands.clear();
      m_index.clear();
      m_tools.clear();

      const QJsonObject capabilities
          = m_server.value("capabilities").toObject();

      add_protocol_commands(capabilities);

      if(capabilities.contains("tools"))
      {
        try
        {
          for(const QJsonObject& tool : fetch_all("tools/list", "tools"))
          {
            const QString name = tool.value("name").toString();
            if(name.isEmpty())
            {
              continue;
            }
            m_tools.insert(name, tool);
            add_command(
                name, schema_usage(tool.value("inputSchema").toObject()),
                tool.value("description").toString(), tool,
                [this, tool](const QString& tail) { run_tool(tool, tail); });
          }
        }
        catch(const std::exception& e)
        {
          m_out << "warning: tools/list failed: " << e.what() << "\n";
        }
      }

      if(capabilities.contains("resources"))
      {
        try
        {
          for(const QJsonObject& resource :
              fetch_all("resources/list", "resources"))
          {
            QString label = resource.value("name").toString();
            if(label.isEmpty())
            {
              label = resource.value("uri").toString();
            }
            add_command(
                "resource:" + label, "",
                resource.value("description").toString(), resource,
                [this, resource](const QString&) {
                  print_json(request(
                      "resources/read",
                      QJsonObject{ { "uri", resource.value("uri") } }));
                });
          }
        }
        catch(const std::exception& e)
        {
          m_out << "warning: resources/list failed: " << e.what() << "\n";
        }

        try
        {
          for(const QJsonObject& resource_template :
              fetch_all("resources/templates/list", "resourceTemplates"))
          {
            QString label = resource_template.value("name").toString();
            if(label.isEmpty())
            {
              label = resource_template.value("uriTemplate").toString();
            }
            QStringList usage;
            for(const QString& variable : uri_template_variables(
                    resource_template.value("uriTemplate").toString()))
            {
              usage << variable + "=<value>";
            }
            add_command(
                "template:" + label, usage.join(' '),
                resource_template.value("description").toString(),
                resource_template,
                [this, resource_template](const QString& tail) {
                  run_template(resource_template, tail);
                });
          }
        }
        catch(const std::exception& e)
        {
          m_out << "warning: resources/templates/list failed: " << e.what()
                << "\n";
        }
      }

      if(capabilities.contains("prompts"))
      {
        try
        {
          for(const QJsonObject& prompt : fetch_all("prompts/list", "prompts"))
          {
            QStringList usage;
            for(const QJsonValue& value :
                prompt.value("arguments").toArray())
            {
              const QJsonObject argument = value.toObject();
              QString part = argument.value("name").toString();
              if(!argument.value("required").toBool(false))
              {
                part = "[" + part + "]";
              }
              usage << part;
            }
            add_command(
                "prompt:" + prompt.value("name").toString(),
                usage.join(' '), prompt.value("description").toString(),
                prompt, [this, prompt](const QString& tail) {
                  run_prompt(prompt, tail);
                });
          }
        }
        catch(const std::exception& e)
        {
          m_out << "warning: prompts/list failed: " << e.what() << "\n";
        }
      }
    }

    void print_help(void)
    {
      const QJsonObject server_info = m_server.value("serverInfo").toObject();
      m_out << "server: " << server_info.value("name").toString() << " "
            << server_info.value("version").toString() << "\n";
      const QString instructions = m_server.value("instructions").toString();
      if(!instructions.isEmpty())
      {
        m_out << "instructions: " << instructions << "\n";
      }
      m_out << "protocol: " << m_server.value("protocolVersion").toString()
            << "\n";
      m_out << "\ncommands generated from server data:\n";
      for(const Command& command : m_commands)
      {
        m_out << "  " << command.name;
        if(!command.usage.isEmpty())
        {
          m_out << " " << command.usage;
        }
        if(!command.description.isEmpty())
        {
          m_out << " — " << command.description;
        }
        m_out << "\n";
      }
      m_out << "  help [command] — show this list or full command details\n"
            << "  quit, exit — disconnect and exit\n";
    }

    void show_command_details(const QString& name)
    {
      auto it = m_index.find(name);
      if(it == m_index.end())
      {
        m_out << "unknown command '" << name << "'\n";
        return;
      }
      const Command& command = m_commands.at(it.value());
      m_out << command.name;
      if(!command.usage.isEmpty())
      {
        m_out << " " << command.usage;
      }
      m_out << "\n";
      if(!command.description.isEmpty())
      {
        m_out << command.description << "\n";
      }
      if(!command.details.isNull() && !command.details.isUndefined())
      {
        print_json(command.details);
      }
    }

    void loop(void)
    {
      while(true)
      {
        if(g_refresh_requested.exchange(false))
        {
          refresh();
        }

        m_out << "mcp_tester> ";
        m_out.flush();

        const QString line = m_in.readLine();
        if(line.isNull())
        {
          break;
        }
        const QString trimmed = line.trimmed();
        if(trimmed.isEmpty())
        {
          continue;
        }

        const qsizetype space = trimmed.indexOf(' ');
        const QString name
            = space < 0 ? trimmed : trimmed.left(space);
        const QString tail
            = space < 0 ? QString{} : trimmed.mid(space + 1).trimmed();

        if(name == "quit" || name == "exit")
        {
          break;
        }
        if(name == "help" || name == "?")
        {
          if(tail.isEmpty())
          {
            print_help();
          }
          else
          {
            show_command_details(tail.split(' ').value(0));
          }
          continue;
        }

        auto it = m_index.find(name);
        if(it == m_index.end())
        {
          m_out << "unknown command '" << name << "'; type help\n";
          continue;
        }

        try
        {
          m_commands.at(it.value()).run(tail);
        }
        catch(const std::exception& e)
        {
          m_out << name << " failed: " << e.what() << "\n";
        }
        m_out.flush();
      }

      m_session.close();
      m_out << "bye\n";
      m_out.flush();
    }

    jsonrpc::Session& m_session;
    InStream& m_in;
    OutStream& m_out;
    QString m_endpoint;
    QJsonObject m_server;
    QList<Command> m_commands;
    QHash<QString, qsizetype> m_index;
    QHash<QString, QJsonObject> m_tools;
  };

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

  void configure_notifications(jsonrpc::Session& session)
  {
    session.set_notification_handler(
        "notifications/message",
        [](const QJsonValue& params) {
          print_notification("message", params);
        });
    session.set_notification_handler(
        "notifications/progress",
        [](const QJsonValue& params) {
          print_notification("progress", params);
        });
    session.set_notification_handler(
        "notifications/tasks/status",
        [](const QJsonValue& params) {
          print_notification("tasks/status", params);
        });
    session.set_notification_handler(
        "notifications/resources/updated",
        [](const QJsonValue& params) {
          print_notification("resources/updated", params);
        });
    session.set_notification_handler(
        "notifications/resources/list_changed",
        [](const QJsonValue&) {
          print_notification("resources/list_changed", QJsonObject{});
          g_refresh_requested.store(true);
        });
    session.set_notification_handler(
        "notifications/prompts/list_changed",
        [](const QJsonValue&) {
          print_notification("prompts/list_changed", QJsonObject{});
          g_refresh_requested.store(true);
        });
    session.set_notification_handler(
        "notifications/tools/list_changed",
        [](const QJsonValue&) {
          print_notification("tools/list_changed", QJsonObject{});
          g_refresh_requested.store(true);
        });
    session.set_notification_handler(
        "notifications/elicitation/complete",
        [](const QJsonValue& params) {
          print_notification("elicitation/complete", params);
        });

    session.set_request_handler("roots/list", [](const QJsonValue&) {
      const QString cwd = QDir::currentPath();
      QJsonObject root{
        { "uri", QUrl::fromLocalFile(cwd).toString() },
        { "name", QFileInfo(cwd).fileName().isEmpty() ? cwd
                                                      : QFileInfo(cwd).fileName() },
      };
      return QJsonObject{ { "roots", QJsonArray{ root } } };
    });
    session.set_request_handler("elicitation/create", [](const QJsonValue&) {
      return QJsonObject{ { "action", "decline" } };
    });
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
      "or stdio. After initialize, the command interface is generated from "
      "the server's capabilities and its tools, resources, and prompts.");
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

  std::unique_ptr<jsonrpc::Transport> transport;
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
    transport = std::make_unique<HttpTransportAdapter>(
        std::make_unique<mcp::HttpClientTransport>(std::move(opts)));
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
    transport = std::make_unique<ProcessTransport>(std::move(opts));
    endpoint = program;
    if(!server_args.isEmpty())
    {
      endpoint += ' ' + server_args.join(' ');
    }
  }

  jsonrpc::Session session(std::move(transport));
  configure_notifications(session);
  session.start();

  std::thread interactive([&session, &endpoint, &app] {
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
      Tester tester(session, in, out, endpoint);
      tester.run();
    }
    catch(const std::exception& e)
    {
      QTextStream(stderr) << "mcp_tester: " << e.what() << "\n";
      session.close();
    }
    QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
  });

  const int status = app.exec();
  if(interactive.joinable())
  {
    interactive.join();
  }
  session.close();
  return status;
}
