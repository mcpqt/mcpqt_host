// SPDX-License-Identifier: AGPL-3.0-only
#include "protocol.h"

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

namespace mcp {

// =====================================================================
// RequestId
// =====================================================================

QString RequestId::canonical(void) const {
    if (is_string()) {
        return "s:" + as_string();
    }
    return "i:" + QString::number(as_integer());
}

QJsonValue to_json(const RequestId& id) {
    return std::visit([](const auto& v) { return to_json(v); }, id.value());
}

void from_json(const QJsonValue& j, RequestId& id) {
    if (j.isString()) {
        id = RequestId{j.toString()};
        return;
    }
    if (j.isDouble()) {
        // The spec types `id` as `string | number`; some JS clients
        // emit small whole numbers as floats (`1.0`) because JS has
        // no integer literal. Accept whole values; reject fractional.
        std::int64_t v{};
        from_json(j, v);
        id = RequestId{v};
        return;
    }
    throw Error(error_code::parse_error,
                "JSON-RPC id must be a string or integer");
}

// =====================================================================
// JSON-RPC envelopes
// =====================================================================

QJsonValue to_json(const JsonRpcRequest& r) {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    o.insert(QStringLiteral("id"), to_json(r.id));
    o.insert(QStringLiteral("method"), to_json(r.method));
    if (r.params.has_value())
        o.insert(QStringLiteral("params"), *r.params);
    return o;
}

void from_json(const QJsonValue& j, JsonRpcRequest& r) {
    const QJsonObject o = as_object(j);
    if (require<QString>(o, "jsonrpc") != QString{kJsonrpcVersion}) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    r.id = require<RequestId>(o, "id");
    r.method = require<QString>(o, "method");
    r.params.reset();
    take_optional<Json>(o, "params", r.params);
}

QJsonValue to_json(const JsonRpcNotification& n) {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    o.insert(QStringLiteral("method"), to_json(n.method));
    if (n.params.has_value())
        o.insert(QStringLiteral("params"), *n.params);
    return o;
}

void from_json(const QJsonValue& j, JsonRpcNotification& n) {
    const QJsonObject o = as_object(j);
    if (require<QString>(o, "jsonrpc") != QString{kJsonrpcVersion}) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    n.method = require<QString>(o, "method");
    n.params.reset();
    take_optional<Json>(o, "params", n.params);
}

QJsonValue to_json(const ErrorObject& e) {
    QJsonObject o;
    o.insert(QStringLiteral("code"), to_json(e.code));
    o.insert(QStringLiteral("message"), to_json(e.message));
    if (e.has_data())
        o.insert(QStringLiteral("data"), e.data);
    return o;
}

void from_json(const QJsonValue& j, ErrorObject& e) {
    const QJsonObject o = as_object(j);
    e.code = require<int>(o, "code");
    e.message = require<QString>(o, "message");
    const auto it = o.constFind(QStringLiteral("data"));
    e.data = (it != o.constEnd() && !it->isNull())
                 ? *it
                 : QJsonValue(QJsonValue::Null);
}

QJsonValue to_json(const JsonRpcResponse& r) {
    QJsonObject o;
    o.insert(QStringLiteral("jsonrpc"), to_json(kJsonrpcVersion));
    if (r.id.has_value())
        o.insert(QStringLiteral("id"), to_json(*r.id));
    else
        o.insert(QStringLiteral("id"), QJsonValue(QJsonValue::Null));
    if (r.is_success())
        o.insert(QStringLiteral("result"), std::get<0>(r.outcome));
    else
        o.insert(QStringLiteral("error"), to_json(std::get<1>(r.outcome)));
    return o;
}

void from_json(const QJsonValue& j, JsonRpcResponse& r) {
    const QJsonObject o = as_object(j);
    if (require<QString>(o, "jsonrpc") != QString{kJsonrpcVersion}) {
        throw Error(error_code::invalid_request, "jsonrpc must be \"2.0\"");
    }
    const auto idIt = o.constFind(QStringLiteral("id"));
    if (idIt != o.constEnd() && !idIt->isNull()) {
        r.id = from_json<RequestId>(*idIt);
    } else {
        r.id.reset();
    }
    const bool has_result = o.contains(QStringLiteral("result"));
    const bool has_error = o.contains(QStringLiteral("error"));
    if (has_result == has_error) {
        throw Error(error_code::invalid_request,
                    "JSON-RPC response must carry exactly one of "
                    "\"result\" or \"error\"");
    }
    if (has_result) {
        r.outcome = o.value(QStringLiteral("result"));
    } else {
        r.outcome = from_json<ErrorObject>(o.value(QStringLiteral("error")));
    }
}

JsonRpcMessage parse_message(const Json& j) {
    const QJsonObject o = as_object(j);
    const bool has_id = o.contains(QStringLiteral("id"));
    const bool has_method = o.contains(QStringLiteral("method"));
    const bool has_result = o.contains(QStringLiteral("result"));
    const bool has_error = o.contains(QStringLiteral("error"));

    if (has_method && has_id)
        return from_json<JsonRpcRequest>(j);
    if (has_method)
        return from_json<JsonRpcNotification>(j);
    if (has_result || has_error)
        return from_json<JsonRpcResponse>(j);

    throw Error(
        error_code::invalid_request,
        "JSON-RPC frame has neither \"method\" nor \"result\"/\"error\"");
}

Json serialize_message(const JsonRpcMessage& m) {
    return std::visit([](const auto& msg) { return to_json(msg); }, m);
}

// =====================================================================
// Implementation
// =====================================================================

QJsonValue to_json(const Icon& c) {
    QJsonObject o;
    o.insert(QStringLiteral("src"), to_json(c.src));
    put_optional_string(o, "mimeType", c.mime_type);
    put_optional_string(o, "sizes", c.sizes);
    put_optional_string(o, "theme", c.theme);
    return o;
}
void from_json(const QJsonValue& j, Icon& c) {
    const QJsonObject o = as_object(j);
    c.src = require<QString>(o, "src");
    take_optional_string(o, "mimeType", c.mime_type);
    take_optional_string(o, "sizes", c.sizes);
    take_optional_string(o, "theme", c.theme);
}

QJsonValue to_json(const Implementation& i) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(i.name));
    o.insert(QStringLiteral("version"), to_json(i.version));
    put_optional_string(o, "title", i.title);
    put_optional_string(o, "description", i.description);
    put_optional_string(o, "websiteUrl", i.website_url);
    put_optional(o, "icons", i.icons);
    put_optional<Json>(o, "_meta", i.meta);
    return o;
}

void from_json(const QJsonValue& j, Implementation& i) {
    const QJsonObject o = as_object(j);
    i.name = require<QString>(o, "name");
    i.version = require<QString>(o, "version");
    take_optional_string(o, "title", i.title);
    take_optional_string(o, "description", i.description);
    take_optional_string(o, "websiteUrl", i.website_url);
    take_optional(o, "icons", i.icons);
    take_optional<Json>(o, "_meta", i.meta);
}

// =====================================================================
// Task augmentation / capability fragments
// =====================================================================

QJsonValue to_json(const TaskAugmentation& t) {
    QJsonObject o;
    put_optional(o, "ttl", t.ttl);
    return o;
}
void from_json(const QJsonValue& j, TaskAugmentation& t) {
    const QJsonObject o = as_object(j);
    take_optional(o, "ttl", t.ttl);
}

QJsonValue to_json(const TasksRequestsCapability& c) {
    QJsonObject o;
    put_optional<Json>(o, "tools", c.tools);
    put_optional<Json>(o, "sampling", c.sampling);
    put_optional<Json>(o, "elicitation", c.elicitation);
    return o;
}
void from_json(const QJsonValue& j, TasksRequestsCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "tools", c.tools);
    take_optional<Json>(o, "sampling", c.sampling);
    take_optional<Json>(o, "elicitation", c.elicitation);
}

QJsonValue to_json(const TasksCapability& c) {
    QJsonObject o;
    put_optional<Json>(o, "list", c.list);
    put_optional<Json>(o, "cancel", c.cancel);
    put_optional(o, "requests", c.requests);
    return o;
}
void from_json(const QJsonValue& j, TasksCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "list", c.list);
    take_optional<Json>(o, "cancel", c.cancel);
    take_optional(o, "requests", c.requests);
}

// =====================================================================
// Capabilities
// =====================================================================

QJsonValue to_json(const RootsCapability& c) {
    QJsonObject o;
    put_optional(o, "listChanged", c.list_changed);
    return o;
}
void from_json(const QJsonValue& j, RootsCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", c.list_changed);
}

QJsonValue to_json(const SamplingCapability& c) {
    QJsonObject o;
    put_optional<Json>(o, "context", c.context);
    put_optional<Json>(o, "tools", c.tools);
    return o;
}
void from_json(const QJsonValue& j, SamplingCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "context", c.context);
    take_optional<Json>(o, "tools", c.tools);
}

QJsonValue to_json(const ElicitationCapability& c) {
    QJsonObject o;
    put_optional<Json>(o, "form", c.form);
    put_optional<Json>(o, "url", c.url);
    return o;
}
void from_json(const QJsonValue& j, ElicitationCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "form", c.form);
    take_optional<Json>(o, "url", c.url);
}

QJsonValue to_json(const ClientCapabilities& c) {
    QJsonObject o;
    put_optional<Json>(o, "experimental", c.experimental);
    put_optional(o, "roots", c.roots);
    put_optional(o, "sampling", c.sampling);
    put_optional(o, "elicitation", c.elicitation);
    put_optional(o, "tasks", c.tasks);
    return o;
}
void from_json(const QJsonValue& j, ClientCapabilities& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "experimental", c.experimental);
    take_optional(o, "roots", c.roots);
    take_optional(o, "sampling", c.sampling);
    take_optional(o, "elicitation", c.elicitation);
    take_optional(o, "tasks", c.tasks);
}

QJsonValue to_json(const PromptsCapability& c) {
    QJsonObject o;
    put_optional(o, "listChanged", c.list_changed);
    return o;
}
void from_json(const QJsonValue& j, PromptsCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", c.list_changed);
}

QJsonValue to_json(const ResourcesCapability& c) {
    QJsonObject o;
    put_optional(o, "subscribe", c.subscribe);
    put_optional(o, "listChanged", c.list_changed);
    return o;
}
void from_json(const QJsonValue& j, ResourcesCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional(o, "subscribe", c.subscribe);
    take_optional(o, "listChanged", c.list_changed);
}

QJsonValue to_json(const ToolsCapability& c) {
    QJsonObject o;
    put_optional(o, "listChanged", c.list_changed);
    return o;
}
void from_json(const QJsonValue& j, ToolsCapability& c) {
    const QJsonObject o = as_object(j);
    take_optional(o, "listChanged", c.list_changed);
}

QJsonValue to_json(const ServerCapabilities& c) {
    QJsonObject o;
    put_optional<Json>(o, "experimental", c.experimental);
    put_optional<Json>(o, "logging", c.logging);
    put_optional<Json>(o, "completions", c.completions);
    put_optional(o, "prompts", c.prompts);
    put_optional(o, "resources", c.resources);
    put_optional(o, "tools", c.tools);
    put_optional(o, "tasks", c.tasks);
    return o;
}
void from_json(const QJsonValue& j, ServerCapabilities& c) {
    const QJsonObject o = as_object(j);
    take_optional<Json>(o, "experimental", c.experimental);
    take_optional<Json>(o, "logging", c.logging);
    take_optional<Json>(o, "completions", c.completions);
    take_optional(o, "prompts", c.prompts);
    take_optional(o, "resources", c.resources);
    take_optional(o, "tools", c.tools);
    take_optional(o, "tasks", c.tasks);
}

// =====================================================================
// initialize
// =====================================================================

QJsonValue to_json(const InitializeRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("protocolVersion"), to_json(p.protocol_version));
    o.insert(QStringLiteral("capabilities"), to_json(p.capabilities));
    o.insert(QStringLiteral("clientInfo"), to_json(p.client_info));
    return o;
}

void from_json(const QJsonValue& j, InitializeRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.protocol_version = require<QString>(o, "protocolVersion");
    p.capabilities = require<ClientCapabilities>(o, "capabilities");
    p.client_info = require<Implementation>(o, "clientInfo");
}

QJsonValue to_json(const InitializeResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("protocolVersion"), to_json(r.protocol_version));
    o.insert(QStringLiteral("capabilities"), to_json(r.capabilities));
    o.insert(QStringLiteral("serverInfo"), to_json(r.server_info));
    put_optional_string(o, "instructions", r.instructions);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, InitializeResult& r) {
    const QJsonObject o = as_object(j);
    r.protocol_version = require<QString>(o, "protocolVersion");
    r.capabilities = require<ServerCapabilities>(o, "capabilities");
    r.server_info = require<Implementation>(o, "serverInfo");
    take_optional_string(o, "instructions", r.instructions);
    take_optional<Json>(o, "_meta", r.meta);
}

// =====================================================================
// Content blocks
// =====================================================================

QString to_string(Role r) noexcept {
    switch (r) {
    case Role::user:
        return "user";
    case Role::assistant:
        return "assistant";
    }
    return "?";
}

namespace {

Role role_from_string(QString s) {
    if (s == "user")
        return Role::user;
    if (s == "assistant")
        return Role::assistant;
    throw Error(error_code::parse_error,
                QString{"unknown role: "} + QString{s});
}

}  // namespace

QJsonValue to_json(Role r) {
    return to_json(to_string(r));
}
void from_json(const QJsonValue& j, Role& r) {
    r = role_from_string(from_json<QString>(j));
}

QJsonValue to_json(const Annotations& a) {
    QJsonObject o;
    if (a.audience.has_value()) {
        QJsonArray arr;
        for (auto r : *a.audience)
            arr.append(to_json(r));
        o.insert(QStringLiteral("audience"), arr);
    }
    put_optional(o, "priority", a.priority);
    put_optional_string(o, "lastModified", a.last_modified);
    return o;
}

void from_json(const QJsonValue& j, Annotations& a) {
    const QJsonObject o = as_object(j);
    if (auto it = o.constFind(QStringLiteral("audience"));
        it != o.constEnd() && it->isArray()) {
        const QJsonArray arr = it->toArray();
        QList<Role> roles;
        roles.reserve(arr.size());
        for (const auto& v : arr)
            roles.push_back(from_json<Role>(v));
        a.audience = std::move(roles);
    }
    take_optional(o, "priority", a.priority);
    take_optional_string(o, "lastModified", a.last_modified);
}

QJsonValue to_json(const TextContent& c) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("text"));
    o.insert(QStringLiteral("text"), to_json(c.text));
    put_optional(o, "annotations", c.annotations);
    return o;
}

void from_json(const QJsonValue& j, TextContent& c) {
    const QJsonObject o = as_object(j);
    c.text = require<QString>(o, "text");
    take_optional(o, "annotations", c.annotations);
}

QJsonValue to_json(const ImageContent& c) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("image"));
    o.insert(QStringLiteral("data"), to_json(c.data));
    o.insert(QStringLiteral("mimeType"), to_json(c.mime_type));
    put_optional(o, "annotations", c.annotations);
    return o;
}

void from_json(const QJsonValue& j, ImageContent& c) {
    const QJsonObject o = as_object(j);
    c.data = require<QString>(o, "data");
    c.mime_type = require<QString>(o, "mimeType");
    take_optional(o, "annotations", c.annotations);
}

QJsonValue to_json(const AudioContent& c) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("audio"));
    o.insert(QStringLiteral("data"), to_json(c.data));
    o.insert(QStringLiteral("mimeType"), to_json(c.mime_type));
    put_optional(o, "annotations", c.annotations);
    return o;
}

void from_json(const QJsonValue& j, AudioContent& c) {
    const QJsonObject o = as_object(j);
    c.data = require<QString>(o, "data");
    c.mime_type = require<QString>(o, "mimeType");
    take_optional(o, "annotations", c.annotations);
}

// =====================================================================
// Resources
// =====================================================================

QJsonValue to_json(const TextResourceContents& c) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(c.uri));
    o.insert(QStringLiteral("text"), to_json(c.text));
    put_optional_string(o, "mimeType", c.mime_type);
    return o;
}

void from_json(const QJsonValue& j, TextResourceContents& c) {
    const QJsonObject o = as_object(j);
    c.uri = require<QString>(o, "uri");
    c.text = require<QString>(o, "text");
    take_optional_string(o, "mimeType", c.mime_type);
}

QJsonValue to_json(const BlobResourceContents& c) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(c.uri));
    o.insert(QStringLiteral("blob"), to_json(c.blob));
    put_optional_string(o, "mimeType", c.mime_type);
    return o;
}

void from_json(const QJsonValue& j, BlobResourceContents& c) {
    const QJsonObject o = as_object(j);
    c.uri = require<QString>(o, "uri");
    c.blob = require<QString>(o, "blob");
    take_optional_string(o, "mimeType", c.mime_type);
}

QJsonValue to_json(const ResourceContents& c) {
    return std::visit([](const auto& v) { return to_json(v); }, c);
}

void from_json(const QJsonValue& j, ResourceContents& c) {
    const QJsonObject o = as_object(j);
    if (o.contains(QStringLiteral("text"))) {
        c = from_json<TextResourceContents>(j);
        return;
    }
    if (o.contains(QStringLiteral("blob"))) {
        c = from_json<BlobResourceContents>(j);
        return;
    }
    throw Error(error_code::parse_error,
                "ResourceContents must have either \"text\" or \"blob\"");
}

QJsonValue to_json(const Resource& r) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(r.uri));
    o.insert(QStringLiteral("name"), to_json(r.name));
    put_optional_string(o, "title", r.title);
    put_optional_string(o, "description", r.description);
    put_optional_string(o, "mimeType", r.mime_type);
    put_optional(o, "annotations", r.annotations);
    put_optional(o, "size", r.size);
    put_optional(o, "icons", r.icons);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, Resource& r) {
    const QJsonObject o = as_object(j);
    r.uri = require<QString>(o, "uri");
    r.name = require<QString>(o, "name");
    take_optional_string(o, "title", r.title);
    take_optional_string(o, "description", r.description);
    take_optional_string(o, "mimeType", r.mime_type);
    take_optional(o, "annotations", r.annotations);
    take_optional(o, "size", r.size);
    take_optional(o, "icons", r.icons);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const ResourceTemplate& r) {
    QJsonObject o;
    o.insert(QStringLiteral("uriTemplate"), to_json(r.uri_template));
    o.insert(QStringLiteral("name"), to_json(r.name));
    put_optional_string(o, "title", r.title);
    put_optional_string(o, "description", r.description);
    put_optional_string(o, "mimeType", r.mime_type);
    put_optional(o, "annotations", r.annotations);
    put_optional(o, "icons", r.icons);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, ResourceTemplate& r) {
    const QJsonObject o = as_object(j);
    r.uri_template = require<QString>(o, "uriTemplate");
    r.name = require<QString>(o, "name");
    take_optional_string(o, "title", r.title);
    take_optional_string(o, "description", r.description);
    take_optional_string(o, "mimeType", r.mime_type);
    take_optional(o, "annotations", r.annotations);
    take_optional(o, "icons", r.icons);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const ResourceLink& r) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("resource_link"));
    o.insert(QStringLiteral("uri"), to_json(r.uri));
    o.insert(QStringLiteral("name"), to_json(r.name));
    put_optional_string(o, "title", r.title);
    put_optional_string(o, "description", r.description);
    put_optional_string(o, "mimeType", r.mime_type);
    put_optional(o, "annotations", r.annotations);
    put_optional(o, "size", r.size);
    put_optional(o, "icons", r.icons);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, ResourceLink& r) {
    const QJsonObject o = as_object(j);
    r.uri = require<QString>(o, "uri");
    r.name = require<QString>(o, "name");
    take_optional_string(o, "title", r.title);
    take_optional_string(o, "description", r.description);
    take_optional_string(o, "mimeType", r.mime_type);
    take_optional(o, "annotations", r.annotations);
    take_optional(o, "size", r.size);
    take_optional(o, "icons", r.icons);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const EmbeddedResource& r) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("resource"));
    o.insert(QStringLiteral("resource"), to_json(r.resource));
    put_optional(o, "annotations", r.annotations);
    return o;
}

void from_json(const QJsonValue& j, EmbeddedResource& r) {
    const QJsonObject o = as_object(j);
    r.resource = require<ResourceContents>(o, "resource");
    take_optional(o, "annotations", r.annotations);
}

QJsonValue to_json(const ToolUseContent& c) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("tool_use"));
    o.insert(QStringLiteral("id"), to_json(c.id));
    o.insert(QStringLiteral("name"), to_json(c.name));
    o.insert(QStringLiteral("input"), c.input);
    put_optional(o, "annotations", c.annotations);
    return o;
}
void from_json(const QJsonValue& j, ToolUseContent& c) {
    const QJsonObject o = as_object(j);
    c.id = require<QString>(o, "id");
    c.name = require<QString>(o, "name");
    c.input = require<Json>(o, "input");
    take_optional(o, "annotations", c.annotations);
}

// PlainContentBlock is the variant exclusion of tool_use/tool_result.
QJsonValue to_json(const PlainContentBlock& c) {
    return std::visit([](const auto& v) { return to_json(v); }, c);
}
void from_json(const QJsonValue& j, PlainContentBlock& c) {
    const QJsonObject o = as_object(j);
    const auto t = require<QString>(o, "type");
    if (t == "text") {
        c = from_json<TextContent>(j);
        return;
    }
    if (t == "image") {
        c = from_json<ImageContent>(j);
        return;
    }
    if (t == "audio") {
        c = from_json<AudioContent>(j);
        return;
    }
    if (t == "resource_link") {
        c = from_json<ResourceLink>(j);
        return;
    }
    if (t == "resource") {
        c = from_json<EmbeddedResource>(j);
        return;
    }
    throw Error(error_code::parse_error,
                QString{"plain content block must not be \"" + t + "\""});
}

QJsonValue to_json(const ToolResultContent& c) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("tool_result"));
    o.insert(QStringLiteral("toolUseId"), to_json(c.tool_use_id));
    o.insert(QStringLiteral("content"), to_json(c.content));
    put_optional(o, "isError", c.is_error);
    put_optional(o, "annotations", c.annotations);
    return o;
}
void from_json(const QJsonValue& j, ToolResultContent& c) {
    const QJsonObject o = as_object(j);
    c.tool_use_id = require<QString>(o, "toolUseId");
    c.content = require<QList<PlainContentBlock>>(o, "content");
    take_optional(o, "isError", c.is_error);
    take_optional(o, "annotations", c.annotations);
}

QJsonValue to_json(const ContentBlock& c) {
    return std::visit([](const auto& v) { return to_json(v); }, c);
}

void from_json(const QJsonValue& j, ContentBlock& c) {
    const QJsonObject o = as_object(j);
    const auto t = require<QString>(o, "type");
    if (t == "text") {
        c = from_json<TextContent>(j);
        return;
    }
    if (t == "image") {
        c = from_json<ImageContent>(j);
        return;
    }
    if (t == "audio") {
        c = from_json<AudioContent>(j);
        return;
    }
    if (t == "resource_link") {
        c = from_json<ResourceLink>(j);
        return;
    }
    if (t == "resource") {
        c = from_json<EmbeddedResource>(j);
        return;
    }
    if (t == "tool_use") {
        c = from_json<ToolUseContent>(j);
        return;
    }
    if (t == "tool_result") {
        c = from_json<ToolResultContent>(j);
        return;
    }
    throw Error(error_code::parse_error,
                QString{"unknown content block type: "} + t);
}

// =====================================================================
// Tools
// =====================================================================

QJsonValue to_json(const ToolAnnotations& a) {
    QJsonObject o;
    put_optional_string(o, "title", a.title);
    put_optional(o, "readOnlyHint", a.read_only_hint);
    put_optional(o, "destructiveHint", a.destructive_hint);
    put_optional(o, "idempotentHint", a.idempotent_hint);
    put_optional(o, "openWorldHint", a.open_world_hint);
    return o;
}

void from_json(const QJsonValue& j, ToolAnnotations& a) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "title", a.title);
    take_optional(o, "readOnlyHint", a.read_only_hint);
    take_optional(o, "destructiveHint", a.destructive_hint);
    take_optional(o, "idempotentHint", a.idempotent_hint);
    take_optional(o, "openWorldHint", a.open_world_hint);
}

QJsonValue to_json(TaskSupport t) {
    switch (t) {
    case TaskSupport::forbidden:
        return to_json("forbidden");
    case TaskSupport::optional:
        return to_json("optional");
    case TaskSupport::required:
        return to_json("required");
    }
    return to_json("optional");  // unreachable; defensive default
}
void from_json(const QJsonValue& j, TaskSupport& t) {
    const auto v = from_json<QString>(j);
    if (v == "forbidden")
        t = TaskSupport::forbidden;
    else if (v == "optional")
        t = TaskSupport::optional;
    else if (v == "required")
        t = TaskSupport::required;
    else
        throw Error(error_code::invalid_params,
                    "tool execution: unknown taskSupport \"" + v + "\"");
}

QJsonValue to_json(const ToolExecution& e) {
    QJsonObject o;
    o.insert(QStringLiteral("taskSupport"), to_json(e.task_support));
    return o;
}
void from_json(const QJsonValue& j, ToolExecution& e) {
    const QJsonObject o = as_object(j);
    if (auto it = o.constFind(QStringLiteral("taskSupport"));
        it != o.constEnd()) {
        e.task_support = from_json<TaskSupport>(*it);
    } else {
        e.task_support = TaskSupport::optional;
    }
}

QJsonValue to_json(const Tool& t) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(t.name));
    o.insert(QStringLiteral("inputSchema"), t.input_schema);
    put_optional_string(o, "title", t.title);
    put_optional_string(o, "description", t.description);
    put_optional<Json>(o, "outputSchema", t.output_schema);
    put_optional(o, "annotations", t.annotations);
    put_optional(o, "icons", t.icons);
    put_optional(o, "execution", t.execution);
    put_optional<Json>(o, "_meta", t.meta);
    return o;
}

void from_json(const QJsonValue& j, Tool& t) {
    const QJsonObject o = as_object(j);
    t.name = require<QString>(o, "name");
    t.input_schema = require<Json>(o, "inputSchema");
    take_optional_string(o, "title", t.title);
    take_optional_string(o, "description", t.description);
    take_optional<Json>(o, "outputSchema", t.output_schema);
    take_optional(o, "annotations", t.annotations);
    take_optional(o, "icons", t.icons);
    take_optional(o, "execution", t.execution);
    take_optional<Json>(o, "_meta", t.meta);
}

QJsonValue to_json(const ListToolsRequestParams& p) {
    QJsonObject o;
    put_optional_string(o, "cursor", p.cursor);
    return o;
}

void from_json(const QJsonValue& j, ListToolsRequestParams& p) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", p.cursor);
}

QJsonValue to_json(const ListToolsResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("tools"), to_json(r.tools));
    put_optional_string(o, "nextCursor", r.next_cursor);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, ListToolsResult& r) {
    const QJsonObject o = as_object(j);
    r.tools = require<QList<Tool>>(o, "tools");
    take_optional_string(o, "nextCursor", r.next_cursor);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const CallToolRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(p.name));
    put_optional<Json>(o, "arguments", p.arguments);
    put_optional(o, "task", p.task);
    return o;
}

void from_json(const QJsonValue& j, CallToolRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.name = require<QString>(o, "name");
    take_optional<Json>(o, "arguments", p.arguments);
    take_optional(o, "task", p.task);
}

QJsonValue to_json(const CallToolResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("content"), to_json(r.content));
    put_optional<Json>(o, "structuredContent", r.structured_content);
    put_optional(o, "isError", r.is_error);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}

void from_json(const QJsonValue& j, CallToolResult& r) {
    const QJsonObject o = as_object(j);
    r.content = require<QList<ContentBlock>>(o, "content");
    take_optional<Json>(o, "structuredContent", r.structured_content);
    take_optional(o, "isError", r.is_error);
    take_optional<Json>(o, "_meta", r.meta);
}

// =====================================================================
// Resource requests / responses
// =====================================================================

QJsonValue to_json(const ListResourcesRequestParams& p) {
    QJsonObject o;
    put_optional_string(o, "cursor", p.cursor);
    return o;
}
void from_json(const QJsonValue& j, ListResourcesRequestParams& p) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", p.cursor);
}

QJsonValue to_json(const ListResourcesResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("resources"), to_json(r.resources));
    put_optional_string(o, "nextCursor", r.next_cursor);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, ListResourcesResult& r) {
    const QJsonObject o = as_object(j);
    r.resources = require<QList<Resource>>(o, "resources");
    take_optional_string(o, "nextCursor", r.next_cursor);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const ListResourceTemplatesRequestParams& p) {
    QJsonObject o;
    put_optional_string(o, "cursor", p.cursor);
    return o;
}
void from_json(const QJsonValue& j, ListResourceTemplatesRequestParams& p) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", p.cursor);
}

QJsonValue to_json(const ListResourceTemplatesResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("resourceTemplates"),
             to_json(r.resource_templates));
    put_optional_string(o, "nextCursor", r.next_cursor);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, ListResourceTemplatesResult& r) {
    const QJsonObject o = as_object(j);
    r.resource_templates =
        require<QList<ResourceTemplate>>(o, "resourceTemplates");
    take_optional_string(o, "nextCursor", r.next_cursor);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const ReadResourceRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(p.uri));
    return o;
}
void from_json(const QJsonValue& j, ReadResourceRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.uri = require<QString>(o, "uri");
}

QJsonValue to_json(const ReadResourceResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("contents"), to_json(r.contents));
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, ReadResourceResult& r) {
    const QJsonObject o = as_object(j);
    r.contents = require<QList<ResourceContents>>(o, "contents");
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const SubscribeRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(p.uri));
    return o;
}
void from_json(const QJsonValue& j, SubscribeRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.uri = require<QString>(o, "uri");
}

QJsonValue to_json(const UnsubscribeRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(p.uri));
    return o;
}
void from_json(const QJsonValue& j, UnsubscribeRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.uri = require<QString>(o, "uri");
}

QJsonValue to_json(const ResourceUpdatedNotificationParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(p.uri));
    return o;
}
void from_json(const QJsonValue& j, ResourceUpdatedNotificationParams& p) {
    const QJsonObject o = as_object(j);
    p.uri = require<QString>(o, "uri");
}

// =====================================================================
// Prompts
// =====================================================================

QJsonValue to_json(const PromptArgument& a) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(a.name));
    put_optional_string(o, "title", a.title);
    put_optional_string(o, "description", a.description);
    put_optional(o, "required", a.required);
    return o;
}
void from_json(const QJsonValue& j, PromptArgument& a) {
    const QJsonObject o = as_object(j);
    a.name = require<QString>(o, "name");
    take_optional_string(o, "title", a.title);
    take_optional_string(o, "description", a.description);
    take_optional(o, "required", a.required);
}

QJsonValue to_json(const Prompt& p) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(p.name));
    put_optional_string(o, "title", p.title);
    put_optional_string(o, "description", p.description);
    if (p.arguments.has_value())
        o.insert(QStringLiteral("arguments"), to_json(*p.arguments));
    put_optional(o, "icons", p.icons);
    put_optional<Json>(o, "_meta", p.meta);
    return o;
}
void from_json(const QJsonValue& j, Prompt& p) {
    const QJsonObject o = as_object(j);
    p.name = require<QString>(o, "name");
    take_optional_string(o, "title", p.title);
    take_optional_string(o, "description", p.description);
    if (auto it = o.constFind(QStringLiteral("arguments"));
        it != o.constEnd() && !it->isNull()) {
        p.arguments = from_json<QList<PromptArgument>>(*it);
    }
    take_optional(o, "icons", p.icons);
    take_optional<Json>(o, "_meta", p.meta);
}

QJsonValue to_json(const PromptMessage& m) {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(m.role));
    o.insert(QStringLiteral("content"), to_json(m.content));
    return o;
}
void from_json(const QJsonValue& j, PromptMessage& m) {
    const QJsonObject o = as_object(j);
    m.role = require<Role>(o, "role");
    m.content = require<ContentBlock>(o, "content");
}

QJsonValue to_json(const ListPromptsRequestParams& p) {
    QJsonObject o;
    put_optional_string(o, "cursor", p.cursor);
    return o;
}
void from_json(const QJsonValue& j, ListPromptsRequestParams& p) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", p.cursor);
}

QJsonValue to_json(const ListPromptsResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("prompts"), to_json(r.prompts));
    put_optional_string(o, "nextCursor", r.next_cursor);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, ListPromptsResult& r) {
    const QJsonObject o = as_object(j);
    r.prompts = require<QList<Prompt>>(o, "prompts");
    take_optional_string(o, "nextCursor", r.next_cursor);
    take_optional<Json>(o, "_meta", r.meta);
}

QJsonValue to_json(const GetPromptRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(p.name));
    if (p.arguments.has_value()) {
        QJsonObject args;
        for (auto it = p.arguments->constBegin(); it != p.arguments->constEnd();
             ++it) {
            args.insert(it.key(), to_json(it.value()));
        }
        o.insert(QStringLiteral("arguments"), args);
    }
    return o;
}
void from_json(const QJsonValue& j, GetPromptRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.name = require<QString>(o, "name");
    if (auto it = o.constFind(QStringLiteral("arguments"));
        it != o.constEnd() && it->isObject()) {
        const QJsonObject src = it->toObject();
        QHash<QString, QString> args;
        for (auto i = src.begin(); i != src.end(); ++i) {
            if (i.value().isString()) {
                args.emplace(i.key(), i.value().toString());
            }
        }
        p.arguments = std::move(args);
    }
}

QJsonValue to_json(const GetPromptResult& r) {
    QJsonObject o;
    put_optional_string(o, "description", r.description);
    o.insert(QStringLiteral("messages"), to_json(r.messages));
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, GetPromptResult& r) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "description", r.description);
    r.messages = require<QList<PromptMessage>>(o, "messages");
    take_optional<Json>(o, "_meta", r.meta);
}

// =====================================================================
// Cancellation
// =====================================================================

QJsonValue to_json(const CancelledNotificationParams& p) {
    QJsonObject o;
    if (p.request_id.has_value())
        o.insert(QStringLiteral("requestId"), to_json(*p.request_id));
    put_optional_string(o, "reason", p.reason);
    return o;
}

void from_json(const QJsonValue& j, CancelledNotificationParams& p) {
    const QJsonObject o = as_object(j);
    if (auto it = o.constFind(QStringLiteral("requestId"));
        it != o.constEnd() && !it->isNull()) {
        p.request_id = from_json<RequestId>(*it);
    }
    take_optional_string(o, "reason", p.reason);
}

// =====================================================================
// Progress
// =====================================================================

QString ProgressToken::canonical(void) const {
    return is_string() ? "s:" + as_string()
                       : "i:" + QString::number(as_integer());
}

QJsonValue to_json(const ProgressToken& t) {
    if (t.is_string())
        return to_json(t.as_string());
    return to_json(t.as_integer());
}
void from_json(const QJsonValue& j, ProgressToken& t) {
    if (j.isString()) {
        t = ProgressToken{j.toString()};
        return;
    }
    if (j.isDouble()) {
        // Same rationale as RequestId — accept whole-number floats.
        std::int64_t v{};
        from_json(j, v);
        t = ProgressToken{v};
        return;
    }
    throw Error(error_code::parse_error,
                "progressToken must be a string or integer");
}

QJsonValue to_json(const ProgressNotificationParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("progressToken"), to_json(p.progress_token));
    o.insert(QStringLiteral("progress"), to_json(p.progress));
    put_optional(o, "total", p.total);
    put_optional_string(o, "message", p.message);
    return o;
}
void from_json(const QJsonValue& j, ProgressNotificationParams& p) {
    const QJsonObject o = as_object(j);
    p.progress_token = require<ProgressToken>(o, "progressToken");
    p.progress = require<double>(o, "progress");
    take_optional(o, "total", p.total);
    take_optional_string(o, "message", p.message);
}

// =====================================================================
// Logging (protocol-level)
// =====================================================================

QString to_string(LoggingLevel l) noexcept {
    switch (l) {
    case LoggingLevel::debug:
        return "debug";
    case LoggingLevel::info:
        return "info";
    case LoggingLevel::notice:
        return "notice";
    case LoggingLevel::warning:
        return "warning";
    case LoggingLevel::error:
        return "error";
    case LoggingLevel::critical:
        return "critical";
    case LoggingLevel::alert:
        return "alert";
    case LoggingLevel::emergency:
        return "emergency";
    }
    return "?";
}

namespace {
LoggingLevel logging_level_from_string(QString s) {
    if (s == "debug")
        return LoggingLevel::debug;
    if (s == "info")
        return LoggingLevel::info;
    if (s == "notice")
        return LoggingLevel::notice;
    if (s == "warning")
        return LoggingLevel::warning;
    if (s == "error")
        return LoggingLevel::error;
    if (s == "critical")
        return LoggingLevel::critical;
    if (s == "alert")
        return LoggingLevel::alert;
    if (s == "emergency")
        return LoggingLevel::emergency;
    throw Error(error_code::parse_error,
                QString{"unknown logging level: "} + QString{s});
}
}  // namespace

QJsonValue to_json(LoggingLevel l) {
    return to_json(to_string(l));
}
void from_json(const QJsonValue& j, LoggingLevel& l) {
    l = logging_level_from_string(from_json<QString>(j));
}

QJsonValue to_json(const SetLevelRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("level"), to_json(p.level));
    return o;
}
void from_json(const QJsonValue& j, SetLevelRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.level = require<LoggingLevel>(o, "level");
}

QJsonValue to_json(const LoggingMessageNotificationParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("level"), to_json(p.level));
    o.insert(QStringLiteral("data"), p.data);
    put_optional_string(o, "logger", p.logger);
    return o;
}
void from_json(const QJsonValue& j, LoggingMessageNotificationParams& p) {
    const QJsonObject o = as_object(j);
    p.level = require<LoggingLevel>(o, "level");
    p.data = require<Json>(o, "data");
    take_optional_string(o, "logger", p.logger);
}

// =====================================================================
// Sampling
// =====================================================================

QJsonValue to_json(const ModelHint& h) {
    QJsonObject o;
    put_optional_string(o, "name", h.name);
    return o;
}
void from_json(const QJsonValue& j, ModelHint& h) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "name", h.name);
}

QJsonValue to_json(const ModelPreferences& p) {
    QJsonObject o;
    if (p.hints.has_value())
        o.insert(QStringLiteral("hints"), to_json(*p.hints));
    put_optional(o, "costPriority", p.cost_priority);
    put_optional(o, "speedPriority", p.speed_priority);
    put_optional(o, "intelligencePriority", p.intelligence_priority);
    return o;
}
void from_json(const QJsonValue& j, ModelPreferences& p) {
    const QJsonObject o = as_object(j);
    if (auto it = o.constFind(QStringLiteral("hints"));
        it != o.constEnd() && it->isArray()) {
        p.hints = from_json<QList<ModelHint>>(*it);
    }
    take_optional(o, "costPriority", p.cost_priority);
    take_optional(o, "speedPriority", p.speed_priority);
    take_optional(o, "intelligencePriority", p.intelligence_priority);
}

QString to_string(IncludeContext c) noexcept {
    switch (c) {
    case IncludeContext::none:
        return "none";
    case IncludeContext::this_server:
        return "thisServer";
    case IncludeContext::all_servers:
        return "allServers";
    }
    return "?";
}
QJsonValue to_json(IncludeContext c) {
    return to_json(to_string(c));
}
void from_json(const QJsonValue& j, IncludeContext& c) {
    const auto s = from_json<QString>(j);
    if (s == "none")
        c = IncludeContext::none;
    else if (s == "thisServer")
        c = IncludeContext::this_server;
    else if (s == "allServers")
        c = IncludeContext::all_servers;
    else
        throw Error(error_code::parse_error, "unknown includeContext: " + s);
}

QString to_string(ToolChoiceMode m) noexcept {
    switch (m) {
    case ToolChoiceMode::m_auto:
        return "auto";
    case ToolChoiceMode::any:
        return "any";
    case ToolChoiceMode::none:
        return "none";
    case ToolChoiceMode::tool:
        return "tool";
    }
    return "?";
}
QJsonValue to_json(ToolChoiceMode m) {
    return to_json(to_string(m));
}
void from_json(const QJsonValue& j, ToolChoiceMode& m) {
    const auto s = from_json<QString>(j);
    if (s == "auto")
        m = ToolChoiceMode::m_auto;
    else if (s == "any")
        m = ToolChoiceMode::any;
    else if (s == "none")
        m = ToolChoiceMode::none;
    else if (s == "tool")
        m = ToolChoiceMode::tool;
    else
        throw Error(error_code::parse_error, "unknown toolChoice mode: " + s);
}

// Tagged-by-shape: when `mode == tool` we MUST emit the object form
// `{ type: "tool", name: "..." }`. Other modes serialise as a bare
// string on the wire. Symmetrically, from_json accepts either form.
QJsonValue to_json(const ToolChoice& c) {
    if (c.mode.has_value() && *c.mode == ToolChoiceMode::tool) {
        QJsonObject o;
        o.insert(QStringLiteral("type"), to_json("tool"));
        if (!c.name.isNull())
            o.insert(QStringLiteral("name"), to_json(c.name));
        return o;
    }
    if (c.mode.has_value()) {
        return to_json(*c.mode);
    }
    return QJsonObject{};
}
void from_json(const QJsonValue& j, ToolChoice& c) {
    if (j.isString()) {
        c.mode = from_json<ToolChoiceMode>(j);
        return;
    }
    if (j.isObject()) {
        const QJsonObject o = j.toObject();
        if (auto it = o.constFind(QStringLiteral("type"));
            it != o.constEnd() && it->isString()) {
            const auto t = it->toString();
            if (t == "tool") {
                c.mode = ToolChoiceMode::tool;
                take_optional_string(o, "name", c.name);
                return;
            }
            // Unknown object shape — fall through to the legacy
            // `{ mode: ... }` form for forward compat.
        }
        take_optional(o, "mode", c.mode);
        take_optional_string(o, "name", c.name);
        return;
    }
    throw Error(error_code::parse_error,
                "toolChoice must be a string or an object");
}

QJsonValue to_json(const SamplingMessage& m) {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(m.role));
    if (m.content.size() == 1) {
        o.insert(QStringLiteral("content"), to_json(m.content.front()));
    } else {
        o.insert(QStringLiteral("content"), to_json(m.content));
    }
    return o;
}
void from_json(const QJsonValue& j, SamplingMessage& m) {
    const QJsonObject o = as_object(j);
    m.role = require<Role>(o, "role");
    const QJsonValue c = o.value(QStringLiteral("content"));
    if (c.isArray()) {
        m.content = from_json<QList<ContentBlock>>(c);
    } else {
        m.content = {from_json<ContentBlock>(c)};
    }
}

QJsonValue to_json(const CreateMessageRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("messages"), to_json(p.messages));
    o.insert(QStringLiteral("maxTokens"), to_json(p.max_tokens));
    put_optional(o, "modelPreferences", p.model_preferences);
    put_optional_string(o, "systemPrompt", p.system_prompt);
    put_optional(o, "includeContext", p.include_context);
    put_optional(o, "temperature", p.temperature);
    if (p.stop_sequences.has_value())
        o.insert(QStringLiteral("stopSequences"), to_json(*p.stop_sequences));
    put_optional<Json>(o, "metadata", p.metadata);
    put_optional(o, "tools", p.tools);
    put_optional(o, "toolChoice", p.tool_choice);
    return o;
}
void from_json(const QJsonValue& j, CreateMessageRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.messages = require<QList<SamplingMessage>>(o, "messages");

    // maxTokens: reject non-integer / negative / fractional values.
    const QJsonValue mt = o.value(QStringLiteral("maxTokens"));
    if (!mt.isDouble()) {
        throw Error(error_code::invalid_params, "maxTokens must be an integer");
    }
    const double d = mt.toDouble();
    if (d < 0) {
        throw Error(error_code::invalid_params,
                    "maxTokens must be non-negative");
    }
    const qint64 iv = mt.toInteger();
    if (static_cast<double>(iv) != d) {
        throw Error(error_code::invalid_params, "maxTokens must be an integer");
    }
    p.max_tokens = static_cast<std::int64_t>(iv);

    take_optional(o, "modelPreferences", p.model_preferences);
    take_optional_string(o, "systemPrompt", p.system_prompt);
    take_optional(o, "includeContext", p.include_context);
    take_optional(o, "temperature", p.temperature);
    if (auto it = o.constFind(QStringLiteral("stopSequences"));
        it != o.constEnd() && it->isArray()) {
        p.stop_sequences = from_json<QList<QString>>(*it);
    }
    take_optional<Json>(o, "metadata", p.metadata);
    take_optional(o, "tools", p.tools);
    take_optional(o, "toolChoice", p.tool_choice);
}

QJsonValue to_json(const CreateMessageResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("role"), to_json(r.role));
    if (r.content.size() == 1) {
        o.insert(QStringLiteral("content"), to_json(r.content.front()));
    } else {
        o.insert(QStringLiteral("content"), to_json(r.content));
    }
    o.insert(QStringLiteral("model"), to_json(r.model));
    put_optional_string(o, "stopReason", r.stop_reason);
    put_optional<Json>(o, "_meta", r.meta);
    return o;
}
void from_json(const QJsonValue& j, CreateMessageResult& r) {
    const QJsonObject o = as_object(j);
    r.role = require<Role>(o, "role");
    const QJsonValue c = o.value(QStringLiteral("content"));
    if (c.isArray()) {
        r.content = from_json<QList<ContentBlock>>(c);
    } else {
        r.content = {from_json<ContentBlock>(c)};
    }
    r.model = require<QString>(o, "model");
    take_optional_string(o, "stopReason", r.stop_reason);
    take_optional<Json>(o, "_meta", r.meta);
}

// =====================================================================
// Roots
// =====================================================================

QJsonValue to_json(const Root& r) {
    QJsonObject o;
    o.insert(QStringLiteral("uri"), to_json(r.uri));
    put_optional_string(o, "name", r.name);
    return o;
}
void from_json(const QJsonValue& j, Root& r) {
    const QJsonObject o = as_object(j);
    r.uri = require<QString>(o, "uri");
    take_optional_string(o, "name", r.name);
}

QJsonValue to_json(const ListRootsResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("roots"), to_json(r.roots));
    return o;
}
void from_json(const QJsonValue& j, ListRootsResult& r) {
    const QJsonObject o = as_object(j);
    r.roots = require<QList<Root>>(o, "roots");
}

// =====================================================================
// Completion
// =====================================================================

QJsonValue to_json(const ResourceTemplateReference& r) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("ref/resource"));
    o.insert(QStringLiteral("uri"), to_json(r.uri_template));
    return o;
}
void from_json(const QJsonValue& j, ResourceTemplateReference& r) {
    const QJsonObject o = as_object(j);
    r.uri_template = require<QString>(o, "uri");
}

QJsonValue to_json(const PromptReference& r) {
    QJsonObject o;
    o.insert(QStringLiteral("type"), to_json("ref/prompt"));
    o.insert(QStringLiteral("name"), to_json(r.name));
    put_optional_string(o, "title", r.title);
    return o;
}
void from_json(const QJsonValue& j, PromptReference& r) {
    const QJsonObject o = as_object(j);
    r.name = require<QString>(o, "name");
    take_optional_string(o, "title", r.title);
}

QJsonValue to_json(const CompletionReference& r) {
    return std::visit([](const auto& v) { return to_json(v); }, r);
}
void from_json(const QJsonValue& j, CompletionReference& r) {
    const QJsonObject o = as_object(j);
    const auto t = require<QString>(o, "type");
    if (t == "ref/resource") {
        r = from_json<ResourceTemplateReference>(j);
        return;
    }
    if (t == "ref/prompt") {
        r = from_json<PromptReference>(j);
        return;
    }
    throw Error(error_code::parse_error,
                QString{"unknown completion reference type: "} + t);
}

QJsonValue to_json(const CompleteArgument& a) {
    QJsonObject o;
    o.insert(QStringLiteral("name"), to_json(a.name));
    o.insert(QStringLiteral("value"), to_json(a.value));
    return o;
}
void from_json(const QJsonValue& j, CompleteArgument& a) {
    const QJsonObject o = as_object(j);
    a.name = require<QString>(o, "name");
    a.value = require<QString>(o, "value");
}

QJsonValue to_json(const CompleteRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("ref"), to_json(p.reference));
    o.insert(QStringLiteral("argument"), to_json(p.argument));
    if (p.context_arguments.has_value()) {
        QJsonObject ctx;
        for (auto it = p.context_arguments->constBegin();
             it != p.context_arguments->constEnd(); ++it) {
            ctx.insert(it.key(), to_json(it.value()));
        }
        QJsonObject wrapper;
        wrapper.insert(QStringLiteral("arguments"), ctx);
        o.insert(QStringLiteral("context"), wrapper);
    }
    return o;
}
void from_json(const QJsonValue& j, CompleteRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.reference = require<CompletionReference>(o, "ref");
    p.argument = require<CompleteArgument>(o, "argument");
    if (auto it = o.constFind(QStringLiteral("context"));
        it != o.constEnd() && it->isObject()) {
        const QJsonObject ctx = it->toObject();
        if (auto a = ctx.constFind(QStringLiteral("arguments"));
            a != ctx.constEnd() && a->isObject()) {
            const QJsonObject src = a->toObject();
            QHash<QString, QString> args;
            for (auto i = src.begin(); i != src.end(); ++i) {
                if (i.value().isString()) {
                    args.emplace(i.key(), i.value().toString());
                }
            }
            p.context_arguments = std::move(args);
        }
    }
}

QJsonValue to_json(const CompletionValues& v) {
    QJsonObject o;
    o.insert(QStringLiteral("values"), to_json(v.values));
    put_optional(o, "total", v.total);
    put_optional(o, "hasMore", v.has_more);
    return o;
}
void from_json(const QJsonValue& j, CompletionValues& v) {
    const QJsonObject o = as_object(j);
    v.values = require<QList<QString>>(o, "values");
    take_optional(o, "total", v.total);
    take_optional(o, "hasMore", v.has_more);
}

QJsonValue to_json(const CompleteResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("completion"), to_json(r.completion));
    return o;
}
void from_json(const QJsonValue& j, CompleteResult& r) {
    const QJsonObject o = as_object(j);
    r.completion = require<CompletionValues>(o, "completion");
}

// =====================================================================
// Elicitation
// =====================================================================

QJsonValue to_json(const ElicitFormRequestParams& p) {
    QJsonObject o;
    // mode is optional and defaults to "form"; we omit it to match the
    // canonical wire form most servers and validators emit.
    o.insert(QStringLiteral("message"), to_json(p.message));
    o.insert(QStringLiteral("requestedSchema"), p.requested_schema);
    return o;
}
void from_json(const QJsonValue& j, ElicitFormRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.message = require<QString>(o, "message");
    p.requested_schema = require<Json>(o, "requestedSchema");
    // Per spec, requestedSchema is a JSON-Schema object describing
    // the expected response shape. Reject non-object values up
    // front rather than letting them propagate to a confused
    // handler.
    if (!p.requested_schema.isObject()) {
        throw Error(
            error_code::invalid_params,
            "elicitation/create: `requestedSchema` must be a JSON object");
    }
}

QJsonValue to_json(const ElicitUrlRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("mode"), to_json("url"));
    o.insert(QStringLiteral("message"), to_json(p.message));
    o.insert(QStringLiteral("url"), to_json(p.url));
    o.insert(QStringLiteral("elicitationId"), to_json(p.elicitation_id));
    return o;
}
void from_json(const QJsonValue& j, ElicitUrlRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.message = require<QString>(o, "message");
    p.url = require<QString>(o, "url");
    p.elicitation_id = require<QString>(o, "elicitationId");
}

QJsonValue to_json(const ElicitRequestParams& p) {
    return std::visit([](const auto& alt) { return to_json(alt); }, p);
}
void from_json(const QJsonValue& j, ElicitRequestParams& p) {
    // Tag-by-`mode`: omitted ⇒ form mode (spec default). A present
    // `mode` field MUST be a string; null and other types are
    // rejected so a future spec extension that ships a
    // non-string-mode shape can't silently fall through to form.
    const QJsonObject o = as_object(j);
    QString mode = "form";
    if (auto it = o.constFind(QStringLiteral("mode")); it != o.constEnd()) {
        if (!it->isString()) {
            throw Error(error_code::invalid_params,
                        "elicitation/create: `mode` must be a string");
        }
        mode = it->toString();
    }
    if (mode == "form") {
        p = from_json<ElicitFormRequestParams>(j);
    } else if (mode == "url") {
        p = from_json<ElicitUrlRequestParams>(j);
    } else {
        throw Error(error_code::invalid_params,
                    "elicitation/create: unknown mode \"" + mode + "\"");
    }
}

QJsonValue to_json(ElicitAction a) {
    switch (a) {
    case ElicitAction::accept:
        return to_json("accept");
    case ElicitAction::decline:
        return to_json("decline");
    case ElicitAction::cancel:
        return to_json("cancel");
    }
    return to_json("cancel");  // unreachable; defensive default
}
void from_json(const QJsonValue& j, ElicitAction& a) {
    const auto s = from_json<QString>(j);
    if (s == "accept")
        a = ElicitAction::accept;
    else if (s == "decline")
        a = ElicitAction::decline;
    else if (s == "cancel")
        a = ElicitAction::cancel;
    else
        throw Error(error_code::invalid_params,
                    "elicitation result: unknown action \"" + s + "\"");
}

QJsonValue to_json(const ElicitResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("action"), to_json(r.action));
    put_optional<Json>(o, "content", r.content);
    return o;
}
void from_json(const QJsonValue& j, ElicitResult& r) {
    const QJsonObject o = as_object(j);
    r.action = require<ElicitAction>(o, "action");
    take_optional<Json>(o, "content", r.content);
}

QJsonValue to_json(const ElicitationCompleteNotificationParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("elicitationId"), to_json(p.elicitation_id));
    return o;
}
void from_json(const QJsonValue& j, ElicitationCompleteNotificationParams& p) {
    const QJsonObject o = as_object(j);
    p.elicitation_id = require<QString>(o, "elicitationId");
}

QJsonValue to_json(const UrlElicitationRequiredErrorData& d) {
    QJsonObject o;
    o.insert(QStringLiteral("elicitations"), to_json(d.elicitations));
    return o;
}
void from_json(const QJsonValue& j, UrlElicitationRequiredErrorData& d) {
    const QJsonObject o = as_object(j);
    d.elicitations = require<QList<ElicitUrlRequestParams>>(o, "elicitations");
}

// =====================================================================
// Tasks
// =====================================================================

QJsonValue to_json(TaskStatus s) {
    switch (s) {
    case TaskStatus::working:
        return to_json("working");
    case TaskStatus::input_required:
        return to_json("input_required");
    case TaskStatus::completed:
        return to_json("completed");
    case TaskStatus::failed:
        return to_json("failed");
    case TaskStatus::cancelled:
        return to_json("cancelled");
    }
    return to_json("failed");  // unreachable; defensive default
}
void from_json(const QJsonValue& j, TaskStatus& s) {
    const auto v = from_json<QString>(j);
    if (v == "working")
        s = TaskStatus::working;
    else if (v == "input_required")
        s = TaskStatus::input_required;
    else if (v == "completed")
        s = TaskStatus::completed;
    else if (v == "failed")
        s = TaskStatus::failed;
    else if (v == "cancelled")
        s = TaskStatus::cancelled;
    else
        throw Error(error_code::invalid_params,
                    "task: unknown status \"" + v + "\"");
}

QJsonValue to_json(const Task& t) {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(t.taskId));
    o.insert(QStringLiteral("status"), to_json(t.status));
    o.insert(QStringLiteral("createdAt"), to_json(t.created_at));
    o.insert(QStringLiteral("lastUpdatedAt"), to_json(t.last_updated_at));
    put_optional_string(o, "statusMessage", t.status_message);
    put_optional(o, "ttl", t.ttl);
    put_optional(o, "pollInterval", t.poll_interval);
    return o;
}
void from_json(const QJsonValue& j, Task& t) {
    const QJsonObject o = as_object(j);
    t.taskId = require<QString>(o, "taskId");
    t.status = require<TaskStatus>(o, "status");
    t.created_at = require<QString>(o, "createdAt");
    t.last_updated_at = require<QString>(o, "lastUpdatedAt");
    take_optional_string(o, "statusMessage", t.status_message);
    take_optional(o, "ttl", t.ttl);
    take_optional(o, "pollInterval", t.poll_interval);
}

QJsonValue to_json(const CreateTaskResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("task"), to_json(r.task));
    return o;
}
void from_json(const QJsonValue& j, CreateTaskResult& r) {
    const QJsonObject o = as_object(j);
    r.task = require<Task>(o, "task");
}

QJsonValue to_json(const GetTaskRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(p.task_id));
    return o;
}
void from_json(const QJsonValue& j, GetTaskRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.task_id = require<QString>(o, "taskId");
}

QJsonValue to_json(const GetTaskResultRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(p.task_id));
    return o;
}
void from_json(const QJsonValue& j, GetTaskResultRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.task_id = require<QString>(o, "taskId");
}

QJsonValue to_json(const ListTasksRequestParams& p) {
    QJsonObject o;
    put_optional_string(o, "cursor", p.cursor);
    return o;
}
void from_json(const QJsonValue& j, ListTasksRequestParams& p) {
    const QJsonObject o = as_object(j);
    take_optional_string(o, "cursor", p.cursor);
}

QJsonValue to_json(const ListTasksResult& r) {
    QJsonObject o;
    o.insert(QStringLiteral("tasks"), to_json(r.tasks));
    put_optional_string(o, "nextCursor", r.next_cursor);
    return o;
}
void from_json(const QJsonValue& j, ListTasksResult& r) {
    const QJsonObject o = as_object(j);
    r.tasks = require<QList<Task>>(o, "tasks");
    take_optional_string(o, "nextCursor", r.next_cursor);
}

QJsonValue to_json(const CancelTaskRequestParams& p) {
    QJsonObject o;
    o.insert(QStringLiteral("taskId"), to_json(p.task_id));
    return o;
}
void from_json(const QJsonValue& j, CancelTaskRequestParams& p) {
    const QJsonObject o = as_object(j);
    p.task_id = require<QString>(o, "taskId");
}

QJsonValue to_json(const TaskStatusNotificationParams& p) {
    // Notifications carry the same Task projection inline (no
    // wrapping object), per spec.
    return to_json(p.task);
}
void from_json(const QJsonValue& j, TaskStatusNotificationParams& p) {
    p.task = from_json<Task>(j);
}

}  // namespace mcp
