// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginInterface implementation: meta-object-driven publication of public
// slots as host-registered functions.

#include "plugin.h"

#include <QMetaClassInfo>
#include <QMetaMethod>
#include <QMetaObject>

#include <cstring>

namespace mcp {

PluginInterface::PluginInterface(QObject* parent) : QObject(parent) {}

QString plugin_class_info(const QMetaObject* meta_object, const char* key) {
    if (meta_object == nullptr || key == nullptr) {
        return {};
    }
    for (int i = 0; i < meta_object->classInfoCount(); ++i) {
        const QMetaClassInfo info = meta_object->classInfo(i);
        if (std::strcmp(info.name(), key) == 0) {
            return QString::fromLatin1(info.value());
        }
    }
    return {};
}

void PluginInterface::registerPublicSlots(void) {
    const QMetaObject* mo = metaObject();
    const QString class_name = QString::fromLatin1(mo->className());

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
        //
        // Signature form:
        //   "return_type class_name::function_name(param_types)"
        if (m_host.regFunc) {
            const QString signature =
                QString::fromLatin1(method.typeName()) + QLatin1Char(' ') +
                class_name + QStringLiteral("::") +
                QString::fromLatin1(method.methodSignature());
            (void)m_host.regFunc(signature);
        }
    }
}

}  // namespace mcp