// GameStream pairing, step for step as moonlight-qt's NvPairingManager
// does it. The user types a 4-digit PIN into the host; client and host
// derive an AES key from it and prove to each other that they know it,
// exchange certificates, and sign secrets with their keys:
//
//   1. getservercert       salt and our certificate; the host answers once
//                           the PIN has been entered and sends its own
//   2. clientchallenge     our random challenge, encrypted
//   3. serverchallengeresp our answer to the host's challenge; the host
//                           sends its secret, signed. A wrong PIN shows here.
//   4. clientpairingsecret our secret, signed
//   5. pairchallenge       over HTTPS with both certificates
//
// Hosts of generation 7 and later (all Sunshine versions) hash with
// SHA-256, older GeForce Experience hosts with SHA-1.

#pragma once

#include <openssl/types.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moonlight {

namespace pairing {

enum class Hash { sha1, sha256 };

/// SHA-256 for generation 7 and later, SHA-1 before.
Hash hash_for_generation(int generation);

std::vector<std::uint8_t> hash(Hash algorithm, std::span<const std::uint8_t> data);

/// The AES-128 key: the first 16 bytes of hash(salt + PIN).
std::array<std::uint8_t, 16> aes_key(Hash algorithm, std::span<const std::uint8_t> salt, std::string_view pin);

/// AES-128-ECB without padding; the length must be a multiple of 16.
std::vector<std::uint8_t> encrypt(std::span<const std::uint8_t> plain, const std::array<std::uint8_t, 16>& key);
std::vector<std::uint8_t> decrypt(std::span<const std::uint8_t> cipher, const std::array<std::uint8_t, 16>& key);

/// The signature bytes of a certificate (its signatureValue).
std::vector<std::uint8_t> certificate_signature(X509* certificate);

/// SHA-256 signatures with the certificate's key, as both sides use them
/// whatever the hash for the rest.
std::vector<std::uint8_t> sign(EVP_PKEY* key, std::span<const std::uint8_t> data);
bool verify(X509* certificate, std::span<const std::uint8_t> data, std::span<const std::uint8_t> signature);

}  // namespace pairing

enum class Channel { http, https };

/// Sends one request (`command` is "pair" or "unpair") with these
/// arguments, to which the client adds uniqueid and uuid, and returns the
/// reply's XML. For HTTPS the host must present `pinned_pem`. Throws on
/// network errors.
using PairRequest = std::function<std::string(Channel channel, std::string_view command, const std::string& arguments,
                                              std::chrono::milliseconds timeout, const std::string& pinned_pem)>;

/// Random bytes: OpenSSL's generator, or fixed values in tests.
using RandomBytes = std::function<void(std::span<std::uint8_t>)>;
void openssl_random(std::span<std::uint8_t> out);

struct PairResult {
    enum class Outcome { paired, wrong_pin, already_in_progress, failed };
    Outcome outcome = Outcome::failed;
    /// The host's certificate (PEM), when paired.
    std::string certificate_pem;
    /// Why it failed, in a sentence for the user.
    std::string message;
};

struct PairClient {
    std::string certificate_pem;
    X509* certificate = nullptr;
    EVP_PKEY* key = nullptr;
};

/// How long the host may take to show that the PIN was entered.
inline constexpr std::chrono::minutes pin_entry_timeout{5};

/// Pairs with a host. Host errors (a status other than 200) come back as
/// `failed` with the host's message; network errors are thrown.
PairResult pair(const PairRequest& request, const PairClient& client, int server_generation, std::string_view pin,
                const RandomBytes& random = openssl_random);

/// A random 4-digit PIN.
std::string random_pin(const RandomBytes& random = openssl_random);

}  // namespace moonlight
