// SPDX-License-Identifier: AGPL-3.0-only
// SPDX-License-Identifier: Apache-2.0
//
// PluginInterface implementation: meta-object-driven publication of public
// slots as host-registered functions.

#include "plugin.hpp"

#include <QMetaMethod>
#include <QMetaObject>

#include <string>

namespace mcp {

PluginInterface::PluginInterface(QObject* parent) : QObject(parent) {}

void PluginInterface::registerPublicSlots() {
    const QMetaObject* mo     = metaObject();
    const QString      prefix = QString::fromStdString(id()) + ".";

    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.methodType() != QMetaMethod::Slot ||
            method.access() != QMetaMethod::Public) {
            continue;
        }

        // Hand every public slot to the host. The host is the sole authority on
        // which slots are acceptable functions (return type, arity, parameter
        // types, inherited methods, duplicates); the plugin does not filter or
        // reject anything here.
        if (host_.regFunc) {
            const QString signature =
                prefix + QString::fromLatin1(method.methodSignature());
            (void)host_.regFunc(signature);
        }
    }
}

}  // namespace mcp