// SPDX-License-Identifier: AGPL-3.0-only
#include "protocol.h"

#include <QMetaEnum>

#include "error.h"
#include "json.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <QHash>
#include <QList>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mcp
{

  // =====================================================================
  // RequestId
  // =====================================================================

  QString RequestId::canonical(void) const
  {
    if(is_string())
    {
      return "s:" + as_string();
    }
    return "i:" + QString::number(as_integer());
  }

  RequestId& RequestId::operator=(const QJsonValue& j)
  {
    if(j.isString())
    {
      *this = RequestId{ j.toString() };
      return *this;
    }
    if(j.isDouble())
    {
      // The spec types `id` as `string | number`; some JS clients
      // emit small whole numbers as floats (`1.0`) because JS has
      // no integer literal. Accept whole values; reject fractional.
      std::int64_t v{};
      from_json(j, v);
      *this = RequestId{ v };
      return *this;
    }
    throw Error(error_code::parse_error,
                "JSON-RPC id must be a string or integer");

    return *this;
  }

  // =====================================================================
  // JSON-RPC envelopes
  // =====================================================================

  JsonRpcRequest::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    o.insert(QStringLiteral("id"), to_json(id));
    o.insert(QStringLiteral("method"), to_json(method));
    if(!params.isNull())
      o.insert(QStringLiteral("params"), params);
    return o;
  }

  JsonRpcRequest& JsonRpcRequest::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(require<QString>(o, "jsonrpc") != QString{ kJsonrpcVersion })
    {
      throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    id = require<RequestId>(o, "id");
    method = require<QString>(o, "method");
    params = QJsonValue(QJsonValue::Null);
    take_optional_json(o, "params", params);

    return *this;
  }

  JsonRpcNotification::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    o.insert(QStringLiteral("method"), to_json(method));
    if(!params.isNull())
      o.insert(QStringLiteral("params"), params);
    return o;
  }

  JsonRpcNotification& JsonRpcNotification::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(require<QString>(o, "jsonrpc") != QString{ kJsonrpcVersion })
    {
      throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    method = require<QString>(o, "method");
    params = QJsonValue(QJsonValue::Null);
    take_optional_json(o, "params", params);

    return *this;
  }

  ErrorObject::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("code"), to_json(static_cast<int>(code)));
    o.insert(QStringLiteral("message"), to_json(message));
    if(has_data())
      o.insert(QStringLiteral("data"), data);
    return o;
  }

  ErrorObject& ErrorObject::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    code = static_cast<error_code>(require<int>(o, "code"));
    message = require<QString>(o, "message");
    const auto it = o.constFind(QStringLiteral("data"));
    data = (it != o.constEnd() && !it->isNull())
               ? *it
               : QJsonValue(QJsonValue::Null);

    return *this;
  }

  JsonRpcResponse::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    if(!id.isNull())
      o.insert(QStringLiteral("id"), to_json(id));
    else
      o.insert(QStringLiteral("id"), QJsonValue(QJsonValue::Null));
    if(is_success())
      o.insert(QStringLiteral("result"), std::get<0>(outcome));
    else
      o.insert(QStringLiteral("error"), to_json(std::get<1>(outcome)));
    return o;
  }

  JsonRpcResponse& JsonRpcResponse::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(require<QString>(o, "jsonrpc") != QString{ kJsonrpcVersion })
    {
      throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    const auto idIt = o.constFind(QStringLiteral("id"));
    if(idIt != o.constEnd() && !idIt->isNull())
    {
      id = from_json<RequestId>(*idIt);
    }
    else
    {
      id = RequestId();
    }
    const bool has_result = o.contains(QStringLiteral("result"));
    const bool has_error = o.contains(QStringLiteral("error"));
    if(has_result == has_error)
    {
      throw Error(error_code::invalid_request,
                  "JSON-RPC response must carry exactly one of "
                  "\"result\" or \"error\"");
    }
    if(has_result)
    {
      outcome = o.value(QStringLiteral("result"));
    }
    else
    {
      outcome = from_json<ErrorObject>(o.value(QStringLiteral("error")));
    }

    return *this;
  }

  JsonRpcMessage parse_message(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    const bool has_id = o.contains(QStringLiteral("id"));
    const bool has_method = o.contains(QStringLiteral("method"));
    const bool has_result = o.contains(QStringLiteral("result"));
    const bool has_error = o.contains(QStringLiteral("error"));

    if(has_method && has_id)
      return from_json<JsonRpcRequest>(j);
    if(has_method)
      return from_json<JsonRpcNotification>(j);
    if(has_result || has_error)
      return from_json<JsonRpcResponse>(j);

    throw Error(
        error_code::invalid_request,
        "JSON-RPC frame has neither \"method\" nor \"result\"/\"error\"");
  }

  QJsonValue serialize_message(const JsonRpcMessage& m)
  {
    return std::visit([](const auto& msg) { return to_json(msg); }, m);
  }

  // =====================================================================
  // Implementation
  // =====================================================================

  Icon::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("src"), to_json(src));
    put_optional_string(o, "mimeType", mime_type);
    put_optional_string(o, "sizes", sizes);
    put_optional_string(o, "theme", theme);
    return o;
  }

  Icon& Icon::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    src = require<QString>(o, "src");
    take_optional_string(o, "mimeType", mime_type);
    take_optional_string(o, "sizes", sizes);
    take_optional_string(o, "theme", theme);

    return *this;
  }

  Implementation::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    o.insert(QStringLiteral("version"), to_json(version));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional_string(o, "websiteUrl", website_url);
    put_optional(o, "icons", icons);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  Implementation& Implementation::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    version = require<QString>(o, "version");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional_string(o, "websiteUrl", website_url);
    take_optional(o, "icons", icons);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  // =====================================================================
  // Task augmentation / capability fragments
  // =====================================================================

  TaskAugmentation::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional(o, "ttl", ttl);
    return o;
  }

  TaskAugmentation& TaskAugmentation::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional(o, "ttl", ttl);

    return *this;
  }

  TasksRequestsCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "tools", tools);
    put_optional_json(o, "sampling", sampling);
    put_optional_json(o, "elicitation", elicitation);
    return o;
  }

  TasksRequestsCapability&
  TasksRequestsCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "tools", tools);
    take_optional_json(o, "sampling", sampling);
    take_optional_json(o, "elicitation", elicitation);

    return *this;
  }

  TasksCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "list", list);
    put_optional_json(o, "cancel", cancel);
    put_optional(o, "requests", requests);
    return o;
  }

  TasksCapability& TasksCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "list", list);
    take_optional_json(o, "cancel", cancel);
    take_optional(o, "requests", requests);

    return *this;
  }

  // =====================================================================
  // Capabilities
  // =====================================================================

  RootsCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional(o, "listChanged", list_changed);
    return o;
  }

  RootsCapability& RootsCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", list_changed);

    return *this;
  }

  SamplingCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "context", context);
    put_optional_json(o, "tools", tools);
    return o;
  }

  SamplingCapability& SamplingCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "context", context);
    take_optional_json(o, "tools", tools);

    return *this;
  }

  ElicitationCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "form", form);
    put_optional_json(o, "url", url);
    return o;
  }

  ElicitationCapability& ElicitationCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "form", form);
    take_optional_json(o, "url", url);

    return *this;
  }

  ClientCapabilities::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "experimental", experimental);
    put_optional(o, "roots", roots);
    put_optional(o, "sampling", sampling);
    put_optional(o, "elicitation", elicitation);
    put_optional(o, "tasks", tasks);
    return o;
  }

  ClientCapabilities& ClientCapabilities::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "experimental", experimental);
    take_optional(o, "roots", roots);
    take_optional(o, "sampling", sampling);
    take_optional(o, "elicitation", elicitation);
    take_optional(o, "tasks", tasks);

    return *this;
  }

  PromptsCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional(o, "listChanged", list_changed);
    return o;
  }

  PromptsCapability& PromptsCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", list_changed);

    return *this;
  }

  ResourcesCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional(o, "subscribe", subscribe);
    put_optional(o, "listChanged", list_changed);
    return o;
  }

  ResourcesCapability& ResourcesCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional(o, "subscribe", subscribe);
    take_optional(o, "listChanged", list_changed);

    return *this;
  }

  ToolsCapability::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional(o, "listChanged", list_changed);
    return o;
  }

  ToolsCapability& ToolsCapability::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", list_changed);

    return *this;
  }

  ServerCapabilities::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_json(o, "experimental", experimental);
    put_optional_json(o, "logging", logging);
    put_optional_json(o, "completions", completions);
    put_optional(o, "prompts", prompts);
    put_optional(o, "resources", resources);
    put_optional(o, "tools", tools);
    put_optional(o, "tasks", tasks);
    return o;
  }

  ServerCapabilities& ServerCapabilities::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_json(o, "experimental", experimental);
    take_optional_json(o, "logging", logging);
    take_optional_json(o, "completions", completions);
    take_optional(o, "prompts", prompts);
    take_optional(o, "resources", resources);
    take_optional(o, "tools", tools);
    take_optional(o, "tasks", tasks);

    return *this;
  }

  // =====================================================================
  // initialize
  // =====================================================================

  InitializeRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("protocolVersion"), to_json(protocol_version));
    o.insert(QStringLiteral("capabilities"), to_json(capabilities));
    o.insert(QStringLiteral("clientInfo"), to_json(client_info));
    return o;
  }

  InitializeRequestParams&
  InitializeRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    protocol_version = require<QString>(o, "protocolVersion");
    capabilities = require<ClientCapabilities>(o, "capabilities");
    client_info = require<Implementation>(o, "clientInfo");

    return *this;
  }

  InitializeResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("protocolVersion"), to_json(protocol_version));
    o.insert(QStringLiteral("capabilities"), to_json(capabilities));
    o.insert(QStringLiteral("serverInfo"), to_json(server_info));
    put_optional_string(o, "instructions", instructions);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  InitializeResult& InitializeResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    protocol_version = require<QString>(o, "protocolVersion");
    capabilities = require<ServerCapabilities>(o, "capabilities");
    server_info = require<Implementation>(o, "serverInfo");
    take_optional_string(o, "instructions", instructions);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  // =====================================================================
  // Content blocks
  // =====================================================================

  namespace
  {
  } // namespace

  Annotations::operator QJsonValue() const
  {
    QJsonObject o;
    if(audience.has_value())
    {
      QJsonArray arr;
      for(auto r : *audience)
        arr.append(to_json(r));
      o.insert(QStringLiteral("audience"), arr);
    }
    put_optional(o, "priority", priority);
    put_optional_string(o, "lastModified", last_modified);
    return o;
  }

  Annotations& Annotations::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(auto it = o.constFind(QStringLiteral("audience"));
       it != o.constEnd() && it->isArray())
    {
      const QJsonArray arr = it->toArray();
      QList<Role> roles;
      roles.reserve(arr.size());
      for(const auto& v : arr)
        roles.push_back(from_json<Role>(v));
      audience = std::move(roles);
    }
    take_optional(o, "priority", priority);
    take_optional_string(o, "lastModified", last_modified);

    return *this;
  }

  TextContent::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("text"));
    o.insert(QStringLiteral("text"), to_json(text));
    put_optional(o, "annotations", annotations);
    return o;
  }

  TextContent& TextContent::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    text = require<QString>(o, "text");
    take_optional(o, "annotations", annotations);

    return *this;
  }

  ImageContent::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("image"));
    o.insert(QStringLiteral("data"), to_json(data));
    o.insert(QStringLiteral("mimeType"), to_json(mime_type));
    put_optional(o, "annotations", annotations);
    return o;
  }

  ImageContent& ImageContent::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    data = require<QString>(o, "data");
    mime_type = require<QString>(o, "mimeType");
    take_optional(o, "annotations", annotations);

    return *this;
  }

  AudioContent::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("audio"));
    o.insert(QStringLiteral("data"), to_json(data));
    o.insert(QStringLiteral("mimeType"), to_json(mime_type));
    put_optional(o, "annotations", annotations);
    return o;
  }

  AudioContent& AudioContent::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    data = require<QString>(o, "data");
    mime_type = require<QString>(o, "mimeType");
    take_optional(o, "annotations", annotations);

    return *this;
  }

  // =====================================================================
  // Resources
  // =====================================================================

  TextResourceContents::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    o.insert(QStringLiteral("text"), to_json(text));
    put_optional_string(o, "mimeType", mime_type);
    return o;
  }

  TextResourceContents& TextResourceContents::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");
    text = require<QString>(o, "text");
    take_optional_string(o, "mimeType", mime_type);

    return *this;
  }

  BlobResourceContents::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    o.insert(QStringLiteral("blob"), to_json(blob));
    put_optional_string(o, "mimeType", mime_type);
    return o;
  }

  BlobResourceContents& BlobResourceContents::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");
    blob = require<QString>(o, "blob");
    take_optional_string(o, "mimeType", mime_type);

    return *this;
  }

  Resource::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional_string(o, "mimeType", mime_type);
    put_optional(o, "annotations", annotations);
    put_optional(o, "size", size);
    put_optional(o, "icons", icons);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  Resource& Resource::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional_string(o, "mimeType", mime_type);
    take_optional(o, "annotations", annotations);
    take_optional(o, "size", size);
    take_optional(o, "icons", icons);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  ResourceTemplate::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uriTemplate"), to_json(uri_template));
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional_string(o, "mimeType", mime_type);
    put_optional(o, "annotations", annotations);
    put_optional(o, "icons", icons);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ResourceTemplate& ResourceTemplate::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri_template = require<QString>(o, "uriTemplate");
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional_string(o, "mimeType", mime_type);
    take_optional(o, "annotations", annotations);
    take_optional(o, "icons", icons);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  ResourceLink::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("resource_link"));
    o.insert(QStringLiteral("uri"), to_json(uri));
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional_string(o, "mimeType", mime_type);
    put_optional(o, "annotations", annotations);
    put_optional(o, "size", size);
    put_optional(o, "icons", icons);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ResourceLink& ResourceLink::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional_string(o, "mimeType", mime_type);
    take_optional(o, "annotations", annotations);
    take_optional(o, "size", size);
    take_optional(o, "icons", icons);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  EmbeddedResource::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("resource"));
    o.insert(QStringLiteral("resource"), to_json(resource));
    put_optional(o, "annotations", annotations);
    return o;
  }

  EmbeddedResource& EmbeddedResource::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    resource = require<ResourceContents>(o, "resource");
    take_optional(o, "annotations", annotations);

    return *this;
  }

  ToolUseContent::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("tool_use"));
    o.insert(QStringLiteral("id"), to_json(id));
    o.insert(QStringLiteral("name"), to_json(name));
    o.insert(QStringLiteral("input"), input);
    put_optional(o, "annotations", annotations);
    return o;
  }

  ToolUseContent& ToolUseContent::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    id = require<QString>(o, "id");
    name = require<QString>(o, "name");
    input = require<QJsonValue>(o, "input");
    take_optional(o, "annotations", annotations);

    return *this;
  }

  // PlainContentBlock is the variant exclusion of tool_use/tool_result.
  QJsonValue to_json(const PlainContentBlock& c)
  {
    return std::visit([](const auto& v) { return to_json(v); }, c);
  }
  void from_json(const QJsonValue& j, PlainContentBlock& c)
  {
    const QJsonObject o = as_object(j);
    const auto t = require<QString>(o, "type");
    if(t == "text")
    {
      c = from_json<TextContent>(j);
      return;
    }
    if(t == "image")
    {
      c = from_json<ImageContent>(j);
      return;
    }
    if(t == "audio")
    {
      c = from_json<AudioContent>(j);
      return;
    }
    if(t == "resource_link")
    {
      c = from_json<ResourceLink>(j);
      return;
    }
    if(t == "resource")
    {
      c = from_json<EmbeddedResource>(j);
      return;
    }
    throw Error(error_code::parse_error,
                QString{ "plain content block must not be \"" + t + "\"" });
  }

  ToolResultContent::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("tool_result"));
    o.insert(QStringLiteral("toolUseId"), to_json(tool_use_id));
    o.insert(QStringLiteral("content"), to_json(content));
    put_optional(o, "isError", is_error);
    put_optional(o, "annotations", annotations);
    return o;
  }

  ToolResultContent& ToolResultContent::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    tool_use_id = require<QString>(o, "toolUseId");
    content = require<QList<PlainContentBlock>>(o, "content");
    take_optional(o, "isError", is_error);
    take_optional(o, "annotations", annotations);

    return *this;
  }

  QJsonValue to_json(const ContentBlock& c)
  {
    return std::visit([](const auto& v) { return to_json(v); }, c);
  }

  void from_json(const QJsonValue& j, ContentBlock& c)
  {
    const QJsonObject o = as_object(j);
    const auto t = require<QString>(o, "type");
    if(t == "text")
      c = from_json<TextContent>(j);
    else if(t == "image")
      c = from_json<ImageContent>(j);
    else if(t == "audio")
      c = from_json<AudioContent>(j);
    else if(t == "resource_link")
      c = from_json<ResourceLink>(j);
    else if(t == "resource")
      c = from_json<EmbeddedResource>(j);
    else if(t == "tool_use")
      c = from_json<ToolUseContent>(j);
    else if(t == "tool_result")
      c = from_json<ToolResultContent>(j);
    else
      throw Error(error_code::parse_error,
                  QString{ "unknown content block type: " } + t);
  }

  // =====================================================================
  // Tools
  // =====================================================================

  ToolAnnotations::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "title", title);
    put_optional(o, "readOnlyHint", read_only_hint);
    put_optional(o, "destructiveHint", destructive_hint);
    put_optional(o, "idempotentHint", idempotent_hint);
    put_optional(o, "openWorldHint", open_world_hint);
    return o;
  }

  ToolAnnotations& ToolAnnotations::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "title", title);
    take_optional(o, "readOnlyHint", read_only_hint);
    take_optional(o, "destructiveHint", destructive_hint);
    take_optional(o, "idempotentHint", idempotent_hint);
    take_optional(o, "openWorldHint", open_world_hint);

    return *this;
  }

  ToolExecution::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("taskSupport"), to_json(task_support));
    return o;
  }

  ToolExecution& ToolExecution::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(auto it = o.constFind(QStringLiteral("taskSupport"));
       it != o.constEnd())
    {
      task_support = from_json<TaskSupport>(*it);
    }
    else
    {
      task_support = TaskSupport::optional;
    }

    return *this;
  }

  Tool::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    o.insert(QStringLiteral("inputSchema"), input_schema);
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional_json(o, "outputSchema", output_schema);
    put_optional(o, "annotations", annotations);
    put_optional(o, "icons", icons);
    put_optional(o, "execution", execution);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  Tool& Tool::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    input_schema = require<QJsonValue>(o, "inputSchema");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional_json(o, "outputSchema", output_schema);
    take_optional(o, "annotations", annotations);
    take_optional(o, "icons", icons);
    take_optional(o, "execution", execution);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  ListToolsRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "cursor", cursor);
    return o;
  }

  ListToolsRequestParams&
  ListToolsRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", cursor);

    return *this;
  }

  ListToolsResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("tools"), to_json(tools));
    put_optional_string(o, "nextCursor", next_cursor);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ListToolsResult& ListToolsResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    tools = require<QList<Tool>>(o, "tools");
    take_optional_string(o, "nextCursor", next_cursor);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  CallToolRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_json(o, "arguments", arguments);
    put_optional(o, "task", task);
    return o;
  }

  CallToolRequestParams& CallToolRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    take_optional_json(o, "arguments", arguments);
    take_optional(o, "task", task);

    return *this;
  }

  CallToolResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("content"), to_json(content));
    put_optional_json(o, "structuredContent", structured_content);
    put_optional(o, "isError", is_error);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  CallToolResult& CallToolResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    content = require<QList<ContentBlock>>(o, "content");
    take_optional_json(o, "structuredContent", structured_content);
    take_optional(o, "isError", is_error);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  // =====================================================================
  // Resource requests / responses
  // =====================================================================

  ListResourcesRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "cursor", cursor);
    return o;
  }

  ListResourcesRequestParams&
  ListResourcesRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", cursor);

    return *this;
  }

  ListResourcesResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("resources"), to_json(resources));
    put_optional_string(o, "nextCursor", next_cursor);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ListResourcesResult& ListResourcesResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    resources = require<QList<Resource>>(o, "resources");
    take_optional_string(o, "nextCursor", next_cursor);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  ListResourceTemplatesRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "cursor", cursor);
    return o;
  }

  ListResourceTemplatesRequestParams&
  ListResourceTemplatesRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", cursor);

    return *this;
  }

  ListResourceTemplatesResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("resourceTemplates"), to_json(resource_templates));
    put_optional_string(o, "nextCursor", next_cursor);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ListResourceTemplatesResult&
  ListResourceTemplatesResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    resource_templates
        = require<QList<ResourceTemplate>>(o, "resourceTemplates");
    take_optional_string(o, "nextCursor", next_cursor);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  ReadResourceRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    return o;
  }

  ReadResourceRequestParams&
  ReadResourceRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");

    return *this;
  }

  ReadResourceResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("contents"), to_json(contents));
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ReadResourceResult& ReadResourceResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    contents = require<QList<ResourceContents>>(o, "contents");
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  SubscribeRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    return o;
  }

  SubscribeRequestParams&
  SubscribeRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");

    return *this;
  }

  UnsubscribeRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    return o;
  }

  UnsubscribeRequestParams&
  UnsubscribeRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");

    return *this;
  }

  ResourceUpdatedNotificationParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    return o;
  }

  ResourceUpdatedNotificationParams&
  ResourceUpdatedNotificationParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");

    return *this;
  }

  // =====================================================================
  // Prompts
  // =====================================================================

  PromptArgument::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    put_optional(o, "required", required);
    return o;
  }

  PromptArgument& PromptArgument::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    take_optional(o, "required", required);

    return *this;
  }

  Prompt::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    put_optional_string(o, "description", description);
    if(arguments.has_value())
      o.insert(QStringLiteral("arguments"), to_json(arguments));
    put_optional(o, "icons", icons);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  Prompt& Prompt::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);
    take_optional_string(o, "description", description);
    if(auto it = o.constFind(QStringLiteral("arguments"));
       it != o.constEnd() && !it->isNull())
    {
      arguments = from_json<QList<PromptArgument>>(*it);
    }
    take_optional(o, "icons", icons);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  PromptMessage::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(role));
    o.insert(QStringLiteral("content"), to_json(content));
    return o;
  }

  PromptMessage& PromptMessage::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    role = require<Role>(o, "role");
    content = require<ContentBlock>(o, "content");

    return *this;
  }

  ListPromptsRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "cursor", cursor);
    return o;
  }

  ListPromptsRequestParams&
  ListPromptsRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", cursor);

    return *this;
  }

  ListPromptsResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("prompts"), to_json(prompts));
    put_optional_string(o, "nextCursor", next_cursor);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  ListPromptsResult& ListPromptsResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    prompts = require<QList<Prompt>>(o, "prompts");
    take_optional_string(o, "nextCursor", next_cursor);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  GetPromptRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    if(arguments.has_value())
    {
      QJsonObject args;
      for(auto it = arguments->constBegin(); it != arguments->constEnd(); ++it)
      {
        args.insert(it.key(), to_json(it.value()));
      }
      o.insert(QStringLiteral("arguments"), args);
    }
    return o;
  }

  GetPromptRequestParams&
  GetPromptRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    if(auto it = o.constFind(QStringLiteral("arguments"));
       it != o.constEnd() && it->isObject())
    {
      const QJsonObject src = it->toObject();
      QHash<QString, QString> args;
      for(auto i = src.begin(); i != src.end(); ++i)
      {
        if(i.value().isString())
        {
          args.emplace(i.key(), i.value().toString());
        }
      }
      arguments = std::move(args);
    }

    return *this;
  }

  GetPromptResult::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "description", description);
    o.insert(QStringLiteral("messages"), to_json(messages));
    put_optional_json(o, "_meta", meta);
    return o;
  }

  GetPromptResult& GetPromptResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "description", description);
    messages = require<QList<PromptMessage>>(o, "messages");
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  // =====================================================================
  // Cancellation
  // =====================================================================

  CancelledNotificationParams::operator QJsonValue() const
  {
    QJsonObject o;
    if(!request_id.isNull())
      o.insert(QStringLiteral("requestId"), to_json(request_id));
    put_optional_string(o, "reason", reason);
    return o;
  }

  CancelledNotificationParams&
  CancelledNotificationParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(auto it = o.constFind(QStringLiteral("requestId"));
       it != o.constEnd() && !it->isNull())
    {
      request_id = from_json<RequestId>(*it);
    }
    else
    {
      request_id = RequestId();
    }
    take_optional_string(o, "reason", reason);

    return *this;
  }

  // =====================================================================
  // Progress
  // =====================================================================

  QString ProgressToken::canonical(void) const
  {
    return is_string() ? "s:" + as_string()
                       : "i:" + QString::number(as_integer());
  }

  ProgressToken& ProgressToken::operator=(const QJsonValue& j)
  {
    if(j.isString())
    {
      *this = ProgressToken{ j.toString() };
      return *this;
    }
    if(j.isDouble())
    {
      // Same rationale as RequestId — accept whole-number floats.
      std::int64_t v{};
      from_json(j, v);
      *this = ProgressToken{ v };
      return *this;
    }
    throw Error(error_code::parse_error,
                "progressToken must be a string or integer");

    return *this;
  }

  ProgressNotificationParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("progressToken"), to_json(progress_token));
    o.insert(QStringLiteral("progress"), to_json(progress));
    put_optional(o, "total", total);
    put_optional_string(o, "message", message);
    return o;
  }

  ProgressNotificationParams&
  ProgressNotificationParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    progress_token = require<ProgressToken>(o, "progressToken");
    progress = require<double>(o, "progress");
    take_optional(o, "total", total);
    take_optional_string(o, "message", message);

    return *this;
  }

  // =====================================================================
  // Logging (protocol-level)
  // =====================================================================

  namespace
  {
  } // namespace

  SetLevelRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("level"), to_json(level));
    return o;
  }

  SetLevelRequestParams& SetLevelRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    level = require<LoggingLevel>(o, "level");

    return *this;
  }

  LoggingMessageNotificationParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("level"), to_json(level));
    o.insert(QStringLiteral("data"), data);
    put_optional_string(o, "logger", logger);
    return o;
  }

  LoggingMessageNotificationParams&
  LoggingMessageNotificationParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    level = require<LoggingLevel>(o, "level");
    data = require<QJsonValue>(o, "data");
    take_optional_string(o, "logger", logger);

    return *this;
  }

  // =====================================================================
  // Sampling
  // =====================================================================

  ModelHint::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "name", name);
    return o;
  }

  ModelHint& ModelHint::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "name", name);

    return *this;
  }

  ModelPreferences::operator QJsonValue() const
  {
    QJsonObject o;
    if(hints.has_value())
      o.insert(QStringLiteral("hints"), to_json(*hints));
    put_optional(o, "costPriority", cost_priority);
    put_optional(o, "speedPriority", speed_priority);
    put_optional(o, "intelligencePriority", intelligence_priority);
    return o;
  }

  ModelPreferences& ModelPreferences::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    if(auto it = o.constFind(QStringLiteral("hints"));
       it != o.constEnd() && it->isArray())
    {
      hints = from_json<QList<ModelHint>>(*it);
    }
    take_optional(o, "costPriority", cost_priority);
    take_optional(o, "speedPriority", speed_priority);
    take_optional(o, "intelligencePriority", intelligence_priority);

    return *this;
  }

  // Tagged-by-shape: when `mode == tool` we MUST emit the object form
  // `{ type: "tool", name: "..." }`. Other modes serialise as a bare
  // string on the wire. Symmetrically, from_json accepts either form.

  QString to_string(ToolChoiceMode m) noexcept
  {
    if(m == ToolChoiceMode::m_auto)
    {
      return QStringLiteral("auto");
    }
    return QMetaEnum::fromType<ToolChoiceMode>().valueToKey(
        static_cast<int>(m));
  }

  QJsonValue to_json(ToolChoiceMode m) { return QJsonValue(to_string(m)); }

  void from_json(const QJsonValue& j, ToolChoiceMode& m)
  {
    const QString s = from_json<QString>(j);
    if(s == QStringLiteral("auto"))
    {
      m = ToolChoiceMode::m_auto;
      return;
    }
    bool ok = false;
    const int v = QMetaEnum::fromType<ToolChoiceMode>().keyToValue(
        s.toUtf8().constData(), &ok);
    if(!ok)
    {
      throw Error(error_code::parse_error,
                  QStringLiteral("unknown toolChoice mode: ") + s);
    }
    m = static_cast<ToolChoiceMode>(v);
  }

  ToolChoice::operator QJsonValue() const
  {
    if(mode.has_value() && *mode == ToolChoiceMode::tool)
    {
      QJsonObject o;
      o.insert(QStringLiteral("type"), to_json("tool"));
      if(!name.isNull())
        o.insert(QStringLiteral("name"), to_json(name));
      return o;
    }
    if(mode.has_value())
    {
      return to_json(*mode);
    }
    return QJsonObject{};
  }

  ToolChoice& ToolChoice::operator=(const QJsonValue& j)
  {
    if(j.isString())
    {
      mode = from_json<ToolChoiceMode>(j);
      return *this;
    }
    if(j.isObject())
    {
      const QJsonObject o = j.toObject();
      if(auto it = o.constFind(QStringLiteral("type"));
         it != o.constEnd() && it->isString())
      {
        const auto t = it->toString();
        if(t == "tool")
        {
          mode = ToolChoiceMode::tool;
          take_optional_string(o, "name", name);
          return *this;
        }
        // Unknown object shape — fall through to the legacy
        // `{ mode: ... }` form for forward compat.
      }
      take_optional(o, "mode", mode);
      take_optional_string(o, "name", name);
      return *this;
    }
    throw Error(error_code::parse_error,
                "toolChoice must be a string or an object");

    return *this;
  }

  SamplingMessage::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(role));
    if(content.size() == 1)
    {
      o.insert(QStringLiteral("content"), to_json(content.front()));
    }
    else
    {
      o.insert(QStringLiteral("content"), to_json(content));
    }
    return o;
  }

  SamplingMessage& SamplingMessage::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    role = require<Role>(o, "role");
    const QJsonValue c = o.value(QStringLiteral("content"));
    if(c.isArray())
    {
      content = from_json<QList<ContentBlock>>(c);
    }
    else
    {
      content = { from_json<ContentBlock>(c) };
    }

    return *this;
  }

  CreateMessageRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("messages"), to_json(messages));
    o.insert(QStringLiteral("maxTokens"), to_json(max_tokens));
    put_optional(o, "modelPreferences", model_preferences);
    put_optional_string(o, "systemPrompt", system_prompt);
    put_optional(o, "includeContext", include_context);
    put_optional(o, "temperature", temperature);
    if(stop_sequences.has_value())
      o.insert(QStringLiteral("stopSequences"), to_json(*stop_sequences));
    put_optional_json(o, "metadata", metadata);
    put_optional(o, "tools", tools);
    put_optional(o, "toolChoice", tool_choice);
    return o;
  }

  CreateMessageRequestParams&
  CreateMessageRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    messages = require<QList<SamplingMessage>>(o, "messages");

    // maxTokens: reject non-integer / negative / fractional values.
    const QJsonValue mt = o.value(QStringLiteral("maxTokens"));
    if(!mt.isDouble())
    {
      throw Error(error_code::invalid_params, "maxTokens must be an integer");
    }
    const double d = mt.toDouble();
    if(d < 0)
    {
      throw Error(error_code::invalid_params,
                  "maxTokens must be non-negative");
    }
    const qint64 iv = mt.toInteger();
    if(static_cast<double>(iv) != d)
    {
      throw Error(error_code::invalid_params, "maxTokens must be an integer");
    }
    max_tokens = static_cast<std::int64_t>(iv);

    take_optional(o, "modelPreferences", model_preferences);
    take_optional_string(o, "systemPrompt", system_prompt);
    take_optional(o, "includeContext", include_context);
    take_optional(o, "temperature", temperature);
    if(auto it = o.constFind(QStringLiteral("stopSequences"));
       it != o.constEnd() && it->isArray())
    {
      stop_sequences = from_json<QList<QString>>(*it);
    }
    take_optional_json(o, "metadata", metadata);
    take_optional(o, "tools", tools);
    take_optional(o, "toolChoice", tool_choice);

    return *this;
  }

  CreateMessageResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(role));
    if(content.size() == 1)
    {
      o.insert(QStringLiteral("content"), to_json(content.front()));
    }
    else
    {
      o.insert(QStringLiteral("content"), to_json(content));
    }
    o.insert(QStringLiteral("model"), to_json(model));
    put_optional_string(o, "stopReason", stop_reason);
    put_optional_json(o, "_meta", meta);
    return o;
  }

  CreateMessageResult& CreateMessageResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    role = require<Role>(o, "role");
    const QJsonValue c = o.value(QStringLiteral("content"));
    if(c.isArray())
    {
      content = from_json<QList<ContentBlock>>(c);
    }
    else
    {
      content = { from_json<ContentBlock>(c) };
    }
    model = require<QString>(o, "model");
    take_optional_string(o, "stopReason", stop_reason);
    take_optional_json(o, "_meta", meta);

    return *this;
  }

  // =====================================================================
  // Roots
  // =====================================================================

  Root::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(uri));
    put_optional_string(o, "name", name);
    return o;
  }

  Root& Root::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri = require<QString>(o, "uri");
    take_optional_string(o, "name", name);

    return *this;
  }

  ListRootsResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("roots"), to_json(roots));
    return o;
  }

  ListRootsResult& ListRootsResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    roots = require<QList<Root>>(o, "roots");

    return *this;
  }

  // =====================================================================
  // Completion
  // =====================================================================

  ResourceTemplateReference::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("ref/resource"));
    o.insert(QStringLiteral("uri"), to_json(uri_template));
    return o;
  }

  ResourceTemplateReference&
  ResourceTemplateReference::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    uri_template = require<QString>(o, "uri");

    return *this;
  }

  PromptReference::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("ref/prompt"));
    o.insert(QStringLiteral("name"), to_json(name));
    put_optional_string(o, "title", title);
    return o;
  }

  PromptReference& PromptReference::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    take_optional_string(o, "title", title);

    return *this;
  }

  CompleteArgument::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(name));
    o.insert(QStringLiteral("value"), to_json(value));
    return o;
  }

  CompleteArgument& CompleteArgument::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    name = require<QString>(o, "name");
    value = require<QString>(o, "value");

    return *this;
  }

  CompleteRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("ref"), to_json(reference));
    o.insert(QStringLiteral("argument"), to_json(argument));
    if(context_arguments.has_value())
    {
      QJsonObject ctx;
      for(auto it = context_arguments->constBegin();
          it != context_arguments->constEnd(); ++it)
      {
        ctx.insert(it.key(), to_json(it.value()));
      }
      QJsonObject wrapper;
      wrapper.insert(QStringLiteral("arguments"), ctx);
      o.insert(QStringLiteral("context"), wrapper);
    }
    return o;
  }

  CompleteRequestParams& CompleteRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    reference = require<CompletionReference>(o, "ref");
    argument = require<CompleteArgument>(o, "argument");
    if(auto it = o.constFind(QStringLiteral("context"));
       it != o.constEnd() && it->isObject())
    {
      const QJsonObject ctx = it->toObject();
      if(auto a = ctx.constFind(QStringLiteral("arguments"));
         a != ctx.constEnd() && a->isObject())
      {
        const QJsonObject src = a->toObject();
        QHash<QString, QString> args;
        for(auto i = src.begin(); i != src.end(); ++i)
        {
          if(i.value().isString())
          {
            args.emplace(i.key(), i.value().toString());
          }
        }
        context_arguments = std::move(args);
      }
    }

    return *this;
  }

  CompletionValues::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("values"), to_json(values));
    put_optional(o, "total", total);
    put_optional(o, "hasMore", has_more);
    return o;
  }

  CompletionValues& CompletionValues::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    values = require<QList<QString>>(o, "values");
    take_optional(o, "total", total);
    take_optional(o, "hasMore", has_more);

    return *this;
  }

  CompleteResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("completion"), to_json(completion));
    return o;
  }

  CompleteResult& CompleteResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    completion = require<CompletionValues>(o, "completion");

    return *this;
  }

  // =====================================================================
  // Elicitation
  // =====================================================================

  ElicitFormRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    // mode is optional and defaults to "form"; we omit it to match the
    // canonical wire form most servers and validators emit.
    o.insert(QStringLiteral("message"), to_json(message));
    o.insert(QStringLiteral("requestedSchema"), requested_schema);
    return o;
  }

  ElicitFormRequestParams&
  ElicitFormRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    message = require<QString>(o, "message");
    requested_schema = require<QJsonValue>(o, "requestedSchema");
    // Per spec, requestedSchema is a JSON-Schema object describing
    // the expected response shape. Reject non-object values up
    // front rather than letting them propagate to a confused
    // handler.
    if(!requested_schema.isObject())
    {
      throw Error(
          error_code::invalid_params,
          "elicitation/create: `requestedSchema` must be a JSON object");
    }

    return *this;
  }

  ElicitUrlRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("mode"), to_json("url"));
    o.insert(QStringLiteral("message"), to_json(message));
    o.insert(QStringLiteral("url"), to_json(url));
    o.insert(QStringLiteral("elicitationId"), to_json(elicitation_id));
    return o;
  }

  ElicitUrlRequestParams&
  ElicitUrlRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    message = require<QString>(o, "message");
    url = require<QString>(o, "url");
    elicitation_id = require<QString>(o, "elicitationId");

    return *this;
  }

  ElicitResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("action"), to_json(action));
    put_optional_json(o, "content", content);
    return o;
  }

  ElicitResult& ElicitResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    action = require<ElicitAction>(o, "action");
    take_optional_json(o, "content", content);

    return *this;
  }

  ElicitationCompleteNotificationParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("elicitationId"), to_json(elicitation_id));
    return o;
  }

  ElicitationCompleteNotificationParams&
  ElicitationCompleteNotificationParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    elicitation_id = require<QString>(o, "elicitationId");

    return *this;
  }

  UrlElicitationRequiredErrorData::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("elicitations"), to_json(elicitations));
    return o;
  }

  UrlElicitationRequiredErrorData&
  UrlElicitationRequiredErrorData::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    elicitations = require<QList<ElicitUrlRequestParams>>(o, "elicitations");

    return *this;
  }

  // =====================================================================
  // Tasks
  // =====================================================================

  Task::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(taskId));
    o.insert(QStringLiteral("status"), to_json(status));
    o.insert(QStringLiteral("createdAt"), to_json(created_at));
    o.insert(QStringLiteral("lastUpdatedAt"), to_json(last_updated_at));
    put_optional_string(o, "statusMessage", status_message);
    put_optional(o, "ttl", ttl);
    put_optional(o, "pollInterval", poll_interval);
    return o;
  }

  Task& Task::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    taskId = require<QString>(o, "taskId");
    status = require<TaskStatus>(o, "status");
    created_at = require<QString>(o, "createdAt");
    last_updated_at = require<QString>(o, "lastUpdatedAt");
    take_optional_string(o, "statusMessage", status_message);
    take_optional(o, "ttl", ttl);
    take_optional(o, "pollInterval", poll_interval);

    return *this;
  }

  CreateTaskResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("task"), to_json(task));
    return o;
  }

  CreateTaskResult& CreateTaskResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    task = require<Task>(o, "task");

    return *this;
  }

  GetTaskRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(task_id));
    return o;
  }

  GetTaskRequestParams& GetTaskRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    task_id = require<QString>(o, "taskId");

    return *this;
  }

  GetTaskResultRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(task_id));
    return o;
  }

  GetTaskResultRequestParams&
  GetTaskResultRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    task_id = require<QString>(o, "taskId");

    return *this;
  }

  ListTasksRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    put_optional_string(o, "cursor", cursor);
    return o;
  }

  ListTasksRequestParams&
  ListTasksRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", cursor);

    return *this;
  }

  ListTasksResult::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("tasks"), to_json(tasks));
    put_optional_string(o, "nextCursor", next_cursor);
    return o;
  }

  ListTasksResult& ListTasksResult::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    tasks = require<QList<Task>>(o, "tasks");
    take_optional_string(o, "nextCursor", next_cursor);

    return *this;
  }

  CancelTaskRequestParams::operator QJsonValue() const
  {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(task_id));
    return o;
  }

  CancelTaskRequestParams&
  CancelTaskRequestParams::operator=(const QJsonValue& j)
  {
    const QJsonObject o = as_object(j);
    task_id = require<QString>(o, "taskId");

    return *this;
  }

  TaskStatusNotificationParams::operator QJsonValue() const
  {
    // Notifications carry the same Task projection inline (no
    // wrapping object), per spec.
    return to_json(task);
  }

  TaskStatusNotificationParams&
  TaskStatusNotificationParams::operator=(const QJsonValue& j)
  {
    task = from_json<Task>(j);

    return *this;
  }

} // namespace mcp
