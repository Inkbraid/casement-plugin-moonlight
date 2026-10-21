// A small HTTP/1.1 client for the GameStream control API: one request per
// connection, plain or over TLS with a client certificate, the server's
// certificate checked against the one pinned at pairing. Every request has
// a deadline and can be cancelled from another thread.

#pragma once

#include <openssl/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonlight::http {

class Error : public std::runtime_error {
public:
    enum class Kind { resolve, connect, timeout, tls, certificate, io, protocol, cancelled };
    Error(Kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    Kind kind() const { return kind_; }

private:
    Kind kind_;
};

/// Aborts the request in progress, from any thread. Once cancelled, every
/// later request through it fails at once.
class Canceller {
public:
    void cancel();
    bool cancelled() const { return cancelled_.load(); }

    // For the client: the socket a request is waiting on.
    void attach(std::uintptr_t socket);
    void detach();

private:
    std::mutex lock_;
    std::uintptr_t socket_ = ~std::uintptr_t{0};
    std::atomic<bool> cancelled_{false};
};

struct Tls {
    /// Our certificate and key for mutual TLS; may be null.
    X509* certificate = nullptr;
    EVP_PKEY* key = nullptr;
    /// The only server certificate accepted (PEM).
    std::string pinned_pem;
    /// Accept any server certificate. Only for tests that talk to a web UI
    /// with a self-signed certificate.
    bool any_server = false;
};

struct Request {
    std::string method = "GET";
    /// Path and query, already encoded.
    std::string target = "/";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::chrono::milliseconds timeout{5000};
};

struct Response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

/// Sends one request to host:port (host without IPv6 brackets) and reads
/// the response. Throws Error.
Response fetch(std::string_view host, std::uint16_t port, const Request& request, const Tls* tls = nullptr,
               Canceller* cancel = nullptr);

/// Parses a response from bytes received so far: nothing until it is
/// complete. `eof` says the connection has closed. Throws Error (protocol)
/// for a malformed response.
std::optional<Response> parse_response(std::string_view data, bool eof);

}  // namespace moonlight::http
