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

inline constexpr std::string_view kLatestProtocolVersion = "2025-11-25";
inline constexpr std::string_view kJsonrpcVersion = "2.0";

// --------------------------------------------------------------------------
// JSON-RPC envelope
// --------------------------------------------------------------------------

/// A JSON-RPC request id. The spec permits string or number; we preserve the
/// original kind on round-trip so a client that emits integer ids gets back
/// integer ids, and likewise for strings.
class RequestId {
public:
    using Value = std::variant<std::string, std::int64_t>;

    RequestId(void) : m_value(std::int64_t{0}) {}
    RequestId(std::string s) : m_value(std::move(s)) {}
    RequestId(const char* s) : m_value(std::string{s}) {}
    RequestId(std::int64_t i) : m_value(i) {}
    RequestId(int i) : m_value(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] const Value& value(void) const noexcept { return m_value; }

    [[nodiscard]] bool is_string(void) const noexcept {
        return m_value.index() == 0;
    }
    [[nodiscard]] bool is_integer(void) const noexcept {
        return m_value.index() == 1;
    }

    [[nodiscard]] const std::string& as_string(void) const {
        return std::get<0>(m_value);
    }
    [[nodiscard]] std::int64_t as_integer(void) const {
        return std::get<1>(m_value);
    }

    /// Canonical string form used for hash/map keys. String ids are returned
    /// as-is with a leading 's:' marker; integer ids get an 'i:' marker. This
    /// guarantees that the integer 5 and the string "5" do not collide.
    [[nodiscard]] std::string canonical(void) const;

    friend bool operator==(const RequestId&, const RequestId&) = default;

private:
    Value m_value;
};

[[nodiscard]] QJsonValue to_json(const RequestId& id);
void from_json(const QJsonValue& j, RequestId& id);

/// A JSON-RPC 2.0 request. Always has an id.
struct JsonRpcRequest {
    RequestId id;
    std::string method;
    std::optional<Json> params = {};
};

[[nodiscard]] QJsonValue to_json(const JsonRpcRequest& r);
void from_json(const QJsonValue& j, JsonRpcRequest& r);

/// A JSON-RPC 2.0 notification. Has no id; sender expects no response.
struct JsonRpcNotification {
    std::string method;
    std::optional<Json> params = {};
};

[[nodiscard]] QJsonValue to_json(const JsonRpcNotification& n);
void from_json(const QJsonValue& j, JsonRpcNotification& n);

/// A JSON-RPC 2.0 response. Carries either a `result` or an `error`. Per
/// the spec the id is REQUIRED on success, and `null`/absent only on parse
/// errors that prevented identifying the request — we model that with
/// `std::optional<RequestId>`.
struct JsonRpcResponse {
    std::optional<RequestId> id = {};
    std::variant<Json /*result*/, ErrorObject /*error*/> outcome;

    [[nodiscard]] bool is_error(void) const noexcept {
        return outcome.index() == 1;
    }
    [[nodiscard]] bool is_success(void) const noexcept {
        return outcome.index() == 0;
    }

    [[nodiscard]] const Json& result(void) const {
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
[[nodiscard]] JsonRpcMessage parse_message(const Json& j);

/// Serialize a JSON-RPC frame.
[[nodiscard]] Json serialize_message(const JsonRpcMessage& m);

// --------------------------------------------------------------------------
// Implementation metadata
// --------------------------------------------------------------------------

/// 2025-11-25: optional icon descriptor used by Implementation,
/// Tool, Resource, Prompt, and ResourceTemplate. Mirrors the spec's
/// `Icon` shape — a URL/data pointer plus optional metadata.
struct Icon {
    /// `src` is either an absolute URL or a `data:` URL (RFC 2397).
    std::string src;
    std::optional<std::string> mime_type = {};
    /// Whitespace-delimited list of WxH dimensions ("16x16 32x32"),
    /// matching HTML `<link rel=icon sizes>`.
    std::optional<std::string> sizes = {};
    /// "light" / "dark" / "monochrome" / app-defined.
    std::optional<std::string> theme = {};
};
[[nodiscard]] QJsonValue to_json(const Icon& c);
void from_json(const QJsonValue& j, Icon& c);

struct Implementation {
    std::string name;
    std::optional<std::string> title = {};
    std::string version;
    std::optional<std::string> description = {};
    std::optional<std::string> website_url = {};
    /// 2025-11-25: optional list of icons describing this party.
    std::optional<std::vector<Icon>> icons = {};
    /// Top-level `_meta` envelope for protocol-level metadata.
    /// Round-trips opaquely; spec reserves keys with the
    /// `io.modelcontextprotocol/` prefix.
    std::optional<Json> meta = {};
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
    std::optional<Json> tools = {};
    std::optional<Json> sampling = {};
    std::optional<Json> elicitation = {};
};
[[nodiscard]] QJsonValue to_json(const TasksRequestsCapability& c);
void from_json(const QJsonValue& j, TasksRequestsCapability& c);

struct TasksCapability {
    std::optional<Json> list = {};
    std::optional<Json> cancel = {};
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
    std::optional<Json> context = {};
    std::optional<Json> tools = {};
};
[[nodiscard]] QJsonValue to_json(const SamplingCapability& c);
void from_json(const QJsonValue& j, SamplingCapability& c);

struct ElicitationCapability {
    std::optional<Json> form = {};  // form-based elicitation
    std::optional<Json> url = {};   // URL-based elicitation
};
[[nodiscard]] QJsonValue to_json(const ElicitationCapability& c);
void from_json(const QJsonValue& j, ElicitationCapability& c);

struct ClientCapabilities {
    std::optional<Json> experimental = {};
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
    std::optional<Json> experimental = {};
    std::optional<Json> logging = {};      // presence-only
    std::optional<Json> completions = {};  // presence-only
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
    std::string protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;
};
[[nodiscard]] QJsonValue to_json(const InitializeRequestParams& p);
void from_json(const QJsonValue& j, InitializeRequestParams& p);

struct InitializeResult {
    std::string protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    std::optional<std::string> instructions = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const InitializeResult& r);
void from_json(const QJsonValue& j, InitializeResult& r);

inline constexpr std::string_view method_initialize = "initialize";
inline constexpr std::string_view method_notifications_initialized =
    "notifications/initialized";

// --------------------------------------------------------------------------
// Content blocks
// --------------------------------------------------------------------------

enum class Role : int { user, assistant };
[[nodiscard]] std::string_view to_string(Role r) noexcept;
[[nodiscard]] QJsonValue to_json(Role r);
void from_json(const QJsonValue& j, Role& r);

struct Annotations {
    std::optional<std::vector<Role>> audience = {};
    std::optional<double> priority = {};
    std::optional<std::string> last_modified = {};
};
[[nodiscard]] QJsonValue to_json(const Annotations& a);
void from_json(const QJsonValue& j, Annotations& a);

struct TextContent {
    std::string text;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const TextContent& c);
void from_json(const QJsonValue& j, TextContent& c);

struct ImageContent {
    std::string data;  // base64
    std::string mime_type;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const ImageContent& c);
void from_json(const QJsonValue& j, ImageContent& c);

struct AudioContent {
    std::string data;  // base64
    std::string mime_type;
    std::optional<Annotations> annotations = {};
};
[[nodiscard]] QJsonValue to_json(const AudioContent& c);
void from_json(const QJsonValue& j, AudioContent& c);

// --------------------------------------------------------------------------
// Resources (must come before ContentBlock; ResourceLink and
// EmbeddedResource are content variants that reference resource shapes)
// --------------------------------------------------------------------------

struct TextResourceContents {
    std::string uri;
    std::optional<std::string> mime_type = {};
    std::string text;
};
[[nodiscard]] QJsonValue to_json(const TextResourceContents& c);
void from_json(const QJsonValue& j, TextResourceContents& c);

struct BlobResourceContents {
    std::string uri;
    std::optional<std::string> mime_type = {};
    std::string blob;  // base64
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
    std::string uri;
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    std::optional<std::string> mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {};  // bytes
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const Resource& r);
void from_json(const QJsonValue& j, Resource& r);

struct ResourceTemplate {
    std::string uri_template;
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    std::optional<std::string> mime_type = {};
    std::optional<Annotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ResourceTemplate& r);
void from_json(const QJsonValue& j, ResourceTemplate& r);

struct ResourceLink {
    std::string uri;
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    std::optional<std::string> mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {};
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons = {};
    std::optional<Json> meta = {};
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
    std::string id;
    /// Name of the tool being invoked. Should match a registered
    /// `Tool.name` server-side.
    std::string name;
    /// Arguments to the tool. JSON object matching the tool's
    /// `inputSchema`.
    Json input;
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
    std::string tool_use_id;
    /// The tool's actual output. Restricted to plain content blocks
    /// per spec (no nested tool_* allowed).
    std::vector<PlainContentBlock> content;
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
    std::optional<std::string> title = {};
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
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    Json input_schema;                  // JSON Schema; type:"object"
    std::optional<Json> output_schema = {};  // optional JSON Schema
    std::optional<ToolAnnotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons = {};
    std::optional<ToolExecution> execution = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const Tool& t);
void from_json(const QJsonValue& j, Tool& t);

struct ListToolsRequestParams {
    std::optional<std::string> cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListToolsRequestParams& p);
void from_json(const QJsonValue& j, ListToolsRequestParams& p);

struct ListToolsResult {
    std::vector<Tool> tools;
    std::optional<std::string> next_cursor = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListToolsResult& r);
void from_json(const QJsonValue& j, ListToolsResult& r);

struct CallToolRequestParams {
    std::string name;
    std::optional<Json> arguments = {};  // JSON object
    /// When set, the request is task-augmented: the receiver should
    /// return a CreateTaskResult envelope and run the call async. The
    /// final CallToolResult is fetched via `tasks/result`.
    std::optional<TaskAugmentation> task = {};
};
[[nodiscard]] QJsonValue to_json(const CallToolRequestParams& p);
void from_json(const QJsonValue& j, CallToolRequestParams& p);

struct CallToolResult {
    std::vector<ContentBlock> content;
    std::optional<Json> structured_content = {};  // matches outputSchema
    std::optional<bool> is_error = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const CallToolResult& r);
void from_json(const QJsonValue& j, CallToolResult& r);

inline constexpr std::string_view method_tools_list = "tools/list";
inline constexpr std::string_view method_tools_call = "tools/call";

// --------------------------------------------------------------------------
// Resource requests / responses
// --------------------------------------------------------------------------

struct ListResourcesRequestParams {
    std::optional<std::string> cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourcesRequestParams& p);
void from_json(const QJsonValue& j, ListResourcesRequestParams& p);

struct ListResourcesResult {
    std::vector<Resource> resources;
    std::optional<std::string> next_cursor = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourcesResult& r);
void from_json(const QJsonValue& j, ListResourcesResult& r);

struct ListResourceTemplatesRequestParams {
    std::optional<std::string> cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourceTemplatesRequestParams& p);
void from_json(const QJsonValue& j, ListResourceTemplatesRequestParams& p);

struct ListResourceTemplatesResult {
    std::vector<ResourceTemplate> resource_templates;
    std::optional<std::string> next_cursor = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListResourceTemplatesResult& r);
void from_json(const QJsonValue& j, ListResourceTemplatesResult& r);

struct ReadResourceRequestParams {
    std::string uri;
};
[[nodiscard]] QJsonValue to_json(const ReadResourceRequestParams& p);
void from_json(const QJsonValue& j, ReadResourceRequestParams& p);

struct ReadResourceResult {
    std::vector<ResourceContents> contents;
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ReadResourceResult& r);
void from_json(const QJsonValue& j, ReadResourceResult& r);

struct SubscribeRequestParams {
    std::string uri;
};
[[nodiscard]] QJsonValue to_json(const SubscribeRequestParams& p);
void from_json(const QJsonValue& j, SubscribeRequestParams& p);

struct UnsubscribeRequestParams {
    std::string uri;
};
[[nodiscard]] QJsonValue to_json(const UnsubscribeRequestParams& p);
void from_json(const QJsonValue& j, UnsubscribeRequestParams& p);

struct ResourceUpdatedNotificationParams {
    std::string uri;
};
[[nodiscard]] QJsonValue to_json(const ResourceUpdatedNotificationParams& p);
void from_json(const QJsonValue& j, ResourceUpdatedNotificationParams& p);

inline constexpr std::string_view method_resources_list = "resources/list";
inline constexpr std::string_view method_resources_templates_list =
    "resources/templates/list";
inline constexpr std::string_view method_resources_read = "resources/read";
inline constexpr std::string_view method_resources_subscribe =
    "resources/subscribe";
inline constexpr std::string_view method_resources_unsubscribe =
    "resources/unsubscribe";
inline constexpr std::string_view method_notifications_resources_list_changed =
    "notifications/resources/list_changed";
inline constexpr std::string_view method_notifications_resources_updated =
    "notifications/resources/updated";

// --------------------------------------------------------------------------
// Prompts
// --------------------------------------------------------------------------

struct PromptArgument {
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    std::optional<bool> required = {};
};
[[nodiscard]] QJsonValue to_json(const PromptArgument& a);
void from_json(const QJsonValue& j, PromptArgument& a);

struct Prompt {
    std::string name;
    std::optional<std::string> title = {};
    std::optional<std::string> description = {};
    std::optional<std::vector<PromptArgument>> arguments = {};
    /// 2025-11-25 additions:
    std::optional<std::vector<Icon>> icons = {};
    std::optional<Json> meta = {};
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
    std::optional<std::string> cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListPromptsRequestParams& p);
void from_json(const QJsonValue& j, ListPromptsRequestParams& p);

struct ListPromptsResult {
    std::vector<Prompt> prompts;
    std::optional<std::string> next_cursor = {};
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const ListPromptsResult& r);
void from_json(const QJsonValue& j, ListPromptsResult& r);

struct GetPromptRequestParams {
    std::string name;
    std::optional<std::unordered_map<std::string, std::string>> arguments = {};
};
[[nodiscard]] QJsonValue to_json(const GetPromptRequestParams& p);
void from_json(const QJsonValue& j, GetPromptRequestParams& p);

struct GetPromptResult {
    std::optional<std::string> description = {};
    std::vector<PromptMessage> messages;
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const GetPromptResult& r);
void from_json(const QJsonValue& j, GetPromptResult& r);

inline constexpr std::string_view method_prompts_list = "prompts/list";
inline constexpr std::string_view method_prompts_get = "prompts/get";
inline constexpr std::string_view method_notifications_prompts_list_changed =
    "notifications/prompts/list_changed";

// --------------------------------------------------------------------------
// Cancellation
// --------------------------------------------------------------------------

struct CancelledNotificationParams {
    std::optional<RequestId> request_id = {};
    std::optional<std::string> reason = {};
};
[[nodiscard]] QJsonValue to_json(const CancelledNotificationParams& p);
void from_json(const QJsonValue& j, CancelledNotificationParams& p);

inline constexpr std::string_view method_notifications_cancelled =
    "notifications/cancelled";

// --------------------------------------------------------------------------
// Progress
// --------------------------------------------------------------------------

/// Progress tokens are opaque values carried in `_meta.progressToken`
/// on the originating request. The spec permits string or number; we
/// normalise to a thin variant.
class ProgressToken {
public:
    ProgressToken(void) : m_value(std::int64_t{0}) {}
    ProgressToken(std::string s) : m_value(std::move(s)) {}
    ProgressToken(const char* s) : m_value(std::string{s}) {}
    ProgressToken(std::int64_t i) : m_value(i) {}
    ProgressToken(int i) : m_value(static_cast<std::int64_t>(i)) {}

    [[nodiscard]] bool is_string(void) const noexcept {
        return m_value.index() == 0;
    }
    [[nodiscard]] bool is_integer(void) const noexcept {
        return m_value.index() == 1;
    }

    [[nodiscard]] const std::string& as_string(void) const {
        return std::get<0>(m_value);
    }
    [[nodiscard]] std::int64_t as_integer(void) const {
        return std::get<1>(m_value);
    }
    [[nodiscard]] std::string canonical(void) const;

    friend bool operator==(const ProgressToken&,
                           const ProgressToken&) = default;

private:
    std::variant<std::string, std::int64_t> m_value;
};
[[nodiscard]] QJsonValue to_json(const ProgressToken& t);
void from_json(const QJsonValue& j, ProgressToken& t);

struct ProgressNotificationParams {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total = {};
    std::optional<std::string> message = {};
};
[[nodiscard]] QJsonValue to_json(const ProgressNotificationParams& p);
void from_json(const QJsonValue& j, ProgressNotificationParams& p);

inline constexpr std::string_view method_notifications_progress =
    "notifications/progress";

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
[[nodiscard]] std::string_view to_string(LoggingLevel l) noexcept;
[[nodiscard]] QJsonValue to_json(LoggingLevel l);
void from_json(const QJsonValue& j, LoggingLevel& l);

struct SetLevelRequestParams {
    LoggingLevel level;
};
[[nodiscard]] QJsonValue to_json(const SetLevelRequestParams& p);
void from_json(const QJsonValue& j, SetLevelRequestParams& p);

struct LoggingMessageNotificationParams {
    LoggingLevel level;
    std::optional<std::string> logger = {};  // optional name of the logger source
    Json data;                          // free-form payload (string/object/...)
};
[[nodiscard]] QJsonValue to_json(const LoggingMessageNotificationParams& p);
void from_json(const QJsonValue& j, LoggingMessageNotificationParams& p);

inline constexpr std::string_view method_logging_set_level = "logging/setLevel";
inline constexpr std::string_view method_notifications_message =
    "notifications/message";

// --------------------------------------------------------------------------
// Ping
// --------------------------------------------------------------------------

inline constexpr std::string_view method_ping = "ping";

// --------------------------------------------------------------------------
// Sampling (sampling/createMessage) — server-initiated LLM calls
// --------------------------------------------------------------------------

struct ModelHint {
    std::optional<std::string> name = {};
};
[[nodiscard]] QJsonValue to_json(const ModelHint& h);
void from_json(const QJsonValue& j, ModelHint& h);

struct ModelPreferences {
    std::optional<std::vector<ModelHint>> hints = {};
    std::optional<double> cost_priority = {};
    std::optional<double> speed_priority = {};
    std::optional<double> intelligence_priority = {};
};
[[nodiscard]] QJsonValue to_json(const ModelPreferences& p);
void from_json(const QJsonValue& j, ModelPreferences& p);

enum class IncludeContext { none, this_server, all_servers };
[[nodiscard]] std::string_view to_string(IncludeContext c) noexcept;
[[nodiscard]] QJsonValue to_json(IncludeContext c);
void from_json(const QJsonValue& j, IncludeContext& c);

/// Sampling tool-choice mode (2025-11-25). The spec lets the
/// requester force / forbid / pin tool use:
///   - "auto"   — model decides whether to call tools (default)
///   - "any"    — model MUST call at least one tool
///   - "none"   — model MUST NOT call tools
///   - "tool"   — model MUST call the specific tool named
enum class ToolChoiceMode { m_auto, any, none, tool };
[[nodiscard]] std::string_view to_string(ToolChoiceMode m) noexcept;
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
    std::optional<std::string> name = {};
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
    std::vector<ContentBlock> content;
};
[[nodiscard]] QJsonValue to_json(const SamplingMessage& m);
void from_json(const QJsonValue& j, SamplingMessage& m);

struct CreateMessageRequestParams {
    std::vector<SamplingMessage> messages;
    std::optional<ModelPreferences> model_preferences = {};
    std::optional<std::string> system_prompt = {};
    std::optional<IncludeContext> include_context = {};
    std::optional<double> temperature = {};
    std::int64_t max_tokens{};  // required by spec
    std::optional<std::vector<std::string>> stop_sequences = {};
    std::optional<Json> metadata = {};
    /// 2025-11-25: tools the assistant may call during this
    /// sampling. Server side is expected to translate to the
    /// underlying LLM's tool-calling shape; `Tool::input_schema`
    /// is the JSON Schema the model is told to fit.
    std::optional<std::vector<Tool>> tools = {};
    /// 2025-11-25: how to constrain tool calling.
    std::optional<ToolChoice> tool_choice = {};
};
[[nodiscard]] QJsonValue to_json(const CreateMessageRequestParams& p);
void from_json(const QJsonValue& j, CreateMessageRequestParams& p);

struct CreateMessageResult {
    Role role;
    std::vector<ContentBlock> content = {};
    std::string model;
    std::optional<std::string> stop_reason = {};
    /// Top-level _meta envelope for protocol-level metadata.
    std::optional<Json> meta = {};
};
[[nodiscard]] QJsonValue to_json(const CreateMessageResult& r);
void from_json(const QJsonValue& j, CreateMessageResult& r);

inline constexpr std::string_view method_sampling_create_message =
    "sampling/createMessage";

// --------------------------------------------------------------------------
// Roots (client → server: filesystem/URI scopes)
// --------------------------------------------------------------------------

struct Root {
    std::string uri;
    std::optional<std::string> name = {};
};
[[nodiscard]] QJsonValue to_json(const Root& r);
void from_json(const QJsonValue& j, Root& r);

struct ListRootsResult {
    std::vector<Root> roots;
};
[[nodiscard]] QJsonValue to_json(const ListRootsResult& r);
void from_json(const QJsonValue& j, ListRootsResult& r);

inline constexpr std::string_view method_roots_list = "roots/list";
inline constexpr std::string_view method_notifications_roots_list_changed =
    "notifications/roots/list_changed";

// --------------------------------------------------------------------------
// Completion (autocomplete)
// --------------------------------------------------------------------------

struct ResourceTemplateReference {
    std::string uri_template;
};
[[nodiscard]] QJsonValue to_json(const ResourceTemplateReference& r);
void from_json(const QJsonValue& j, ResourceTemplateReference& r);

struct PromptReference {
    std::string name;
    std::optional<std::string> title = {};
};
[[nodiscard]] QJsonValue to_json(const PromptReference& r);
void from_json(const QJsonValue& j, PromptReference& r);

using CompletionReference =
    std::variant<ResourceTemplateReference, PromptReference>;
[[nodiscard]] QJsonValue to_json(const CompletionReference& r);
void from_json(const QJsonValue& j, CompletionReference& r);

struct CompleteArgument {
    std::string name;
    std::string value;
};
[[nodiscard]] QJsonValue to_json(const CompleteArgument& a);
void from_json(const QJsonValue& j, CompleteArgument& a);

struct CompleteRequestParams {
    CompletionReference reference;
    CompleteArgument argument;
    std::optional<std::unordered_map<std::string, std::string>>
        context_arguments;
};
[[nodiscard]] QJsonValue to_json(const CompleteRequestParams& p);
void from_json(const QJsonValue& j, CompleteRequestParams& p);

struct CompletionValues {
    std::vector<std::string> values;
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

inline constexpr std::string_view method_completion_complete =
    "completion/complete";

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
    std::string message;
    /// JSON Schema describing the expected response. Per spec, restricted
    /// to a flat object of primitive properties; we don't enforce that
    /// here so applications can negotiate richer shapes when both sides
    /// agree, but the basic SDK sticks to the spec.
    Json requested_schema;
};
[[nodiscard]] QJsonValue to_json(const ElicitFormRequestParams& p);
void from_json(const QJsonValue& j, ElicitFormRequestParams& p);

struct ElicitUrlRequestParams {
    std::string message;
    /// HTTPS URL the user is asked to visit out-of-band.
    std::string url;
    /// Identifier the server uses to correlate later
    /// `notifications/elicitation/complete` notifications.
    std::string elicitation_id;
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
    std::optional<Json> content = {};
};
[[nodiscard]] QJsonValue to_json(const ElicitResult& r);
void from_json(const QJsonValue& j, ElicitResult& r);

/// URL-mode out-of-band completion notification. Emitted by the server
/// or relayed by the client once the out-of-band flow finishes — but
/// the spec leaves precise emitter conventions open. The MCP SDK
/// accepts the notification on either side.
struct ElicitationCompleteNotificationParams {
    std::string elicitation_id;
};
[[nodiscard]] QJsonValue
to_json(const ElicitationCompleteNotificationParams& p);
void from_json(const QJsonValue& j, ElicitationCompleteNotificationParams& p);

inline constexpr std::string_view method_elicitation_create =
    "elicitation/create";
inline constexpr std::string_view method_notifications_elicitation_complete =
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
    std::vector<ElicitUrlRequestParams> elicitations;
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
    std::string taskId;
    TaskStatus status;
    /// Optional human-readable progress description.
    std::optional<std::string> status_message = {};
    /// ISO 8601 UTC timestamps.
    std::string created_at;
    std::string last_updated_at;
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
    std::string task_id;
};
[[nodiscard]] QJsonValue to_json(const GetTaskRequestParams& p);
void from_json(const QJsonValue& j, GetTaskRequestParams& p);

struct GetTaskResultRequestParams {
    std::string task_id;
};
[[nodiscard]] QJsonValue to_json(const GetTaskResultRequestParams& p);
void from_json(const QJsonValue& j, GetTaskResultRequestParams& p);

struct ListTasksRequestParams {
    std::optional<std::string> cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListTasksRequestParams& p);
void from_json(const QJsonValue& j, ListTasksRequestParams& p);

struct ListTasksResult {
    std::vector<Task> tasks;
    std::optional<std::string> next_cursor = {};
};
[[nodiscard]] QJsonValue to_json(const ListTasksResult& r);
void from_json(const QJsonValue& j, ListTasksResult& r);

struct CancelTaskRequestParams {
    std::string task_id;
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

inline constexpr std::string_view method_tasks_get = "tasks/get";
inline constexpr std::string_view method_tasks_result = "tasks/result";
inline constexpr std::string_view method_tasks_list = "tasks/list";
inline constexpr std::string_view method_tasks_cancel = "tasks/cancel";
inline constexpr std::string_view method_notifications_tasks_status =
    "notifications/tasks/status";

/// Meta key the spec mandates on `tasks/result` payloads to bind the
/// result back to its task. Server-side tooling sets it; clients can
/// optionally check it.
inline constexpr std::string_view tasks_related_task_meta_key =
    "io.modelcontextprotocol/related-task";

}  // namespace mcp

// std::hash specialization so RequestId can key unordered containers.
namespace std {
template <> struct hash<::mcp::RequestId> {
    [[nodiscard]] std::size_t
    operator()(const ::mcp::RequestId& id) const noexcept {
        return std::hash<std::string>{}(id.canonical());
    }
};
}  // namespace std
