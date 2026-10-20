// SPDX-License-Identifier: AGPL-3.0-only
//
// Minimal MCP client. Spawns a server (with any server arguments) as a child
// process, drives an end-to-end conversation, and prints results:
//
//     ./examples/simple_client/simple_client \
//         ./build/server/mcp_stdio_server \
//         ./build/plugins/calculator/calculator_plugin.so

#include "mcp.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

extern char** environ;

namespace {

struct Subprocess {
    pid_t pid = -1;
    int read_fd = -1;
    int write_fd = -1;
    ~Subprocess() {
        if (write_fd >= 0)
            ::close(write_fd);
        if (read_fd >= 0)
            ::close(read_fd);
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }
};

std::unique_ptr<Subprocess> spawn(char** argv) {
    int in_pipe[2];
    int out_pipe[2];
    if (::pipe(in_pipe) != 0)
        return nullptr;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return nullptr;
    }
    posix_spawn_file_actions_t act;
    posix_spawn_file_actions_init(&act);
    posix_spawn_file_actions_adddup2(&act, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&act, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&act, in_pipe[0]);
    posix_spawn_file_actions_addclose(&act, in_pipe[1]);
    posix_spawn_file_actions_addclose(&act, out_pipe[0]);
    posix_spawn_file_actions_addclose(&act, out_pipe[1]);

    std::vector<char*> child_argv;
    for (char** p = argv; *p != nullptr; ++p) {
        child_argv.push_back(*p);
    }
    child_argv.push_back(nullptr);

    pid_t pid = -1;
    int rc =
        ::posix_spawn(&pid, argv[0], &act, nullptr, child_argv.data(), environ);
    posix_spawn_file_actions_destroy(&act);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return nullptr;
    }
    auto sp = std::make_unique<Subprocess>();
    sp->pid = pid;
    sp->read_fd = out_pipe[0];
    sp->write_fd = in_pipe[1];
    return sp;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-server-binary>\n", argv[0]);
        return 1;
    }

    auto sp = spawn(argv + 1);
    if (!sp) {
        std::fprintf(stderr, "failed to spawn %s\n", argv[1]);
        return 1;
    }

    mcp::StdioTransport::Options topts{};
    topts.read_fd = sp->read_fd;
    topts.write_fd = sp->write_fd;
    auto transport = std::make_unique<mcp::StdioTransport>(topts);

    mcp::Client client{mcp::Implementation{
        .name = "simple_client",
        .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    auto info = client.initialize().get();
    std::cout << "connected to " << info.server_info.name << " "
              << info.server_info.version << "\n";
    if (info.instructions.has_value()) {
        std::cout << "instructions: " << *info.instructions << "\n";
    }

    auto tools = client.list_tools().get();
    std::cout << "\navailable tools:\n";
    for (const auto& t : tools.tools) {
        std::cout << "  - " << t.name;
        if (t.description.has_value())
            std::cout << " — " << *t.description;
        std::cout << "\n";
    }

    if (!tools.tools.empty()) {
        const auto& first = tools.tools[0].name;
        std::cout << "\ncalling " << first << "(a=2, b=3)...\n";
        auto out =
            client.call_tool(first, QJsonObject{{"a", 2}, {"b", 3}}).get();
        if (auto* text = std::get_if<mcp::TextContent>(&out.content[0])) {
            std::cout << "  -> " << text->text << "\n";
        }
    }

    client.disconnect();
    return 0;
}
