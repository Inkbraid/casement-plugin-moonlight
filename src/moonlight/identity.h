// What the plugin keeps between connections, in
// %LOCALAPPDATA%\Casement\plugin-data\moonlight\:
//
//   client.json  the client's certificate, its private key (protected with
//                DPAPI for the current Windows user) and its unique ID
//   hosts.json   each paired host's certificate, by the host's uniqueid
//
// Protocol v1 doesn't name a data folder; proposal P3 would let Casement
// pass one in `connect`.

#pragma once

#include <openssl/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moonlight {

/// The plugin's data folder: CASEMENT_MOONLIGHT_DATA when set (tests,
/// portable setups), otherwise %LOCALAPPDATA%\Casement\plugin-data\moonlight.
std::filesystem::path data_folder();

/// DPAPI for the current user.
std::vector<std::uint8_t> protect(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t> data);

/// The client's certificate and key: one RSA 2048 self-signed certificate
/// per Windows user, made the way Moonlight makes its own. Pairing binds it
/// to a host.
class Identity {
public:
    /// Loads the identity from `folder`, creating it the first time (or
    /// when the stored one can't be read). Throws std::runtime_error.
    static Identity load(const std::filesystem::path& folder);
    /// A new identity that isn't stored anywhere.
    static Identity generate();

    Identity(Identity&& other) noexcept;
    Identity& operator=(Identity&& other) noexcept;
    ~Identity();

    const std::string& certificate_pem() const { return certificate_pem_; }
    X509* certificate() const { return certificate_; }
    EVP_PKEY* key() const { return key_; }
    /// The `uniqueid` the client sends with every request.
    const std::string& unique_id() const { return unique_id_; }

private:
    Identity() = default;
    static Identity from_pem(std::string certificate_pem, const std::string& key_pem, std::string unique_id);

    std::string certificate_pem_;
    std::string unique_id_;
    X509* certificate_ = nullptr;
    EVP_PKEY* key_ = nullptr;
};

/// The certificates of paired hosts, by the uniqueid each host reports.
class HostStore {
public:
    explicit HostStore(std::filesystem::path folder) : folder_(std::move(folder)) {}

    /// The pinned certificate (PEM), if the host was paired.
    std::optional<std::string> certificate(const std::string& host_id) const;
    void pin(const std::string& host_id, const std::string& certificate_pem, const std::string& name,
             const std::string& address);
    void forget(const std::string& host_id);

private:
    std::filesystem::path folder_;
};

}  // namespace moonlight
