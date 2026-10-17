// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end test that spawns the `mcp_notes_server` example as a real
// subprocess and drives it via pipes. Mirrors calculator_subprocess_test
// but exercises the SQLite/FTS5 demo: write → read → list → search →
// delete (no elicitation prompt for non-glob keys).

#include "client.hpp"
#include "error.hpp"
#include "protocol.hpp"
#include "stdio_transport.hpp"

#if __has_include("test_binary_paths.hpp")
#include "test_binary_paths.hpp"
#endif

#include "qt_test_support.hpp"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>

extern char** environ;

namespace {

namespace fs = std::filesystem;
using mcp::Json;
using namespace std::chrono_literals;

struct Subprocess {
    pid_t pid       = -1;
    int   read_fd   = -1;
    int   write_fd  = -1;

    ~Subprocess() {
        if (write_fd >= 0) ::close(write_fd);
        if (read_fd  >= 0) ::close(read_fd);
        if (pid > 0) {
            int status = 0;
            ::kill(pid, SIGTERM);
            ::waitpid(pid, &status, 0);
        }
    }
};

std::unique_ptr<Subprocess>
spawn_notes(const std::string& binary, const std::string& db_path) {
    int in_pipe[2];
    int out_pipe[2];
    if (::pipe(in_pipe)  != 0)  return nullptr;
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        return nullptr;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0],  STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    pid_t pid = -1;
    char  arg0[]    = "mcp_notes_server";
    char  flag_db[] = "--db";
    std::string db  = db_path;  // mutable storage for argv
    char* argv[] = {
        const_cast<char*>(binary.c_str()),
        flag_db,
        db.data(),
        nullptr,
    };
    const int rc = ::posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                                  argv, environ);
    (void)arg0;
    posix_spawn_file_actions_destroy(&actions);

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);

    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return nullptr;
    }

    auto sp = std::make_unique<Subprocess>();
    sp->pid       = pid;
    sp->read_fd   = out_pipe[0];
    sp->write_fd  = in_pipe[1];
    return sp;
}

}  // namespace

class NotesSubprocess : public QObject {
    Q_OBJECT
private slots:
    void EndToEnd();
    void DeleteWithoutConfirmIsAbortedOnElicitDecline();
};

void NotesSubprocess::EndToEnd() {
#if !defined(MCP_TEST_NOTES_SERVER)
    QSKIP("mcp_notes_server not built (no SQLite3); skipping");
#else
    const char* path = MCP_TEST_NOTES_SERVER;

    // Use a unique tempfile per test so we don't collide with state
    // from a prior run.
    const auto db_path = (fs::temp_directory_path()
                          / ("mcp-notes-test-" + std::to_string(::getpid()) + ".db"))
                          .string();
    std::error_code ec;
    fs::remove(db_path, ec);

    auto sp = spawn_notes(path, db_path);
    QVERIFY(sp != nullptr);

    mcp::StdioTransport::Options opts{};
    opts.read_fd  = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{ mcp::Implementation{
        .name = "notes-tester", .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    auto info = client.initialize().get();
    QCOMPARE(info.server_info.name, std::string{"mcp-notes"});
    QVERIFY(info.capabilities.tools.has_value());
    QVERIFY(info.capabilities.resources.has_value());

    auto tools = client.list_tools().get();
    QVERIFY(tools.tools.size() >= 6u);  // write/read/list/search/delete/info

    // write a note
    {
        auto out = client.call_tool("notes/write", QJsonObject{
            {"key",   "db.prod"},
            {"value", "postgres://prod.example/main"},
            {"tags",  "db,connection"},
        }).get();
        QVERIFY(!out.is_error.value_or(false));
    }

    // read it back
    {
        auto out = client.call_tool("notes/read",
                                     QJsonObject{{"key", "db.prod"}}).get();
        QVERIFY(!out.is_error.value_or(false));
        QCOMPARE(std::get<mcp::TextContent>(out.content[0]).text,
                 std::string{"postgres://prod.example/main"});
    }

    // list — should see exactly one note
    {
        auto out = client.call_tool("notes/list", QJsonObject{}).get();
        QVERIFY(!out.is_error.value_or(false));
        QVERIFY(out.structured_content.has_value());
        QCOMPARE((*out.structured_content)["notes"].toArray().size(),
                 std::size_t{1});
    }

    // search by content
    {
        auto out = client.call_tool("notes/search",
                                     QJsonObject{{"query", "postgres"}}).get();
        QVERIFY(!out.is_error.value_or(false));
        QVERIFY(out.structured_content.has_value());
        const auto matches = (*out.structured_content)["matches"];
        QCOMPARE(matches.toArray().size(), std::size_t{1});
        QVERIFY(matches[0]["snippet"].toString().toStdString().find("postgres")
                != std::string::npos);
    }

    // resource read
    {
        auto res = client.read_resource("notes://all").get();
        QCOMPARE(res.contents.size(), std::size_t{1});
        const auto& tc = std::get<mcp::TextResourceContents>(res.contents[0]);
        QVERIFY(tc.text.find("db.prod") != std::string::npos);
    }

    // delete with confirm=true (no elicitation needed)
    {
        auto out = client.call_tool("notes/delete", QJsonObject{
            {"key", "db.prod"}, {"confirm", true},
        }).get();
        QVERIFY(!out.is_error.value_or(false));
    }

    // info — count is back to zero
    {
        auto out = client.call_tool("notes/info", QJsonObject{}).get();
        QVERIFY(!out.is_error.value_or(false));
        QVERIFY(out.structured_content.has_value());
        QCOMPARE((*out.structured_content)["noteCount"].toInt(), 0);
    }

    client.disconnect();
    fs::remove(db_path, ec);
#endif
}

void NotesSubprocess::DeleteWithoutConfirmIsAbortedOnElicitDecline() {
#if !defined(MCP_TEST_NOTES_SERVER)
    QSKIP("mcp_notes_server not built; skipping");
#else
    // notes/delete with confirm omitted routes through elicitation;
    // declining the elicitation should leave the row in place.
    const char* path = MCP_TEST_NOTES_SERVER;

    const auto db_path = (fs::temp_directory_path()
                          / ("mcp-notes-elicit-" + std::to_string(::getpid()) + ".db"))
                          .string();
    std::error_code ec;
    fs::remove(db_path, ec);

    auto sp = spawn_notes(path, db_path);
    QVERIFY(sp != nullptr);

    mcp::StdioTransport::Options opts{};
    opts.read_fd  = sp->read_fd;
    opts.write_fd = sp->write_fd;
    opts.owns_fds = false;
    auto transport = std::make_unique<mcp::StdioTransport>(opts);

    mcp::Client client{ mcp::Implementation{
        .name = "notes-tester", .version = "0.1.0",
    }};
    client.connect(std::move(transport));

    // Decline whatever the server elicits.
    client.set_elicitation_handler(
        [](const mcp::ElicitRequestParams&) -> mcp::ElicitResult {
            return mcp::ElicitResult{.action = mcp::ElicitAction::decline};
        });

    (void)client.initialize().get();

    (void)client.call_tool("notes/write", QJsonObject{
        {"key", "k1"}, {"value", "v1"},
    }).get();

    auto del = client.call_tool("notes/delete",
                                  QJsonObject{{"key", "k1"}}).get();
    QVERIFY(del.is_error.value_or(false));
    QVERIFY(std::get<mcp::TextContent>(del.content[0]).text.find("cancelled")
            != std::string::npos);

    // Note should still be there.
    auto info = client.call_tool("notes/info", QJsonObject{}).get();
    QVERIFY(info.structured_content.has_value());
    QCOMPARE((*info.structured_content)["noteCount"].toInt(), 1);

    client.disconnect();
    fs::remove(db_path, ec);
#endif
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        NotesSubprocess suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "notes_subprocess_test.moc"
