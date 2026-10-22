// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginInterface implementation: meta-object-driven publication of public
// slots as host-registered functions.

#include "plugin.h"

#include <QMetaMethod>
#include <QMetaObject>

namespace mcp
{
  void PluginInterface::registerPublicSlots(void)
  {
    const QMetaObject* mo = metaObject();
    const QString class_name = QString::fromLatin1(mo->className());

    for(int i = 0; i < mo->methodCount(); ++i)
      if(const QMetaMethod method = mo->method(i);
         method.methodType() == QMetaMethod::Slot &&
         method.access() == QMetaMethod::Public &&
         m_host.regFunc)
      {
        const QString signature
            = QString::fromLatin1(method.typeName()) + ' '
              + class_name + "::" + method.methodSignature();
        m_host.regFunc(signature);
      }
  }
} // namespace mcp
