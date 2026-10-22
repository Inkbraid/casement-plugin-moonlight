// A loopback server for the HTTP client's tests: listens on 127.0.0.1 and
// hands each connection, plain or TLS, to a handler on the server's thread.

#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace test {

class Conn {
public:
    Conn(SOCKET s, SSL* ssl, const std::atomic<bool>& stop) : s_(s), ssl_(ssl), stop_(stop) {}

    /// Reads the request head (and a Content-Length body); empty when the
    /// client went away.
    std::string read_request() {
        std::string data;
        char buf[4096];
        for (;;) {
            auto end = data.find("\r\n\r\n");
            if (end != std::string::npos) {
                auto cl = data.find("Content-Length: ");
                std::size_t length = cl != std::string::npos && cl < end ? std::stoul(data.substr(cl + 16)) : 0;
                if (data.size() >= end + 4 + length) return data;
            }
            int n = receive(buf, sizeof(buf));
            if (n <= 0) return data;
            data.append(buf, static_cast<std::size_t>(n));
        }
    }

    void write(std::string_view text) {
        if (ssl_) SSL_write(ssl_, text.data(), static_cast<int>(text.size()));
        else send(s_, text.data(), static_cast<int>(text.size()), 0);
    }

    /// Waits until the client closes the connection (or the server stops).
    void wait_for_close() {
        char buf[256];
        while (receive(buf, sizeof(buf)) > 0) {
        }
    }

    /// The client's certificate, for TLS connections.
    X509* peer_certificate() { return ssl_ ? SSL_get0_peer_certificate(ssl_) : nullptr; }

private:
    int receive(char* buf, int size) {
        for (;;) {
            if (stop_) return 0;
            if (!ssl_ || SSL_pending(ssl_) == 0) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(s_, &fds);
                timeval tv{0, 50000};
                int r = select(0, &fds, nullptr, nullptr, &tv);
                if (r == 0) continue;
                if (r < 0) return -1;
            }
            return ssl_ ? SSL_read(ssl_, buf, size) : recv(s_, buf, size, 0);
        }
    }

    SOCKET s_;
    SSL* ssl_;
    const std::atomic<bool>& stop_;
};

class Server {
public:
    using Handler = std::function<void(Conn&)>;

    /// With a certificate and key the server speaks TLS and asks for a
    /// client certificate.
    explicit Server(Handler handler, std::string_view cert_pem = {}, std::string_view key_pem = {})
        : handler_(std::move(handler)) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int len = sizeof(addr);
        getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(listen_, 8);
        if (!cert_pem.empty()) {
            ctx_ = SSL_CTX_new(TLS_server_method());
            BIO* b = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
            X509* cert = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
            BIO_free(b);
            b = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
            EVP_PKEY* key = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
            BIO_free(b);
            SSL_CTX_use_certificate(ctx_, cert);
            SSL_CTX_use_PrivateKey(ctx_, key);
            X509_free(cert);
            EVP_PKEY_free(key);
            // Like Sunshine: ask for a client certificate, decide later.
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });
        }
        thread_ = std::thread([this] { serve(); });
    }

    ~Server() {
        stop_ = true;
        thread_.join();
        closesocket(listen_);
        if (ctx_) SSL_CTX_free(ctx_);
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }

private:
    void serve() {
        while (!stop_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(listen_, &fds);
            timeval tv{0, 50000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET s = accept(listen_, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            SSL* ssl = nullptr;
            if (ctx_) {
                ssl = SSL_new(ctx_);
                SSL_set_fd(ssl, static_cast<int>(s));
                if (SSL_accept(ssl) != 1) {
                    ERR_clear_error();
                    SSL_free(ssl);
                    closesocket(s);
                    continue;
                }
            }
            Conn c(s, ssl, stop_);
            handler_(c);
            if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
            closesocket(s);
        }
    }

    Handler handler_;
    SOCKET listen_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    SSL_CTX* ctx_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace test
