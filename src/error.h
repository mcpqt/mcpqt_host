// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QJsonValue>

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace mcp {

/// MCP / JSON-RPC 2.0 error codes.
///
/// Values in [-32768, -32000] are reserved by JSON-RPC 2.0 for transport-level
/// errors. The MCP spec inherits the standard codes and adds its own in
/// [-32000, -32099]. Server- or domain-specific errors should use
/// application-defined codes outside the reserved range.
///
/// `no_error` (0) represents the absence of an error. This is an unscoped
/// enumeration: it converts implicitly to the wire integer, so it can be
/// passed to Error and compared with ErrorObject::code without casts.
enum error_code : int {
    no_error = 0,

    // JSON-RPC 2.0 standard codes.
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found,  // -32601
    invalid_params,    // -32602
    internal_error,    // -32603

    // MCP-specific (in the implementation-defined JSON-RPC range).
    url_elicitation_required = -32042,
};

/// A wire-format-compatible JSON-RPC error object.
///
/// This is the value that lives inside `JSONRPCErrorResponse::error`. It is
/// also raised as `Error` (an exception) when a method call receives an error
/// response and the caller asks for the result via a future.
struct ErrorObject {
    int code{error_code::no_error};
    QString message;
    QJsonValue data;  // optional; null when absent on the wire

    [[nodiscard]] bool has_data(void) const noexcept { return !data.isNull(); }
};

/// Exception type for MCP errors. Carries the wire `ErrorObject`.
///
/// Thrown only at well-defined boundaries (e.g. `std::future<T>::get()` for a
/// failed call). Library internals prefer to plumb errors through return
/// values or callbacks rather than throwing.
class Error : public std::runtime_error {
public:
    explicit Error(ErrorObject err)
        : std::runtime_error(err.message.toStdString()),
          m_error(std::move(err)) {}

    Error(int code, QString msg, QJsonValue data = QJsonValue(QJsonValue::Null))
        : std::runtime_error(msg.toStdString()),
          m_error{code, std::move(msg), std::move(data)} {}

    [[nodiscard]] const ErrorObject& object(void) const noexcept {
        return m_error;
    }
    [[nodiscard]] int code(void) const noexcept { return m_error.code; }
    [[nodiscard]] const QString& message(void) const noexcept {
        return m_error.message;
    }
    [[nodiscard]] const QJsonValue& data(void) const noexcept {
        return m_error.data;
    }

private:
    ErrorObject m_error;
};

}  // namespace mcp
