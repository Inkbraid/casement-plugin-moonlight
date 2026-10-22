// The client certificate, its DPAPI-protected key and the pinned host
// certificates in the data folder.

#include "check.h"

#include <windows.h>

#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "moonlight/identity.h"

using namespace moonlight;
namespace fs = std::filesystem;

namespace {

fs::path temp_folder(const char* name) {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    fs::path p = fs::path(buf) / ("casement-moonlight-test-" + std::to_string(GetCurrentProcessId())) / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

TEST(identity_is_like_moonlights) {
    Identity id = Identity::generate();
    REQUIRE(id.certificate());
    REQUIRE(id.key());
    char name[256];
    X509_NAME_oneline(X509_get_subject_name(id.certificate()), name, sizeof(name));
    CHECK_EQ(std::string(name), std::string("/CN=NVIDIA GameStream Client"));
    CHECK_EQ(EVP_PKEY_get_bits(id.key()), 2048);
    CHECK_EQ(X509_get_signature_nid(id.certificate()), NID_sha256WithRSAEncryption);
    CHECK_EQ(X509_check_private_key(id.certificate(), id.key()), 1);
    int days = 0, seconds = 0;
    ASN1_TIME_diff(&days, &seconds, X509_get0_notBefore(id.certificate()), X509_get0_notAfter(id.certificate()));
    CHECK(days >= 365 * 20 - 1);
    CHECK(id.certificate_pem().starts_with("-----BEGIN CERTIFICATE-----\n"));
    CHECK_EQ(id.unique_id().size(), std::size_t{16});
    CHECK(id.unique_id() != Identity::generate().unique_id());
}

TEST(identity_is_kept) {
    fs::path folder = temp_folder("identity");
    Identity first = Identity::load(folder);
    std::string stored = read(folder / "client.json");
    // The key is only on disk protected for this Windows user.
    CHECK(stored.find("PRIVATE KEY") == std::string::npos);
    CHECK(stored.find(first.unique_id()) != std::string::npos);
    Identity second = Identity::load(folder);
    CHECK_EQ(second.certificate_pem(), first.certificate_pem());
    CHECK_EQ(second.unique_id(), first.unique_id());
    CHECK_EQ(X509_check_private_key(second.certificate(), second.key()), 1);

    // A damaged file gives a new identity rather than no connection.
    std::ofstream(folder / "client.json", std::ios::trunc) << "{\"uniqueid\": 1}";
    Identity third = Identity::load(folder);
    CHECK(third.certificate_pem() != first.certificate_pem());
    CHECK_EQ(Identity::load(folder).certificate_pem(), third.certificate_pem());
}

TEST(dpapi_round_trip) {
    std::vector<std::uint8_t> secret{'k', 'e', 'y', 0, 1, 2, 3};
    auto blob = protect(secret);
    CHECK(blob.size() > secret.size());
    auto back = unprotect(blob);
    REQUIRE(back);
    CHECK(*back == secret);
    std::vector<std::uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!unprotect(garbage));
}

TEST(host_store) {
    fs::path folder = temp_folder("hosts");
    HostStore hosts(folder);
    CHECK(!hosts.certificate("A"));
    hosts.pin("A", "PEM-A", "gaming-pc", "gaming-pc.lan:47989");
    hosts.pin("B", "PEM-B", "htpc", "[fd00::5]:47989");
    REQUIRE(hosts.certificate("A"));
    CHECK_EQ(*hosts.certificate("A"), std::string("PEM-A"));
    CHECK_EQ(*HostStore(folder).certificate("B"), std::string("PEM-B"));
    hosts.pin("A", "PEM-A2", "gaming-pc", "10.0.0.2:47989");
    CHECK_EQ(*hosts.certificate("A"), std::string("PEM-A2"));
    hosts.forget("A");
    CHECK(!hosts.certificate("A"));
    CHECK(hosts.certificate("B"));
    std::ofstream(folder / "hosts.json", std::ios::trunc) << "not json";
    CHECK(!hosts.certificate("B"));
    hosts.pin("C", "PEM-C", "c", "c:1");
    CHECK_EQ(*hosts.certificate("C"), std::string("PEM-C"));
}

TEST(data_folder_location) {
    SetEnvironmentVariableW(L"CASEMENT_MOONLIGHT_DATA", nullptr);
    fs::path p = data_folder();
    CHECK(p.wstring().ends_with(L"\\Casement\\plugin-data\\moonlight") ||
          p.wstring().ends_with(L"/Casement/plugin-data/moonlight"));
    SetEnvironmentVariableW(L"CASEMENT_MOONLIGHT_DATA", L"C:\\elsewhere");
    CHECK(data_folder() == fs::path(L"C:\\elsewhere"));
    SetEnvironmentVariableW(L"CASEMENT_MOONLIGHT_DATA", nullptr);
}
