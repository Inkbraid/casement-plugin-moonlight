#include "moonlight/identity.h"

#include <windows.h>
#include <dpapi.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "casement/log.h"
#include "casement/protocol.h"
#include "casement/text.h"

namespace fs = std::filesystem;

namespace moonlight {

namespace {

using casement::json;

/// Serialises changes to the data folder between plugin processes: two
/// tabs may connect, and pair, at the same time.
class DataLock {
public:
    DataLock() : mutex_(CreateMutexW(nullptr, FALSE, L"Local\\CasementMoonlightData")) {
        if (!mutex_) return;
        DWORD r = WaitForSingleObject(mutex_, 15000);
        owned_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        if (!owned_) casement::log::warn("the data folder is busy; going ahead without the lock");
    }
    ~DataLock() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    DataLock(const DataLock&) = delete;
    DataLock& operator=(const DataLock&) = delete;

private:
    HANDLE mutex_;
    bool owned_ = false;
};

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/// Replaces the file in one step, so a crash never leaves half a file.
void write_file(const fs::path& path, std::string_view data) {
    fs::path temp = path;
    temp += L".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw std::runtime_error(std::format("can't write {}", casement::narrow(temp.native())));
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error(std::format("can't replace {}: {}", casement::narrow(path.native()),
                                             casement::system_message(GetLastError())));
}

struct BioFree {
    void operator()(BIO* b) const { BIO_free(b); }
};

std::string bio_text(BIO* bio) {
    char* data = nullptr;
    long n = BIO_get_mem_data(bio, &data);
    return n > 0 ? std::string(data, static_cast<std::size_t>(n)) : std::string();
}

void wipe(std::string& s) {
    if (!s.empty()) OPENSSL_cleanse(s.data(), s.size());
    s.clear();
}

}  // namespace

fs::path data_folder() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"CASEMENT_MOONLIGHT_DATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf);
    PWSTR local = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) base = local;
    CoTaskMemFree(local);
    if (base.empty()) throw std::runtime_error("Windows didn't say where the local application data folder is.");
    return base / L"Casement" / L"plugin-data" / L"moonlight";
}

std::vector<std::uint8_t> protect(std::span<const std::uint8_t> data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Casement Moonlight client key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &out))
        throw std::runtime_error("can't protect the client key: " + casement::system_message(GetLastError()));
    std::vector<std::uint8_t> result(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return result;
}

std::optional<std::vector<std::uint8_t>> unprotect(std::span<const std::uint8_t> data) {
    DATA_BLOB in{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::vector<std::uint8_t> result(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

Identity::Identity(Identity&& other) noexcept { *this = std::move(other); }

Identity& Identity::operator=(Identity&& other) noexcept {
    if (this != &other) {
        X509_free(certificate_);
        EVP_PKEY_free(key_);
        certificate_pem_ = std::move(other.certificate_pem_);
        unique_id_ = std::move(other.unique_id_);
        certificate_ = std::exchange(other.certificate_, nullptr);
        key_ = std::exchange(other.key_, nullptr);
    }
    return *this;
}

Identity::~Identity() {
    X509_free(certificate_);
    EVP_PKEY_free(key_);
}

Identity Identity::from_pem(std::string certificate_pem, const std::string& key_pem, std::string unique_id) {
    Identity id;
    std::unique_ptr<BIO, BioFree> cert_bio(
        BIO_new_mem_buf(certificate_pem.data(), static_cast<int>(certificate_pem.size())));
    std::unique_ptr<BIO, BioFree> key_bio(BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size())));
    id.certificate_ = PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr);
    id.key_ = PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr);
    if (!id.certificate_ || !id.key_ || X509_check_private_key(id.certificate_, id.key_) != 1)
        throw std::runtime_error("the stored client certificate and key don't belong together");
    id.certificate_pem_ = std::move(certificate_pem);
    id.unique_id_ = std::move(unique_id);
    return id;
}

Identity Identity::generate() {
    // As Moonlight does it: RSA 2048, SHA-256, "NVIDIA GameStream Client",
    // valid for 20 years. GeForce Experience expected exactly this; Sunshine
    // accepts any certificate.
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (!key) throw std::runtime_error("can't make an RSA key");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_owner(key, &EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), &X509_free);
    X509_set_version(cert.get(), 2);
    std::uint64_t serial = 0;
    RAND_bytes(reinterpret_cast<unsigned char*>(&serial), sizeof(serial));
    ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()), serial >> 1);  // positive
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 365 * 20);
    X509_set_pubkey(cert.get(), key);
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("NVIDIA GameStream Client"), -1, -1, 0);
    X509_set_subject_name(cert.get(), name);
    X509_set_issuer_name(cert.get(), name);
    X509_NAME_free(name);
    if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) throw std::runtime_error("can't sign the client certificate");

    std::unique_ptr<BIO, BioFree> cert_bio(BIO_new(BIO_s_mem()));
    std::unique_ptr<BIO, BioFree> key_bio(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(cert_bio.get(), cert.get());
    PEM_write_bio_PrivateKey(key_bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
    std::string key_pem = bio_text(key_bio.get());

    std::uint64_t uid = 0;
    RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof(uid));
    Identity id = from_pem(bio_text(cert_bio.get()), key_pem, std::format("{:016x}", uid));
    wipe(key_pem);
    return id;
}

Identity Identity::load(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    DataLock lock;
    fs::path file = folder / L"client.json";
    if (auto text = read_file(file)) {
        try {
            json j = json::parse(*text);
            auto blob = casement::base64_decode(j.at("key").get<std::string>());
            auto plain = blob ? unprotect(*blob) : std::nullopt;
            if (!plain) throw std::runtime_error("the client key can't be unlocked for this Windows user");
            std::string key_pem(plain->begin(), plain->end());
            SecureZeroMemory(plain->data(), plain->size());
            Identity id =
                from_pem(j.at("certificate").get<std::string>(), key_pem, j.at("uniqueid").get<std::string>());
            wipe(key_pem);
            return id;
        } catch (const std::exception& e) {
            casement::log::warn("{} can't be used ({}); making a new client certificate, hosts must be paired again",
                                casement::narrow(file.native()), e.what());
        }
    }

    Identity id = generate();
    std::unique_ptr<BIO, BioFree> key_bio(BIO_new(BIO_s_mem()));
    PEM_write_bio_PrivateKey(key_bio.get(), id.key(), nullptr, nullptr, 0, nullptr, nullptr);
    std::string key_pem = bio_text(key_bio.get());
    auto blob = protect(std::span(reinterpret_cast<const std::uint8_t*>(key_pem.data()), key_pem.size()));
    wipe(key_pem);
    json j{{"uniqueid", id.unique_id()}, {"certificate", id.certificate_pem()}, {"key", casement::base64_encode(blob)}};
    write_file(file, j.dump(2));
    casement::log::info("made a new client certificate in {}", casement::narrow(folder.native()));
    return id;
}

std::optional<std::string> HostStore::certificate(const std::string& host_id) const {
    DataLock lock;
    auto text = read_file(folder_ / L"hosts.json");
    if (!text) return std::nullopt;
    json j = json::parse(*text, nullptr, false);
    if (!j.is_object() || !j.contains("hosts") || !j["hosts"].is_object() || !j["hosts"].contains(host_id))
        return std::nullopt;
    const json& h = j["hosts"][host_id];
    if (!h.is_object() || !h.contains("certificate") || !h["certificate"].is_string()) return std::nullopt;
    return h["certificate"].get<std::string>();
}

void HostStore::pin(const std::string& host_id, const std::string& certificate_pem, const std::string& name,
                    const std::string& address) {
    std::error_code ec;
    fs::create_directories(folder_, ec);
    DataLock lock;
    fs::path file = folder_ / L"hosts.json";
    json j = json::object();
    if (auto text = read_file(file)) j = json::parse(*text, nullptr, false);
    if (!j.is_object()) j = json::object();
    if (!j.contains("hosts") || !j["hosts"].is_object()) j["hosts"] = json::object();
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    j["hosts"][host_id] = json{{"name", name}, {"address", address}, {"certificate", certificate_pem},
                               {"paired", std::format("{:%FT%TZ}", now)}};
    write_file(file, j.dump(2));
}

void HostStore::forget(const std::string& host_id) {
    DataLock lock;
    fs::path file = folder_ / L"hosts.json";
    auto text = read_file(file);
    if (!text) return;
    json j = json::parse(*text, nullptr, false);
    if (!j.is_object() || !j.contains("hosts") || !j["hosts"].is_object()) return;
    if (j["hosts"].erase(host_id) > 0) write_file(file, j.dump(2));
}

}  // namespace moonlight
