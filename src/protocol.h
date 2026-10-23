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
#include <QVariant>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mcp
{

  Q_NAMESPACE

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
  ///
  /// The value is the QJsonValue itself (the struct inherits QJsonValue), so
  /// no custom metatype registration or extra QVariant boxing is needed.
  /// Conversion operators expose the string and integer views; assignment
  /// accepts a wire value and validates the kind.
  struct RequestId : QJsonValue
  {
    RequestId(void) : QJsonValue(QJsonValue::Null) {}
    RequestId(QString s) : QJsonValue(std::move(s)) {}
    RequestId(const char* s) : QJsonValue(QString::fromUtf8(s)) {}
    RequestId(std::int64_t i) : QJsonValue(static_cast<qint64>(i)) {}
    RequestId(int i) : QJsonValue(static_cast<qint64>(i)) {}

    [[nodiscard]] bool is_string(void) const noexcept { return isString(); }
    [[nodiscard]] bool is_integer(void) const noexcept { return isDouble(); }

    [[nodiscard]] QString as_string(void) const { return toString(); }
    [[nodiscard]] std::int64_t as_integer(void) const { return toInteger(); }

    /// Canonical string form used for hash/map keys. String ids are returned
    /// as-is with a leading 's:' marker; integer ids get an 'i:' marker. This
    /// guarantees that the integer 5 and the string "5" do not collide.
    [[nodiscard]] QString canonical(void) const;

    friend bool operator==(const RequestId&, const RequestId&) = default;

    inline operator std::int64_t(void) const { return as_integer(); }
    inline operator QString(void) const { return as_string(); }
    RequestId& operator=(const QJsonValue&);
  };

  /// A JSON-RPC 2.0 request. Always has an id.
  struct JsonRpcRequest
  {
    RequestId id;
    QString method;
    QJsonValue params = {};

    [[nodiscard]] operator QJsonValue() const;
    JsonRpcRequest& operator=(const QJsonValue&);
  };

  /// A JSON-RPC 2.0 notification. Has no id; sender expects no response.
  struct JsonRpcNotification
  {
    QString method;
    QJsonValue params = {};

    [[nodiscard]] operator QJsonValue() const;
    JsonRpcNotification& operator=(const QJsonValue&);
  };

  /// A JSON-RPC 2.0 response. Carries either a `result` or an `error`. Per
  /// the spec the id is REQUIRED on success, and `null`/absent only on parse
  /// errors that prevented identifying the request — we model that with a
  /// null RequestId.
  struct JsonRpcResponse
  {
    RequestId id;
    std::variant<QJsonValue /*result*/, ErrorObject /*error*/> outcome;

    [[nodiscard]] bool is_error(void) const noexcept
    {
      return outcome.index() == 1;
    }
    [[nodiscard]] bool is_success(void) const noexcept
    {
      return outcome.index() == 0;
    }

    [[nodiscard]] const QJsonValue& result(void) const
    {
      return std::get<0>(outcome);
    }
    [[nodiscard]] const ErrorObject& error(void) const
    {
      return std::get<1>(outcome);
    }

    [[nodiscard]] operator QJsonValue() const;
    JsonRpcResponse& operator=(const QJsonValue&);
  };

  // Codec for the wire error object (defined here rather than in error.h so
  // that header stays free of the JSON codec).

  /// Tagged union over every JSON-RPC frame we accept off the wire. Use
  /// `std::visit` to dispatch on the concrete kind.
  using JsonRpcMessage
      = std::variant<JsonRpcRequest, JsonRpcNotification, JsonRpcResponse>;

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
  struct Icon
  {
    /// `src` is either an absolute URL or a `data:` URL (RFC 2397).
    QString src;
    QString mime_type = {};
    /// Whitespace-delimited list of WxH dimensions ("16x16 32x32"),
    /// matching HTML `<link rel=icon sizes>`.
    QString sizes = {};
    /// "light" / "dark" / "monochrome" / app-defined.
    QString theme = {};

    [[nodiscard]] operator QJsonValue() const;
    Icon& operator=(const QJsonValue&);
  };

  struct Implementation
  {
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
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    Implementation& operator=(const QJsonValue&);
  };

  // --------------------------------------------------------------------------
  // Tasks: small capability/augmentation types (the primitive's own
  // methods + Task struct live further down). Hoisted so capabilities
  // and CallToolRequestParams below can hold std::optional<...> of them.
  // --------------------------------------------------------------------------

  struct TaskAugmentation
  {
    /// Milliseconds the task is retained after creation. Empty / null
    /// means unlimited (or implementation-defined retention).
    std::optional<std::int64_t> ttl = {};

    [[nodiscard]] operator QJsonValue() const;
    TaskAugmentation& operator=(const QJsonValue&);
  };

  struct TasksRequestsCapability
  {
    /// Per-namespace presence-only objects — e.g. `tools.call`,
    /// `sampling.createMessage`, `elicitation.create`. Held as raw
    /// json so a future spec extension (a new method) round-trips
    /// without an SDK release.
    QJsonValue tools;
    QJsonValue sampling;
    QJsonValue elicitation;

    [[nodiscard]] operator QJsonValue() const;
    TasksRequestsCapability& operator=(const QJsonValue&);
  };

  struct TasksCapability
  {
    QJsonValue list;
    QJsonValue cancel;
    std::optional<TasksRequestsCapability> requests = {};

    [[nodiscard]] operator QJsonValue() const;
    TasksCapability& operator=(const QJsonValue&);
  };

  // --------------------------------------------------------------------------
  // Capabilities
  // --------------------------------------------------------------------------

  struct RootsCapability
  {
    std::optional<bool> list_changed = {};

    [[nodiscard]] operator QJsonValue() const;
    RootsCapability& operator=(const QJsonValue&);
  };

  struct SamplingCapability
  {
    // Per spec: `context?: object`, `tools?: object`. Presence-only flags;
    // we capture the raw object so future fields round-trip.
    QJsonValue context = {};
    QJsonValue tools;

    [[nodiscard]] operator QJsonValue() const;
    SamplingCapability& operator=(const QJsonValue&);
  };

  struct ElicitationCapability
  {
    QJsonValue form; // form-based elicitation
    QJsonValue url;  // URL-based elicitation

    [[nodiscard]] operator QJsonValue() const;
    ElicitationCapability& operator=(const QJsonValue&);
  };

  struct ClientCapabilities
  {
    QJsonValue experimental;
    std::optional<RootsCapability> roots = {};
    std::optional<SamplingCapability> sampling = {};
    std::optional<ElicitationCapability> elicitation = {};
    /// 2025-11-25: client advertises which task-augmentable inbound
    /// methods it can handle (e.g. sampling/createMessage as a task).
    std::optional<TasksCapability> tasks = {};

    [[nodiscard]] operator QJsonValue() const;
    ClientCapabilities& operator=(const QJsonValue&);
  };

  struct PromptsCapability
  {
    std::optional<bool> list_changed = {};

    [[nodiscard]] operator QJsonValue() const;
    PromptsCapability& operator=(const QJsonValue&);
  };

  struct ResourcesCapability
  {
    std::optional<bool> subscribe = {};
    std::optional<bool> list_changed = {};

    [[nodiscard]] operator QJsonValue() const;
    ResourcesCapability& operator=(const QJsonValue&);
  };

  struct ToolsCapability
  {
    std::optional<bool> list_changed = {};

    [[nodiscard]] operator QJsonValue() const;
    ToolsCapability& operator=(const QJsonValue&);
  };

  struct ServerCapabilities
  {
    QJsonValue experimental;
    QJsonValue logging;     // presence-only
    QJsonValue completions; // presence-only
    std::optional<PromptsCapability> prompts = {};
    std::optional<ResourcesCapability> resources = {};
    std::optional<ToolsCapability> tools = {};
    /// 2025-11-25: which task-augmentable inbound methods the server
    /// can handle (e.g. tools/call as a task), plus list/cancel.
    std::optional<TasksCapability> tasks = {};

    [[nodiscard]] operator QJsonValue() const;
    ServerCapabilities& operator=(const QJsonValue&);
  };

  // --------------------------------------------------------------------------
  // initialize / initialized
  // --------------------------------------------------------------------------

  struct InitializeRequestParams
  {
    QString protocol_version;
    ClientCapabilities capabilities;
    Implementation client_info;

    [[nodiscard]] operator QJsonValue() const;
    InitializeRequestParams& operator=(const QJsonValue&);
  };

  struct InitializeResult
  {
    QString protocol_version;
    ServerCapabilities capabilities;
    Implementation server_info;
    QString instructions = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    InitializeResult& operator=(const QJsonValue&);
  };

  inline const QString method_initialize = "initialize";
  inline const QString method_notifications_initialized = "notifications/initialized";

  // --------------------------------------------------------------------------
  // Content blocks
  // --------------------------------------------------------------------------

  enum class Role : int
  {
    user,
    assistant
  };
  Q_ENUM_NS(Role)

  struct Annotations
  {
    std::optional<QList<Role>> audience = {};
    std::optional<double> priority = {};
    QString last_modified = {};

    [[nodiscard]] operator QJsonValue() const;
    Annotations& operator=(const QJsonValue&);
  };

  struct TextContent
  {
    QString text;
    std::optional<Annotations> annotations = {};

    [[nodiscard]] operator QJsonValue() const;
    TextContent& operator=(const QJsonValue&);
  };

  struct ImageContent
  {
    QString data; // base64
    QString mime_type;
    std::optional<Annotations> annotations = {};

    [[nodiscard]] operator QJsonValue() const;
    ImageContent& operator=(const QJsonValue&);
  };

  struct AudioContent
  {
    QString data; // base64
    QString mime_type;
    std::optional<Annotations> annotations = {};

    [[nodiscard]] operator QJsonValue() const;
    AudioContent& operator=(const QJsonValue&);
  };

  // --------------------------------------------------------------------------
  // Resources (must come before ContentBlock; ResourceLink and
  // EmbeddedResource are content variants that reference resource shapes)
  // --------------------------------------------------------------------------

  struct TextResourceContents
  {
    QString uri;
    QString mime_type = {};
    QString text;

    [[nodiscard]] operator QJsonValue() const;
    TextResourceContents& operator=(const QJsonValue&);
  };

  struct BlobResourceContents
  {
    QString uri;
    QString mime_type = {};
    QString blob; // base64

    [[nodiscard]] operator QJsonValue() const;
    BlobResourceContents& operator=(const QJsonValue&);
  };

  /// Resource contents carry either text or a base64 blob; on the wire
  /// the discriminator is which member ("text" or "blob") is present.
  struct ResourceContents : QJsonValue
  {
    ResourceContents(void) : QJsonValue(QJsonValue::Null) {}
    ResourceContents(const QJsonValue& j) : QJsonValue(j) {}
    ResourceContents(const TextResourceContents& c) : QJsonValue(c) {}
    ResourceContents(const BlobResourceContents& c) : QJsonValue(c) {}

    [[nodiscard]] bool is_text(void) const
    {
      return toObject().contains("text");
    }
    [[nodiscard]] bool is_blob(void) const
    {
      return toObject().contains("blob");
    }

    [[nodiscard]] operator TextResourceContents(void) const
    {
      return from_json<TextResourceContents>(*this);
    }
    [[nodiscard]] operator BlobResourceContents(void) const
    {
      return from_json<BlobResourceContents>(*this);
    }

    ResourceContents& operator=(const QJsonValue& j)
    {
      const QJsonObject o = as_object(j);
      if(o.contains("text"))
      {
        const TextResourceContents decoded
            = from_json<TextResourceContents>(j);
        QJsonValue::operator=(decoded);
      }
      else if(o.contains("blob"))
      {
        const BlobResourceContents decoded
            = from_json<BlobResourceContents>(j);
        QJsonValue::operator=(decoded);
      }
      else
      {
        throw Error(error_code::parse_error,
                    "ResourceContents must have either \"text\" or "
                    "\"blob\"");
      }
      return *this;
    }
  };

  struct Resource
  {
    QString uri;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {}; // bytes
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    Resource& operator=(const QJsonValue&);
  };

  struct ResourceTemplate
  {
    QString uri_template;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ResourceTemplate& operator=(const QJsonValue&);
  };

  struct ResourceLink
  {
    QString uri;
    QString name;
    QString title = {};
    QString description = {};
    QString mime_type = {};
    std::optional<Annotations> annotations = {};
    std::optional<std::int64_t> size = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ResourceLink& operator=(const QJsonValue&);
  };

  struct EmbeddedResource
  {
    ResourceContents resource;
    std::optional<Annotations> annotations = {};

    [[nodiscard]] operator QJsonValue() const;
    EmbeddedResource& operator=(const QJsonValue&);
  };

  /// 2025-11-25: tool-use content block. Used in sampling messages to
  /// represent the assistant emitting a tool call (i.e. an
  /// agentic-loop step). Wire type discriminator: `"tool_use"`.
  struct ToolUseContent
  {
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

    [[nodiscard]] operator QJsonValue() const;
    ToolUseContent& operator=(const QJsonValue&);
  };

  /// Plain (non-tool) content blocks — the subset legal as the
  /// `content` of a ToolResultContent. The spec forbids nested
  /// tool_use/tool_result, and modelling the constraint in the type
  /// system catches malformed payloads at compile time.
  using PlainContentBlock
      = std::variant<TextContent, ImageContent, AudioContent, ResourceLink,
                     EmbeddedResource>;
  [[nodiscard]] QJsonValue to_json(const PlainContentBlock& c);
  void from_json(const QJsonValue& j, PlainContentBlock& c);

  /// 2025-11-25: tool-result content block. Carries the result of a
  /// previous `tool_use` block, referenced by `toolUseId`. Wire type
  /// discriminator: `"tool_result"`.
  struct ToolResultContent
  {
    /// The `id` of the originating ToolUseContent.
    QString tool_use_id;
    /// The tool's actual output. Restricted to plain content blocks
    /// per spec (no nested tool_* allowed).
    QList<PlainContentBlock> content;
    /// True when the tool errored. The content array typically
    /// carries a TextContent describing the error in that case.
    std::optional<bool> is_error = {};
    std::optional<Annotations> annotations = {};

    [[nodiscard]] operator QJsonValue() const;
    ToolResultContent& operator=(const QJsonValue&);
  };

  /// Content blocks used by tools/calls, prompts, and sampling. The
  /// type discriminator on the wire is `"text" | "image" | "audio"
  /// | "resource_link" | "resource" | "tool_use" | "tool_result"`.
  /// The last two are 2025-11-25 additions for agentic sampling
  /// loops.
  using ContentBlock
      = std::variant<TextContent, ImageContent, AudioContent, ResourceLink,
                     EmbeddedResource, ToolUseContent, ToolResultContent>;

  [[nodiscard]] QJsonValue to_json(const ContentBlock& c);
  void from_json(const QJsonValue& j, ContentBlock& c);

  // --------------------------------------------------------------------------
  // Tools
  // --------------------------------------------------------------------------

  struct ToolAnnotations
  {
    QString title = {};
    std::optional<bool> read_only_hint = {};
    std::optional<bool> destructive_hint = {};
    std::optional<bool> idempotent_hint = {};
    std::optional<bool> open_world_hint = {};

    [[nodiscard]] operator QJsonValue() const;
    ToolAnnotations& operator=(const QJsonValue&);
  };

  /// 2025-11-25: per-tool execution policy. The receiver advertises
  /// whether a tool supports being task-augmented at all, optionally,
  /// or requires it.
  enum class TaskSupport
  {
    forbidden,
    optional,
    required
  };
  Q_ENUM_NS(TaskSupport)

  struct ToolExecution
  {
    /// "forbidden" / "optional" / "required". Default is
    /// "optional" when `execution` is absent.
    TaskSupport task_support = TaskSupport::optional;

    [[nodiscard]] operator QJsonValue() const;
    ToolExecution& operator=(const QJsonValue&);
  };

  struct Tool
  {
    QString name;
    QString title = {};
    QString description = {};
    QJsonValue input_schema;  // JSON Schema; type:"object"
    QJsonValue output_schema = {}; // optional JSON Schema
    std::optional<ToolAnnotations> annotations = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    std::optional<ToolExecution> execution = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    Tool& operator=(const QJsonValue&);
  };

  struct ListToolsRequestParams
  {
    QString cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListToolsRequestParams& operator=(const QJsonValue&);
  };

  struct ListToolsResult
  {
    QList<Tool> tools;
    QString next_cursor = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ListToolsResult& operator=(const QJsonValue&);
  };

  struct CallToolRequestParams
  {
    QString name;
    QJsonValue arguments; // JSON object
    /// When set, the request is task-augmented: the receiver should
    /// return a CreateTaskResult envelope and run the call async. The
    /// final CallToolResult is fetched via `tasks/result`.
    std::optional<TaskAugmentation> task = {};

    [[nodiscard]] operator QJsonValue() const;
    CallToolRequestParams& operator=(const QJsonValue&);
  };

  struct CallToolResult
  {
    QList<ContentBlock> content;
    QJsonValue structured_content = {}; // matches outputSchema
    std::optional<bool> is_error = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    CallToolResult& operator=(const QJsonValue&);
  };

  inline const QString method_tools_list = "tools/list";
  inline const QString method_tools_call = "tools/call";

  // --------------------------------------------------------------------------
  // Resource requests / responses
  // --------------------------------------------------------------------------

  struct ListResourcesRequestParams
  {
    QString cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListResourcesRequestParams& operator=(const QJsonValue&);
  };

  struct ListResourcesResult
  {
    QList<Resource> resources;
    QString next_cursor = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ListResourcesResult& operator=(const QJsonValue&);
  };

  struct ListResourceTemplatesRequestParams
  {
    QString cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListResourceTemplatesRequestParams& operator=(const QJsonValue&);
  };

  struct ListResourceTemplatesResult
  {
    QList<ResourceTemplate> resource_templates;
    QString next_cursor = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ListResourceTemplatesResult& operator=(const QJsonValue&);
  };

  struct ReadResourceRequestParams
  {
    QString uri;

    [[nodiscard]] operator QJsonValue() const;
    ReadResourceRequestParams& operator=(const QJsonValue&);
  };

  struct ReadResourceResult
  {
    QList<ResourceContents> contents;
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ReadResourceResult& operator=(const QJsonValue&);
  };

  struct SubscribeRequestParams
  {
    QString uri;

    [[nodiscard]] operator QJsonValue() const;
    SubscribeRequestParams& operator=(const QJsonValue&);
  };

  struct UnsubscribeRequestParams
  {
    QString uri;

    [[nodiscard]] operator QJsonValue() const;
    UnsubscribeRequestParams& operator=(const QJsonValue&);
  };

  struct ResourceUpdatedNotificationParams
  {
    QString uri;

    [[nodiscard]] operator QJsonValue() const;
    ResourceUpdatedNotificationParams& operator=(const QJsonValue&);
  };

  inline const QString method_resources_list                        = "resources/list";
  inline const QString method_resources_templates_list              = "resources/templates/list";
  inline const QString method_resources_read                        = "resources/read";
  inline const QString method_resources_subscribe                   = "resources/subscribe";
  inline const QString method_resources_unsubscribe                 = "resources/unsubscribe";
  inline const QString method_notifications_resources_list_changed  = "notifications/resources/list_changed";
  inline const QString method_notifications_resources_updated       = "notifications/resources/updated";

  // --------------------------------------------------------------------------
  // Prompts
  // --------------------------------------------------------------------------

  struct PromptArgument
  {
    QString name;
    QString title = {};
    QString description = {};
    std::optional<bool> required = {};

    [[nodiscard]] operator QJsonValue() const;
    PromptArgument& operator=(const QJsonValue&);
  };

  struct Prompt
  {
    QString name;
    QString title = {};
    QString description = {};
    std::optional<QList<PromptArgument>> arguments = {};
    /// 2025-11-25 additions:
    std::optional<QList<Icon>> icons = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    Prompt& operator=(const QJsonValue&);
  };

  struct PromptMessage
  {
    Role role;
    ContentBlock content;

    [[nodiscard]] operator QJsonValue() const;
    PromptMessage& operator=(const QJsonValue&);
  };

  struct ListPromptsRequestParams
  {
    QString cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListPromptsRequestParams& operator=(const QJsonValue&);
  };

  struct ListPromptsResult
  {
    QList<Prompt> prompts;
    QString next_cursor = {};
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    ListPromptsResult& operator=(const QJsonValue&);
  };

  struct GetPromptRequestParams
  {
    QString name;
    std::optional<QHash<QString, QString>> arguments = {};

    [[nodiscard]] operator QJsonValue() const;
    GetPromptRequestParams& operator=(const QJsonValue&);
  };

  struct GetPromptResult
  {
    QString description = {};
    QList<PromptMessage> messages;
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    GetPromptResult& operator=(const QJsonValue&);
  };

  inline const QString method_prompts_list = "prompts/list";
  inline const QString method_prompts_get = "prompts/get";
  inline const QString method_notifications_prompts_list_changed
      = "notifications/prompts/list_changed";

  // --------------------------------------------------------------------------
  // Cancellation
  // --------------------------------------------------------------------------

  struct CancelledNotificationParams
  {
    RequestId request_id;
    QString reason = {};

    [[nodiscard]] operator QJsonValue() const;
    CancelledNotificationParams& operator=(const QJsonValue&);
  };

  inline const QString method_notifications_cancelled
      = "notifications/cancelled";

  // --------------------------------------------------------------------------
  // Progress
  // --------------------------------------------------------------------------

  /// Progress tokens are opaque values carried in `_meta.progressToken`
  /// on the originating request. The spec permits string or number; we
  /// normalise to a thin variant.
  inline std::size_t qHash(const RequestId& id, std::size_t seed = 0) noexcept
  {
    return qHash(id.canonical(), seed);
  }

  struct ProgressToken : QJsonValue
  {
    ProgressToken(void) : QJsonValue(QJsonValue::Null) {}
    ProgressToken(QString s) : QJsonValue(std::move(s)) {}
    ProgressToken(const char* s) : QJsonValue(QString::fromUtf8(s)) {}
    ProgressToken(std::int64_t i) : QJsonValue(static_cast<qint64>(i)) {}
    ProgressToken(int i) : QJsonValue(static_cast<qint64>(i)) {}

    [[nodiscard]] bool is_string(void) const noexcept { return isString(); }
    [[nodiscard]] bool is_integer(void) const noexcept { return isDouble(); }

    [[nodiscard]] QString as_string(void) const { return toString(); }
    [[nodiscard]] std::int64_t as_integer(void) const { return toInteger(); }
    [[nodiscard]] QString canonical(void) const;

    friend bool operator==(const ProgressToken&, const ProgressToken&)
        = default;

    ProgressToken& operator=(const QJsonValue&);
  };

  struct ProgressNotificationParams
  {
    ProgressToken progress_token;
    double progress;
    std::optional<double> total = {};
    QString message = {};

    [[nodiscard]] operator QJsonValue() const;
    ProgressNotificationParams& operator=(const QJsonValue&);
  };

  inline const QString method_notifications_progress = "notifications/progress";

  // --------------------------------------------------------------------------
  // Logging (server → client log messages)
  // --------------------------------------------------------------------------

  /// Per-spec MCP log severity levels — this is the protocol-level
  /// "logging" capability, distinct from this library's internal
  /// LogLevel which writes to the host's stderr.
  enum class LoggingLevel
  {
    debug,
    info,
    notice,
    warning,
    error,
    critical,
    alert,
    emergency,
  };
  Q_ENUM_NS(LoggingLevel)

  struct SetLevelRequestParams
  {
    LoggingLevel level;

    [[nodiscard]] operator QJsonValue() const;
    SetLevelRequestParams& operator=(const QJsonValue&);
  };

  struct LoggingMessageNotificationParams
  {
    LoggingLevel level;
    QString logger = {}; // optional name of the logger source
    QJsonValue data;     // free-form payload (string/object/...)

    [[nodiscard]] operator QJsonValue() const;
    LoggingMessageNotificationParams& operator=(const QJsonValue&);
  };

  inline const QString method_logging_set_level = "logging/setLevel";
  inline const QString method_notifications_message = "notifications/message";

  // --------------------------------------------------------------------------
  // Ping
  // --------------------------------------------------------------------------

  inline const QString method_ping = "ping";

  // --------------------------------------------------------------------------
  // Sampling (sampling/createMessage) — server-initiated LLM calls
  // --------------------------------------------------------------------------

  struct ModelHint
  {
    QString name = {};

    [[nodiscard]] operator QJsonValue() const;
    ModelHint& operator=(const QJsonValue&);
  };

  struct ModelPreferences
  {
    std::optional<QList<ModelHint>> hints = {};
    std::optional<double> cost_priority = {};
    std::optional<double> speed_priority = {};
    std::optional<double> intelligence_priority = {};

    [[nodiscard]] operator QJsonValue() const;
    ModelPreferences& operator=(const QJsonValue&);
  };

  enum class IncludeContext
  {
    none,
    thisServer,
    allServers
  };
  Q_ENUM_NS(IncludeContext)

  /// Sampling tool-choice mode (2025-11-25). The spec lets the
  /// requester force / forbid / pin tool use:
  ///   - "auto"   — model decides whether to call tools (default)
  ///   - "any"    — model MUST call at least one tool
  ///   - "none"   — model MUST NOT call tools
  ///   - "tool"   — model MUST call the specific tool named
  enum class ToolChoiceMode
  {
    m_auto,
    any,
    none,
    tool
  };
  Q_ENUM_NS(ToolChoiceMode)
  [[nodiscard]] QString to_string(ToolChoiceMode m) noexcept;
  [[nodiscard]] QJsonValue to_json(ToolChoiceMode m);
  void from_json(const QJsonValue& j, ToolChoiceMode& m);

  /// Tool choice. The wire shape is either a bare string ("auto" |
  /// "any" | "none") or an object `{ type: "tool", name: "..." }`
  /// for the named-tool case. We model it as a struct with both
  /// fields optional; serialise emits the object form when
  /// `mode == tool` AND `name` is present.
  struct ToolChoice
  {
    std::optional<ToolChoiceMode> mode = {};
    /// Required when mode == ToolChoiceMode::tool.
    QString name = {};

    [[nodiscard]] operator QJsonValue() const;
    ToolChoice& operator=(const QJsonValue&);
  };

  /// A message in a sampling exchange. Phase 3 supports text/image/audio
  /// content; tool_use and tool_result variants land in Phase 4 alongside
  /// the rest of the agentic-loop primitives.
  ///
  /// The spec's `content` field accepts either a single block or an array
  /// of blocks; we always model it as a vector internally so multi-block
  /// messages don't get silently truncated. On the wire we emit an array
  /// when there's more than one block and a single-block object otherwise
  /// (matching what nearly every existing implementation produces).
  struct SamplingMessage
  {
    Role role;
    QList<ContentBlock> content;

    [[nodiscard]] operator QJsonValue() const;
    SamplingMessage& operator=(const QJsonValue&);
  };

  struct CreateMessageRequestParams
  {
    QList<SamplingMessage> messages;
    std::optional<ModelPreferences> model_preferences = {};
    QString system_prompt = {};
    std::optional<IncludeContext> include_context = {};
    std::optional<double> temperature = {};
    std::int64_t max_tokens{}; // required by spec
    std::optional<QList<QString>> stop_sequences = {};
    QJsonValue metadata = {};
    /// 2025-11-25: tools the assistant may call during this
    /// sampling. Server side is expected to translate to the
    /// underlying LLM's tool-calling shape; `Tool::input_schema`
    /// is the JSON Schema the model is told to fit.
    std::optional<QList<Tool>> tools = {};
    /// 2025-11-25: how to constrain tool calling.
    std::optional<ToolChoice> tool_choice = {};

    [[nodiscard]] operator QJsonValue() const;
    CreateMessageRequestParams& operator=(const QJsonValue&);
  };

  struct CreateMessageResult
  {
    Role role;
    QList<ContentBlock> content = {};
    QString model;
    QString stop_reason = {};
    /// Top-level _meta envelope for protocol-level metadata.
    QJsonValue meta = {};

    [[nodiscard]] operator QJsonValue() const;
    CreateMessageResult& operator=(const QJsonValue&);
  };

  inline const QString method_sampling_create_message
      = "sampling/createMessage";

  // --------------------------------------------------------------------------
  // Roots (client → server: filesystem/URI scopes)
  // --------------------------------------------------------------------------

  struct Root
  {
    QString uri;
    QString name = {};

    [[nodiscard]] operator QJsonValue() const;
    Root& operator=(const QJsonValue&);
  };

  struct ListRootsResult
  {
    QList<Root> roots;

    [[nodiscard]] operator QJsonValue() const;
    ListRootsResult& operator=(const QJsonValue&);
  };

  inline const QString method_roots_list = "roots/list";
  inline const QString method_notifications_roots_list_changed = "notifications/roots/list_changed";

  // --------------------------------------------------------------------------
  // Completion (autocomplete)
  // --------------------------------------------------------------------------

  struct ResourceTemplateReference
  {
    QString uri_template;

    [[nodiscard]] operator QJsonValue() const;
    ResourceTemplateReference& operator=(const QJsonValue&);
  };

  struct PromptReference
  {
    QString name;
    QString title = {};

    [[nodiscard]] operator QJsonValue() const;
    PromptReference& operator=(const QJsonValue&);
  };

  struct CompletionReference : QJsonValue
  {
    CompletionReference(void) : QJsonValue(QJsonValue::Null) {}
    CompletionReference(const QJsonValue& j) : QJsonValue(j) {}
    CompletionReference(const ResourceTemplateReference& r) : QJsonValue(r) {}
    CompletionReference(const PromptReference& r) : QJsonValue(r) {}

    [[nodiscard]] bool is_resource_template(void) const
    {
      return toObject().value("type").toString() == "ref/resource";
    }
    [[nodiscard]] bool is_prompt(void) const
    {
      return toObject().value("type").toString() == "ref/prompt";
    }

    [[nodiscard]] operator ResourceTemplateReference(void) const
    {
      return from_json<ResourceTemplateReference>(*this);
    }
    [[nodiscard]] operator PromptReference(void) const
    {
      return from_json<PromptReference>(*this);
    }

    CompletionReference& operator=(const QJsonValue& j)
    {
      const QJsonObject o = as_object(j);
      const QString t = require<QString>(o, "type");
      if(t == "ref/resource")
      {
        const ResourceTemplateReference decoded
            = from_json<ResourceTemplateReference>(j);
        QJsonValue::operator=(decoded);
      }
      else if(t == "ref/prompt")
      {
        const PromptReference decoded = from_json<PromptReference>(j);
        QJsonValue::operator=(decoded);
      }
      else
      {
        throw Error(error_code::parse_error,
                    QString{ "unknown completion reference type: " } + t);
      }
      return *this;
    }

    CompletionReference& operator=(const ResourceTemplateReference& r)
    {
      QJsonValue::operator=(r);
      return *this;
    }
    CompletionReference& operator=(const PromptReference& r)
    {
      QJsonValue::operator=(r);
      return *this;
    }
  };

  struct CompleteArgument
  {
    QString name;
    QString value;

    [[nodiscard]] operator QJsonValue() const;
    CompleteArgument& operator=(const QJsonValue&);
  };

  struct CompleteRequestParams
  {
    CompletionReference reference;
    CompleteArgument argument;
    std::optional<QHash<QString, QString>> context_arguments;

    [[nodiscard]] operator QJsonValue() const;
    CompleteRequestParams& operator=(const QJsonValue&);
  };

  struct CompletionValues
  {
    QList<QString> values;
    std::optional<std::int64_t> total = {};
    std::optional<bool> has_more = {};

    [[nodiscard]] operator QJsonValue() const;
    CompletionValues& operator=(const QJsonValue&);
  };

  struct CompleteResult
  {
    CompletionValues completion;

    [[nodiscard]] operator QJsonValue() const;
    CompleteResult& operator=(const QJsonValue&);
  };

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

  struct ElicitFormRequestParams
  {
    /// Plain-text rationale for the request, shown to the user.
    QString message;
    /// JSON Schema describing the expected response. Per spec, restricted
    /// to a flat object of primitive properties; we don't enforce that
    /// here so applications can negotiate richer shapes when both sides
    /// agree, but the basic SDK sticks to the spec.
    QJsonValue requested_schema;

    [[nodiscard]] operator QJsonValue() const;
    ElicitFormRequestParams& operator=(const QJsonValue&);
  };

  struct ElicitUrlRequestParams
  {
    QString message;
    /// HTTPS URL the user is asked to visit out-of-band.
    QString url;
    /// Identifier the server uses to correlate later
    /// `notifications/elicitation/complete` notifications.
    QString elicitation_id;

    [[nodiscard]] operator QJsonValue() const;
    ElicitUrlRequestParams& operator=(const QJsonValue&);
  };

  struct ElicitRequestParams : QJsonValue
  {
    ElicitRequestParams(void) : QJsonValue(QJsonValue::Null) {}
    ElicitRequestParams(const QJsonValue& j) : QJsonValue(j) {}
    ElicitRequestParams(const ElicitFormRequestParams& p) : QJsonValue(p) {}
    ElicitRequestParams(const ElicitUrlRequestParams& p) : QJsonValue(p) {}

    [[nodiscard]] bool is_form(void) const
    {
      const auto o = toObject();
      if(auto it = o.constFind("mode"); it != o.constEnd())
        return it->toString() == "form";
      return true;
    }
    [[nodiscard]] bool is_url(void) const
    {
      return toObject().value("mode").toString() == "url";
    }

    [[nodiscard]] operator ElicitFormRequestParams(void) const
    {
      return from_json<ElicitFormRequestParams>(*this);
    }
    [[nodiscard]] operator ElicitUrlRequestParams(void) const
    {
      return from_json<ElicitUrlRequestParams>(*this);
    }

    ElicitRequestParams& operator=(const QJsonValue& j)
    {
      const QJsonObject o = as_object(j);
      QString mode = "form";
      if(auto it = o.constFind("mode"); it != o.constEnd())
      {
        if(!it->isString())
        {
          throw Error(error_code::invalid_params, "elicitation/create: `mode` must be a string");
        }
        mode = it->toString();
      }
      if(mode == "form")
      {
        const ElicitFormRequestParams decoded
            = from_json<ElicitFormRequestParams>(j);
        QJsonValue::operator=(decoded);
      }
      else if(mode == "url")
      {
        const ElicitUrlRequestParams decoded
            = from_json<ElicitUrlRequestParams>(j);
        QJsonValue::operator=(decoded);
      }
      else
      {
        throw Error(error_code::invalid_params,
                    "elicitation/create: unknown mode \"" + mode + "\"");
      }
      return *this;
    }

    ElicitRequestParams& operator=(const ElicitFormRequestParams& p)
    {
      QJsonValue::operator=(p);
      return *this;
    }
    ElicitRequestParams& operator=(const ElicitUrlRequestParams& p)
    {
      QJsonValue::operator=(p);
      return *this;
    }
  };

  enum class ElicitAction
  {
    accept,
    decline,
    cancel
  };
  Q_ENUM_NS(ElicitAction)

  struct ElicitResult
  {
    ElicitAction action;
    /// Form-mode only: the user's submission, conforming to the
    /// server-supplied schema. Absent for decline/cancel and for URL
    /// mode.
    QJsonValue content = {};

    [[nodiscard]] operator QJsonValue() const;
    ElicitResult& operator=(const QJsonValue&);
  };

  /// URL-mode out-of-band completion notification. Emitted by the server
  /// or relayed by the client once the out-of-band flow finishes — but
  /// the spec leaves precise emitter conventions open. The MCP SDK
  /// accepts the notification on either side.
  struct ElicitationCompleteNotificationParams
  {
    QString elicitation_id;

    [[nodiscard]] operator QJsonValue() const;
    ElicitationCompleteNotificationParams& operator=(const QJsonValue&);
  };
  inline const QString method_elicitation_create = "elicitation/create";
  inline const QString method_notifications_elicitation_complete
      = "notifications/elicitation/complete";

  /// 2025-11-25: structured `data` for the JSON-RPC error code
  /// `error_code::url_elicitation_required` (-32042). Sent by the
  /// server when an in-flight request can't proceed without the
  /// user finishing one or more URL-mode elicitation flows. The
  /// client MAY drive the URL flows out-of-band, then retry the
  /// original request.
  struct UrlElicitationRequiredErrorData
  {
    /// One or more URL-mode elicitations the client must run.
    /// Each element is the params shape of an `elicitation/create`
    /// request — message + url + elicitationId.
    QList<ElicitUrlRequestParams> elicitations;

    [[nodiscard]] operator QJsonValue() const;
    UrlElicitationRequiredErrorData& operator=(const QJsonValue&);
  };

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

  enum class TaskStatus
  {
    working,
    input_required,
    completed,
    failed,
    cancelled,
  };
  Q_ENUM_NS(TaskStatus)

  struct Task
  {
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

    [[nodiscard]] operator QJsonValue() const;
    Task& operator=(const QJsonValue&);
  };

  /// Returned in place of the underlying request's normal result when
  /// the request was task-augmented.
  struct CreateTaskResult
  {
    Task task;

    [[nodiscard]] operator QJsonValue() const;
    CreateTaskResult& operator=(const QJsonValue&);
  };

  struct GetTaskRequestParams
  {
    QString task_id;

    [[nodiscard]] operator QJsonValue() const;
    GetTaskRequestParams& operator=(const QJsonValue&);
  };

  struct GetTaskResultRequestParams
  {
    QString task_id;

    [[nodiscard]] operator QJsonValue() const;
    GetTaskResultRequestParams& operator=(const QJsonValue&);
  };

  struct ListTasksRequestParams
  {
    QString cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListTasksRequestParams& operator=(const QJsonValue&);
  };

  struct ListTasksResult
  {
    QList<Task> tasks;
    QString next_cursor = {};

    [[nodiscard]] operator QJsonValue() const;
    ListTasksResult& operator=(const QJsonValue&);
  };

  struct CancelTaskRequestParams
  {
    QString task_id;

    [[nodiscard]] operator QJsonValue() const;
    CancelTaskRequestParams& operator=(const QJsonValue&);
  };

  struct TaskStatusNotificationParams
  {
    /// A point-in-time projection of the underlying Task. Same shape
    /// as Task, no extra fields.
    Task task;

    [[nodiscard]] operator QJsonValue() const;
    TaskStatusNotificationParams& operator=(const QJsonValue&);
  };

  inline const QString method_tasks_get = "tasks/get";
  inline const QString method_tasks_result = "tasks/result";
  inline const QString method_tasks_list = "tasks/list";
  inline const QString method_tasks_cancel = "tasks/cancel";
  inline const QString method_notifications_tasks_status = "notifications/tasks/status";

  /// Meta key the spec mandates on `tasks/result` payloads to bind the
  /// result back to its task. Server-side tooling sets it; clients can
  /// optionally check it.
  inline const QString tasks_related_task_meta_key = "io.modelcontextprotocol/related-task";

} // namespace mcp

// std::hash specialization so RequestId can key unordered containers.
namespace std
{
  template <> struct hash<::mcp::RequestId>
  {
    [[nodiscard]] std::size_t
    operator()(const ::mcp::RequestId& id) const noexcept
    {
      return std::hash<QString>{}(id.canonical());
    }
  };
} // namespace std
