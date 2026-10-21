// SPDX-License-Identifier: AGPL-3.0-only
//
// Wire types for the Model Context Protocol, revision 2025-11-25.
//
// The C++ struct fields are snake_case; the JSON wire format uses the spec's
// camelCase names. JSON serialization is implemented via ADL `to_json` /
// `from_json` free functions defined alongside each type so they can be
// reached through the generic codec in <json.h>.
//
// Encoding:  QJsonValue to_json(const T&);
// Decoding:  void from_json(const QJsonValue&, T&);
//
// Forward compatibility: unknown JSON fields are silently dropped on read.
// Optional spec fields are represented with `std::optional<T>`; their
// absence on the wire matches `std::nullopt` in C++. Capability presence is
// itself optional ("the capability is offered iff the optional has a value").

#pragma once

#include "error.h"
#include "json.h"

#include <QHash>
#include <QList>
#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mcp {

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

inline const QString kLatestProtocolVersion = "2025-11-25";
inline const QString kJsonrpcVersion = "2.0";

// --------------------------------------------------------------------------
// JSON-RPC envelope
// --------------------------------------------------------------------------

/// A JSON-RPC request id. The spec permits string or number; we preserve the
/// original kind on round-trip so a client that emits integer ids gets back
/// integer ids, and likewise for strings.
class RequestId {
public:
    using Value = std::variant<QString, std::int64_t>;

    RequestId(void) : m_value(std::int64_t{0}) {}
    RequestId(QString s) : m_value(std::move(s)) {}
    RequestId(const char* s) : m_value(QString{s}) {}
    RequestId(std::int64_t i) : m_value(i) {}
    RequestId(int i) : m_value(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] const Value& value(void) const noexcept { return m_value; }

    [[nodiscard]] bool is_string(void) const noexcept {
        return m_value.index() == 0;
    }
    [[nodiscard]] bool is_integer(void) const noexcept {
        return m_value.index() == 1;
    }

    [[nodiscard]] const QString& as_string(void) const {
        return std::get<0>(m_value);
    }
    [[nodiscard]] std::int64_t as_integer(void) const {
        return std::get<1>(m_value);
    }

    /// Canonical string form used for hash/map keys. String ids are returned
    /// as-is with a leading 's:' marker; integer ids get an 'i:' marker. This
    /// guarantees that the integer 5 and the string "5" do not collide.
    [[nodiscard]] QString canonical(void) const;

    friend bool operator==(const RequestId&, const RequestId&) = default;

private:
    Value m_value;
};

[[nodiscard]] QJsonValue to_json(const RequestId& id);
void from_json(const QJsonValue& j, RequestId& id);

/// A JSON-RPC 2.0 request. Always has an id.
struct JsonRpcRequest {
    RequestId id;
    QString method;
    std::optional<QJsonValue> params = {};
};

[[nodiscard]] QJsonValue to_json(const JsonRpcRequest& r);
void from_json(const QJsonValue& j, JsonRpcRequest& r);

/// A JSON-RPC 2.0 notification. Has no id; sender expects no response.
struct JsonRpcNotification {
    QString method;
    std::optional<QJsonValue> params = {};
};

[[nodiscard]] QJsonValue to_json(const JsonRpcNotification& n);
void from_json(const QJsonValue& j, JsonRpcNotification& n);

/// A JSON-RPC 2.0 response. Carries either a `result` or an `error`. Per
/// the spec the id is REQUIRED on success, and `null`/absent only on parse
/// errors that prevented identifying the request — we model that with
/// `std::optional<RequestId>`.
struct JsonRpcResponse {
    std::optional<RequestId> id = {};
    std::variant<QJsonValue /*result*/, ErrorObject /*error*/> outcome;

    [[nodiscard]] bool is_error(void) const noexcept {
        return outcome.index() == 1;
    }
    [[nodiscard]] bool is_success(void) const noexcept {
        return outcome.index() == 0;
    }

    [[nodiscard]] const QJsonValue& result(void) const {
        return std::get<0>(outcome);
    }
    [[nodiscard]] const ErrorObject& error(void) const {
        return std::get<1>(outcome);
    }
};

[[nodiscard]] QJsonValue to_json(const JsonRpcResponse& r);
void from_json(const QJsonValue& j, JsonRpcResponse& r);

// Codec for the wire error object (defined here rather than in error.h so
// that header stays free of the JSON codec).
[[nodiscard]] QJsonValue to_json(const ErrorObject& e);
void from_json(const QJsonValue& j, ErrorObject& e);

/// Tagged union over every JSON-RPC frame we accept off the wire. Use
/// `std::visit` to dispatch on the concrete kind.
using JsonRpcMessage =
    std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse>;

/// Parse a single JSON-RPC frame. Returns the variant; throws
/// `mcp::Error(parse_error)` on parse error or envelope-shape errors that
/// prevent classification.
[[nodiscard]] JsonRpcMessage parse_message(const QJsonValue& j);

/// Serialize a JSON-RPC frame.
[[nodiscard]] QJsonValue serialize_message(const JsonRpcMessage& m);

// --------------------------------------------------------------------------
// Implementation metadata
// --------------------------------------------------------------------------

/// 2025-11-25: optional icon descriptor used by Implementation,
/// Tool, Resource, Prompt, and ResourceTemplate. Mirrors the spec's
/// `Icon` shape — a URL/data pointer plus optional metadata.
struct Icon {
    /// `src` is either an absolute URL or a `data:` URL (RFC 2397).
    QString src;
    QString mime_type = {};
    /// Whitespace-delimited list of WxH dimensions ("16x16 32x32"),
    /// matching HTML `<link rel=icon sizes>`.
    QString sizes = {};
    /// "light" / "dark" / "monochrome" / app-defined.
    QString theme = {};
};
[[nodiscard]] QJsonValue to_json(const Icon& c);
void from_json(const QJsonValue& j, Icon& c);

struct Implementation {
    QString name;
    QString title = {};
    QString version;
    QString description = {};
    QString website_url = {};
    /// 2025-11-25: optional list of icons describing this party.
    std::optional<QList<Icon>> icons = {};
    /// Top-level `_meta` envelope for protocol-level metadata.
    /// Round-trips opaquely; spec reserves keys with the
    /// `io.modelcontextprotocol/` prefix.
    std::optional<QJsonValue> meta = {};
};

[[nodiscard]] QJsonValue to_json(const Implementation& i);
void from_json(const QJsonValue& j, Implementation& i);

// --------------------------------------------------------------------------
// Tasks: small capability/augmentation types (the primitive's own
// methods + Task struct live further down). Hoisted so capabilities
// and CallToolRequestParams below can hold std::optional<...> of them.
// --------------------------------------------------------------------------

struct TaskAugmentation {
    /// Milliseconds the task is retained after creation. Empty / null
    /// means unlimited (or implementation-defined retention).
    std::optional<std::int64_t> ttl = {};
};
[[nodiscard]] QJsonValue to_json(const TaskAugmentation& t);
void from_json(const QJsonValue& j, TaskAugmentation& t);

struct TasksRequestsCapability {
    /// Per-namespace presence-only objects — e.g. `tools.call`,
    /// `sampling.createMessage`, `elicitation.create`. Held as raw
    /// json so a future spec extension (a new method) round-trips
    /// without an SDK release.
    std::optional<QJsonValue> tools = {};
    std::optional<QJsonValue> sampling = {};
    std::optional<QJsonValue> elicitation = {};
};
[[nodiscard]] QJsonValue to_json(const TasksRequestsCapability& c);
void from_json(const QJsonValue& j, TasksRequestsCapability& c);

struct TasksCapability {
    std::optional<QJsonValue> list = {};
    std::optional<QJsonValue> cancel = {};
    std::optional<TasksRequestsCapability> requests = {};
};
[[nodiscard]] QJsonValue to_json(const TasksCapability& c);
void from_json(const QJsonValue& j, TasksCapability& c);

// --------------------------------------------------------------------------
// Capabilities
// --------------------------------------------------------------------------

struct RootsCapability {
    std::optional<bool> list_changed = {};
};
[[nodiscard]] QJsonValue to_json(const RootsCapability& c);
void from_json(const QJsonValue& j, RootsCapability& c);

struct SamplingCapability {
    // Per spec: `context?: object`, `tools?: object`. Presence-only flags;
    // we capture the raw object so future fields round-trip.
    std::optional<QJsonValue> context = {};
    std::optional<QJsonValue> tools = {};
};
[[nodiscard]] QJsonValue to_json(const SamplingCapability& c);
void from_json(const QJsonValue& j, SamplingCapability& c);

struct ElicitationCapability {
    std::optional<QJsonValue> form = {};  // form-based elicitation
    std::optional<QJsonValue> url = {};   // URL-based elicitation
};
[[nodiscard]] QJsonValue to_json(const ElicitationCapability& c);
void from_json(const QJsonValue& j, ElicitationCapability& c);

struct ClientCapabilities {
    std::optional<QJsonValue> experimental = {};
    std::optional<RootsCapability> roots = {};
    std::optional<SamplingCapability> sampling = {};
    std::optional<ElicitationCapability> elicitation = {};
    /// 2025-11-25: client advertises which task-augmentable inbound
    /// methods it can handle (e.g. sampling/createMessage as a task).
    std::optional<TasksCapability> tasks = {};
};
[[nodiscard]] QJsonValue to_json(const ClientCapabilities& c);
void from_json(const QJsonValue& j, ClientCapabilities& c);

struct PromptsCapability {
    std::optional<bool> list_changed = {};
};
[[nodiscard]] QJsonValue to_json(const PromptsCapability& c);
void from_json(const QJsonValue& j, PromptsCapability& c);

struct ResourcesCapability {
    std::optional<bool> subscribe = {};
    std::optional<bool> list_changed = {};
};
[[nodiscard]] QJsonValue to_json(const ResourcesCapability& c);
void from_json(const QJsonValue& j, ResourcesCapability& c);

struct ToolsCapability {
    std::optional<bool> list_changed = {};
};
[[nodiscard]] QJsonValue to_json(const ToolsCapability& c);
void from_json(const QJsonValue& j, ToolsCapability& c);

struct ServerCapabilities {
    std::optional<QJsonValue> experimental = {};
    std::optional<QJsonValue> logging = {};      // presence-only
    std::optional<QJsonValue> completions = {};  // presence-only
    std::optional<PromptsCapability> prompts = {};
    std::optional<ResourcesCapability> resources = {};
    std::optional<ToolsCapability> tools = {};
    /// 2025-11-25: which task-augmentable inbound methods the server
    /// can handle (e.g. tools/call as a task), plus list/cancel.
    std::optional<TasksCapability> tasks = {};
};
[[nodiscard]] QJsonValue to_json(const ServerCapabilities& c);
void from_json(const QJsonValue& j, ServerCapabilities& c);

// --------------------------------------------------------------------------
// initialize / initialized
// --------------------------------------------------------------------------

struct InitializeRequestParams {
    QString protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;
};
[[nodiscard]] QJsonValue to_json(const InitializeRequestParams& p);
void from_json(const QJsonValue& j, InitializeRequestParams& p);

struct InitializeResult {
    QString protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    QString instructions = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const InitializeResult& r);
void from_json(const QJsonValue& j, InitializeResult& r);

inline const QString method_initialize = "initialize";
inline const QString method_notifications_initialized =
    "notifications/initialized";

// --------------------------------------------------------------------------
// Content blocks
// --------------------------------------------------------------------------

enum class Role : int { user, assistant };
[[nodiscard]] QString to_string(Role r) noexcept;
[[nodiscard]] QJsonValue to_json(Role r);
void from_json(const QJsonValue& j, Role& r);

struct Annotations {
    std::optional<QList<Role>> audience = {};
    std::optional<double> priority = {};
    QString last_modified = {};
};
[[nodiscard]] QJsonValue to_json(const Annotations& a);
void from_json(const QJsonValue& j, Annotations& a);

struct TextContent {
    QString text;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const TextContent& c);
void from_json(const QJsonValue& j, TextContent& c);

struct ImageContent {
    QString data;  // base64
    QString mime_type;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const ImageContent& c);
void from_json(const QJsonValue& j, ImageContent& c);

struct AudioContent {
    QString data;  // base64
    QString mime_type;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const AudioContent& c);
void from_json(const QJsonValue& j, AudioContent& c);

// --------------------------------------------------------------------------
// Resources (must come before ContentBlock; ResourceLink and
// EmbeddedResource are content variants that reference resource shapes)
// --------------------------------------------------------------------------

struct TextResourceContents {
    QString uri;
    QString mime_type = {};
    QString text;
};
[[nodiscard]] QJsonValue to_json(const TextResourceContents& c);
void from_json(const QJsonValue& j, TextResourceContents& c);

struct BlobResourceContents {
    QString uri;
    QString mime_type = {};
    QString blob;  // base64
};
[[nodiscard]] QJsonValue to_json(const BlobResourceContents& c);
void from_json(const QJsonValue& j, BlobResourceContents& c);

/// Resource contents carry either text or a base64 blob; on the wire
/// the discriminator is which member ("text" or "blob") is present.
using ResourceContents =
    std::variant<TextResourceContents, BlobResourceContents>;

[[nodiscard]] QJsonValue to_json(const ResourceContents& c);
void from_json(const QJsonValue& j, ResourceContents& c);

struct Resource {
    QString uri;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {};  // bytes
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const Resource& r);
void from_json(const QJsonValue& j, Resource& r);

struct ResourceTemplate {
    QString uri_template;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ResourceTemplate& r);
void from_json(const QJsonValue& j, ResourceTemplate& r);

struct ResourceLink {
    QString uri;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ResourceLink& r);
void from_json(const QJsonValue& j, ResourceLink& r);

struct EmbeddedResource {
    ResourceContents resource;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const EmbeddedResource& r);
void from_json(const QJsonValue& j, EmbeddedResource& r);

/// 2025-11-25: tool-use content block. Used in sampling messages to
/// represent the assistant emitting a tool call (i.e. an
/// agentic-loop step). Wire type discriminator: `"tool_use"`.
struct ToolUseContent {
    /// Identifier the assistant assigns to this call so the
    /// corresponding `tool_result` block can reference it.
    QString id;
    /// Name of the tool being invoked. Should match a registered
    /// `Tool.name` server-side.
    QString name;
    /// Arguments to the tool. JSON object matching the tool's
    /// `inputSchema`.
    QJsonValue input;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const ToolUseContent& c);
void from_json(const QJsonValue& j, ToolUseContent& c);

/// Plain (non-tool) content blocks — the subset legal as the
/// `content` of a ToolResultContent. The spec forbids nested
/// tool_use/tool_result, and modelling the constraint in the type
/// system catches malformed payloads at compile time.
using PlainContentBlock = std::variant<TextContent, ImageContent, AudioContent,
                                       ResourceLink, EmbeddedResource>;
[[nodiscard]] QJsonValue to_json(const PlainContentBlock& c);
void from_json(const QJsonValue& j, PlainContentBlock& c);

/// 2025-11-25: tool-result content block. Carries the result of a
/// previous `tool_use` block, referenced by `toolUseId`. Wire type
/// discriminator: `"tool_result"`.
struct ToolResultContent {
    /// The `id` of the originating ToolUseContent.
    QString tool_use_id;
    /// The tool's actual output. Restricted to plain content blocks
    /// per spec (no nested tool_* allowed).
    QList<PlainContentBlock> content;
    /// True when the tool errored. The content array typically
    /// carries a TextContent describing the error in that case.
    std::optional<bool> is_error = {};
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const ToolResultContent& c);
void from_json(const QJsonValue& j, ToolResultContent& c);

/// Content blocks used by tools/calls, prompts, and sampling. The
/// type discriminator on the wire is `"text" | "image" | "audio"
/// | "resource_link" | "resource" | "tool_use" | "tool_result"`.
/// The last two are 2025-11-25 additions for agentic sampling
/// loops.
using ContentBlock =
    std::variant<TextContent, ImageContent, AudioContent, ResourceLink,
                 EmbeddedResource, ToolUseContent, ToolResultContent>;

[[nodiscard]] QJsonValue to_json(const ContentBlock& c);
void from_json(const QJsonValue& j, ContentBlock& c);

// --------------------------------------------------------------------------
// Tools
// --------------------------------------------------------------------------

struct ToolAnnotations {
    QString title = {};
    std::optional<bool> read_only_hint = {};
    std::optional<bool> destructive_hint = {};
    std::optional<bool> idempotent_hint = {};
    std::optional<bool> open_world_hint = {};
};
[[nodiscard]] QJsonValue to_json(const ToolAnnotations& a);
void from_json(const QJsonValue& j, ToolAnnotations& a);

/// 2025-11-25: per-tool execution policy. The receiver advertises
/// whether a tool supports being task-augmented at all, optionally,
/// or requires it.
enum class TaskSupport { forbidden, optional, required };
[[nodiscard]] QJsonValue to_json(TaskSupport t);
void from_json(const QJsonValue& j, TaskSupport& t);

struct ToolExecution {
    /// "forbidden" / "optional" / "required". Default is
    /// "optional" when `execution` is absent.
    TaskSupport task_support = TaskSupport::optional;
};
[[nodiscard]] QJsonValue to_json(const ToolExecution& e);
void from_json(const QJsonValue& j, ToolExecution& e);

struct Tool {
    QString name;
    QString title = {};
    QString description = {};
    QJsonValue input_schema;                       // JSON Schema; type:"object"
    std::optional<QJsonValue> output_schema = {};  // optional JSON Schema
    std::optional<ToolAnnotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<ToolExecution> execution = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const Tool& t);
void from_json(const QJsonValue& j, Tool& t);

struct ListToolsRequestParams {
    QString cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListToolsRequestParams& p);
void from_json(const QJsonValue& j, ListToolsRequestParams& p);

struct ListToolsResult {
    QList<Tool> tools;
    QString next_cursor = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListToolsResult& r);
void from_json(const QJsonValue& j, ListToolsResult& r);

struct CallToolRequestParams {
    QString name;
    std::optional<QJsonValue> arguments = {};  // JSON object
    /// When set, the request is task-augmented: the receiver should
    /// return a CreateTaskResult envelope and run the call async. The
    /// final CallToolResult is fetched via `tasks/result`.
    std::optional<TaskAugmentation> task = {};
};
[[nodiscard]] QJsonValue to_json(const CallToolRequestParams& p);
void from_json(const QJsonValue& j, CallToolRequestParams& p);

struct CallToolResult {
    QList<ContentBlock> content;
    std::optional<QJsonValue> structured_content = {};  // matches outputSchema
    std::optional<bool> is_error = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const CallToolResult& r);
void from_json(const QJsonValue& j, CallToolResult& r);

inline const QString method_tools_list = "tools/list";
inline const QString method_tools_call = "tools/call";

// --------------------------------------------------------------------------
// Resource requests / responses
// --------------------------------------------------------------------------

struct ListResourcesRequestParams {
    QString cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourcesRequestParams& p);
void from_json(const QJsonValue& j, ListResourcesRequestParams& p);

struct ListResourcesResult {
    QList<Resource> resources;
    QString next_cursor = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourcesResult& r);
void from_json(const QJsonValue& j, ListResourcesResult& r);

struct ListResourceTemplatesRequestParams {
    QString cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourceTemplatesRequestParams& p);
void from_json(const QJsonValue& j, ListResourceTemplatesRequestParams& p);

struct ListResourceTemplatesResult {
    QList<ResourceTemplate> resource_templates;
    QString next_cursor = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourceTemplatesResult& r);
void from_json(const QJsonValue& j, ListResourceTemplatesResult& r);

struct ReadResourceRequestParams {
    QString uri;
};
[[nodiscard]] QJsonValue to_json(const ReadResourceRequestParams& p);
void from_json(const QJsonValue& j, ReadResourceRequestParams& p);

struct ReadResourceResult {
    QList<ResourceContents> contents;
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ReadResourceResult& r);
void from_json(const QJsonValue& j, ReadResourceResult& r);

struct SubscribeRequestParams {
    QString uri;
};
[[nodiscard]] QJsonValue to_json(const SubscribeRequestParams& p);
void from_json(const QJsonValue& j, SubscribeRequestParams& p);

struct UnsubscribeRequestParams {
    QString uri;
};
[[nodiscard]] QJsonValue to_json(const UnsubscribeRequestParams& p);
void from_json(const QJsonValue& j, UnsubscribeRequestParams& p);

struct ResourceUpdatedNotificationParams {
    QString uri;
};
[[nodiscard]] QJsonValue to_json(const ResourceUpdatedNotificationParams& p);
void from_json(const QJsonValue& j, ResourceUpdatedNotificationParams& p);

inline const QString method_resources_list = "resources/list";
inline const QString method_resources_templates_list =
    "resources/templates/list";
inline const QString method_resources_read = "resources/read";
inline const QString method_resources_subscribe = "resources/subscribe";
inline const QString method_resources_unsubscribe = "resources/unsubscribe";
inline const QString method_notifications_resources_list_changed =
    "notifications/resources/list_changed";
inline const QString method_notifications_resources_updated =
    "notifications/resources/updated";

// --------------------------------------------------------------------------
// Prompts
// --------------------------------------------------------------------------

struct PromptArgument {
    QString name;
    QString title = {};
    QString description = {};
    std::optional<bool> required = {};
};
[[nodiscard]] QJsonValue to_json(const PromptArgument& a);
void from_json(const QJsonValue& j, PromptArgument& a);

struct Prompt {
    QString name;
    QString title = {};
    QString description = {};
    std::optional<QList<PromptArgument>> arguments = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const Prompt& p);
void from_json(const QJsonValue& j, Prompt& p);

struct PromptMessage {
    Role role;
    ContentBlock content;
};
[[nodiscard]] QJsonValue to_json(const PromptMessage& m);
void from_json(const QJsonValue& j, PromptMessage& m);

struct ListPromptsRequestParams {
    QString cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListPromptsRequestParams& p);
void from_json(const QJsonValue& j, ListPromptsRequestParams& p);

struct ListPromptsResult {
    QList<Prompt> prompts;
    QString next_cursor = {};
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListPromptsResult& r);
void from_json(const QJsonValue& j, ListPromptsResult& r);

struct GetPromptRequestParams {
    QString name;
    std::optional<QHash<QString, QString>> arguments = {};
};
[[nodiscard]] QJsonValue to_json(const GetPromptRequestParams& p);
void from_json(const QJsonValue& j, GetPromptRequestParams& p);

struct GetPromptResult {
    QString description = {};
    QList<PromptMessage> messages;
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const GetPromptResult& r);
void from_json(const QJsonValue& j, GetPromptResult& r);

inline const QString method_prompts_list = "prompts/list";
inline const QString method_prompts_get = "prompts/get";
inline const QString method_notifications_prompts_list_changed =
    "notifications/prompts/list_changed";

// --------------------------------------------------------------------------
// Cancellation
// --------------------------------------------------------------------------

struct CancelledNotificationParams {
    std::optional<RequestId> request_id = {};
    QString reason = {};
};
[[nodiscard]] QJsonValue to_json(const CancelledNotificationParams& p);
void from_json(const QJsonValue& j, CancelledNotificationParams& p);

inline const QString method_notifications_cancelled = "notifications/cancelled";

// --------------------------------------------------------------------------
// Progress
// --------------------------------------------------------------------------

/// Progress tokens are opaque values carried in `_meta.progressToken`
/// on the originating request. The spec permits string or number; we
/// normalise to a thin variant.
inline std::size_t qHash(const RequestId& id, std::size_t seed = 0) noexcept {
    return qHash(id.canonical(), seed);
}

class ProgressToken {
public:
    ProgressToken(void) : m_value(std::int64_t{0}) {}
    ProgressToken(QString s) : m_value(std::move(s)) {}
    ProgressToken(const char* s) : m_value(QString{s}) {}
    ProgressToken(std::int64_t i) : m_value(i) {}
    ProgressToken(int i) : m_value(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] bool is_string(void) const noexcept {
        return m_value.index() == 0;
    }
    [[nodiscard]] bool is_integer(void) const noexcept {
        return m_value.index() == 1;
    }

    [[nodiscard]] const QString& as_string(void) const {
        return std::get<0>(m_value);
    }
    [[nodiscard]] std::int64_t as_integer(void) const {
        return std::get<1>(m_value);
    }
    [[nodiscard]] QString canonical(void) const;

    friend bool operator==(const ProgressToken&,
                           const ProgressToken&) = default;

private:
    std::variant<QString, std::int64_t> m_value;
};
[[nodiscard]] QJsonValue to_json(const ProgressToken& t);
void from_json(const QJsonValue& j, ProgressToken& t);

struct ProgressNotificationParams {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total = {};
    QString message = {};
};
[[nodiscard]] QJsonValue to_json(const ProgressNotificationParams& p);
void from_json(const QJsonValue& j, ProgressNotificationParams& p);

inline const QString method_notifications_progress = "notifications/progress";

// --------------------------------------------------------------------------
// Logging (server → client log messages)
// --------------------------------------------------------------------------

/// Per-spec MCP log severity levels — this is the protocol-level
/// "logging" capability, distinct from this library's internal
/// LogLevel which writes to the host's stderr.
enum class LoggingLevel {
    debug,
    info,
    notice,
    warning,
    error,
    critical,
    alert,
    emergency,
};
[[nodiscard]] QString to_string(LoggingLevel l) noexcept;
[[nodiscard]] QJsonValue to_json(LoggingLevel l);
void from_json(const QJsonValue& j, LoggingLevel& l);

struct SetLevelRequestParams {
    LoggingLevel level;
};
[[nodiscard]] QJsonValue to_json(const SetLevelRequestParams& p);
void from_json(const QJsonValue& j, SetLevelRequestParams& p);

struct LoggingMessageNotificationParams {
    LoggingLevel level;
    QString logger = {};  // optional name of the logger source
    QJsonValue data;            // free-form payload (string/object/...)
};
[[nodiscard]] QJsonValue to_json(const LoggingMessageNotificationParams& p);
void from_json(const QJsonValue& j, LoggingMessageNotificationParams& p);

inline const QString method_logging_set_level = "logging/setLevel";
inline const QString method_notifications_message = "notifications/message";

// --------------------------------------------------------------------------
// Ping
// --------------------------------------------------------------------------

inline const QString method_ping = "ping";

// --------------------------------------------------------------------------
// Sampling (sampling/createMessage) — server-initiated LLM calls
// --------------------------------------------------------------------------

struct ModelHint {
    QString name = {};
};
[[nodiscard]] QJsonValue to_json(const ModelHint& h);
void from_json(const QJsonValue& j, ModelHint& h);

struct ModelPreferences {
    std::optional<QList<ModelHint>> hints = {};
    std::optional<double> cost_priority = {};
    std::optional<double> speed_priority = {};
    std::optional<double> intelligence_priority = {};
};
[[nodiscard]] QJsonValue to_json(const ModelPreferences& p);
void from_json(const QJsonValue& j, ModelPreferences& p);

enum class IncludeContext { none, this_server, all_servers };
[[nodiscard]] QString to_string(IncludeContext c) noexcept;
[[nodiscard]] QJsonValue to_json(IncludeContext c);
void from_json(const QJsonValue& j, IncludeContext& c);

/// Sampling tool-choice mode (2025-11-25). The spec lets the
/// requester force / forbid / pin tool use:
///   - "auto"   — model decides whether to call tools (default)
///   - "any"    — model MUST call at least one tool
///   - "none"   — model MUST NOT call tools
///   - "tool"   — model MUST call the specific tool named
enum class ToolChoiceMode { m_auto, any, none, tool };
[[nodiscard]] QString to_string(ToolChoiceMode m) noexcept;
[[nodiscard]] QJsonValue to_json(ToolChoiceMode m);
void from_json(const QJsonValue& j, ToolChoiceMode& m);

/// Tool choice. The wire shape is either a bare string ("auto" |
/// "any" | "none") or an object `{ type: "tool", name: "..." }`
/// for the named-tool case. We model it as a struct with both
/// fields optional; serialise emits the object form when
/// `mode == tool` AND `name` is present.
struct ToolChoice {
    std::optional<ToolChoiceMode> mode = {};
    /// Required when mode == ToolChoiceMode::tool.
    QString name = {};
};
[[nodiscard]] QJsonValue to_json(const ToolChoice& c);
void from_json(const QJsonValue& j, ToolChoice& c);

/// A message in a sampling exchange. Phase 3 supports text/image/audio
/// content; tool_use and tool_result variants land in Phase 4 alongside
/// the rest of the agentic-loop primitives.
///
/// The spec's `content` field accepts either a single block or an array
/// of blocks; we always model it as a vector internally so multi-block
/// messages don't get silently truncated. On the wire we emit an array
/// when there's more than one block and a single-block object otherwise
/// (matching what nearly every existing implementation produces).
struct SamplingMessage {
    Role role;
    QList<ContentBlock> content;
};
[[nodiscard]] QJsonValue to_json(const SamplingMessage& m);
void from_json(const QJsonValue& j, SamplingMessage& m);

struct CreateMessageRequestParams {
    QList<SamplingMessage> messages;
    std::optional<ModelPreferences> model_preferences = {};
    QString system_prompt = {};
    std::optional<IncludeContext> include_context = {};
    std::optional<double> temperature = {};
    std::int64_t max_tokens{};  // required by spec
    std::optional<QList<QString>> stop_sequences = {};
    std::optional<QJsonValue> metadata = {};
    /// 2025-11-25: tools the assistant may call during this
    /// sampling. Server side is expected to translate to the
    /// underlying LLM's tool-calling shape; `Tool::input_schema`
    /// is the JSON Schema the model is told to fit.
    std::optional<QList<Tool>> tools = {};
    /// 2025-11-25: how to constrain tool calling.
    std::optional<ToolChoice> tool_choice = {};
};
[[nodiscard]] QJsonValue to_json(const CreateMessageRequestParams& p);
void from_json(const QJsonValue& j, CreateMessageRequestParams& p);

struct CreateMessageResult {
    Role role;
    QList<ContentBlock> content = {};
    QString model;
    QString stop_reason = {};
    /// Top-level _meta envelope for protocol-level metadata.
    std::optional<QJsonValue> meta = {};
};
[[nodiscard]] QJsonValue to_json(const CreateMessageResult& r);
void from_json(const QJsonValue& j, CreateMessageResult& r);

inline const QString method_sampling_create_message = "sampling/createMessage";

// --------------------------------------------------------------------------
// Roots (client → server: filesystem/URI scopes)
// --------------------------------------------------------------------------

struct Root {
    QString uri;
    QString name = {};
};
[[nodiscard]] QJsonValue to_json(const Root& r);
void from_json(const QJsonValue& j, Root& r);

struct ListRootsResult {
    QList<Root> roots;
};
[[nodiscard]] QJsonValue to_json(const ListRootsResult& r);
void from_json(const QJsonValue& j, ListRootsResult& r);

inline const QString method_roots_list = "roots/list";
inline const QString method_notifications_roots_list_changed =
    "notifications/roots/list_changed";

// --------------------------------------------------------------------------
// Completion (autocomplete)
// --------------------------------------------------------------------------

struct ResourceTemplateReference {
    QString uri_template;
};
[[nodiscard]] QJsonValue to_json(const ResourceTemplateReference& r);
void from_json(const QJsonValue& j, ResourceTemplateReference& r);

struct PromptReference {
    QString name;
    QString title = {};
};
[[nodiscard]] QJsonValue to_json(const PromptReference& r);
void from_json(const QJsonValue& j, PromptReference& r);

using CompletionReference =
    std::variant<ResourceTemplateReference, PromptReference>;
[[nodiscard]] QJsonValue to_json(const CompletionReference& r);
void from_json(const QJsonValue& j, CompletionReference& r);

struct CompleteArgument {
    QString name;
    QString value;
};
[[nodiscard]] QJsonValue to_json(const CompleteArgument& a);
void from_json(const QJsonValue& j, CompleteArgument& a);

struct CompleteRequestParams {
    CompletionReference reference;
    CompleteArgument argument;
    std::optional<QHash<QString, QString>> context_arguments;
};
[[nodiscard]] QJsonValue to_json(const CompleteRequestParams& p);
void from_json(const QJsonValue& j, CompleteRequestParams& p);

struct CompletionValues {
    QList<QString> values;
    std::optional<std::int64_t> total = {};
    std::optional<bool> has_more = {};
};
[[nodiscard]] QJsonValue to_json(const CompletionValues& v);
void from_json(const QJsonValue& j, CompletionValues& v);

struct CompleteResult {
    CompletionValues completion;
};
[[nodiscard]] QJsonValue to_json(const CompleteResult& r);
void from_json(const QJsonValue& j, CompleteResult& r);

inline const QString method_completion_complete = "completion/complete";

// --------------------------------------------------------------------------
// Elicitation (server → client: ask the user for input)
// --------------------------------------------------------------------------
//
// Two modes: form (an in-band JSON-Schema-described form rendered by the
// client) and url (an out-of-band browser navigation, with an optional
// completion notification).
//
// The wire shape is a tagged-by-`mode` object: omitted/`form` means form
// mode, `url` means URL mode. We model it as a variant so callers don't
// have to inspect a discriminator field by hand.

struct ElicitFormRequestParams {
    /// Plain-text rationale for the request, shown to the user.
    QString message;
    /// JSON Schema describing the expected response. Per spec, restricted
    /// to a flat object of primitive properties; we don't enforce that
    /// here so applications can negotiate richer shapes when both sides
    /// agree, but the basic SDK sticks to the spec.
    QJsonValue requested_schema;
};
[[nodiscard]] QJsonValue to_json(const ElicitFormRequestParams& p);
void from_json(const QJsonValue& j, ElicitFormRequestParams& p);

struct ElicitUrlRequestParams {
    QString message;
    /// HTTPS URL the user is asked to visit out-of-band.
    QString url;
    /// Identifier the server uses to correlate later
    /// `notifications/elicitation/complete` notifications.
    QString elicitation_id;
};
[[nodiscard]] QJsonValue to_json(const ElicitUrlRequestParams& p);
void from_json(const QJsonValue& j, ElicitUrlRequestParams& p);

using ElicitRequestParams =
    std::variant<ElicitFormRequestParams, ElicitUrlRequestParams>;
[[nodiscard]] QJsonValue to_json(const ElicitRequestParams& p);
void from_json(const QJsonValue& j, ElicitRequestParams& p);

enum class ElicitAction { accept, decline, cancel };
[[nodiscard]] QJsonValue to_json(ElicitAction a);
void from_json(const QJsonValue& j, ElicitAction& a);

struct ElicitResult {
    ElicitAction action;
    /// Form-mode only: the user's submission, conforming to the
    /// server-supplied schema. Absent for decline/cancel and for URL
    /// mode.
    std::optional<QJsonValue> content = {};
};
[[nodiscard]] QJsonValue to_json(const ElicitResult& r);
void from_json(const QJsonValue& j, ElicitResult& r);

/// URL-mode out-of-band completion notification. Emitted by the server
/// or relayed by the client once the out-of-band flow finishes — but
/// the spec leaves precise emitter conventions open. The MCP SDK
/// accepts the notification on either side.
struct ElicitationCompleteNotificationParams {
    QString elicitation_id;
};
[[nodiscard]] QJsonValue
to_json(const ElicitationCompleteNotificationParams& p);
void from_json(const QJsonValue& j, ElicitationCompleteNotificationParams& p);

inline const QString method_elicitation_create = "elicitation/create";
inline const QString method_notifications_elicitation_complete =
    "notifications/elicitation/complete";

/// 2025-11-25: structured `data` for the JSON-RPC error code
/// `error_code::url_elicitation_required` (-32042). Sent by the
/// server when an in-flight request can't proceed without the
/// user finishing one or more URL-mode elicitation flows. The
/// client MAY drive the URL flows out-of-band, then retry the
/// original request.
struct UrlElicitationRequiredErrorData {
    /// One or more URL-mode elicitations the client must run.
    /// Each element is the params shape of an `elicitation/create`
    /// request — message + url + elicitationId.
    QList<ElicitUrlRequestParams> elicitations;
};
[[nodiscard]] QJsonValue to_json(const UrlElicitationRequiredErrorData& d);
void from_json(const QJsonValue& j, UrlElicitationRequiredErrorData& d);

// --------------------------------------------------------------------------
// Tasks (long-running operations)
// --------------------------------------------------------------------------
//
// A task primitive lets a request that would otherwise be synchronous
// (`tools/call`, `sampling/createMessage`, `elicitation/create`) be
// promoted to an async, polled operation. The requester opts in by
// attaching a `task` field to the request params; the receiver returns
// a `CreateTaskResult { task }` envelope right away, and the actual
// result is fetched later via `tasks/result`. Status transitions can
// be polled via `tasks/get` or pushed via `notifications/tasks/status`.

enum class TaskStatus {
    working,
    input_required,
    completed,
    failed,
    cancelled,
};
[[nodiscard]] QJsonValue to_json(TaskStatus s);
void from_json(const QJsonValue& j, TaskStatus& s);

struct Task {
    /// Receiver-generated, unique. The string need not be a UUID; we
    /// emit a 128-bit hex token by default.
    QString taskId;
    TaskStatus status;
    /// Optional human-readable progress description.
    QString status_message = {};
    /// ISO 8601 UTC timestamps.
    QString created_at;
    QString last_updated_at;
    /// Milliseconds the task is retained after creation. Absent / null
    /// means unlimited (or implementation-defined retention).
    std::optional<std::int64_t> ttl = {};
    /// Suggested polling interval in milliseconds; advisory.
    std::optional<std::int64_t> poll_interval = {};
};
[[nodiscard]] QJsonValue to_json(const Task& t);
void from_json(const QJsonValue& j, Task& t);

/// Returned in place of the underlying request's normal result when
/// the request was task-augmented.
struct CreateTaskResult {
    Task task;
};
[[nodiscard]] QJsonValue to_json(const CreateTaskResult& r);
void from_json(const QJsonValue& j, CreateTaskResult& r);

struct GetTaskRequestParams {
    QString task_id;
};
[[nodiscard]] QJsonValue to_json(const GetTaskRequestParams& p);
void from_json(const QJsonValue& j, GetTaskRequestParams& p);

struct GetTaskResultRequestParams {
    QString task_id;
};
[[nodiscard]] QJsonValue to_json(const GetTaskResultRequestParams& p);
void from_json(const QJsonValue& j, GetTaskResultRequestParams& p);

struct ListTasksRequestParams {
    QString cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListTasksRequestParams& p);
void from_json(const QJsonValue& j, ListTasksRequestParams& p);

struct ListTasksResult {
    QList<Task> tasks;
    QString next_cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListTasksResult& r);
void from_json(const QJsonValue& j, ListTasksResult& r);

struct CancelTaskRequestParams {
    QString task_id;
};
[[nodiscard]] QJsonValue to_json(const CancelTaskRequestParams& p);
void from_json(const QJsonValue& j, CancelTaskRequestParams& p);

struct TaskStatusNotificationParams {
    /// A point-in-time projection of the underlying Task. Same shape
    /// as Task, no extra fields.
    Task task;
};
[[nodiscard]] QJsonValue to_json(const TaskStatusNotificationParams& p);
void from_json(const QJsonValue& j, TaskStatusNotificationParams& p);

inline const QString method_tasks_get = "tasks/get";
inline const QString method_tasks_result = "tasks/result";
inline const QString method_tasks_list = "tasks/list";
inline const QString method_tasks_cancel = "tasks/cancel";
inline const QString method_notifications_tasks_status =
    "notifications/tasks/status";

/// Meta key the spec mandates on `tasks/result` payloads to bind the
/// result back to its task. Server-side tooling sets it; clients can
/// optionally check it.
inline const QString tasks_related_task_meta_key =
    "io.modelcontextprotocol/related-task";

}  // namespace mcp

// std::hash specialization so RequestId can key unordered containers.
namespace std {
template <> struct hash<::mcp::RequestId> {
    [[nodiscard]] std::size_t
    operator()(const ::mcp::RequestId& id) const noexcept {
        return std::hash<QString>{}(id.canonical());
    }
};
}  // namespace std
