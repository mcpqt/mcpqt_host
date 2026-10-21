// SPDX-License-Identifier: AGPL-3.0-only
//
// Minimal in-process HTTP/1.1 server used by the HTTP transport tests as a
// stand-in MCP endpoint.
//
// It runs a QTcpServer on a private QThread with its own event loop, so tests
// may block on std::future / condition variables without pumping Qt's event
// loop.
// Each request is answered with the exact raw bytes returned by the Handler,
// which gives tests byte-level control over status lines, headers and bodies
// (SSE, 202-with-no-body, custom session headers, ...).

#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <thread>
#include <utility>

namespace mcp::test
{

  class FakeHttpServer
  {
  public:
    struct Request
    {
      QByteArray method;
      QByteArray path;
      QHash<QByteArray, QByteArray> headers; // keys lower-cased
      QByteArray body;

      [[nodiscard]] QByteArray header(const char* name) const
      {
        return headers.value(QByteArray(name).toLower());
      }
    };

    /// Returns the full raw HTTP response for `req`.
    using Handler = std::function<QByteArray(const Request&)>;

    explicit FakeHttpServer(Handler handler)
    {
      m_worker = new Worker(std::move(handler));
      m_thread = new QThread;
      m_worker->moveToThread(m_thread);
      QObject::connect(m_thread, &QThread::started, m_worker,
                       [this] { m_worker->start(); });
      QObject::connect(m_thread, &QThread::finished, m_worker,
                       &QObject::deleteLater);
      m_thread->start();
      while(m_worker->port() == 0)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    ~FakeHttpServer()
    {
      m_thread->quit();
      m_thread->wait();
      delete m_thread;
    }

    FakeHttpServer(const FakeHttpServer&) = delete;
    FakeHttpServer& operator=(const FakeHttpServer&) = delete;

    [[nodiscard]] quint16 port(void) const { return m_worker->port(); }
    [[nodiscard]] QString url(const QString& path
                              = QStringLiteral("/mcp")) const
    {
      return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path);
    }

  private:
    class Worker : public QObject
    {
    public:
      explicit Worker(Handler h) : m_handler(std::move(h)) {}

      void start(void)
      {
        m_server = new QTcpServer(this);
        QObject::connect(m_server, &QTcpServer::newConnection, this, [this] {
          while(QTcpSocket* c = m_server->nextPendingConnection())
          {
            handle(c);
          }
        });
        m_server->listen(QHostAddress::LocalHost, 0);
        m_port.store(m_server->serverPort());
      }

      [[nodiscard]] quint16 port(void) const { return m_port.load(); }

    private:
      static bool request_complete(const QByteArray& buf)
      {
        const qsizetype end = buf.indexOf("\r\n\r\n");
        if(end < 0)
          return false;
        int content_length = 0;
        const QByteArray head = buf.left(end);
        for(const QByteArray& line : head.split('\n'))
        {
          const QByteArray t = line.trimmed();
          if(t.toLower().startsWith("content-length:"))
          {
            content_length = t.mid(t.indexOf(':') + 1).trimmed().toInt();
          }
        }
        return buf.size() >= end + 4 + content_length;
      }

      static Request parse(const QByteArray& buf)
      {
        Request r;
        const qsizetype end = buf.indexOf("\r\n\r\n");
        const QByteArray head = buf.left(end);
        const QList<QByteArray> lines = head.split('\n');
        if(!lines.isEmpty())
        {
          const QList<QByteArray> parts = lines.first().trimmed().split(' ');
          if(parts.size() >= 2)
          {
            r.method = parts[0];
            r.path = parts[1];
          }
        }
        for(qsizetype i = 1; i < lines.size(); ++i)
        {
          const QByteArray t = lines[i].trimmed();
          const qsizetype colon = t.indexOf(':');
          if(colon > 0)
          {
            r.headers.insert(t.left(colon).toLower(),
                             t.mid(colon + 1).trimmed());
          }
        }
        if(end >= 0)
          r.body = buf.mid(end + 4);
        return r;
      }

      void handle(QTcpSocket* c)
      {
        auto buf = std::make_shared<QByteArray>();
        QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
          buf->append(c->readAll());
          if(!request_complete(*buf))
            return;
          const QByteArray resp = m_handler(parse(*buf));
          c->write(resp);
          c->flush();
          c->disconnectFromHost();
        });
        QObject::connect(c, &QTcpSocket::disconnected, c,
                         &QObject::deleteLater);
      }

      QTcpServer* m_server = nullptr;
      Handler m_handler;
      std::atomic<quint16> m_port{ 0 };
    };

    Worker* m_worker = nullptr;
    QThread* m_thread = nullptr;
  };

  // --------------------------------------------------------------------------
  // Response builders — always Content-Length framed and Connection: close so
  // the client sees a complete, self-delimiting response per request.
  // --------------------------------------------------------------------------

  inline QByteArray
  http_response(int status, const char* reason, const QByteArray& content_type,
                const QByteArray& body,
                std::initializer_list<QPair<QByteArray, QByteArray>> headers
                = {})
  {
    QByteArray out
        = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    for(const auto& [k, v] : headers)
    {
      out += k + ": " + v + "\r\n";
    }
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
  }

  inline QByteArray json_response(
      const QByteArray& body, int status = 200, const char* reason = "OK",
      std::initializer_list<QPair<QByteArray, QByteArray>> headers = {})
  {
    return http_response(status, reason, "application/json", body, headers);
  }

  inline QByteArray
  sse_response(const QByteArray& body,
               std::initializer_list<QPair<QByteArray, QByteArray>> headers
               = {})
  {
    return http_response(200, "OK", "text/event-stream", body, headers);
  }

  inline QByteArray empty_response(int status, const char* reason)
  {
    return http_response(status, reason, "application/octet-stream",
                         QByteArray{}, {});
  }

} // namespace mcp::test
