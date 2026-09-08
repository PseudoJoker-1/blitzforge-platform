#include "lua_manifest_scanner.h"

#include <cstddef>
#include <cstring>

namespace wotbmod {
namespace lua {
namespace {

// Long enough for the current permission names and the key itself. A
// longer name is still scanned to its end, but cannot match this vocabulary.
constexpr size_t kMaxNameChars = 64u;
constexpr size_t kMaxManifestBytes = 256u * 1024u;
constexpr int kMaxDepth = 32;

struct Scan {
    const char* p;
    const char* end;
    const char* error;  // static literal; first failure wins
};

bool Fail(Scan& scan, const char* message) noexcept {
    if (!scan.error) scan.error = message;
    return false;
}

void SkipWhitespace(Scan& scan) noexcept {
    while (scan.p != scan.end) {
        const char c = *scan.p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++scan.p;
        } else {
            break;
        }
    }
}

bool Peek(const Scan& scan, char* out) noexcept {
    if (scan.p == scan.end) return false;
    *out = *scan.p;
    return true;
}

bool IsHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

// kTooLong is provably an unknown permission. kEscaped is different: this
// scanner declined to decode the value, so fail-closed policy must refuse it.
enum class StringForm { kExact, kTooLong, kEscaped };

bool ScanString(Scan& scan, char* out, size_t out_capacity,
                StringForm* out_form) noexcept {
    if (out_form) *out_form = StringForm::kExact;
    char c = 0;
    if (!Peek(scan, &c) || c != '"') return Fail(scan, "expected a string");
    ++scan.p;
    size_t length = 0u;
    for (;;) {
        if (scan.p == scan.end) return Fail(scan, "an unterminated string");
        const char ch = *scan.p++;
        if (ch == '"') break;
        char decoded = ch;
        if (ch == '\\') {
            if (scan.p == scan.end) return Fail(scan, "an unterminated escape");
            const char escape = *scan.p++;
            switch (escape) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                case 'n': decoded = '\n'; break;
                case 'r': decoded = '\r'; break;
                case 't': decoded = '\t'; break;
                case 'u': {
                    for (int i = 0; i < 4; ++i) {
                        if (scan.p == scan.end || !IsHexDigit(*scan.p)) {
                            return Fail(scan, "a malformed \\u escape");
                        }
                        ++scan.p;
                    }
                    if (out_form) *out_form = StringForm::kEscaped;
                    continue;
                }
                default:
                    return Fail(scan, "an unknown escape in a string");
            }
        } else if (static_cast<unsigned char>(ch) < 0x20u) {
            return Fail(scan, "a raw control character in a string");
        }
        if (!out) continue;
        if (length + 1u < out_capacity) {
            out[length++] = decoded;
        } else if (out_form && *out_form == StringForm::kExact) {
            *out_form = StringForm::kTooLong;
        }
    }
    if (out && out_capacity != 0u) out[length] = '\0';
    return true;
}

bool SkipNumber(Scan& scan) noexcept {
    if (scan.p != scan.end && *scan.p == '-') ++scan.p;
    size_t digits = 0u;
    while (scan.p != scan.end && *scan.p >= '0' && *scan.p <= '9') {
        ++scan.p;
        ++digits;
    }
    if (digits == 0u) return Fail(scan, "a value that is not JSON");
    if (scan.p != scan.end && *scan.p == '.') {
        ++scan.p;
        digits = 0u;
        while (scan.p != scan.end && *scan.p >= '0' && *scan.p <= '9') {
            ++scan.p;
            ++digits;
        }
        if (digits == 0u) return Fail(scan, "a malformed number");
    }
    if (scan.p != scan.end && (*scan.p == 'e' || *scan.p == 'E')) {
        ++scan.p;
        if (scan.p != scan.end && (*scan.p == '+' || *scan.p == '-')) {
            ++scan.p;
        }
        digits = 0u;
        while (scan.p != scan.end && *scan.p >= '0' && *scan.p <= '9') {
            ++scan.p;
            ++digits;
        }
        if (digits == 0u) return Fail(scan, "a malformed number");
    }
    return true;
}

bool SkipLiteral(Scan& scan, const char* text) noexcept {
    const size_t length = std::strlen(text);
    if (static_cast<size_t>(scan.end - scan.p) < length ||
        std::memcmp(scan.p, text, length) != 0) {
        return Fail(scan, "a value that is not JSON");
    }
    scan.p += length;
    return true;
}

bool SkipValue(Scan& scan, int depth) noexcept;

enum class Sequence { kContinue, kDone, kFailed };

Sequence NextInSequence(Scan& scan, char closing) noexcept {
    SkipWhitespace(scan);
    char c = 0;
    if (!Peek(scan, &c)) {
        Fail(scan, "an unterminated object or array");
        return Sequence::kFailed;
    }
    if (c == ',') {
        ++scan.p;
        SkipWhitespace(scan);
        if (Peek(scan, &c) && c == closing) {
            Fail(scan, "a trailing comma");
            return Sequence::kFailed;
        }
        return Sequence::kContinue;
    }
    if (c == closing) {
        ++scan.p;
        return Sequence::kDone;
    }
    Fail(scan, "expected ',' or a closing bracket");
    return Sequence::kFailed;
}

bool SkipValue(Scan& scan, int depth) noexcept {
    if (depth > kMaxDepth) return Fail(scan, "the manifest nests too deeply");
    SkipWhitespace(scan);
    char c = 0;
    if (!Peek(scan, &c)) return Fail(scan, "a value was expected");
    switch (c) {
        case '"':
            return ScanString(scan, nullptr, 0u, nullptr);
        case '{': {
            ++scan.p;
            SkipWhitespace(scan);
            if (Peek(scan, &c) && c == '}') {
                ++scan.p;
                return true;
            }
            for (;;) {
                SkipWhitespace(scan);
                if (!ScanString(scan, nullptr, 0u, nullptr)) return false;
                SkipWhitespace(scan);
                if (!Peek(scan, &c) || c != ':') {
                    return Fail(scan, "expected ':' after a key");
                }
                ++scan.p;
                if (!SkipValue(scan, depth + 1)) return false;
                const Sequence next = NextInSequence(scan, '}');
                if (next == Sequence::kFailed) return false;
                if (next == Sequence::kDone) return true;
            }
        }
        case '[': {
            ++scan.p;
            SkipWhitespace(scan);
            if (Peek(scan, &c) && c == ']') {
                ++scan.p;
                return true;
            }
            for (;;) {
                if (!SkipValue(scan, depth + 1)) return false;
                const Sequence next = NextInSequence(scan, ']');
                if (next == Sequence::kFailed) return false;
                if (next == Sequence::kDone) return true;
            }
        }
        case 't': return SkipLiteral(scan, "true");
        case 'f': return SkipLiteral(scan, "false");
        case 'n': return SkipLiteral(scan, "null");
        default: return SkipNumber(scan);
    }
}

bool ScanPermissionArray(Scan& scan, PermissionBitResolver resolver,
                         PermissionBits* out_bits) noexcept {
    SkipWhitespace(scan);
    char c = 0;
    if (!Peek(scan, &c) || c != '[') {
        return Fail(scan, "\"permissions\" must be an array of strings");
    }
    ++scan.p;
    SkipWhitespace(scan);
    if (Peek(scan, &c) && c == ']') {
        ++scan.p;
        return true;
    }
    for (;;) {
        SkipWhitespace(scan);
        if (!Peek(scan, &c) || c != '"') {
            return Fail(scan,
                        "every entry in \"permissions\" must be a string");
        }
        char name[kMaxNameChars] = {};
        StringForm form = StringForm::kExact;
        if (!ScanString(scan, name, sizeof(name), &form)) return false;
        if (form == StringForm::kEscaped) {
            return Fail(scan,
                        "a permission name written with a \\u escape is not "
                        "understood here; write it as plain text");
        }
        *out_bits |= resolver(name);
        const Sequence next = NextInSequence(scan, ']');
        if (next == Sequence::kFailed) return false;
        if (next == Sequence::kDone) return true;
    }
}

bool ScanManifestString(Scan& scan, const char* field, char* out,
                        size_t out_capacity) noexcept {
    SkipWhitespace(scan);
    char c = 0;
    if (!Peek(scan, &c) || c != '"') {
        return Fail(scan, field);
    }
    StringForm form = StringForm::kExact;
    if (!ScanString(scan, out, out_capacity, &form)) return false;
    if (form == StringForm::kEscaped) {
        return Fail(scan,
                    "installed manifest id and entrypoint must not use "
                    "\\u escapes");
    }
    if (form == StringForm::kTooLong) {
        return Fail(scan, "an installed manifest field is too long");
    }
    return true;
}

}  // namespace

bool ScanLuaManifest(const char* json, size_t json_size,
                     PermissionBitResolver resolver, LuaManifest* out_manifest,
                     const char** out_error) noexcept {
    if (!json || !resolver || !out_manifest || !out_error) return false;
    *out_error = nullptr;
    *out_manifest = LuaManifest{};
    if (json_size > kMaxManifestBytes) {
        *out_error = "the manifest is too large to be one";
        return false;
    }
    Scan scan = {json, json + json_size, nullptr};
    bool seen_permissions = false;
    bool seen_id = false;
    bool seen_entrypoint = false;

    SkipWhitespace(scan);
    char c = 0;
    if (!Peek(scan, &c) || c != '{') {
        *out_error = "a manifest must be a JSON object";
        return false;
    }
    ++scan.p;
    SkipWhitespace(scan);
    if (Peek(scan, &c) && c == '}') {
        ++scan.p;
    } else {
        for (;;) {
            SkipWhitespace(scan);
            if (!Peek(scan, &c) || c != '"') {
                Fail(scan, "expected a key");
                break;
            }
            char key[kMaxNameChars] = {};
            StringForm key_form = StringForm::kExact;
            if (!ScanString(scan, key, sizeof(key), &key_form)) break;
            const bool usable = key_form == StringForm::kExact;
            SkipWhitespace(scan);
            if (!Peek(scan, &c) || c != ':') {
                Fail(scan, "expected ':' after a key");
                break;
            }
            ++scan.p;
            if (usable && std::strcmp(key, "permissions") == 0) {
                if (seen_permissions) {
                    Fail(scan, "the manifest names \"permissions\" twice");
                    break;
                }
                seen_permissions = true;
                if (!ScanPermissionArray(scan, resolver,
                                         &out_manifest->requested_permissions)) {
                    break;
                }
            } else if (usable && std::strcmp(key, "id") == 0) {
                if (seen_id) {
                    Fail(scan, "the manifest names \"id\" twice");
                    break;
                }
                seen_id = true;
                if (!ScanManifestString(
                        scan, "\"id\" must be a string", out_manifest->id,
                        sizeof(out_manifest->id))) {
                    break;
                }
                out_manifest->has_id = true;
            } else if (usable && std::strcmp(key, "entrypoint") == 0) {
                if (seen_entrypoint) {
                    Fail(scan, "the manifest names \"entrypoint\" twice");
                    break;
                }
                seen_entrypoint = true;
                if (!ScanManifestString(
                        scan, "\"entrypoint\" must be a string",
                        out_manifest->entrypoint,
                        sizeof(out_manifest->entrypoint))) {
                    break;
                }
                out_manifest->has_entrypoint = true;
            } else if (!SkipValue(scan, 1)) {
                break;
            }
            const Sequence next = NextInSequence(scan, '}');
            if (next == Sequence::kFailed) break;
            if (next == Sequence::kDone) break;
        }
    }
    if (!scan.error) {
        SkipWhitespace(scan);
        if (scan.p != scan.end) {
            Fail(scan, "trailing text after the manifest object");
        }
    }
    if (scan.error) {
        *out_error = scan.error;
        *out_manifest = LuaManifest{};
        return false;
    }
    return true;
}

bool ScanPermissionManifest(const char* json, PermissionBitResolver resolver,
                            PermissionBits* out_bits,
                            const char** out_error) noexcept {
    if (!json || !resolver || !out_bits || !out_error) return false;
    LuaManifest manifest;
    if (!ScanLuaManifest(json, std::strlen(json), resolver, &manifest,
                         out_error)) {
        return false;
    }
    *out_bits |= manifest.requested_permissions;
    return true;
}

}  // namespace lua
}  // namespace wotbmod
