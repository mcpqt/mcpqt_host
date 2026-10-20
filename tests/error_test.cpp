// SPDX-License-Identifier: AGPL-3.0-only
#include "error.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <stdexcept>
#include <string>

class ErrorObject : public QObject {
    Q_OBJECT
private slots:
    void DefaultConstructibleAndDataAbsent(void);
    void HasDataReflectsNullness(void);
};

class ErrorException : public QObject {
    Q_OBJECT
private slots:
    void CarriesCodeMessageAndData(void);
    void IsStdRuntimeError(void);
    void ConstructFromObjectPreservesFields(void);
};

void ErrorObject::DefaultConstructibleAndDataAbsent(void) {
    mcp::ErrorObject e{};
    QCOMPARE(e.code, 0);
    QVERIFY(e.message.empty());
    QVERIFY(!e.has_data());
}

void ErrorObject::HasDataReflectsNullness(void) {
    mcp::ErrorObject e{};
    e.data = QJsonObject{{"k", 1}};
    QVERIFY(e.has_data());
}

void ErrorException::CarriesCodeMessageAndData(void) {
    const QJsonObject extra{{"why", "because"}};
    mcp::Error err{mcp::error_code::invalid_params, "bad arg", extra};
    QCOMPARE(err.code(), mcp::error_code::invalid_params);
    QCOMPARE(err.message(), std::string{"bad arg"});
    QCOMPARE(err.data(), mcp::Json{extra});
    QCOMPARE(QString::fromUtf8(err.what()), QStringLiteral("bad arg"));
}

void ErrorException::IsStdRuntimeError(void) {
    mcp::Error err{mcp::error_code::internal_error, "boom"};
    bool caught_as_runtime_error = false;
    try {
        throw err;
    } catch (const std::runtime_error& e) {
        caught_as_runtime_error = true;
        QCOMPARE(QString::fromUtf8(e.what()), QStringLiteral("boom"));
    } catch (...) {
        QFAIL("Error did not derive from std::runtime_error");
    }
    QVERIFY(caught_as_runtime_error);
}

void ErrorException::ConstructFromObjectPreservesFields(void) {
    mcp::ErrorObject obj{mcp::error_code::method_not_found, "no such", {}};
    mcp::Error err{obj};
    QCOMPARE(err.code(), obj.code);
    QCOMPARE(err.message(), obj.message);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        ErrorObject suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        ErrorException suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "error_test.moc"
