#include "moonlight/pairing.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "casement/log.h"
#include "casement/text.h"
#include "moonlight/xml.h"

namespace moonlight {

namespace pairing {

Hash hash_for_generation(int generation) { return generation >= 7 ? Hash::sha256 : Hash::sha1; }

std::vector<std::uint8_t> hash(Hash algorithm, std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(algorithm == Hash::sha256 ? SHA256_DIGEST_LENGTH : SHA_DIGEST_LENGTH);
    unsigned int n = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &n, algorithm == Hash::sha256 ? EVP_sha256() : EVP_sha1(),
               nullptr);
    out.resize(n);
    return out;
}

std::array<std::uint8_t, 16> aes_key(Hash algorithm, std::span<const std::uint8_t> salt, std::string_view pin) {
    std::vector<std::uint8_t> salted(salt.begin(), salt.end());
    salted.insert(salted.end(), pin.begin(), pin.end());
    std::vector<std::uint8_t> h = hash(algorithm, salted);
    std::array<std::uint8_t, 16> key{};
    std::copy_n(h.begin(), 16, key.begin());
    return key;
}

namespace {

std::vector<std::uint8_t> ecb(bool encrypting, std::span<const std::uint8_t> in,
                              const std::array<std::uint8_t, 16>& key) {
    if (in.size() % 16 != 0) throw std::invalid_argument("AES-ECB needs whole blocks");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
    std::vector<std::uint8_t> out(in.size());
    int n = 0;
    if (!ctx ||
        EVP_CipherInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr, encrypting ? 1 : 0) != 1 ||
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1 ||
        EVP_CipherUpdate(ctx.get(), out.data(), &n, in.data(), static_cast<int>(in.size())) != 1)
        throw std::runtime_error("AES failed");
    out.resize(static_cast<std::size_t>(n));
    return out;
}

}  // namespace

std::vector<std::uint8_t> encrypt(std::span<const std::uint8_t> plain, const std::array<std::uint8_t, 16>& key) {
    return ecb(true, plain, key);
}

std::vector<std::uint8_t> decrypt(std::span<const std::uint8_t> cipher, const std::array<std::uint8_t, 16>& key) {
    return ecb(false, cipher, key);
}

std::vector<std::uint8_t> certificate_signature(X509* certificate) {
    const ASN1_BIT_STRING* signature = nullptr;
    X509_get0_signature(&signature, nullptr, certificate);
    if (!signature) return {};
    const unsigned char* p = ASN1_STRING_get0_data(signature);
    return std::vector<std::uint8_t>(p, p + ASN1_STRING_length(signature));
}

std::vector<std::uint8_t> sign(EVP_PKEY* key, std::span<const std::uint8_t> data) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    std::size_t length = 0;
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) != 1 ||
        EVP_DigestSignFinal(ctx.get(), nullptr, &length) != 1)
        throw std::runtime_error("can't sign with the client key");
    std::vector<std::uint8_t> out(length);
    if (EVP_DigestSignFinal(ctx.get(), out.data(), &length) != 1)
        throw std::runtime_error("can't sign with the client key");
    out.resize(length);
    return out;
}

bool verify(X509* certificate, std::span<const std::uint8_t> data, std::span<const std::uint8_t> signature) {
    EVP_PKEY* key = X509_get0_pubkey(certificate);
    if (!key) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) == 1 &&
           EVP_DigestVerifyUpdate(ctx.get(), data.data(), data.size()) == 1 &&
           EVP_DigestVerifyFinal(ctx.get(), signature.data(), signature.size()) == 1;
}

}  // namespace pairing

void openssl_random(std::span<std::uint8_t> out) {
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) throw std::runtime_error("no random numbers");
}

std::string random_pin(const RandomBytes& random) {
    for (;;) {
        std::uint8_t b[2];
        random(b);
        unsigned v = b[0] << 8 | b[1];
        // 60000 is a multiple of 10000, so every PIN is equally likely.
        if (v < 60000) return std::format("{:04}", v % 10000);
    }
}

namespace {

using namespace std::chrono_literals;
using pairing::Hash;
using Outcome = PairResult::Outcome;

constexpr auto request_timeout = 5s;

std::vector<std::uint8_t> bytes(std::string_view s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

std::vector<std::uint8_t> concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (auto p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

/// The text of `name` in a successful reply, decoded from hex (empty when
/// missing or not hex).
std::vector<std::uint8_t> hex_field(const Reply& r, std::string_view name) {
    auto v = casement::parse_hex(r.get(name));
    return v ? *v : std::vector<std::uint8_t>();
}

struct X509Free {
    void operator()(X509* p) const { X509_free(p); }
};

class Pairing {
public:
    Pairing(const PairRequest& request, const PairClient& client, int generation, const RandomBytes& random)
        : request_(request), client_(client), algorithm_(pairing::hash_for_generation(generation)),
          hash_length_(algorithm_ == Hash::sha256 ? 32 : 20), random_(random) {
        casement::log::info("pairing with a generation {} host ({})", generation,
                            algorithm_ == Hash::sha256 ? "SHA-256" : "SHA-1");
    }

    PairResult run(std::string_view pin) {
        std::vector<std::uint8_t> salt(16);
        random_(salt);
        auto key = pairing::aes_key(algorithm_, salt, pin);

        // 1. Our certificate for theirs; the host holds this request until
        // the PIN has been entered.
        Reply cert_reply = call(Channel::http,
                                "devicename=roth&updateState=1&phrase=getservercert&salt=" + casement::hex(salt) +
                                    "&clientcert=" + casement::hex(bytes(client_.certificate_pem)),
                                pin_entry_timeout);
        if (cert_reply.get("paired") != "1") return failed("The host didn't start pairing.", false);
        auto server_pem_bytes = hex_field(cert_reply, "plaincert");
        if (server_pem_bytes.empty())
            return finish(Outcome::already_in_progress, "The host is already pairing with another device.");
        std::string server_pem(server_pem_bytes.begin(), server_pem_bytes.end());
        BIO* bio = BIO_new_mem_buf(server_pem.data(), static_cast<int>(server_pem.size()));
        std::unique_ptr<X509, X509Free> server_cert(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
        BIO_free(bio);
        if (!server_cert) return failed("The host sent a certificate that can't be read.");

        // 2. Our challenge; the host answers with a hash over it, its
        // certificate's signature and its secret, plus a challenge of its own.
        std::vector<std::uint8_t> challenge(16);
        random_(challenge);
        Reply challenge_reply =
            call(Channel::http,
                 "devicename=roth&updateState=1&clientchallenge=" + casement::hex(pairing::encrypt(challenge, key)),
                 request_timeout);
        if (challenge_reply.get("paired") != "1") return failed("The host stopped pairing at the challenge.");
        auto response_cipher = hex_field(challenge_reply, "challengeresponse");
        if (response_cipher.empty() || response_cipher.size() % 16 != 0)
            return failed("The host's challenge response can't be read.");
        auto response = pairing::decrypt(response_cipher, key);
        if (response.size() < hash_length_ + 16) return failed("The host's challenge response is too short.");
        std::span<const std::uint8_t> server_response(response.data(), hash_length_);
        std::span<const std::uint8_t> server_challenge(response.data() + hash_length_, 16);

        // 3. Our answer to its challenge, with our secret in the hash.
        std::vector<std::uint8_t> client_secret(16);
        random_(client_secret);
        auto answer = pairing::hash(
            algorithm_, concat({server_challenge, pairing::certificate_signature(client_.certificate), client_secret}));
        answer.resize(32);  // SHA-1 is padded to two blocks
        Reply secret_reply =
            call(Channel::http,
                 "devicename=roth&updateState=1&serverchallengeresp=" + casement::hex(pairing::encrypt(answer, key)),
                 request_timeout);
        if (secret_reply.get("paired") != "1") return failed("The host stopped pairing at the challenge response.");
        auto pairing_secret = hex_field(secret_reply, "pairingsecret");
        if (pairing_secret.size() <= 16) return failed("The host's pairing secret can't be read.");
        std::span<const std::uint8_t> server_secret(pairing_secret.data(), 16);
        std::span<const std::uint8_t> server_signature(pairing_secret.data() + 16, pairing_secret.size() - 16);
        if (!pairing::verify(server_cert.get(), server_secret, server_signature))
            return failed("The host's pairing secret isn't signed by its certificate. Someone may be intercepting "
                          "the connection.");
        auto expected = pairing::hash(
            algorithm_, concat({challenge, pairing::certificate_signature(server_cert.get()), server_secret}));
        std::vector<std::uint8_t> received(server_response.begin(), server_response.end());

        // 4. Our secret, signed. Sent even after a wrong PIN, which the host
        // then refuses too: that ends its half of the attempt, and without
        // that Sunshine refuses the next attempt from this client once.
        auto client_pairing_secret = concat({client_secret, pairing::sign(client_.key, client_secret)});
        std::string secret_arguments =
            "devicename=roth&updateState=1&clientpairingsecret=" + casement::hex(client_pairing_secret);
        if (expected != received) {
            try {
                request_(Channel::http, "pair", secret_arguments, request_timeout, {});
            } catch (const std::exception& e) {
                casement::log::debug("closing the host's pairing attempt: {}", e.what());
            }
            return finish(Outcome::wrong_pin, "The PIN entered on the host was wrong.");
        }
        Reply final_reply = call(Channel::http, secret_arguments, request_timeout);
        if (final_reply.get("paired") != "1") return failed("The host refused our pairing secret.");

        // 5. Both certificates over HTTPS.
        Reply https_reply =
            call(Channel::https, "devicename=roth&updateState=1&phrase=pairchallenge", request_timeout, server_pem);
        if (https_reply.get("paired") != "1") return failed("The host didn't accept the paired connection.");

        PairResult r;
        r.outcome = Outcome::paired;
        r.certificate_pem = std::move(server_pem);
        return r;
    }

private:
    Reply call(Channel channel, const std::string& arguments, std::chrono::milliseconds timeout,
               const std::string& pinned = {}) {
        return Reply::parse(request_(channel, "pair", arguments, timeout, pinned));
    }

    /// Moonlight tells the host to drop the pairing after every failure but
    /// the first; Sunshine ignores this, GeForce Experience needed it.
    void unpair() {
        try {
            request_(Channel::http, "unpair", "", request_timeout, {});
        } catch (const std::exception& e) {
            casement::log::debug("unpair: {}", e.what());
        }
    }

    PairResult failed(std::string message, bool tell_host = true) {
        return finish(Outcome::failed, std::move(message), tell_host);
    }

    PairResult finish(Outcome outcome, std::string message, bool tell_host = true) {
        casement::log::warn("pairing: {}", message);
        if (tell_host) unpair();
        PairResult r;
        r.outcome = outcome;
        r.message = std::move(message);
        return r;
    }

    const PairRequest& request_;
    const PairClient& client_;
    Hash algorithm_;
    std::size_t hash_length_;
    const RandomBytes& random_;
};

}  // namespace

PairResult pair(const PairRequest& request, const PairClient& client, int server_generation, std::string_view pin,
                const RandomBytes& random) {
    Pairing p(request, client, server_generation, random);
    try {
        return p.run(pin);
    } catch (const HostError& e) {
        PairResult r;
        r.message = e.host_message().empty() ? std::format("The host refused pairing (status {}).", e.code())
                                             : std::format("The host refused pairing: {}", e.host_message());
        casement::log::warn("pairing: {}", r.message);
        return r;
    } catch (const ReplyError& e) {
        PairResult r;
        r.message = e.what();
        return r;
    }
}

}  // namespace moonlight
