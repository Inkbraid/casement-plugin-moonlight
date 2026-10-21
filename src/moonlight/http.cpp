#include "moonlight/http.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"

namespace moonlight::http {

namespace {

using clock = std::chrono::steady_clock;
constexpr std::size_t max_response = std::size_t{16} << 20;

void start_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

std::string openssl_error() {
    unsigned long e = ERR_get_error();
    if (e == 0) return "TLS failed";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    ERR_clear_error();
    return buf;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

bool is_ip_literal(std::string_view host) {
    in6_addr a6;
    in_addr a4;
    std::string h(host);
    return inet_pton(AF_INET, h.c_str(), &a4) == 1 || inet_pton(AF_INET6, h.c_str(), &a6) == 1;
}

struct SslCtxFree {
    void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
};
struct SslFree {
    void operator()(SSL* p) const { SSL_free(p); }
};
struct X509Free {
    void operator()(X509* p) const { X509_free(p); }
};

std::vector<unsigned char> der(X509* cert) {
    int n = i2d_X509(cert, nullptr);
    if (n <= 0) return {};
    std::vector<unsigned char> out(static_cast<std::size_t>(n));
    unsigned char* p = out.data();
    i2d_X509(cert, &p);
    return out;
}

/// One TCP connection, optionally with TLS, with every wait bounded by
/// the request's deadline and interrupted by cancellation.
class Connection {
public:
    Connection(std::string_view host, std::uint16_t port, clock::time_point deadline, Canceller* cancel)
        : host_(host), deadline_(deadline), cancel_(cancel) {
        try {
            open(port);
        } catch (...) {
            close_socket();
            throw;
        }
    }

    ~Connection() {
        ssl_.reset();
        ctx_.reset();
        close_socket();
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void open(std::uint16_t port) {
        start_winsock();
        check_cancelled();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* found = nullptr;
        std::string port_text = std::to_string(port);
        if (int r = getaddrinfo(host_.c_str(), port_text.c_str(), &hints, &found); r != 0 || !found)
            throw Error(Error::Kind::resolve, std::format("Can't find the host {}: {}", host_,
                                                          casement::system_message(static_cast<unsigned long>(r))));
        std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> list(found, &freeaddrinfo);
        std::string last_error = "no address";
        for (addrinfo* entry = found; entry; entry = entry->ai_next) {
            SOCKET s = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nonblocking = 1;
            ioctlsocket(s, FIONBIO, &nonblocking);
            BOOL nodelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
            socket_ = s;
            if (cancel_) cancel_->attach(static_cast<std::uintptr_t>(s));
            if (::connect(s, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) return;
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                try {
                    wait(POLLWRNORM);
                } catch (const Error& e) {
                    if (e.kind() != Error::Kind::io) throw;
                }
                int so_error = 0;
                int len = sizeof(so_error);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
                if (so_error == 0) return;
                err = so_error;
            }
            last_error = casement::system_message(static_cast<unsigned long>(err));
            close_socket();
        }
        throw Error(Error::Kind::connect, std::format("Can't connect to {} port {}: {}", host_, port, last_error));
    }

    void start_tls(const Tls& tls) {
        ctx_.reset(SSL_CTX_new(TLS_client_method()));
        if (!ctx_) throw Error(Error::Kind::tls, openssl_error());
        SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);
        // Hosts often close without a TLS close_notify.
        SSL_CTX_set_options(ctx_.get(), SSL_OP_IGNORE_UNEXPECTED_EOF);
        // The chain is self-signed; the certificate is compared with the
        // pinned one below instead.
        SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);
        if (tls.certificate && tls.key) {
            if (SSL_CTX_use_certificate(ctx_.get(), tls.certificate) != 1 ||
                SSL_CTX_use_PrivateKey(ctx_.get(), tls.key) != 1)
                throw Error(Error::Kind::tls, "Can't use the client certificate: " + openssl_error());
        }
        ssl_.reset(SSL_new(ctx_.get()));
        if (!ssl_) throw Error(Error::Kind::tls, openssl_error());
        SSL_set_fd(ssl_.get(), static_cast<int>(socket_));
        if (!is_ip_literal(host_)) SSL_set_tlsext_host_name(ssl_.get(), host_.c_str());
        for (;;) {
            int r = SSL_connect(ssl_.get());
            if (r == 1) break;
            int e = SSL_get_error(ssl_.get(), r);
            if (e == SSL_ERROR_WANT_READ) wait(POLLRDNORM);
            else if (e == SSL_ERROR_WANT_WRITE) wait(POLLWRNORM);
            else throw Error(Error::Kind::tls, std::format("TLS with {} failed: {}", host_, openssl_error()));
        }
        if (tls.any_server) return;
        std::unique_ptr<X509, X509Free> peer(SSL_get1_peer_certificate(ssl_.get()));
        std::unique_ptr<X509, X509Free> pinned;
        if (!tls.pinned_pem.empty()) {
            BIO* bio = BIO_new_mem_buf(tls.pinned_pem.data(), static_cast<int>(tls.pinned_pem.size()));
            pinned.reset(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
            BIO_free(bio);
        }
        if (!peer || !pinned || der(peer.get()) != der(pinned.get()))
            throw Error(Error::Kind::certificate,
                        std::format("{} presented a different certificate than when it was paired.", host_));
    }

    void send_all(std::string_view data) {
        while (!data.empty()) {
            check_cancelled();
            if (ssl_) {
                std::size_t written = 0;
                int r = SSL_write_ex(ssl_.get(), data.data(), data.size(), &written);
                if (r == 1) {
                    data.remove_prefix(written);
                    continue;
                }
                int e = SSL_get_error(ssl_.get(), r);
                if (e == SSL_ERROR_WANT_READ) wait(POLLRDNORM);
                else if (e == SSL_ERROR_WANT_WRITE) wait(POLLWRNORM);
                else throw Error(Error::Kind::io, std::format("Sending to {} failed: {}", host_, openssl_error()));
            } else {
                int n = ::send(socket_, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), 1 << 20)), 0);
                if (n > 0) {
                    data.remove_prefix(static_cast<std::size_t>(n));
                    continue;
                }
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) throw io_error("Sending to", err);
                wait(POLLWRNORM);
            }
        }
    }

    /// Reads what is available, waiting for some; 0 at the end of the stream.
    std::size_t receive(char* buf, std::size_t size) {
        for (;;) {
            check_cancelled();
            if (ssl_) {
                std::size_t got = 0;
                int r = SSL_read_ex(ssl_.get(), buf, size, &got);
                if (r == 1) return got;
                int e = SSL_get_error(ssl_.get(), r);
                if (e == SSL_ERROR_ZERO_RETURN) return 0;
                if (e == SSL_ERROR_WANT_READ) wait(POLLRDNORM);
                else if (e == SSL_ERROR_WANT_WRITE) wait(POLLWRNORM);
                else if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return 0;
                else throw Error(Error::Kind::io, std::format("Reading from {} failed: {}", host_, openssl_error()));
            } else {
                int n = ::recv(socket_, buf, static_cast<int>(std::min<std::size_t>(size, 1 << 20)), 0);
                if (n >= 0) return static_cast<std::size_t>(n);
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) throw io_error("Reading from", err);
                wait(POLLRDNORM);
            }
        }
    }

private:
    Error io_error(std::string_view what, int err) {
        if (cancel_ && cancel_->cancelled()) return Error(Error::Kind::cancelled, "Cancelled.");
        return Error(Error::Kind::io, std::format("{} {} failed: {}", what, host_,
                                                  casement::system_message(static_cast<unsigned long>(err))));
    }

    void check_cancelled() {
        if (cancel_ && cancel_->cancelled()) throw Error(Error::Kind::cancelled, "Cancelled.");
    }

    /// Waits until the socket is ready for `events`, in short slices so a
    /// cancellation is noticed even when the socket stays quiet.
    void wait(short events) {
        for (;;) {
            check_cancelled();
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - clock::now()).count();
            if (left <= 0) throw Error(Error::Kind::timeout, std::format("{} didn't answer in time.", host_));
            WSAPOLLFD p{};
            p.fd = socket_;
            p.events = events;
            int r = WSAPoll(&p, 1, static_cast<INT>(std::min<long long>(left, 100)));
            // Ready, closed or broken: the next read or write says which, and
            // a read still gets data that arrived before a reset.
            if (r > 0) return;
            if (r < 0) throw io_error("Waiting for", WSAGetLastError());
        }
    }

    void close_socket() {
        if (socket_ == INVALID_SOCKET) return;
        if (cancel_) cancel_->detach();
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    std::string host_;
    clock::time_point deadline_;
    Canceller* cancel_;
    SOCKET socket_ = INVALID_SOCKET;
    std::unique_ptr<SSL_CTX, SslCtxFree> ctx_;
    std::unique_ptr<SSL, SslFree> ssl_;
};

std::optional<std::size_t> find_header_end(std::string_view data) {
    auto p = data.find("\r\n\r\n");
    if (p == std::string_view::npos) return std::nullopt;
    return p + 4;
}

}  // namespace

void Canceller::cancel() {
    std::lock_guard g(lock_);
    cancelled_ = true;
    if (socket_ != ~std::uintptr_t{0}) shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
}

void Canceller::attach(std::uintptr_t socket) {
    std::lock_guard g(lock_);
    socket_ = socket;
    if (cancelled_) shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
}

void Canceller::detach() {
    std::lock_guard g(lock_);
    socket_ = ~std::uintptr_t{0};
}

std::optional<Response> parse_response(std::string_view data, bool eof) {
    auto head_end = find_header_end(data);
    if (!head_end) {
        if (eof) throw Error(Error::Kind::protocol, data.empty() ? "The host closed the connection without answering."
                                                                 : "The host's answer was cut off.");
        if (data.size() > 65536) throw Error(Error::Kind::protocol, "The host's answer has no end of headers.");
        return std::nullopt;
    }
    std::string_view head = data.substr(0, *head_end - 4);
    Response response;
    auto line_end = head.find("\r\n");
    std::string_view status_line = head.substr(0, line_end);
    if (!status_line.starts_with("HTTP/1.") || status_line.size() < 12)
        throw Error(Error::Kind::protocol, "The host's answer isn't HTTP.");
    auto code = status_line.substr(9, 3);
    if (std::from_chars(code.data(), code.data() + 3, response.status).ec != std::errc())
        throw Error(Error::Kind::protocol, "The host's answer has no status code.");
    std::optional<std::size_t> content_length;
    bool chunked = false;
    std::string_view rest = line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
    while (!rest.empty()) {
        auto e = rest.find("\r\n");
        std::string_view line = rest.substr(0, e);
        rest = e == std::string_view::npos ? std::string_view() : rest.substr(e + 2);
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        std::string name = lower(line.substr(0, colon));
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        if (name == "content-length") {
            std::size_t n = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), n).ec != std::errc() || n > max_response)
                throw Error(Error::Kind::protocol, "The host's answer has a bad Content-Length.");
            content_length = n;
        } else if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) {
            chunked = true;
        }
        response.headers.emplace_back(std::move(name), std::string(value));
    }
    std::string_view body = data.substr(*head_end);
    if (chunked) {
        std::string out;
        for (;;) {
            auto e = body.find("\r\n");
            if (e == std::string_view::npos) break;
            std::size_t size = 0;
            std::string_view digits = body.substr(0, e);
            if (auto semi = digits.find(';'); semi != std::string_view::npos) digits = digits.substr(0, semi);
            if (std::from_chars(digits.data(), digits.data() + digits.size(), size, 16).ec != std::errc() ||
                size > max_response)
                throw Error(Error::Kind::protocol, "The host's answer has a bad chunk.");
            if (size == 0) {
                response.body = std::move(out);
                return response;
            }
            if (body.size() < e + 2 + size + 2) break;
            out.append(body.substr(e + 2, size));
            body = body.substr(e + 2 + size + 2);
        }
        if (eof) throw Error(Error::Kind::protocol, "The host's answer was cut off.");
        return std::nullopt;
    }
    if (content_length) {
        if (body.size() >= *content_length) {
            response.body = std::string(body.substr(0, *content_length));
            return response;
        }
        if (eof) throw Error(Error::Kind::protocol, "The host's answer was cut off.");
        return std::nullopt;
    }
    if (!eof) return std::nullopt;
    response.body = std::string(body);
    return response;
}

Response fetch(std::string_view host, std::uint16_t port, const Request& request, const Tls* tls, Canceller* cancel) {
    auto deadline = clock::now() + request.timeout;
    Connection c(host, port, deadline, cancel);
    if (tls) c.start_tls(*tls);

    std::string text = std::format("{} {} HTTP/1.1\r\nHost: {}:{}\r\nConnection: close\r\n", request.method,
                                   request.target, host.find(':') != std::string_view::npos
                                                       ? "[" + std::string(host) + "]"
                                                       : std::string(host),
                                   port);
    for (const auto& [name, value] : request.headers) text += std::format("{}: {}\r\n", name, value);
    if (!request.body.empty() || request.method != "GET")
        text += std::format("Content-Length: {}\r\n", request.body.size());
    text += "\r\n";
    text += request.body;

    // A host that refuses a client may answer and close before reading the
    // request; its answer still counts.
    std::optional<Error> send_error;
    try {
        c.send_all(text);
    } catch (const Error& e) {
        if (e.kind() != Error::Kind::io) throw;
        send_error = e;
    }

    std::string data;
    char buf[16384];
    for (;;) {
        std::size_t n = 0;
        try {
            n = c.receive(buf, sizeof(buf));
        } catch (const Error& e) {
            if (send_error && e.kind() == Error::Kind::io) throw *send_error;
            throw;
        }
        data.append(buf, n);
        if (data.size() > max_response) throw Error(Error::Kind::protocol, "The host's answer is too large.");
        if (auto r = parse_response(data, n == 0)) return std::move(*r);
    }
}

}  // namespace moonlight::http
