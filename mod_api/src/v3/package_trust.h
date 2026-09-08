#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace wotbmod {
namespace v3 {

enum class PackageTrustResult : uint32_t {
    Valid = 1u,
    Invalid = 2u,
    Untrusted = 3u,
    Unsupported = 4u,
    IoError = 5u
};

inline bool PackageTrustKeyIdValid(const std::string& key_id) {
    if (key_id.empty() || key_id.size() >= 128u ||
        key_id.front() == '.' || key_id.back() == '.') {
        return false;
    }
    return std::all_of(
        key_id.begin(), key_id.end(),
        [](unsigned char value) {
            return (value >= 'a' && value <= 'z') ||
                   (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') ||
                   value == '_' || value == '-' || value == '.';
        });
}

inline int PackageTrustHexNibble(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline bool PackageTrustDecodeHex(
    const std::string& text,
    size_t expected_bytes,
    std::vector<uint8_t>* out) {
    if (!out || text.size() != expected_bytes * 2u) return false;
    out->assign(expected_bytes, 0u);
    for (size_t i = 0u; i < expected_bytes; ++i) {
        const int high = PackageTrustHexNibble(
            static_cast<unsigned char>(text[i * 2u]));
        const int low = PackageTrustHexNibble(
            static_cast<unsigned char>(text[i * 2u + 1u]));
        if (high < 0 || low < 0) {
            out->clear();
            return false;
        }
        (*out)[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

inline std::string PackageTrustTrim(std::string value) {
    const auto whitespace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!value.empty() && whitespace(
               static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && whitespace(
               static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

inline PackageTrustResult VerifyTrustedP256Sha256(
    const std::filesystem::path& trust_store,
    const std::string& key_id,
    const std::string& digest_hex,
    const std::string& signature_hex,
    std::string* error) {
    if (!PackageTrustKeyIdValid(key_id)) {
        if (error) *error = "signature key id is invalid";
        return PackageTrustResult::Invalid;
    }
    std::vector<uint8_t> digest;
    std::vector<uint8_t> signature;
    if (!PackageTrustDecodeHex(digest_hex, 32u, &digest) ||
        !PackageTrustDecodeHex(signature_hex, 64u, &signature)) {
        if (error) *error = "signature digest or value is not strict hexadecimal";
        return PackageTrustResult::Invalid;
    }

    const std::filesystem::path key_path =
        trust_store / std::filesystem::u8path(key_id + ".p256");
    const DWORD store_attributes =
        GetFileAttributesW(trust_store.c_str());
    if (store_attributes != INVALID_FILE_ATTRIBUTES &&
        (store_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        if (error) *error = "trust store cannot be reparse-backed";
        return PackageTrustResult::Invalid;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(key_path, ec) || ec) {
        if (error) *error = "signature key is not present in the trust store";
        return PackageTrustResult::Untrusted;
    }
    if ((GetFileAttributesW(key_path.c_str()) &
         FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        if (error) *error = "trust-store key cannot be reparse-backed";
        return PackageTrustResult::Invalid;
    }
    std::ifstream key_input(key_path, std::ios::binary);
    if (!key_input) {
        if (error) *error = "trusted key cannot be read";
        return PackageTrustResult::IoError;
    }
    std::string key_hex(
        (std::istreambuf_iterator<char>(key_input)),
        std::istreambuf_iterator<char>());
    key_hex = PackageTrustTrim(key_hex);
    std::vector<uint8_t> public_xy;
    if (!PackageTrustDecodeHex(key_hex, 64u, &public_xy)) {
        if (error) *error = "trusted P-256 key must be 64-byte X||Y hexadecimal";
        return PackageTrustResult::Invalid;
    }

    std::vector<uint8_t> blob(
        sizeof(BCRYPT_ECCKEY_BLOB) + public_xy.size());
    BCRYPT_ECCKEY_BLOB header = {};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32u;
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(
        blob.data() + sizeof(header),
        public_xy.data(), public_xy.size());

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0u);
    if (status >= 0) {
        status = BCryptImportKeyPair(
            algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB,
            &key, blob.data(), static_cast<ULONG>(blob.size()), 0u);
    }
    if (status >= 0) {
        status = BCryptVerifySignature(
            key, nullptr,
            digest.data(), static_cast<ULONG>(digest.size()),
            signature.data(), static_cast<ULONG>(signature.size()),
            0u);
    }
    if (key) BCryptDestroyKey(key);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0u);
    if (status < 0) {
        if (error) *error = "ECDSA P-256 signature verification failed";
        return PackageTrustResult::Invalid;
    }
    if (error) error->clear();
    return PackageTrustResult::Valid;
}

}  // namespace v3
}  // namespace wotbmod
