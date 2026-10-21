// Pairing: the client's messages byte for byte against vectors derived
// independently from moonlight-qt's algorithm (tools/pairing-vectors.py),
// and whole pairings against a host written after Sunshine's nvhttp.cpp.

#include "check.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "casement/text.h"
#include "moonlight/pairing.h"
#include "pairing_vectors.h"

using namespace moonlight;
using namespace std::chrono_literals;

namespace {

std::vector<std::uint8_t> unhex(std::string_view s) {
    auto v = casement::parse_hex(s);
    return v ? *v : std::vector<std::uint8_t>();
}

std::vector<std::uint8_t> bytes(std::string_view s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

/// A certificate and key from PEM text.
struct Credentials {
    Credentials(std::string_view cert_pem, std::string_view key_pem) : pem(cert_pem) {
        BIO* b = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
        cert = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
        BIO_free(b);
        b = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
        key = PEM_read_bio_PrivateKey(b, nullptr, nullptr, nullptr);
        BIO_free(b);
    }
    ~Credentials() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;
    PairClient client() const { return PairClient{pem, cert, key}; }

    std::string pem;
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
};

/// Random bytes from fixed values, one per request, in order.
RandomBytes fixed(std::vector<std::vector<std::uint8_t>> values) {
    auto queue = std::make_shared<std::deque<std::vector<std::uint8_t>>>(values.begin(), values.end());
    return [queue](std::span<std::uint8_t> out) {
        REQUIRE(!queue->empty());
        auto v = queue->front();
        queue->pop_front();
        REQUIRE(v.size() == out.size());
        std::copy(v.begin(), v.end(), out.begin());
    };
}

std::string reply(const std::string& body, int status = 200, const std::string& message = {}) {
    std::string root = "<root status_code=\"" + std::to_string(status) + "\"";
    if (!message.empty()) root += " status_message=\"" + message + "\"";
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>" + root + ">" + body + "</root>";
}

struct Expected {
    std::string_view aes_key, getservercert, plaincert, clientchallenge, challengeresponse, serverchallengeresp,
        pairingsecret, clientpairingsecret, pairchallenge;
};

constexpr Expected sha256{vectors::sha256::aes_key,           vectors::sha256::getservercert,
                          vectors::sha256::plaincert,         vectors::sha256::clientchallenge,
                          vectors::sha256::challengeresponse, vectors::sha256::serverchallengeresp,
                          vectors::sha256::pairingsecret,     vectors::sha256::clientpairingsecret,
                          vectors::sha256::pairchallenge};
constexpr Expected sha1{vectors::sha1::aes_key,           vectors::sha1::getservercert,
                        vectors::sha1::plaincert,         vectors::sha1::clientchallenge,
                        vectors::sha1::challengeresponse, vectors::sha1::serverchallengeresp,
                        vectors::sha1::pairingsecret,     vectors::sha1::clientpairingsecret,
                        vectors::sha1::pairchallenge};

/// Plays the host's side from the vectors and checks each request.
void scripted_pairing(const Expected& v, int generation) {
    Credentials client(vectors::client_cert, vectors::client_key);
    int step = 0;
    PairRequest host = [&](Channel channel, std::string_view command, const std::string& args,
                           std::chrono::milliseconds, const std::string& pinned) -> std::string {
        ++step;
        CHECK_EQ(std::string(command), std::string("pair"));
        switch (step) {
            case 1:
                CHECK(channel == Channel::http);
                CHECK_EQ(args, std::string(v.getservercert));
                return reply("<paired>1</paired><plaincert>" + std::string(v.plaincert) + "</plaincert>");
            case 2:
                CHECK_EQ(args, std::string(v.clientchallenge));
                return reply("<paired>1</paired><challengeresponse>" + std::string(v.challengeresponse) +
                             "</challengeresponse>");
            case 3:
                CHECK_EQ(args, std::string(v.serverchallengeresp));
                return reply("<paired>1</paired><pairingsecret>" + std::string(v.pairingsecret) + "</pairingsecret>");
            case 4:
                CHECK_EQ(args, std::string(v.clientpairingsecret));
                return reply("<paired>1</paired>");
            case 5:
                CHECK(channel == Channel::https);
                CHECK_EQ(args, std::string(v.pairchallenge));
                CHECK_EQ(pinned, std::string(vectors::host_cert));
                return reply("<paired>1</paired>");
        }
        CHECK(false);
        return reply("", 500);
    };
    auto random = fixed({unhex(vectors::salt), unhex(vectors::client_challenge), unhex(vectors::client_secret)});
    PairResult r = pair(host, client.client(), generation, vectors::pin, random);
    CHECK(r.outcome == PairResult::Outcome::paired);
    CHECK_EQ(r.certificate_pem, std::string(vectors::host_cert));
    CHECK_EQ(step, 5);
}

/// A host doing its side as Sunshine's nvhttp.cpp does, with OpenSSL
/// directly rather than the plugin's helpers.
class FakeHost {
public:
    FakeHost(std::string entered_pin, bool sha256_hash)
        : pin_(std::move(entered_pin)), md_(sha256_hash ? EVP_sha256() : EVP_sha1()),
          host_(vectors::host_cert, vectors::host_key) {}

    PairRequest transport() {
        return [this](Channel channel, std::string_view command, const std::string& args, std::chrono::milliseconds,
                      const std::string& pinned) { return handle(channel, command, args, pinned); };
    }

    // What the host saw.
    bool client_paired = false;
    int pairing_secrets = 0;
    int unpairs = 0;
    // Misbehaviour to test.
    bool send_no_certificate = false;
    bool sign_with_wrong_key = false;
    bool refuse = false;

private:
    std::string handle(Channel channel, std::string_view command, const std::string& args, const std::string& pinned) {
        if (command == "unpair") {
            ++unpairs;
            return reply("", 404);
        }
        auto a = parse(args);
        if (refuse) return reply("<paired>0</paired>", 403, "Pairing is disabled for this instance");
        if (a.contains("phrase") && a["phrase"] == "getservercert") {
            auto salt = unhex(a["salt"]);
            auto pem = unhex(a["clientcert"]);
            BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
            client_cert_.reset(PEM_read_bio_X509(b, nullptr, nullptr, nullptr));
            BIO_free(b);
            auto salted = salt;
            salted.insert(salted.end(), pin_.begin(), pin_.end());
            auto h = digest(salted);
            key_.assign(h.begin(), h.begin() + 16);
            if (send_no_certificate) return reply("<paired>1</paired>");
            return reply("<paired>1</paired><plaincert>" + casement::hex(bytes(host_.pem)) + "</plaincert>");
        }
        if (a.contains("phrase") && a["phrase"] == "pairchallenge") {
            CHECK(channel == Channel::https);
            return reply(pinned == host_.pem ? "<paired>1</paired>" : "<paired>0</paired>");
        }
        if (a.contains("clientchallenge")) {
            auto challenge = aes(false, unhex(a["clientchallenge"]));
            secret_.resize(16);
            server_challenge_.resize(16);
            RAND_bytes(secret_.data(), 16);
            RAND_bytes(server_challenge_.data(), 16);
            auto data = challenge;
            auto sig = signature(host_.cert);
            data.insert(data.end(), sig.begin(), sig.end());
            data.insert(data.end(), secret_.begin(), secret_.end());
            auto plain = digest(data);
            plain.insert(plain.end(), server_challenge_.begin(), server_challenge_.end());
            plain.resize((plain.size() + 15) / 16 * 16);
            return reply("<paired>1</paired><challengeresponse>" + casement::hex(aes(true, plain)) +
                         "</challengeresponse>");
        }
        if (a.contains("serverchallengeresp")) {
            client_hash_ = aes(false, unhex(a["serverchallengeresp"]));
            auto out = secret_;
            auto sig = sign(sign_with_wrong_key ? wrong_key() : host_.key, secret_);
            out.insert(out.end(), sig.begin(), sig.end());
            return reply("<paired>1</paired><pairingsecret>" + casement::hex(out) + "</pairingsecret>");
        }
        if (a.contains("clientpairingsecret")) {
            ++pairing_secrets;
            auto all = unhex(a["clientpairingsecret"]);
            std::vector<std::uint8_t> secret(all.begin(), all.begin() + 16), sig(all.begin() + 16, all.end());
            auto data = server_challenge_;
            auto csig = signature(client_cert_.get());
            data.insert(data.end(), csig.begin(), csig.end());
            data.insert(data.end(), secret.begin(), secret.end());
            auto h = digest(data);
            bool same = client_hash_.size() >= h.size() && std::equal(h.begin(), h.end(), client_hash_.begin());
            client_paired = same && verify(client_cert_.get(), secret, sig);
            return reply(client_paired ? "<paired>1</paired>" : "<paired>0</paired>");
        }
        return reply("", 404, "Invalid pairing request");
    }

    static std::map<std::string, std::string> parse(const std::string& args) {
        std::map<std::string, std::string> out;
        std::size_t p = 0;
        while (p <= args.size()) {
            auto amp = args.find('&', p);
            std::string pair = args.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
            auto eq = pair.find('=');
            out[pair.substr(0, eq)] = eq == std::string::npos ? "" : pair.substr(eq + 1);
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
        return out;
    }

    std::vector<std::uint8_t> digest(const std::vector<std::uint8_t>& data) {
        std::vector<std::uint8_t> out(EVP_MAX_MD_SIZE);
        unsigned n = 0;
        EVP_Digest(data.data(), data.size(), out.data(), &n, md_, nullptr);
        out.resize(n);
        return out;
    }

    std::vector<std::uint8_t> aes(bool encrypt, const std::vector<std::uint8_t>& in) {
        EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
        EVP_CipherInit_ex(c, EVP_aes_128_ecb(), nullptr, key_.data(), nullptr, encrypt ? 1 : 0);
        EVP_CIPHER_CTX_set_padding(c, 0);
        std::vector<std::uint8_t> out(in.size());
        int n = 0;
        EVP_CipherUpdate(c, out.data(), &n, in.data(), static_cast<int>(in.size()));
        EVP_CIPHER_CTX_free(c);
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

    static std::vector<std::uint8_t> signature(X509* cert) {
        const ASN1_BIT_STRING* s = nullptr;
        X509_get0_signature(&s, nullptr, cert);
        return std::vector<std::uint8_t>(s->data, s->data + s->length);
    }

    static std::vector<std::uint8_t> sign(EVP_PKEY* key, const std::vector<std::uint8_t>& data) {
        EVP_MD_CTX* c = EVP_MD_CTX_new();
        std::size_t n = 0;
        EVP_DigestSignInit(c, nullptr, EVP_sha256(), nullptr, key);
        EVP_DigestSign(c, nullptr, &n, data.data(), data.size());
        std::vector<std::uint8_t> out(n);
        EVP_DigestSign(c, out.data(), &n, data.data(), data.size());
        EVP_MD_CTX_free(c);
        out.resize(n);
        return out;
    }

    static bool verify(X509* cert, const std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& sig) {
        EVP_MD_CTX* c = EVP_MD_CTX_new();
        bool ok = EVP_DigestVerifyInit(c, nullptr, EVP_sha256(), nullptr, X509_get0_pubkey(cert)) == 1 &&
                  EVP_DigestVerify(c, sig.data(), sig.size(), data.data(), data.size()) == 1;
        EVP_MD_CTX_free(c);
        return ok;
    }

    EVP_PKEY* wrong_key() {
        if (!wrong_) wrong_ = std::make_unique<Credentials>(vectors::client_cert, vectors::client_key);
        return wrong_->key;
    }

    std::string pin_;
    const EVP_MD* md_;
    Credentials host_;
    std::unique_ptr<Credentials> wrong_;
    std::unique_ptr<X509, decltype(&X509_free)> client_cert_{nullptr, &X509_free};
    std::vector<std::uint8_t> key_, secret_, server_challenge_, client_hash_;
};

}  // namespace

TEST(pairing_hash_follows_generation) {
    CHECK(pairing::hash_for_generation(7) == pairing::Hash::sha256);
    CHECK(pairing::hash_for_generation(8) == pairing::Hash::sha256);
    CHECK(pairing::hash_for_generation(6) == pairing::Hash::sha1);
    CHECK(pairing::hash_for_generation(0) == pairing::Hash::sha1);
}

TEST(pairing_aes_key_vectors) {
    auto salt = unhex(vectors::salt);
    CHECK_EQ(casement::hex(pairing::aes_key(pairing::Hash::sha256, salt, vectors::pin)), std::string(sha256.aes_key));
    CHECK_EQ(casement::hex(pairing::aes_key(pairing::Hash::sha1, salt, vectors::pin)), std::string(sha1.aes_key));
}

TEST(pairing_aes_round_trip) {
    auto key = pairing::aes_key(pairing::Hash::sha256, unhex(vectors::salt), vectors::pin);
    auto plain = unhex(vectors::client_challenge);
    auto cipher = pairing::encrypt(plain, key);
    // The vector's clientchallenge argument ends with the encrypted challenge.
    std::string expected(sha256.clientchallenge.substr(sha256.clientchallenge.find("clientchallenge=") + 16));
    CHECK_EQ(casement::hex(cipher), expected);
    CHECK(pairing::decrypt(cipher, key) == plain);
}

TEST(pairing_signatures) {
    Credentials client(vectors::client_cert, vectors::client_key);
    Credentials host(vectors::host_cert, vectors::host_key);
    auto secret = unhex(vectors::client_secret);
    auto sig = pairing::sign(client.key, secret);
    CHECK_EQ(sig.size(), std::size_t{256});
    CHECK(pairing::verify(client.cert, secret, sig));
    CHECK(!pairing::verify(host.cert, secret, sig));
    sig[10] ^= 1;
    CHECK(!pairing::verify(client.cert, secret, sig));
    CHECK_EQ(pairing::certificate_signature(client.cert).size(), std::size_t{256});
}

TEST(pairing_messages_match_moonlight_sha256) { scripted_pairing(sha256, 7); }

TEST(pairing_messages_match_moonlight_sha1) { scripted_pairing(sha1, 6); }

TEST(pairing_with_host_succeeds) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("4821", true);
    PairResult r = pair(host.transport(), client.client(), 7, "4821");
    CHECK(r.outcome == PairResult::Outcome::paired);
    CHECK_EQ(r.certificate_pem, std::string(vectors::host_cert));
    CHECK(host.client_paired);
    CHECK_EQ(host.unpairs, 0);
}

TEST(pairing_with_old_host_uses_sha1) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("0007", false);
    PairResult r = pair(host.transport(), client.client(), 6, "0007");
    CHECK(r.outcome == PairResult::Outcome::paired);
    CHECK(host.client_paired);
}

TEST(pairing_wrong_pin) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("1234", true);  // the user typed another PIN
    PairResult r = pair(host.transport(), client.client(), 7, "4821");
    CHECK(r.outcome == PairResult::Outcome::wrong_pin);
    CHECK(!host.client_paired);
    // The host's half of the attempt was closed, and Moonlight's unpair sent.
    CHECK_EQ(host.pairing_secrets, 1);
    CHECK_EQ(host.unpairs, 1);
}

TEST(pairing_host_already_pairing) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("4821", true);
    host.send_no_certificate = true;
    PairResult r = pair(host.transport(), client.client(), 7, "4821");
    CHECK(r.outcome == PairResult::Outcome::already_in_progress);
    CHECK_EQ(host.unpairs, 1);
}

TEST(pairing_detects_a_forged_secret) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("4821", true);
    host.sign_with_wrong_key = true;
    PairResult r = pair(host.transport(), client.client(), 7, "4821");
    CHECK(r.outcome == PairResult::Outcome::failed);
    CHECK(r.message.find("intercepting") != std::string::npos);
    CHECK_EQ(host.pairing_secrets, 0);
}

TEST(pairing_refused_by_host) {
    Credentials client(vectors::client_cert, vectors::client_key);
    FakeHost host("4821", true);
    host.refuse = true;
    PairResult r = pair(host.transport(), client.client(), 7, "4821");
    CHECK(r.outcome == PairResult::Outcome::failed);
    CHECK(r.message.find("Pairing is disabled for this instance") != std::string::npos);
}

TEST(pairing_random_pin) {
    // Values from 60000 up are skipped so every PIN is equally likely.
    auto random = fixed({{0xEA, 0x60}, {0x12, 0x34}});
    CHECK_EQ(random_pin(random), std::string("4660"));
    CHECK_EQ(random_pin(fixed({{0x00, 0x07}})), std::string("0007"));
    for (int i = 0; i < 50; ++i) {
        std::string pin = random_pin();
        CHECK_EQ(pin.size(), std::size_t{4});
        CHECK(pin.find_first_not_of("0123456789") == std::string::npos);
    }
}
