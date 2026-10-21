// SPDX-License-Identifier: AGPL-3.0-only
//
// End-to-end test that spawns `mcp_stdio_server` with the calculator plugin
// as a real subprocess and drives it via pipes — proving the server and
// plugin work across a process boundary using real stdio framing. The server
// and plugin paths are baked into test_binary_paths.h by CMake.

#include "client.h"
#include "error.h"
#include "protocol.h"
#include "stdio_transport.h"

#if __has_include("test_binary_paths.h")
#include "test_binary_paths.h"
#endif

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

extern char** environ;

namespace {

using namespace std::chrono_literals;

struct Subprocess {
    pid_t pid = -1;
    int read_fd = -1;   // server → us
    int write_fd = -1;  // us → server

    ~Subprocess() {
        if (write_fd >= 0)
            ::close(write_fd);
        if (read_fd >= 0)
            ::close(read_fd);
        if (pid > 0) {
            int status = 0;
            ::kill(pid, SIGTERM);
            ::waitpid(pid, &status, 0);
        }
    }
};

// posix_spawn the stdio server with the calculator plugin, returning pipes.
std::unique_ptr<Subprocess> spawn_calculator(void) {
#if !defined(MCP_TEST_STDIO_SERVER) || !defined(MCP_TEST_CALCULATOR_PLUGIN)
    return nullptr;
#else
    const char* path = MCP_TEST_STDIO_SERVER;
    const char* plugin = MCP_TEST_CALCULATOR_PLUGIN;

    int in_pipe[2];   // client writes to in_pipe[1], server reads in_pipe[0]
    int out_pipe[2];  // server writes to out_pipe[1], client reads out_pipe[0]
    if (::pipe(in_pipe) != 0)
        return nullptr;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return nullptr;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // child stdin = in_pipe[0]
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    // child stdout = out_pipe[1]
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    // close the unused pipe ends in the child
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    pid_t pid = -1;
    char* argv[] = {const_cast<char*>(path), const_cast<char*>(plugin),
                    nullptr};
    int rc = ::posix_spawn(&pid, path, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    // Close child-only ends in the parent.
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
#endif
}

}  // namespace

class CalculatorSubprocess : public QObject {
    Q_OBJECT
private slots:
    void EndToEnd(void);
};

void CalculatorSubprocess::EndToEnd(void) {
#if !defined(MCP_TEST_STDIO_SERVER) || !defined(MCP_TEST_CALCULATOR_PLUGIN)
    QSKIP("stdio server or calculator plugin not built; skipping");
#else
    auto sp = spawn_calculator();
    QVERIFY(sp != nullptr);

    mcp::StdioTransport::Options opts{};
    opts.read_fd = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;  // Subprocess dtor closes the parent ends
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{mcp::Implementation{
        .name = "subprocess-tester",
        .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    auto info = client.initialize().get();
    QCOMPARE(info.protocol_version, mcp::kLatestProtocolVersion);
    QCOMPARE(info.server_info.name, QString{"mcp-server"});
    QVERIFY(info.capabilities.tools.has_value());

    auto tools = client.list_tools().get();
    QVERIFY(tools.tools.size() >= 4u);

    auto add_out =
        client.call_tool("add", QJsonObject{{"a", 6.0}, {"b", 7.0}}).get();
    QVERIFY(!add_out.is_error.value_or(false));
    QCOMPARE(add_out.content.size(), std::size_t{1});
    QVERIFY(std::get<mcp::TextContent>(add_out.content[0]).text.contains("13"));

    auto div_out =
        client.call_tool("divide", QJsonObject{{"a", 1.0}, {"b", 0.0}}).get();
    QVERIFY(div_out.is_error.value_or(false));

    client.disconnect();
#endif
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        CalculatorSubprocess suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "calculator_subprocess_test.moc"
#include <QString>
