// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginInterface implementation: meta-object-driven publication of public
// slots as host-registered functions.

#include "plugin.hpp"

#include "log.hpp"

#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>

#include <cstddef>
#include <string>

namespace mcp {

PluginInterface::PluginInterface(QObject* parent) : QObject(parent) {}

void PluginInterface::registerPublicSlots() {
    const QMetaObject* mo     = metaObject();
    const QString      prefix = QString::fromStdString(id()) + ".";
    std::size_t        published = 0;
    bool               saw_slot  = false;

    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.methodType() != QMetaMethod::Slot) {
            continue;
        }
        if (method.access() != QMetaMethod::Public) {
            continue;
        }
        saw_slot = true;

        // Return type: void or QVariant. Anything else has no defined way to
        // be handed back through HostInterface::callFunc.
        const QMetaType returnType = method.returnMetaType();
        if (returnType != QMetaType::fromType<void>() &&
            returnType != QMetaType::fromType<QVariant>()) {
            MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                         "): skipping slot '" +
                         method.methodSignature().toStdString() +
                         "' — return type must be void or QVariant");
            continue;
        }

        // Parameters: QMetaMethod::invoke carries at most 10.
        if (method.parameterCount() > Q_METAMETHOD_INVOKE_MAX_ARGS) {
            MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                         "): skipping slot '" +
                         method.methodSignature().toStdString() +
                         "' — more than 10 parameters");
            continue;
        }

        bool params_ok = true;
        for (int p = 0; p < method.parameterCount(); ++p) {
            if (!method.parameterMetaType(p).isValid()) {
                MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                             "): skipping slot '" +
                             method.methodSignature().toStdString() +
                             "' — parameter " + std::to_string(p) +
                             " has no registered QMetaType");
                params_ok = false;
                break;
            }
        }
        if (!params_ok) {
            continue;
        }

        const QString signature =
            prefix + QString::fromLatin1(method.methodSignature());
        if (host_.regFunc && !host_.regFunc(signature)) {
            MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                         "): host refused to publish '" +
                         signature.toStdString() + "'");
        } else {
            ++published;
        }
    }

    if (saw_slot && published == 0) {
        MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                     "): registerPublicSlots() found no publishable slot");
    }
}

}  // namespace mcp