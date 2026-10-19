// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// Test logging plugin (Scope A). Claims the process-wide log sink so the test
// can verify that (a) the plugin receives the core's log events and decides
// what to do with them, (b) other plugins/embedders cannot replace it once
// claimed, and (c) the host pins it against unloading.

#include "plugin.hpp"

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <string>

class TestLogPlugin : public mcp::PluginInterface {
    Q_OBJECT
    Q_CLASSINFO("id", "test-logger")
    Q_CLASSINFO("version", "0.1.0")
    Q_PLUGIN_METADATA(IID "ai.mcp.qt.mcp-plugin/2.0" FILE "test_log_plugin.json")
    Q_INTERFACES(mcp::PluginInterface)

public:
    explicit TestLogPlugin(QObject* parent = nullptr)
        : mcp::PluginInterface(parent) {}

    void initialize(const mcp::HostInterface& host) override {
        host_ = host;
        claimed_ = host_.claimLogging(
            [this](mcp::LogLevel, std::string_view msg) {
                count_.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(mu_);
                last_.assign(msg.data(), msg.size());
            });
        registerPublicSlots();
    }
    void shutdown() override {}

public slots:
    // Number of log events this plugin has received via its sink.
    int count() { return count_.load(std::memory_order_relaxed); }

    // Whether the claim succeeded.
    bool claimed() { return claimed_; }

    // The most recent message the sink received.
    QString last() {
        std::lock_guard<std::mutex> lk(mu_);
        return QString::fromStdString(last_);
    }

private:
    std::atomic<int> count_{0};
    bool             claimed_ = false;
    std::mutex       mu_;
    std::string      last_;
};

#include "test_log_plugin.moc"