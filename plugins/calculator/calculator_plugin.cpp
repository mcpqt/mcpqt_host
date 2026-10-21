// SPDX-License-Identifier: AGPL-3.0-only
//
// Calculator plugin: the arithmetic tools formerly provided by the
// calculator_server example.

#include "plugin.h"

#include <QObject>
#include <QString>

#include <stdexcept>
#include <string>

namespace
{

  class CalculatorPlugin : public mcp::PluginInterface
  {
    Q_OBJECT
    Q_CLASSINFO("id", "calculator")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description",
                "Arithmetic tools: add, subtract, multiply, divide.")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE
                          "calculator_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

  public:
    explicit CalculatorPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent)
    {
    }

    void initialize(const mcp::HostInterface& host) override
    {
      m_host = host;
      registerPublicSlots();
    }
    void shutdown(void) override {}

  public slots:
    double add(double a, double b) { return a + b; }
    double subtract(double a, double b) { return a - b; }
    double multiply(double a, double b) { return a * b; }

    double divide(double a, double b)
    {
      if(b == 0.0)
      {
        throw std::runtime_error("division by zero");
      }
      return a / b;
    }
  };

} // namespace

#include "calculator_plugin.moc"