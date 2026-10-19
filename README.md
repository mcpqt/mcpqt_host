# mcp-cpp

A modern C++20 SDK for the [Model Context Protocol](https://modelcontextprotocol.io)
(MCP), targeting protocol revision **2025-11-25**.

> **Status:** beta. The wire format is locked to the official MCP spec; the C++
> API may still see refinements before 1.0.

[![CI](https://github.com/Neumann-Labs/mcp-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Neumann-Labs/mcp-cpp/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

## Why

LLM applications increasingly need a uniform way to talk to tool, resource, and
prompt providers. MCP is that protocol; this SDK aims to be a clean, fast,
spec-faithful C++ implementation suitable for embedding in editors, IDEs,
agents, native applications, and high-throughput servers.

**Design goals:**

- **Spec-faithful.** Field names, method names, and behaviors match the
  [official TypeScript schema](https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2025-11-25/schema.ts)
  verbatim.
- **Idiomatic C++20.** RAII, value semantics, `std::variant` for sum types,
  futures for async results, no inheritance hierarchies where composition will
  do.
- **Small surface, big leverage.** A focused public API; most of the work
  happens behind a few well-named types.
- **Production-ready.** Sanitizer-clean, thread-safe where it must be, no
  hidden allocations on hot paths.

## What's in 0.1

| Capability | Server | Client | Notes |
| --- | --- | --- | --- |
| Initialize / capabilities negotiation | ✅ | ✅ | spec 2025-11-25 |
| Tools (list, call) | ✅ | ✅ | text/image/audio content blocks |
| Resources (list, read, templates, subscribe/unsubscribe) | ✅ | ✅ | text + base64 blob contents |
| Prompts (list, get) | ✅ | ✅ | typed messages with content blocks |
| Sampling (`sampling/createMessage`) | ✅ initiator | ✅ responder | server-initiated LLM calls |
| Roots (`roots/list`) | ✅ initiator | ✅ responder | client-side filesystem scopes |
| Completion (`completion/complete`) | ✅ | ✅ | autocompletion suggestions |
| Cancellation (`notifications/cancelled`) | ✅ | ✅ | wire-level support |
| Progress (`notifications/progress`) | ✅ | ✅ | typed token + handler |
| Logging (`notifications/message`, `logging/setLevel`) | ✅ | ✅ | RFC-5424 levels |
| Ping (`ping`) | ✅ | ✅ | bi-directional liveness |
| Pagination | ✅ | ✅ | configurable page size |
| `stdio` transport | ✅ | ✅ | spec-correct framing |
| Streamable HTTP transport | ✅ | ✅ | POST + SSE GET stream + session ids |
| Tasks (`tasks/get`/`result`/`list`/`cancel`, status notifications) | ✅ | ✅ | augment `tools/call` via `task` field; spec 2025-11-25 |
| Elicitation (`elicitation/create`, form + url modes) | ✅ initiator | ✅ responder | spec 2025-11-25 |
| OAuth 2.1 authorization (HTTP) | ✅ | ✅ | bearer + RFC 9728 protected resource metadata |
| Plugins (functions from loadable modules) | ✅ host | — | `mcp::PluginManager` + `QPluginLoader`; typed public slots into a cross-plugin registry via `HostInterface` (MCP tool exposure pending) |

## Quick start

### A minimal server

```cpp
#include <mcp/mcp.hpp>

int main() {
    mcp::Server server{
        mcp::Implementation{.name = "calc", .version = "1.0.0"},
    };

    server.tool("add",
        QJsonObject{
            {"type", "object"},
            {"properties", QJsonObject{
                {"a", QJsonObject{{"type", "number"}}},
                {"b", QJsonObject{{"type", "number"}}},
            }},
            {"required", QJsonArray{"a", "b"}},
        },
        [](const mcp::Json& args) -> mcp::CallToolResult {
            const QJsonObject o = args.toObject();
            const double a = mcp::require<double>(o, "a");
            const double b = mcp::require<double>(o, "b");
            return {
                .content = { mcp::TextContent{.text = std::to_string(a + b)} },
            };
        });

    server.run(std::make_unique<mcp::StdioTransport>());
}
```

Run from a host that supports MCP servers (Claude Desktop, Cursor, the official
inspector, etc.) by pointing at the resulting binary. Stderr is yours for
diagnostics; stdout is reserved for the JSON-RPC stream.

### A minimal client

```cpp
#include <mcp/mcp.hpp>

int main() {
    auto pair = /* spawn the server, wire its stdio to a transport */;

    mcp::Client client{
        mcp::Implementation{.name = "my-app", .version = "0.1.0"},
    };
    client.connect(std::move(pair.transport));

    auto info = client.initialize().get();
    std::cout << "connected to " << info.server_info.name << "\n";

    auto tools = client.list_tools().get();
    for (const auto& t : tools.tools) {
        std::cout << " - " << t.name << "\n";
    }

    auto out = client.call_tool("add",
                                QJsonObject{{"a", 2}, {"b", 3}}).get();
    if (auto* text = std::get_if<mcp::TextContent>(&out.content[0])) {
        std::cout << "result: " << text->text << "\n";
    }
}
```

### Hosting over HTTP

Same server, exposed as a Streamable HTTP endpoint. Multiple clients
each get their own `Mcp-Session-Id`-keyed session.

```cpp
mcp::HttpServerHost host{
    mcp::Implementation{.name = "calc", .version = "1.0"},
    mcp::HttpServerHost::Options{
        .host            = "127.0.0.1",
        .port            = 8080,
        .path            = "/mcp",
        .allowed_origins = {"https://example.com"},  // DNS-rebind defense
    },
    [](mcp::Server& s) {
        s.tool("add", schema, handler);
    },
};
host.start();
// ... wait for shutdown signal ...
host.stop();
```

A client connects to it the same way it connects to a stdio server,
but with `HttpClientTransport` instead of `StdioTransport`:

```cpp
mcp::HttpClientTransport::Options topts;
topts.url = "http://localhost:8080/mcp";
mcp::Client client{mcp::Implementation{.name = "my-app", .version = "0.1"}};
client.connect(std::make_unique<mcp::HttpClientTransport>(topts));
auto info = client.initialize().get();
auto out  = client.call_tool("add", {{"a", 1}, {"b", 2}}).get();
```

Both `examples/http_calculator_server` (binds 127.0.0.1:8080 by
default) and the stdio `examples/calculator_server` ship with the
build.

### A non-trivial demo: persistent notes for the assistant

`examples/mcp_notes_server` is a SQLite + FTS5 scratchpad that gives
Claude (or any MCP client) persistent, full-text-searchable memory
across sessions. Plug it into Claude Desktop / Claude Code and ask
"save my Postgres prod connection string under db.prod" — it's
there next time you open a chat. Search "postgres" and you get a
BM25-ranked snippet hit. Glob deletes go through the spec's
elicitation primitive so the human gets a yes/no prompt before
anything is wiped.

Build it (`libsqlite3-dev` on Linux, ships with macOS):

```
cmake -S . -B build -DMCP_BUILD_EXAMPLES=ON
cmake --build build --target mcp_notes_server
```

See `examples/mcp_notes_server/README.md` for the wire-up.

### Server-initiated LLM calls (sampling)

A common agentic pattern is for a tool to ask the host application to make
an LLM call on its behalf.

```cpp
server.tool("ask", schema,
    [&server](const mcp::Json& args) -> mcp::CallToolResult {
        auto resp = server.sample(mcp::CreateMessageRequestParams{
            .messages = {
                mcp::SamplingMessage{
                    .role    = mcp::Role::user,
                    .content = mcp::TextContent{
                        .text = args.toObject().value("q").toString().toStdString(),
                    },
                },
            },
            .max_tokens = 512,
        }).get();
        return {
            .content = { mcp::TextContent{
                .text = std::get<mcp::TextContent>(resp.content).text,
            }},
        };
    });
```

The client plugs in a sampling handler that does the actual LLM call:

```cpp
client.set_sampling_handler(
    [](const mcp::CreateMessageRequestParams& req) -> mcp::CreateMessageResult {
        // ...invoke your LLM provider with `req.messages` etc...
        return {
            .role    = mcp::Role::assistant,
            .content = mcp::TextContent{.text = "..."},
            .model   = "claude-3-5-sonnet-20241022",
        };
    });
```

The Session dispatches inbound requests on a worker thread, so calling
`server.sample(...).get()` from inside a tool handler is safe — it does not
deadlock.

### Plugins: functions from dynamically loaded modules

Plugins can extend the host at runtime. `mcp::PluginManager` loads Qt plugins,
reflects each plugin's public slots into a shared function registry, and routes
cross-plugin calls. Each plugin is an ordinary Qt plugin (`QObject` +
`mcp::PluginInterface`) built as its own `MODULE` library — it never links
against the host binary.

> Functions are currently exposed only to other plugins, not as MCP tools.

```cpp
#include <mcp/mcp.hpp>   // brings in PluginManager + PluginInterface

mcp::PluginManager plugins;
plugins.load_directory("/opt/my-host/plugins"); // or plugins.load_file("foo.so")
```

A plugin publishes normal typed functions as public slots — 0–10 parameters
using ordinary QMetaType-registered types, and a `void` or `QVariant` return.
Parameters are *not* packed into a QVariant: the host invokes the slot
reflectively with `QGenericArgument`, so its signature is its address, e.g.
`myplugin.reverse(QString)`.

```cpp
#include <mcp/plugin.hpp>

class MyPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "myplugin.json")
    Q_INTERFACES(mcp::PluginInterface)
public:
    std::string id() const override { return "myplugin"; }
    void initialize(const mcp::HostInterface& host) override {
        host_ = host;              // keep the handle for later calls
        registerPublicSlots();     // publish every eligible public slot
    }
public slots:
    QVariant reverse(QString text) { return text.reversed(); }
    void     reset()               { /* ... */ }
};
```

Build it as a separate target:

```cmake
add_library(myplugin MODULE myplugin.cpp)
set_target_properties(myplugin PROPERTIES PREFIX "")
target_link_libraries(myplugin PRIVATE mcp::mcp)
```

Plugins call functions in *other* plugins through the `HostInterface` they
received at initialize — discovery via `getFuncs()`, invocation via
`callFunc(signature, args)` where `args` is a positional `QVariantList` with
one value per parameter:

```cpp
void MyPlugin::initialize(const mcp::HostInterface& host) override {
    host_ = host;
    for (const QString& sig : host_.getFuncs())          // discover
        if (sig.startsWith("other.greet("))
            target_ = sig;                                // remember it
    registerPublicSlots();
}

QVariant MyPlugin::greet_via_host(QString who) {
    return host_.callFunc(target_, { who });              // one value, in order
}
```

A plugin's metadata JSON may declare the functions/plugins it requires before
it can initialize (`"dependencies": ["other"]`); `PluginManager` defers such a
plugin until those dependencies are satisfied. `load_directory()` retries
deferred plugins across passes, so the directory can be scanned in any order.

See `examples/hello_plugin` + `examples/cross_caller` (two plugins where one
calls the other) and `examples/plugin_host` (loads a directory and prints the
registered functions). The `Q_PLUGIN_METADATA` IID must be the literal
`"ai.mcp.qt.mcp-plugin/2.0"` (it matches `MCP_PLUGIN_IID`). Duplicate
signatures are skipped with a warning. Load plugins before use; unloading while
calls are in flight is not supported.

`examples/plugintester` is a standalone interactive binary for trying plugins
out. It scans a directory and accepts `list`, `load <name|path>`, `loadall`,
`unload <plugin-id>`, `loaded`, `help`, and `quit`. Loading honors declared
dependencies; `unload_plugin()` refuses to remove a plugin that another loaded
plugin depends on (and refuses pinned ones). The tester claims the log sink
with a plain stderr dumper, so SDK logging always goes to stderr and a logging
plugin cannot take it over.

```sh
./build/examples/plugintester/plugintester /path/to/plugins
```

#### Plugin-owned logging

A plugin can take over SDK diagnostics entirely: it claims the log sink and
then decides how each event is rendered and where it goes. `claimLogging` is
exclusive — the first successful claim wins, and afterwards neither another
plugin nor `set_log_sink()` can replace it. The core keeps a minimal
timestamped stderr sink as a fallback for before the claim and when no logging
plugin is present.

```cpp
void initialize(const mcp::HostInterface& host) override {
    // Claim last: this must be the final setup step.
    (void)host.claimLogging([](mcp::LogLevel level, std::string_view msg) {
        // The plugin owns presentation/destination (JSON, file, syslog, ...).
        write_json_line(level, msg);
    });
}
```

A claiming plugin is **pinned**: `PluginManager::unload_all()` will not detach
it, because the installed sink runs code from the plugin's library. See
`examples/log_plugin` for a JSON-lines implementation (writes to
`$MCP_LOG_FILE`, else stderr).

## Building

Requires a C++20 compiler and Qt 6.4+ (development packages for `Core`,
`Network`, `HttpServer`, and `Test`). On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-httpserver-dev
```

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### Build options

| Option                    | Default | Effect                                       |
| ------------------------- | ------- | -------------------------------------------- |
| `MCP_BUILD_TESTS`         | ON      | Build the QtTest test suite.                 |
| `MCP_BUILD_EXAMPLES`      | ON      | Build the example servers/clients.           |
| `MCP_WARNINGS_AS_ERRORS`  | ON*     | Promote warnings to errors (top-level only). |
| `MCP_ENABLE_HTTP`         | ON      | Build the `mcp::http` target (Streamable HTTP transport; needs Qt Network + Qt HttpServer). |
| `MCP_ENABLE_ASAN`         | OFF     | `-fsanitize=address,undefined`.              |
| `MCP_ENABLE_TSAN`         | OFF     | `-fsanitize=thread`.                         |
| `MCP_ENABLE_COVERAGE`     | OFF     | `--coverage` for gcov/llvm-cov.              |

The library targets **Qt 6.4 or newer** (`Qt6::Core`, `Qt6::Network`,
`Qt6::HttpServer` for HTTP, `Qt6::Test` for the test suite); no bundled
third-party dependencies are downloaded at configure time.

\* defaults to ON when this is the top-level project; OFF when consumed via `add_subdirectory`.

## Consuming the library

### CMake `find_package`

After `cmake --install build`, downstream projects can do:

```cmake
find_package(mcp REQUIRED)
target_link_libraries(my_app PRIVATE mcp::mcp)   # stdio + core: QtCore only
```

The core `mcp::mcp` target depends only on `Qt6::Core` (plus `Qt6::Network`).
To use the Streamable HTTP transport (`HttpServerHost` /
`HttpClientTransport`), link the separate `mcp::http` target instead — it pulls
in `mcp::mcp` plus `Qt6::Network` and `Qt6::HttpServer`:

```cmake
target_link_libraries(my_app PRIVATE mcp::http)  # HTTP transport (needs MCP_ENABLE_HTTP)
```

### `add_subdirectory` / FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(mcp
    GIT_REPOSITORY https://github.com/Neumann-Labs/mcp-cpp.git
    GIT_TAG        main)
FetchContent_MakeAvailable(mcp)

target_link_libraries(my_app PRIVATE mcp::mcp)   # or mcp::http for HTTP
```

## Architecture in 60 seconds

```
                  +-----------------+
   application -> | Server / Client | <- public façade
                  +-----------------+
                          |
                  +-----------------+
                  |     Session     | <- JSON-RPC dispatch +
                  +-----------------+    request/response correlation
                          |
                  +-----------------+
                  |   Transport     | <- byte shovel (StdioTransport,
                  +-----------------+    Streamable HTTP, in-memory pair)
                          |
                          v
                       wire
```

- **Transport** is a small abstract interface: it shovels frames; the Session
  parses them.
- **Session** owns the request-id generator, the pending-request map, the
  request/notification dispatch table, and the timeout sweep. It's the same
  class on both sides of the wire — Server and Client just register different
  handlers and call different convenience methods.
- **Server / Client** are thin façades: they translate typed C++ values into
  JSON-RPC frames and back, expose `tool()` / `resource()` / `prompt()` /
  `sample()` / `list_tools()` / etc., and own one Session each.
- **PluginManager** sits beside a Server: it loads `PluginInterface` modules
  via `QPluginLoader` and calls `Server::tool()` once per plugin command, so
  plugins become ordinary tools with no special-casing in the Session.

## Threading

- The transport reads on its own thread and invokes the Session's
  on-message callback.
- Inbound *requests* are dispatched to a detached worker thread so user
  handlers can themselves issue further requests on the same Session
  (e.g. `server.sample(...).get()` from inside a tool handler).
  `Session::close()` waits for those workers to finish before tearing
  members down.
- Inbound *notifications* and *responses* run on the read thread directly.
- `send_request` / `send_notification` are safe to call from any thread.

## License

Apache 2.0 — see [LICENSE](LICENSE).
