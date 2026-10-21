// SPDX-License-Identifier: AGPL-3.0-only
#include "client.h"

#include "error.h"
#include "log.h"
#include "protocol.h"
#include "session.h"
#include "transport.h"

#include <QHash>
#include <QString>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace mcp
{

  Client::Client(Implementation client_info)
      : m_client_info(std::move(client_info))
  {
  }

  Client::~Client() { disconnect(); }

  void Client::connect(std::unique_ptr<Transport> transport)
  {
    if(!transport)
    {
      throw Error{ error_code::internal_error,
                   "Client::connect: transport is null" };
    }
    disconnect();
    {
      std::lock_guard<std::mutex> lk(m_session_mu);
      m_session = std::make_shared<Session>(std::move(transport));
    }
    // Start under the lock-acquired pointer, but don't hold the lock
    // across start() — start spawns the read thread, which can run
    // arbitrary user callbacks.
    auto local = acquire_session();
    if(local)
      local->start();
    m_connected.store(true, std::memory_order_release);
  }

  void Client::disconnect(void)
  {
    if(!m_connected.exchange(false, std::memory_order_acq_rel))
      return;
    std::shared_ptr<Session> local;
    {
      std::lock_guard<std::mutex> lk(m_session_mu);
      local = std::move(m_session);
      m_session.reset();
    }
    // close() + drop our reference outside the lock; concurrent
    // call sites that took an earlier copy will keep the Session
    // alive via shared_ptr count until they finish.
    if(local)
      local->close();
    {
      std::lock_guard<std::mutex> lk(m_server_mu);
      m_server.reset();
    }
  }

  std::shared_ptr<Session> Client::acquire_session(void) const
  {
    std::lock_guard<std::mutex> lk(m_session_mu);
    return m_session;
  }

  bool Client::is_connected(void) const noexcept
  {
    return m_connected.load(std::memory_order_acquire);
  }

  std::optional<InitializeResult> Client::server(void) const
  {
    std::lock_guard<std::mutex> lk(m_server_mu);
    return m_server;
  }

  // =====================================================================
  // initialize
  // =====================================================================

  std::future<InitializeResult> Client::initialize(void)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    ClientCapabilities caps;
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      // First derive caps from the registered handlers...
      if(m_sampling_handler)
        caps.sampling = SamplingCapability{};
      if(m_roots_handler)
        caps.roots = RootsCapability{};
      if(m_elicitation_handler)
      {
        ElicitationCapability ec;
        ec.form = QJsonObject{};
        ec.url = QJsonObject{};
        caps.elicitation = std::move(ec);
      }
      // ...then merge any explicit override on top. Each engaged
      // override field replaces the corresponding derived one
      // (typical use: narrow elicitation modes, or attach
      // experimental keys); fields the override leaves unset
      // keep their handler-derived values rather than getting
      // wiped out, which is what users expect from "override"
      // semantics.
      if(m_capabilities_override.has_value())
      {
        const auto& o = *m_capabilities_override;
        if(!o.experimental.isNull())
          caps.experimental = o.experimental;
        if(o.roots.has_value())
          caps.roots = o.roots;
        if(o.sampling.has_value())
          caps.sampling = o.sampling;
        if(o.elicitation.has_value())
          caps.elicitation = o.elicitation;
        if(o.tasks.has_value())
          caps.tasks = o.tasks;
      }
    }
    InitializeRequestParams params{
      .protocol_version = QString{ kLatestProtocolVersion },
      .capabilities = std::move(caps),
      .client_info = m_client_info,
    };

    return std::async(
        std::launch::async,
        [this, session, payload = to_json(params)]() -> InitializeResult {
          auto raw
              = session->send_request(QString{ method_initialize }, payload)
                    .get();
          auto res = from_json<InitializeResult>(raw);
          {
            std::lock_guard<std::mutex> lk(m_server_mu);
            m_server = res;
          }
          const auto note_ec
              = session->send_notification(method_notifications_initialized);
          if(note_ec)
          {
            throw Error{ error_code::internal_error,
                         QStringLiteral(
                             "failed to send notifications/initialized: ")
                             + note_ec.message().c_str() };
          }
          return res;
        });
  }

  // =====================================================================
  // tools
  // =====================================================================

  std::future<ListToolsResult> Client::list_tools(QString cursor)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    ListToolsRequestParams params{ .cursor = std::move(cursor) };
    return session->send_request_for<ListToolsResult>(
        QString{ method_tools_list }, to_json(params),
        [](QJsonValue j) { return from_json<ListToolsResult>(j); });
  }

  std::future<CallToolResult> Client::call_tool(QString name,
                                                QJsonValue arguments)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    CallToolRequestParams params{
      .name = std::move(name),
      .arguments = arguments.isNull() ? QJsonValue() : arguments,
      .task = std::nullopt,
    };
    return session->send_request_for<CallToolResult>(
        QString{ method_tools_call }, to_json(params),
        [](QJsonValue j) { return from_json<CallToolResult>(j); });
  }

  std::future<CreateTaskResult>
  Client::call_tool_as_task(QString name, QJsonValue arguments,
                            std::optional<std::int64_t> ttl_ms)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    CallToolRequestParams params{
      .name = std::move(name),
      .arguments = arguments.isNull() ? QJsonValue() : arguments,
      .task = TaskAugmentation{ .ttl = ttl_ms },
    };
    return session->send_request_for<CreateTaskResult>(
        QString{ method_tools_call }, to_json(params),
        [](QJsonValue j) { return from_json<CreateTaskResult>(j); });
  }

  std::future<Task> Client::task_get(QString task_id)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    return session->send_request_for<Task>(
        QString{ method_tasks_get },
        to_json(GetTaskRequestParams{ .task_id = std::move(task_id) }),
        [](QJsonValue j) { return from_json<Task>(j); });
  }

  std::future<QJsonValue> Client::task_result(QString task_id)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    // Already a future<json>; no conversion needed, so return the
    // Session future directly (no per-call worker thread).
    return session->send_request(
        QString{ method_tasks_result },
        to_json(GetTaskResultRequestParams{ .task_id = std::move(task_id) }));
  }

  std::future<ListTasksResult> Client::task_list(QString cursor)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    ListTasksRequestParams params{ .cursor = std::move(cursor) };
    return session->send_request_for<ListTasksResult>(
        QString{ method_tasks_list }, to_json(params),
        [](QJsonValue j) { return from_json<ListTasksResult>(j); });
  }

  std::future<Task> Client::task_cancel(QString task_id)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    return session->send_request_for<Task>(
        QString{ method_tasks_cancel },
        to_json(CancelTaskRequestParams{ .task_id = std::move(task_id) }),
        [](QJsonValue j) { return from_json<Task>(j); });
  }

  void Client::set_task_status_handler(TaskStatusHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      m_task_status_handler = handler;
    }
    const QString method{ method_notifications_tasks_status };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue& params) {
            if(params.isNull())
              return;
            Task projection;
            try
            {
              // notifications/tasks/status carries the Task
              // projection inline (no wrapper object).
              projection = from_json<Task>(params);
            }
            catch(...)
            {
              return;
            }
            h(projection);
          });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  // =====================================================================
  // resources
  // =====================================================================

  std::future<ListResourcesResult> Client::list_resources(QString cursor)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    ListResourcesRequestParams params{ .cursor = std::move(cursor) };
    return session->send_request_for<ListResourcesResult>(
        QString{ method_resources_list }, to_json(params),
        [](QJsonValue j) { return from_json<ListResourcesResult>(j); });
  }

  std::future<ListResourceTemplatesResult>
  Client::list_resource_templates(QString cursor)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    ListResourceTemplatesRequestParams params{ .cursor = std::move(cursor) };
    return session->send_request_for<ListResourceTemplatesResult>(
        QString{ method_resources_templates_list }, to_json(params),
        [](QJsonValue j) {
          return from_json<ListResourceTemplatesResult>(j);
        });
  }

  std::future<ReadResourceResult> Client::read_resource(QString uri)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    ReadResourceRequestParams params{ .uri = std::move(uri) };
    return session->send_request_for<ReadResourceResult>(
        QString{ method_resources_read }, to_json(params),
        [](QJsonValue j) { return from_json<ReadResourceResult>(j); });
  }

  std::future<void> Client::subscribe(QString uri)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    SubscribeRequestParams params{ .uri = std::move(uri) };
    return session->send_request_void(QString{ method_resources_subscribe },
                                      to_json(params));
  }

  std::future<void> Client::unsubscribe(QString uri)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    UnsubscribeRequestParams params{ .uri = std::move(uri) };
    return session->send_request_void(QString{ method_resources_unsubscribe },
                                      to_json(params));
  }

  void Client::set_resource_updated_handler(ResourceUpdatedHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    const QString method{ method_notifications_resources_updated };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue& params) {
            if(params.isNull())
              return;
            h(from_json<ResourceUpdatedNotificationParams>(params));
          });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  void Client::set_resources_list_changed_handler(ListChangedHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    const QString method{ method_notifications_resources_list_changed };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue&) { h(); });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  // =====================================================================
  // prompts
  // =====================================================================

  std::future<ListPromptsResult> Client::list_prompts(QString cursor)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    ListPromptsRequestParams params{ .cursor = std::move(cursor) };
    return session->send_request_for<ListPromptsResult>(
        QString{ method_prompts_list }, to_json(params),
        [](QJsonValue j) { return from_json<ListPromptsResult>(j); });
  }

  std::future<GetPromptResult>
  Client::get_prompt(QString name,
                     std::optional<QHash<QString, QString>> arguments)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    GetPromptRequestParams params{
      .name = std::move(name),
      .arguments = std::move(arguments),
    };
    return session->send_request_for<GetPromptResult>(
        QString{ method_prompts_get }, to_json(params),
        [](QJsonValue j) { return from_json<GetPromptResult>(j); });
  }

  void Client::set_prompts_list_changed_handler(ListChangedHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    const QString method{ method_notifications_prompts_list_changed };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue&) { h(); });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  // =====================================================================
  // Cancellation, ping, log, progress
  // =====================================================================

  std::error_code Client::cancel_request(RequestId request_id, QString reason)
  {
    auto session = acquire_session();
    if(!session)
    {
      throw Error{ error_code::internal_error, "client not connected" };
    }
    CancelledNotificationParams params{
      .request_id = std::move(request_id),
      .reason = std::move(reason),
    };
    return session->send_notification(
        QString{ method_notifications_cancelled }, to_json(params));
  }

  std::future<void> Client::ping(void)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    return session->send_request_void(QString{ method_ping }, {});
  }

  std::future<void> Client::set_log_level(LoggingLevel level)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    SetLevelRequestParams params{ .level = level };
    return session->send_request_void(QString{ method_logging_set_level },
                                      to_json(params));
  }

  void Client::set_log_message_handler(LogMessageHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    const QString method{ method_notifications_message };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue& params) {
            if(params.isNull())
              return;
            h(from_json<LoggingMessageNotificationParams>(params));
          });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  void Client::set_progress_handler(ProgressHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    const QString method{ method_notifications_progress };
    if(handler)
    {
      session->set_notification_handler(
          method, [h = std::move(handler)](const QJsonValue& params) {
            if(params.isNull())
              return;
            h(from_json<ProgressNotificationParams>(params));
          });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  // =====================================================================
  // Sampling, roots, completion
  // =====================================================================

  void Client::set_sampling_handler(SamplingHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      m_sampling_handler = handler;
    }
    const QString method{ method_sampling_create_message };
    if(handler)
    {
      session->set_request_handler(
          method,
          [h = std::move(handler)](const QJsonValue& params) -> QJsonValue {
            if(params.isNull())
            {
              throw Error{ error_code::invalid_params,
                           "sampling/createMessage requires params" };
            }
            CreateMessageRequestParams parsed;
            try
            {
              parsed = from_json<CreateMessageRequestParams>(params);
            }
            catch(const std::exception& e)
            {
              throw Error{ error_code::invalid_params,
                           QString{ "invalid sampling params: " } + e.what() };
            }
            return to_json(h(parsed));
          });
    }
    else
    {
      session->clear_request_handler(method);
    }
  }

  void Client::set_elicitation_handler(ElicitationHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      m_elicitation_handler = handler;
    }
    const QString method{ method_elicitation_create };
    if(handler)
    {
      session->set_request_handler(
          method,
          [h = std::move(handler)](const QJsonValue& params) -> QJsonValue {
            if(params.isNull())
            {
              throw Error{ error_code::invalid_params,
                           "elicitation/create requires params" };
            }
            ElicitRequestParams parsed;
            try
            {
              parsed = from_json<ElicitRequestParams>(params);
            }
            catch(const Error&)
            {
              throw;
            }
            catch(const std::exception& e)
            {
              throw Error{ error_code::invalid_params,
                           QString{ "invalid elicitation params: " }
                               + e.what() };
            }
            return to_json(h(parsed));
          });
    }
    else
    {
      session->clear_request_handler(method);
    }
  }

  void
  Client::set_elicitation_complete_handler(ElicitationCompleteHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      m_elicitation_complete_handler = handler;
    }
    const QString method{ method_notifications_elicitation_complete };
    if(handler)
    {
      session->set_notification_handler(method, [h = std::move(handler)](
                                                    const QJsonValue& params) {
        if(!params.isObject())
          return;
        ElicitationCompleteNotificationParams parsed;
        try
        {
          parsed = from_json<ElicitationCompleteNotificationParams>(params);
        }
        catch(const std::exception& e)
        {
          MCP_LOG_WARN(QString{ "malformed elicitation/complete: " }
                       + e.what());
          return;
        }
        catch(...)
        {
          MCP_LOG_WARN("malformed elicitation/complete");
          return;
        }
        h(std::move(parsed.elicitation_id));
      });
    }
    else
    {
      session->clear_notification_handler(method);
    }
  }

  void Client::set_roots_list_handler(RootsListHandler handler)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    {
      std::lock_guard<std::mutex> lk(m_handlers_mu);
      m_roots_handler = handler;
    }
    const QString method{ method_roots_list };
    if(handler)
    {
      session->set_request_handler(
          method, [h = std::move(handler)](const QJsonValue&) -> QJsonValue {
            return to_json(h());
          });
    }
    else
    {
      session->clear_request_handler(method);
    }
  }

  std::future<CompleteResult>
  Client::complete(CompletionReference reference, CompleteArgument argument,
                   std::optional<QHash<QString, QString>> context_arguments)
  {
    auto session = acquire_session();
    if(!session)
      throw Error{ error_code::internal_error, "client not connected" };
    CompleteRequestParams params{
      .reference = std::move(reference),
      .argument = std::move(argument),
      .context_arguments = std::move(context_arguments),
    };
    return session->send_request_for<CompleteResult>(
        QString{ method_completion_complete }, to_json(params),
        [](QJsonValue j) { return from_json<CompleteResult>(j); });
  }

  void Client::set_client_capabilities(ClientCapabilities caps)
  {
    std::lock_guard<std::mutex> lk(m_handlers_mu);
    m_capabilities_override = std::move(caps);
  }

  std::error_code Client::notify_roots_list_changed(void)
  {
    auto session = acquire_session();
    if(!session)
    {
      return std::make_error_code(std::errc::not_connected);
    }
    return session->send_notification(
        QString{ method_notifications_roots_list_changed });
  }

  std::error_code Client::notify_elicitation_complete(QString elicitation_id)
  {
    auto session = acquire_session();
    if(!session)
      return std::make_error_code(std::errc::not_connected);
    return session->send_notification(
        QString{ method_notifications_elicitation_complete },
        to_json(ElicitationCompleteNotificationParams{
            .elicitation_id = std::move(elicitation_id),
        }));
  }

} // namespace mcp
