// SPDX-License-Identifier: AGPL-3.0-only
#include "log.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <QMutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

  struct CapturedLine
  {
    mcp::LogLevel level;
    QString message;
  };

  QMutex g_capture_mu;
  QList<CapturedLine> g_captured;

  QList<CapturedLine> drain(void)
  {
    QMutexLocker lk(&g_capture_mu);
    return std::move(g_captured);
  }

  // The sink can be claimed only once per process, so install it lazily on the
  // first test and keep capturing into the file-scope buffer thereafter.
  void install_capture_sink(void)
  {
    static const bool installed = mcp::claim_log_sink(
        [](mcp::LogLevel lvl, const QString& msg) {
          QMutexLocker lk(&g_capture_mu);
          g_captured.push_back({ lvl, msg });
        });
    Q_UNUSED(installed);
  }

} // namespace

class LogTest : public QObject
{
  Q_OBJECT
private slots:
  void init(void);
  void cleanup(void);

  void EmitsAtAllLevels(void);
  void ConcurrentEmittersAreSerialized(void);
};

void LogTest::init(void)
{
  install_capture_sink();
  (void)drain();
}

void LogTest::cleanup(void)
{
}

void LogTest::EmitsAtAllLevels(void)
{
  MCP_LOG_TRACE("t");
  MCP_LOG_DEBUG("d");
  MCP_LOG_INFO("i");
  MCP_LOG_WARN("w");
  MCP_LOG_ERROR("e");

  auto lines = drain();
  QCOMPARE(lines.size(), std::size_t{ 5 });
  QCOMPARE(lines[0].level, mcp::LogLevel::trace);
  QCOMPARE(lines[0].message, QString("t"));
  QCOMPARE(lines[4].level, mcp::LogLevel::error);
}

void LogTest::ConcurrentEmittersAreSerialized(void)
{
  constexpr int kThreads = 8;
  constexpr int kPer = 200;

  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for(int t = 0; t < kThreads; ++t)
  {
    ts.emplace_back([t]() {
      for(int i = 0; i < kPer; ++i)
      {
        MCP_LOG_INFO(QString("thread ") + QString::number(t));
      }
    });
  }
  for(auto& th : ts)
    th.join();

  QCOMPARE(drain().size(), static_cast<std::size_t>(kThreads * kPer));
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  int status = 0;
  {
    LogTest suite;
    status |= QTest::qExec(&suite, argc, argv);
  }
  return status;
}

#include "log_test.moc"
#include <QList>
#include <QString>
