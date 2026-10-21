// SPDX-License-Identifier: AGPL-3.0-only
#include "session.h"

#include "error.h"
#include "log.h"
#include "protocol.h"
#include "transport.h"

#include <QHash>
#include <QList>
#include <QString>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace mcp
{

  using std::chrono::steady_clock;

  // =====================================================================
  // Construction / destruction
  // =====================================================================

  Session::Session(std::unique_ptr<Transport> transport)
      : Session(std::move(transport), Options{})
  {
  }

  Session::Session(std::unique_ptr<Transport> transport, Options opts)
      : m_transport(std::move(transport)), m_opts(opts)
  {
    if(!m_transport)
    {
      throw Error{ error_code::internal_error, "Session: transport is null" };
    }
    m_transport->on_message(
        [this](QByteArray raw) { this->handle_frame(std::move(raw)); });
    m_transport->on_error([](std::error_code ec) {
      MCP_LOG_WARN(QString{ "transport error: " } + ec.message().c_str());
    });
    m_transport->on_close([this]() {
      ErrorObject reason{
        error_code::internal_error,
        "transport closed",
        {},
      };
      cancel_all_pending(reason);
      // Mark the session closed so is_open() flips, and notify any
      // user code waiting on the close hook (e.g. Server::run).
      m_closed.store(true, std::memory_order_release);
      ClosedCallback cb;
      {
        std::lock_guard<std::mutex> lk(m_closed_cb_mu);
        if(!m_closed_cb_fired.exchange(true, std::memory_order_acq_rel))
        {
          cb = m_on_closed;
        }
      }
      if(cb)
      {
        try
        {
          cb();
        }
        catch(const std::exception& e)
        {
          MCP_LOG_ERROR(QString{ "on_closed threw: " } + e.what());
        }
      }
    });
  }

  Session::~Session() { close(); }

  // =====================================================================
  // Handlers
  // =====================================================================

  void Session::set_request_handler(QString method, RequestHandler h)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_req_handlers[std::move(method)] = std::move(h);
  }

  void Session::set_notification_handler(QString method, NotificationHandler h)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_note_handlers[std::move(method)] = std::move(h);
  }

  void Session::clear_request_handler(const QString& method)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_req_handlers.remove(method);
  }

  void Session::clear_notification_handler(const QString& method)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_note_handlers.remove(method);
  }

  void Session::set_fallback_request_handler(RequestHandler h)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_fallback_req = std::move(h);
  }

  void Session::set_fallback_notification_handler(NotificationHandler h)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_fallback_note = std::move(h);
  }

  void Session::set_on_closed(ClosedCallback cb)
  {
    std::lock_guard<std::mutex> lk(m_closed_cb_mu);
    m_on_closed = std::move(cb);
  }

  // =====================================================================
  // Lifecycle
  // =====================================================================

  void Session::start(void)
  {
    bool expected = false;
    if(!m_started.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel))
    {
      return;
    }
    m_closed.store(false, std::memory_order_release);
    m_timeout_thread = std::thread([this] { this->timeout_loop(); });
    m_transport->start();
  }

  void Session::close(void)
  {
    // Two callers can land here: (1) the application calling close()
    // directly, (2) the Session destructor. Whichever arrives first
    // tears down the transport; both must run the rest so that
    // double-destruction can't leave the timeout thread joinable
    // (which would call std::terminate from ~thread).
    bool expected = false;
    const bool first = m_closed.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel);

    // Cancel pending request promises so any worker awaiting a future
    // (e.g. server.sample()) unblocks and the worker can return.
    cancel_all_pending(ErrorObject{
        error_code::internal_error,
        "session closed",
        {},
    });

    // Wait for in-flight request workers to finish before we close
    // the transport. Workers may still be writing their final
    // responses to stdout, and closing the transport here would turn
    // those writes into "not_connected" errors.
    {
      std::unique_lock<std::mutex> lk(m_workers_mu);
      m_workers_cv.wait(lk, [this] { return m_workers_running == 0; });
    }

    // Now actually shut down the transport. Only the first close()
    // does this; subsequent calls (e.g. from ~Session) ride through
    // the join-and-fire path the transport already provides.
    if(first && m_transport)
      m_transport->close();

    {
      std::lock_guard<std::mutex> lk(m_pending_mu);
      m_timeout_cv.notify_all();
    }
    if(m_timeout_thread.joinable())
      m_timeout_thread.join();
  }

  // =====================================================================
  // Send paths
  // =====================================================================

  RequestId Session::next_id(void) noexcept
  {
    return RequestId{ m_next_id.fetch_add(1, std::memory_order_relaxed) };
  }

  std::future<QJsonValue>
  Session::send_request(QString method, QJsonValue params,
                        std::chrono::milliseconds timeout)
  {
    return send_request_for<QJsonValue>(
        std::move(method), std::move(params), [](QJsonValue j) { return j; },
        timeout);
  }

  std::future<void>
  Session::send_request_void(QString method, QJsonValue params,
                             std::chrono::milliseconds timeout)
  {
    auto pr = std::make_shared<std::promise<void>>();
    auto fut = pr->get_future();
    Pending pending;
    pending.resolve = [pr](QJsonValue) { pr->set_value(); };
    pending.reject = [pr](std::exception_ptr e) { pr->set_exception(e); };
    register_pending(std::move(method), std::move(params), std::move(pending),
                     timeout);
    return fut;
  }

  void Session::register_pending(QString method, QJsonValue params,
                                 Pending pending,
                                 std::chrono::milliseconds timeout)
  {
    auto id = next_id();

    JsonRpcRequest req;
    req.id = id;
    req.method = std::move(method);
    if(!params.isNull())
      req.params = std::move(params);

    const auto effective_timeout = timeout == std::chrono::milliseconds{ 0 }
                                       ? m_opts.default_request_timeout
                                       : timeout;
    pending.deadline = steady_clock::now() + effective_timeout;

    // Keep a copy of the reject callback for the error paths below, which
    // must fire *after* releasing m_pending_mu (resolving the caller's
    // future can wake a waiter that re-enters the Session).
    auto reject = pending.reject;

    enum class Parked
    {
      kOk,
      kClosed,
      kDuplicate
    };
    Parked state;
    {
      std::lock_guard<std::mutex> lk(m_pending_mu);
      if(m_closed.load(std::memory_order_acquire))
      {
        // Don't park a new entry on a closed session — its future
        // would never resolve, since cancel_all_pending already
        // drained the map.
        state = Parked::kClosed;
      }
      else
      {
        if(!m_pending.contains(id))
        {
          m_pending.insert(id, std::move(pending));
          state = Parked::kOk;
          m_timeout_cv.notify_all();
        }
        else
        {
          // The id space is monotonic per Session, so this is a
          // sign of corruption (e.g. a Session reused across
          // logical connections). Fail fast.
          state = Parked::kDuplicate;
        }
      }
    }

    if(state == Parked::kClosed)
    {
      reject(std::make_exception_ptr(
          Error{ error_code::internal_error, "session closed" }));
      return;
    }
    if(state == Parked::kDuplicate)
    {
      reject(std::make_exception_ptr(
          Error{ error_code::internal_error,
                 "internal: duplicate request id " + id.canonical() }));
      return;
    }

    try
    {
      send_message(req);
    }
    catch(...)
    {
      // The transport refused. Pull the pending entry back out and
      // surface the error through its future — unless a response or
      // timeout raced us to it first.
      Pending parked;
      bool found = false;
      {
        std::lock_guard<std::mutex> lk(m_pending_mu);
        auto it = m_pending.find(id);
        if(it != m_pending.end())
        {
          parked = std::move(it.value());
          m_pending.erase(it);
          found = true;
        }
      }
      if(found && parked.reject)
        parked.reject(std::current_exception());
    }
  }

  std::error_code Session::send_notification(QString method, QJsonValue params)
  {
    JsonRpcNotification note;
    note.method = std::move(method);
    if(!params.isNull())
      note.params = std::move(params);
    try
    {
      send_message(note);
    }
    catch(const std::system_error& e)
    {
      return e.code();
    }
    catch(...)
    {
      return std::make_error_code(std::errc::io_error);
    }
    return {};
  }

  void Session::send_message(const JsonRpcMessage& msg)
  {
    const auto j = serialize_message(msg);
    const QByteArray frame = dump(j).toUtf8();
    auto ec = m_transport->send(frame);
    if(ec)
    {
      throw std::system_error{
        ec, "Session::send_message: transport->send failed"
      };
    }
  }

  // =====================================================================
  // Inbound dispatch
  // =====================================================================

  void Session::handle_frame(QByteArray raw)
  {
    QJsonValue j;
    try
    {
      j = parse_json(raw);
    }
    catch(const Error& e)
    {
      MCP_LOG_WARN(QString{ "rejecting non-JSON frame: " } + e.what());
      return; // JSON-RPC says we send a parse error response, but per the
              // spec we cannot when there is no id; simplest is to drop.
    }

    JsonRpcMessage msg;
    try
    {
      msg = parse_message(j);
    }
    catch(const Error& e)
    {
      MCP_LOG_WARN(QString{ "rejecting malformed JSON-RPC frame: " }
                   + e.what());
      return;
    }
    catch(const std::exception& e)
    {
      // A from_json deeper in parse_message can throw when fields are
      // mistyped or missing. Treat those identically to a parse error
      // and drop the frame.
      MCP_LOG_WARN(QString{ "rejecting malformed JSON-RPC frame: " }
                   + e.what());
      return;
    }

    std::visit(
        [&](auto&& m) {
          using T = std::decay_t<decltype(m)>;
          if constexpr(std::is_same_v<T, JsonRpcRequest>)
          {
            // Dispatch requests to a detached worker so user handlers
            // can themselves issue further requests on this Session
            // (e.g. server.sample() inside a tool handler) without
            // deadlocking the read thread.
            //
            // close() waits for `m_workers_running` to drain before
            // returning so the dispatched worker can never outlive
            // members it touches (m_transport, handlers).
            {
              std::lock_guard<std::mutex> lk(m_workers_mu);
              ++m_workers_running;
            }
            std::thread([this, req = std::move(m)]() mutable {
              handle_request(std::move(req));
              // Decrement and notify under the same lock. If the
              // unlock happened first, close() could observe
              // m_workers_running==0, return, and destroy the cv
              // before this thread reaches notify_all — TSan
              // catches that race against pthread_cond_destroy.
              std::lock_guard<std::mutex> lk(m_workers_mu);
              --m_workers_running;
              m_workers_cv.notify_all();
            }).detach();
          }
          else if constexpr(std::is_same_v<T, JsonRpcNotification>)
          {
            handle_notification(std::move(m));
          }
          else
          {
            handle_response(std::move(m));
          }
        },
        msg);
  }

  void Session::handle_request(JsonRpcRequest req)
  {
    RequestHandler handler;
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      auto it = m_req_handlers.find(req.method);
      if(it != m_req_handlers.end())
        handler = it.value();
      else
        handler = m_fallback_req;
    }

    JsonRpcResponse resp;
    resp.id = req.id;

    if(!handler)
    {
      resp.outcome = ErrorObject{
        error_code::method_not_found,
        "method not found: " + req.method,
        {},
      };
    }
    else
    {
      try
      {
        static const QJsonValue kNull{ QJsonValue::Null };
        const QJsonValue& params_in = req.params.isNull() ? kNull : req.params;
        resp.outcome = handler(params_in);
      }
      catch(const Error& e)
      {
        if(e.code() == error_code::parse_error)
        {
          // A codec error escaping a handler almost always means the
          // inbound params were the wrong shape; map to invalid_params
          // rather than parse_error so clients can fix their request.
          resp.outcome = ErrorObject{
            error_code::invalid_params,
            QString{ "invalid params: " } + e.what(),
            {},
          };
        }
        else
        {
          resp.outcome = e.object();
        }
      }
      catch(const std::exception& e)
      {
        resp.outcome = ErrorObject{
          error_code::internal_error,
          QString{ "handler threw: " } + e.what(),
          {},
        };
      }
      catch(...)
      {
        resp.outcome = ErrorObject{
          error_code::internal_error,
          "handler threw a non-std exception",
          {},
        };
      }
    }

    try
    {
      send_message(resp);
    }
    catch(const std::exception& e)
    {
      MCP_LOG_ERROR(QString{ "failed to send response: " } + e.what());
    }
  }

  void Session::handle_notification(JsonRpcNotification note)
  {
    NotificationHandler handler;
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      auto it = m_note_handlers.find(note.method);
      if(it != m_note_handlers.end())
        handler = it.value();
      else
        handler = m_fallback_note;
    }
    if(!handler)
      return;
    try
    {
      static const QJsonValue kNull{ QJsonValue::Null };
      handler(note.params.isNull() ? kNull : note.params);
    }
    catch(const std::exception& e)
    {
      MCP_LOG_ERROR(QString{ "notification handler threw: " } + e.what());
    }
    catch(...)
    {
      MCP_LOG_ERROR("notification handler threw a non-std exception");
    }
  }

  void Session::handle_response(JsonRpcResponse resp)
  {
    if(resp.id.isNull())
    {
      // Per spec, an error response with no id can occur on parse errors
      // we issued; nothing to correlate against.
      if(resp.is_error())
      {
        MCP_LOG_WARN(QString{ "received untargeted error: " }
                     + resp.error().message);
      }
      return;
    }

    Pending pending;
    {
      std::lock_guard<std::mutex> lk(m_pending_mu);
      auto it = m_pending.find(resp.id);
      if(it == m_pending.end())
      {
        MCP_LOG_WARN("received response for unknown id: "
                     + resp.id.canonical());
        return;
      }
      pending = std::move(it.value());
      m_pending.erase(it);
      m_timeout_cv.notify_all();
    }

    // Resolve outside m_pending_mu (released above): the conversion +
    // promise resolution can wake a waiter that re-enters the Session.
    if(resp.is_success())
    {
      if(pending.resolve)
        pending.resolve(std::move(resp).result());
    }
    else
    {
      if(pending.reject)
        pending.reject(std::make_exception_ptr(Error{ resp.error() }));
    }
  }

  // =====================================================================
  // Timeout
  // =====================================================================

  void Session::timeout_loop(void)
  {
    std::unique_lock<std::mutex> lk(m_pending_mu);
    while(!m_closed.load(std::memory_order_acquire))
    {
      if(m_pending.empty())
      {
        m_timeout_cv.wait(lk);
        continue;
      }

      auto soonest = steady_clock::time_point::max();
      for(auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
      {
        if(it.value().deadline < soonest)
          soonest = it.value().deadline;
      }
      if(m_timeout_cv.wait_until(lk, soonest) == std::cv_status::timeout)
      {
        const auto now = steady_clock::now();
        // Snapshot the expired entries under the lock, drop them
        // out of the pending map, then release the lock before
        // rejecting. Resolving a future can run user continuations,
        // which we never want to do while holding our own mutex;
        // snapshotting into a local vector also means we don't have
        // to keep an iterator valid across cancel_all_pending
        // swapping the map under us.
        QList<std::function<void(std::exception_ptr)>> expired;
        for(auto it = m_pending.begin(); it != m_pending.end();)
        {
          if(it.value().deadline <= now)
          {
            if(it.value().reject)
              expired.push_back(std::move(it.value().reject));
            it = m_pending.erase(it);
          }
          else
          {
            ++it;
          }
        }
        lk.unlock();
        for(auto& reject : expired)
        {
          reject(std::make_exception_ptr(
              Error{ error_code::internal_error, "request timed out" }));
        }
        lk.lock();
      }
    }
  }

  void Session::cancel_all_pending(const ErrorObject& reason)
  {
    QHash<RequestId, Pending> drained;
    {
      std::lock_guard<std::mutex> lk(m_pending_mu);
      drained.swap(m_pending);
      m_timeout_cv.notify_all();
    }
    for(auto it = drained.begin(); it != drained.end(); ++it)
    {
      if(it.value().reject)
        it.value().reject(std::make_exception_ptr(Error{ reason }));
    }
  }

} // namespace mcp
