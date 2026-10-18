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
    const QMetaObject* mo       = metaObject();
    const QString      prefix   = QString::fromStdString(id()) + ".";
    std::size_t        published = 0;

    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.methodType() != QMetaMethod::Slot) {
            continue;
        }
        if (method.access() != QMetaMethod::Public) {
            continue;
        }
        // Uniform calling convention: exactly one QVariant in, QVariant out.
        if (method.parameterCount() != 1 ||
            method.parameterMetaType(0) != QMetaType::fromType<QVariant>()) {
            continue;
        }
        if (method.returnMetaType() != QMetaType::fromType<QVariant>()) {
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

    if (published == 0) {
        MCP_LOG_WARN(std::string("PluginInterface(") + id() +
                     "): registerPublicSlots() found no slot matching the "
                     "`QVariant (const QVariant&)` convention");
    }
}

}  // namespace mcp