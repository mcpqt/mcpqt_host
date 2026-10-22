// SPDX-License-Identifier: AGPL-3.0-only
//
// Plugin ABI for the MCP host.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QtPlugin>

#include <functional>

namespace mcp
{
  struct HostInterface
  {
    std::function<QStringList()> getFuncs;
    std::function<QVariant(const QString& signature, const QVariantList& args)> callFunc;
    std::function<bool(const QString& signature)> regFunc;
    std::function<void(const QString& reason)> unloadSelf;
    /// Diagnostic logging. `level` is an mcp::LogLevel value (0=trace ..
    /// 4=error, 5=off). The host routes this through its global log sink.
    std::function<void(int level, const QString& message)> log;
  };

  class PluginInterface : public QObject
  {
  public:
    using QObject::QObject;
    PluginInterface(const PluginInterface&) = delete;
    PluginInterface& operator=(const PluginInterface&) = delete;
    ~PluginInterface(void) override = default;

    virtual void initialize(const HostInterface& host) = 0;
    virtual void shutdown(void) {}
  protected:
    void registerPublicSlots(void);
    HostInterface m_host;
  };

} // namespace mcp

/// Unique interface identifier by Q_PLUGIN_METADATA
#define MCP_PLUGIN_IID "ai.mcp.qt.plugin/1.0"

Q_DECLARE_INTERFACE(mcp::PluginInterface, MCP_PLUGIN_IID)
