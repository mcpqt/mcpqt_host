// SPDX-License-Identifier: AGPL-3.0-only
//
// JSON codec layer.
//
// The SDK's wire types are ordinary C++ structs; JSON conversion is done by
// free functions rather than by a third-party JSON library. On the wire we use
// Qt's JSON value types (QJsonValue / QJsonObject / QJsonArray) directly, so
// the only external dependency is QtCore.
//
// Encoding:
//   mcp::to_json(x)            -> QJsonValue
// Decoding:
//   mcp::from_json<T>(value)   -> T            (throws mcp::Error on shape
//   errors)
//
// `to_json` / `from_json` are overloaded for the STL types the protocol uses
// (bool, integers, double, std::string, std::optional, std::vector) and for
// every wire struct, whose overloads are declared alongside the type in
// protocol.h and found via overload resolution / ADL.
//
// Reading helpers enforce a single, consistent null-vs-absent policy:
//   require<T>(obj, key)             required field; throws parse_error if
//   absent take_optional<T>(obj, key, out)  set `out` only if present and
//   non-null put_optional(obj, key, value)    insert only if the optional is
//   engaged

#pragma once

#include "error.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mcp {

using Json = QJsonValue;
using JsonObject = QJsonObject;
using JsonArray = QJsonArray;

// --------------------------------------------------------------------------
// Encoding
// --------------------------------------------------------------------------

inline QJsonValue to_json(std::nullptr_t) {
    return QJsonValue(QJsonValue::Null);
}
inline QJsonValue to_json(const QJsonValue& v) {
    return v;
}
inline QJsonValue to_json(const QJsonObject& v) {
    return v;
}
inline QJsonValue to_json(const QJsonArray& v) {
    return v;
}
inline QJsonValue to_json(bool v) {
    return QJsonValue(v);
}
inline QJsonValue to_json(int v) {
    return QJsonValue(v);
}
inline QJsonValue to_json(std::int64_t v) {
    return QJsonValue(static_cast<qint64>(v));
}
inline QJsonValue to_json(double v) {
    return QJsonValue(v);
}
inline QJsonValue to_json(const std::string& v) {
    return QJsonValue(QString::fromStdString(v));
}
inline QJsonValue to_json(std::string_view v) {
    return QJsonValue(QString::fromUtf8(v.data(), static_cast<int>(v.size())));
}
inline QJsonValue to_json(const char* v) {
    return QJsonValue(QString::fromUtf8(v));
}

inline QJsonValue to_json(std::uint64_t v) {
    // QJsonValue stores integers as qint64; values beyond 2^53 cannot be
    // represented exactly. Reject rather than silently lose precision.
    if (v > UINT64_C(9007199254740992)) {
        throw Error(error_code::internal_error,
                    "integer value too large for a JSON number");
    }
    return QJsonValue(static_cast<qint64>(v));
}

template <class T> QJsonValue to_json(const std::optional<T>& v) {
    if (!v.has_value()) {
        return QJsonValue(QJsonValue::Null);
    }
    return to_json(*v);
}

template <class T> QJsonValue to_json(const std::vector<T>& v) {
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
        throw Error(error_code::parse_error, "expected a boolean");
    }
    out = v.toBool();
}

inline void from_json(const QJsonValue& v, std::int64_t& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error, "expected an integer");
    }
    const double d = v.toDouble();
    const qint64 i = v.toInteger();
    if (static_cast<double>(i) != d) {
        throw Error(error_code::parse_error, "expected an integer");
    }
    out = static_cast<std::int64_t>(i);
}

inline void from_json(const QJsonValue& v, int& out) {
    std::int64_t wide{};
    from_json(v, wide);
    if (wide < INT32_MIN || wide > INT32_MAX) {
        throw Error(error_code::parse_error, "integer out of range");
    }
    out = static_cast<int>(wide);
}

inline void from_json(const QJsonValue& v, std::uint64_t& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error, "expected an integer");
    }
    const double d = v.toDouble();
    const qint64 i = v.toInteger();
    if (i < 0 || static_cast<double>(i) != d) {
        throw Error(error_code::parse_error, "expected a non-negative integer");
    }
    out = static_cast<std::uint64_t>(i);
}

inline void from_json(const QJsonValue& v, double& out) {
    if (!v.isDouble()) {
        throw Error(error_code::parse_error, "expected a number");
    }
    out = v.toDouble();
}

inline void from_json(const QJsonValue& v, std::string& out) {
    if (!v.isString()) {
        throw Error(error_code::parse_error, "expected a string");
    }
    out = v.toString().toStdString();
}

inline void from_json(const QJsonValue& v, QJsonObject& out) {
    if (!v.isObject()) {
        throw Error(error_code::parse_error, "expected a JSON object");
    }
    out = v.toObject();
}

inline void from_json(const QJsonValue& v, QJsonArray& out) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error, "expected a JSON array");
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

template <class T> void from_json(const QJsonValue& v, std::vector<T>& out) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error, "expected a JSON array");
    }
    out.clear();
    const QJsonArray arr = v.toArray();
    out.reserve(static_cast<std::size_t>(arr.size()));
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
                    std::string("missing required field '") + key + "'");
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

/// Coerce a value that must be a JSON object.
inline QJsonObject as_object(const QJsonValue& v) {
    if (!v.isObject()) {
        throw Error(error_code::parse_error, "expected a JSON object");
    }
    return v.toObject();
}

/// Coerce a value that must be a JSON array.
inline QJsonArray as_array(const QJsonValue& v) {
    if (!v.isArray()) {
        throw Error(error_code::parse_error, "expected a JSON array");
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
// objects and arrays compactly.

[[nodiscard]] inline QJsonValue parse_json(std::string_view text) {
    QJsonParseError err{};
    const QByteArray bytes(text.data(), static_cast<int>(text.size()));
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError) {
        throw Error(error_code::parse_error, err.errorString().toStdString());
    }
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    throw Error(error_code::parse_error,
                "top-level JSON value must be an object or array");
}

[[nodiscard]] inline std::string dump(const QJsonValue& v) {
    if (v.isObject()) {
        return QJsonDocument(v.toObject())
            .toJson(QJsonDocument::Compact)
            .toStdString();
    }
    if (v.isArray()) {
        return QJsonDocument(v.toArray())
            .toJson(QJsonDocument::Compact)
            .toStdString();
    }
    if (v.isNull() || v.isUndefined())
        return "null";
    if (v.isBool())
        return v.toBool() ? "true" : "false";
    if (v.isDouble()) {
        const qint64 i = v.toInteger();
        if (static_cast<double>(i) == v.toDouble())
            return std::to_string(i);
        return std::to_string(v.toDouble());
    }
    if (v.isString()) {
        // Escape the UTF-8 bytes into a JSON string literal.
        const QByteArray utf8 = v.toString().toUtf8();
        std::string out = "\"";
        for (const char ch : utf8) {
            const auto u = static_cast<unsigned char>(ch);
            switch (u) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out += ch;
                }
                break;
            }
        }
        out += '"';
        return out;
    }
    return "null";
}

}  // namespace mcp
