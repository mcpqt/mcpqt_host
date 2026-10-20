// SPDX-License-Identifier: AGPL-3.0-only
//
// mcp_tester: a small interactive MCP client.
//
// Spawns an MCP server as a child process (argv[1] is the server binary; any
// following arguments are passed to it), connects over stdio, initializes the
// session, and then presents a simple command menu for inspecting and calling
// the server's tools.
//
//   ./mcp_tester /path/to/server [server args...]
//
// Commands (also see `help`):
//   list                 list the server's tools (name, description, schema)
//   call                 pick a tool and call it with JSON arguments, then
//                        display the returned content / structured result
//   info                 show server info, instructions, and capabilities
//   ping                 send a ping and wait for the response
//   help                 show this help
//   quit                 disconnect and exit

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

std::unique_ptr<Subprocess> spawn_server(char** argv) {
    int in_pipe[2];
    int out_pipe[2];
    if (::pipe(in_pipe) != 0) {
        return nullptr;
    }
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
    const int rc =
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

void print_help(void) {
    std::cout << "commands:\n"
              << "  list                 list the server's tools\n"
              << "  call                 call a tool with JSON arguments\n"
              << "  info                 show server info and instructions\n"
              << "  ping                 send a ping\n"
              << "  help                 show this help\n"
              << "  quit                 disconnect and exit\n";
}

void print_content(const mcp::ContentBlock& block) {
    if (const auto* text = std::get_if<mcp::TextContent>(&block)) {
        std::cout << text->text;
        return;
    }
    if (const auto* image = std::get_if<mcp::ImageContent>(&block)) {
        std::cout << "[image " << image->mime_type << ", " << image->data.size()
                  << " base64 bytes]";
        return;
    }
    if (const auto* audio = std::get_if<mcp::AudioContent>(&block)) {
        std::cout << "[audio " << audio->mime_type << ", " << audio->data.size()
                  << " base64 bytes]";
        return;
    }
    std::cout << mcp::dump(mcp::to_json(block));
}

void print_result(const mcp::CallToolResult& result) {
    if (result.is_error.value_or(false)) {
        std::cout << "error: ";
    }
    if (result.content.empty()) {
        std::cout << "(no content)";
    } else {
        for (std::size_t i = 0; i < result.content.size(); ++i) {
            if (i != 0) {
                std::cout << "\n  ";
            }
            print_content(result.content[i]);
        }
    }
    std::cout << "\n";
    if (result.structured_content.has_value()) {
        std::cout << "structured: " << mcp::dump(*result.structured_content)
                  << "\n";
    }
}

void list_tools(mcp::Client& client) {
    const mcp::ListToolsResult tools = client.list_tools().get();
    if (tools.tools.empty()) {
        std::cout << "(server exposes no tools)\n";
        return;
    }
    for (const mcp::Tool& tool : tools.tools) {
        std::cout << "  - " << tool.name;
        if (tool.description.has_value() && !tool.description->empty()) {
            std::cout << " — " << *tool.description;
        }
        std::cout << "\n";
    }
}

void show_info(const mcp::Client& client) {
    const std::optional<mcp::InitializeResult> info = client.server();
    if (!info.has_value()) {
        std::cout << "(not initialized)\n";
        return;
    }
    std::cout << "server: " << info->server_info.name << " "
              << info->server_info.version << "\n";
    if (info->instructions.has_value()) {
        std::cout << "instructions: " << *info->instructions << "\n";
    }
}

void call_tool(mcp::Client& client) {
    std::cout << "tool name: " << std::flush;
    std::string name;
    if (!std::getline(std::cin, name) || name.empty()) {
        std::cout << "no tool selected\n";
        return;
    }

    std::cout << "arguments JSON (blank for {}): " << std::flush;
    std::string args_text;
    if (!std::getline(std::cin, args_text)) {
        return;
    }

    QJsonValue args = QJsonObject{};
    if (!args_text.empty()) {
        try {
            args = mcp::parse_json(args_text);
        } catch (const std::exception& e) {
            std::cout << "invalid JSON: " << e.what() << "\n";
            return;
        }
        if (!args.isObject()) {
            std::cout << "arguments must be a JSON object\n";
            return;
        }
    }

    try {
        const mcp::CallToolResult result = client.call_tool(name, args).get();
        print_result(result);
    } catch (const std::exception& e) {
        std::cout << "call failed: " << e.what() << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <server-binary> [server-args...]\n",
                     argv[0]);
        return 1;
    }

    mcp::set_log_level(mcp::LogLevel::warn);

    std::unique_ptr<Subprocess> server = spawn_server(argv + 1);
    if (!server) {
        std::fprintf(stderr, "failed to spawn %s\n", argv[1]);
        return 1;
    }

    mcp::StdioTransport::Options opts;
    opts.read_fd = server->read_fd;
    opts.write_fd = server->write_fd;

    mcp::Client client{
        mcp::Implementation{.name = "mcp_tester", .version = "0.1.0"}};
    client.connect(std::make_unique<mcp::StdioTransport>(opts));

    try {
        const mcp::InitializeResult info = client.initialize().get();
        std::cout << "connected to " << info.server_info.name << " "
                  << info.server_info.version << "\n";
    } catch (const std::exception& e) {
        std::fprintf(stderr, "initialize failed: %s\n", e.what());
        return 1;
    }

    std::cout << "mcp_tester — server: " << argv[1] << "\n";
    print_help();

    std::string line;
    while (true) {
        std::cout << "mcp_tester> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        if (line.empty()) {
            continue;
        }

        if (line == "quit" || line == "exit") {
            break;
        } else if (line == "help" || line == "?") {
            print_help();
        } else if (line == "list" || line == "tools" || line == "ls") {
            try {
                list_tools(client);
            } catch (const std::exception& e) {
                std::cout << "list failed: " << e.what() << "\n";
            }
        } else if (line == "call") {
            call_tool(client);
        } else if (line == "info") {
            show_info(client);
        } else if (line == "ping") {
            try {
                client.ping().get();
                std::cout << "pong\n";
            } catch (const std::exception& e) {
                std::cout << "ping failed: " << e.what() << "\n";
            }
        } else {
            std::cout << "unknown command '" << line << "'; type help\n";
        }
    }

    client.disconnect();
    std::cout << "bye\n";
    return 0;
}