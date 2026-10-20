// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Minimal Streamable-HTTP interop server.
//
// Exists to verify real-world MCP clients (Claude Code, VS Code,
// Cursor, MCP Inspector) against this SDK's `HttpServerHost`:
// initialize handshake, `Mcp-Session-Id` round-trip, tools/list,
// tools/call, long-lived sessions, DELETE teardown, and bearer-token
// enforcement — with a single `echo` tool as the payload.
//
//   ./http_echo_server [host] [port] [path]
//
// Defaults to 127.0.0.1:8848 /mcp. Set MCP_TOKEN in the environment
// to require `Authorization: Bearer <token>` on every request.
//
//   claude mcp add --transport http echo http://127.0.0.1:8848/mcp \
//       --header "Authorization: Bearer $MCP_TOKEN"

#include "http_server_host.h"
#include "log.h"
#include "mcp.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::atomic_flag g_shutdown_requested = ATOMIC_FLAG_INIT;

extern "C" void on_signal(int) {
    g_shutdown_requested.test_and_set(std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    mcp::set_log_level(mcp::LogLevel::debug);

    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    int         port = argc > 2 ? std::atoi(argv[2]) : 8848;
    std::string path = argc > 3 ? argv[3] : "/mcp";

    mcp::HttpServerHost::Options opts;
    opts.host = host;
    opts.port = port;
    opts.path = path;

    if (const char* token = std::getenv("MCP_TOKEN"); token && *token) {
        opts.bearer_validator =
            [expected = std::string(token)](std::string_view presented) {
                using Outcome = mcp::HttpServerHost::Options::BearerOutcome;
                using Status  = mcp::HttpServerHost::Options::BearerStatus;
                Outcome out;
                out.status = presented == expected ? Status::allow
                                                   : Status::invalid_token;
                return out;
            };
        std::fprintf(stderr, "[echo] bearer auth ON\n");
    }

    mcp::HttpServerHost host_obj{
        mcp::Implementation{.name = "http-echo", .version = "0.1.0"},
        opts,
        [](mcp::Server& s) {
            s.tool(
                "echo",
                QJsonObject{
                    {"type", "object"},
                    {"properties",
                     QJsonObject{
                         {"text",
                          QJsonObject{
                              {"type", "string"},
                              {"description", "text to echo back"}}}}},
                    {"required", QJsonArray{"text"}},
                },
                [](const mcp::Json& args) -> mcp::CallToolResult {
                    std::string text;
                    const QJsonValue t = args.toObject().value("text");
                    if (t.isString()) text = t.toString().toStdString();
                    return {
                        .content = {mcp::TextContent{
                            .text = "echo: " + text}},
                    };
                });
        },
    };

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    host_obj.start();
    std::fprintf(stderr, "[echo] listening on http://%s:%d%s\n", host.c_str(),
                 host_obj.port(), path.c_str());

    while (!g_shutdown_requested.test(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    std::fprintf(stderr, "[echo] shutting down (%zu active sessions)\n",
                 host_obj.active_sessions());
    host_obj.stop();
    return 0;
}
