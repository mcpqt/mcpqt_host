// SPDX-License-Identifier: AGPL-3.0-only
//
// Shared helpers for the QtTest-based test suite.
//
// The SDK's JSON type is QJsonValue (alias mcp::Json). QtTest's QCOMPARE
// requires both operands to have the *same* type.
// The `jv()` factories below make the right-hand side of a comparison an
// unambiguous QJsonValue so tests can read naturally:
//
//   QCOMPARE(j["name"], mcp::test::jv("calc"));   // string
//   QCOMPARE(j["n"],    mcp::test::jv(3));        // int
//   QCOMPARE(j["ok"],   mcp::test::jv(true));     // bool
//
// For values that are inherently a container, prefer the typed accessors and
// compare those instead, e.g. QCOMPARE(j["xs"].toArray(), QJsonArray{...}).

#pragma once

#include "json.h"

#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdint>
#include <string>

namespace mcp::test {

inline QJsonValue jv(bool v) {
    return QJsonValue(v);
}
inline QJsonValue jv(int v) {
    return QJsonValue(v);
}
inline QJsonValue jv(qint64 v) {
    return QJsonValue(v);
}
inline QJsonValue jv(double v) {
    return QJsonValue(v);
}
inline QJsonValue jv(const char* v) {
    return QJsonValue(QString::fromUtf8(v));
}
inline QJsonValue jv(const QString& v) {
    return QJsonValue(v);
}

/// True when `v` is an object that contains `key`.
inline bool has_key(const QJsonValue& v, const char* key) {
    return v.isObject() && v.toObject().contains(QString::fromUtf8(key));
}

/// Object/array equality that sidesteps QJsonValue's ordering-independent
/// assumptions when comparing whole documents.
inline bool json_equal(const QJsonValue& a, const QJsonValue& b) {
    return a == b;
}

}  // namespace mcp::test
