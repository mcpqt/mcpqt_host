// SPDX-License-Identifier: AGPL-3.0-only
#include "log.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CapturedLine {
    mcp::LogLevel level;
    QString message;
};

}  // namespace

class LogTest : public QObject {
    Q_OBJECT
private slots:
    void init(void);
    void cleanup(void);

    void EmitsAtAllLevels(void);
    void LevelFilterDropsBelowThreshold(void);
    void OffSilencesEverything(void);
    void ConcurrentEmittersAreSerialized(void);
    void ResettingSinkRestoresDefault(void);

private:
    QList<CapturedLine> drain(void) {
        std::lock_guard<std::mutex> lk(m_mu);
        return std::move(m_captured);
    }

    std::mutex m_mu;
    QList<CapturedLine> m_captured;
    mcp::LogLevel m_prev_level{mcp::LogLevel::info};
};

class LogLevelStrings : public QObject {
    Q_OBJECT
private slots:
    void MapsAllValues(void);
};

void LogTest::init(void) {
    m_prev_level = mcp::log_level();
    mcp::set_log_level(mcp::LogLevel::trace);
    (void)mcp::set_log_sink([this](mcp::LogLevel lvl, QString msg) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_captured.push_back({lvl, QString(msg)});
    });
}

void LogTest::cleanup(void) {
    (void)mcp::set_log_sink(nullptr);  // reset to default sink
    mcp::set_log_level(m_prev_level);
}

void LogTest::EmitsAtAllLevels(void) {
    MCP_LOG_TRACE("t");
    MCP_LOG_DEBUG("d");
    MCP_LOG_INFO("i");
    MCP_LOG_WARN("w");
    MCP_LOG_ERROR("e");

    auto lines = drain();
    QCOMPARE(lines.size(), std::size_t{5});
    QCOMPARE(lines[0].level, mcp::LogLevel::trace);
    QCOMPARE(lines[0].message, QString("t"));
    QCOMPARE(lines[4].level, mcp::LogLevel::error);
}

void LogTest::LevelFilterDropsBelowThreshold(void) {
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

void LogTest::OffSilencesEverything(void) {
    mcp::set_log_level(mcp::LogLevel::off);
    MCP_LOG_ERROR("nope");
    QVERIFY(drain().empty());
}

void LogTest::ConcurrentEmittersAreSerialized(void) {
    constexpr int kThreads = 8;
    constexpr int kPer = 200;

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t]() {
            for (int i = 0; i < kPer; ++i) {
                MCP_LOG_INFO(QString("thread ") + QString::number(t));
            }
        });
    }
    for (auto& th : ts)
        th.join();

    QCOMPARE(drain().size(), static_cast<std::size_t>(kThreads * kPer));
}

void LogTest::ResettingSinkRestoresDefault(void) {
    (void)mcp::set_log_sink(nullptr);  // restore default
    // Default writes to stderr; just verify the call doesn't crash.
    MCP_LOG_INFO("after-reset");
}

void LogLevelStrings::MapsAllValues(void) {
    QCOMPARE(mcp::to_string(mcp::LogLevel::trace), "TRACE");
    QCOMPARE(mcp::to_string(mcp::LogLevel::debug), "DEBUG");
    QCOMPARE(mcp::to_string(mcp::LogLevel::info), "INFO");
    QCOMPARE(mcp::to_string(mcp::LogLevel::warn), "WARN");
    QCOMPARE(mcp::to_string(mcp::LogLevel::error), "ERROR");
    QCOMPARE(mcp::to_string(mcp::LogLevel::off), "OFF");
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
#include <QList>
#include <QString>
