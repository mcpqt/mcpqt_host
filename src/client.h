// SPDX-License-Identifier: AGPL-3.0-only
//
// Client: high-level MCP client façade. Composed on top of a Session.
//
// Typical usage:
//
//   mcp::Client client{ mcp::Implementation{
//       .name = "my-app", .version = "0.1.0",
//   }};
//   client.connect(std::make_unique<mcp::StdioTransport>(...));
//   auto info = client.initialize().get();
//   auto tools = client.list_tools().get();
//   auto out = client.call_tool("add", {{"a", 1}, {"b", 2}}).get();
//   client.shutdown();

#pragma once

#include "protocol.h"
#include "session.h"
#include "transport.h"

#include "json.h"

#include <QHash>
#include <QString>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace mcp {

class Client {
public:
    explicit Client(Implementation client_info);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    ~Client();

    /// Bind a transport and start the underlying session. Idempotent
    /// per Client instance — calling twice rebinds (after disconnect).
    void connect(std::unique_ptr<Transport> transport);

    /// Tear down: close the session and drop the transport.
    void disconnect(void);

    /// Send `initialize` and (on resolution) the `notifications/initialized`
    /// notification. The future resolves to the InitializeResult or
    /// throws on error.
    [[nodiscard]] std::future<InitializeResult> initialize(void);

    /// `tools/list`. Optional cursor for pagination (Phase 3 will fully
    /// support paginated iteration; for now you get the whole list).
    [[nodiscard]] std::future<ListToolsResult> list_tools(QString cursor = {});

    /// `tools/call` with the given name and JSON-shaped arguments.
    [[nodiscard]] std::future<CallToolResult>
    call_tool(QString name, QJsonValue arguments = {});

    /// `tools/call` with a `task` augmentation. The server returns a
    /// `CreateTaskResult` envelope right away; the actual
    /// `CallToolResult` is fetched later via `task_result()`. The
    /// server must advertise the tasks capability for tools/call.
    /// `ttl_ms` is the per-call retention override; nullopt uses the
    /// server's default.
    [[nodiscard]] std::future<CreateTaskResult>
    call_tool_as_task(QString name, QJsonValue arguments = {},
                      std::optional<std::int64_t> ttl_ms = std::nullopt);

    /// `tasks/get` — fetch the current envelope for a task.
    [[nodiscard]] std::future<Task> task_get(QString task_id);

    /// `tasks/result` — block on the server until the task is
    /// terminal, then return the underlying request's result as raw
    /// JSON. Decode it as the appropriate result type (e.g.
    /// `CallToolResult`) by calling `mcp::from_json<T>(...)`.
    [[nodiscard]] std::future<QJsonValue> task_result(QString task_id);

    /// `tasks/list` — paginated enumeration of all tasks the server
    /// currently knows about.
    [[nodiscard]] std::future<ListTasksResult> task_list(QString cursor = {});

    /// `tasks/cancel` — request cancellation. Returns the task
    /// envelope post-transition (status: cancelled).
    [[nodiscard]] std::future<Task> task_cancel(QString task_id);

    /// Hook for inbound `notifications/tasks/status`. The handler
    /// receives the latest Task projection on every transition.
    using TaskStatusHandler = std::function<void(const Task&)>;
    void set_task_status_handler(TaskStatusHandler handler);

    /// `resources/list`. Cursor optional; pagination support is
    /// callee-side for Phase 2.
    [[nodiscard]] std::future<ListResourcesResult>
    list_resources(QString cursor = {});

    /// `resources/templates/list`.
    [[nodiscard]] std::future<ListResourceTemplatesResult>
    list_resource_templates(QString cursor = {});

    /// `resources/read` for the given URI.
    [[nodiscard]] std::future<ReadResourceResult> read_resource(QString uri);

    /// `resources/subscribe` — request `notifications/resources/updated`
    /// frames whenever this URI's contents change. Subscribe handlers
    /// for incoming updates: see `set_resource_updated_handler`. The
    /// future resolves on success and throws mcp::Error on failure.
    [[nodiscard]] std::future<void> subscribe(QString uri);

    /// `resources/unsubscribe`.
    [[nodiscard]] std::future<void> unsubscribe(QString uri);

    /// Register a handler for inbound resource-update notifications.
    /// The handler is called whenever the server sends
    /// `notifications/resources/updated`.
    using ResourceUpdatedHandler =
        std::function<void(const ResourceUpdatedNotificationParams&)>;
    void set_resource_updated_handler(ResourceUpdatedHandler handler);

    /// Register a handler for inbound resource-list-changed notifications.
    using ListChangedHandler = std::function<void()>;
    void set_resources_list_changed_handler(ListChangedHandler handler);

    /// `prompts/list`.
    [[nodiscard]] std::future<ListPromptsResult>
    list_prompts(QString cursor = {});

    /// `prompts/get` with the given prompt name and (optional) string-keyed
    /// arguments to fill in template parameters.
    [[nodiscard]] std::future<GetPromptResult>
    get_prompt(QString name,
               std::optional<QHash<QString, QString>> arguments = std::nullopt);

    /// Hook for inbound notifications/prompts/list_changed.
    void set_prompts_list_changed_handler(ListChangedHandler handler);

    /// Send a `notifications/cancelled` for a previously-issued request.
    /// Phase 2 does not yet integrate cancellation with the futures
    /// returned by request methods — the caller must already know the
    /// id (typically obtained via a future Phase 3 cancellation token).
    /// Returns the underlying transport error_code on failure (e.g.
    /// transport closed); empty error_code on success.
    std::error_code cancel_request(RequestId request_id, QString reason = {});

    /// Send `ping` and wait for a response. Returns void on success;
    /// the future throws on timeout / error.
    [[nodiscard]] std::future<void> ping(void);

    /// Send `logging/setLevel` to ask the server to change its
    /// log-message threshold. The server may reject if it does not
    /// support the logging capability. The future resolves on success
    /// and throws mcp::Error on failure.
    [[nodiscard]] std::future<void> set_log_level(LoggingLevel level);

    /// Hook for inbound `notifications/message` (server-emitted log).
    using LogMessageHandler =
        std::function<void(const LoggingMessageNotificationParams&)>;
    void set_log_message_handler(LogMessageHandler handler);

    /// Hook for inbound `notifications/progress`.
    using ProgressHandler =
        std::function<void(const ProgressNotificationParams&)>;
    void set_progress_handler(ProgressHandler handler);

    /// Handler for `sampling/createMessage` requests from the server.
    /// Setting this also advertises the `sampling` capability when the
    /// next initialize() runs (see set_capabilities()).
    using SamplingHandler =
        std::function<CreateMessageResult(const CreateMessageRequestParams&)>;
    void set_sampling_handler(SamplingHandler handler);

    /// Handler for `roots/list` requests from the server. Setting this
    /// also advertises the `roots` capability.
    using RootsListHandler = std::function<ListRootsResult()>;
    void set_roots_list_handler(RootsListHandler handler);

    /// Handler for `elicitation/create` requests from the server. Setting
    /// this also advertises the `elicitation` capability (form+url) on
    /// the next initialize(). Use set_client_capabilities() to declare a
    /// narrower mode set.
    using ElicitationHandler =
        std::function<ElicitResult(const ElicitRequestParams&)>;
    void set_elicitation_handler(ElicitationHandler handler);

    /// Handler for `notifications/elicitation/complete` (URL-mode
    /// completion). Receives the elicitation_id of the flow that
    /// finished out of band. Pass nullptr to clear.
    using ElicitationCompleteHandler =
        std::function<void(QString elicitation_id)>;
    void set_elicitation_complete_handler(ElicitationCompleteHandler handler);

    /// Send `notifications/elicitation/complete` to the server. The
    /// spec lets either side emit it; the typical case is the client
    /// notifying the server once the OAuth redirect_uri (or other
    /// URL flow) completes.
    std::error_code notify_elicitation_complete(QString elicitation_id);

    /// `completion/complete` — ask the server for autocompletion
    /// suggestions.
    [[nodiscard]] std::future<CompleteResult>
    complete(CompletionReference reference, CompleteArgument argument,
             std::optional<QHash<QString, QString>> context_arguments =
                 std::nullopt);

    /// Layer extra / narrowed capabilities on top of the ones derived
    /// from registered handlers. Engaged fields in `caps` REPLACE the
    /// corresponding derived field; unset fields leave the derived
    /// value alone. Typical uses: narrow `elicitation` to form-mode
    /// only (`{form:{}, url:nullopt}`), attach an `experimental`
    /// blob, or override `tasks` to advertise additional augmentable
    /// methods.
    void set_client_capabilities(ClientCapabilities caps);

    /// Notify the server that the client's roots list changed.
    /// Returns the underlying transport error_code on failure.
    std::error_code notify_roots_list_changed(void);

    /// True between connect() and disconnect().
    [[nodiscard]] bool is_connected(void) const noexcept;

    /// Cached server information from the last successful initialize().
    [[nodiscard]] std::optional<InitializeResult> server(void) const;

private:
    /// Acquire a strong reference to the live Session, or empty if
    /// disconnected. Centralises the lock+copy that protects against
    /// races with disconnect() resetting `m_session`. Returns by value
    /// so callers can drop the lock immediately and still keep the
    /// session alive for the duration of their operation.
    [[nodiscard]] std::shared_ptr<Session> acquire_session(void) const;

    Implementation m_client_info;
    std::shared_ptr<Session> m_session;
    mutable std::mutex m_session_mu;
    std::optional<InitializeResult> m_server;
    mutable std::mutex m_server_mu;
    std::atomic<bool> m_connected{false};

    SamplingHandler m_sampling_handler;
    RootsListHandler m_roots_handler;
    ElicitationHandler m_elicitation_handler;
    ElicitationCompleteHandler m_elicitation_complete_handler;
    TaskStatusHandler m_task_status_handler;
    std::optional<ClientCapabilities> m_capabilities_override;
    std::mutex m_handlers_mu;
};

}  // namespace mcp
