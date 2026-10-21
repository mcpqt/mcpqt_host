// SPDX-License-Identifier: AGPL-3.0-only
//
// Tiny blocking HTTP/1.1 client used by tests that need byte-level control
// (asserting on status codes and raw bodies/headers) against a running
// HttpServerHost. Built on QTcpSocket; no Qt event loop is required because
// the waitFor*() helpers block.

#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QTcpSocket>

#include <utility>

namespace mcp::test
{

  struct RawResponse
  {
    int status = 0;
    QHash<QByteArray, QByteArray> headers; // keys lower-cased
    QByteArray body;
    bool connected = false;

    [[nodiscard]] QByteArray header(const char* name) const
    {
      return headers.value(QByteArray(name).toLower());
    }
  };

  namespace detail
  {

    inline RawResponse parse_buffer(const QByteArray& data, bool* complete)
    {
      RawResponse out;
      *complete = false;
      const qsizetype head_end = data.indexOf("\r\n\r\n");
      if(head_end < 0)
        return out;

      const QList<QByteArray> lines = data.left(head_end).split('\n');
      if(!lines.isEmpty())
      {
        const QList<QByteArray> status = lines.first().trimmed().split(' ');
        if(status.size() >= 2)
          out.status = status[1].toInt();
      }
      for(qsizetype i = 1; i < lines.size(); ++i)
      {
        const QByteArray t = lines[i].trimmed();
        const qsizetype colon = t.indexOf(':');
        if(colon > 0)
        {
          out.headers.insert(t.left(colon).toLower(),
                             t.mid(colon + 1).trimmed());
        }
      }
      out.connected = true;

      const QByteArray te = out.header("Transfer-Encoding");
      QByteArray payload = data.mid(head_end + 4);

      if(te.contains("chunked"))
      {
        QByteArray decoded;
        qsizetype pos = 0;
        while(true)
        {
          const qsizetype line_end = payload.indexOf("\r\n", pos);
          if(line_end < 0)
            return out; // incomplete
          bool ok = false;
          const qsizetype chunk = QByteArray(payload.mid(pos, line_end - pos))
                                      .trimmed()
                                      .toLongLong(&ok, 16);
          if(!ok)
          {
            *complete = true;
            out.body = decoded;
            return out;
          }
          if(chunk == 0)
          {
            *complete = true;
            out.body = decoded;
            return out;
          }
          const qsizetype chunk_start = line_end + 2;
          if(payload.size() < chunk_start + chunk)
            return out; // incomplete
          decoded += payload.mid(chunk_start, chunk);
          pos = chunk_start + chunk + 2; // skip trailing CRLF
        }
      }

      if(out.status == 204 || out.status == 304 || out.status == 100)
      {
        *complete = true;
        return out;
      }
      const QByteArray cl = out.header("Content-Length");
      if(!cl.isEmpty())
      {
        const qsizetype len = cl.toLongLong();
        if(payload.size() >= len)
        {
          out.body = payload.left(len);
          *complete = true;
        }
        return out;
      }
      // No framing headers: fall back to read-until-close.
      out.body = payload;
      return out;
    }

  } // namespace detail

  inline RawResponse
  raw_request(quint16 port, const QByteArray& method, const QByteArray& path,
              const QList<QPair<QByteArray, QByteArray>>& headers,
              const QByteArray& body = {})
  {
    QTcpSocket sock;
    sock.connectToHost(QHostAddress::LocalHost, port);
    if(!sock.waitForConnected(5000))
      return {};

    QByteArray req = method + ' ' + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    bool has_connection = false;
    bool has_cl = false;
    for(const auto& [k, v] : headers)
    {
      req += k + ": " + v + "\r\n";
      if(k.toLower() == "connection")
        has_connection = true;
      if(k.toLower() == "content-length")
        has_cl = true;
    }
    if(!has_connection)
      req += "Connection: close\r\n";
    if(!has_cl && !body.isEmpty())
    {
      req += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    }
    req += "\r\n";
    req += body;
    sock.write(req);
    sock.flush();

    QByteArray data;
    QElapsedTimer timer;
    timer.start();
    RawResponse parsed;
    while(timer.elapsed() < 8000)
    {
      sock.waitForReadyRead(100);
      data += sock.readAll();
      bool complete = false;
      parsed = detail::parse_buffer(data, &complete);
      if(complete)
        break;
      if(sock.state() == QAbstractSocket::UnconnectedState)
      {
        data += sock.readAll();
        parsed = detail::parse_buffer(data, &complete);
        break;
      }
    }
    return parsed;
  }

} // namespace mcp::test
