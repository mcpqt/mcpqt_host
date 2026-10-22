// SPDX-License-Identifier: AGPL-3.0-only
//
// A plugin used by plugin_test. Built as a MODULE library the test loads at
// runtime. Its public slots are published by the host via meta-object
// reflection; signatures become addressable functions, e.g.
//   QString TestPlugin::echo(QString), double TestPlugin::add(double,double),
//   void TestPlugin::boom(), QString TestPlugin::dup(), void
//   TestPlugin::reset().

#include "plugin.h"

#include <QObject>
#include <QString>

#include <stdexcept>
#include <string>

class TestPlugin : public mcp::PluginInterface
{
  Q_OBJECT
  Q_CLASSINFO("id", "test-plugin")
  Q_CLASSINFO("version", "9.9.9")
  // IID must match MCP_PLUGIN_IID in <plugin.h>.
  Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "test_plugin.json")
  Q_INTERFACES(mcp::PluginInterface)

public:
  explicit TestPlugin(QObject* parent = nullptr) : mcp::PluginInterface(parent)
  {
  }

  void initialize(const mcp::HostInterface& host) override
  {
    m_host = host;
    registerPublicSlots();
  }
  void shutdown(void) override {}

public slots:
  // Natural return type, one typed QString parameter.
  QString echo(QString message) { return message; }

  // Typed numeric parameters and a natural double return.
  double add(double a, double b) { return a + b; }

  // void return, no parameters.
  void reset(void) {}

  // void return that throws; the host maps this to an error.
  void boom(void) { throw std::runtime_error("boom from plugin"); }

  // Natural QString return.
  QString dup(void) { return "dup"; }

  // Non-QVariant return types (carried via QGenericReturnArgument).
  int meaning(void) { return 42; }
  QString upper(QString text) { return text.toUpper(); }
};

#include "test_plugin.moc"
