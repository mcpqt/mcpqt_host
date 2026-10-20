// SPDX-License-Identifier: AGPL-3.0-only
//
// Round-trip and integration tests for the resources surface added in
// Phase 2: list_resources, list_resource_templates, read_resource,
// subscribe/unsubscribe, plus the resource-related content blocks
// (ResourceLink, EmbeddedResource).

#include "in_memory_transport.h"

#include "client.h"
#include "protocol.h"
#include "server.h"

#include "qt_test_support.h"

#include <QtTest/QtTest>

#include <QCoreApplication>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <variant>

namespace {

using namespace std::chrono_literals;

// -------------------------------------------------------------------------
// Server + Client integration harness
// -------------------------------------------------------------------------

struct ResourcesServerThread {
    std::shared_ptr<mcp::Server> server;
    std::thread thread;

    template <typename Configure>
    ResourcesServerThread(std::unique_ptr<mcp::Transport> t, Configure cfg)
        : server(std::make_shared<mcp::Server>(mcp::Implementation{
              .name = "resources-test",
              .version = "1.0",
          })) {
        cfg(*server);
        thread = std::thread(
            [s = server, t = std::move(t)]() mutable { s->run(std::move(t)); });
    }
    ~ResourcesServerThread() {
        server->stop();
        if (thread.joinable())
            thread.join();
    }
};

}  // namespace

using mcp::Json;

// -------------------------------------------------------------------------
// Round-trip tests for the new types
// -------------------------------------------------------------------------

class Resources : public QObject {
    Q_OBJECT
private slots:
    void TextContentsRoundTrip(void);
    void BlobContentsRoundTrip(void);
    void ContentsVariantDispatchesByDiscriminator(void);
    void ResourceRoundTrip(void);
    void ResourceTemplateRoundTrip(void);
    void ResourceLinkContentBlock(void);
    void EmbeddedResourceContentBlock(void);
};

void Resources::TextContentsRoundTrip(void) {
    mcp::TextResourceContents c{
        .uri = "file:///hi.txt",
        .mime_type = "text/plain",
        .text = "hi",
    };
    Json j = mcp::to_json(c);
    QCOMPARE(j["uri"], mcp::test::jv("file:///hi.txt"));
    QCOMPARE(j["mimeType"], mcp::test::jv("text/plain"));
    QCOMPARE(j["text"], mcp::test::jv("hi"));
    auto back = mcp::from_json<mcp::TextResourceContents>(j);
    QCOMPARE(back.uri, c.uri);
    QCOMPARE(back.text, c.text);
}

void Resources::BlobContentsRoundTrip(void) {
    mcp::BlobResourceContents c{
        .uri = "file:///bin",
        .mime_type = "application/octet-stream",
        .blob = "Zm9v",
    };
    Json j = mcp::to_json(c);
    QCOMPARE(j["blob"], mcp::test::jv("Zm9v"));
    auto back = mcp::from_json<mcp::BlobResourceContents>(j);
    QCOMPARE(back.blob, c.blob);
}

void Resources::ContentsVariantDispatchesByDiscriminator(void) {
    QJsonObject text_obj = {{"uri", "x"}, {"text", "y"}};
    auto v = mcp::from_json<mcp::ResourceContents>(text_obj);
    QVERIFY(std::holds_alternative<mcp::TextResourceContents>(v));

    QJsonObject blob_obj = {{"uri", "x"}, {"blob", "Zm9v"}};
    v = mcp::from_json<mcp::ResourceContents>(blob_obj);
    QVERIFY(std::holds_alternative<mcp::BlobResourceContents>(v));

    QJsonObject bad = {{"uri", "x"}};
    QVERIFY_EXCEPTION_THROWN(mcp::from_json<mcp::ResourceContents>(bad),
                             mcp::Error);
}

void Resources::ResourceRoundTrip(void) {
    mcp::Resource r{
        .uri = "file:///hi",
        .name = "hi",
        .title = "Hi",
        .mime_type = "text/plain",
        .size = 10,
    };
    Json j = mcp::to_json(r);
    QCOMPARE(j["uri"], mcp::test::jv("file:///hi"));
    QCOMPARE(j["mimeType"], mcp::test::jv("text/plain"));
    QCOMPARE(j["size"], mcp::test::jv(10));
    auto back = mcp::from_json<mcp::Resource>(j);
    QCOMPARE(back.uri, r.uri);
    QCOMPARE(back.size, r.size);
}

void Resources::ResourceTemplateRoundTrip(void) {
    mcp::ResourceTemplate r{
        .uri_template = "file:///{name}.txt",
        .name = "files",
    };
    Json j = mcp::to_json(r);
    QCOMPARE(j["uriTemplate"], mcp::test::jv("file:///{name}.txt"));
    auto back = mcp::from_json<mcp::ResourceTemplate>(j);
    QCOMPARE(back.uri_template, r.uri_template);
}

void Resources::ResourceLinkContentBlock(void) {
    mcp::ResourceLink rl{
        .uri = "file:///doc",
        .name = "Doc",
    };
    Json j = mcp::to_json(mcp::ContentBlock{rl});
    QCOMPARE(j["type"], mcp::test::jv("resource_link"));
    QCOMPARE(j["uri"], mcp::test::jv("file:///doc"));

    auto block = mcp::from_json<mcp::ContentBlock>(j);
    QVERIFY(std::holds_alternative<mcp::ResourceLink>(block));
    QCOMPARE(std::get<mcp::ResourceLink>(block).uri, rl.uri);
}

void Resources::EmbeddedResourceContentBlock(void) {
    mcp::EmbeddedResource er{
        .resource =
            mcp::TextResourceContents{
                .uri = "file:///r",
                .mime_type = "text/plain",
                .text = "embedded",
            },
    };
    Json j = mcp::to_json(mcp::ContentBlock{er});
    QCOMPARE(j["type"], mcp::test::jv("resource"));
    QCOMPARE(j["resource"]["text"], mcp::test::jv("embedded"));

    auto block = mcp::from_json<mcp::ContentBlock>(j);
    QVERIFY(std::holds_alternative<mcp::EmbeddedResource>(block));
    auto inner = std::get<mcp::EmbeddedResource>(block);
    QVERIFY(std::holds_alternative<mcp::TextResourceContents>(inner.resource));
}

// -------------------------------------------------------------------------
// Server + Client integration
// -------------------------------------------------------------------------

class ResourcesIntegration : public QObject {
    Q_OBJECT
private slots:
    void ListAndRead(void);
    void MissingResourceMapsToError(void);
    void FallbackHandlesUnknownUri(void);
    void ResourceTemplateIsReturned(void);
};

void ResourcesIntegration::ListAndRead(void) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource(
            mcp::Resource{
                .uri = "file:///hello.txt",
                .name = "hello",
                .mime_type = "text/plain",
            },
            [](const std::string& uri) {
                return mcp::ReadResourceResult{
                    .contents = {mcp::TextResourceContents{
                        .uri = uri,
                        .mime_type = "text/plain",
                        .text = "hello, world\n",
                    }},
                };
            });
        s.resource(
            mcp::Resource{
                .uri = "file:///bin",
                .name = "bin",
                .mime_type = "application/octet-stream",
            },
            [](const std::string& uri) {
                return mcp::ReadResourceResult{
                    .contents = {mcp::BlobResourceContents{
                        .uri = uri,
                        .mime_type = "application/octet-stream",
                        .blob = "Zm9vYmFy",
                    }},
                };
            });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    auto info = client.initialize().get();
    QVERIFY(info.capabilities.resources.has_value());

    auto list = client.list_resources().get();
    QCOMPARE(list.resources.size(), std::size_t{2});

    auto read = client.read_resource("file:///hello.txt").get();
    QCOMPARE(read.contents.size(), std::size_t{1});
    QVERIFY(
        std::holds_alternative<mcp::TextResourceContents>(read.contents[0]));
    QCOMPARE(std::get<mcp::TextResourceContents>(read.contents[0]).text,
             std::string{"hello, world\n"});

    auto blob = client.read_resource("file:///bin").get();
    QCOMPARE(blob.contents.size(), std::size_t{1});
    QVERIFY(
        std::holds_alternative<mcp::BlobResourceContents>(blob.contents[0]));

    client.disconnect();
}

void ResourcesIntegration::MissingResourceMapsToError(void) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource(
            mcp::Resource{.uri = "file:///x", .name = "x"},
            [](const std::string&) { return mcp::ReadResourceResult{}; });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto fut = client.read_resource("file:///nope");
    try {
        (void)fut.get();
        QFAIL("expected Error");
    } catch (const mcp::Error& e) {
        QCOMPARE(e.code(), mcp::error_code::method_not_found);
    }

    client.disconnect();
}

void ResourcesIntegration::FallbackHandlesUnknownUri(void) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.fallback_resource_handler([](const std::string& uri) {
            return mcp::ReadResourceResult{
                .contents = {mcp::TextResourceContents{
                    .uri = uri,
                    .text = "fallback for " + uri,
                }},
            };
        });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto read = client.read_resource("file:///anything").get();
    QCOMPARE(read.contents.size(), std::size_t{1});
    QVERIFY(std::get<mcp::TextResourceContents>(read.contents[0])
                .text.find("fallback") != std::string::npos);

    client.disconnect();
}

void ResourcesIntegration::ResourceTemplateIsReturned(void) {
    auto p = mcp::test::make_in_memory_pair();
    ResourcesServerThread srv(std::move(p.b), [](mcp::Server& s) {
        s.resource_template(mcp::ResourceTemplate{
            .uri_template = "file:///{name}.txt",
            .name = "files",
        });
    });

    mcp::Client client{mcp::Implementation{.name = "tester", .version = "0"}};
    client.connect(std::move(p.a));
    (void)client.initialize().get();

    auto list = client.list_resource_templates().get();
    QCOMPARE(list.resource_templates.size(), std::size_t{1});
    QCOMPARE(list.resource_templates[0].uri_template,
             std::string{"file:///{name}.txt"});

    client.disconnect();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        Resources suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    {
        ResourcesIntegration suite;
        status |= QTest::qExec(&suite, argc, argv);
    }
    return status;
}

#include "resources_test.moc"
