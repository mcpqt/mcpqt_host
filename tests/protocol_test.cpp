// SPDX-License-Identifier: AGPL-3.0-only
//
// Round-trip and shape tests for the protocol types. The strategy: for each
// type, build a non-trivial value, serialize to JSON, compare to a reference
// JSON shape from the spec, then deserialize back and verify equality.

#define QT_NO_DEPRECATED_WARNINGS

#include "protocol.h"

#include "error.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <cstdint>
#include <string>
#include <variant>

namespace
{
} // namespace

// -------------------------------------------------------------------------
// RequestId
// -------------------------------------------------------------------------

class RequestId : public QObject
{
  Q_OBJECT
private slots:
  void IntegerRoundTrip(void);
  void StringRoundTrip(void);
  void CanonicalDistinguishesStringFromInt(void);
  void RejectsBoolAndNull(void);
};

void RequestId::IntegerRoundTrip(void)
{
  mcp::RequestId id{ 42 };
  QVERIFY(id.is_integer());
  QCOMPARE(id.as_integer(), std::int64_t{ 42 });
  QJsonValue j = mcp::to_json(id);
  QCOMPARE(j, mcp::test::jv(42));
  auto back = mcp::from_json<mcp::RequestId>(j);
  QCOMPARE(id, back);
}

void RequestId::StringRoundTrip(void)
{
  mcp::RequestId id{ QString{ "abc" } };
  QVERIFY(id.is_string());
  QCOMPARE(id.as_string(), QString{ "abc" });
  QJsonValue j = mcp::to_json(id);
  QCOMPARE(j, mcp::test::jv("abc"));
  auto back = mcp::from_json<mcp::RequestId>(j);
  QCOMPARE(id, back);
}

void RequestId::CanonicalDistinguishesStringFromInt(void)
{
  mcp::RequestId i{ 5 };
  mcp::RequestId s{ "5" };
  QVERIFY(i.canonical() != s.canonical());
  QVERIFY(std::hash<mcp::RequestId>{}(i) != std::hash<mcp::RequestId>{}(s));
}

void RequestId::RejectsBoolAndNull(void)
{
  QJsonValue j = true;
  mcp::RequestId out;
  QVERIFY_EXCEPTION_THROWN(mcp::from_json(j, out), mcp::Error);
  j = QJsonValue(QJsonValue::Null);
  QVERIFY_EXCEPTION_THROWN(mcp::from_json(j, out), mcp::Error);
}

// -------------------------------------------------------------------------
// JSON-RPC envelope
// -------------------------------------------------------------------------

class JsonRpc : public QObject
{
  Q_OBJECT
private slots:
  void RequestRoundTrip(void);
  void RequestRejectsBadJsonrpc(void);
  void NotificationOmitsId(void);
  void ResponseSuccess(void);
  void ResponseError(void);
  void ResponseRejectsBothResultAndError(void);
  void ParseMessageDispatchesOnShape(void);
  void ParseMessageRejectsBadShape(void);
};

void JsonRpc::RequestRoundTrip(void)
{
  mcp::JsonRpcRequest r{
    .id = mcp::RequestId{ 1 },
    .method = "tools/list",
    .params = QJsonObject{ { "cursor", "abc" } },
  };
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["jsonrpc"], mcp::test::jv("2.0"));
  QCOMPARE(j["id"], mcp::test::jv(1));
  QCOMPARE(j["method"], mcp::test::jv("tools/list"));
  QCOMPARE(j["params"]["cursor"], mcp::test::jv("abc"));
  auto back = mcp::from_json<mcp::JsonRpcRequest>(j);
  QCOMPARE(back.id, r.id);
  QCOMPARE(back.method, r.method);
  QCOMPARE(back.params, r.params);
}

void JsonRpc::RequestRejectsBadJsonrpc(void)
{
  QJsonValue j
      = QJsonObject{ { "jsonrpc", "1.0" }, { "id", 1 }, { "method", "x" } };
  QVERIFY_EXCEPTION_THROWN(mcp::from_json<mcp::JsonRpcRequest>(j), mcp::Error);
}

void JsonRpc::NotificationOmitsId(void)
{
  mcp::JsonRpcNotification n{ .method = "notifications/initialized" };
  QJsonValue j = mcp::to_json(n);
  QVERIFY(!mcp::test::has_key(j, "id"));
  QCOMPARE(j["jsonrpc"], mcp::test::jv("2.0"));
  QCOMPARE(j["method"], mcp::test::jv("notifications/initialized"));
  auto back = mcp::from_json<mcp::JsonRpcNotification>(j);
  QCOMPARE(back.method, n.method);
}

void JsonRpc::ResponseSuccess(void)
{
  mcp::JsonRpcResponse r{
    .id = mcp::RequestId{ 1 },
    .outcome = QJsonObject{ { "k", "v" } },
  };
  QJsonValue j = mcp::to_json(r);
  QVERIFY(mcp::test::has_key(j, "result"));
  QVERIFY(!mcp::test::has_key(j, "error"));
  auto back = mcp::from_json<mcp::JsonRpcResponse>(j);
  QVERIFY(back.is_success());
  QCOMPARE(back.result(), r.result());
}

void JsonRpc::ResponseError(void)
{
  mcp::JsonRpcResponse r{
    .id = mcp::RequestId{ 1 },
    .outcome = mcp::ErrorObject{ mcp::error_code::method_not_found, "no", {} },
  };
  QJsonValue j = mcp::to_json(r);
  QVERIFY(mcp::test::has_key(j, "error"));
  QVERIFY(!mcp::test::has_key(j, "result"));
  auto back = mcp::from_json<mcp::JsonRpcResponse>(j);
  QVERIFY(back.is_error());
  QCOMPARE(back.error().code, mcp::error_code::method_not_found);
  QCOMPARE(back.error().message, QString{ "no" });
}

void JsonRpc::ResponseRejectsBothResultAndError(void)
{
  QJsonValue j = QJsonObject{ { "jsonrpc", "2.0" },
                              { "id", 1 },
                              { "result", "x" },
                              { "error", QJsonObject{ { "code", -1 },
                                                      { "message", "y" } } } };
  QVERIFY_EXCEPTION_THROWN(mcp::from_json<mcp::JsonRpcResponse>(j),
                           mcp::Error);
}

void JsonRpc::ParseMessageDispatchesOnShape(void)
{
  QJsonValue req
      = QJsonObject{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "x" } };
  QJsonValue note = QJsonObject{ { "jsonrpc", "2.0" }, { "method", "x" } };
  QJsonValue resp
      = QJsonObject{ { "jsonrpc", "2.0" }, { "id", 1 }, { "result", "y" } };

  auto m1 = mcp::parse_message(req);
  auto m2 = mcp::parse_message(note);
  auto m3 = mcp::parse_message(resp);

  QVERIFY(std::holds_alternative<mcp::JsonRpcRequest>(m1));
  QVERIFY(std::holds_alternative<mcp::JsonRpcNotification>(m2));
  QVERIFY(std::holds_alternative<mcp::JsonRpcResponse>(m3));
}

void JsonRpc::ParseMessageRejectsBadShape(void)
{
  const QJsonValue bad = QJsonObject{ { "jsonrpc", "2.0" } };
  QVERIFY_EXCEPTION_THROWN(([&] { (void)mcp::parse_message(bad); }()),
                           mcp::Error);
}

// -------------------------------------------------------------------------
// Implementation, capabilities
// -------------------------------------------------------------------------

class Implementation : public QObject
{
  Q_OBJECT
private slots:
  void RoundTripWithAllFields(void);
  void MinimalShape(void);
};

void Implementation::RoundTripWithAllFields(void)
{
  mcp::Implementation imp{
    .name = "calc",
    .title = "Calculator",
    .version = "1.0.0",
    .description = "Tiny arithmetic server",
    .website_url = "https://example.com",
  };
  QJsonValue j = mcp::to_json(imp);
  QCOMPARE(j["name"], mcp::test::jv("calc"));
  QCOMPARE(j["title"], mcp::test::jv("Calculator"));
  QCOMPARE(j["version"], mcp::test::jv("1.0.0"));
  QCOMPARE(j["description"], mcp::test::jv("Tiny arithmetic server"));
  QCOMPARE(j["websiteUrl"], mcp::test::jv("https://example.com"));
  auto back = mcp::from_json<mcp::Implementation>(j);
  QCOMPARE(back.name, imp.name);
  QCOMPARE(back.title, imp.title);
  QCOMPARE(back.version, imp.version);
  QCOMPARE(back.description, imp.description);
  QCOMPARE(back.website_url, imp.website_url);
}

void Implementation::MinimalShape(void)
{
  mcp::Implementation imp{ .name = "x", .version = "1" };
  QJsonValue j = mcp::to_json(imp);
  QVERIFY(!mcp::test::has_key(j, "title"));
  QVERIFY(!mcp::test::has_key(j, "description"));
  QVERIFY(!mcp::test::has_key(j, "websiteUrl"));
  auto back = mcp::from_json<mcp::Implementation>(j);
  QVERIFY(back.title.isNull());
}

class Capabilities : public QObject
{
  Q_OBJECT
private slots:
  void ServerToolsListChanged(void);
  void ServerLoggingPresenceOnly(void);
  void ResourcesSubscribeAndListChanged(void);
  void ClientSamplingTools(void);
  void EmptyServerCapsSerializeAsEmptyObject(void);
};

void Capabilities::ServerToolsListChanged(void)
{
  mcp::ServerCapabilities caps;
  caps.tools = mcp::ToolsCapability{ .list_changed = true };
  QJsonValue j = mcp::to_json(caps);
  QCOMPARE(j["tools"]["listChanged"], mcp::test::jv(true));
  auto back = mcp::from_json<mcp::ServerCapabilities>(j);
  QVERIFY(back.tools.has_value());
  QCOMPARE(back.tools->list_changed, true);
}

void Capabilities::ServerLoggingPresenceOnly(void)
{
  mcp::ServerCapabilities caps;
  caps.logging = QJsonObject{}; // presence-only
  QJsonValue j = mcp::to_json(caps);
  QVERIFY(j["logging"].isObject());
  auto back = mcp::from_json<mcp::ServerCapabilities>(j);
  QVERIFY(!back.logging.isNull());
}

void Capabilities::ResourcesSubscribeAndListChanged(void)
{
  mcp::ServerCapabilities caps;
  caps.resources = mcp::ResourcesCapability{
    .subscribe = true,
    .list_changed = false,
  };
  QJsonValue j = mcp::to_json(caps);
  QCOMPARE(j["resources"]["subscribe"], mcp::test::jv(true));
  QCOMPARE(j["resources"]["listChanged"], mcp::test::jv(false));
  auto back = mcp::from_json<mcp::ServerCapabilities>(j);
  QVERIFY(back.resources.has_value());
  QCOMPARE(back.resources->subscribe, true);
  QCOMPARE(back.resources->list_changed, false);
}

void Capabilities::ClientSamplingTools(void)
{
  mcp::ClientCapabilities caps;
  caps.sampling = mcp::SamplingCapability{ .tools = QJsonObject{} };
  QJsonValue j = mcp::to_json(caps);
  QVERIFY(j["sampling"]["tools"].isObject());
  auto back = mcp::from_json<mcp::ClientCapabilities>(j);
  QVERIFY(back.sampling.has_value());
  QVERIFY(!back.sampling->tools.isNull());
}

void Capabilities::EmptyServerCapsSerializeAsEmptyObject(void)
{
  mcp::ServerCapabilities caps;
  QJsonValue j = mcp::to_json(caps);
  QVERIFY(j.isObject());
  QVERIFY(j.toObject().isEmpty());
}

// -------------------------------------------------------------------------
// initialize
// -------------------------------------------------------------------------

class Initialize : public QObject
{
  Q_OBJECT
private slots:
  void RequestParamsRoundTrip(void);
  void ResultRoundTrip(void);
  void NotificationMethodConstantsAreSpecCorrect(void);
};

void Initialize::RequestParamsRoundTrip(void)
{
  mcp::InitializeRequestParams p{
    .protocol_version = QString{ mcp::kLatestProtocolVersion },
    .capabilities = {},
    .client_info = { .name = "c", .version = "1" },
  };
  p.capabilities.roots = mcp::RootsCapability{ .list_changed = true };
  QJsonValue j = mcp::to_json(p);
  QCOMPARE(j["protocolVersion"], mcp::test::jv("2025-11-25"));
  QCOMPARE(j["clientInfo"]["name"], mcp::test::jv("c"));
  QCOMPARE(j["capabilities"]["roots"]["listChanged"], mcp::test::jv(true));
  auto back = mcp::from_json<mcp::InitializeRequestParams>(j);
  QCOMPARE(back.protocol_version, p.protocol_version);
  QCOMPARE(back.client_info.name, p.client_info.name);
  QVERIFY(back.capabilities.roots.has_value());
  QCOMPARE(back.capabilities.roots->list_changed, true);
}

void Initialize::ResultRoundTrip(void)
{
  mcp::InitializeResult r{
    .protocol_version = "2025-11-25",
    .capabilities = {},
    .server_info = { .name = "s", .version = "1" },
    .instructions = "hello",
  };
  r.capabilities.tools = mcp::ToolsCapability{ .list_changed = true };
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["protocolVersion"], mcp::test::jv("2025-11-25"));
  QCOMPARE(j["serverInfo"]["name"], mcp::test::jv("s"));
  QCOMPARE(j["instructions"], mcp::test::jv("hello"));
  QCOMPARE(j["capabilities"]["tools"]["listChanged"], mcp::test::jv(true));
  auto back = mcp::from_json<mcp::InitializeResult>(j);
  QCOMPARE(back.protocol_version, r.protocol_version);
  QCOMPARE(back.instructions, r.instructions);
  QVERIFY(back.capabilities.tools.has_value());
}

void Initialize::NotificationMethodConstantsAreSpecCorrect(void)
{
  QCOMPARE(mcp::method_initialize, QString{ "initialize" });
  QCOMPARE(mcp::method_notifications_initialized,
           QString{ "notifications/initialized" });
}

// -------------------------------------------------------------------------
// Content blocks
// -------------------------------------------------------------------------

class Content : public QObject
{
  Q_OBJECT
private slots:
  void TextBlockShape(void);
  void ImageBlockShape(void);
  void AudioBlockShape(void);
  void BlockVariantDispatchesOnType(void);
  void BlockVariantRejectsUnknownType(void);
  void AnnotationsRoundTrip(void);
};

void Content::TextBlockShape(void)
{
  mcp::TextContent t{ .text = "hi" };
  QJsonValue j = mcp::to_json(t);
  QCOMPARE(j["type"], mcp::test::jv("text"));
  QCOMPARE(j["text"], mcp::test::jv("hi"));
  auto back = mcp::from_json<mcp::TextContent>(j);
  QCOMPARE(back.text, t.text);
}

void Content::ImageBlockShape(void)
{
  mcp::ImageContent i{ .data = "Zm9v", .mime_type = "image/png" };
  QJsonValue j = mcp::to_json(i);
  QCOMPARE(j["type"], mcp::test::jv("image"));
  QCOMPARE(j["data"], mcp::test::jv("Zm9v"));
  QCOMPARE(j["mimeType"], mcp::test::jv("image/png"));
  auto back = mcp::from_json<mcp::ImageContent>(j);
  QCOMPARE(back.mime_type, i.mime_type);
}

void Content::AudioBlockShape(void)
{
  mcp::AudioContent a{ .data = "abcd", .mime_type = "audio/wav" };
  QJsonValue j = mcp::to_json(a);
  QCOMPARE(j["type"], mcp::test::jv("audio"));
  QCOMPARE(j["mimeType"], mcp::test::jv("audio/wav"));
}

void Content::BlockVariantDispatchesOnType(void)
{
  QJsonValue text = QJsonObject{ { "type", "text" }, { "text", "x" } };
  auto block = mcp::from_json<mcp::ContentBlock>(text);
  QVERIFY(std::holds_alternative<mcp::TextContent>(block));
  QCOMPARE(std::get<mcp::TextContent>(block).text, QString{ "x" });

  QJsonValue image = QJsonObject{ { "type", "image" },
                                  { "data", "Zm9v" },
                                  { "mimeType", "image/png" } };
  block = mcp::from_json<mcp::ContentBlock>(image);
  QVERIFY(std::holds_alternative<mcp::ImageContent>(block));
}

void Content::BlockVariantRejectsUnknownType(void)
{
  QJsonValue unknown = QJsonObject{ { "type", "video" } };
  QVERIFY_EXCEPTION_THROWN(mcp::from_json<mcp::ContentBlock>(unknown),
                           mcp::Error);
}

void Content::AnnotationsRoundTrip(void)
{
  mcp::Annotations a;
  a.audience = QList<mcp::Role>{ mcp::Role::user, mcp::Role::assistant };
  a.priority = 0.5;
  a.last_modified = "2025-01-12T15:00:58Z";
  QJsonValue j = mcp::to_json(a);
  const QJsonArray audience{ mcp::test::jv("user"),
                             mcp::test::jv("assistant") };
  QCOMPARE(j["audience"].toArray(), audience);
  QCOMPARE(j["priority"], mcp::test::jv(0.5));
  auto back = mcp::from_json<mcp::Annotations>(j);
  QVERIFY(back.audience.has_value());
  QCOMPARE(back.audience->size(), std::size_t{ 2 });
  QCOMPARE((*back.audience)[0], mcp::Role::user);
}

// -------------------------------------------------------------------------
// Tools
// -------------------------------------------------------------------------

class Tools : public QObject
{
  Q_OBJECT
private slots:
  void ToolRoundTrip(void);
  void ListToolsResultRoundTrip(void);
  void CallToolRequestParamsShape(void);
  void CallToolResultRoundTrip(void);
  void ToolAnnotationsRoundTrip(void);
};

void Tools::ToolRoundTrip(void)
{
  mcp::Tool t{
    .name = "add",
    .title = "Add",
    .description = "Add two numbers",
    .input_schema
    = QJsonObject{ { "type", "object" },
                   { "properties",
                     QJsonObject{
                         { "a", QJsonObject{ { "type", "number" } } },
                         { "b", QJsonObject{ { "type", "number" } } } } },
                   { "required",
                     QJsonArray{ mcp::test::jv("a"), mcp::test::jv("b") } } },
  };
  QJsonValue j = mcp::to_json(t);
  QCOMPARE(j["name"], mcp::test::jv("add"));
  QCOMPARE(j["inputSchema"]["type"], mcp::test::jv("object"));
  auto back = mcp::from_json<mcp::Tool>(j);
  QCOMPARE(back.name, t.name);
  QCOMPARE(back.input_schema, t.input_schema);
}

void Tools::ListToolsResultRoundTrip(void)
{
  mcp::ListToolsResult r;
  r.tools.push_back(mcp::Tool{
      .name = "x",
      .input_schema = QJsonObject{ { "type", "object" } },
  });
  r.next_cursor = "next";
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["tools"][0]["name"], mcp::test::jv("x"));
  QCOMPARE(j["nextCursor"], mcp::test::jv("next"));
  auto back = mcp::from_json<mcp::ListToolsResult>(j);
  QCOMPARE(back.tools.size(), std::size_t{ 1 });
  QCOMPARE(back.tools[0].name, QString{ "x" });
  QCOMPARE(back.next_cursor, QString{ "next" });
}

void Tools::CallToolRequestParamsShape(void)
{
  mcp::CallToolRequestParams p{
    .name = "add",
    .arguments = QJsonObject{ { "a", 1 }, { "b", 2 } },
  };
  QJsonValue j = mcp::to_json(p);
  QCOMPARE(j["name"], mcp::test::jv("add"));
  QCOMPARE(j["arguments"]["a"], mcp::test::jv(1));
  auto back = mcp::from_json<mcp::CallToolRequestParams>(j);
  QCOMPARE(back.name, p.name);
  QCOMPARE(back.arguments, p.arguments);
}

void Tools::CallToolResultRoundTrip(void)
{
  mcp::CallToolResult r;
  r.content.push_back(mcp::TextContent{ .text = "3" });
  r.is_error = false;
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["content"][0]["type"], mcp::test::jv("text"));
  QCOMPARE(j["content"][0]["text"], mcp::test::jv("3"));
  QCOMPARE(j["isError"], mcp::test::jv(false));
  auto back = mcp::from_json<mcp::CallToolResult>(j);
  QCOMPARE(back.content.size(), std::size_t{ 1 });
  QVERIFY(std::holds_alternative<mcp::TextContent>(back.content[0]));
  QCOMPARE(std::get<mcp::TextContent>(back.content[0]).text, QString{ "3" });
}

void Tools::ToolAnnotationsRoundTrip(void)
{
  mcp::ToolAnnotations a{
    .title = "Add",
    .read_only_hint = true,
    .destructive_hint = false,
    .idempotent_hint = true,
    .open_world_hint = false,
  };
  QJsonValue j = mcp::to_json(a);
  QCOMPARE(j["readOnlyHint"], mcp::test::jv(true));
  QCOMPARE(j["destructiveHint"], mcp::test::jv(false));
  auto back = mcp::from_json<mcp::ToolAnnotations>(j);
  QCOMPARE(back.read_only_hint, a.read_only_hint);
  QCOMPARE(back.destructive_hint, a.destructive_hint);
}

// -------------------------------------------------------------------------
// 2025-11-25 spec additions
// -------------------------------------------------------------------------

class SpecAdditions : public QObject
{
  Q_OBJECT
private slots:
  void ToolUseContentRoundTrip(void);
  void ToolResultContentRoundTrip(void);
  void ToolResultRejectsNestedToolUseInContent(void);
  void SamplingToolsAndToolChoiceRoundTrip(void);
  void ToolChoiceObjectFormForNamedTool(void);
  void IconsAndMetaOnImplementation(void);
  void MetaOnInitializeResult(void);
  void ToolExecutionRoundTrip(void);
  void UrlElicitationRequiredErrorDataRoundTrip(void);
};

void SpecAdditions::ToolUseContentRoundTrip(void)
{
  mcp::ToolUseContent c{
    .id = "tu_abc",
    .name = "search",
    .input = QJsonObject{ { "q", "lima beans" } },
  };
  mcp::ContentBlock cb = c;
  QJsonValue j = mcp::to_json(cb);
  QCOMPARE(j["type"], mcp::test::jv("tool_use"));
  QCOMPARE(j["id"], mcp::test::jv("tu_abc"));
  QCOMPARE(j["name"], mcp::test::jv("search"));
  QCOMPARE(j["input"]["q"], mcp::test::jv("lima beans"));
  auto back = mcp::from_json<mcp::ContentBlock>(j);
  QVERIFY(std::holds_alternative<mcp::ToolUseContent>(back));
  QCOMPARE(std::get<mcp::ToolUseContent>(back).id, QString{ "tu_abc" });
}

void SpecAdditions::ToolResultContentRoundTrip(void)
{
  mcp::ToolResultContent c{
    .tool_use_id = "tu_abc",
    .content = { mcp::TextContent{ .text = "found 42" } },
    .is_error = false,
  };
  mcp::ContentBlock cb = c;
  QJsonValue j = mcp::to_json(cb);
  QCOMPARE(j["type"], mcp::test::jv("tool_result"));
  QCOMPARE(j["toolUseId"], mcp::test::jv("tu_abc"));
  QCOMPARE(j["content"][0]["type"], mcp::test::jv("text"));
  QCOMPARE(j["isError"], mcp::test::jv(false));
  auto back = mcp::from_json<mcp::ContentBlock>(j);
  QVERIFY(std::holds_alternative<mcp::ToolResultContent>(back));
  QCOMPARE(std::get<mcp::ToolResultContent>(back).tool_use_id,
           QString{ "tu_abc" });
}

void SpecAdditions::ToolResultRejectsNestedToolUseInContent(void)
{
  // PlainContentBlock parser MUST reject "tool_use" / "tool_result"
  // — that's the spec rule preventing recursive content blocks.
  QJsonValue j = QJsonObject{
    { "type", "tool_result" },
    { "toolUseId", "x" },
    { "content",
      QJsonArray{
          QJsonObject{ { "type", "tool_use" },
                       { "id", "y" },
                       { "name", "z" },
                       { "input", QJsonObject{} } },
      } },
  };
  QVERIFY_EXCEPTION_THROWN(mcp::from_json<mcp::ContentBlock>(j), mcp::Error);
}

void SpecAdditions::SamplingToolsAndToolChoiceRoundTrip(void)
{
  mcp::CreateMessageRequestParams p{
    .messages = { mcp::SamplingMessage{
        .role = mcp::Role::user,
        .content = { mcp::TextContent{ .text = "?" } },
    } },
    .max_tokens = 100,
  };
  p.tools = QList<mcp::Tool>{ mcp::Tool{
      .name = "search",
      .input_schema = QJsonObject{ { "type", "object" } },
  } };
  p.tool_choice = mcp::ToolChoice{ .mode = mcp::ToolChoiceMode::any };

  QJsonValue j = mcp::to_json(p);
  QVERIFY(mcp::test::has_key(j, "tools"));
  QCOMPARE(j["tools"][0]["name"], mcp::test::jv("search"));
  QCOMPARE(j["toolChoice"], mcp::test::jv("any"));

  auto back = mcp::from_json<mcp::CreateMessageRequestParams>(j);
  QVERIFY(back.tools.has_value());
  QCOMPARE(back.tools->size(), std::size_t{ 1 });
  QVERIFY(back.tool_choice.has_value());
  QCOMPARE(*back.tool_choice->mode, mcp::ToolChoiceMode::any);
}

void SpecAdditions::ToolChoiceObjectFormForNamedTool(void)
{
  mcp::ToolChoice c{
    .mode = mcp::ToolChoiceMode::tool,
    .name = "search",
  };
  QJsonValue j = mcp::to_json(c);
  QVERIFY(j.isObject());
  QCOMPARE(j["type"], mcp::test::jv("tool"));
  QCOMPARE(j["name"], mcp::test::jv("search"));
  auto back = mcp::from_json<mcp::ToolChoice>(j);
  QCOMPARE(*back.mode, mcp::ToolChoiceMode::tool);
  QCOMPARE(back.name, QString{ "search" });
}

void SpecAdditions::IconsAndMetaOnImplementation(void)
{
  mcp::Implementation impl{
    .name = "x",
    .version = "1",
    .icons = QList<mcp::Icon>{ {
        .src = "https://example.com/icon.png",
        .mime_type = "image/png",
    } },
    .meta = QJsonObject{ { "app.example/internal", 42 } },
  };
  QJsonValue j = mcp::to_json(impl);
  QCOMPARE(j["icons"][0]["src"],
           mcp::test::jv("https://example.com/icon.png"));
  QCOMPARE(j["_meta"]["app.example/internal"], mcp::test::jv(42));
  auto back = mcp::from_json<mcp::Implementation>(j);
  QVERIFY(back.icons.has_value());
  QCOMPARE(back.icons->front().src, QString{ "https://example.com/icon.png" });
  QVERIFY(!back.meta.isNull());
  QCOMPARE(back.meta["app.example/internal"], mcp::test::jv(42));
}

void SpecAdditions::MetaOnInitializeResult(void)
{
  mcp::InitializeResult r{
    .protocol_version = "2025-11-25",
    .capabilities = {},
    .server_info = mcp::Implementation{ .name = "s", .version = "0" },
    .meta = QJsonObject{ { "x", 1 } },
  };
  QJsonValue j = mcp::to_json(r);
  QCOMPARE(j["_meta"]["x"], mcp::test::jv(1));
  auto back = mcp::from_json<mcp::InitializeResult>(j);
  QVERIFY(!back.meta.isNull());
  QCOMPARE(back.meta["x"], mcp::test::jv(1));
}

void SpecAdditions::ToolExecutionRoundTrip(void)
{
  mcp::Tool t{
        .name = "danger",
        .input_schema = QJsonObject{{"type", "object"}},
        .execution =
            mcp::ToolExecution{
                .task_support = mcp::TaskSupport::required,
            },
    };
  QJsonValue j = mcp::to_json(t);
  QCOMPARE(j["execution"]["taskSupport"], mcp::test::jv("required"));
  auto back = mcp::from_json<mcp::Tool>(j);
  QVERIFY(back.execution.has_value());
  QCOMPARE(back.execution->task_support, mcp::TaskSupport::required);
}

void SpecAdditions::UrlElicitationRequiredErrorDataRoundTrip(void)
{
  mcp::UrlElicitationRequiredErrorData d{
    .elicitations = { mcp::ElicitUrlRequestParams{
        .message = "Authorise via the linked URL",
        .url = "https://idp.example/auth",
        .elicitation_id = "eid-1",
    } },
  };
  QJsonValue j = mcp::to_json(d);
  QCOMPARE(j["elicitations"][0]["url"],
           mcp::test::jv("https://idp.example/auth"));
  auto back = mcp::from_json<mcp::UrlElicitationRequiredErrorData>(j);
  QCOMPARE(back.elicitations.front().elicitation_id, QString{ "eid-1" });
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int status = 0;
  {
    RequestId suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    JsonRpc suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    Implementation suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    Capabilities suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    Initialize suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    Content suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    Tools suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  {
    SpecAdditions suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  return status;
}

#include "protocol_test.moc"
#include <QList>
#include <QString>
