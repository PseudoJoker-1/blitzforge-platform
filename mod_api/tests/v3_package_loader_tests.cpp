#include "../src/v3/package_loader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;
using namespace wotbmod::v3;

static_assert(
    std::is_standard_layout<PackagePreflightLimits>::value,
    "package limits must remain POD-compatible");
static_assert(
    std::is_standard_layout<PackagePreflightOptions>::value,
    "package options must remain POD-compatible");
static_assert(
    std::is_standard_layout<PackagePermissionGrant>::value,
    "permission grants must remain POD-compatible");
static_assert(
    std::is_standard_layout<PackagePlanEntry>::value,
    "package plan entries must remain POD-compatible");
static_assert(
    std::is_standard_layout<PackagePlanSummary>::value,
    "package summary must remain POD-compatible");
static_assert(
    offsetof(PackagePlanEntry, struct_size) == 0u &&
        offsetof(PackagePlanEntry, api_version) ==
            sizeof(uint32_t),
    "package plan ABI header must remain first");

namespace {

struct TestState {
    uint32_t checks = 0u;
    uint32_t failures = 0u;
};

void Check(
    TestState* state,
    bool condition,
    const char* message) {
    ++state->checks;
    if (!condition) {
        ++state->failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void WriteText(
    const fs::path& path,
    const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));
}

std::string Hex(const uint8_t* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string value(size * 2u, '0');
    for (size_t i = 0u; i < size; ++i) {
        value[i * 2u] = digits[bytes[i] >> 4u];
        value[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    return value;
}

bool DecodeHex32(
    const char* text,
    uint8_t out[32]) {
    if (!text || std::strlen(text) != 64u) return false;
    auto nibble = [](unsigned char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (size_t i = 0u; i < 32u; ++i) {
        const int high = nibble(
            static_cast<unsigned char>(text[i * 2u]));
        const int low = nibble(
            static_cast<unsigned char>(text[i * 2u + 1u]));
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

bool MakeP256Signature(
    const char* digest_hex,
    std::string* public_xy_hex,
    std::string* signature_hex) {
    if (!public_xy_hex || !signature_hex) return false;
    uint8_t digest[32] = {};
    if (!DecodeHex32(digest_hex, digest)) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0u);
    if (status >= 0) {
        status = BCryptGenerateKeyPair(
            algorithm, &key, 256u, 0u);
    }
    if (status >= 0) status = BCryptFinalizeKeyPair(key, 0u);
    ULONG public_size = 0u;
    if (status >= 0) {
        status = BCryptExportKey(
            key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
            nullptr, 0u, &public_size, 0u);
    }
    std::vector<uint8_t> public_blob(public_size);
    if (status >= 0) {
        status = BCryptExportKey(
            key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
            public_blob.data(),
            static_cast<ULONG>(public_blob.size()),
            &public_size, 0u);
    }
    ULONG signature_size = 0u;
    if (status >= 0) {
        status = BCryptSignHash(
            key, nullptr, digest, sizeof(digest),
            nullptr, 0u, &signature_size, 0u);
    }
    std::vector<uint8_t> signature(signature_size);
    if (status >= 0) {
        status = BCryptSignHash(
            key, nullptr, digest, sizeof(digest),
            signature.data(),
            static_cast<ULONG>(signature.size()),
            &signature_size, 0u);
    }
    if (status >= 0 &&
        public_blob.size() ==
            sizeof(BCRYPT_ECCKEY_BLOB) + 64u &&
        signature.size() == 64u) {
        *public_xy_hex = Hex(
            public_blob.data() + sizeof(BCRYPT_ECCKEY_BLOB),
            64u);
        *signature_hex = Hex(signature.data(), signature.size());
    } else {
        status = -1;
    }
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0u);
    return status >= 0;
}

void WriteSignedRevocations(
    const fs::path& root,
    const std::string& contents) {
    const fs::path list = root / "trust" / "revocations.list";
    WriteText(list, contents);
    char digest[65] = {};
    if (Sha256FileUtf8(list.u8string().c_str(), digest) !=
        WOTBMOD_V3_OK) {
        return;
    }
    std::string public_key;
    std::string signature;
    if (!MakeP256Signature(digest, &public_key, &signature)) return;
    WriteText(
        root / "trust" / "keys" / "tests.revocations.p256",
        public_key + "\n");
    WriteText(
        fs::u8path(list.u8string() + ".sig"),
        "WOTBMOD-SIGNATURE-V1\n"
        "algorithm=ecdsa-p256-sha256\n"
        "key_id=tests.revocations\n"
        "sha256=" + std::string(digest) + "\n"
        "signature=" + signature + "\n");
}

PackagePreflightOptions OptionsFor(
    const fs::path& root,
    const fs::path& staging) {
    PackagePreflightOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    options.flags =
        PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED;
    options.max_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    const std::string root_text = root.u8string();
    const std::string staging_text = staging.u8string();
    std::copy(
        root_text.begin(), root_text.end(),
        options.mods_root);
    options.mods_root[root_text.size()] = '\0';
    std::copy(
        staging_text.begin(), staging_text.end(),
        options.archive_staging_root);
    options.archive_staging_root[staging_text.size()] = '\0';
    const std::string build = "11.19.0.834";
    std::copy(
        build.begin(), build.end(),
        options.client_build);
    const std::string executable_hash(64u, 'a');
    std::copy(
        executable_hash.begin(),
        executable_hash.end(),
        options.client_executable_sha256);
    options.client_executable_sha256[64] = '\0';
    options.limits = DefaultPackagePreflightLimits();
    return options;
}

PackagePlanSummary EmptySummary() {
    PackagePlanSummary summary = {};
    summary.struct_size = sizeof(summary);
    summary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    return summary;
}

std::string NativeManifest(
    const std::string& id,
    const std::string& version,
    const std::string& dependency = std::string(),
    const std::string& permission = "core") {
    std::string dependencies;
    if (!dependency.empty()) {
        dependencies =
            ",\"dependencies\":{\"" + dependency +
            "\":\"^1.0\"}";
    }
    return
        "{\"manifest_version\":1,\"type\":\"native\","
        "\"id\":\"" + id + "\",\"name\":\"" + id +
        "\",\"version\":\"" + version +
        "\",\"developer\":\"tests\","
        "\"client\":{\"builds\":[\"11.19.0.834\"],"
        "\"executable_hashes\":[\"" +
        std::string(64u, 'a') +
        "\"]},\"entrypoints\":{\"windows-x86\":\"mod.dll\"},"
        "\"permissions\":[\"" + permission + "\"]" +
        dependencies + "}";
}

void AddNative(
    const fs::path& root,
    const std::string& directory,
    const std::string& manifest) {
    const fs::path package = root / directory;
    WriteText(package / "manifest.json", manifest);
    WriteText(package / "mod.dll", "MZ-test-payload");
}

uint32_t TestCrc32(
    const uint8_t* bytes,
    size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < size; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^
                  (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void Push16(std::vector<uint8_t>* bytes, uint16_t value) {
    bytes->push_back(static_cast<uint8_t>(value & 0xffu));
    bytes->push_back(
        static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void Push32(std::vector<uint8_t>* bytes, uint32_t value) {
    for (uint32_t shift = 0u; shift < 32u; shift += 8u) {
        bytes->push_back(
            static_cast<uint8_t>(
                (value >> shift) & 0xffu));
    }
}

struct TestZipEntry {
    std::string name;
    std::string data;
    uint16_t method = 0u;
    uint32_t offset = 0u;
    uint32_t crc = 0u;
};

void WriteStoredZip(
    const fs::path& path,
    std::vector<TestZipEntry> entries) {
    std::vector<uint8_t> bytes;
    for (TestZipEntry& entry : entries) {
        entry.offset = static_cast<uint32_t>(bytes.size());
        entry.crc = TestCrc32(
            reinterpret_cast<const uint8_t*>(
                entry.data.data()),
            entry.data.size());
        Push32(&bytes, 0x04034b50u);
        Push16(&bytes, 20u);
        Push16(&bytes, 0u);
        Push16(&bytes, entry.method);
        Push16(&bytes, 0u);
        Push16(&bytes, 0u);
        Push32(&bytes, entry.crc);
        Push32(
            &bytes,
            static_cast<uint32_t>(entry.data.size()));
        Push32(
            &bytes,
            static_cast<uint32_t>(entry.data.size()));
        Push16(
            &bytes,
            static_cast<uint16_t>(entry.name.size()));
        Push16(&bytes, 0u);
        bytes.insert(
            bytes.end(),
            entry.name.begin(), entry.name.end());
        bytes.insert(
            bytes.end(),
            entry.data.begin(), entry.data.end());
    }
    const uint32_t central_offset =
        static_cast<uint32_t>(bytes.size());
    for (const TestZipEntry& entry : entries) {
        Push32(&bytes, 0x02014b50u);
        Push16(&bytes, 20u);
        Push16(&bytes, 20u);
        Push16(&bytes, 0u);
        Push16(&bytes, entry.method);
        Push16(&bytes, 0u);
        Push16(&bytes, 0u);
        Push32(&bytes, entry.crc);
        Push32(
            &bytes,
            static_cast<uint32_t>(entry.data.size()));
        Push32(
            &bytes,
            static_cast<uint32_t>(entry.data.size()));
        Push16(
            &bytes,
            static_cast<uint16_t>(entry.name.size()));
        Push16(&bytes, 0u);
        Push16(&bytes, 0u);
        Push16(&bytes, 0u);
        Push16(&bytes, 0u);
        Push32(&bytes, 0u);
        Push32(&bytes, entry.offset);
        bytes.insert(
            bytes.end(),
            entry.name.begin(), entry.name.end());
    }
    const uint32_t central_size =
        static_cast<uint32_t>(bytes.size()) -
        central_offset;
    Push32(&bytes, 0x06054b50u);
    Push16(&bytes, 0u);
    Push16(&bytes, 0u);
    Push16(
        &bytes,
        static_cast<uint16_t>(entries.size()));
    Push16(
        &bytes,
        static_cast<uint16_t>(entries.size()));
    Push32(&bytes, central_size);
    Push32(&bytes, central_offset);
    Push16(&bytes, 0u);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void WritePackageSignature(
    const fs::path& sidecar,
    const std::string& digest,
    const std::string& signature,
    const std::string& extra = std::string()) {
    WriteText(
        sidecar,
        "WOTBMOD-SIGNATURE-V1\n"
        "algorithm=ecdsa-p256-sha256\n"
        "key_id=tests.release\n"
        "sha256=" + digest + "\n"
        "signature=" + signature + "\n" + extra);
}

void CheckRejectedStoredArchive(
    TestState* state,
    const fs::path& base,
    const std::string& case_name,
    std::vector<TestZipEntry> zip_entries,
    WotbModV3Result expected,
    const char* message) {
    const fs::path root = base / case_name / "mods";
    WriteStoredZip(
        root / "rejected.wotbmod",
        std::move(zip_entries));
    PackagePreflightOptions options =
        OptionsFor(root, base / case_name / "stage");
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    std::vector<PackagePlanEntry> entries(2u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.discovered_count == 1u &&
            summary.blocked_count == 1u &&
            static_cast<WotbModV3Result>(entries[0].result) ==
                expected,
        message);
}

void TestHash(
    TestState* state,
    const fs::path& root) {
    const fs::path file = root / "sha" / "abc.bin";
    WriteText(file, "abc");
    char hash[65] = {};
    Check(
        state,
        Sha256FileUtf8(file.u8string().c_str(), hash) ==
            WOTBMOD_V3_OK,
        "SHA-256 helper succeeds");
    Check(
        state,
        std::string(hash) ==
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad",
        "SHA-256 helper returns the standard digest");
}

void TestDependencyOrder(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "dependency" / "mods";
    AddNative(
        root, "common",
        NativeManifest(
            "tests.common", "1.4.0"));
    AddNative(
        root, "app",
        NativeManifest(
            "tests.app", "2.0.0", "tests.common"));
    PackagePreflightOptions options =
        OptionsFor(root, base / "dependency" / "stage");
    std::vector<PackagePlanEntry> entries(8u);
    PackagePlanSummary summary = EmptySummary();
    const WotbModV3Result result = BuildPackageLoadPlan(
        &options, entries.data(),
        static_cast<uint32_t>(entries.size()), &summary);
    Check(state, result == WOTBMOD_V3_OK,
          "dependency plan builds");
    Check(state, summary.ready_count == 2u,
          "both dependency packages are ready");
    Check(
        state,
        std::string(entries[0].id) == "tests.common" &&
            std::string(entries[1].id) == "tests.app",
        "topological order loads dependency first");
    Check(
        state,
        entries[0].package_sha256[64] == '\0' &&
            entries[0].payload_sha256[64] == '\0',
        "package and payload hashes are populated");
}

void TestContentAndPolicy(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "content" / "mods";
    const fs::path package = root / "sound";
    WriteText(
        package / "manifest.json",
        "{\"manifest_version\":1,\"type\":\"content\","
        "\"id\":\"tests.sound\",\"name\":\"Sound\","
        "\"version\":\"1.0.0\",\"developer\":\"tests\","
        "\"content\":\"content.json\","
        "\"permissions\":[\"resources\"]}");
    WriteText(package / "content.json", "{\"overrides\":{}}");
    AddNative(
        root, "unsafe",
        NativeManifest(
            "tests.unsafe", "1.0.0",
            std::string(), "native.hook.address"));
    PackagePreflightOptions options =
        OptionsFor(root, base / "content" / "stage");
    options.max_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    std::vector<PackagePlanEntry> entries(8u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "content/policy plan builds");
    Check(state, summary.ready_count == 1u,
          "content-only package is structurally loadable");
    Check(state, summary.blocked_count == 1u,
          "unsafe native package is blocked by grant");
    bool found_content = false;
    bool found_permission_block = false;
    for (uint32_t i = 0u; i < summary.discovered_count; ++i) {
        if (std::string(entries[i].id) == "tests.sound") {
            found_content =
                entries[i].package_type ==
                    WOTBMOD_V3_PACKAGE_CONTENT_ONLY &&
                entries[i].content_path[0] != '\0' &&
                entries[i].permission_count == 1u &&
                std::strcmp(
                    entries[i].permissions[0],
                    "resources") == 0;
        }
        if (std::string(entries[i].id) == "tests.unsafe") {
            found_permission_block =
                entries[i].result ==
                    WOTBMOD_V3_E_PERMISSION_DENIED;
        }
    }
    Check(state, found_content,
          "content descriptor path is resolved");
    Check(state, found_permission_block,
          "permission block reason is represented in plan");

    PackagePermissionGrant grant = {};
    grant.struct_size = sizeof(grant);
    grant.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    grant.max_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    const std::string unsafe_id = "TESTS.UNSAFE";
    std::copy(
        unsafe_id.begin(), unsafe_id.end(), grant.id);
    options.permission_grants = &grant;
    options.permission_grant_count = 1u;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 2u,
        "per-mod permission grant overrides safe default");
    bool unsafe_granted = false;
    for (uint32_t i = 0u; i < summary.discovered_count; ++i) {
        if (std::string(entries[i].id) == "tests.unsafe") {
            unsafe_granted =
                entries[i].granted_permission_tier ==
                    WOTBMOD_V3_PERMISSION_UNSAFE &&
                entries[i].permission_count == 1u &&
                std::strcmp(
                    entries[i].permissions[0],
                    "native.hook.address") == 0 &&
                (entries[i].warning_flags &
                 PACKAGE_WARNING_UNSAFE_PERMISSION_TIER) != 0u;
        }
    }
    Check(
        state, unsafe_granted,
        "case-insensitive unsafe grant remains visible as a plan warning");

    PackagePermissionGrant duplicateGrants[2] = {grant, grant};
    std::fill(
        std::begin(duplicateGrants[0].id),
        std::end(duplicateGrants[0].id),
        '\0');
    std::fill(
        std::begin(duplicateGrants[1].id),
        std::end(duplicateGrants[1].id),
        '\0');
    const std::string lowerId = "tests.unsafe";
    const std::string upperId = "TESTS.UNSAFE";
    std::copy(
        lowerId.begin(),
        lowerId.end(),
        duplicateGrants[0].id);
    std::copy(
        upperId.begin(),
        upperId.end(),
        duplicateGrants[1].id);
    options.permission_grants = duplicateGrants;
    options.permission_grant_count = 2u;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options,
            entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_E_CONFLICT,
        "case-only duplicate permission grant ids are rejected");
}

void TestCycleAndTraversal(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "invalid" / "mods";
    AddNative(
        root, "a",
        NativeManifest(
            "tests.a", "1.0.0", "tests.b"));
    AddNative(
        root, "b",
        NativeManifest(
            "tests.b", "1.0.0", "tests.a"));
    const fs::path traversal = root / "traversal";
    WriteText(
        traversal / "manifest.json",
        "{\"manifest_version\":1,\"type\":\"content\","
        "\"id\":\"tests.traversal\",\"name\":\"Traversal\","
        "\"version\":\"1.0.0\",\"developer\":\"tests\","
        "\"content\":\"../outside.json\"}");
    PackagePreflightOptions options =
        OptionsFor(root, base / "invalid" / "stage");
    std::vector<PackagePlanEntry> entries(8u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "invalid candidates stay in a diagnostic plan");
    Check(state, summary.blocked_count == 3u,
          "cycle and traversal packages are blocked");
    uint32_t conflicts = 0u;
    uint32_t parse_errors = 0u;
    for (uint32_t i = 0u; i < summary.discovered_count; ++i) {
        if (entries[i].result == WOTBMOD_V3_E_CONFLICT) {
            ++conflicts;
        }
        if (entries[i].result == WOTBMOD_V3_E_PARSE) {
            ++parse_errors;
        }
    }
    Check(state, conflicts == 2u,
          "both cycle participants are diagnosed");
    Check(state, parse_errors == 1u,
          "manifest traversal is rejected before loading");
}

void TestClientAndDuplicate(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "identity" / "mods";
    std::string wrong_client =
        NativeManifest("tests.client", "1.0.0");
    const size_t build = wrong_client.find("11.19.0.834");
    wrong_client.replace(build, 11u, "11.18.0.1");
    AddNative(root, "client", wrong_client);
    AddNative(
        root, "duplicate-a",
        NativeManifest("tests.duplicate", "1.0.0"));
    AddNative(
        root, "duplicate-b",
        NativeManifest("tests.duplicate", "2.0.0"));
    PackagePreflightOptions options =
        OptionsFor(root, base / "identity" / "stage");
    std::vector<PackagePlanEntry> entries(8u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "identity/client diagnostics plan builds");
    Check(
        state,
        summary.ready_count == 0u &&
            summary.blocked_count == 3u,
        "client mismatch and duplicate ids are blocked");
    uint32_t mismatches = 0u;
    uint32_t duplicates = 0u;
    for (uint32_t i = 0u; i < summary.discovered_count; ++i) {
        if (entries[i].result ==
            WOTBMOD_V3_E_CLIENT_MISMATCH) {
            ++mismatches;
        }
        if (entries[i].result ==
            WOTBMOD_V3_E_CONFLICT) {
            ++duplicates;
        }
    }
    Check(state, mismatches == 1u,
          "client build allowlist is enforced");
    Check(state, duplicates == 2u,
          "all duplicate-id sources are diagnosed");
}

std::string ContentManifest() {
    return
        "{\"manifest_version\":1,\"type\":\"content\","
        "\"id\":\"tests.archive\",\"name\":\"Archive\","
        "\"version\":\"1.0.0\",\"developer\":\"tests\","
        "\"content\":\"content.json\","
        "\"permissions\":[\"resources\"]}";
}

void TestStoredArchive(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "archive" / "mods";
    const fs::path staging = base / "archive" / "stage";
    WriteStoredZip(
        root / "valid.wotbmod",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{\"overrides\":{}}", 0u}
        });
    PackagePreflightOptions options =
        OptionsFor(root, staging);
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    std::vector<PackagePlanEntry> entries(4u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, nullptr, 0u, &summary) ==
                WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
            summary.required_capacity == 1u,
        "archive count-only query reports capacity");
    Check(
        state,
        !fs::exists(staging),
        "archive count-only query has no extraction side effect");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "stored wotbmod plan builds");
    Check(
        state,
        summary.ready_count == 1u &&
            entries[0].source_kind ==
                PACKAGE_SOURCE_STORED_WOTBMOD_ARCHIVE,
        "stored wotbmod is structurally loadable");
    Check(
        state,
        fs::is_regular_file(
            fs::u8path(entries[0].content_path)),
        "stored wotbmod payload is materialized");
    Check(
        state,
        (entries[0].warning_flags &
         PACKAGE_WARNING_UNSIGNED) != 0u,
        "unsigned archive is explicit in plan");
    Check(
        state,
        std::strstr(entries[0].reason, "trust warnings") != nullptr,
        "READY archive status preserves trust warnings");

    const fs::path unsupported =
        base / "archive-compressed" / "mods";
    WriteStoredZip(
        unsupported / "compressed.wotbmod",
        {
            {"manifest.json", ContentManifest(), 8u},
            {"content.json", "{}", 8u}
        });
    options = OptionsFor(
        unsupported,
        base / "archive-compressed" / "stage");
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "unsupported archive remains diagnostic");
    Check(
        state,
        summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_NOT_SUPPORTED,
        "compressed archive never reports false success");

    const fs::path traversal =
        base / "archive-traversal" / "mods";
    WriteStoredZip(
        traversal / "traversal.wotbmod",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u},
            {"../escape.txt", "forbidden", 0u}
        });
    options = OptionsFor(
        traversal,
        base / "archive-traversal" / "stage");
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "archive traversal remains diagnostic");
    Check(
        state,
        entries[0].result ==
            WOTBMOD_V3_E_PERMISSION_DENIED,
        "archive traversal is rejected before extraction");

    CheckRejectedStoredArchive(
        state, base, "archive-absolute",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u},
            {"/absolute.txt", "forbidden", 0u}
        },
        WOTBMOD_V3_E_PERMISSION_DENIED,
        "absolute archive path is rejected");
    CheckRejectedStoredArchive(
        state, base, "archive-drive-path",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u},
            {"C:/absolute.txt", "forbidden", 0u}
        },
        WOTBMOD_V3_E_PERMISSION_DENIED,
        "drive-qualified archive path is rejected");
    CheckRejectedStoredArchive(
        state, base, "archive-duplicate-entry",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u},
            {"content.json", "duplicate", 0u}
        },
        WOTBMOD_V3_E_CONFLICT,
        "duplicate archive entry is rejected");
    CheckRejectedStoredArchive(
        state, base, "archive-case-collision",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u},
            {"Content.json", "case collision", 0u}
        },
        WOTBMOD_V3_E_CONFLICT,
        "case-folded archive path collision is rejected");
    CheckRejectedStoredArchive(
        state, base, "archive-repeated-manifest",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u}
        },
        WOTBMOD_V3_E_CONFLICT,
        "repeated root manifest is rejected");
    CheckRejectedStoredArchive(
        state, base, "archive-renamed-file",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"renamed.json", "{}", 0u}
        },
        WOTBMOD_V3_E_NOT_FOUND,
        "renamed manifest payload is rejected");

    const fs::path signed_root =
        base / "archive-signed-case" / "mods";
    const fs::path signed_archive =
        signed_root / "signed.wotbmod";
    WriteStoredZip(
        signed_archive,
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{}", 0u}
        });
    options = OptionsFor(
        signed_root,
        base / "archive-signed-case" / "stage");
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u,
        "archive signature fixture obtains canonical package digest");
    const std::string signed_digest =
        entries[0].package_sha256;
    std::string signed_public_key;
    std::string signed_signature;
    Check(
        state,
        MakeP256Signature(
            signed_digest.c_str(),
            &signed_public_key,
            &signed_signature),
        "archive signature fixture creates a P-256 signature");
    WriteText(
        signed_root / "trust" / "keys" /
            "tests.release.p256",
        signed_public_key + "\n");
    WritePackageSignature(
        fs::u8path(signed_archive.u8string() + ".sig"),
        signed_digest,
        signed_signature);
    options.flags |=
        PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u &&
            entries[0].signature_status ==
                WOTBMOD_V3_SIGNATURE_VALID,
        "trusted detached signature accepts exact archive bytes");
    WriteStoredZip(
        signed_archive,
        {
            {"manifest.json", ContentManifest(), 0u},
            {"Content.json", "{}", 0u}
        });
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_HASH_MISMATCH,
        "changing archive path case invalidates its signature");
}

void TestTransientArchivePublishRetry(
    TestState* state,
    const fs::path& base) {
    const fs::path root =
        base / "archive-publish-retry" / "mods";
    const fs::path staging =
        base / "archive-publish-retry" / "stage";
    std::error_code setup_error;
    fs::create_directories(staging, setup_error);
    Check(
        state,
        !setup_error,
        "archive publish retry staging directory is created");
    WriteStoredZip(
        root / "retry.wotbmod",
        {
            {"manifest.json", ContentManifest(), 0u},
            {"content.json", "{\"overrides\":{}}", 0u},
            {"payload.bin", std::string(16u * 1024u * 1024u, 'x'), 0u}
        });
    PackagePreflightOptions options =
        OptionsFor(root, staging);
    options.flags |=
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    std::vector<PackagePlanEntry> entries(4u);
    PackagePlanSummary summary = EmptySummary();
    std::atomic<bool> finished{false};
    std::atomic<bool> blocked_once{false};
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    std::thread publisher([&]() {
        result = BuildPackageLoadPlan(
            &options,
            entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary);
        finished.store(true);
    });
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!finished.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::error_code scan_error;
        fs::directory_iterator iterator(staging, scan_error);
        if (!scan_error) {
            for (const fs::directory_entry& entry : iterator) {
                const std::wstring name =
                    entry.path().filename().wstring();
                if (name.rfind(L".wotbmod-tmp-", 0u) != 0u) {
                    continue;
                }
                HANDLE handle = CreateFileW(
                    entry.path().c_str(),
                    FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr);
                if (handle != INVALID_HANDLE_VALUE) {
                    blocked_once.store(true);
                    Sleep(350u);
                    CloseHandle(handle);
                    break;
                }
            }
        }
        if (blocked_once.load()) break;
        SwitchToThread();
    }
    publisher.join();
    Check(
        state,
        blocked_once.load(),
        "archive publish test observed and held the temporary directory");
    Check(
        state,
        result == WOTBMOD_V3_OK &&
            summary.ready_count == 1u &&
            fs::is_regular_file(
                fs::u8path(entries[0].content_path)),
        "archive publish retries a transient directory lock");
}

void TestCatalogAndCapacity(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "catalog" / "mods";
    AddNative(
        root, "catalog",
        NativeManifest(
            "tests.catalog", "1.0.0"));
    PackagePreflightOptions options =
        OptionsFor(root, base / "catalog" / "stage");
    std::vector<PackagePlanEntry> entries(2u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "unreviewed package can be inspected");
    const std::string actual_hash =
        entries[0].package_sha256;
    WotbModV3CatalogRecord record = {};
    record.struct_size = sizeof(record);
    record.api_version = WOTBMOD_V3_CATALOG_VERSION;
    record.status = WOTBMOD_V3_CATALOG_COMMUNITY;
    const std::string id = "tests.catalog";
    const std::string version = "1.0.0";
    std::copy(id.begin(), id.end(), record.id);
    std::copy(
        version.begin(), version.end(), record.version);
    std::copy(
        actual_hash.begin(), actual_hash.end(),
        record.sha256);
    options.catalog_records = &record;
    options.catalog_record_count = 1u;
    options.flags =
        PACKAGE_PREFLIGHT_REQUIRE_CATALOG_RECORD;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK,
        "matching catalog record is accepted");
    Check(
        state,
        entries[0].status == PACKAGE_PLAN_READY &&
            entries[0].catalog_status ==
                WOTBMOD_V3_CATALOG_COMMUNITY &&
            (entries[0].warning_flags &
             PACKAGE_WARNING_UNREVIEWED) == 0u,
        "community catalog SHA is verified");
    Check(
        state,
        std::strstr(
            entries[0].reason,
            "trust warnings") != nullptr,
        "READY directory status preserves unsigned warning");
    record.status = WOTBMOD_V3_CATALOG_BANNED;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            entries[0].result ==
                WOTBMOD_V3_E_INCOMPATIBLE,
        "catalog blocklist rejects matching id/hash");
    record.status = WOTBMOD_V3_CATALOG_COMMUNITY;
    const std::string wrong_hash(64u, '0');
    std::copy(
        wrong_hash.begin(), wrong_hash.end(),
        record.sha256);
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            entries[0].result ==
                WOTBMOD_V3_E_HASH_MISMATCH,
        "catalog SHA mismatch blocks loading");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, nullptr, 0u, &summary) ==
            WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
            summary.required_capacity == 1u,
        "capacity query reports required output size");
}

void TestPackageTrust(
    TestState* state,
    const fs::path& base) {
    const fs::path root = base / "trust" / "mods";
    const fs::path package = root / "signed";
    AddNative(
        root, "signed",
        NativeManifest("tests.signed", "1.0.0"));
    PackagePreflightOptions options =
        OptionsFor(root, base / "trust" / "stage");
    std::vector<PackagePlanEntry> entries(4u);
    PackagePlanSummary summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u &&
            entries[0].signature_status ==
                WOTBMOD_V3_SIGNATURE_UNSIGNED &&
            (entries[0].warning_flags &
             PACKAGE_WARNING_UNSIGNED) != 0u,
        "unsigned package remains loadable with explicit warning");

    const std::string digest = entries[0].package_sha256;
    std::string public_key;
    std::string signature;
    Check(
        state,
        MakeP256Signature(
            digest.c_str(), &public_key, &signature),
        "test creates a real ECDSA P-256 signature");
    const fs::path sidecar =
        fs::u8path(package.u8string() + ".wotbmod.sig");
    WritePackageSignature(
        sidecar, digest, signature);

    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u &&
            entries[0].signature_status ==
                WOTBMOD_V3_SIGNATURE_DECLARED &&
            (entries[0].warning_flags &
             PACKAGE_WARNING_UNTRUSTED_SIGNER) != 0u,
        "unknown signer stays loadable only as explicitly untrusted");

    options.flags |=
        PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_SIGNATURE_INVALID,
        "strict trust policy blocks an unknown signer");

    WriteText(
        root / "trust" / "keys" / "tests.release.p256",
        public_key + "\n");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u &&
            entries[0].signature_status ==
                WOTBMOD_V3_SIGNATURE_VALID &&
            std::strcmp(
                entries[0].signature_key_id,
                "tests.release") == 0,
        "trusted P-256 signature is verified before loading");

    WritePackageSignature(
        sidecar, digest, signature,
        "unknown_field=forbidden\n");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_SIGNATURE_INVALID,
        "signature sidecar rejects unknown fields");

    WritePackageSignature(
        sidecar, digest,
        "304402200102030405060708090a0b0c");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_SIGNATURE_INVALID,
        "DER or malformed-length signature is rejected; raw R||S is required");

    WritePackageSignature(
        sidecar, digest, signature);
    WriteText(package / "mod.dll", "MZ-test-payload-tampered");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_HASH_MISMATCH,
        "one-byte package payload tamper invalidates signature");
    WriteText(package / "mod.dll", "MZ-test-payload");

    WriteText(
        package / "manifest.json",
        NativeManifest(
            "tests.signed", "1.0.0",
            "tests.missing", "resources"));
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_HASH_MISMATCH,
        "manifest permission and dependency tamper invalidates signature");
    WriteText(
        package / "manifest.json",
        NativeManifest("tests.signed", "1.0.0"));

    WriteText(
        package / "manifest.json",
        NativeManifest("tests.other", "1.0.0"));
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_HASH_MISMATCH,
        "signature copied to a different mod ID is rejected");
    WriteText(
        package / "manifest.json",
        NativeManifest("tests.signed", "1.0.0"));

    std::error_code rename_ec;
    fs::rename(
        package / "mod.dll",
        package / "renamed.dll",
        rename_ec);
    std::string renamed_manifest =
        NativeManifest("tests.signed", "1.0.0");
    const size_t entrypoint =
        renamed_manifest.find("mod.dll");
    if (entrypoint != std::string::npos) {
        renamed_manifest.replace(
            entrypoint, std::strlen("mod.dll"),
            "renamed.dll");
    }
    WriteText(package / "manifest.json", renamed_manifest);
    summary = EmptySummary();
    Check(
        state,
        !rename_ec &&
            BuildPackageLoadPlan(
                &options, entries.data(),
                static_cast<uint32_t>(entries.size()),
                &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_HASH_MISMATCH,
        "internal file rename invalidates signature");
    WriteText(
        package / "manifest.json",
        NativeManifest("tests.signed", "1.0.0"));
    fs::rename(
        package / "renamed.dll",
        package / "mod.dll",
        rename_ec);

    WriteSignedRevocations(
        root,
        "WOTBMOD-REVOCATIONS-V1\n"
        "revoke_key=tests.release\n");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_SIGNATURE_INVALID,
        "signed revocation list blocks a revoked signer");

    WriteSignedRevocations(
        root,
        "WOTBMOD-REVOCATIONS-V1\n"
        "revoke_release=tests.signed@1.0.0\n");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result == WOTBMOD_V3_E_SIGNATURE_INVALID,
        "signed revocation list blocks an exact mod release downgrade");

    WriteText(
        root / "trust" / "revocations.list",
        "WOTBMOD-REVOCATIONS-V1\n"
        "unknown=entry\n");
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u,
        "tampered or malformed revocation list fails closed");

    std::error_code cleanup_ec;
    fs::remove(root / "trust" / "revocations.list", cleanup_ec);
    fs::remove(
        root / "trust" / "revocations.list.sig", cleanup_ec);

    signature[0] = signature[0] == '0' ? '1' : '0';
    WritePackageSignature(
        sidecar, digest, signature);
    options.flags &=
        ~PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].signature_status ==
                WOTBMOD_V3_SIGNATURE_INVALID,
        "invalid signature is blocked even in permissive trust mode");

    fs::remove(sidecar, cleanup_ec);
    WriteText(
        package / "manifest.json",
        NativeManifest("tests.signed", "0.9.0"));
    options.flags &=
        ~PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.ready_count == 1u,
        "downgrade fixture obtains its exact package digest");
    const std::string downgrade_digest =
        entries[0].package_sha256;
    std::string downgrade_public_key;
    std::string downgrade_signature;
    Check(
        state,
        MakeP256Signature(
            downgrade_digest.c_str(),
            &downgrade_public_key,
            &downgrade_signature),
        "downgrade fixture creates a trusted signature");
    WriteText(
        root / "trust" / "keys" / "tests.release.p256",
        downgrade_public_key + "\n");
    WritePackageSignature(
        sidecar,
        downgrade_digest,
        downgrade_signature);
    WriteSignedRevocations(
        root,
        "WOTBMOD-REVOCATIONS-V1\n"
        "revoke_release=tests.signed@0.9.0\n");
    options.flags |=
        PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
    summary = EmptySummary();
    Check(
        state,
        BuildPackageLoadPlan(
            &options, entries.data(),
            static_cast<uint32_t>(entries.size()),
            &summary) == WOTBMOD_V3_OK &&
            summary.blocked_count == 1u &&
            entries[0].result ==
                WOTBMOD_V3_E_SIGNATURE_INVALID,
        "signed revocation list blocks a trusted downgrade release");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path base =
        argc > 1
        ? fs::u8path(argv[1])
        : fs::temp_directory_path() /
              "wotbmod-v3-package-tests";
    std::error_code ec;
    const fs::path absolute = fs::absolute(base, ec);
    if (ec || absolute.root_path() == absolute ||
        absolute.filename().empty()) {
        std::cerr << "unsafe test root\n";
        return 2;
    }
    fs::remove_all(absolute, ec);
    if (ec) {
        std::cerr << "cannot clean test root\n";
        return 3;
    }
    fs::create_directories(absolute, ec);
    if (ec) {
        std::cerr << "cannot create test root\n";
        return 4;
    }
    TestState state;
    TestHash(&state, absolute);
    TestDependencyOrder(&state, absolute);
    TestContentAndPolicy(&state, absolute);
    TestCycleAndTraversal(&state, absolute);
    TestClientAndDuplicate(&state, absolute);
    TestStoredArchive(&state, absolute);
    TestTransientArchivePublishRetry(&state, absolute);
    TestCatalogAndCapacity(&state, absolute);
    TestPackageTrust(&state, absolute);
    fs::remove_all(absolute, ec);
    std::cout << "V3 PACKAGE: checks=" << state.checks
              << " failures=" << state.failures << "\n";
    return state.failures == 0u ? 0 : 1;
}
