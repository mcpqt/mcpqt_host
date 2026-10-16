// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
#include "mcp/log.hpp"

#include "qt_test_support.hpp"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CapturedLine {
    mcp::LogLevel level;
    std::string   message;
};

}  // namespace

class LogTest : public QObject {
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void EmitsAtAllLevels();
    void LevelFilterDropsBelowThreshold();
    void OffSilencesEverything();
    void ConcurrentEmittersAreSerialized();
    void ResettingSinkRestoresDefault();

private:
    std::vector<CapturedLine> drain() {
        std::lock_guard<std::mutex> lk(mu_);
        return std::move(captured_);
    }

    std::mutex                mu_;
    std::vector<CapturedLine> captured_;
    mcp::LogLevel             prev_level_{mcp::LogLevel::info};
};

class LogLevelStrings : public QObject {
    Q_OBJECT
private slots:
    void MapsAllValues();
};

void LogTest::init() {
    prev_level_ = mcp::log_level();
    mcp::set_log_level(mcp::LogLevel::trace);
    mcp::set_log_sink([this](mcp::LogLevel lvl, std::string_view msg) {
        std::lock_guard<std::mutex> lk(mu_);
        captured_.push_back({lvl, std::string(msg)});
    });
}

void LogTest::cleanup() {
    mcp::set_log_sink(nullptr);  // reset to default sink
    mcp::set_log_level(prev_level_);
}

void LogTest::EmitsAtAllLevels() {
    MCP_LOG_TRACE("t");
    MCP_LOG_DEBUG("d");
    MCP_LOG_INFO("i");
    MCP_LOG_WARN("w");
    MCP_LOG_ERROR("e");

    auto lines = drain();
    QCOMPARE(lines.size(), std::size_t{5});
    QCOMPARE(lines[0].level, mcp::LogLevel::trace);
    QCOMPARE(lines[0].message, std::string("t"));
    QCOMPARE(lines[4].level, mcp::LogLevel::error);
}

void LogTest::LevelFilterDropsBelowThreshold() {
    mcp::set_log_level(mcp::LogLevel::warn);
    MCP_LOG_TRACE("t");
    MCP_LOG_DEBUG("d");
    MCP_LOG_INFO("i");
    MCP_LOG_WARN("w");
    MCP_LOG_ERROR("e");

    auto lines = drain();
    QCOMPARE(lines.size(), std::size_t{2});
    QCOMPARE(lines[0].level, mcp::LogLevel::warn);
    QCOMPARE(lines[1].level, mcp::LogLevel::error);
}

void LogTest::OffSilencesEverything() {
    mcp::set_log_level(mcp::LogLevel::off);
    MCP_LOG_ERROR("nope");
    QVERIFY(drain().empty());
}

void LogTest::ConcurrentEmittersAreSerialized() {
    constexpr int kThreads = 8;
    constexpr int kPer     = 200;

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t]() {
            for (int i = 0; i < kPer; ++i) {
                MCP_LOG_INFO(std::string("thread ") + std::to_string(t));
            }
        });
    }
    for (auto& th : ts) th.join();

    QCOMPARE(drain().size(), static_cast<std::size_t>(kThreads * kPer));
}

void LogTest::ResettingSinkRestoresDefault() {
    mcp::set_log_sink(nullptr);  // restore default
    // Default writes to stderr; just verify the call doesn't crash.
    MCP_LOG_INFO("after-reset");
}

void LogLevelStrings::MapsAllValues() {
    QCOMPARE(mcp::to_string(mcp::LogLevel::trace), "TRACE");
    QCOMPARE(mcp::to_string(mcp::LogLevel::debug), "DEBUG");
    QCOMPARE(mcp::to_string(mcp::LogLevel::info),  "INFO");
    QCOMPARE(mcp::to_string(mcp::LogLevel::warn),  "WARN");
    QCOMPARE(mcp::to_string(mcp::LogLevel::error), "ERROR");
    QCOMPARE(mcp::to_string(mcp::LogLevel::off),   "OFF");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        LogTest suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        LogLevelStrings suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "log_test.moc"
