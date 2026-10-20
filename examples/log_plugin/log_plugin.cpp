// SPDX-License-Identifier: AGPL-3.0-only
//
// Logging plugin (Scope A): it claims the process-wide diagnostic log sink and
// decides *how* received log events are rendered and where they go. The core's
// minimal fallback (plain timestamped stderr lines) is replaced by JSON-lines
// output here.
//
// Destination: $MCP_LOG_FILE if set (opened in append mode), else stderr.
//
// Once loaded, this plugin owns logging: no other plugin can replace the sink
// (the core rejects replacement after a claim), and the host pins this plugin
// so its sink code is never detached while in use.

#include "plugin.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QVariant>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>

namespace {

class LogPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "logging")
    Q_CLASSINFO("version", "1.0.0")
    Q_CLASSINFO("description",
                "Owns SDK diagnostic logging; renders events as JSON lines.")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "log_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit LogPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        m_host = host;

        if (const char* path = std::getenv("MCP_LOG_FILE");
            path != nullptr && *path != '\0') {
            m_file = std::fopen(path, "a");
        }
        m_output = m_file != nullptr ? m_file : stderr;

        // Claim last: this must be the final step so a setup failure above
        // cannot leave a claimed sink pointing at a half-initialised plugin.
        const bool ok = m_host.claimLogging(
            [this](mcp::LogLevel level, std::string_view msg) {
                write(level, msg);
            });
        if (!ok) {
            qWarning(
                "logging plugin: log sink already claimed; not taking over");
        } else {
            // Demonstrate that the plugin controls rendering: a structured,
            // plugin-authored line (not the core fallback format).
            mcp::log_message(mcp::LogLevel::info,
                             "logging plugin installed its JSON-lines sink");
        }
    }

    void shutdown(void) override {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_file != nullptr) {
            std::fclose(m_file);
            m_file = nullptr;
            m_output = stderr;
        }
    }

private:
    void write(mcp::LogLevel level, std::string_view msg) {
        const QJsonObject record{
            {QStringLiteral("level"),
             QString::fromLatin1(mcp::to_string(level).data())},
            {QStringLiteral("msg"),
             QString::fromUtf8(msg.data(), static_cast<int>(msg.size()))},
        };
        const QByteArray line =
            QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';

        std::lock_guard<std::mutex> lk(m_mu);
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()),
                    m_output);
        std::fflush(m_output);
    }

    std::mutex m_mu;
    std::FILE* m_file = nullptr;
    std::FILE* m_output = stderr;
};

}  // namespace

#include "log_plugin.moc"