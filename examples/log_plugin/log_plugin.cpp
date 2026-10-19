// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
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

#include "plugin.hpp"

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
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "log_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit LogPlugin(QObject* parent = nullptr) : mcp::PluginInterface(parent) {}

    std::string id() const override { return "logging"; }
    std::string version() const override { return "1.0.0"; }
    std::string description() const override {
        return "Owns SDK diagnostic logging; renders events as JSON lines.";
    }

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;

        if (const char* path = std::getenv("MCP_LOG_FILE"); path != nullptr && *path != '\0') {
            file_ = std::fopen(path, "a");
        }
        output_ = file_ != nullptr ? file_ : stderr;

        // Claim last: this must be the final step so a setup failure above
        // cannot leave a claimed sink pointing at a half-initialised plugin.
        const bool ok = host_.claimLogging(
            [this](mcp::LogLevel level, std::string_view msg) { write(level, msg); });
        if (!ok) {
            qWarning("logging plugin: log sink already claimed; not taking over");
        } else {
            // Demonstrate that the plugin controls rendering: a structured,
            // plugin-authored line (not the core fallback format).
            mcp::log_message(mcp::LogLevel::info,
                             "logging plugin installed its JSON-lines sink");
        }
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
            output_ = stderr;
        }
    }

private:
    void write(mcp::LogLevel level, std::string_view msg) {
        const QJsonObject record{
            { QStringLiteral("level"), QString::fromLatin1(mcp::to_string(level).data()) },
            { QStringLiteral("msg"),
              QString::fromUtf8(msg.data(), static_cast<int>(msg.size())) },
        };
        const QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';

        std::lock_guard<std::mutex> lk(mu_);
        std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), output_);
        std::fflush(output_);
    }

    std::mutex mu_;
    std::FILE* file_   = nullptr;
    std::FILE* output_ = stderr;
};

}  // namespace

#include "log_plugin.moc"