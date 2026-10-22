// SPDX-License-Identifier: AGPL-3.0-only
//
// Test logging plugin (Scope A). Claims the process-wide log sink so the test
// can verify that (a) the plugin receives the core's log events and decides
// what to do with them, (b) other plugins/embedders cannot replace it once
// claimed, and (c) the host pins it against unloading.

#include "plugin.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <string>

class TestLogPlugin : public mcp::PluginInterface
{
  Q_OBJECT
  Q_CLASSINFO("id", "test-logger")
  Q_CLASSINFO("version", "0.1.0")
  Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "test_log_plugin.json")
  Q_INTERFACES(mcp::PluginInterface)

public:
  explicit TestLogPlugin(QObject* parent = nullptr)
      : mcp::PluginInterface(parent)
  {
  }

  void initialize(const mcp::HostInterface& host) override
  {
    m_host = host;
    registerPublicSlots();
    m_claimed
        = m_host
              .callFunc("bool mcp::Host::registerLogger(QString)",
                  { QStringLiteral("void TestLogPlugin::log(int,QString)") })
              .toBool();
  }
  void shutdown(void) override {}

public slots:
  // Sink invoked by the host after registerLogger.
  void log(int level, QString message)
  {
    (void)level;
    m_count.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(m_mu);
    m_last = message;
  }

  // Number of log events this plugin has received via its sink.
  int count(void) { return m_count.load(std::memory_order_relaxed); }

  // Whether the claim succeeded.
  bool claimed(void) { return m_claimed; }

  // The most recent message the sink received.
  QString last(void)
  {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_last;
  }

private:
  std::atomic<int> m_count{ 0 };
  bool m_claimed = false;
  std::mutex m_mu;
  QString m_last;
};

#include "test_log_plugin.moc"
