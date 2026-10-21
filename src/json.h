// SPDX-License-Identifier: AGPL-3.0-only
//
// JSON codec layer.
//
// The wire types are ordinary C++ structs; JSON conversion is done by free
// functions rather than by a third-party JSON library. On the wire we use Qt's
// JSON value types (QJsonValue / QJsonObject / QJsonArray) directly, and the
// value types are Qt types (QString / QList).
//
// Encoding:
//   mcp::to_json(x)            -> QJsonValue
// Decoding:
//   mcp::from_json<T>(value)   -> T            (throws mcp::Error on shape
//   errors)
//
// Reading helpers enforce a single, consistent null-vs-absent policy:
//   require<T>(obj, key)                    required field; throws parse_error
//                                           if absent
//   take_optional<T>(obj, key, out)         set `out` only if present/non-null
//   put_optional(obj, key, value)           insert only if the optional is
//   engaged take_optional_string(obj, key, out)     QString field: null when
//   absent put_optional_string(obj, key, value)    insert only when value is
//   not null

#pragma once

#include "error.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>
#include <utility>

namespace mcp {

// --------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------

inline QJsonValue to_json(std::nullptr_t) {
    return QJsonValue(QJsonValue::Null);
}
inline QJsonValue to_json(const QJsonValue&  v) { return v; }
inline QJsonValue to_json(const QJsonObject& v) { return v; }
inline QJsonValue to_json(const QJsonArray&  v) { return v; }
inline QJsonValue to_json(bool v) { return QJsonValue(v); }
inline QJsonValue to_json(int v) { return QJsonValue(v); }
inline QJsonValue to_json(std::int64_t v) { return QJsonValue(static_cast<qint64>(v)); }
inline QJsonValue to_json(double v) { return QJsonValue(v); }
inline QJsonValue to_json(const QString& v) { return QJsonValue(v); }
inline QJsonValue to_json(QStringView v) { return QJsonValue(v.toString()); }
inline QJsonValue to_json(const char* v) { return QJsonValue(QString::fromUtf8(v)); }

inline QJsonValue to_json(std::uint64_t v) {
    // QJsonValue stores integers as qint64. Reject rather than silently misrepresent value.
    if (v > INT64_MAX) {
        throw Error(
            error_code::internal_error,
            QStringLiteral("integer value too large for a JSON number"));
    }
    return QJsonValue(static_cast<qint64>(v));
}

template <class T> QJsonValue to_json(const std::optional<T>& v) {
    if (!v.has_value()) { return QJsonValue(QJsonValue::Null); }
    return to_json(*v);
}

template <class T> QJsonValue to_json(const QList<T>& v) {
    QJsonArray arr;
    for (const auto& elem : v) {
        arr.append(to_json(elem));
    }
    return arr;
}

// --------------------------------------------------------------------------
// Decoding
// --------------------------------------------------------------------------
//
// The two-argument `from_json(value, out)` overloads do the work; the
// single-argument `from_json<T>(value)` is a thin convenience wrapper. The
// scalar/container overloads are declared *before* the wrapper so ordinary
// lookup (at the wrapper's definition point) plus ADL (at instantiation) find
// the right overload for both built-in and wire-struct types.

inline void from_json(const QJsonValue& v, QJsonValue& out) {
    out = v;
}

inline void from_json(const QJsonValue& v, bool& out) {
    if (!v.isBool()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a boolean"));
    }
    out = v.toBool();
}

inline void from_json(const QJsonValue& v, std::int64_t& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected an integer"));
    }
    const double d = v.toDouble();
    const qint64 i = v.toInteger();
    if (static_cast<double>(i) != d) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected an integer"));
    }
    out = static_cast<std::int64_t>(i);
}

inline void from_json(const QJsonValue& v, int& out) {
    std::int64_t wide{};
    from_json(v, wide);
    if (wide < INT32_MIN || wide > INT32_MAX) {
        throw Error(error_code::parse_error,
                    QStringLiteral("integer out of range"));
    }
    out = static_cast<int>(wide);
}

inline void from_json(const QJsonValue& v, std::uint64_t& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected an integer"));
    }
    const double d = v.toDouble();
    const qint64 i = v.toInteger();
    if (i < 0 || static_cast<double>(i) != d) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a non-negative integer"));
    }
    out = static_cast<std::uint64_t>(i);
}

inline void from_json(const QJsonValue& v, double& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a number"));
    }
    out = v.toDouble();
}

inline void from_json(const QJsonValue& v, QString& out) {
    if (!v.isString()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a string"));
    }
    out = v.toString();
}

inline void from_json(const QJsonValue& v, QJsonObject& out) {
    if (!v.isObject()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a JSON object"));
    }
    out = v.toObject();
}

inline void from_json(const QJsonValue& v, QJsonArray& out) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a JSON array"));
    }
    out = v.toArray();
}

template <class T> void from_json(const QJsonValue& v, std::optional<T>& out) {
    if (v.isNull() || v.isUndefined()) {
        out.reset();
        return;
    }
    T tmp{};
    from_json(v, tmp);
    out = std::move(tmp);
}

template <class T> void from_json(const QJsonValue& v, QList<T>& out) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a JSON array"));
    }
    out.clear();
    const QJsonArray arr = v.toArray();
    out.reserve(arr.size());
    for (const QJsonValue& elem : arr) {
        T tmp{};
        from_json(elem, tmp);
        out.push_back(std::move(tmp));
    }
}

template <class T> T from_json(const QJsonValue& v) {
    T out{};
    from_json(v, out);
    return out;
}

// --------------------------------------------------------------------------
// Object helpers
// --------------------------------------------------------------------------

/// Fetch a required field and decode it. Throws mcp::Error(parse_error) if the
/// field is absent.
template <class T> T require(const QJsonObject& obj, const char* key) {
    const auto it = obj.constFind(QString::fromUtf8(key));
    if (it == obj.constEnd()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("missing required field '") +
                        QString::fromUtf8(key) + QLatin1Char('\''));
    }
    return from_json<T>(*it);
}

/// Decode `key` into `out` only when present and non-null. Absent or explicit
/// null leave `out` untouched (callers pass a pre-initialised optional).
template <class T>
void take_optional(const QJsonObject& obj, const char* key,
                   std::optional<T>& out) {
    const auto it = obj.constFind(QString::fromUtf8(key));
    if (it != obj.constEnd() && !it->isNull()) {
        out = from_json<T>(*it);
    }
}

/// Insert `key` into `obj` only when `value` is engaged.
template <class T>
void put_optional(QJsonObject& obj, const char* key,
                  const std::optional<T>& value) {
    if (value.has_value()) {
        obj.insert(QString::fromUtf8(key), to_json(*value));
    }
}

/// Decode a string field. Absent or explicit null produce a null QString;
/// a present value (including an empty string) is stored as-is.
inline void take_optional_string(const QJsonObject& obj, const char* key,
                                 QString& out) {
    const auto it = obj.constFind(QString::fromUtf8(key));
    if (it == obj.constEnd() || it->isNull()) {
        out = QString();
        return;
    }
    from_json(*it, out);
}

/// Insert a string field unless it is null (null means "absent").
inline void put_optional_string(QJsonObject& obj, const char* key,
                                const QString& value) {
    if (!value.isNull()) {
        obj.insert(QString::fromUtf8(key), value);
    }
}

/// Coerce a value that must be a JSON object.
inline QJsonObject as_object(const QJsonValue& v) {
    if (!v.isObject()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a JSON object"));
    }
    return v.toObject();
}

/// Coerce a value that must be a JSON array.
inline QJsonArray as_array(const QJsonValue& v) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error,
                    QStringLiteral("expected a JSON array"));
    }
    return v.toArray();
}

// --------------------------------------------------------------------------
// Text ↔ value
// --------------------------------------------------------------------------
//
// Qt's QJsonDocument only carries a top-level object or array, which is all
// JSON-RPC frames ever use. `parse_json` therefore accepts an object or array
// at the top level (mirroring what arrives on the wire); `dump` serialises
// values compactly.

[[nodiscard]] inline QJsonValue parse_json(QByteArrayView text) {
    QJsonParseError err{};
    const QByteArray bytes(text.data(), static_cast<int>(text.size()));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        throw Error(error_code::parse_error, err.errorString());
    }
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    throw Error(
        error_code::parse_error,
        QStringLiteral("top-level JSON value must be an object or array"));
}

[[nodiscard]] inline QString dump(const QJsonValue& v) {
    if (v.isObject()) {
        return QString::fromUtf8(
            QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    }
    if (v.isArray()) {
        return QString::fromUtf8(
            QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    }
    if (v.isNull() || v.isUndefined())
        return QStringLiteral("null");
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isDouble()) {
        const qint64 i = v.toInteger();
        if (static_cast<double>(i) == v.toDouble())
            return QString::number(i);
        return QString::number(v.toDouble());
    }
    if (v.isString()) {
        // Serialise via an array to reuse QJsonDocument's string escaping,
        // then strip the surrounding brackets.
        const QByteArray encoded =
            QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
    }
    return QStringLiteral("null");
}

}  // namespace mcp
