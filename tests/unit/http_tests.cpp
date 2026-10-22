// The HTTP client against a loopback server: responses, deadlines,
// cancellation, mutual TLS and the pinned host certificate.

#include "check.h"

#include "test_server.h"

#include <openssl/x509.h>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include "moonlight/http.h"
#include "pairing_vectors.h"

using namespace moonlight;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {

/// Loads a certificate and key from PEM.
struct Identity {
    Identity(std::string_view cert_pem, std::string_view key_pem) {
        BIO* b = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
        cert = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
        BIO_free(b);
        b = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
        key = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
        BIO_free(b);
    }
    ~Identity() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    X509* cert;
    EVP_PKEY* key;
};

http::Error::Kind failure(const std::function<void()>& f) {
    try {
        f();
    } catch (const http::Error& e) {
        return e.kind();
    }
    CHECK(false);
    return http::Error::Kind::io;
}

}  // namespace

TEST(http_parse_response) {
    auto r = http::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello and more", false);
    REQUIRE(r);
    CHECK_EQ(r->status, 200);
    CHECK_EQ(r->body, std::string("hello"));
    CHECK(!http::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel", false));
    CHECK(!http::parse_response("HTTP/1.1 200 OK\r\nContent-Le", false));
    r = http::parse_response("HTTP/1.1 404 NOT FOUND\r\n\r\n<root status_code=\"404\"/>", true);
    REQUIRE(r);
    CHECK_EQ(r->status, 404);
    CHECK_EQ(r->body, std::string("<root status_code=\"404\"/>"));
    r = http::parse_response(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;x=y\r\n world\r\n0\r\n\r\n", false);
    REQUIRE(r);
    CHECK_EQ(r->body, std::string("hello world"));
    CHECK(!http::parse_response("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", false));
    CHECK(failure([] { http::parse_response("SSH-2.0-OpenSSH\r\n\r\n", false); }) == http::Error::Kind::protocol);
    CHECK(failure([] { http::parse_response("", true); }) == http::Error::Kind::protocol);
    CHECK(failure([] { http::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nshort", true); }) ==
          http::Error::Kind::protocol);
}

TEST(http_get_with_content_length) {
    std::string seen;
    test::Server server([&](test::Conn& c) {
        seen = c.read_request();
        // The connection stays open: the client must stop at the length.
        c.write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
        c.wait_for_close();
    });
    http::Request req;
    req.target = "/serverinfo?uniqueid=abc&uuid=def";
    auto start = clk::now();
    http::Response r = http::fetch("127.0.0.1", server.port(), req);
    CHECK(clk::now() - start < 2s);
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.body, std::string("hello"));
    CHECK(seen.starts_with("GET /serverinfo?uniqueid=abc&uuid=def HTTP/1.1\r\n"));
    CHECK(seen.find("Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n") != std::string::npos);
    CHECK(seen.find("Connection: close\r\n") != std::string::npos);
}

TEST(http_body_until_close_and_post) {
    std::string seen;
    test::Server server([&](test::Conn& c) {
        seen = c.read_request();
        c.write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":true}");
    });
    http::Request req;
    req.method = "POST";
    req.target = "/api/pin";
    req.headers = {{"Content-Type", "application/json"}};
    req.body = "{\"pin\":\"1234\"}";
    http::Response r = http::fetch("127.0.0.1", server.port(), req);
    CHECK_EQ(r.body, std::string("{\"status\":true}"));
    CHECK(seen.find("Content-Length: 14\r\n\r\n{\"pin\":\"1234\"}") != std::string::npos);
}

TEST(http_timeout_and_cancel) {
    test::Server server([](test::Conn& c) {
        c.read_request();
        c.wait_for_close();  // never answers
    });
    http::Request req;
    req.timeout = 300ms;
    auto start = clk::now();
    CHECK(failure([&] { http::fetch("127.0.0.1", server.port(), req); }) == http::Error::Kind::timeout);
    CHECK(clk::now() - start < 3s);

    // Cancelled from another thread while waiting, like a disconnect during
    // the PIN entry.
    http::Canceller cancel;
    req.timeout = 30s;
    std::thread t([&] {
        std::this_thread::sleep_for(300ms);
        cancel.cancel();
    });
    start = clk::now();
    CHECK(failure([&] { http::fetch("127.0.0.1", server.port(), req, nullptr, &cancel); }) ==
          http::Error::Kind::cancelled);
    CHECK(clk::now() - start < 5s);
    t.join();
    // Once cancelled, always cancelled.
    CHECK(failure([&] { http::fetch("127.0.0.1", server.port(), req, nullptr, &cancel); }) ==
          http::Error::Kind::cancelled);
}

TEST(http_connection_refused) {
    std::uint16_t port;
    {
        test::Server closed([](test::Conn&) {});
        port = closed.port();
    }
    http::Request req;
    CHECK(failure([&] { http::fetch("127.0.0.1", port, req); }) == http::Error::Kind::connect);
    CHECK(failure([&] { http::fetch("no-such-host.invalid", 47989, req); }) == http::Error::Kind::resolve);
}

TEST(http_tls_pinned_and_mutual) {
    Identity client(vectors::client_cert, vectors::client_key);
    std::string presented;
    test::Server server(
        [&](test::Conn& c) {
            c.read_request();
            if (X509* peer = c.peer_certificate()) {
                char name[256];
                X509_NAME_oneline(X509_get_subject_name(peer), name, sizeof(name));
                presented = name;
            }
            c.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        },
        vectors::host_cert, vectors::host_key);

    http::Tls tls;
    tls.certificate = client.cert;
    tls.key = client.key;
    tls.pinned_pem = std::string(vectors::host_cert);
    http::Request req;
    http::Response r = http::fetch("127.0.0.1", server.port(), req, &tls);
    CHECK_EQ(r.body, std::string("ok"));
    CHECK_EQ(presented, std::string("/CN=NVIDIA GameStream Client"));

    // Any other certificate is refused before a request is sent.
    tls.pinned_pem = std::string(vectors::client_cert);
    CHECK(failure([&] { http::fetch("127.0.0.1", server.port(), req, &tls); }) == http::Error::Kind::certificate);
    tls.pinned_pem.clear();
    CHECK(failure([&] { http::fetch("127.0.0.1", server.port(), req, &tls); }) == http::Error::Kind::certificate);
    tls.any_server = true;
    CHECK_EQ(http::fetch("127.0.0.1", server.port(), req, &tls).body, std::string("ok"));
}

TEST(http_answer_before_request) {
    // Sunshine answers an unknown client's certificate with a 401 right
    // after the handshake, without reading the request.
    test::Server server(
        [](test::Conn& c) {
            c.write("HTTP/1.1 200 OK\r\nContent-Length: 25\r\n\r\n<root status_code=\"401\"/>");
            c.wait_for_close();
        },
        vectors::host_cert, vectors::host_key);
    Identity client(vectors::client_cert, vectors::client_key);
    http::Tls tls;
    tls.certificate = client.cert;
    tls.key = client.key;
    tls.pinned_pem = std::string(vectors::host_cert);
    http::Request req;
    http::Response r = http::fetch("127.0.0.1", server.port(), req, &tls);
    CHECK(r.body.find("status_code=\"401\"") != std::string::npos);
}
