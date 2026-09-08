#include "wotb_mod_v3_internal.h"
#include "data_services_backend.h"
#include "dava_native_registry.h"
#include "package_trust.h"
#include "dvpl_decoder.h"

#include "../../include/wotbmod/archive_v1.h"
#include "../../include/wotbmod/catalog_v1.h"
#include "../../include/wotbmod/content_v1.h"
#include "../../include/wotbmod/events_v1.h"
#include "../../include/wotbmod/input_v1.h"
#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/loaders_v1.h"
#include "../../include/wotbmod/manifest_v1.h"
#include "../../include/wotbmod/resources_v1.h"
#include "../../include/wotbmod/settings_v1.h"
#include "../../include/wotbmod/storage_v1.h"
#include "../../include/wotbmod/vfs_v1.h"
#include "../../include/wotbmod/vfs_v2.h"
#include "../../include/wotbmod/yaml_v1.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace wotbmod {
namespace v3 {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kDefaultReadLimit = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxStorageValue = 8ull * 1024ull * 1024ull;
constexpr uint64_t kMaxStorageTotal = 64ull * 1024ull * 1024ull;
constexpr uint32_t kMaxStorageEntries = 4096u;
constexpr uint32_t kMaxSettings = 512u;
constexpr uint32_t kMaxPresets = 128u;
constexpr uint32_t kMaxActionsPerMod = 256u;
constexpr uint32_t kMaxNativeInputEvents = 2048u;
constexpr uint32_t kMaxMountsPerMod = 128u;
constexpr uint32_t kMaxVfsListEntries = 8192u;
constexpr uint32_t kMaxWatchersPerMod = 64u;
constexpr uint32_t kMaxWatchersGlobal = 512u;
constexpr uint32_t kMaxWatchScansPerFrame = 64u;
constexpr uint64_t kMaxWatchHashBytesPerFrame =
    64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxWatchFileBytes =
    64ull * 1024ull * 1024ull;

enum class ObjectKind {
    OwnerAnchor,
    SettingsSubscription,
    StorageTransaction,
    InputAction,
    InputSubscription,
    VfsMount,
    VfsFile,
    PortableWatch,
    Resource,
    YamlDocument,
    Archive,
    Manifest,
    Content
};

struct TaggedObject {
    explicit TaggedObject(ObjectKind value) : kind(value) {}
    virtual ~TaggedObject() = default;
    ObjectKind kind;
};

WotbModV3Result Fail(
    WotbModV3Handle mod,
    WotbModV3Result code,
    const char* message) {
    return SetError(mod, code, message ? message : "data service failure");
}

bool ValidStruct(const void* value, uint32_t actual, uint32_t required) {
    return value != nullptr && actual >= required;
}

bool IsAsciiIdentifier(
    const char* value,
    size_t max_length,
    bool allow_slash = false) {
    if (!value || !*value) {
        return false;
    }
    const size_t length = std::strlen(value);
    if (length >= max_length) {
        return false;
    }
    if (value[0] == '.' || value[length - 1] == '.') {
        return false;
    }
    bool previous_dot = false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const bool accepted =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' ||
            (allow_slash && c == '/');
        if (!accepted || (allow_slash && c == '/' &&
                          (i == 0 || i + 1 == length))) {
            return false;
        }
        if (c == '.' && previous_dot) {
            return false;
        }
        previous_dot = c == '.';
    }
    if (allow_slash) {
        std::string part;
        std::istringstream stream(value);
        while (std::getline(stream, part, '/')) {
            if (part.empty() || part == "." || part == "..") {
                return false;
            }
        }
    }
    return true;
}

bool CopyFixed(char* destination, size_t capacity, const std::string& value) {
    if (!destination || capacity == 0 || value.size() >= capacity) {
        return false;
    }
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    return true;
}

WotbModV3Result CopyOutString(
    WotbModV3Handle mod,
    const std::string& value,
    char* buffer,
    uint32_t* inout_size) {
    if (!inout_size) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "output size pointer is null");
    }
    const uint64_t required64 = value.size() + 1u;
    if (required64 > std::numeric_limits<uint32_t>::max()) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "output string exceeds ABI size range");
    }
    const uint32_t required = static_cast<uint32_t>(required64);
    if (!buffer || *inout_size < required) {
        *inout_size = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value.c_str(), required);
    *inout_size = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CopyOutBytes(
    WotbModV3Handle mod,
    const uint8_t* data,
    size_t size,
    WotbModV3Buffer* buffer) {
    if (!ValidStruct(buffer, buffer ? buffer->struct_size : 0,
                     sizeof(WotbModV3Buffer))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid output buffer descriptor");
    }
    if (size > std::numeric_limits<uint32_t>::max()) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "output buffer exceeds ABI size range");
    }
    buffer->size = static_cast<uint32_t>(size);
    if (!buffer->data || buffer->capacity < size) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    if (size != 0) {
        std::memcpy(buffer->data, data, size);
    }
    return WOTBMOD_V3_OK;
}

std::string ModNamespace(WotbModV3Handle mod) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "mod-%016llx",
                  static_cast<unsigned long long>(mod));
    return text;
}

std::string PathFromGetter(
    WotbModV3Handle mod,
    WotbModV3Result (*getter)(WotbModV3Handle, char*, uint32_t*)) {
    uint32_t size = 0;
    WotbModV3Result first = getter(mod, nullptr, &size);
    if (first != WOTBMOD_V3_E_BUFFER_TOO_SMALL || size == 0 ||
        size > WOTBMOD_V3_MAX_PATH * 4u) {
        return {};
    }
    std::vector<char> buffer(size);
    if (getter(mod, buffer.data(), &size) != WOTBMOD_V3_OK) {
        return {};
    }
    return std::string(buffer.data());
}

std::string ModDataPath(WotbModV3Handle mod) {
    return PathFromGetter(mod, GetModDataDirectory);
}

std::string ModCachePath(WotbModV3Handle mod) {
    return PathFromGetter(mod, GetModCacheDirectory);
}

std::string ModConfigPath(WotbModV3Handle mod) {
    return PathFromGetter(mod, GetModConfigDirectory);
}

bool CanonicalWithin(
    const fs::path& root,
    const fs::path& candidate,
    bool require_existing,
    fs::path* out_path) {
    std::error_code ec;
    fs::path root_canonical = fs::weakly_canonical(root, ec);
    if (ec || root_canonical.empty()) {
        return false;
    }
    fs::path candidate_canonical;
    if (require_existing) {
        candidate_canonical = fs::canonical(candidate, ec);
    } else {
        /*
         * MSVC's weakly_canonical may fail for a multi-segment missing tail.
         * Resolve the nearest existing ancestor (including symlinks), then
         * append only lexical, already-parsed URI segments.
         */
        fs::path existing = candidate;
        std::vector<fs::path> missing_tail;
        for (;;) {
            ec.clear();
            if (fs::exists(existing, ec)) break;
            if (ec &&
                ec != std::make_error_code(
                          std::errc::no_such_file_or_directory) &&
                ec != std::make_error_code(std::errc::not_a_directory)) {
                return false;
            }
            const fs::path parent = existing.parent_path();
            const fs::path leaf = existing.filename();
            if (parent.empty() || parent == existing || leaf.empty()) {
                return false;
            }
            missing_tail.push_back(leaf);
            existing = parent;
        }
        candidate_canonical = fs::canonical(existing, ec);
        if (!ec) {
            for (auto it = missing_tail.rbegin();
                 it != missing_tail.rend();
                 ++it) {
                candidate_canonical /= *it;
            }
            candidate_canonical = candidate_canonical.lexically_normal();
        }
    }
    if (ec || candidate_canonical.empty()) {
        return false;
    }
    auto root_it = root_canonical.begin();
    auto candidate_it = candidate_canonical.begin();
    for (; root_it != root_canonical.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate_canonical.end()) {
            return false;
        }
#if defined(_WIN32)
        std::wstring left = root_it->wstring();
        std::wstring right = candidate_it->wstring();
        std::transform(left.begin(), left.end(), left.begin(), towlower);
        std::transform(right.begin(), right.end(), right.begin(), towlower);
        if (left != right) {
            return false;
        }
#else
        if (*root_it != *candidate_it) {
            return false;
        }
#endif
    }
    if (out_path) {
        *out_path = candidate_canonical;
    }
    return true;
}

bool IsAllowedOwnedPhysicalPath(
    WotbModV3Handle mod,
    const fs::path& candidate,
    bool require_existing,
    fs::path* out_path) {
    const std::vector<fs::path> roots = {
        fs::path(ModDataPath(mod)),
        fs::path(ModCachePath(mod)),
        fs::path(ModConfigPath(mod)),
        fs::path(ModsDirectory() ? ModsDirectory() : "")
    };
    for (const fs::path& root : roots) {
        if (!root.empty() &&
            CanonicalWithin(root, candidate, require_existing, out_path)) {
            return true;
        }
    }
    return false;
}

WotbModV3Result ReadPhysicalFile(
    WotbModV3Handle mod,
    const fs::path& path,
    uint64_t max_bytes,
    std::vector<uint8_t>* out) {
    if (!out || max_bytes == 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid file read request");
    }
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO, "cannot query file size");
    }
    if (size > max_bytes || size > std::numeric_limits<size_t>::max()) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "file exceeds configured read limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Fail(mod, WOTBMOD_V3_E_IO, "cannot open file for reading");
    }
    out->assign(static_cast<size_t>(size), 0);
    if (size != 0) {
        input.read(reinterpret_cast<char*>(out->data()),
                   static_cast<std::streamsize>(size));
        if (!input || static_cast<uint64_t>(input.gcount()) != size) {
            out->clear();
            return Fail(mod, WOTBMOD_V3_E_IO, "short file read");
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result AtomicWrite(
    WotbModV3Handle mod,
    const fs::path& destination,
    const uint8_t* data,
    size_t size) {
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot create storage directory");
    }
    fs::path temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot create temporary storage file");
        }
        if (size != 0) {
            output.write(reinterpret_cast<const char*>(data),
                         static_cast<std::streamsize>(size));
        }
        output.flush();
        if (!output) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot flush temporary storage file");
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, ec);
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "atomic storage replace failed");
    }
#else
    fs::rename(temporary, destination, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "atomic storage replace failed");
    }
#endif
    return WOTBMOD_V3_OK;
}

class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(const void* input, size_t length) {
        const uint8_t* bytes = static_cast<const uint8_t*>(input);
        total_bytes_ += length;
        while (length != 0) {
            const size_t available = 64u - buffered_;
            const size_t count = std::min(available, length);
            std::memcpy(buffer_ + buffered_, bytes, count);
            buffered_ += count;
            bytes += count;
            length -= count;
            if (buffered_ == 64u) {
                Transform(buffer_);
                buffered_ = 0;
            }
        }
    }

    std::string FinalHex() {
        const uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffered_++] = 0x80u;
        if (buffered_ > 56u) {
            while (buffered_ < 64u) {
                buffer_[buffered_++] = 0;
            }
            Transform(buffer_);
            buffered_ = 0;
        }
        while (buffered_ < 56u) {
            buffer_[buffered_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffered_++] =
                static_cast<uint8_t>((bit_length >> shift) & 0xffu);
        }
        Transform(buffer_);
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (uint32_t value : state_) {
            output << std::setw(8) << value;
        }
        const std::string result = output.str();
        Reset();
        return result;
    }

private:
    static uint32_t Rotate(uint32_t value, uint32_t count) {
        return (value >> count) | (value << (32u - count));
    }

    void Reset() {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
        total_bytes_ = 0;
        buffered_ = 0;
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    void Transform(const uint8_t block[64]) {
        static const uint32_t constants[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };
        uint32_t words[64] = {};
        for (uint32_t i = 0; i < 16; ++i) {
            words[i] =
                (static_cast<uint32_t>(block[i * 4]) << 24u) |
                (static_cast<uint32_t>(block[i * 4 + 1]) << 16u) |
                (static_cast<uint32_t>(block[i * 4 + 2]) << 8u) |
                static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (uint32_t i = 16; i < 64; ++i) {
            const uint32_t s0 =
                Rotate(words[i - 15], 7) ^
                Rotate(words[i - 15], 18) ^
                (words[i - 15] >> 3u);
            const uint32_t s1 =
                Rotate(words[i - 2], 17) ^
                Rotate(words[i - 2], 19) ^
                (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 +
                       words[i - 7] + s1;
        }
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        for (uint32_t i = 0; i < 64; ++i) {
            const uint32_t sum1 =
                Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const uint32_t choice = (e & f) ^ ((~e) & g);
            const uint32_t temp1 =
                h + sum1 + choice + constants[i] + words[i];
            const uint32_t sum0 =
                Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const uint32_t majority =
                (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    uint32_t state_[8] = {};
    uint8_t buffer_[64] = {};
    uint64_t total_bytes_ = 0;
    size_t buffered_ = 0;
};

std::string Sha256Bytes(const void* data, size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    return hash.FinalHex();
}

WotbModV3Result Sha256File(
    WotbModV3Handle mod,
    const fs::path& path,
    std::string* out_hash) {
    if (!out_hash) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "hash output is null");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot open file for hashing");
    }
    Sha256 hash;
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hash.Update(buffer, static_cast<size_t>(count));
        }
    }
    if (!input.eof()) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "file hashing failed");
    }
    *out_hash = hash.FinalHex();
    return WOTBMOD_V3_OK;
}

bool IsSha256(const std::string& text) {
    if (text.size() != 64u) {
        return false;
    }
    return std::all_of(
        text.begin(), text.end(),
        [](unsigned char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        });
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<std::pair<std::string, JsonValue>> object;
    std::vector<JsonValue> array;
};

class JsonParser {
public:
    JsonParser(const char* data, size_t size)
        : data_(data), size_(size) {}

    bool Parse(JsonValue* out, std::string* error) {
        if (!out || !data_ || size_ > 8u * 1024u * 1024u) {
            SetParserError(error, "invalid or oversized JSON input");
            return false;
        }
        SkipWhitespace();
        if (!ParseValue(0, out, error)) {
            return false;
        }
        SkipWhitespace();
        if (position_ != size_) {
            SetParserError(error, "trailing JSON data");
            return false;
        }
        return true;
    }

private:
    void SetParserError(std::string* error, const char* message) const {
        if (error) {
            std::ostringstream output;
            output << message << " at byte " << position_;
            *error = output.str();
        }
    }

    void SkipWhitespace() {
        while (position_ < size_) {
            const char c = data_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool ParseValue(
        uint32_t depth,
        JsonValue* out,
        std::string* error) {
        if (depth > 64u || ++nodes_ > 100000u) {
            SetParserError(error, "JSON nesting or node limit exceeded");
            return false;
        }
        SkipWhitespace();
        if (position_ >= size_) {
            SetParserError(error, "unexpected end of JSON");
            return false;
        }
        const char c = data_[position_];
        if (c == '{') {
            return ParseObject(depth, out, error);
        }
        if (c == '[') {
            return ParseArray(depth, out, error);
        }
        if (c == '"') {
            out->type = JsonValue::Type::String;
            return ParseString(&out->text, error);
        }
        if (c == 't' && Match("true")) {
            out->type = JsonValue::Type::Bool;
            out->boolean = true;
            return true;
        }
        if (c == 'f' && Match("false")) {
            out->type = JsonValue::Type::Bool;
            out->boolean = false;
            return true;
        }
        if (c == 'n' && Match("null")) {
            out->type = JsonValue::Type::Null;
            return true;
        }
        return ParseNumber(out, error);
    }

    bool ParseObject(
        uint32_t depth,
        JsonValue* out,
        std::string* error) {
        ++position_;
        out->type = JsonValue::Type::Object;
        SkipWhitespace();
        if (position_ < size_ && data_[position_] == '}') {
            ++position_;
            return true;
        }
        std::set<std::string> keys;
        while (position_ < size_) {
            if (data_[position_] != '"') {
                SetParserError(error, "object key must be a string");
                return false;
            }
            std::string key;
            if (!ParseString(&key, error)) {
                return false;
            }
            if (!keys.insert(key).second) {
                SetParserError(error, "duplicate object key");
                return false;
            }
            SkipWhitespace();
            if (position_ >= size_ || data_[position_++] != ':') {
                SetParserError(error, "missing object colon");
                return false;
            }
            JsonValue value;
            if (!ParseValue(depth + 1u, &value, error)) {
                return false;
            }
            out->object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (position_ >= size_) {
                SetParserError(error, "unterminated object");
                return false;
            }
            const char separator = data_[position_++];
            if (separator == '}') {
                return true;
            }
            if (separator != ',') {
                SetParserError(error, "invalid object separator");
                return false;
            }
            SkipWhitespace();
        }
        SetParserError(error, "unterminated object");
        return false;
    }

    bool ParseArray(
        uint32_t depth,
        JsonValue* out,
        std::string* error) {
        ++position_;
        out->type = JsonValue::Type::Array;
        SkipWhitespace();
        if (position_ < size_ && data_[position_] == ']') {
            ++position_;
            return true;
        }
        while (position_ < size_) {
            JsonValue value;
            if (!ParseValue(depth + 1u, &value, error)) {
                return false;
            }
            out->array.emplace_back(std::move(value));
            SkipWhitespace();
            if (position_ >= size_) {
                SetParserError(error, "unterminated array");
                return false;
            }
            const char separator = data_[position_++];
            if (separator == ']') {
                return true;
            }
            if (separator != ',') {
                SetParserError(error, "invalid array separator");
                return false;
            }
            SkipWhitespace();
        }
        SetParserError(error, "unterminated array");
        return false;
    }

    bool ParseString(std::string* out, std::string* error) {
        if (position_ >= size_ || data_[position_++] != '"') {
            return false;
        }
        out->clear();
        while (position_ < size_) {
            const unsigned char c =
                static_cast<unsigned char>(data_[position_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20u) {
                SetParserError(error, "control character in string");
                return false;
            }
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= size_) {
                SetParserError(error, "unterminated string escape");
                return false;
            }
            const char escape = data_[position_++];
            switch (escape) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t codepoint = 0;
                    if (!ParseHex4(&codepoint)) {
                        SetParserError(error, "invalid unicode escape");
                        return false;
                    }
                    if (codepoint == 0u) {
                        SetParserError(
                            error,
                            "embedded NUL is not supported in JSON strings");
                        return false;
                    }
                    if (codepoint >= 0xd800u && codepoint <= 0xdbffu) {
                        if (position_ + 6u > size_ ||
                            data_[position_] != '\\' ||
                            data_[position_ + 1u] != 'u') {
                            SetParserError(error, "missing low surrogate");
                            return false;
                        }
                        position_ += 2u;
                        uint32_t low = 0;
                        if (!ParseHex4(&low) ||
                            low < 0xdc00u || low > 0xdfffu) {
                            SetParserError(error, "invalid low surrogate");
                            return false;
                        }
                        codepoint = 0x10000u +
                            ((codepoint - 0xd800u) << 10u) +
                            (low - 0xdc00u);
                    } else if (codepoint >= 0xdc00u &&
                               codepoint <= 0xdfffu) {
                        SetParserError(error, "unexpected low surrogate");
                        return false;
                    }
                    AppendUtf8(codepoint, out);
                    break;
                }
                default:
                    SetParserError(error, "unsupported string escape");
                    return false;
            }
            if (out->size() > 1024u * 1024u) {
                SetParserError(error, "JSON string limit exceeded");
                return false;
            }
        }
        SetParserError(error, "unterminated string");
        return false;
    }

    bool ParseHex4(uint32_t* out) {
        if (position_ + 4u > size_) {
            return false;
        }
        uint32_t value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            const char c = data_[position_++];
            value <<= 4u;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        *out = value;
        return true;
    }

    static void AppendUtf8(uint32_t codepoint, std::string* output) {
        if (codepoint <= 0x7fu) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffu) {
            output->push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
            output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else if (codepoint <= 0xffffu) {
            output->push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
            output->push_back(static_cast<char>(
                0x80u | ((codepoint >> 6u) & 0x3fu)));
            output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        } else {
            output->push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
            output->push_back(static_cast<char>(
                0x80u | ((codepoint >> 12u) & 0x3fu)));
            output->push_back(static_cast<char>(
                0x80u | ((codepoint >> 6u) & 0x3fu)));
            output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    bool ParseNumber(JsonValue* out, std::string* error) {
        const size_t start = position_;
        if (position_ < size_ && data_[position_] == '-') {
            ++position_;
        }
        if (position_ >= size_) {
            SetParserError(error, "invalid number");
            return false;
        }
        if (data_[position_] == '0') {
            ++position_;
            if (position_ < size_ &&
                data_[position_] >= '0' && data_[position_] <= '9') {
                SetParserError(error, "leading zero in number");
                return false;
            }
        } else if (data_[position_] >= '1' && data_[position_] <= '9') {
            while (position_ < size_ &&
                   data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
        } else {
            SetParserError(error, "invalid number");
            return false;
        }
        if (position_ < size_ && data_[position_] == '.') {
            ++position_;
            const size_t digits = position_;
            while (position_ < size_ &&
                   data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                SetParserError(error, "missing fractional digits");
                return false;
            }
        }
        if (position_ < size_ &&
            (data_[position_] == 'e' || data_[position_] == 'E')) {
            ++position_;
            if (position_ < size_ &&
                (data_[position_] == '+' || data_[position_] == '-')) {
                ++position_;
            }
            const size_t digits = position_;
            while (position_ < size_ &&
                   data_[position_] >= '0' && data_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                SetParserError(error, "missing exponent digits");
                return false;
            }
        }
        const std::string number(data_ + start, position_ - start);
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(number.c_str(), &end);
        if (errno == ERANGE || !end || *end != '\0' ||
            !std::isfinite(value)) {
            SetParserError(error, "number outside finite range");
            return false;
        }
        out->type = JsonValue::Type::Number;
        out->number = value;
        return true;
    }

    bool Match(const char* literal) {
        const size_t length = std::strlen(literal);
        if (position_ + length > size_ ||
            std::memcmp(data_ + position_, literal, length) != 0) {
            return false;
        }
        position_ += length;
        return true;
    }

    const char* data_ = nullptr;
    size_t size_ = 0;
    size_t position_ = 0;
    uint32_t nodes_ = 0;
};

const JsonValue* JsonField(const JsonValue& object, const char* key) {
    if (object.type != JsonValue::Type::Object) {
        return nullptr;
    }
    for (const auto& item : object.object) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

bool JsonString(
    const JsonValue& object,
    const char* key,
    std::string* out,
    bool required = true) {
    const JsonValue* value = JsonField(object, key);
    if (!value) {
        return !required;
    }
    if (value->type != JsonValue::Type::String) {
        return false;
    }
    if (out) {
        *out = value->text;
    }
    return true;
}

bool ParseJsonBuffer(
    const WotbModV3ConstBuffer* input,
    JsonValue* output,
    std::string* error) {
    if (!ValidStruct(input, input ? input->struct_size : 0,
                     sizeof(WotbModV3ConstBuffer)) ||
        !input->data || input->size == 0) {
        if (error) {
            *error = "invalid JSON buffer descriptor";
        }
        return false;
    }
    JsonParser parser(
        static_cast<const char*>(input->data),
        input->size);
    return parser.Parse(output, error);
}

template <typename T>
void DestroyObject(void* object) {
    delete static_cast<T*>(object);
}

template <typename T>
WotbModV3Result GetObject(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t handle_type,
    ObjectKind kind,
    T** out) {
    if (!out) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "object output pointer is null");
    }
    void* raw = nullptr;
    WotbModV3Result result =
        InspectOwnedHandle(mod, handle, handle_type, &raw, nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    TaggedObject* tagged = static_cast<TaggedObject*>(raw);
    if (!tagged || tagged->kind != kind) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_HANDLE,
                    "handle belongs to another data service object");
    }
    *out = static_cast<T*>(tagged);
    return WOTBMOD_V3_OK;
}

}  // namespace

namespace {

WotbModV3Result EnsureDataOwner(WotbModV3Handle mod);
WotbModV3Result VfsAccess(WotbModV3Handle mod);
WotbModV3Result VfsOverlayAccess(WotbModV3Handle mod);
WotbModV3Result ReadUri(
    WotbModV3Handle mod,
    const char* uri,
    uint64_t max_bytes,
    std::vector<uint8_t>* out,
    std::string* out_normalized = nullptr);
WotbModV3Result ResolveWritableUri(
    WotbModV3Handle mod,
    const char* uri_text,
    fs::path* out_path);
WotbModV3Result NormalizeOwnedUri(
    WotbModV3Handle mod,
    const char* uri_text,
    std::string* out_normalized);
WotbModV3Result WOTBMOD_V3_CALL YamlParseUri(
    WotbModV3Handle mod,
    const char* uri,
    const WotbModV3YamlLimits* limits,
    WotbModV3Handle* out_document);
WotbModV3Result LoadNativeDavaYamlSnapshot(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_document);
WotbModV3Result OpenNativeDavaArchiveSnapshot(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3ArchiveHandle* out_archive);
bool SafeRelativeValue(const std::string& value);
bool IsSemanticVersion(const std::string& value);
bool IsSafeGameUri(
    WotbModV3Handle mod,
    const std::string& value);
std::string Trim(const std::string& input);
bool IsUriSegmentSafe(const std::string& segment);

bool FixedTerminated(const char* text, size_t capacity) {
    return text && std::memchr(text, '\0', capacity) != nullptr;
}

template <typename T>
void AppendPod(std::vector<uint8_t>* bytes, const T& value) {
    const uint8_t* begin =
        reinterpret_cast<const uint8_t*>(&value);
    bytes->insert(bytes->end(), begin, begin + sizeof(T));
}

void AppendString16(
    std::vector<uint8_t>* bytes,
    const std::string& text) {
    const uint16_t size = static_cast<uint16_t>(text.size());
    AppendPod(bytes, size);
    bytes->insert(bytes->end(), text.begin(), text.end());
}

template <typename T>
bool ReadPod(
    const std::vector<uint8_t>& bytes,
    size_t* position,
    T* out) {
    if (!position || !out ||
        *position > bytes.size() ||
        bytes.size() - *position < sizeof(T)) {
        return false;
    }
    std::memcpy(out, bytes.data() + *position, sizeof(T));
    *position += sizeof(T);
    return true;
}

bool ReadString16(
    const std::vector<uint8_t>& bytes,
    size_t* position,
    std::string* out,
    size_t max_size) {
    uint16_t size = 0;
    if (!ReadPod(bytes, position, &size) ||
        size > max_size ||
        *position > bytes.size() ||
        bytes.size() - *position < size) {
        return false;
    }
    out->assign(
        reinterpret_cast<const char*>(bytes.data() + *position),
        size);
    *position += size;
    return true;
}

bool IsValidUtf8(const uint8_t* data, size_t size) {
    if (!data && size != 0) return false;
    size_t position = 0;
    while (position < size) {
        const uint8_t first = data[position++];
        if (first <= 0x7fu) {
            if (first == 0) return false;
            continue;
        }
        uint32_t codepoint = 0;
        size_t continuation_count = 0;
        if ((first & 0xe0u) == 0xc0u) {
            codepoint = first & 0x1fu;
            continuation_count = 1;
            if (codepoint < 2u) return false;
        } else if ((first & 0xf0u) == 0xe0u) {
            codepoint = first & 0x0fu;
            continuation_count = 2;
        } else if ((first & 0xf8u) == 0xf0u) {
            codepoint = first & 0x07u;
            continuation_count = 3;
        } else {
            return false;
        }
        if (position + continuation_count > size) return false;
        for (size_t i = 0; i < continuation_count; ++i) {
            const uint8_t continuation = data[position++];
            if ((continuation & 0xc0u) != 0x80u) return false;
            codepoint =
                (codepoint << 6u) | (continuation & 0x3fu);
        }
        if ((continuation_count == 1u && codepoint < 0x80u) ||
            (continuation_count == 2u && codepoint < 0x800u) ||
            (continuation_count == 3u && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

WotbModV3Result LoadersAccess(WotbModV3Handle mod) {
    return VfsAccess(mod);
}

WotbModV3Result ResolveNativeDavaPath(
    WotbModV3Handle mod,
    const char* uri,
    std::vector<char>* out_path) {
    if (!uri || !out_path) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "native DAVA path request is invalid");
    }
    uint32_t required = 0u;
    WotbModV3Result result =
        ResolveVfsUriPhysical(mod, uri, nullptr, &required);
    if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL || required == 0u ||
        required > WOTBMOD_V3_MAX_PATH * 4u) {
        return result == WOTBMOD_V3_OK
            ? Fail(mod, WOTBMOD_V3_E_PLATFORM,
                   "VFS returned an invalid native DAVA path size")
            : result;
    }
    out_path->assign(required, '\0');
    result = ResolveVfsUriPhysical(
        mod, uri, out_path->data(), &required);
    if (result != WOTBMOD_V3_OK) {
        out_path->clear();
    }
    return result;
}

WotbModV3Result NativeDavaFailure(
    WotbModV3Handle mod,
    WotbModV3Result result,
    const char* operation) {
    if (result == WOTBMOD_V3_E_NOT_SUPPORTED) {
        std::string message = operation ? operation : "native DAVA operation";
        message +=
            " is not bound for this client build; no native result was fabricated";
        return Fail(mod, result, message.c_str());
    }
    return SetError(
        mod,
        result == WOTBMOD_V3_OK ? WOTBMOD_V3_E_PLATFORM : result,
        operation ? operation : "native DAVA provider failed");
}

WotbModV3Result LoadUriToBuffer(
    WotbModV3Handle mod,
    const char* uri,
    uint64_t max_bytes,
    bool validate_utf8,
    WotbModV3Buffer* inout_buffer) {
    if (!uri || max_bytes == 0 ||
        max_bytes > kDefaultReadLimit) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "loader read limit is invalid");
    }
    std::vector<uint8_t> bytes;
    WotbModV3Result read =
        ReadUri(mod, uri, max_bytes, &bytes);
    if (read != WOTBMOD_V3_OK) return read;
    if (validate_utf8 && !IsValidUtf8(bytes.data(), bytes.size())) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "text resource is not strict UTF-8 or contains NUL");
    }
    return CopyOutBytes(
        mod, bytes.data(), bytes.size(), inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersLoadText(
    WotbModV3Handle mod,
    const char* uri,
    uint64_t max_bytes,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return LoadUriToBuffer(
        mod, uri, max_bytes, true, inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersLoadBinary(
    WotbModV3Handle mod,
    const char* uri,
    uint64_t max_bytes,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return LoadUriToBuffer(
        mod, uri, max_bytes, false, inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersLoadYaml(
    WotbModV3Handle mod,
    const char* uri,
    const WotbModV3YamlLimits* limits,
    WotbModV3Handle* out_document) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return YamlParseUri(mod, uri, limits, out_document);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersLoadDavaYaml(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_document) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!uri || !out_document) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid DAVA YAML load request");
    }
    *out_document = WOTBMOD_V3_INVALID_HANDLE;
    return LoadNativeDavaYamlSnapshot(mod, uri, out_document);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersUnpackDvpl(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!uri ||
        !ValidStruct(
            inout_buffer,
            inout_buffer ? inout_buffer->struct_size : 0,
            sizeof(WotbModV3Buffer))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid DVPL unpack request");
    }
    std::vector<uint8_t> file_bytes;
    WotbModV3Result read =
        ReadUri(mod, uri, kDefaultReadLimit, &file_bytes);
    if (read != WOTBMOD_V3_OK) return read;

    std::vector<uint8_t> unpacked;
    DvplDecodeInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_DVPL_DECODER_VERSION;
    std::string error;
    const WotbModV3Result decoded = DecodeDvpl(
        file_bytes.data(),
        file_bytes.size(),
        kDefaultReadLimit,
        &unpacked,
        &info,
        &error);
    if (decoded != WOTBMOD_V3_OK) {
        return Fail(
            mod,
            decoded,
            error.empty()
                ? "DVPL decode failed"
                : error.c_str());
    }
    return CopyOutBytes(
        mod,
        unpacked.data(),
        unpacked.size(),
        inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersOpenDavaArchive(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3ArchiveHandle* out_archive) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!uri || !out_archive) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid DAVA archive request");
    }
    *out_archive = WOTBMOD_V3_INVALID_HANDLE;
    return OpenNativeDavaArchiveSnapshot(mod, uri, out_archive);
}

WotbModV3Result WOTBMOD_V3_CALL LoadersGetBackendInfo(
    WotbModV3Handle mod,
    uint32_t backend,
    WotbModV3LoaderBackendInfo* out_info) {
    WotbModV3Result access = LoadersAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_info, out_info ? out_info->struct_size : 0,
            sizeof(WotbModV3LoaderBackendInfo))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid loader backend info output");
    }
    out_info->backend = backend;
    std::string name;
    std::string reason;
    switch (backend) {
        case WOTBMOD_V3_LOADER_UTF8_TEXT:
            name = "strict UTF-8 text";
            out_info->available = 1u;
            break;
        case WOTBMOD_V3_LOADER_BINARY:
            name = "bounded binary";
            out_info->available = 1u;
            break;
        case WOTBMOD_V3_LOADER_MINIMAL_YAML:
            name = "strict minimal YAML";
            out_info->available = 1u;
            break;
        case WOTBMOD_V3_LOADER_DAVA_YAML:
            name = "DAVA YAML";
            out_info->available =
                (InstalledDavaNativeCapabilities() &
                 WOTBMOD_DAVA_NATIVE_CAP_YAML) != 0u
                    ? 1u : 0u;
            if (!out_info->available) {
                reason = "native DAVA YAML provider is not installed";
            }
            break;
        case WOTBMOD_V3_LOADER_DVPL:
            name = "bounded DVPL";
            reason.clear();
            out_info->available = 1u;
            break;
        case WOTBMOD_V3_LOADER_DAVA_ARCHIVE:
            name = "DAVA archive";
            out_info->available =
                (InstalledDavaNativeCapabilities() &
                 WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE) != 0u
                    ? 1u : 0u;
            if (!out_info->available) {
                reason = "native DAVA archive provider is not installed";
            }
            break;
        default:
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "unknown loader backend");
    }
    CopyFixed(out_info->name, sizeof(out_info->name), name);
    CopyFixed(
        out_info->unavailable_reason,
        sizeof(out_info->unavailable_reason), reason);
    return WOTBMOD_V3_OK;
}

void CleanupOwnerState(WotbModV3Handle owner);

}  // namespace

namespace {

WotbModV3Result WOTBMOD_V3_CALL VfsMountOverlay(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* target_game_uri,
    const char* source_mod_uri,
    int32_t priority,
    WotbModV3Handle* out_mount);
WotbModV3Result WOTBMOD_V3_CALL VfsUnmount(
    WotbModV3Handle mod,
    WotbModV3Handle mount_handle);

struct ContentObject final : TaggedObject {
    ContentObject()
        : TaggedObject(ObjectKind::Content) {}
    std::mutex mutex;
    std::string id;
    std::string name;
    std::string version;
    std::vector<WotbModV3ContentOverride> overrides;
    std::vector<WotbModV3Handle> mounts;
    bool applied = false;
};

uint32_t ContentKindFromKey(const std::string& key) {
    if (key == "audio") return WOTBMOD_V3_CONTENT_AUDIO;
    if (key == "textures") return WOTBMOD_V3_CONTENT_TEXTURE;
    if (key == "ui") return WOTBMOD_V3_CONTENT_UI;
    if (key == "hangar") return WOTBMOD_V3_CONTENT_HANGAR;
    if (key == "models") return WOTBMOD_V3_CONTENT_MODEL;
    if (key == "localization")
        return WOTBMOD_V3_CONTENT_LOCALIZATION;
    return 0;
}

bool ValidContentTarget(
    WotbModV3Handle mod,
    uint32_t kind,
    const std::string& target) {
    if (target.empty() || target.size() >= WOTBMOD_V3_MAX_PATH) {
        return false;
    }
    if (kind == WOTBMOD_V3_CONTENT_AUDIO ||
        kind == WOTBMOD_V3_CONTENT_HANGAR ||
        kind == WOTBMOD_V3_CONTENT_LOCALIZATION) {
        return IsAsciiIdentifier(
            target.c_str(), WOTBMOD_V3_MAX_PATH, true);
    }
    return IsSafeGameUri(mod, target);
}

bool ParseContentRoot(
    WotbModV3Handle mod,
    const JsonValue& root,
    ContentObject* content,
    std::string* error) {
    if (!content || root.type != JsonValue::Type::Object) {
        if (error) *error = "content root must be an object";
        return false;
    }
    std::string type;
    if (!JsonString(root, "type", &type) || type != "content" ||
        !JsonString(root, "id", &content->id)) {
        if (error) *error = "content descriptor requires type=content and id";
        return false;
    }
    if (!JsonString(root, "name", &content->name, false) ||
        !JsonString(root, "version", &content->version, false)) {
        if (error) *error = "content name/version must be strings";
        return false;
    }
    if (content->name.empty()) content->name = content->id;
    if (content->version.empty()) content->version = "0.0.0";
    if (!IsAsciiIdentifier(
            content->id.c_str(), WOTBMOD_V3_MAX_ID, false) ||
        content->name.size() >= WOTBMOD_V3_MAX_NAME ||
        !IsSemanticVersion(content->version)) {
        if (error) *error = "content identity fields are invalid";
        return false;
    }
    const JsonValue* overrides = JsonField(root, "overrides");
    if (!overrides ||
        overrides->type != JsonValue::Type::Object) {
        if (error) *error = "content overrides must be an object";
        return false;
    }
    std::set<std::pair<uint32_t, std::string>> unique;
    for (const auto& category : overrides->object) {
        const uint32_t kind = ContentKindFromKey(category.first);
        if (kind == 0 ||
            category.second.type != JsonValue::Type::Object) {
            if (error) *error = "unknown or invalid content override category";
            return false;
        }
        for (const auto& item : category.second.object) {
            if (content->overrides.size() >= 4096u ||
                !ValidContentTarget(mod, kind, item.first) ||
                !unique.insert({kind, item.first}).second) {
                if (error) *error = "content target is unsafe or duplicated";
                return false;
            }
            std::string asset;
            int32_t priority = 0;
            if (item.second.type == JsonValue::Type::String) {
                asset = item.second.text;
            } else if (item.second.type ==
                       JsonValue::Type::Object) {
                if (!JsonString(item.second, "asset", &asset)) {
                    if (error) *error = "content override asset is missing";
                    return false;
                }
                const JsonValue* priority_value =
                    JsonField(item.second, "priority");
                if (priority_value) {
                    if (priority_value->type != JsonValue::Type::Number ||
                        priority_value->number !=
                            std::floor(priority_value->number) ||
                        priority_value->number < -100000.0 ||
                        priority_value->number > 100000.0) {
                        if (error) *error = "content priority is invalid";
                        return false;
                    }
                    priority =
                        static_cast<int32_t>(priority_value->number);
                }
            } else {
                if (error) *error = "content override must be a string or object";
                return false;
            }
            if (!SafeRelativeValue(asset)) {
                if (error) *error = "content asset path escapes the package";
                return false;
            }
            WotbModV3ContentOverride override_value = {};
            override_value.struct_size = sizeof(override_value);
            override_value.api_version = WOTBMOD_V3_CONTENT_VERSION;
            override_value.kind = kind;
            override_value.priority = priority;
            CopyFixed(
                override_value.target,
                sizeof(override_value.target),
                item.first);
            CopyFixed(
                override_value.asset_path,
                sizeof(override_value.asset_path),
                asset);
            content->overrides.push_back(override_value);
        }
    }
    if (content->overrides.empty()) {
        if (error) *error = "content descriptor has no overrides";
        return false;
    }
    return true;
}

WotbModV3Result ContentAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "content");
}

WotbModV3Result ParseContentData(
    WotbModV3Handle mod,
    const void* data,
    size_t size,
    WotbModV3Handle* out_content) {
    if (!data || size == 0 || size > 4u * 1024u * 1024u ||
        !out_content) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid content descriptor buffer");
    }
    JsonValue root;
    std::string error;
    JsonParser parser(static_cast<const char*>(data), size);
    if (!parser.Parse(&root, &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    std::unique_ptr<ContentObject> content(new ContentObject());
    if (!ParseContentRoot(mod, root, content.get(), &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        content.get(),
        DestroyObject<ContentObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    content.release();
    *out_content = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ContentParseJson(
    WotbModV3Handle mod,
    const WotbModV3ConstBuffer* json_utf8,
    WotbModV3Handle* out_content) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            json_utf8, json_utf8 ? json_utf8->struct_size : 0,
            sizeof(WotbModV3ConstBuffer))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid content buffer descriptor");
    }
    return ParseContentData(
        mod, json_utf8->data, json_utf8->size, out_content);
}

WotbModV3Result WOTBMOD_V3_CALL ContentParseUri(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_content) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::vector<uint8_t> bytes;
    WotbModV3Result read =
        ReadUri(mod, uri, 4u * 1024u * 1024u, &bytes);
    if (read != WOTBMOD_V3_OK) return read;
    return ParseContentData(
        mod, bytes.data(), bytes.size(), out_content);
}

WotbModV3Result GetContent(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    ContentObject** out) {
    return GetObject(
        mod, handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::Content, out);
}

WotbModV3Result WOTBMOD_V3_CALL ContentValidate(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ContentObject* content = nullptr;
    WotbModV3Result found = GetContent(mod, handle, &content);
    if (found != WOTBMOD_V3_OK) return found;
    if (content->overrides.empty() ||
        !IsAsciiIdentifier(
            content->id.c_str(), WOTBMOD_V3_MAX_ID, false)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "content object failed invariant validation");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ContentGetInfo(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    WotbModV3ContentInfo* out_info) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_info, out_info ? out_info->struct_size : 0,
            sizeof(WotbModV3ContentInfo))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid content info output");
    }
    ContentObject* content = nullptr;
    WotbModV3Result found = GetContent(mod, handle, &content);
    if (found != WOTBMOD_V3_OK) return found;
    out_info->override_count =
        static_cast<uint32_t>(content->overrides.size());
    CopyFixed(out_info->id, sizeof(out_info->id), content->id);
    CopyFixed(out_info->name, sizeof(out_info->name), content->name);
    CopyFixed(
        out_info->version, sizeof(out_info->version),
        content->version);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ContentGetOverride(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    WotbModV3ContentOverride* out_override) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_override,
            out_override ? out_override->struct_size : 0,
            sizeof(WotbModV3ContentOverride))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid content override output");
    }
    ContentObject* content = nullptr;
    WotbModV3Result found = GetContent(mod, handle, &content);
    if (found != WOTBMOD_V3_OK) return found;
    if (index >= content->overrides.size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "content override index is out of range");
    }
    *out_override = content->overrides[index];
    return WOTBMOD_V3_OK;
}

bool ContentKindSupportsFileOverlay(uint32_t kind) {
    return kind == WOTBMOD_V3_CONTENT_TEXTURE ||
           kind == WOTBMOD_V3_CONTENT_UI ||
           kind == WOTBMOD_V3_CONTENT_MODEL;
}

WotbModV3Result WOTBMOD_V3_CALL ContentApply(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsOverlayAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ContentObject* content = nullptr;
    WotbModV3Result found = GetContent(mod, handle, &content);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(content->mutex);
    if (content->applied) return WOTBMOD_V3_OK;

    std::vector<std::string> sources;
    sources.reserve(content->overrides.size());
    for (const WotbModV3ContentOverride& item :
         content->overrides) {
        if (!ContentKindSupportsFileOverlay(item.kind)) {
            return Fail(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "content apply supports native-intercepted UI, texture and model file overlays; semantic audio, hangar and localization overrides require dedicated native interceptors");
        }
        std::string source = "mod://self/";
        source += item.asset_path;
        if (source.size() >= WOTBMOD_V3_MAX_PATH) {
            return Fail(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "content asset URI exceeds the ABI path limit");
        }
        char resolved[WOTBMOD_V3_MAX_PATH] = {};
        uint32_t resolved_size = sizeof(resolved);
        const WotbModV3Result resolved_result =
            ResolveVfsUriPhysical(
                mod,
                source.c_str(),
                resolved,
                &resolved_size);
        if (resolved_result != WOTBMOD_V3_OK) {
            return resolved_result;
        }
        sources.push_back(std::move(source));
    }

    std::vector<WotbModV3Handle> created;
    created.reserve(content->overrides.size());
    for (size_t index = 0u;
         index < content->overrides.size(); ++index) {
        char provider[WOTBMOD_V3_MAX_ID] = {};
        const int written = std::snprintf(
            provider,
            sizeof(provider),
            "content/%u",
            static_cast<unsigned>(index));
        if (written <= 0 ||
            static_cast<size_t>(written) >= sizeof(provider)) {
            for (auto mounted = created.rbegin();
                 mounted != created.rend(); ++mounted) {
                VfsUnmount(mod, *mounted);
            }
            return Fail(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "content provider id exceeds the ABI limit");
        }
        WotbModV3Handle mount = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3ContentOverride& item =
            content->overrides[index];
        const WotbModV3Result mounted =
            VfsMountOverlay(
                mod,
                provider,
                item.target,
                sources[index].c_str(),
                item.priority,
                &mount);
        if (mounted != WOTBMOD_V3_OK) {
            for (auto previous = created.rbegin();
                 previous != created.rend(); ++previous) {
                VfsUnmount(mod, *previous);
            }
            return mounted;
        }
        created.push_back(mount);
    }
    content->mounts = std::move(created);
    content->applied = true;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ContentUnapply(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    WotbModV3Result access = ContentAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsOverlayAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ContentObject* content = nullptr;
    WotbModV3Result found = GetContent(mod, handle, &content);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(content->mutex);
    if (!content->applied) return WOTBMOD_V3_OK;
    WotbModV3Result first_error = WOTBMOD_V3_OK;
    for (auto item = content->mounts.rbegin();
         item != content->mounts.rend(); ++item) {
        const WotbModV3Result result =
            VfsUnmount(mod, *item);
        if (result != WOTBMOD_V3_OK &&
            result != WOTBMOD_V3_E_INVALID_HANDLE &&
            result != WOTBMOD_V3_E_OBJECT_DESTROYED &&
            first_error == WOTBMOD_V3_OK) {
            first_error = result;
        }
    }
    content->mounts.clear();
    content->applied = false;
    return first_error;
}

WotbModV3Result CatalogAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "catalog");
}

WotbModV3Result ValidateCatalogRecordInternal(
    WotbModV3Handle mod,
    const WotbModV3CatalogRecord* record) {
    if (!ValidStruct(
            record, record ? record->struct_size : 0,
            sizeof(WotbModV3CatalogRecord)) ||
        !FixedTerminated(record->id, sizeof(record->id)) ||
        !FixedTerminated(record->version, sizeof(record->version)) ||
        !FixedTerminated(record->sha256, sizeof(record->sha256)) ||
        !FixedTerminated(record->reason, sizeof(record->reason)) ||
        !FixedTerminated(record->signer_id, sizeof(record->signer_id)) ||
        record->status < WOTBMOD_V3_CATALOG_VERIFIED ||
        record->status > WOTBMOD_V3_CATALOG_BANNED ||
        !IsAsciiIdentifier(
            record->id, sizeof(record->id), false)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid catalog record");
    }
    if (!IsSemanticVersion(record->version)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "catalog version is not semantic");
    }
    if ((record->status == WOTBMOD_V3_CATALOG_VERIFIED ||
         record->status == WOTBMOD_V3_CATALOG_COMMUNITY ||
         record->status == WOTBMOD_V3_CATALOG_BANNED) &&
        !IsSha256(record->sha256)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "reviewed/banned catalog record requires SHA-256");
    }
    if (record->status == WOTBMOD_V3_CATALOG_VERIFIED &&
        record->signer_id[0] == '\0') {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "verified catalog record requires signer id");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CatalogValidateRecord(
    WotbModV3Handle mod,
    const WotbModV3CatalogRecord* record) {
    WotbModV3Result access = CatalogAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ValidateCatalogRecordInternal(mod, record);
}

WotbModV3Result WOTBMOD_V3_CALL CatalogEvaluateInstall(
    WotbModV3Handle mod,
    const WotbModV3CatalogRecord* record,
    const char* manifest_id,
    const char* manifest_version,
    const char* package_sha256,
    WotbModV3CatalogDecision* out_decision) {
    WotbModV3Result access = CatalogAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!manifest_id || !manifest_version || !package_sha256 ||
        !out_decision ||
        !ValidStruct(
            out_decision, out_decision->struct_size,
            sizeof(WotbModV3CatalogDecision)) ||
        !IsAsciiIdentifier(
            manifest_id, WOTBMOD_V3_MAX_ID, false) ||
        !IsSha256(package_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid catalog install evaluation request");
    }
    if (!IsSemanticVersion(manifest_version)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "manifest version is not semantic");
    }
    out_decision->status = WOTBMOD_V3_CATALOG_UNREVIEWED;
    out_decision->load_allowed = 1u;
    out_decision->warning_required = 1u;
    out_decision->hash_verified = 0u;
    CopyFixed(
        out_decision->reason,
        sizeof(out_decision->reason),
        "manual sideload: unreviewed");
    if (!record) {
        return WOTBMOD_V3_OK;
    }
    WotbModV3Result valid =
        ValidateCatalogRecordInternal(mod, record);
    if (valid != WOTBMOD_V3_OK) return valid;
    if (record->id != std::string(manifest_id) ||
        record->version != std::string(manifest_version)) {
        return Fail(mod, WOTBMOD_V3_E_CLIENT_MISMATCH,
                    "catalog record id/version does not match manifest");
    }
    const bool hash_matches =
        record->sha256[0] != '\0' &&
        LowerAscii(record->sha256) ==
            LowerAscii(package_sha256);
    out_decision->status = record->status;
    out_decision->hash_verified = hash_matches ? 1u : 0u;
    if (record->status == WOTBMOD_V3_CATALOG_BANNED) {
        out_decision->load_allowed = 0u;
        out_decision->warning_required = 1u;
        CopyFixed(
            out_decision->reason,
            sizeof(out_decision->reason),
            record->reason[0] != '\0'
                ? record->reason : "catalog blocklist");
        return WOTBMOD_V3_E_INCOMPATIBLE;
    }
    if (record->sha256[0] != '\0' && !hash_matches) {
        out_decision->load_allowed = 0u;
        out_decision->warning_required = 1u;
        CopyFixed(
            out_decision->reason,
            sizeof(out_decision->reason),
            "catalog package SHA-256 mismatch");
        return WOTBMOD_V3_E_HASH_MISMATCH;
    }
    out_decision->warning_required =
        record->status == WOTBMOD_V3_CATALOG_UNREVIEWED ? 1u : 0u;
    CopyFixed(
        out_decision->reason,
        sizeof(out_decision->reason),
        record->reason[0] != '\0'
            ? record->reason
            : (record->status == WOTBMOD_V3_CATALOG_VERIFIED
                ? "verified catalog package"
                : record->status == WOTBMOD_V3_CATALOG_COMMUNITY
                    ? "community reviewed package"
                    : "unreviewed package"));
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CatalogStatusName(
    WotbModV3Handle mod,
    uint32_t status,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = CatalogAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    const char* name = nullptr;
    switch (status) {
        case WOTBMOD_V3_CATALOG_VERIFIED:
            name = "verified";
            break;
        case WOTBMOD_V3_CATALOG_COMMUNITY:
            name = "community";
            break;
        case WOTBMOD_V3_CATALOG_UNREVIEWED:
            name = "unreviewed";
            break;
        case WOTBMOD_V3_CATALOG_BANNED:
            name = "banned";
            break;
        default:
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "unknown catalog status");
    }
    return CopyOutString(mod, name, buffer, inout_size);
}

}  // namespace

namespace {

struct SemVer {
    uint64_t major = 0;
    uint64_t minor = 0;
    uint64_t patch = 0;
    std::string prerelease;
};

bool ParseUnsigned(const std::string& text, uint64_t* out) {
    if (text.empty() || !out ||
        (text.size() > 1u && text[0] == '0')) {
        return false;
    }
    uint64_t value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() ||
        result.ptr != text.data() + text.size()) {
        return false;
    }
    *out = value;
    return true;
}

bool ParseSemVer(const std::string& text, SemVer* out) {
    if (!out || text.empty() ||
        text.size() >= WOTBMOD_V3_MAX_VERSION) {
        return false;
    }
    const size_t plus = text.find('+');
    const std::string without_build =
        plus == std::string::npos
        ? text : text.substr(0, plus);
    const size_t dash = without_build.find('-');
    const std::string core =
        dash == std::string::npos
        ? without_build : without_build.substr(0, dash);
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= core.size()) {
        const size_t dot = core.find('.', start);
        parts.push_back(core.substr(
            start,
            dot == std::string::npos
                ? core.size() - start : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1u;
    }
    if (parts.empty() || parts.size() > 3u) return false;
    SemVer version;
    if (!ParseUnsigned(parts[0], &version.major)) return false;
    if (parts.size() > 1u &&
        !ParseUnsigned(parts[1], &version.minor)) return false;
    if (parts.size() > 2u &&
        !ParseUnsigned(parts[2], &version.patch)) return false;
    if (dash != std::string::npos) {
        version.prerelease = without_build.substr(dash + 1u);
        if (version.prerelease.empty()) return false;
        for (unsigned char c : version.prerelease) {
            if (!(std::isalnum(c) || c == '.' || c == '-')) {
                return false;
            }
        }
    }
    *out = std::move(version);
    return true;
}

bool IsSemanticVersion(const std::string& value) {
    SemVer parsed;
    return ParseSemVer(value, &parsed);
}

int CompareSemVer(const SemVer& left, const SemVer& right) {
    if (left.major != right.major)
        return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor)
        return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch)
        return left.patch < right.patch ? -1 : 1;
    if (left.prerelease.empty() != right.prerelease.empty()) {
        return left.prerelease.empty() ? 1 : -1;
    }
    if (left.prerelease == right.prerelease) return 0;
    return left.prerelease < right.prerelease ? -1 : 1;
}

bool MatchComparator(
    const SemVer& version,
    const std::string& comparator) {
    std::string operation;
    std::string text = comparator;
    if (text.rfind(">=", 0) == 0 ||
        text.rfind("<=", 0) == 0) {
        operation = text.substr(0, 2u);
        text = Trim(text.substr(2u));
    } else if (!text.empty() &&
               (text[0] == '>' || text[0] == '<' ||
                text[0] == '=')) {
        operation = text.substr(0, 1u);
        text = Trim(text.substr(1u));
    } else {
        operation = "=";
    }
    SemVer expected;
    if (!ParseSemVer(text, &expected)) return false;
    const int comparison = CompareSemVer(version, expected);
    if (operation == ">=") return comparison >= 0;
    if (operation == "<=") return comparison <= 0;
    if (operation == ">") return comparison > 0;
    if (operation == "<") return comparison < 0;
    return comparison == 0;
}

bool VersionMatchesRange(
    const std::string& version_text,
    const std::string& range_text) {
    SemVer version;
    if (!ParseSemVer(version_text, &version)) return false;
    const std::string range = Trim(range_text);
    if (range == "*" || range.empty()) return true;
    if (range[0] == '^' || range[0] == '~') {
        SemVer lower;
        if (!ParseSemVer(Trim(range.substr(1u)), &lower) ||
            CompareSemVer(version, lower) < 0) {
            return false;
        }
        SemVer upper = lower;
        if (range[0] == '^') {
            if (lower.major != 0) {
                ++upper.major;
                upper.minor = 0;
                upper.patch = 0;
            } else {
                ++upper.minor;
                upper.patch = 0;
            }
        } else {
            ++upper.minor;
            upper.patch = 0;
        }
        upper.prerelease.clear();
        return CompareSemVer(version, upper) < 0;
    }
    std::string normalized = range;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream stream(normalized);
    std::string comparator;
    bool any = false;
    while (stream >> comparator) {
        any = true;
        if (!MatchComparator(version, comparator)) {
            return false;
        }
    }
    return any;
}

bool ValidVersionRange(const std::string& range) {
    const std::string normalized_range = Trim(range);
    if (normalized_range.empty()) {
        return false;
    }
    if (normalized_range == "*") {
        return true;
    }
    if (normalized_range[0] == '^' ||
        normalized_range[0] == '~') {
        SemVer version;
        return ParseSemVer(
            Trim(normalized_range.substr(1u)), &version);
    }
    std::string normalized = normalized_range;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream stream(normalized);
    std::string comparator;
    bool any = false;
    while (stream >> comparator) {
        any = true;
        if (comparator.rfind(">=", 0) == 0 ||
            comparator.rfind("<=", 0) == 0) {
            comparator = comparator.substr(2u);
        } else if (!comparator.empty() &&
                   (comparator[0] == '>' ||
                    comparator[0] == '<' ||
                    comparator[0] == '=')) {
            comparator = comparator.substr(1u);
        }
        SemVer version;
        if (!ParseSemVer(Trim(comparator), &version)) {
            return false;
        }
    }
    return any;
}

bool SafeManifestPattern(const std::string& value) {
    if (value.empty() ||
        value.size() >= WOTBMOD_V3_MAX_PATH ||
        value.find('\\') != std::string::npos ||
        value.find('%') != std::string::npos ||
        fs::path(value).is_absolute()) {
        return false;
    }
    std::istringstream stream(value);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == "." ||
            segment == "..") {
            return false;
        }
        for (unsigned char c : segment) {
            if (c < 0x20u || c == 0x7fu ||
                c == ':' || c == '?' || c == '#') {
                return false;
            }
        }
    }
    return true;
}

bool SafeManifestPath(const std::string& value) {
    return SafeManifestPattern(value) &&
           value.find_first_of("*?[]{}") == std::string::npos;
}

bool SafeClientBuild(const std::string& value) {
    if (value.empty() || value.size() >= WOTBMOD_V3_MAX_NAME) {
        return false;
    }
    for (unsigned char c : value) {
        const bool accepted =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_' || c == '+';
        if (!accepted) {
            return false;
        }
    }
    return true;
}

struct ManifestObject final : TaggedObject {
    ManifestObject()
        : TaggedObject(ObjectKind::Manifest) {}
    uint32_t manifest_version = 0;
    uint32_t package_type = WOTBMOD_V3_PACKAGE_NATIVE;
    std::string id;
    std::string name;
    std::string version;
    std::string developer;
    std::string settings_path;
    std::string content_descriptor_path;
    std::vector<WotbModV3ManifestApiRequirement> api_requirements;
    std::vector<WotbModV3ManifestDependency> dependencies;
    std::vector<std::string> permissions;
    std::vector<std::string> resources;
    std::vector<std::string> locales;
    std::vector<WotbModV3ManifestEntrypoint> entrypoints;
    std::vector<std::string> client_builds;
    std::vector<std::string> client_executable_hashes;
    std::string signature_algorithm;
    std::string signature_key_id;
    std::string signature_value;
};

bool CopyJsonObjectStrings(
    const JsonValue* object,
    uint32_t kind,
    std::vector<WotbModV3ManifestDependency>* output,
    std::string* error) {
    if (!object) return true;
    if (object->type != JsonValue::Type::Object ||
        object->object.size() >
            WOTBMOD_V3_PREFLIGHT_MAX_DEPENDENCIES) {
        if (error) *error = "dependency section must be an object";
        return false;
    }
    for (const auto& item : object->object) {
        if (!IsAsciiIdentifier(
                item.first.c_str(), WOTBMOD_V3_MAX_ID, false) ||
            item.second.type != JsonValue::Type::String ||
            item.second.text.size() >= WOTBMOD_V3_MAX_VERSION ||
            !ValidVersionRange(item.second.text)) {
            if (error) *error = "invalid dependency id or version range";
            return false;
        }
        WotbModV3ManifestDependency dependency = {};
        dependency.struct_size = sizeof(dependency);
        dependency.api_version = WOTBMOD_V3_MANIFEST_VERSION;
        dependency.kind = kind;
        CopyFixed(
            dependency.id, sizeof(dependency.id), item.first);
        CopyFixed(
            dependency.version_range,
            sizeof(dependency.version_range),
            item.second.text);
        output->push_back(dependency);
    }
    return true;
}

bool ParseApiRequirements(
    const JsonValue* object,
    std::vector<WotbModV3ManifestApiRequirement>* output,
    std::string* error) {
    if (!object) return true;
    if (!output || object->type != JsonValue::Type::Object ||
        object->object.size() > 256u) {
        if (error) *error = "manifest api section is invalid";
        return false;
    }
    for (const auto& item : object->object) {
        if (!IsAsciiIdentifier(
                item.first.c_str(),
                WOTBMOD_V3_MAX_INTERFACE_NAME,
                false) ||
            item.second.type != JsonValue::Type::String ||
            item.second.text.size() >= WOTBMOD_V3_MAX_VERSION ||
            !ValidVersionRange(item.second.text)) {
            if (error) {
                *error =
                    "manifest API name or version range is invalid";
            }
            return false;
        }
        WotbModV3ManifestApiRequirement requirement = {};
        requirement.struct_size = sizeof(requirement);
        requirement.api_version = WOTBMOD_V3_MANIFEST_VERSION;
        CopyFixed(
            requirement.interface_name,
            sizeof(requirement.interface_name),
            item.first);
        CopyFixed(
            requirement.version_range,
            sizeof(requirement.version_range),
            item.second.text);
        output->push_back(requirement);
    }
    return true;
}

bool ParseStringArray(
    const JsonValue* value,
    size_t max_items,
    size_t max_length,
    std::vector<std::string>* output,
    std::string* error) {
    if (!value) return true;
    if (value->type != JsonValue::Type::Array ||
        value->array.size() > max_items) {
        if (error) *error = "manifest array has invalid type or size";
        return false;
    }
    std::set<std::string> unique;
    for (const JsonValue& item : value->array) {
        if (item.type != JsonValue::Type::String ||
            item.text.empty() || item.text.size() >= max_length ||
            !unique.insert(item.text).second) {
            if (error) *error = "manifest array contains invalid/duplicate text";
            return false;
        }
        output->push_back(item.text);
    }
    return true;
}

bool ParseStringOrArray(
    const JsonValue* value,
    size_t max_items,
    size_t max_length,
    std::vector<std::string>* output,
    std::string* error) {
    if (!value) return true;
    if (value->type == JsonValue::Type::String) {
        if (value->text.empty() ||
            value->text.size() >= max_length) {
            if (error) *error = "manifest text value is invalid";
            return false;
        }
        output->push_back(value->text);
        return true;
    }
    return ParseStringArray(
        value, max_items, max_length, output, error);
}

bool ValidNetworkPermission(const std::string& permission) {
    static const std::string prefix = "network:https://";
    if (permission.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string host = permission.substr(prefix.size());
    if (host.empty() ||
        host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string::npos) {
        return false;
    }
    for (unsigned char c : host) {
        const bool accepted =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == ':';
        if (!accepted) {
            return false;
        }
    }
    return true;
}

uint32_t ManifestPermissionTier(const std::string& permission) {
    if (permission == "native.memory" ||
        permission == "native.memory_patch" ||
        permission == "native.hook.address" ||
        permission == "native.hooks" ||
        permission == "render.native" ||
        permission == "bigworld.rpc.modify") {
        return WOTBMOD_V3_PERMISSION_UNSAFE;
    }
    if (permission.rfind("gameplay.tweak", 0) == 0) {
        return WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK;
    }
    if (ValidNetworkPermission(permission) ||
        permission == "battle.ui" ||
        permission == "battle.render.overlay" ||
        permission == "camera.battle.read" ||
        permission == "visible.projectile.events" ||
        permission == "game.entity.public" ||
        permission == "ui.modify.game" ||
        permission == "resources.overlay.game" ||
        permission == "resources.write.mod_data" ||
        permission == "hooks.symbol" ||
        permission == "render.callbacks" ||
        permission == "bigworld.observe" ||
        permission == "bigworld.rpc.observe" ||
        permission == "bigworld.rpc.metadata" ||
        permission == "client.leave_to_hangar" ||
        permission == "network.http") {
        return WOTBMOD_V3_PERMISSION_REVIEWED;
    }
    static const std::set<std::string> safe_permissions = {
        "core",
        "ui",
        "ui.create",
        "ui.modify.own",
        "localization",
        "audio",
        "audio.custom",
        "audio.events",
        "resources",
        "resources.mod",
        "filesystem.mod_data",
        "input",
        "input.actions",
        "settings",
        "storage",
        "events.public",
        "entity.public.visible",
        "hangar.scene",
        "vehicle.local.cosmetic",
        "camera.hangar",
        "camera.replay",
        "network.http.allowlisted",
        "content"
    };
    if (safe_permissions.find(permission) !=
        safe_permissions.end()) {
        return WOTBMOD_V3_PERMISSION_SAFE;
    }
    return WOTBMOD_V3_PERMISSION_UNSAFE;
}

bool RegisteredManifestPermission(const std::string& permission) {
    if (ValidNetworkPermission(permission)) return true;
    static const std::set<std::string> permissions = {
        "core", "ui", "ui.create", "ui.modify.own", "localization",
        "audio", "audio.custom", "audio.events", "resources",
        "resources.mod", "filesystem.mod_data", "input", "input.actions",
        "content", "settings", "storage", "events.public",
        "entity.public.visible", "hangar.scene", "vehicle.local.cosmetic",
        "camera.hangar", "camera.replay", "network.http.allowlisted",
        "gameplay.tweak.camera", "gameplay.tweak.hud",
        "gameplay.tweak.hangar", "gameplay.tweak.replay",
        "gameplay.tweak.cosmetic", "gameplay.tweak.vehicle",
        "gameplay.tweak.projectile_visual", "gameplay.tweak.freecam",
        "ui.modify.game", "battle.ui", "resources.overlay.game",
        "resources.write.mod_data",
        "hooks.symbol", "render.callbacks", "battle.render.overlay",
        "camera.battle.read", "visible.projectile.events",
        "game.entity.public", "bigworld.observe", "bigworld.rpc.observe",
        "bigworld.rpc.metadata", "client.leave_to_hangar", "network.http",
        "native.memory", "native.memory_patch", "native.hook.address",
        "native.hooks", "render.native", "bigworld.rpc.modify",
        "ges.observe", "ges.publish",
        "session.cluster.read", "session.cluster.change",
        "packages.manage"
    };
    return permissions.find(permission) != permissions.end();
}

bool ParseManifestRoot(
    const JsonValue& root,
    ManifestObject* manifest,
    std::string* error) {
    if (!manifest || root.type != JsonValue::Type::Object) {
        if (error) *error = "manifest root must be an object";
        return false;
    }
    const JsonValue* manifest_version =
        JsonField(root, "manifest_version");
    if (!manifest_version ||
        manifest_version->type != JsonValue::Type::Number ||
        manifest_version->number !=
            std::floor(manifest_version->number) ||
        manifest_version->number != 1.0) {
        if (error) *error = "manifest_version must equal 1";
        return false;
    }
    manifest->manifest_version = 1u;
    if (!JsonString(root, "id", &manifest->id) ||
        !JsonString(root, "name", &manifest->name) ||
        !JsonString(root, "version", &manifest->version) ||
        !JsonString(root, "developer", &manifest->developer)) {
        if (error) *error = "manifest identity fields are missing or invalid";
        return false;
    }
    if (!IsAsciiIdentifier(
            manifest->id.c_str(), WOTBMOD_V3_MAX_ID, false) ||
        manifest->name.empty() ||
        manifest->name.size() >= WOTBMOD_V3_MAX_NAME ||
        manifest->developer.empty() ||
        manifest->developer.size() >= WOTBMOD_V3_MAX_NAME ||
        !IsSemanticVersion(manifest->version)) {
        if (error) *error = "manifest id/name/version/developer is invalid";
        return false;
    }
    std::string type;
    if (!JsonString(root, "type", &type, false)) {
        if (error) *error = "manifest type must be text";
        return false;
    }
    if (!type.empty()) {
        if (type == "content") {
            manifest->package_type =
                WOTBMOD_V3_PACKAGE_CONTENT_ONLY;
        } else if (type == "native") {
            manifest->package_type =
                WOTBMOD_V3_PACKAGE_NATIVE;
        } else {
            if (error) *error = "manifest type must be native or content";
            return false;
        }
    }
    if (!JsonString(
            root, "settings", &manifest->settings_path, false) ||
        (!manifest->settings_path.empty() &&
         !SafeManifestPath(manifest->settings_path))) {
        if (error) *error = "manifest settings path is unsafe";
        return false;
    }
    if (!JsonString(
            root, "content",
            &manifest->content_descriptor_path, false) ||
        (!manifest->content_descriptor_path.empty() &&
         !SafeManifestPath(
             manifest->content_descriptor_path))) {
        if (error) {
            *error = "manifest content descriptor path is unsafe";
        }
        return false;
    }
    if (manifest->package_type ==
        WOTBMOD_V3_PACKAGE_CONTENT_ONLY) {
        if (manifest->content_descriptor_path.empty()) {
            manifest->content_descriptor_path = "content.json";
        }
    } else if (!manifest->content_descriptor_path.empty()) {
        if (error) {
            *error =
                "native manifest cannot declare a content descriptor";
        }
        return false;
    }
    if (!ParseApiRequirements(
            JsonField(root, "api"),
            &manifest->api_requirements, error)) {
        return false;
    }
    if (!CopyJsonObjectStrings(
            JsonField(root, "dependencies"),
            WOTBMOD_V3_DEPENDENCY_REQUIRED,
            &manifest->dependencies, error) ||
        !CopyJsonObjectStrings(
            JsonField(root, "optional_dependencies"),
            WOTBMOD_V3_DEPENDENCY_OPTIONAL,
            &manifest->dependencies, error) ||
        !CopyJsonObjectStrings(
            JsonField(root, "incompatibilities"),
            WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE,
            &manifest->dependencies, error)) {
        return false;
    }
    if (manifest->dependencies.size() >
        WOTBMOD_V3_PREFLIGHT_MAX_DEPENDENCIES) {
        if (error) {
            *error = "manifest dependency limit exceeded";
        }
        return false;
    }
    std::set<std::string> dependency_ids;
    for (const auto& dependency : manifest->dependencies) {
        if (!dependency_ids.insert(dependency.id).second) {
            if (error) {
                *error =
                    "dependency id appears in multiple manifest sections";
            }
            return false;
        }
    }
    if (!ParseStringArray(
            JsonField(root, "permissions"),
            WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS,
            WOTBMOD_V3_MAX_PERMISSION_NAME,
            &manifest->permissions, error)) {
        return false;
    }
    std::set<std::string> unique_permissions;
    for (const std::string& permission : manifest->permissions) {
        if (!IsAsciiIdentifier(
                permission.c_str(),
                WOTBMOD_V3_MAX_PERMISSION_NAME,
                true) &&
            !ValidNetworkPermission(permission)) {
            if (error) *error = "manifest permission name is invalid";
            return false;
        }
        std::string normalized(permission);
        std::transform(
            normalized.begin(),
            normalized.end(),
            normalized.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (!RegisteredManifestPermission(normalized)) {
            if (error) {
                *error = "manifest permission is not registered: " +
                    permission;
            }
            return false;
        }
        if (!unique_permissions.insert(normalized).second) {
            if (error) {
                *error =
                    "manifest permission name appears more than once";
            }
            return false;
        }
    }
    if (!ParseStringArray(
            JsonField(root, "resources"),
            4096u, WOTBMOD_V3_MAX_PATH,
            &manifest->resources, error)) {
        return false;
    }
    for (const std::string& pattern : manifest->resources) {
        if (!SafeManifestPattern(pattern)) {
            if (error) *error = "manifest resource pattern is unsafe";
            return false;
        }
    }
    if (!ParseStringOrArray(
            JsonField(root, "locales"),
            4096u, WOTBMOD_V3_MAX_PATH,
            &manifest->locales, error)) {
        return false;
    }
    {
        std::set<std::string> locale_patterns;
        for (const std::string& pattern : manifest->locales) {
            if (!SafeManifestPattern(pattern) ||
                !locale_patterns.insert(pattern).second) {
                if (error) {
                    *error =
                        "manifest locale pattern is unsafe or duplicated";
                }
                return false;
            }
        }
    }
    const JsonValue* client = JsonField(root, "client");
    if (client) {
        if (client->type != JsonValue::Type::Object ||
            !ParseStringArray(
                JsonField(*client, "builds"),
                WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_BUILDS,
                WOTBMOD_V3_MAX_NAME,
                &manifest->client_builds, error) ||
            !ParseStringArray(
                JsonField(*client, "executable_hashes"),
                WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_HASHES,
                65u,
                &manifest->client_executable_hashes, error)) {
            if (error && error->empty()) {
                *error = "manifest client section is invalid";
            }
            return false;
        }
        for (const std::string& build :
             manifest->client_builds) {
            if (!SafeClientBuild(build)) {
                if (error) *error = "manifest client build is invalid";
                return false;
            }
        }
        for (std::string& hash :
             manifest->client_executable_hashes) {
            if (!IsSha256(hash)) {
                if (error) {
                    *error =
                        "manifest client executable hash is invalid";
                }
                return false;
            }
            hash = LowerAscii(hash);
        }
    }
    const JsonValue* entrypoints =
        JsonField(root, "entrypoints");
    if (entrypoints) {
        if (entrypoints->type != JsonValue::Type::Object ||
            entrypoints->object.size() > 32u) {
            if (error) *error = "manifest entrypoints section is invalid";
            return false;
        }
        for (const auto& item : entrypoints->object) {
            if (!IsAsciiIdentifier(
                    item.first.c_str(), WOTBMOD_V3_MAX_ID, false) ||
                item.second.type != JsonValue::Type::String ||
                !SafeManifestPath(item.second.text)) {
                if (error) *error = "manifest entrypoint is invalid";
                return false;
            }
            WotbModV3ManifestEntrypoint entrypoint = {};
            entrypoint.struct_size = sizeof(entrypoint);
            entrypoint.api_version = WOTBMOD_V3_MANIFEST_VERSION;
            CopyFixed(
                entrypoint.platform,
                sizeof(entrypoint.platform),
                item.first);
            CopyFixed(
                entrypoint.relative_path,
                sizeof(entrypoint.relative_path),
                item.second.text);
            manifest->entrypoints.push_back(entrypoint);
        }
    }
    if (manifest->package_type == WOTBMOD_V3_PACKAGE_NATIVE &&
        manifest->entrypoints.empty()) {
        if (error) *error = "native manifest requires at least one entrypoint";
        return false;
    }
    if (manifest->package_type ==
            WOTBMOD_V3_PACKAGE_CONTENT_ONLY &&
        !manifest->entrypoints.empty()) {
        if (error) *error = "content-only manifest cannot contain native entrypoints";
        return false;
    }
    const JsonValue* signature = JsonField(root, "signature");
    if (signature) {
        if (signature->type != JsonValue::Type::Object ||
            !JsonString(
                *signature, "algorithm",
                &manifest->signature_algorithm) ||
            !JsonString(
                *signature, "key_id",
                &manifest->signature_key_id) ||
            !JsonString(
                *signature, "value",
                &manifest->signature_value) ||
            manifest->signature_algorithm.empty() ||
            manifest->signature_key_id.empty() ||
            manifest->signature_value.empty() ||
            manifest->signature_value.size() > 4096u) {
            if (error) *error = "manifest signature metadata is invalid";
            return false;
        }
    }
    return true;
}

WotbModV3Result ManifestAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "manifest");
}

WotbModV3Result ParseManifestData(
    WotbModV3Handle mod,
    const void* data,
    size_t size,
    WotbModV3Handle* out_manifest) {
    if (!data || size == 0 || size > 4u * 1024u * 1024u ||
        !out_manifest) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid manifest JSON buffer");
    }
    JsonValue root;
    std::string error;
    JsonParser parser(static_cast<const char*>(data), size);
    if (!parser.Parse(&root, &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    std::unique_ptr<ManifestObject> manifest(new ManifestObject());
    if (!ParseManifestRoot(root, manifest.get(), &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        manifest.get(),
        DestroyObject<ManifestObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    manifest.release();
    *out_manifest = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestParseJson(
    WotbModV3Handle mod,
    const WotbModV3ConstBuffer* json_utf8,
    WotbModV3Handle* out_manifest) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            json_utf8, json_utf8 ? json_utf8->struct_size : 0,
            sizeof(WotbModV3ConstBuffer))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid manifest buffer descriptor");
    }
    return ParseManifestData(
        mod, json_utf8->data, json_utf8->size, out_manifest);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestParseUri(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_manifest) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::vector<uint8_t> bytes;
    WotbModV3Result read =
        ReadUri(mod, uri, 4u * 1024u * 1024u, &bytes);
    if (read != WOTBMOD_V3_OK) return read;
    return ParseManifestData(
        mod, bytes.data(), bytes.size(), out_manifest);
}

WotbModV3Result GetManifest(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    ManifestObject** out) {
    return GetObject(
        mod, handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::Manifest, out);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestValidate(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    if (manifest->manifest_version != 1u ||
        !IsAsciiIdentifier(
            manifest->id.c_str(), WOTBMOD_V3_MAX_ID, false)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "manifest object failed invariant validation");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetInfo(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    WotbModV3ManifestInfo* out_info) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_info, out_info ? out_info->struct_size : 0,
            sizeof(WotbModV3ManifestInfo))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid manifest info output");
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    out_info->manifest_version = manifest->manifest_version;
    out_info->package_type = manifest->package_type;
    out_info->api_requirement_count =
        static_cast<uint32_t>(
            manifest->api_requirements.size());
    out_info->dependency_count =
        static_cast<uint32_t>(manifest->dependencies.size());
    out_info->permission_count =
        static_cast<uint32_t>(manifest->permissions.size());
    out_info->resource_count =
        static_cast<uint32_t>(manifest->resources.size());
    out_info->locale_count =
        static_cast<uint32_t>(manifest->locales.size());
    out_info->entrypoint_count =
        static_cast<uint32_t>(manifest->entrypoints.size());
    out_info->client_build_count =
        static_cast<uint32_t>(
            manifest->client_builds.size());
    out_info->client_executable_hash_count =
        static_cast<uint32_t>(
            manifest->client_executable_hashes.size());
    out_info->signature_status =
        manifest->signature_algorithm.empty()
        ? WOTBMOD_V3_SIGNATURE_UNSIGNED
        : WOTBMOD_V3_SIGNATURE_DECLARED;
    out_info->reserved = 0u;
    if (!CopyFixed(out_info->id, sizeof(out_info->id), manifest->id) ||
        !CopyFixed(
            out_info->name, sizeof(out_info->name), manifest->name) ||
        !CopyFixed(
            out_info->version,
            sizeof(out_info->version),
            manifest->version) ||
        !CopyFixed(
            out_info->developer,
            sizeof(out_info->developer),
            manifest->developer) ||
        !CopyFixed(
            out_info->settings_path,
            sizeof(out_info->settings_path),
            manifest->settings_path) ||
        !CopyFixed(
            out_info->content_descriptor_path,
            sizeof(out_info->content_descriptor_path),
            manifest->content_descriptor_path)) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "manifest info exceeds ABI text limits");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetApiRequirement(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    WotbModV3ManifestApiRequirement* out_requirement) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_requirement,
            out_requirement ? out_requirement->struct_size : 0,
            sizeof(WotbModV3ManifestApiRequirement))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid API requirement output");
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found =
        GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    if (index >= manifest->api_requirements.size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "manifest API requirement index is out of range");
    }
    *out_requirement = manifest->api_requirements[index];
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetDependency(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    WotbModV3ManifestDependency* out_dependency) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_dependency,
            out_dependency ? out_dependency->struct_size : 0,
            sizeof(WotbModV3ManifestDependency))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid dependency output");
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    if (index >= manifest->dependencies.size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "manifest dependency index is out of range");
    }
    *out_dependency = manifest->dependencies[index];
    return WOTBMOD_V3_OK;
}

enum ManifestStringList : uint32_t {
    MANIFEST_STRING_PERMISSION = 1,
    MANIFEST_STRING_RESOURCE = 2,
    MANIFEST_STRING_LOCALE = 3,
    MANIFEST_STRING_CLIENT_BUILD = 4,
    MANIFEST_STRING_CLIENT_HASH = 5
};

WotbModV3Result ManifestGetIndexedString(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    uint32_t list,
    char* buffer,
    uint32_t* inout_size) {
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    const std::vector<std::string>* values = nullptr;
    switch (list) {
        case MANIFEST_STRING_PERMISSION:
            values = &manifest->permissions;
            break;
        case MANIFEST_STRING_RESOURCE:
            values = &manifest->resources;
            break;
        case MANIFEST_STRING_LOCALE:
            values = &manifest->locales;
            break;
        case MANIFEST_STRING_CLIENT_BUILD:
            values = &manifest->client_builds;
            break;
        case MANIFEST_STRING_CLIENT_HASH:
            values = &manifest->client_executable_hashes;
            break;
        default:
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "manifest string list is invalid");
    }
    if (index >= values->size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "manifest string index is out of range");
    }
    return CopyOutString(
        mod, (*values)[index], buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetPermission(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ManifestGetIndexedString(
        mod, handle, index, MANIFEST_STRING_PERMISSION,
        buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetResourcePattern(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ManifestGetIndexedString(
        mod, handle, index, MANIFEST_STRING_RESOURCE,
        buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetLocalePattern(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ManifestGetIndexedString(
        mod, handle, index, MANIFEST_STRING_LOCALE,
        buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetEntrypoint(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    WotbModV3ManifestEntrypoint* out_entrypoint) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_entrypoint,
            out_entrypoint ? out_entrypoint->struct_size : 0,
            sizeof(WotbModV3ManifestEntrypoint))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid entrypoint output");
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    if (index >= manifest->entrypoints.size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "manifest entrypoint index is out of range");
    }
    *out_entrypoint = manifest->entrypoints[index];
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestGetClientBuild(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ManifestGetIndexedString(
        mod, handle, index, MANIFEST_STRING_CLIENT_BUILD,
        buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL
ManifestGetClientExecutableHash(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t index,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return ManifestGetIndexedString(
        mod, handle, index, MANIFEST_STRING_CLIENT_HASH,
        buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL ManifestResolveDependencies(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    const WotbModV3InstalledMod* installed,
    uint32_t installed_count,
    WotbModV3DependencyReport* out_report) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_report,
            out_report ? out_report->struct_size : 0,
            sizeof(WotbModV3DependencyReport)) ||
        installed_count > 4096u ||
        (installed_count != 0 && !installed)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid dependency resolution request");
    }
    std::map<std::string, std::string> active;
    for (uint32_t i = 0; i < installed_count; ++i) {
        if (!ValidStruct(
                &installed[i], installed[i].struct_size,
                sizeof(WotbModV3InstalledMod)) ||
            !FixedTerminated(
                installed[i].id, sizeof(installed[i].id)) ||
            !FixedTerminated(
                installed[i].version,
                sizeof(installed[i].version)) ||
            !IsAsciiIdentifier(
                installed[i].id,
                sizeof(installed[i].id), false) ||
            !IsSemanticVersion(installed[i].version)) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid installed mod descriptor");
        }
        if (installed[i].enabled) {
            if (!active.emplace(
                    installed[i].id,
                    installed[i].version).second) {
                return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                            "installed mod list contains duplicate ids");
            }
        }
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    std::memset(
        reinterpret_cast<char*>(out_report) +
            offsetof(WotbModV3DependencyReport, missing_required),
        0,
        sizeof(*out_report) -
            offsetof(WotbModV3DependencyReport, missing_required));
    std::string first_problem;
    for (const auto& dependency : manifest->dependencies) {
        auto candidate = active.find(dependency.id);
        if (dependency.kind ==
            WOTBMOD_V3_DEPENDENCY_REQUIRED) {
            if (candidate == active.end()) {
                ++out_report->missing_required;
                if (first_problem.empty())
                    first_problem =
                        std::string("missing required dependency ") +
                        dependency.id;
            } else if (!VersionMatchesRange(
                           candidate->second,
                           dependency.version_range)) {
                ++out_report->version_mismatch;
                if (first_problem.empty())
                    first_problem =
                        std::string("dependency version mismatch for ") +
                        dependency.id;
            }
        } else if (dependency.kind ==
                   WOTBMOD_V3_DEPENDENCY_OPTIONAL) {
            if (candidate == active.end()) {
                ++out_report->optional_missing;
            } else if (!VersionMatchesRange(
                           candidate->second,
                           dependency.version_range)) {
                ++out_report->version_mismatch;
                if (first_problem.empty())
                    first_problem =
                        std::string("optional dependency version mismatch for ") +
                        dependency.id;
            }
        } else if (candidate != active.end() &&
                   VersionMatchesRange(
                       candidate->second,
                       dependency.version_range)) {
            ++out_report->incompatibility_count;
            if (first_problem.empty())
                first_problem =
                    std::string("incompatible mod installed: ") +
                    dependency.id;
        }
    }
    CopyFixed(
        out_report->message,
        sizeof(out_report->message),
        first_problem.empty()
            ? "dependencies satisfied" : first_problem);
    if (out_report->incompatibility_count != 0) {
        return Fail(mod, WOTBMOD_V3_E_INCOMPATIBLE,
                    out_report->message);
    }
    if (out_report->missing_required != 0 ||
        out_report->version_mismatch != 0) {
        return Fail(mod, WOTBMOD_V3_E_DEPENDENCY_MISSING,
                    out_report->message);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestVerifyContentSha256(
    WotbModV3Handle mod,
    const char* physical_file,
    const char* expected_sha256) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!physical_file || !expected_sha256 ||
        !IsSha256(expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid package hash verification request");
    }
    fs::path canonical;
    if (!IsAllowedOwnedPhysicalPath(
            mod, fs::path(physical_file), true, &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "package file is outside owned roots");
    }
    std::string actual;
    WotbModV3Result hashed =
        Sha256File(mod, canonical, &actual);
    if (hashed != WOTBMOD_V3_OK) return hashed;
    if (actual != LowerAscii(expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "package SHA-256 mismatch");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ManifestVerifySignature(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    const char* physical_package) {
    WotbModV3Result access = ManifestAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!physical_package) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "signature package path is null");
    }
    ManifestObject* manifest = nullptr;
    WotbModV3Result found = GetManifest(mod, handle, &manifest);
    if (found != WOTBMOD_V3_OK) return found;
    if (manifest->signature_algorithm.empty()) {
        return Fail(mod, WOTBMOD_V3_E_SIGNATURE_INVALID,
                    "manifest is unsigned");
    }
    fs::path canonical;
    if (!IsAllowedOwnedPhysicalPath(
            mod, fs::path(physical_package), true, &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "signed package is outside owned roots");
    }
    if (LowerAscii(manifest->signature_algorithm) !=
        "ecdsa-p256-sha256") {
        return Fail(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "manifest signature algorithm is not ecdsa-p256-sha256");
    }
    std::string digest;
    const WotbModV3Result hashed =
        Sha256File(mod, canonical, &digest);
    if (hashed != WOTBMOD_V3_OK) return hashed;
    std::string verification_error;
    const PackageTrustResult verified = VerifyTrustedP256Sha256(
        fs::u8path(ModsDirectory() ? ModsDirectory() : "") /
            "trust" / "keys",
        manifest->signature_key_id,
        digest,
        manifest->signature_value,
        &verification_error);
    if (verified == PackageTrustResult::Valid) {
        return WOTBMOD_V3_OK;
    }
    if (verified == PackageTrustResult::Untrusted) {
        return Fail(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            verification_error.c_str());
    }
    if (verified == PackageTrustResult::IoError) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    verification_error.c_str());
    }
    return Fail(mod, WOTBMOD_V3_E_SIGNATURE_INVALID,
                verification_error.c_str());
}

bool InitializePreflightSummary(
    DataManifestPreflightSummary* summary) {
    if (!summary ||
        summary->struct_size <
            sizeof(DataManifestPreflightSummary)) {
        return false;
    }
    std::memset(summary, 0, sizeof(*summary));
    summary->struct_size = sizeof(*summary);
    summary->api_version =
        WOTBMOD_V3_DATA_PREFLIGHT_VERSION;
    return true;
}

WotbModV3Result PreflightFailure(
    DataManifestPreflightSummary* summary,
    WotbModV3Result result,
    const std::string& message) {
    if (summary) {
        CopyFixed(
            summary->error,
            sizeof(summary->error),
            message.size() < sizeof(summary->error)
                ? message
                : message.substr(0, sizeof(summary->error) - 1u));
    }
    return result;
}

}  // namespace

WotbModV3Result PreflightManifestUtf8(
    const void* json_utf8,
    size_t json_size,
    DataManifestPreflightSummary* out_summary) {
    if (!InitializePreflightSummary(out_summary)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!json_utf8 || json_size == 0) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "manifest preflight JSON is empty");
    }
    if (json_size > 4u * 1024u * 1024u) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "manifest preflight JSON exceeds 4 MiB");
    }
    JsonValue root;
    std::string error;
    JsonParser parser(
        static_cast<const char*>(json_utf8), json_size);
    if (!parser.Parse(&root, &error)) {
        return PreflightFailure(
            out_summary, WOTBMOD_V3_E_PARSE, error);
    }
    ManifestObject manifest;
    if (!ParseManifestRoot(root, &manifest, &error)) {
        return PreflightFailure(
            out_summary, WOTBMOD_V3_E_PARSE, error);
    }
    out_summary->manifest_version =
        manifest.manifest_version;
    out_summary->package_type = manifest.package_type;
    CopyFixed(
        out_summary->id,
        sizeof(out_summary->id),
        manifest.id);
    CopyFixed(
        out_summary->name,
        sizeof(out_summary->name),
        manifest.name);
    CopyFixed(
        out_summary->version,
        sizeof(out_summary->version),
        manifest.version);
    CopyFixed(
        out_summary->developer,
        sizeof(out_summary->developer),
        manifest.developer);
    CopyFixed(
        out_summary->content_descriptor_path,
        sizeof(out_summary->content_descriptor_path),
        manifest.content_descriptor_path);
    out_summary->has_signature =
        manifest.signature_algorithm.empty() ? 0u : 1u;
    CopyFixed(
        out_summary->signature_algorithm,
        sizeof(out_summary->signature_algorithm),
        manifest.signature_algorithm);
    CopyFixed(
        out_summary->signature_key_id,
        sizeof(out_summary->signature_key_id),
        manifest.signature_key_id);
    CopyFixed(
        out_summary->signature_value,
        sizeof(out_summary->signature_value),
        manifest.signature_value);

    uint32_t requested_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    for (const std::string& permission :
         manifest.permissions) {
        requested_tier = std::max(
            requested_tier,
            ManifestPermissionTier(permission));
    }
    out_summary->requested_permission_tier =
        requested_tier;
    out_summary->permission_count =
        static_cast<uint32_t>(manifest.permissions.size());
    for (size_t i = 0;
         i < manifest.permissions.size(); ++i) {
        CopyFixed(
            out_summary->permissions[i],
            sizeof(out_summary->permissions[i]),
            manifest.permissions[i]);
    }

    out_summary->dependency_count =
        static_cast<uint32_t>(
            manifest.dependencies.size());
    for (size_t i = 0;
         i < manifest.dependencies.size(); ++i) {
        out_summary->dependencies[i] =
            manifest.dependencies[i];
    }
    out_summary->client_build_count =
        static_cast<uint32_t>(
            manifest.client_builds.size());
    for (size_t i = 0;
         i < manifest.client_builds.size(); ++i) {
        CopyFixed(
            out_summary->client_builds[i],
            sizeof(out_summary->client_builds[i]),
            manifest.client_builds[i]);
    }
    out_summary->client_executable_hash_count =
        static_cast<uint32_t>(
            manifest.client_executable_hashes.size());
    for (size_t i = 0;
         i < manifest.client_executable_hashes.size(); ++i) {
        CopyFixed(
            out_summary->client_executable_hashes[i],
            sizeof(
                out_summary->client_executable_hashes[i]),
            manifest.client_executable_hashes[i]);
    }

    for (const auto& entrypoint : manifest.entrypoints) {
        if (std::strcmp(
                entrypoint.platform,
                "windows-x86") == 0) {
            out_summary->has_windows_x86_entrypoint = 1u;
            CopyFixed(
                out_summary->windows_x86_entrypoint,
                sizeof(
                    out_summary->windows_x86_entrypoint),
                entrypoint.relative_path);
            break;
        }
    }
    if (manifest.package_type ==
            WOTBMOD_V3_PACKAGE_NATIVE &&
        out_summary->has_windows_x86_entrypoint == 0u) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_PLATFORM,
            "native manifest has no windows-x86 entrypoint");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result PreflightManifestFile(
    const char* physical_manifest_file,
    DataManifestPreflightSummary* out_summary) {
    if (!InitializePreflightSummary(out_summary)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!physical_manifest_file ||
        !*physical_manifest_file) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "physical manifest file path is empty");
    }
    const fs::path path =
        fs::u8path(physical_manifest_file);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_IO,
            "manifest preflight file is not a regular file");
    }
    const uint64_t size = fs::file_size(path, ec);
    if (ec) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_IO,
            "cannot query manifest preflight file size");
    }
    if (size == 0 || size > 4u * 1024u * 1024u ||
        size > std::numeric_limits<size_t>::max()) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "manifest preflight file size is invalid");
    }
    std::vector<uint8_t> bytes(
        static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_IO,
            "cannot open manifest preflight file");
    }
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input ||
        static_cast<uint64_t>(input.gcount()) != size) {
        return PreflightFailure(
            out_summary,
            WOTBMOD_V3_E_IO,
            "short manifest preflight file read");
    }
    out_summary->struct_size = sizeof(*out_summary);
    return PreflightManifestUtf8(
        bytes.data(), bytes.size(), out_summary);
}

namespace {

struct ArchiveStoredEntry {
    std::string relative_path;
    fs::path physical_path;
    bool directory = false;
    bool package_entry = false;
    bool memory_entry = false;
    uint32_t depth = 0;
    uint64_t size = 0;
    uint64_t data_offset = 0;
    uint32_t crc32 = 0;
    std::string sha256;
    std::vector<uint8_t> memory_bytes;
};

struct ArchiveObject final : TaggedObject {
    ArchiveObject()
        : TaggedObject(ObjectKind::Archive) {}
    fs::path root;
    WotbModV3ArchiveLimits limits = {};
    std::vector<ArchiveStoredEntry> entries;
    std::string sha256;
    std::atomic<bool> cancelled{false};
};

WotbModV3Result ArchiveAccess(WotbModV3Handle mod) {
    return VfsAccess(mod);
}

bool ValidRelativeArchivePath(
    const char* text,
    std::vector<std::string>* out_segments = nullptr) {
    if (!text || !*text ||
        std::strlen(text) >= WOTBMOD_V3_MAX_PATH ||
        std::strchr(text, '\\') || std::strchr(text, '%') ||
        fs::path(text).is_absolute()) {
        return false;
    }
    std::istringstream stream(text);
    std::string segment;
    std::vector<std::string> segments;
    while (std::getline(stream, segment, '/')) {
        if (!IsUriSegmentSafe(segment)) {
            return false;
        }
        segments.push_back(segment);
    }
    if (segments.empty()) return false;
    if (out_segments) *out_segments = std::move(segments);
    return true;
}

bool ArchiveRangeFits(
    uint64_t offset,
    uint64_t length,
    uint64_t bound) {
    return offset <= bound && length <= bound - offset;
}

uint16_t ArchiveReadU16(const uint8_t* bytes) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[0]) |
        (static_cast<uint16_t>(bytes[1]) << 8u));
}

uint32_t ArchiveReadU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

bool ArchiveReadAt(
    std::ifstream* input,
    uint64_t offset,
    void* destination,
    size_t size) {
    if (!input || (!destination && size != 0u) ||
        offset > static_cast<uint64_t>(
            (std::numeric_limits<std::streamoff>::max)()) ||
        size > static_cast<size_t>(
            (std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    input->clear();
    input->seekg(
        static_cast<std::streamoff>(offset),
        std::ios::beg);
    if (!*input) return false;
    if (size == 0u) return true;
    input->read(
        static_cast<char*>(destination),
        static_cast<std::streamsize>(size));
    return input->good() ||
           (input->eof() &&
            static_cast<size_t>(input->gcount()) == size);
}

bool ArchiveIsWindowsReservedSegment(
    const std::string& segment) {
    std::string base = segment;
    const size_t dot = base.find('.');
    if (dot != std::string::npos) {
        base.resize(dot);
    }
    base = LowerAscii(base);
    if (base == "con" || base == "prn" ||
        base == "aux" || base == "nul" ||
        base == "conin$" || base == "conout$") {
        return true;
    }
    return base.size() == 4u &&
           (base.rfind("com", 0u) == 0u ||
            base.rfind("lpt", 0u) == 0u) &&
           base[3] >= '1' && base[3] <= '9';
}

bool FoldArchivePathForWindows(
    const std::string& value,
    std::wstring* out) {
    if (!out || value.empty()) return false;
#if defined(_WIN32)
    if (value.size() >
        static_cast<size_t>(
            (std::numeric_limits<int>::max)())) {
        return false;
    }
    const int source_size =
        static_cast<int>(value.size());
    const int wide_size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        source_size,
        nullptr,
        0);
    if (wide_size <= 0) return false;
    std::wstring wide(
        static_cast<size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            source_size,
            &wide[0],
            wide_size) != wide_size) {
        return false;
    }
    const int folded_size = LCMapStringW(
        LOCALE_INVARIANT,
        LCMAP_LOWERCASE,
        wide.data(),
        wide_size,
        nullptr,
        0);
    if (folded_size <= 0) return false;
    std::wstring folded(
        static_cast<size_t>(folded_size), L'\0');
    if (LCMapStringW(
            LOCALE_INVARIANT,
            LCMAP_LOWERCASE,
            wide.data(),
            wide_size,
            &folded[0],
            folded_size) != folded_size) {
        return false;
    }
    *out = std::move(folded);
#else
    std::wstring folded;
    folded.reserve(value.size());
    for (unsigned char c : value) {
        folded.push_back(
            static_cast<wchar_t>(std::tolower(c)));
    }
    *out = std::move(folded);
#endif
    return true;
}

enum class PackagePathStatus {
    Valid,
    Unsafe,
    DepthExceeded,
    UnsupportedEncoding,
    MalformedEncoding
};

PackagePathStatus ValidatePackageArchivePath(
    const std::string& text,
    bool utf8_flag,
    uint32_t max_depth,
    bool* out_directory,
    uint32_t* out_depth,
    std::wstring* out_folded) {
    if (out_directory) *out_directory = false;
    if (out_depth) *out_depth = 0u;
    if (text.empty() ||
        text.size() >= WOTBMOD_V3_MAX_PATH ||
        text.front() == '/' ||
        text.find("//") != std::string::npos ||
        text.find('\\') != std::string::npos ||
        text.find('\0') != std::string::npos) {
        return PackagePathStatus::Unsafe;
    }
    const bool contains_non_ascii = std::any_of(
        text.begin(), text.end(),
        [](unsigned char c) { return c >= 0x80u; });
    if (contains_non_ascii && !utf8_flag) {
        return PackagePathStatus::UnsupportedEncoding;
    }
    if (!IsValidUtf8(
            reinterpret_cast<const uint8_t*>(text.data()),
            text.size())) {
        return utf8_flag
            ? PackagePathStatus::MalformedEncoding
            : PackagePathStatus::UnsupportedEncoding;
    }
    const bool directory = text.back() == '/';
    const size_t logical_size =
        directory ? text.size() - 1u : text.size();
    if (logical_size == 0u) {
        return PackagePathStatus::Unsafe;
    }

    uint32_t depth = 0u;
    size_t start = 0u;
    while (start < logical_size) {
        const size_t slash = text.find('/', start);
        const size_t end =
            slash == std::string::npos ||
            slash > logical_size
            ? logical_size : slash;
        const std::string segment =
            text.substr(start, end - start);
        if (segment.empty() || segment == "." ||
            segment == ".." || segment.size() > 255u ||
            segment.back() == ' ' || segment.back() == '.' ||
            ArchiveIsWindowsReservedSegment(segment)) {
            return PackagePathStatus::Unsafe;
        }
        for (unsigned char c : segment) {
            if (c < 0x20u || c == 0x7fu ||
                c == '<' || c == '>' || c == ':' ||
                c == '"' || c == '|' || c == '?' ||
                c == '*' || c == '%' || c == '#') {
                return PackagePathStatus::Unsafe;
            }
        }
        ++depth;
        if (depth > max_depth) {
            return PackagePathStatus::DepthExceeded;
        }
        if (end == logical_size) break;
        start = end + 1u;
    }

    const std::string logical_path =
        text.substr(0u, logical_size);
    if (!FoldArchivePathForWindows(
            logical_path, out_folded)) {
        return PackagePathStatus::UnsupportedEncoding;
    }
    if (out_directory) *out_directory = directory;
    if (out_depth) *out_depth = depth;
    return PackagePathStatus::Valid;
}

uint32_t ArchiveCrc32Update(
    uint32_t crc,
    const uint8_t* bytes,
    size_t size) {
    for (size_t i = 0u; i < size; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^
                  (0xedb88320u & mask);
        }
    }
    return crc;
}

WotbModV3ArchiveLimits EffectiveArchiveLimits(
    const WotbModV3ArchiveLimits* requested,
    bool* valid) {
    WotbModV3ArchiveLimits limits = {};
    limits.struct_size = sizeof(limits);
    limits.api_version = WOTBMOD_V3_ARCHIVE_VERSION;
    limits.max_depth = 16u;
    limits.max_files = 4096u;
    limits.max_total_unpacked_bytes = 512ull * 1024ull * 1024ull;
    limits.max_single_file_bytes = 128ull * 1024ull * 1024ull;
    if (valid) *valid = true;
    if (!requested) return limits;
    if (!ValidStruct(
            requested, requested->struct_size,
            sizeof(WotbModV3ArchiveLimits)) ||
        requested->max_depth == 0 ||
        requested->max_depth > 64u ||
        requested->max_files == 0 ||
        requested->max_files > 100000u ||
        requested->max_total_unpacked_bytes == 0 ||
        requested->max_total_unpacked_bytes >
            4ull * 1024ull * 1024ull * 1024ull ||
        requested->max_single_file_bytes == 0 ||
        requested->max_single_file_bytes >
            requested->max_total_unpacked_bytes ||
        !FixedTerminated(
            requested->expected_sha256,
            sizeof(requested->expected_sha256)) ||
        (requested->expected_sha256[0] != '\0' &&
         !IsSha256(requested->expected_sha256))) {
        if (valid) *valid = false;
        return limits;
    }
    return *requested;
}

WotbModV3Result OpenNativeDavaArchiveSnapshot(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3ArchiveHandle* out_archive) {
    if (!uri || !out_archive) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "native DAVA archive snapshot request is invalid");
    }
    *out_archive = WOTBMOD_V3_INVALID_HANDLE;
    if ((InstalledDavaNativeCapabilities() &
         WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE) == 0u) {
        return NativeDavaFailure(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "native DAVA archive open");
    }

    std::vector<char> physical_path;
    WotbModV3Result result =
        ResolveNativeDavaPath(mod, uri, &physical_path);
    if (result != WOTBMOD_V3_OK) return result;

    WotbModDavaNativeToken native_archive = 0u;
    result = InstalledDavaNativeArchiveOpenFile(
        mod, physical_path.data(), &native_archive);
    if (result != WOTBMOD_V3_OK) {
        return NativeDavaFailure(
            mod, result, "native DAVA archive open");
    }

    auto finish_native = [&](WotbModV3Result operation_result) {
        const WotbModV3Result released =
            InstalledDavaNativeRelease(mod, native_archive);
        if (operation_result != WOTBMOD_V3_OK) {
            return operation_result;
        }
        return released == WOTBMOD_V3_OK
            ? WOTBMOD_V3_OK
            : NativeDavaFailure(
                  mod, released, "native DAVA archive release");
    };

    uint32_t count = 0u;
    result = InstalledDavaNativeArchiveGetEntryCount(
        mod, native_archive, &count);
    if (result != WOTBMOD_V3_OK) {
        return finish_native(NativeDavaFailure(
            mod, result, "native DAVA archive enumeration"));
    }

    bool limits_valid = false;
    const WotbModV3ArchiveLimits limits =
        EffectiveArchiveLimits(nullptr, &limits_valid);
    if (!limits_valid || count > limits.max_files) {
        return finish_native(Fail(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "native DAVA archive file count limit exceeded"));
    }

    std::unique_ptr<ArchiveObject> archive(new ArchiveObject());
    archive->root = fs::path(physical_path.data());
    archive->limits = limits;
    archive->entries.reserve(count);
    std::set<std::wstring> folded_paths;
    uint64_t total_size = 0u;

    for (uint32_t index = 0u; index < count; ++index) {
        WotbModDavaNativeArchiveEntry native_entry = {};
        native_entry.struct_size = sizeof(native_entry);
        result = InstalledDavaNativeArchiveGetEntry(
            mod, native_archive, index, &native_entry);
        if (result != WOTBMOD_V3_OK) {
            return finish_native(NativeDavaFailure(
                mod, result, "native DAVA archive entry query"));
        }
        bool directory = false;
        uint32_t depth = 0u;
        std::wstring folded;
        const PackagePathStatus path_status =
            ValidatePackageArchivePath(
                native_entry.relative_path,
                true,
                limits.max_depth,
                &directory,
                &depth,
                &folded);
        if (native_entry.index != index || native_entry.flags != 0u ||
            directory || path_status != PackagePathStatus::Valid) {
            return finish_native(Fail(
                mod, WOTBMOD_V3_E_PARSE,
                "native DAVA archive returned unsafe entry metadata"));
        }
        if (!folded_paths.insert(folded).second) {
            return finish_native(Fail(
                mod, WOTBMOD_V3_E_CONFLICT,
                "native DAVA archive contains colliding paths"));
        }
        if (native_entry.original_size >
                limits.max_single_file_bytes ||
            native_entry.original_size >
                (std::numeric_limits<uint32_t>::max)() ||
            total_size > limits.max_total_unpacked_bytes -
                native_entry.original_size) {
            return finish_native(Fail(
                mod, WOTBMOD_V3_E_LIMIT_REACHED,
                "native DAVA archive unpacked size limit exceeded"));
        }

        WotbModDavaNativeBuffer query = {};
        query.struct_size = sizeof(query);
        result = InstalledDavaNativeArchiveReadEntry(
            mod, native_archive, index, &query);
        if ((native_entry.original_size != 0u &&
             result != WOTBMOD_V3_E_BUFFER_TOO_SMALL) ||
            (native_entry.original_size == 0u &&
             result != WOTBMOD_V3_OK &&
             result != WOTBMOD_V3_E_BUFFER_TOO_SMALL) ||
            query.size != native_entry.original_size) {
            return finish_native(NativeDavaFailure(
                mod,
                result == WOTBMOD_V3_OK
                    ? WOTBMOD_V3_E_PLATFORM : result,
                "native DAVA archive entry sizing"));
        }

        ArchiveStoredEntry entry;
        entry.relative_path = native_entry.relative_path;
        entry.depth = depth;
        entry.size = native_entry.original_size;
        entry.crc32 = native_entry.original_crc32;
        entry.memory_entry = true;
        entry.memory_bytes.resize(query.size);
        if (!entry.memory_bytes.empty()) {
            WotbModDavaNativeBuffer read = {};
            read.struct_size = sizeof(read);
            read.data = entry.memory_bytes.data();
            read.capacity = static_cast<uint32_t>(
                entry.memory_bytes.size());
            result = InstalledDavaNativeArchiveReadEntry(
                mod, native_archive, index, &read);
            if (result != WOTBMOD_V3_OK ||
                read.size != entry.memory_bytes.size()) {
                return finish_native(NativeDavaFailure(
                    mod,
                    result == WOTBMOD_V3_OK
                        ? WOTBMOD_V3_E_PLATFORM : result,
                    "native DAVA archive entry read"));
            }
        }
        uint32_t actual_crc32 = 0xffffffffu;
        actual_crc32 = ArchiveCrc32Update(
            actual_crc32,
            entry.memory_bytes.data(),
            entry.memory_bytes.size());
        if (~actual_crc32 != entry.crc32) {
            return finish_native(Fail(
                mod, WOTBMOD_V3_E_HASH_MISMATCH,
                "native DAVA archive entry CRC does not match copied bytes"));
        }
        entry.sha256 = Sha256Bytes(
            entry.memory_bytes.data(), entry.memory_bytes.size());
        total_size += entry.size;
        archive->entries.push_back(std::move(entry));
    }

    std::sort(
        archive->entries.begin(), archive->entries.end(),
        [](const ArchiveStoredEntry& left,
           const ArchiveStoredEntry& right) {
            return left.relative_path < right.relative_path;
        });
    if (!folded_paths.empty()) {
        auto previous = folded_paths.begin();
        for (auto current = std::next(previous);
             current != folded_paths.end(); ++current, ++previous) {
            const std::wstring parent = *previous + L"/";
            if (current->rfind(parent, 0u) == 0u) {
                return finish_native(Fail(
                    mod, WOTBMOD_V3_E_CONFLICT,
                    "native DAVA archive file path collides with a child entry"));
            }
        }
    }

    Sha256 digest;
    for (const ArchiveStoredEntry& entry : archive->entries) {
        const uint32_t path_size =
            static_cast<uint32_t>(entry.relative_path.size());
        digest.Update(&path_size, sizeof(path_size));
        digest.Update(
            entry.relative_path.data(), entry.relative_path.size());
        digest.Update(&entry.size, sizeof(entry.size));
        digest.Update(
            entry.memory_bytes.data(), entry.memory_bytes.size());
    }
    archive->sha256 = digest.FinalHex();

    result = finish_native(WOTBMOD_V3_OK);
    if (result != WOTBMOD_V3_OK) return result;

    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_ARCHIVE,
        archive.get(),
        DestroyObject<ArchiveObject>,
        &handle);
    if (result != WOTBMOD_V3_OK) return result;
    archive.release();
    *out_archive = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ValidateArchiveZipExtra(
    WotbModV3Handle mod,
    std::ifstream* input,
    uint64_t offset,
    uint16_t size) {
    if (size == 0u) return WOTBMOD_V3_OK;
    std::vector<uint8_t> bytes(size);
    if (!ArchiveReadAt(
            input, offset, bytes.data(), bytes.size())) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot read ZIP extra fields");
    }
    size_t position = 0u;
    while (position < bytes.size()) {
        if (bytes.size() - position < 4u) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP extra field header is truncated");
        }
        const uint16_t id =
            ArchiveReadU16(bytes.data() + position);
        const uint16_t field_size =
            ArchiveReadU16(bytes.data() + position + 2u);
        position += 4u;
        if (field_size > bytes.size() - position) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP extra field payload is truncated");
        }
        if (id == 0x0001u) {
            return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                        "ZIP64 extra fields are unsupported");
        }
        position += field_size;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result HashStoredPackageEntry(
    WotbModV3Handle mod,
    const fs::path& package,
    uint64_t data_offset,
    uint64_t size,
    std::string* out_sha256,
    uint32_t* out_crc32) {
    if (!out_sha256 || !out_crc32) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "ZIP entry hash output is null");
    }
    std::ifstream input(package, std::ios::binary);
    if (!input) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot open ZIP entry for validation");
    }
    if (!ArchiveReadAt(&input, data_offset, nullptr, 0u)) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot seek to ZIP entry data");
    }
    Sha256 sha256;
    uint32_t crc32 = 0xffffffffu;
    uint64_t remaining = size;
    std::vector<uint8_t> buffer(64u * 1024u);
    while (remaining != 0u) {
        const size_t chunk = static_cast<size_t>(
            (std::min)(
                remaining,
                static_cast<uint64_t>(buffer.size())));
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(chunk));
        if (static_cast<size_t>(input.gcount()) != chunk) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "short ZIP entry read during validation");
        }
        sha256.Update(buffer.data(), chunk);
        crc32 = ArchiveCrc32Update(
            crc32, buffer.data(), chunk);
        remaining -= chunk;
    }
    *out_sha256 = sha256.FinalHex();
    *out_crc32 = ~crc32;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ReadStoredPackageEntry(
    WotbModV3Handle mod,
    const ArchiveObject& archive,
    const ArchiveStoredEntry& entry,
    std::vector<uint8_t>* out_bytes) {
    if (!out_bytes || entry.directory ||
        !entry.package_entry) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid stored ZIP entry read request");
    }
    if (entry.size >
            archive.limits.max_single_file_bytes ||
        entry.size >
            static_cast<uint64_t>(
                (std::numeric_limits<size_t>::max)()) ||
        entry.size >
            static_cast<uint64_t>(
                (std::numeric_limits<uint32_t>::max)())) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "ZIP entry exceeds readable ABI size");
    }
    std::ifstream input(archive.root, std::ios::binary);
    if (!input) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot open ZIP package for reading");
    }
    out_bytes->assign(
        static_cast<size_t>(entry.size), 0u);
    if (!ArchiveReadAt(
            &input,
            entry.data_offset,
            out_bytes->empty() ? nullptr : out_bytes->data(),
            out_bytes->size())) {
        out_bytes->clear();
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "short ZIP entry read");
    }
    uint32_t crc32 = 0xffffffffu;
    crc32 = ArchiveCrc32Update(
        crc32, out_bytes->data(), out_bytes->size());
    if (~crc32 != entry.crc32 ||
        Sha256Bytes(
            out_bytes->data(),
            out_bytes->size()) != entry.sha256) {
        out_bytes->clear();
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "ZIP entry changed after validation");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result BuildStoredZipArchive(
    WotbModV3Handle mod,
    ArchiveObject* archive) {
    if (!archive) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive object is null");
    }
    constexpr uint64_t kZipMetadataBudget =
        64ull * 1024ull * 1024ull;
    std::error_code ec;
    const uint64_t file_size =
        fs::file_size(archive->root, ec);
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot query ZIP package size");
    }
    uint64_t package_budget =
        archive->limits.max_total_unpacked_bytes;
    if (package_budget >
        static_cast<uint64_t>(
            (std::numeric_limits<uint32_t>::max)()) -
            kZipMetadataBudget) {
        package_budget =
            (std::numeric_limits<uint32_t>::max)();
    } else {
        package_budget += kZipMetadataBudget;
    }
    if (file_size < 22u) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "package is not a ZIP archive");
    }
    if (file_size > package_budget) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "ZIP package exceeds bounded on-disk size");
    }
    if (file_size >
        static_cast<uint64_t>(
            (std::numeric_limits<uint32_t>::max)())) {
        return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                    "ZIP64 packages are unsupported");
    }

    std::ifstream input(archive->root, std::ios::binary);
    if (!input) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot open ZIP package");
    }
    const uint64_t tail_size64 = (std::min)(
        file_size, 22ull + 65535ull);
    const size_t tail_size =
        static_cast<size_t>(tail_size64);
    std::vector<uint8_t> tail(tail_size);
    const uint64_t tail_offset = file_size - tail_size64;
    if (!ArchiveReadAt(
            &input, tail_offset, tail.data(), tail.size())) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot read ZIP end record");
    }

    size_t eocd_in_tail =
        (std::numeric_limits<size_t>::max)();
    for (size_t candidate = tail.size() - 22u + 1u;
         candidate-- > 0u;) {
        if (ArchiveReadU32(
                tail.data() + candidate) != 0x06054b50u) {
            continue;
        }
        const uint16_t comment_size =
            ArchiveReadU16(tail.data() + candidate + 20u);
        if (candidate + 22u + comment_size == tail.size()) {
            eocd_in_tail = candidate;
            break;
        }
    }
    if (eocd_in_tail ==
        (std::numeric_limits<size_t>::max)()) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "ZIP end record is missing or malformed");
    }
    const uint8_t* eocd = tail.data() + eocd_in_tail;
    const uint64_t eocd_offset =
        tail_offset + eocd_in_tail;
    const uint16_t disk = ArchiveReadU16(eocd + 4u);
    const uint16_t central_disk =
        ArchiveReadU16(eocd + 6u);
    const uint16_t disk_entries =
        ArchiveReadU16(eocd + 8u);
    const uint16_t total_entries =
        ArchiveReadU16(eocd + 10u);
    const uint32_t central_size =
        ArchiveReadU32(eocd + 12u);
    const uint32_t central_offset =
        ArchiveReadU32(eocd + 16u);
    if (disk != 0u || central_disk != 0u ||
        disk_entries != total_entries) {
        return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                    "multi-disk ZIP packages are unsupported");
    }
    if (total_entries == 0xffffu ||
        central_size == 0xffffffffu ||
        central_offset == 0xffffffffu) {
        return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                    "ZIP64 packages are unsupported");
    }
    if (total_entries > archive->limits.max_files) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "ZIP entry count limit exceeded");
    }
    if (central_size > kZipMetadataBudget) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "ZIP central directory exceeds metadata budget");
    }
    if (!ArchiveRangeFits(
            central_offset, central_size, eocd_offset) ||
        static_cast<uint64_t>(central_offset) +
            central_size != eocd_offset) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "ZIP central directory bounds are invalid");
    }

    struct FoldedZipPath {
        std::wstring path;
        bool directory = false;
    };
    std::map<std::wstring, bool> unique_paths;
    std::vector<FoldedZipPath> folded_paths;
    std::vector<std::pair<uint64_t, uint64_t>>
        occupied_ranges;
    archive->entries.clear();
    archive->entries.reserve(total_entries);
    folded_paths.reserve(total_entries);
    occupied_ranges.reserve(total_entries);
    uint64_t cursor = central_offset;
    uint64_t total_unpacked = 0u;

    for (uint32_t index = 0u;
         index < total_entries; ++index) {
        uint8_t central[46] = {};
        if (!ArchiveRangeFits(cursor, sizeof(central), eocd_offset) ||
            !ArchiveReadAt(
                &input, cursor, central, sizeof(central))) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP central entry is truncated");
        }
        if (ArchiveReadU32(central) != 0x02014b50u) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP central entry signature is invalid");
        }
        const uint16_t version_made_by =
            ArchiveReadU16(central + 4u);
        const uint16_t flags =
            ArchiveReadU16(central + 8u);
        const uint16_t method =
            ArchiveReadU16(central + 10u);
        const uint32_t expected_crc32 =
            ArchiveReadU32(central + 16u);
        const uint32_t compressed_size =
            ArchiveReadU32(central + 20u);
        const uint32_t unpacked_size =
            ArchiveReadU32(central + 24u);
        const uint16_t name_size =
            ArchiveReadU16(central + 28u);
        const uint16_t extra_size =
            ArchiveReadU16(central + 30u);
        const uint16_t comment_size =
            ArchiveReadU16(central + 32u);
        const uint16_t start_disk =
            ArchiveReadU16(central + 34u);
        const uint32_t external_attributes =
            ArchiveReadU32(central + 38u);
        const uint32_t local_header_offset =
            ArchiveReadU32(central + 42u);
        if (start_disk != 0u) {
            return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                        "multi-disk ZIP entries are unsupported");
        }
        if (compressed_size == 0xffffffffu ||
            unpacked_size == 0xffffffffu ||
            local_header_offset == 0xffffffffu) {
            return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                        "ZIP64 entries are unsupported");
        }
        if (name_size == 0u ||
            name_size >= WOTBMOD_V3_MAX_PATH) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP entry name length is invalid");
        }
        const uint64_t variable_size =
            static_cast<uint64_t>(name_size) +
            extra_size + comment_size;
        if (!ArchiveRangeFits(
                cursor + sizeof(central),
                variable_size,
                eocd_offset)) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP central entry metadata is truncated");
        }
        std::vector<uint8_t> name_bytes(name_size);
        if (!ArchiveReadAt(
                &input,
                cursor + sizeof(central),
                name_bytes.data(),
                name_bytes.size())) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot read ZIP entry name");
        }
        WotbModV3Result extra_result =
            ValidateArchiveZipExtra(
                mod,
                &input,
                cursor + sizeof(central) + name_size,
                extra_size);
        if (extra_result != WOTBMOD_V3_OK) {
            return extra_result;
        }
        if ((flags & ~0x0800u) != 0u) {
            return Fail(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "encrypted, descriptor-backed, patched, or masked ZIP entries are unsupported");
        }
        if (method != 0u) {
            return Fail(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "compressed ZIP entries are unsupported; use store method");
        }
        const std::string path(
            reinterpret_cast<const char*>(name_bytes.data()),
            name_bytes.size());
        bool directory = false;
        uint32_t depth = 0u;
        std::wstring folded;
        const PackagePathStatus path_status =
            ValidatePackageArchivePath(
                path,
                (flags & 0x0800u) != 0u,
                archive->limits.max_depth,
                &directory,
                &depth,
                &folded);
        if (path_status ==
            PackagePathStatus::DepthExceeded) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "ZIP entry depth limit exceeded");
        }
        if (path_status ==
            PackagePathStatus::UnsupportedEncoding) {
            return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                        "ZIP entry path encoding is unsupported");
        }
        if (path_status ==
            PackagePathStatus::MalformedEncoding) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP UTF-8 entry path is malformed");
        }
        if (path_status != PackagePathStatus::Valid) {
            return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                        "ZIP contains an unsafe entry path");
        }
        if (!unique_paths.emplace(
                folded, directory).second) {
            return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                        "ZIP contains duplicate Windows paths");
        }
        folded_paths.push_back({folded, directory});
        if (compressed_size != unpacked_size) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "stored ZIP entry size fields differ");
        }
        if (directory &&
            (unpacked_size != 0u ||
             expected_crc32 != 0u)) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP directory entry contains file data");
        }
        if (unpacked_size >
            archive->limits.max_single_file_bytes) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "ZIP single-entry size limit exceeded");
        }
        if (unpacked_size >
                archive->limits.max_total_unpacked_bytes ||
            total_unpacked >
            archive->limits.max_total_unpacked_bytes -
                unpacked_size) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "ZIP total unpacked-size limit exceeded");
        }
        total_unpacked += unpacked_size;

        const uint8_t creator =
            static_cast<uint8_t>(version_made_by >> 8u);
        const uint16_t unix_mode =
            static_cast<uint16_t>(
                external_attributes >> 16u);
        const uint16_t unix_type =
            static_cast<uint16_t>(
                unix_mode & 0xf000u);
        if (unix_type == 0xa000u) {
            return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                        "symbolic links in ZIP packages are unsupported");
        }
        if (creator == 3u || creator == 19u) {
            if (unix_type != 0u &&
                unix_type != 0x8000u &&
                unix_type != 0x4000u) {
                return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                            "non-regular ZIP filesystem entries are unsupported");
            }
            if ((unix_type == 0x4000u && !directory) ||
                (unix_type == 0x8000u && directory)) {
                return Fail(mod, WOTBMOD_V3_E_PARSE,
                            "ZIP path and Unix entry type differ");
            }
        }
        if ((external_attributes & 0x10u) != 0u &&
            !directory) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP path and DOS directory attribute differ");
        }

        uint8_t local[30] = {};
        if (!ArchiveRangeFits(
                local_header_offset,
                sizeof(local),
                central_offset) ||
            !ArchiveReadAt(
                &input,
                local_header_offset,
                local,
                sizeof(local)) ||
            ArchiveReadU32(local) != 0x04034b50u) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP local header is invalid");
        }
        const uint16_t local_flags =
            ArchiveReadU16(local + 6u);
        const uint16_t local_method =
            ArchiveReadU16(local + 8u);
        const uint32_t local_crc32 =
            ArchiveReadU32(local + 14u);
        const uint32_t local_compressed_size =
            ArchiveReadU32(local + 18u);
        const uint32_t local_unpacked_size =
            ArchiveReadU32(local + 22u);
        const uint16_t local_name_size =
            ArchiveReadU16(local + 26u);
        const uint16_t local_extra_size =
            ArchiveReadU16(local + 28u);
        const uint64_t local_name_offset =
            static_cast<uint64_t>(local_header_offset) +
            sizeof(local);
        if (local_flags != flags ||
            local_method != method ||
            local_crc32 != expected_crc32 ||
            local_compressed_size != compressed_size ||
            local_unpacked_size != unpacked_size ||
            local_name_size != name_size ||
            !ArchiveRangeFits(
                local_name_offset,
                static_cast<uint64_t>(local_name_size) +
                    local_extra_size,
                central_offset)) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP local and central metadata differ");
        }
        std::vector<uint8_t> local_name(local_name_size);
        if (!ArchiveReadAt(
                &input,
                local_name_offset,
                local_name.data(),
                local_name.size()) ||
            local_name != name_bytes) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP local and central names differ");
        }
        extra_result = ValidateArchiveZipExtra(
            mod,
            &input,
            local_name_offset + local_name_size,
            local_extra_size);
        if (extra_result != WOTBMOD_V3_OK) {
            return extra_result;
        }
        const uint64_t data_offset =
            local_name_offset +
            local_name_size +
            local_extra_size;
        if (!ArchiveRangeFits(
                data_offset,
                compressed_size,
                central_offset)) {
            return Fail(mod, WOTBMOD_V3_E_PARSE,
                        "ZIP entry data exceeds local-data bounds");
        }
        occupied_ranges.emplace_back(
            local_header_offset,
            data_offset + compressed_size);

        ArchiveStoredEntry entry;
        entry.relative_path = path;
        entry.directory = directory;
        entry.package_entry = true;
        entry.depth = depth;
        entry.size = unpacked_size;
        entry.data_offset = data_offset;
        entry.crc32 = expected_crc32;
        if (!directory) {
            uint32_t actual_crc32 = 0u;
            WotbModV3Result hashed =
                HashStoredPackageEntry(
                    mod,
                    archive->root,
                    entry.data_offset,
                    entry.size,
                    &entry.sha256,
                    &actual_crc32);
            if (hashed != WOTBMOD_V3_OK) return hashed;
            if (actual_crc32 != entry.crc32) {
                return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                            "ZIP entry CRC32 mismatch");
            }
        }
        archive->entries.push_back(std::move(entry));
        cursor += sizeof(central) + variable_size;
    }
    if (cursor != eocd_offset) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "ZIP central directory has trailing records");
    }
    if (file_size < total_unpacked ||
        file_size - total_unpacked >
            kZipMetadataBudget) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "ZIP metadata and prefix budget exceeded");
    }

    for (const FoldedZipPath& item : folded_paths) {
        size_t slash = item.path.find(L'/');
        while (slash != std::wstring::npos) {
            const std::wstring ancestor =
                item.path.substr(0u, slash);
            const auto found = unique_paths.find(ancestor);
            if (found != unique_paths.end() &&
                !found->second) {
                return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                            "ZIP file path collides with a child entry");
            }
            slash = item.path.find(L'/', slash + 1u);
        }
    }
    std::sort(
        occupied_ranges.begin(),
        occupied_ranges.end());
    for (size_t index = 1u;
         index < occupied_ranges.size(); ++index) {
        if (occupied_ranges[index - 1u].second >
            occupied_ranges[index].first) {
            return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                        "ZIP local entry ranges overlap");
        }
    }
    std::sort(
        archive->entries.begin(),
        archive->entries.end(),
        [](const ArchiveStoredEntry& left,
           const ArchiveStoredEntry& right) {
            return left.relative_path < right.relative_path;
        });
    WotbModV3Result hashed =
        Sha256File(mod, archive->root, &archive->sha256);
    if (hashed != WOTBMOD_V3_OK) return hashed;
    if (archive->limits.expected_sha256[0] != '\0' &&
        archive->sha256 != LowerAscii(
            archive->limits.expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "ZIP package SHA-256 mismatch");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result BuildDirectoryArchive(
    WotbModV3Handle mod,
    ArchiveObject* archive) {
    if (!archive) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive object is null");
    }
    std::error_code ec;
    uint32_t file_count = 0;
    uint64_t total_size = 0;
    for (fs::recursive_directory_iterator iterator(
             archive->root,
             fs::directory_options::skip_permission_denied,
             ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (archive->cancelled.load()) {
            return Fail(mod, WOTBMOD_V3_E_CANCELLED,
                        "archive enumeration was cancelled");
        }
        if (iterator->is_symlink(ec) || ec) {
            return Fail(
                mod,
                ec ? WOTBMOD_V3_E_IO :
                     WOTBMOD_V3_E_PERMISSION_DENIED,
                ec ? "archive entry inspection failed" :
                     "symbolic links are forbidden in directory archives");
        }
        fs::path relative =
            fs::relative(iterator->path(), archive->root, ec);
        if (ec) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot calculate archive relative path");
        }
        const std::string relative_text =
            relative.generic_string();
        std::vector<std::string> segments;
        if (!ValidRelativeArchivePath(
                relative_text.c_str(), &segments)) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "archive contains an unsafe path");
        }
        if (segments.size() > archive->limits.max_depth) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "archive depth limit exceeded");
        }
        ArchiveStoredEntry entry;
        entry.relative_path = relative_text;
        entry.physical_path = iterator->path();
        entry.depth = static_cast<uint32_t>(segments.size());
        entry.directory = iterator->is_directory(ec);
        if (ec) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "archive entry type inspection failed");
        }
        if (!entry.directory) {
            if (!iterator->is_regular_file(ec) || ec) {
                return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                            "archive contains a non-regular entry");
            }
            ++file_count;
            if (file_count > archive->limits.max_files) {
                return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                            "archive file count limit exceeded");
            }
            entry.size = iterator->file_size(ec);
            if (ec) {
                return Fail(mod, WOTBMOD_V3_E_IO,
                            "cannot query archive entry size");
            }
            if (entry.size >
                archive->limits.max_single_file_bytes) {
                return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                            "archive single-file size limit exceeded");
            }
            if (total_size >
                archive->limits.max_total_unpacked_bytes -
                entry.size) {
                return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                            "archive total unpacked size limit exceeded");
            }
            total_size += entry.size;
            WotbModV3Result hashed =
                Sha256File(mod, entry.physical_path, &entry.sha256);
            if (hashed != WOTBMOD_V3_OK) return hashed;
        }
        archive->entries.push_back(std::move(entry));
    }
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "archive directory enumeration failed");
    }
    std::sort(
        archive->entries.begin(), archive->entries.end(),
        [](const ArchiveStoredEntry& left,
           const ArchiveStoredEntry& right) {
            return left.relative_path < right.relative_path;
        });
    Sha256 digest;
    for (const ArchiveStoredEntry& entry : archive->entries) {
        if (entry.directory) continue;
        const uint32_t path_size =
            static_cast<uint32_t>(entry.relative_path.size());
        digest.Update(&path_size, sizeof(path_size));
        digest.Update(
            entry.relative_path.data(),
            entry.relative_path.size());
        digest.Update(&entry.size, sizeof(entry.size));
        std::ifstream input(entry.physical_path, std::ios::binary);
        if (!input) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot read archive entry for directory hash");
        }
        char buffer[64 * 1024];
        while (input) {
            input.read(buffer, sizeof(buffer));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                digest.Update(buffer, static_cast<size_t>(count));
            }
        }
        if (!input.eof()) {
            return Fail(mod, WOTBMOD_V3_E_IO,
                        "archive directory hashing failed");
        }
    }
    archive->sha256 = digest.FinalHex();
    if (archive->limits.expected_sha256[0] != '\0' &&
        archive->sha256 !=
            LowerAscii(archive->limits.expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "archive SHA-256 mismatch");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveOpenDirectory(
    WotbModV3Handle mod,
    const char* physical_directory,
    const WotbModV3ArchiveLimits* requested_limits,
    WotbModV3ArchiveHandle* out_archive) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    bool limits_valid = false;
    WotbModV3ArchiveLimits limits =
        EffectiveArchiveLimits(requested_limits, &limits_valid);
    if (!physical_directory || !out_archive || !limits_valid) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid directory archive request");
    }
    fs::path canonical;
    if (!IsAllowedOwnedPhysicalPath(
            mod, fs::path(physical_directory), true,
            &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "archive source is outside owned roots");
    }
    std::error_code ec;
    if (!fs::is_directory(canonical, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive source is not a directory");
    }
    std::unique_ptr<ArchiveObject> archive(new ArchiveObject());
    archive->root = canonical;
    archive->limits = limits;
    WotbModV3Result built =
        BuildDirectoryArchive(mod, archive.get());
    if (built != WOTBMOD_V3_OK) return built;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_ARCHIVE,
        archive.get(),
        DestroyObject<ArchiveObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    archive.release();
    *out_archive = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveOpenPackageFile(
    WotbModV3Handle mod,
    const char* physical_file,
    const WotbModV3ArchiveLimits* requested_limits,
    WotbModV3ArchiveHandle* out_archive) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    bool valid = false;
    WotbModV3ArchiveLimits limits =
        EffectiveArchiveLimits(requested_limits, &valid);
    if (!physical_file || !out_archive || !valid) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid package archive request");
    }
    *out_archive = WOTBMOD_V3_INVALID_HANDLE;
    std::error_code ec;
    const fs::file_status source_status =
        fs::symlink_status(fs::path(physical_file), ec);
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot inspect ZIP package source");
    }
    if (fs::is_symlink(source_status)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "ZIP package source cannot be a symbolic link");
    }
    fs::path canonical;
    if (!IsAllowedOwnedPhysicalPath(
            mod, fs::path(physical_file), true, &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "package archive is outside owned roots");
    }
    if (!fs::is_regular_file(canonical, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "ZIP package source is not a regular file");
    }
    std::unique_ptr<ArchiveObject> archive(new ArchiveObject());
    archive->root = canonical;
    archive->limits = limits;
    WotbModV3Result built =
        BuildStoredZipArchive(mod, archive.get());
    if (built != WOTBMOD_V3_OK) return built;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_ARCHIVE,
        archive.get(),
        DestroyObject<ArchiveObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    archive.release();
    *out_archive = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetArchive(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    ArchiveObject** out) {
    return GetObject(
        mod, handle, WOTBMOD_V3_HANDLE_ARCHIVE,
        ObjectKind::Archive, out);
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveGetEntryCount(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    uint32_t* out_count) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_count) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "archive count output is null");
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found == WOTBMOD_V3_OK) {
        *out_count = static_cast<uint32_t>(archive->entries.size());
    }
    return found;
}

void FillArchiveEntry(
    const ArchiveStoredEntry& source,
    WotbModV3ArchiveEntry* destination) {
    destination->is_directory = source.directory ? 1u : 0u;
    destination->depth = source.depth;
    destination->size = source.size;
    CopyFixed(
        destination->relative_path,
        sizeof(destination->relative_path),
        source.relative_path);
    CopyFixed(
        destination->sha256,
        sizeof(destination->sha256),
        source.sha256);
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveGetEntry(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    uint32_t index,
    WotbModV3ArchiveEntry* out_entry) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_entry, out_entry ? out_entry->struct_size : 0,
            sizeof(WotbModV3ArchiveEntry))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid archive entry output");
    }
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    if (index >= archive->entries.size()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "archive entry index is out of range");
    }
    FillArchiveEntry(archive->entries[index], out_entry);
    return WOTBMOD_V3_OK;
}

const ArchiveStoredEntry* FindArchiveEntry(
    const ArchiveObject& archive,
    const char* relative_path) {
    if (!ValidRelativeArchivePath(relative_path)) return nullptr;
    const std::string normalized =
        fs::path(relative_path).generic_string();
    auto found = std::lower_bound(
        archive.entries.begin(), archive.entries.end(), normalized,
        [](const ArchiveStoredEntry& entry,
           const std::string& value) {
            return entry.relative_path < value;
        });
    return found != archive.entries.end() &&
           found->relative_path == normalized
        ? &*found : nullptr;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveReadEntry(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    const char* relative_path,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    if (archive->cancelled.load()) {
        return Fail(mod, WOTBMOD_V3_E_CANCELLED,
                    "archive operation was cancelled");
    }
    const ArchiveStoredEntry* entry =
        FindArchiveEntry(*archive, relative_path);
    if (!entry) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "archive entry was not found");
    }
    if (entry->directory) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "cannot read an archive directory entry");
    }
    std::vector<uint8_t> bytes;
    WotbModV3Result read = WOTBMOD_V3_OK;
    if (entry->memory_entry) {
        bytes = entry->memory_bytes;
    } else if (entry->package_entry) {
        read = ReadStoredPackageEntry(
            mod, *archive, *entry, &bytes);
    } else {
        read = ReadPhysicalFile(
            mod, entry->physical_path,
            archive->limits.max_single_file_bytes, &bytes);
    }
    if (read != WOTBMOD_V3_OK) return read;
    if (!entry->package_entry && !entry->memory_entry &&
        Sha256Bytes(bytes.data(), bytes.size()) != entry->sha256) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "archive entry changed after validation");
    }
    return CopyOutBytes(
        mod, bytes.data(), bytes.size(), inout_buffer);
}

WotbModV3Result ValidateArchiveDestination(
    WotbModV3Handle mod,
    const char* destination_uri,
    std::string* out_normalized) {
    if (!destination_uri) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive destination URI is null");
    }
    std::string local_normalized;
    std::string* normalized_output =
        out_normalized ? out_normalized : &local_normalized;
    WotbModV3Result normalized = NormalizeOwnedUri(
        mod, destination_uri, normalized_output);
    if (normalized != WOTBMOD_V3_OK) return normalized;
    if (normalized_output->rfind("data://", 0) != 0 &&
        normalized_output->rfind("cache://", 0) != 0) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "archive extraction destination must use data:// or cache://");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ExtractArchiveFile(
    WotbModV3Handle mod,
    ArchiveObject* archive,
    const ArchiveStoredEntry& entry,
    const char* destination_uri) {
    if (archive->cancelled.load()) {
        return Fail(mod, WOTBMOD_V3_E_CANCELLED,
                    "archive extraction was cancelled");
    }
    if (entry.directory) return WOTBMOD_V3_OK;
    std::string normalized_base;
    WotbModV3Result normalized =
        ValidateArchiveDestination(
            mod, destination_uri, &normalized_base);
    if (normalized != WOTBMOD_V3_OK) return normalized;
    std::string child_uri = normalized_base;
    if (!entry.relative_path.empty()) {
        child_uri += "/" + entry.relative_path;
    }
    fs::path destination;
    WotbModV3Result writable =
        ResolveWritableUri(mod, child_uri.c_str(), &destination);
    if (writable != WOTBMOD_V3_OK) return writable;
    std::vector<uint8_t> bytes;
    WotbModV3Result read = WOTBMOD_V3_OK;
    if (entry.memory_entry) {
        bytes = entry.memory_bytes;
    } else if (entry.package_entry) {
        read = ReadStoredPackageEntry(
            mod, *archive, entry, &bytes);
    } else {
        read = ReadPhysicalFile(
            mod, entry.physical_path,
            archive->limits.max_single_file_bytes, &bytes);
    }
    if (read != WOTBMOD_V3_OK) return read;
    if (!entry.package_entry && !entry.memory_entry &&
        Sha256Bytes(bytes.data(), bytes.size()) != entry.sha256) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "archive entry changed after validation");
    }
    return AtomicWrite(
        mod, destination, bytes.data(), bytes.size());
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveExtractEntry(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    const char* relative_path,
    const char* destination_uri) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!destination_uri) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive destination URI is null");
    }
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    WotbModV3Result destination_valid =
        ValidateArchiveDestination(
            mod, destination_uri, nullptr);
    if (destination_valid != WOTBMOD_V3_OK) {
        return destination_valid;
    }
    const ArchiveStoredEntry* entry =
        FindArchiveEntry(*archive, relative_path);
    if (!entry) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "archive entry was not found");
    }
    return ExtractArchiveFile(
        mod, archive, *entry, destination_uri);
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveExtractAll(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    const char* destination_uri) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!destination_uri) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive destination URI is null");
    }
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    WotbModV3Result destination_valid =
        ValidateArchiveDestination(
            mod, destination_uri, nullptr);
    if (destination_valid != WOTBMOD_V3_OK) {
        return destination_valid;
    }
    for (const ArchiveStoredEntry& entry : archive->entries) {
        WotbModV3Result extracted =
            ExtractArchiveFile(
                mod, archive, entry, destination_uri);
        if (extracted != WOTBMOD_V3_OK) return extracted;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveGetSha256(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    char out_sha256[65]) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_sha256) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "archive hash output is null");
    }
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    std::memcpy(out_sha256, archive->sha256.c_str(), 65u);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveVerifySha256(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle,
    const char* expected_sha256) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!expected_sha256 ||
        !IsSha256(expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "expected archive SHA-256 is invalid");
    }
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    if (archive->sha256 != LowerAscii(expected_sha256)) {
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "archive SHA-256 mismatch");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ArchiveCancel(
    WotbModV3Handle mod,
    WotbModV3ArchiveHandle handle) {
    WotbModV3Result access = ArchiveAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ArchiveObject* archive = nullptr;
    WotbModV3Result found = GetArchive(mod, handle, &archive);
    if (found != WOTBMOD_V3_OK) return found;
    archive->cancelled.store(true);
    return WOTBMOD_V3_OK;
}

}  // namespace

namespace {

struct YamlNode {
    uint32_t type = WOTBMOD_V3_YAML_NULL;
    std::string scalar;
    bool boolean = false;
    int64_t integer = 0;
    double floating = 0.0;
    std::vector<std::pair<std::string, WotbModV3YamlNodeId>> map;
    std::vector<WotbModV3YamlNodeId> sequence;
};

struct YamlDocument final : TaggedObject {
    YamlDocument()
        : TaggedObject(ObjectKind::YamlDocument) {}
    WotbModV3YamlNodeId root = WOTBMOD_V3_YAML_INVALID_NODE;
    std::vector<YamlNode> nodes;
};

struct YamlLine {
    uint32_t indent = 0;
    uint32_t line_number = 0;
    std::string text;
};

std::string Trim(const std::string& input) {
    size_t begin = 0;
    while (begin < input.size() &&
           (input[begin] == ' ' || input[begin] == '\t' ||
            input[begin] == '\r' || input[begin] == '\n')) {
        ++begin;
    }
    size_t end = input.size();
    while (end > begin &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' ||
            input[end - 1] == '\r' || input[end - 1] == '\n')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string StripYamlComment(const std::string& line) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (double_quote && escaped) {
            escaped = false;
            continue;
        }
        if (double_quote && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (!single && !double_quote && c == '#' &&
            (i == 0 || line[i - 1] == ' ')) {
            return line.substr(0, i);
        }
    }
    return line;
}

bool PrepareYamlLines(
    const char* data,
    size_t size,
    std::vector<YamlLine>* out,
    std::string* error) {
    if (!data || !out) return false;
    size_t start = 0;
    uint32_t line_number = 1;
    while (start <= size) {
        const size_t end =
            std::string(data, size).find('\n', start);
        const size_t length =
            end == std::string::npos ? size - start : end - start;
        std::string line(data + start, length);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find('\t') != std::string::npos) {
            if (error) {
                *error = "tabs are forbidden in strict YAML at line " +
                         std::to_string(line_number);
            }
            return false;
        }
        uint32_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') {
            ++indent;
        }
        std::string text =
            Trim(StripYamlComment(line.substr(indent)));
        if (!text.empty() && text != "---" && text != "...") {
            if (text[0] == '%' || text[0] == '!' ||
                text[0] == '&' || text[0] == '*') {
                if (error) {
                    *error =
                        "directives, tags, anchors and aliases are unsupported at line " +
                        std::to_string(line_number);
                }
                return false;
            }
            out->push_back(YamlLine{indent, line_number, text});
        }
        if (end == std::string::npos) break;
        start = end + 1u;
        ++line_number;
    }
    return true;
}

size_t FindYamlColon(const std::string& text) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (double_quote && escaped) {
            escaped = false;
            continue;
        }
        if (double_quote && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
        } else if (!single && c == '"') {
            double_quote = !double_quote;
        } else if (!single && !double_quote && c == ':') {
            if (i + 1u == text.size() || text[i + 1u] == ' ') {
                return i;
            }
        }
    }
    return std::string::npos;
}

bool ParseYamlQuotedString(
    const std::string& text,
    std::string* output) {
    if (text.size() < 2u || !output) return false;
    if (text.front() == '"' && text.back() == '"') {
        JsonValue value;
        std::string error;
        JsonParser parser(text.data(), text.size());
        if (!parser.Parse(&value, &error) ||
            value.type != JsonValue::Type::String) {
            return false;
        }
        *output = value.text;
        return true;
    }
    if (text.front() == '\'' && text.back() == '\'') {
        output->clear();
        for (size_t i = 1; i + 1 < text.size(); ++i) {
            if (text[i] == '\'' && i + 2 < text.size() &&
                text[i + 1u] == '\'') {
                output->push_back('\'');
                ++i;
            } else {
                output->push_back(text[i]);
            }
        }
        return true;
    }
    return false;
}

bool ParseYamlScalar(
    const std::string& text,
    YamlNode* node,
    std::string* error) {
    if (!node) return false;
    if (text.empty()) {
        node->type = WOTBMOD_V3_YAML_NULL;
        return true;
    }
    if (text[0] == '[' || text[0] == '{' ||
        text[0] == '|' || text[0] == '>' ||
        text[0] == '!' || text[0] == '&' ||
        text[0] == '*') {
        if (error) {
            *error =
                "flow collections, block scalars, tags, anchors and aliases are unsupported";
        }
        return false;
    }
    if (text.front() == '"' || text.front() == '\'') {
        if (!ParseYamlQuotedString(text, &node->scalar)) {
            if (error) *error = "invalid quoted YAML scalar";
            return false;
        }
        node->type = WOTBMOD_V3_YAML_STRING;
        return true;
    }
    const std::string lower = LowerAscii(text);
    if (lower == "null" || text == "~") {
        node->type = WOTBMOD_V3_YAML_NULL;
        return true;
    }
    if (lower == "true" || lower == "false") {
        node->type = WOTBMOD_V3_YAML_BOOL;
        node->boolean = lower == "true";
        return true;
    }
    int64_t integer = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto integer_result =
        std::from_chars(begin, end, integer);
    if (integer_result.ec == std::errc() &&
        integer_result.ptr == end) {
        node->type = WOTBMOD_V3_YAML_INT;
        node->integer = integer;
        return true;
    }
    if (text.find_first_of(".eE") != std::string::npos) {
        char* number_end = nullptr;
        errno = 0;
        const double number =
            std::strtod(text.c_str(), &number_end);
        if (errno != ERANGE && number_end &&
            *number_end == '\0' && std::isfinite(number)) {
            node->type = WOTBMOD_V3_YAML_FLOAT;
            node->floating = number;
            return true;
        }
    }
    if (text.size() > WOTBMOD_V3_SETTING_TEXT_MAX * 4u) {
        if (error) *error = "YAML scalar length limit exceeded";
        return false;
    }
    node->type = WOTBMOD_V3_YAML_STRING;
    node->scalar = text;
    return true;
}

class StrictYamlParser {
public:
    StrictYamlParser(
        const std::vector<YamlLine>& lines,
        uint32_t max_depth,
        uint32_t max_nodes,
        YamlDocument* document)
        : lines_(lines),
          max_depth_(max_depth),
          max_nodes_(max_nodes),
          document_(document) {}

    bool Parse(std::string* error) {
        if (lines_.empty()) {
            YamlNode root;
            root.type = WOTBMOD_V3_YAML_NULL;
            document_->nodes.push_back(std::move(root));
            document_->root = 0;
            return true;
        }
        if (lines_[0].indent != 0) {
            SetErrorAt(0, "root YAML node must start at indent zero", error);
            return false;
        }
        size_t position = 0;
        if (!ParseBlock(
                &position, lines_[0].indent, 0,
                &document_->root, error)) {
            return false;
        }
        if (position != lines_.size()) {
            SetErrorAt(position, "unexpected YAML indentation", error);
            return false;
        }
        return true;
    }

private:
    WotbModV3YamlNodeId AddNode(
        YamlNode node,
        std::string* error,
        size_t line_index) {
        if (document_->nodes.size() >= max_nodes_) {
            SetErrorAt(line_index, "YAML node limit exceeded", error);
            return WOTBMOD_V3_YAML_INVALID_NODE;
        }
        document_->nodes.push_back(std::move(node));
        return static_cast<WotbModV3YamlNodeId>(
            document_->nodes.size() - 1u);
    }

    void SetErrorAt(
        size_t line_index,
        const char* message,
        std::string* error) const {
        if (!error) return;
        const uint32_t line =
            line_index < lines_.size()
            ? lines_[line_index].line_number : 0;
        *error = std::string(message) +
                 (line ? " at line " + std::to_string(line) : "");
    }

    bool ParseBlock(
        size_t* position,
        uint32_t indent,
        uint32_t depth,
        WotbModV3YamlNodeId* out_node,
        std::string* error) {
        if (!position || !out_node || *position >= lines_.size()) {
            return false;
        }
        if (depth > max_depth_) {
            SetErrorAt(*position, "YAML depth limit exceeded", error);
            return false;
        }
        if (lines_[*position].indent != indent) {
            SetErrorAt(*position, "inconsistent YAML indentation", error);
            return false;
        }
        const bool sequence =
            lines_[*position].text == "-" ||
            lines_[*position].text.rfind("- ", 0) == 0;
        return sequence
            ? ParseSequence(position, indent, depth, out_node, error)
            : ParseMap(position, indent, depth, out_node, error);
    }

    bool ParseNestedOrNull(
        size_t* position,
        uint32_t parent_indent,
        uint32_t depth,
        WotbModV3YamlNodeId* out_node,
        std::string* error) {
        if (*position < lines_.size() &&
            lines_[*position].indent > parent_indent) {
            return ParseBlock(
                position, lines_[*position].indent,
                depth + 1u, out_node, error);
        }
        YamlNode null_node;
        null_node.type = WOTBMOD_V3_YAML_NULL;
        *out_node = AddNode(
            std::move(null_node), error,
            *position == 0 ? 0 : *position - 1u);
        return *out_node != WOTBMOD_V3_YAML_INVALID_NODE;
    }

    bool ParseMapEntry(
        const std::string& text,
        size_t line_index,
        size_t* position,
        uint32_t indent,
        uint32_t depth,
        std::string* out_key,
        WotbModV3YamlNodeId* out_value,
        std::string* error) {
        const size_t colon = FindYamlColon(text);
        if (colon == std::string::npos) {
            SetErrorAt(line_index, "mapping entry is missing ':'", error);
            return false;
        }
        std::string key_text = Trim(text.substr(0, colon));
        std::string key;
        if (!key_text.empty() &&
            (key_text.front() == '"' ||
             key_text.front() == '\'')) {
            if (!ParseYamlQuotedString(key_text, &key)) {
                SetErrorAt(line_index, "invalid quoted mapping key", error);
                return false;
            }
        } else {
            key = key_text;
        }
        if (key.empty() || key.size() > 255u ||
            key.find('\0') != std::string::npos) {
            SetErrorAt(line_index, "invalid mapping key", error);
            return false;
        }
        const std::string value_text =
            Trim(text.substr(colon + 1u));
        if (value_text.empty()) {
            if (!ParseNestedOrNull(
                    position, indent, depth,
                    out_value, error)) {
                return false;
            }
        } else {
            YamlNode scalar;
            std::string scalar_error;
            if (!ParseYamlScalar(
                    value_text, &scalar, &scalar_error)) {
                SetErrorAt(line_index, scalar_error.c_str(), error);
                return false;
            }
            *out_value =
                AddNode(std::move(scalar), error, line_index);
            if (*out_value == WOTBMOD_V3_YAML_INVALID_NODE) {
                return false;
            }
        }
        *out_key = std::move(key);
        return true;
    }

    bool ParseMap(
        size_t* position,
        uint32_t indent,
        uint32_t depth,
        WotbModV3YamlNodeId* out_node,
        std::string* error) {
        YamlNode map;
        map.type = WOTBMOD_V3_YAML_MAP;
        const WotbModV3YamlNodeId map_id =
            AddNode(std::move(map), error, *position);
        if (map_id == WOTBMOD_V3_YAML_INVALID_NODE) return false;
        std::set<std::string> keys;
        while (*position < lines_.size() &&
               lines_[*position].indent == indent) {
            const YamlLine& line = lines_[*position];
            if (line.text == "-" ||
                line.text.rfind("- ", 0) == 0) {
                SetErrorAt(
                    *position,
                    "cannot mix sequence and mapping at one indentation level",
                    error);
                return false;
            }
            const size_t line_index = *position;
            const std::string text = line.text;
            ++(*position);
            std::string key;
            WotbModV3YamlNodeId value =
                WOTBMOD_V3_YAML_INVALID_NODE;
            if (!ParseMapEntry(
                    text, line_index, position, indent,
                    depth, &key, &value, error)) {
                return false;
            }
            if (!keys.insert(key).second) {
                SetErrorAt(line_index, "duplicate YAML mapping key", error);
                return false;
            }
            document_->nodes[map_id].map.emplace_back(
                std::move(key), value);
            if (*position < lines_.size() &&
                lines_[*position].indent > indent) {
                SetErrorAt(
                    *position,
                    "unexpected nested YAML block after scalar",
                    error);
                return false;
            }
        }
        *out_node = map_id;
        return true;
    }

    bool ParseSequence(
        size_t* position,
        uint32_t indent,
        uint32_t depth,
        WotbModV3YamlNodeId* out_node,
        std::string* error) {
        YamlNode sequence;
        sequence.type = WOTBMOD_V3_YAML_SEQUENCE;
        const WotbModV3YamlNodeId sequence_id =
            AddNode(std::move(sequence), error, *position);
        if (sequence_id == WOTBMOD_V3_YAML_INVALID_NODE) return false;
        while (*position < lines_.size() &&
               lines_[*position].indent == indent) {
            const YamlLine& line = lines_[*position];
            if (line.text != "-" &&
                line.text.rfind("- ", 0) != 0) {
                SetErrorAt(
                    *position,
                    "cannot mix mapping and sequence at one indentation level",
                    error);
                return false;
            }
            const size_t line_index = *position;
            const std::string item_text =
                line.text == "-" ? "" :
                Trim(line.text.substr(2u));
            ++(*position);
            WotbModV3YamlNodeId child =
                WOTBMOD_V3_YAML_INVALID_NODE;
            if (item_text.empty()) {
                if (!ParseNestedOrNull(
                        position, indent, depth,
                        &child, error)) {
                    return false;
                }
            } else if (FindYamlColon(item_text) !=
                       std::string::npos) {
                YamlNode map;
                map.type = WOTBMOD_V3_YAML_MAP;
                const WotbModV3YamlNodeId map_id =
                    AddNode(std::move(map), error, line_index);
                if (map_id == WOTBMOD_V3_YAML_INVALID_NODE)
                    return false;
                std::string key;
                WotbModV3YamlNodeId value =
                    WOTBMOD_V3_YAML_INVALID_NODE;
                if (!ParseMapEntry(
                        item_text, line_index, position,
                        indent, depth, &key, &value, error)) {
                    return false;
                }
                document_->nodes[map_id].map.emplace_back(
                    std::move(key), value);
                if (*position < lines_.size() &&
                    lines_[*position].indent > indent) {
                    const uint32_t continuation_indent =
                        lines_[*position].indent;
                    WotbModV3YamlNodeId continuation =
                        WOTBMOD_V3_YAML_INVALID_NODE;
                    if (!ParseBlock(
                            position, continuation_indent,
                            depth + 1u, &continuation, error) ||
                        document_->nodes[continuation].type !=
                            WOTBMOD_V3_YAML_MAP) {
                        SetErrorAt(
                            *position,
                            "sequence mapping continuation must be a map",
                            error);
                        return false;
                    }
                    std::set<std::string> keys;
                    for (const auto& entry :
                         document_->nodes[map_id].map) {
                        keys.insert(entry.first);
                    }
                    for (const auto& entry :
                         document_->nodes[continuation].map) {
                        if (!keys.insert(entry.first).second) {
                            SetErrorAt(
                                line_index,
                                "duplicate sequence mapping key",
                                error);
                            return false;
                        }
                        document_->nodes[map_id].map.push_back(entry);
                    }
                }
                child = map_id;
            } else {
                YamlNode scalar;
                std::string scalar_error;
                if (!ParseYamlScalar(
                        item_text, &scalar, &scalar_error)) {
                    SetErrorAt(
                        line_index, scalar_error.c_str(), error);
                    return false;
                }
                child = AddNode(
                    std::move(scalar), error, line_index);
                if (child == WOTBMOD_V3_YAML_INVALID_NODE)
                    return false;
            }
            document_->nodes[sequence_id].sequence.push_back(child);
            if (*position < lines_.size() &&
                lines_[*position].indent > indent) {
                SetErrorAt(
                    *position,
                    "unexpected nested YAML block after sequence scalar",
                    error);
                return false;
            }
        }
        *out_node = sequence_id;
        return true;
    }

    const std::vector<YamlLine>& lines_;
    uint32_t max_depth_;
    uint32_t max_nodes_;
    YamlDocument* document_;
};

WotbModV3Result YamlAccess(WotbModV3Handle mod) {
    return VfsAccess(mod);
}

WotbModV3Result ParseYamlData(
    WotbModV3Handle mod,
    const void* data,
    size_t size,
    const WotbModV3YamlLimits* requested_limits,
    WotbModV3Handle* out_document) {
    if (!data || !out_document) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML data or output is null");
    }
    uint32_t max_depth = 32u;
    uint32_t max_nodes = 10000u;
    uint64_t max_bytes = 1024u * 1024u;
    if (requested_limits) {
        if (!ValidStruct(
                requested_limits,
                requested_limits->struct_size,
                sizeof(WotbModV3YamlLimits)) ||
            requested_limits->max_depth == 0 ||
            requested_limits->max_depth > 64u ||
            requested_limits->max_nodes == 0 ||
            requested_limits->max_nodes > 100000u ||
            requested_limits->max_bytes == 0 ||
            requested_limits->max_bytes > 8u * 1024u * 1024u) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid YAML parser limits");
        }
        max_depth = requested_limits->max_depth;
        max_nodes = requested_limits->max_nodes;
        max_bytes = requested_limits->max_bytes;
    }
    if (size > max_bytes) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "YAML input exceeds configured byte limit");
    }
    const char* text = static_cast<const char*>(data);
    if (std::memchr(text, '\0', size) != nullptr) {
        return Fail(mod, WOTBMOD_V3_E_PARSE,
                    "YAML input contains an embedded NUL byte");
    }
    std::vector<YamlLine> lines;
    std::string error;
    if (!PrepareYamlLines(text, size, &lines, &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    std::unique_ptr<YamlDocument> document(new YamlDocument());
    StrictYamlParser parser(
        lines, max_depth, max_nodes, document.get());
    if (!parser.Parse(&error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        document.get(),
        DestroyObject<YamlDocument>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    document.release();
    *out_document = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result LoadNativeDavaYamlSnapshot(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_document) {
    constexpr uint32_t kMaxNativeYamlBytes = 8u * 1024u * 1024u;
    if (!uri || !out_document) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "native DAVA YAML snapshot request is invalid");
    }
    *out_document = WOTBMOD_V3_INVALID_HANDLE;
    if ((InstalledDavaNativeCapabilities() &
         WOTBMOD_DAVA_NATIVE_CAP_YAML) == 0u) {
        return NativeDavaFailure(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "native DAVA YAML parse");
    }

    std::vector<char> physical_path;
    WotbModV3Result result =
        ResolveNativeDavaPath(mod, uri, &physical_path);
    if (result != WOTBMOD_V3_OK) return result;

    WotbModDavaNativeToken native_document = 0u;
    result = InstalledDavaNativeYamlParseFile(
        mod, physical_path.data(), &native_document);
    if (result != WOTBMOD_V3_OK) {
        return NativeDavaFailure(
            mod, result, "native DAVA YAML parse");
    }

    auto finish_native = [&](WotbModV3Result operation_result) {
        const WotbModV3Result released =
            InstalledDavaNativeRelease(mod, native_document);
        if (operation_result != WOTBMOD_V3_OK) {
            return operation_result;
        }
        return released == WOTBMOD_V3_OK
            ? WOTBMOD_V3_OK
            : NativeDavaFailure(
                  mod, released, "native DAVA YAML release");
    };

    WotbModDavaNativeBuffer query = {};
    query.struct_size = sizeof(query);
    result = InstalledDavaNativeYamlExportUtf8(
        mod, native_document, &query);
    if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL || query.size == 0u ||
        query.size > kMaxNativeYamlBytes) {
        return finish_native(NativeDavaFailure(
            mod,
            query.size > kMaxNativeYamlBytes
                ? WOTBMOD_V3_E_LIMIT_REACHED
                : (result == WOTBMOD_V3_OK
                       ? WOTBMOD_V3_E_PLATFORM : result),
            "native DAVA YAML export sizing"));
    }

    std::vector<uint8_t> yaml(query.size);
    WotbModDavaNativeBuffer read = {};
    read.struct_size = sizeof(read);
    read.data = yaml.data();
    read.capacity = static_cast<uint32_t>(yaml.size());
    result = InstalledDavaNativeYamlExportUtf8(
        mod, native_document, &read);
    if (result != WOTBMOD_V3_OK || read.size != yaml.size()) {
        return finish_native(NativeDavaFailure(
            mod,
            result == WOTBMOD_V3_OK
                ? WOTBMOD_V3_E_PLATFORM : result,
            "native DAVA YAML export"));
    }

    result = finish_native(WOTBMOD_V3_OK);
    if (result != WOTBMOD_V3_OK) return result;
    return ParseYamlData(
        mod, yaml.data(), yaml.size(), nullptr, out_document);
}

WotbModV3Result WOTBMOD_V3_CALL YamlParse(
    WotbModV3Handle mod,
    const WotbModV3ConstBuffer* yaml_utf8,
    const WotbModV3YamlLimits* limits,
    WotbModV3Handle* out_document) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            yaml_utf8, yaml_utf8 ? yaml_utf8->struct_size : 0,
            sizeof(WotbModV3ConstBuffer)) ||
        !yaml_utf8->data || yaml_utf8->size == 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid YAML buffer");
    }
    return ParseYamlData(
        mod, yaml_utf8->data, yaml_utf8->size,
        limits, out_document);
}

WotbModV3Result WOTBMOD_V3_CALL YamlParseUri(
    WotbModV3Handle mod,
    const char* uri,
    const WotbModV3YamlLimits* limits,
    WotbModV3Handle* out_document) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    uint64_t max_bytes = 1024u * 1024u;
    if (limits) {
        if (!ValidStruct(
                limits, limits->struct_size,
                sizeof(WotbModV3YamlLimits))) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid YAML limits struct");
        }
        max_bytes = limits->max_bytes;
    }
    std::vector<uint8_t> bytes;
    WotbModV3Result read =
        ReadUri(mod, uri, max_bytes, &bytes);
    if (read != WOTBMOD_V3_OK) return read;
    return ParseYamlData(
        mod, bytes.data(), bytes.size(),
        limits, out_document);
}

WotbModV3Result GetYamlDocument(
    WotbModV3Handle mod,
    WotbModV3Handle document_handle,
    YamlDocument** out) {
    return GetObject(
        mod, document_handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::YamlDocument, out);
}

WotbModV3Result GetYamlNode(
    WotbModV3Handle mod,
    WotbModV3Handle document_handle,
    WotbModV3YamlNodeId node_id,
    YamlDocument** out_document,
    YamlNode** out_node) {
    YamlDocument* document = nullptr;
    WotbModV3Result found =
        GetYamlDocument(mod, document_handle, &document);
    if (found != WOTBMOD_V3_OK) return found;
    if (node_id >= document->nodes.size()) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML node id is out of range");
    }
    if (out_document) *out_document = document;
    if (out_node) *out_node = &document->nodes[node_id];
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetRoot(
    WotbModV3Handle mod,
    WotbModV3Handle document_handle,
    WotbModV3YamlNodeId* out_node) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_node) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                               "YAML root output is null");
    YamlDocument* document = nullptr;
    WotbModV3Result found =
        GetYamlDocument(mod, document_handle, &document);
    if (found == WOTBMOD_V3_OK) *out_node = document->root;
    return found;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetType(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    uint32_t* out_type) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_type) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                               "YAML type output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found == WOTBMOD_V3_OK) *out_type = value->type;
    return found;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetSize(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    uint32_t* out_size) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_size) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                               "YAML size output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type == WOTBMOD_V3_YAML_MAP) {
        *out_size = static_cast<uint32_t>(value->map.size());
    } else if (value->type == WOTBMOD_V3_YAML_SEQUENCE) {
        *out_size = static_cast<uint32_t>(value->sequence.size());
    } else {
        *out_size = 0;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL YamlMapGet(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    const char* key,
    WotbModV3YamlNodeId* out_child) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!key || !out_child) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML map key/output is null");
    }
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type != WOTBMOD_V3_YAML_MAP) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML node is not a map");
    }
    for (const auto& item : value->map) {
        if (item.first == key) {
            *out_child = item.second;
            return WOTBMOD_V3_OK;
        }
    }
    return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                "YAML mapping key was not found");
}

WotbModV3Result WOTBMOD_V3_CALL YamlSequenceGet(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    uint32_t index,
    WotbModV3YamlNodeId* out_child) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_child) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "YAML sequence output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type != WOTBMOD_V3_YAML_SEQUENCE ||
        index >= value->sequence.size()) {
        return Fail(
            mod,
            value->type == WOTBMOD_V3_YAML_SEQUENCE
                ? WOTBMOD_V3_E_NOT_FOUND
                : WOTBMOD_V3_E_INVALID_ARGUMENT,
            "YAML sequence index/type is invalid");
    }
    *out_child = value->sequence[index];
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetString(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type != WOTBMOD_V3_YAML_STRING) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML node is not a string");
    }
    return CopyOutString(
        mod, value->scalar, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetBool(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    uint32_t* out_value) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "YAML bool output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type != WOTBMOD_V3_YAML_BOOL) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML node is not a bool");
    }
    *out_value = value->boolean ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetInt(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    int64_t* out_value) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "YAML integer output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type != WOTBMOD_V3_YAML_INT) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "YAML node is not an integer");
    }
    *out_value = value->integer;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL YamlGetFloat(
    WotbModV3Handle mod,
    WotbModV3Handle document,
    WotbModV3YamlNodeId node,
    double* out_value) {
    WotbModV3Result access = YamlAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "YAML float output is null");
    YamlNode* value = nullptr;
    WotbModV3Result found =
        GetYamlNode(mod, document, node, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (value->type == WOTBMOD_V3_YAML_FLOAT) {
        *out_value = value->floating;
        return WOTBMOD_V3_OK;
    }
    if (value->type == WOTBMOD_V3_YAML_INT) {
        *out_value = static_cast<double>(value->integer);
        return WOTBMOD_V3_OK;
    }
    return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                "YAML node is not numeric");
}

}  // namespace

namespace {

WotbModV3Result CreatePortableWatch(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3ResourceHandle resource,
    WotbModV3Token* out_token);

struct ResourceObject final : TaggedObject {
    ResourceObject(
        WotbModV3Handle owner_value,
        const WotbModV3ResourceLoadDesc& desc_value)
        : TaggedObject(ObjectKind::Resource),
          owner(owner_value),
          desc(desc_value),
          uri(desc_value.uri),
          group(desc_value.group) {}
    ~ResourceObject() override;
    mutable std::mutex mutex;
    WotbModV3Handle owner;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3ResourceLoadDesc desc = {};
    std::string uri;
    std::string group;
    uint32_t type = WOTBMOD_V3_RESOURCE_BINARY;
    uint32_t state = WOTBMOD_V3_RESOURCE_READY;
    uint32_t backing = WOTBMOD_V3_RESOURCE_BACKING_NONE;
    std::vector<uint8_t> bytes;
    std::string sha256;
    std::string error;
    bool operation_in_progress = false;
};

struct ResourceAsyncWork {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3ResourceHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    ResourceObject* resource = nullptr;
};

std::mutex g_resource_mutex;
std::unordered_map<WotbModV3Handle, std::vector<ResourceObject*>>
    g_resources;
std::unordered_map<
    WotbModV3Handle,
    std::map<std::string, std::vector<WotbModV3Handle>>>
    g_resource_groups;

ResourceObject::~ResourceObject() {
    std::lock_guard<std::mutex> lock(g_resource_mutex);
    auto owner_resources = g_resources.find(owner);
    if (owner_resources != g_resources.end()) {
        auto& resources = owner_resources->second;
        resources.erase(
            std::remove(resources.begin(), resources.end(), this),
            resources.end());
    }
    auto owner_groups = g_resource_groups.find(owner);
    if (owner_groups != g_resource_groups.end()) {
        for (auto& item : owner_groups->second) {
            auto& handles = item.second;
            handles.erase(
                std::remove(handles.begin(), handles.end(), handle),
                handles.end());
        }
    }
}

uint32_t InferResourceType(const std::string& uri) {
    std::string extension =
        LowerAscii(fs::path(uri).extension().string());
    if (extension == ".txt" || extension == ".md" ||
        extension == ".csv" || extension == ".ini") {
        return WOTBMOD_V3_RESOURCE_TEXT;
    }
    if (extension == ".json") {
        return WOTBMOD_V3_RESOURCE_JSON;
    }
    if (extension == ".yaml" || extension == ".yml") {
        return WOTBMOD_V3_RESOURCE_YAML;
    }
    if (extension == ".png" || extension == ".jpg" ||
        extension == ".jpeg" || extension == ".dds" ||
        extension == ".webp" || extension == ".tga") {
        return WOTBMOD_V3_RESOURCE_IMAGE;
    }
    if (extension == ".ogg" || extension == ".wav" ||
        extension == ".mp3" || extension == ".flac") {
        return WOTBMOD_V3_RESOURCE_AUDIO;
    }
    if (extension == ".obj" || extension == ".gltf" ||
        extension == ".glb" || extension == ".fbx" ||
        extension == ".dae") {
        return WOTBMOD_V3_RESOURCE_MODEL;
    }
    return WOTBMOD_V3_RESOURCE_BINARY;
}

WotbModV3Result ResourcesAccess(WotbModV3Handle mod) {
    return VfsAccess(mod);
}

bool ValidResourceDesc(const WotbModV3ResourceLoadDesc* desc) {
    return ValidStruct(
               desc, desc ? desc->struct_size : 0,
               sizeof(WotbModV3ResourceLoadDesc)) &&
           FixedTerminated(desc->uri, sizeof(desc->uri)) &&
           FixedTerminated(desc->group, sizeof(desc->group)) &&
           FixedTerminated(
               desc->expected_sha256,
               sizeof(desc->expected_sha256)) &&
           desc->uri[0] != '\0' &&
           (desc->flags &
            ~static_cast<uint32_t>(
                WOTBMOD_V3_RESOURCE_LOAD_REQUIRE_NATIVE_CLIENT)) == 0 &&
           (desc->expected_type == 0 ||
            (desc->expected_type >= WOTBMOD_V3_RESOURCE_BINARY &&
             desc->expected_type <= WOTBMOD_V3_RESOURCE_JSON)) &&
           (desc->group[0] == '\0' ||
            IsAsciiIdentifier(
                desc->group, sizeof(desc->group), true)) &&
           (desc->expected_sha256[0] == '\0' ||
            IsSha256(desc->expected_sha256));
}

WotbModV3Result LoadResourceBytes(
    WotbModV3Handle mod,
    ResourceObject* resource,
    bool preserve_previous_on_failure = false) {
    if (!resource) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "resource object is null");
    }
    WotbModV3ResourceLoadDesc desc = {};
    std::string uri;
    std::vector<uint8_t> previous_bytes;
    std::string previous_hash;
    std::string previous_uri;
    std::string previous_error;
    uint32_t previous_type = WOTBMOD_V3_RESOURCE_BINARY;
    uint32_t previous_state = WOTBMOD_V3_RESOURCE_FAILED;
    uint32_t previous_backing =
        WOTBMOD_V3_RESOURCE_BACKING_NONE;
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        if (resource->operation_in_progress) {
            return Fail(
                mod,
                WOTBMOD_V3_E_BUSY,
                "resource load or reload is already in progress");
        }
        if (preserve_previous_on_failure) {
            previous_bytes = resource->bytes;
            previous_hash = resource->sha256;
            previous_uri = resource->uri;
            previous_error = resource->error;
            previous_type = resource->type;
            previous_state = resource->state;
            previous_backing = resource->backing;
        }
        resource->operation_in_progress = true;
        resource->state = WOTBMOD_V3_RESOURCE_LOADING;
        resource->backing = WOTBMOD_V3_RESOURCE_BACKING_NONE;
        resource->error.clear();
        desc = resource->desc;
        uri = resource->uri;
    }
    if ((desc.flags &
         WOTBMOD_V3_RESOURCE_LOAD_REQUIRE_NATIVE_CLIENT) != 0) {
        {
            std::lock_guard<std::mutex> lock(resource->mutex);
            if (preserve_previous_on_failure) {
                resource->bytes = std::move(previous_bytes);
                resource->sha256 = std::move(previous_hash);
                resource->uri = std::move(previous_uri);
                resource->error = std::move(previous_error);
                resource->type = previous_type;
                resource->state = previous_state;
                resource->backing = previous_backing;
            } else {
                resource->state = WOTBMOD_V3_RESOURCE_FAILED;
                resource->error =
                    "native DAVA resource backend is not bound";
            }
            resource->operation_in_progress = false;
        }
        return Fail(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "native DAVA resource backend is not bound");
    }
    std::string normalized;
    std::vector<uint8_t> bytes;
    const uint64_t limit =
        desc.max_bytes == 0
        ? kDefaultReadLimit : desc.max_bytes;
    WotbModV3Result read = ReadUri(
        mod, uri.c_str(), limit, &bytes, &normalized);
    if (read != WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(resource->mutex);
        if (preserve_previous_on_failure) {
            resource->bytes = std::move(previous_bytes);
            resource->sha256 = std::move(previous_hash);
            resource->uri = std::move(previous_uri);
            resource->error = std::move(previous_error);
            resource->type = previous_type;
            resource->state = previous_state;
            resource->backing = previous_backing;
        } else {
            resource->state = WOTBMOD_V3_RESOURCE_FAILED;
            resource->error = "VFS resource read failed";
        }
        resource->operation_in_progress = false;
        return read;
    }
    const uint32_t inferred = InferResourceType(normalized);
    if (desc.expected_type != 0 &&
        desc.expected_type != inferred) {
        {
            std::lock_guard<std::mutex> lock(resource->mutex);
            if (preserve_previous_on_failure) {
                resource->bytes = std::move(previous_bytes);
                resource->sha256 = std::move(previous_hash);
                resource->uri = std::move(previous_uri);
                resource->error = std::move(previous_error);
                resource->type = previous_type;
                resource->state = previous_state;
                resource->backing = previous_backing;
            } else {
                resource->state = WOTBMOD_V3_RESOURCE_FAILED;
                resource->error =
                    "resource type does not match descriptor";
            }
            resource->operation_in_progress = false;
        }
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "resource type does not match descriptor");
    }
    const std::string digest =
        Sha256Bytes(bytes.data(), bytes.size());
    if (desc.expected_sha256[0] != '\0' &&
        LowerAscii(desc.expected_sha256) != digest) {
        {
            std::lock_guard<std::mutex> lock(resource->mutex);
            if (preserve_previous_on_failure) {
                resource->bytes = std::move(previous_bytes);
                resource->sha256 = std::move(previous_hash);
                resource->uri = std::move(previous_uri);
                resource->error = std::move(previous_error);
                resource->type = previous_type;
                resource->state = previous_state;
                resource->backing = previous_backing;
            } else {
                resource->state = WOTBMOD_V3_RESOURCE_FAILED;
                resource->error = "resource SHA-256 mismatch";
            }
            resource->operation_in_progress = false;
        }
        return Fail(mod, WOTBMOD_V3_E_HASH_MISMATCH,
                    "resource SHA-256 mismatch");
    }
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        resource->uri = normalized;
        resource->type = desc.expected_type != 0
            ? desc.expected_type : inferred;
        resource->bytes = std::move(bytes);
        resource->sha256 = digest;
        resource->backing =
            WOTBMOD_V3_RESOURCE_BACKING_RAW_VFS_BYTES;
        resource->state = WOTBMOD_V3_RESOURCE_READY;
        resource->operation_in_progress = false;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result LoadResourceBytesGuarded(
    WotbModV3Handle mod,
    ResourceObject* resource,
    bool preserve_previous_on_failure = false) {
    std::vector<uint8_t> rollback_bytes;
    std::string rollback_hash;
    std::string rollback_uri;
    std::string rollback_error;
    uint32_t rollback_type = WOTBMOD_V3_RESOURCE_BINARY;
    uint32_t rollback_state = WOTBMOD_V3_RESOURCE_FAILED;
    uint32_t rollback_backing =
        WOTBMOD_V3_RESOURCE_BACKING_NONE;
    bool rollback_ready = false;
    try {
        if (resource && preserve_previous_on_failure) {
            std::lock_guard<std::mutex> lock(resource->mutex);
            rollback_bytes = resource->bytes;
            rollback_hash = resource->sha256;
            rollback_uri = resource->uri;
            rollback_error = resource->error;
            rollback_type = resource->type;
            rollback_state = resource->state;
            rollback_backing = resource->backing;
            rollback_ready = true;
        }
        return LoadResourceBytes(
            mod,
            resource,
            preserve_previous_on_failure);
    } catch (...) {
        if (resource) {
            std::lock_guard<std::mutex> lock(resource->mutex);
            if (preserve_previous_on_failure && rollback_ready) {
                resource->bytes = std::move(rollback_bytes);
                resource->sha256 = std::move(rollback_hash);
                resource->uri = std::move(rollback_uri);
                resource->error = std::move(rollback_error);
                resource->type = rollback_type;
                resource->state = rollback_state;
                resource->backing = rollback_backing;
                resource->operation_in_progress = false;
            } else if (resource->operation_in_progress) {
                resource->state = WOTBMOD_V3_RESOURCE_FAILED;
                resource->backing =
                    WOTBMOD_V3_RESOURCE_BACKING_NONE;
                resource->error =
                    "resource loading raised an exception";
                resource->operation_in_progress = false;
            }
        }
        return Fail(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "resource loading raised an exception");
    }
}

void CompleteResourceAsyncWork(
    ResourceAsyncWork* work,
    bool cancelled) {
    if (!work) return;
    if (cancelled && work->resource) {
        std::lock_guard<std::mutex> lock(work->resource->mutex);
        if (!work->resource->operation_in_progress &&
            work->resource->state == WOTBMOD_V3_RESOURCE_LOADING) {
            work->resource->state =
                WOTBMOD_V3_RESOURCE_CANCELLED;
            work->resource->backing =
                WOTBMOD_V3_RESOURCE_BACKING_NONE;
            work->resource->error = "resource load was cancelled";
        }
    }
    ReleaseOwnedHandle(work->owner, work->handle);
    delete work;
}

void ResourceAsyncInvoke(void* user_data) {
    ResourceAsyncWork* work =
        static_cast<ResourceAsyncWork*>(user_data);
    if (!work) return;
    LoadResourceBytesGuarded(work->owner, work->resource);
    CompleteResourceAsyncWork(work, false);
}

void ResourceAsyncCancel(void* user_data) {
    CompleteResourceAsyncWork(
        static_cast<ResourceAsyncWork*>(user_data),
        true);
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesLoad(
    WotbModV3Handle mod,
    const WotbModV3ResourceLoadDesc* desc,
    WotbModV3ResourceHandle* out_resource) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidResourceDesc(desc) || !out_resource) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid resource load descriptor");
    }
    std::unique_ptr<ResourceObject> resource(
        new (std::nothrow) ResourceObject(mod, *desc));
    if (!resource) {
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "resource allocation failed");
    }
    WotbModV3Result loaded =
        LoadResourceBytesGuarded(mod, resource.get());
    if (loaded != WOTBMOD_V3_OK) return loaded;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        resource.get(),
        DestroyObject<ResourceObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    resource->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        g_resources[mod].push_back(resource.get());
        if (!resource->group.empty()) {
            g_resource_groups[mod][resource->group].push_back(handle);
        }
    }
    resource.release();
    *out_resource = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesLoadAsync(
    WotbModV3Handle mod,
    const WotbModV3ResourceLoadDesc* desc,
    WotbModV3ResourceHandle* out_resource) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidResourceDesc(desc) || !out_resource) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid async resource descriptor");
    }
    *out_resource = WOTBMOD_V3_INVALID_HANDLE;
    std::unique_ptr<ResourceObject> resource(
        new (std::nothrow) ResourceObject(mod, *desc));
    if (!resource) {
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "async resource allocation failed");
    }
    resource->state = WOTBMOD_V3_RESOURCE_LOADING;

    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        resource.get(),
        DestroyObject<ResourceObject>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    resource->handle = handle;
    try {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        g_resources[mod].push_back(resource.get());
        if (!resource->group.empty()) {
            g_resource_groups[mod][resource->group].push_back(handle);
        }
    } catch (...) {
        resource.release();
        ReleaseOwnedHandle(mod, handle);
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "async resource registry allocation failed");
    }
    ResourceObject* published = resource.release();

    WotbModV3Result retained =
        RetainOwnedHandle(mod, handle);
    if (retained != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
        return retained;
    }
    std::unique_ptr<ResourceAsyncWork> work(
        new (std::nothrow) ResourceAsyncWork());
    if (!work) {
        ReleaseOwnedHandle(mod, handle);
        ReleaseOwnedHandle(mod, handle);
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "async resource work allocation failed");
    }
    work->owner = mod;
    work->handle = handle;
    work->resource = published;
    WotbModV3Result queued = EnqueueOwnedWorker(
        mod,
        &ResourceAsyncInvoke,
        &ResourceAsyncCancel,
        work.get());
    if (queued != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
        ReleaseOwnedHandle(mod, handle);
        return queued;
    }
    work.release();
    *out_resource = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetResource(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle handle,
    ResourceObject** out) {
    if (!out) {
        return Fail(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "resource output is null");
    }
    *out = nullptr;
    WotbModV3Result retained =
        RetainOwnedHandle(mod, handle);
    if (retained != WOTBMOD_V3_OK) return retained;
    WotbModV3Result found = GetObject(
        mod, handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::Resource, out);
    if (found != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
    }
    return found;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesGetInfo(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle handle,
    WotbModV3ResourceInfo* out_info) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_info, out_info ? out_info->struct_size : 0,
            sizeof(WotbModV3ResourceInfo))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid resource info output");
    }
    ResourceObject* resource = nullptr;
    WotbModV3Result found =
        GetResource(mod, handle, &resource);
    if (found != WOTBMOD_V3_OK) return found;
    bool copied = false;
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        out_info->type = resource->type;
        out_info->state = resource->state;
        out_info->progress =
            resource->state == WOTBMOD_V3_RESOURCE_READY ? 1.0f : 0.0f;
        out_info->backing = resource->backing;
        out_info->memory_bytes = resource->bytes.size();
        copied =
            CopyFixed(
                out_info->uri, sizeof(out_info->uri), resource->uri) &&
            CopyFixed(
                out_info->sha256, sizeof(out_info->sha256),
                resource->sha256) &&
            CopyFixed(
                out_info->error, sizeof(out_info->error),
                resource->error);
    }
    ReleaseOwnedHandle(mod, handle);
    if (!copied) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "resource info strings exceed ABI limits");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesCopyData(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle handle,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ResourceObject* resource = nullptr;
    WotbModV3Result found =
        GetResource(mod, handle, &resource);
    if (found != WOTBMOD_V3_OK) return found;
    std::vector<uint8_t> bytes;
    bool ready = false;
    try {
        std::lock_guard<std::mutex> lock(resource->mutex);
        ready =
            resource->state == WOTBMOD_V3_RESOURCE_READY;
        if (ready) {
            bytes = resource->bytes;
        }
    } catch (...) {
        ReleaseOwnedHandle(mod, handle);
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "resource data snapshot allocation failed");
    }
    ReleaseOwnedHandle(mod, handle);
    if (!ready) {
        return Fail(mod, WOTBMOD_V3_E_BUSY,
                    "resource is not ready");
    }
    return CopyOutBytes(
        mod,
        bytes.data(),
        bytes.size(),
        inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesRetain(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle resource) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ResourceObject* object = nullptr;
    WotbModV3Result found =
        GetResource(mod, resource, &object);
    if (found != WOTBMOD_V3_OK) return found;
    WotbModV3Result retained =
        RetainOwnedHandle(mod, resource);
    ReleaseOwnedHandle(mod, resource);
    return retained;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesRelease(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle resource) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ResourceObject* object = nullptr;
    WotbModV3Result found =
        GetResource(mod, resource, &object);
    if (found != WOTBMOD_V3_OK) return found;
    WotbModV3Result released =
        ReleaseOwnedHandle(mod, resource);
    WotbModV3Result lease_released =
        ReleaseOwnedHandle(mod, resource);
    return released != WOTBMOD_V3_OK ? released : lease_released;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesPreloadGroup(
    WotbModV3Handle mod,
    const char* group,
    const WotbModV3ResourceLoadDesc* resources,
    uint32_t resource_count) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!group ||
        !IsAsciiIdentifier(group, WOTBMOD_V3_MAX_ID, true) ||
        resource_count == 0 || resource_count > 256u ||
        !resources) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid resource preload group");
    }
    {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        auto owner = g_resource_groups.find(mod);
        if (owner != g_resource_groups.end() &&
            owner->second.find(group) != owner->second.end() &&
            !owner->second[group].empty()) {
            return Fail(mod, WOTBMOD_V3_E_ALREADY_EXISTS,
                        "resource group is already loaded");
        }
    }
    std::vector<WotbModV3Handle> loaded;
    for (uint32_t i = 0; i < resource_count; ++i) {
        WotbModV3ResourceLoadDesc desc = resources[i];
        if (!ValidResourceDesc(&desc) ||
            std::strlen(group) >= sizeof(desc.group)) {
            for (WotbModV3Handle handle : loaded) {
                ReleaseOwnedHandle(mod, handle);
            }
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid resource in preload group");
        }
        CopyFixed(desc.group, sizeof(desc.group), group);
        WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3Result result =
            ResourcesLoad(mod, &desc, &handle);
        if (result != WOTBMOD_V3_OK) {
            for (WotbModV3Handle existing : loaded) {
                ReleaseOwnedHandle(mod, existing);
            }
            return result;
        }
        loaded.push_back(handle);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesUnloadGroup(
    WotbModV3Handle mod,
    const char* group) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!group || !*group) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "resource group id is empty");
    }
    std::vector<WotbModV3Handle> handles;
    {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        auto owner = g_resource_groups.find(mod);
        if (owner == g_resource_groups.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "resource group was not found");
        }
        auto found = owner->second.find(group);
        if (found == owner->second.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "resource group was not found");
        }
        handles.swap(found->second);
        owner->second.erase(found);
    }
    WotbModV3Result first_error = WOTBMOD_V3_OK;
    for (WotbModV3Handle handle : handles) {
        WotbModV3Result released =
            ReleaseOwnedHandle(mod, handle);
        if (first_error == WOTBMOD_V3_OK &&
            released != WOTBMOD_V3_OK) {
            first_error = released;
        }
    }
    return first_error;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesReload(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle handle) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ResourceObject* resource = nullptr;
    WotbModV3Result found =
        GetResource(mod, handle, &resource);
    if (found != WOTBMOD_V3_OK) return found;
    WotbModV3Result loaded =
        LoadResourceBytesGuarded(mod, resource, true);
    ReleaseOwnedHandle(mod, handle);
    return loaded;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesWatch(
    WotbModV3Handle mod,
    WotbModV3ResourceHandle handle,
    WotbModV3Token* out_token) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_token) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "resource watch token output is null");
    }
    ResourceObject* resource = nullptr;
    WotbModV3Result found =
        GetResource(mod, handle, &resource);
    if (found != WOTBMOD_V3_OK) return found;
    std::string uri;
    bool ready = false;
    try {
        std::lock_guard<std::mutex> lock(resource->mutex);
        ready =
            resource->state == WOTBMOD_V3_RESOURCE_READY;
        if (ready) {
            uri = resource->uri;
        }
    } catch (...) {
        ReleaseOwnedHandle(mod, handle);
        return Fail(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "resource watch URI allocation failed");
    }
    if (!ready) {
        ReleaseOwnedHandle(mod, handle);
        return Fail(
            mod,
            WOTBMOD_V3_E_BUSY,
            "resource is not ready for watching");
    }
    WotbModV3Result watched = CreatePortableWatch(
        mod, uri.c_str(), handle, out_token);
    ReleaseOwnedHandle(mod, handle);
    return watched;
}

WotbModV3Result WOTBMOD_V3_CALL ResourcesGetMemoryUsage(
    WotbModV3Handle mod,
    uint64_t* out_bytes) {
    WotbModV3Result access = ResourcesAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_bytes) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "resource memory output is null");
    }
    uint64_t total = 0;
    std::lock_guard<std::mutex> lock(g_resource_mutex);
    auto found = g_resources.find(mod);
    if (found != g_resources.end()) {
        for (const ResourceObject* resource : found->second) {
            std::lock_guard<std::mutex> resource_lock(
                resource->mutex);
            total += resource->bytes.size();
        }
    }
    *out_bytes = total;
    return WOTBMOD_V3_OK;
}

}  // namespace

WotbModV3Result SnapshotOwnedResourcesForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsResourceSnapshot>* out_snapshots) {
    if (!out_snapshots) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "resource inspector snapshot output is required");
    }
    out_snapshots->clear();
    const WotbModV3Result access = CheckAccess(
        owner,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    try {
        const std::string filter = selector ? selector : "";
        std::lock_guard<std::mutex> registry_lock(g_resource_mutex);
        const auto found = g_resources.find(owner);
        if (found == g_resources.end()) return WOTBMOD_V3_OK;
        for (const ResourceObject* resource : found->second) {
            if (!resource) continue;
            std::lock_guard<std::mutex> resource_lock(resource->mutex);
            if (!filter.empty() && resource->uri != filter) continue;
            DevtoolsResourceSnapshot snapshot;
            snapshot.resource = resource->handle;
            snapshot.type = resource->type;
            snapshot.state = resource->state;
            snapshot.backing = resource->backing;
            snapshot.operation_in_progress =
                resource->operation_in_progress ? 1u : 0u;
            snapshot.memory_bytes =
                static_cast<uint64_t>(resource->bytes.size());
            snapshot.uri = resource->uri;
            snapshot.group = resource->group;
            snapshot.sha256 = resource->sha256;
            snapshot.error = resource->error;
            out_snapshots->push_back(std::move(snapshot));
        }
        std::stable_sort(
            out_snapshots->begin(),
            out_snapshots->end(),
            [](const DevtoolsResourceSnapshot& left,
               const DevtoolsResourceSnapshot& right) {
                if (left.uri != right.uri) {
                    return left.uri < right.uri;
                }
                return left.resource < right.resource;
            });
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        out_snapshots->clear();
        return SetError(
            owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate resource inspector snapshot");
    }
}

namespace {

struct ParsedUri {
    std::string scheme;
    std::string authority;
    std::vector<std::string> segments;
    std::string normalized;
};

bool IsUriSegmentSafe(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.size() > 255u) {
        return false;
    }
    for (unsigned char c : segment) {
        if (c < 0x20u || c == 0x7fu || c == '\\' ||
            c == ':' || c == '%' || c == '?' || c == '#') {
            return false;
        }
    }
    return true;
}

WotbModV3Result ParseUri(
    WotbModV3Handle mod,
    const char* uri,
    ParsedUri* out) {
    if (!uri || !out) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "URI or URI output is null");
    }
    const size_t length = std::strlen(uri);
    if (length == 0 || length >= WOTBMOD_V3_MAX_PATH) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "URI is empty or too long");
    }
    const std::string source(uri);
    const size_t separator = source.find("://");
    if (separator == std::string::npos || separator == 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "URI must contain a scheme and authority");
    }
    ParsedUri parsed;
    parsed.scheme = LowerAscii(source.substr(0, separator));
    if (parsed.scheme != "mod" && parsed.scheme != "data" &&
        parsed.scheme != "cache" && parsed.scheme != "game") {
        return Fail(mod, WOTBMOD_V3_E_NOT_SUPPORTED,
                    "unsupported VFS URI scheme");
    }
    const std::string remainder = source.substr(separator + 3u);
    if (remainder.empty() ||
        remainder.find('\\') != std::string::npos ||
        remainder.find('%') != std::string::npos) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "URI authority/path is malformed");
    }
    size_t start = 0;
    size_t slash = remainder.find('/');
    parsed.authority = LowerAscii(
        remainder.substr(
            0, slash == std::string::npos ?
                   remainder.size() : slash));
    if (!IsUriSegmentSafe(parsed.authority)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "URI authority is invalid");
    }
    if (parsed.scheme != "game") {
        const std::string own = ModNamespace(mod);
        if (parsed.authority == "self") {
            parsed.authority = own;
        } else if (parsed.authority != own) {
            return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                        "URI authority does not belong to the calling mod");
        }
    }
    if (slash != std::string::npos) {
        start = slash + 1u;
        while (start <= remainder.size()) {
            const size_t end = remainder.find('/', start);
            const std::string segment = remainder.substr(
                start,
                end == std::string::npos ?
                    remainder.size() - start : end - start);
            if (!IsUriSegmentSafe(segment)) {
                return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                            "URI contains an unsafe path segment");
            }
            parsed.segments.push_back(segment);
            if (end == std::string::npos) break;
            start = end + 1u;
        }
    }
    parsed.normalized =
        parsed.scheme + "://" + parsed.authority;
    for (const std::string& segment : parsed.segments) {
        parsed.normalized += "/";
        parsed.normalized += segment;
    }
    *out = std::move(parsed);
    return WOTBMOD_V3_OK;
}

WotbModV3Result NormalizeOwnedUri(
    WotbModV3Handle mod,
    const char* uri_text,
    std::string* out_normalized) {
    if (!out_normalized) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "normalized URI output is null");
    }
    ParsedUri parsed;
    WotbModV3Result result =
        ParseUri(mod, uri_text, &parsed);
    if (result == WOTBMOD_V3_OK) {
        *out_normalized = parsed.normalized;
    }
    return result;
}

bool IsSafeGameUri(
    WotbModV3Handle mod,
    const std::string& value) {
    ParsedUri parsed;
    return ParseUri(mod, value.c_str(), &parsed) ==
               WOTBMOD_V3_OK &&
           parsed.scheme == "game";
}

fs::path UriRelativePath(const ParsedUri& uri) {
    fs::path result;
    if (uri.scheme == "game") {
        result /= fs::path(uri.authority);
    }
    for (const std::string& segment : uri.segments) {
        result /= fs::path(segment);
    }
    return result;
}

struct VfsMount;
std::mutex g_vfs_mutex;
std::vector<VfsMount*> g_mounts;

struct VfsMount final : TaggedObject {
    explicit VfsMount(WotbModV3Handle owner_value)
        : TaggedObject(ObjectKind::VfsMount), owner(owner_value) {}
    ~VfsMount() override {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        g_mounts.erase(
            std::remove(g_mounts.begin(), g_mounts.end(), this),
            g_mounts.end());
    }
    WotbModV3Handle owner;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t kind = 0;
    int32_t priority = 0;
    std::string provider_id;
    fs::path physical_root;
    std::string target_uri;
    std::string source_uri;
};

struct VfsFile final : TaggedObject {
    VfsFile(std::string uri_value, std::vector<uint8_t> data_value)
        : TaggedObject(ObjectKind::VfsFile),
          uri(std::move(uri_value)),
          data(std::move(data_value)) {}
    std::string uri;
    std::vector<uint8_t> data;
};

WotbModV3Result VfsAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckNamedPermission(
        mod,
        "resources.mod",
        WOTBMOD_V3_PERMISSION_SAFE);
}

WotbModV3Result VfsOverlayAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckNamedPermission(
        mod,
        "resources.overlay.game",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

bool OverlayMatches(
    const std::string& target,
    const std::string& requested,
    std::string* suffix) {
    if (requested == target) {
        if (suffix) suffix->clear();
        return true;
    }
    if (requested.size() > target.size() &&
        requested.compare(0, target.size(), target) == 0 &&
        requested[target.size()] == '/') {
        if (suffix) {
            *suffix = requested.substr(target.size() + 1u);
        }
        return true;
    }
    return false;
}

WotbModV3Result ResolveBaseUri(
    WotbModV3Handle mod,
    const ParsedUri& uri,
    bool require_existing,
    fs::path* out_path,
    std::string* out_provider) {
    fs::path root;
    if (uri.scheme == "data") {
        root = fs::path(ModDataPath(mod));
    } else if (uri.scheme == "cache") {
        root = fs::path(ModCachePath(mod));
    } else if (uri.scheme == "game") {
        root = fs::path(GameDirectory() ? GameDirectory() : "");
    } else if (uri.scheme == "mod") {
        std::vector<VfsMount*> mounts;
        {
            std::lock_guard<std::mutex> lock(g_vfs_mutex);
            for (VfsMount* mount : g_mounts) {
                if (mount->owner == mod &&
                    mount->kind == WOTBMOD_V3_VFS_MOUNT_PACKAGE) {
                    mounts.push_back(mount);
                }
            }
        }
        std::sort(
            mounts.begin(), mounts.end(),
            [](const VfsMount* left, const VfsMount* right) {
                if (left->priority != right->priority)
                    return left->priority > right->priority;
                return left->provider_id < right->provider_id;
            });
        for (const VfsMount* mount : mounts) {
            fs::path candidate = mount->physical_root /
                                 UriRelativePath(uri);
            fs::path canonical;
            if (CanonicalWithin(
                    mount->physical_root, candidate,
                    require_existing, &canonical)) {
                if (!require_existing ||
                    fs::exists(canonical)) {
                    if (out_path) *out_path = canonical;
                    if (out_provider)
                        *out_provider = mount->provider_id;
                    return WOTBMOD_V3_OK;
                }
            }
        }
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "no mounted package provides the mod URI");
    }
    if (root.empty()) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "runtime did not provide a VFS root");
    }
    const fs::path candidate = root / UriRelativePath(uri);
    fs::path canonical;
    if (require_existing) {
        std::error_code exists_error;
        const bool exists = fs::exists(candidate, exists_error);
        if (!exists || exists_error) {
            /*
             * Preserve the traversal check for a missing leaf by resolving
             * its existing prefix, then report the semantic NOT_FOUND result
             * instead of misclassifying an absent file as a permission
             * violation.
             */
            if (!CanonicalWithin(root, candidate, false, &canonical)) {
                return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                            "resolved path escapes the VFS root");
            }
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "VFS path does not exist");
        }
    }
    if (!CanonicalWithin(
            root, candidate, require_existing, &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "resolved path escapes the VFS root");
    }
    if (require_existing && !fs::exists(canonical)) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "VFS path does not exist");
    }
    if (out_path) *out_path = canonical;
    if (out_provider) {
        *out_provider =
            uri.scheme == "game" ? "game.base" :
            uri.scheme + "." + ModNamespace(mod);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ResolveUriInternal(
    WotbModV3Handle mod,
    const char* uri_text,
    bool require_existing,
    fs::path* out_path,
    std::string* out_normalized,
    std::string* out_provider) {
    ParsedUri uri;
    WotbModV3Result parsed = ParseUri(mod, uri_text, &uri);
    if (parsed != WOTBMOD_V3_OK) return parsed;
    if (out_normalized) *out_normalized = uri.normalized;
    if (uri.scheme == "game") {
        std::vector<VfsMount*> overlays;
        {
            std::lock_guard<std::mutex> lock(g_vfs_mutex);
            for (VfsMount* mount : g_mounts) {
                if (mount->kind ==
                    WOTBMOD_V3_VFS_MOUNT_OVERLAY) {
                    std::string suffix;
                    if (OverlayMatches(
                            mount->target_uri,
                            uri.normalized,
                            &suffix)) {
                        overlays.push_back(mount);
                    }
                }
            }
        }
        std::sort(
            overlays.begin(), overlays.end(),
            [](const VfsMount* left, const VfsMount* right) {
                if (left->priority != right->priority)
                    return left->priority > right->priority;
                if (left->provider_id != right->provider_id)
                    return left->provider_id < right->provider_id;
                return left->owner < right->owner;
            });
        for (const VfsMount* overlay : overlays) {
            std::string suffix;
            OverlayMatches(
                overlay->target_uri, uri.normalized, &suffix);
            std::string source = overlay->source_uri;
            if (!suffix.empty()) source += "/" + suffix;
            ParsedUri source_uri;
            if (ParseUri(
                    overlay->owner, source.c_str(),
                    &source_uri) != WOTBMOD_V3_OK) {
                continue;
            }
            fs::path candidate;
            if (ResolveBaseUri(
                    overlay->owner, source_uri,
                    require_existing, &candidate,
                    nullptr) == WOTBMOD_V3_OK) {
                if (out_path) *out_path = candidate;
                if (out_provider)
                    *out_provider = overlay->provider_id;
                return WOTBMOD_V3_OK;
            }
        }
    }
    return ResolveBaseUri(
        mod, uri, require_existing, out_path, out_provider);
}

WotbModV3Result ReadUri(
    WotbModV3Handle mod,
    const char* uri,
    uint64_t max_bytes,
    std::vector<uint8_t>* out,
    std::string* out_normalized) {
    fs::path physical;
    WotbModV3Result resolved = ResolveUriInternal(
        mod, uri, true, &physical, out_normalized, nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    std::error_code ec;
    if (!fs::is_regular_file(physical, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS URI does not identify a regular file");
    }
    return ReadPhysicalFile(
        mod, physical,
        max_bytes == 0 ? kDefaultReadLimit : max_bytes,
        out);
}

WotbModV3Result ResolveWritableUri(
    WotbModV3Handle mod,
    const char* uri_text,
    fs::path* out_path) {
    ParsedUri uri;
    WotbModV3Result parsed = ParseUri(mod, uri_text, &uri);
    if (parsed != WOTBMOD_V3_OK) return parsed;
    if (uri.scheme != "data" && uri.scheme != "cache") {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "writes are restricted to data:// and cache://");
    }
    fs::path path;
    WotbModV3Result resolved =
        ResolveBaseUri(mod, uri, false, &path, nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    if (out_path) *out_path = path;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsGetNamespace(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    return CopyOutString(
        mod, ModNamespace(mod), buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL VfsNormalizeUri(
    WotbModV3Handle mod,
    const char* uri,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    ParsedUri parsed;
    WotbModV3Result result = ParseUri(mod, uri, &parsed);
    if (result != WOTBMOD_V3_OK) return result;
    return CopyOutString(
        mod, parsed.normalized, buffer, inout_size);
}

/*
 * The provider-id and mount-limit scan that both mount entry points run
 * before they take a handle, together with the g_vfs_mutex acquisition it
 * needs, lifted out of those callers. Three things about the shape are
 * deliberate and none of them is style.
 *
 * __try/__finally rather than std::lock_guard, for the reason measured on
 * this toolchain and written up at MountV3PackageDuringEnable in
 * wotb_mod_runtime.cpp: everything here compiles /EHsc, under which MSVC
 * runs no C++ destructor while unwinding to an __except. This region is
 * reachable from mod code running underneath exactly such a handler. A mod
 * calling set_mod_enabled(self, true) from its own callback reaches
 * HostSetModEnabled -> SetRecordEnabled -> MountV3PackageDuringEnable ->
 * EnsureV3PackageMounted -> MountVfsPackageForRuntime -> VfsMountPackage,
 * and every mod callback runs inside __try/__except(EXCEPTION_EXECUTE_
 * HANDLER) that marks the mod faulted and lets the thread carry on. The
 * loop below dereferences raw VfsMount pointers out of g_mounts, which is
 * where an access violation would land. A skipped lock_guard destructor
 * there would leave g_vfs_mutex -- a plain non-recursive std::mutex, so
 * there is no recursion count to leak, the whole lock is simply gone --
 * held forever by a thread that keeps running. Every later mod:// or
 * game:// resolution, mount, unmount and overlay in the process then
 * blocks on it, and the visible symptom is a hang minutes later on an
 * unrelated thread with nothing pointing back here. A termination handler
 * does run during that unwind, so it closes the path a destructor does
 * not.
 *
 * A separate function rather than a __try inside VfsMountPackage, because
 * under /EHsc a function that owns objects requiring unwinding cannot
 * contain __try at all (C2712), and both callers own a std::unique_ptr and
 * two fs::paths. Nothing in this frame has a destructor, which is also why
 * the loop below is indexed rather than a range-for.
 *
 * Fail() is called after the lock is dropped rather than inside the scan
 * as it used to be. Fail -> SetError takes g_state_mutex, and holding
 * g_vfs_mutex across that is a lock-ordering edge worth not having. The
 * codes, the messages and the order they are checked in are unchanged.
 *
 * What this does not close: the ten remaining g_vfs_mutex critical
 * sections in this file are the same class of hazard, untouched here. The
 * push_back sites in the two callers are the weakest of them -- they touch
 * only the vector, and any corruption bad enough to fault there would have
 * faulted in this scan first -- but that is an argument, not a fix.
 */
WotbModV3Result ReserveVfsProviderSlot(
    WotbModV3Handle mod,
    const std::string& provider_id) {
    WotbModV3Result scanned = WOTBMOD_V3_OK;
    g_vfs_mutex.lock();
    __try {
        uint32_t owned_count = 0u;
        for (size_t index = 0; index < g_mounts.size(); ++index) {
            const VfsMount* existing = g_mounts[index];
            if (existing->owner != mod) continue;
            ++owned_count;
            if (existing->provider_id == provider_id) {
                scanned = WOTBMOD_V3_E_ALREADY_EXISTS;
                break;
            }
        }
        if (scanned == WOTBMOD_V3_OK &&
            owned_count >= kMaxMountsPerMod) {
            scanned = WOTBMOD_V3_E_LIMIT_REACHED;
        }
    } __finally {
        g_vfs_mutex.unlock();
    }
    if (scanned == WOTBMOD_V3_E_ALREADY_EXISTS) {
        return Fail(mod, WOTBMOD_V3_E_ALREADY_EXISTS,
                    "VFS provider id already exists for mod");
    }
    if (scanned == WOTBMOD_V3_E_LIMIT_REACHED) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "VFS mount limit reached");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsMountPackage(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* physical_directory,
    int32_t priority,
    WotbModV3Handle* out_mount) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!provider_id || !physical_directory || !out_mount ||
        !IsAsciiIdentifier(
            provider_id, WOTBMOD_V3_MAX_ID, true) ||
        priority < -100000 || priority > 100000) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid package mount descriptor");
    }
    const fs::path requested(physical_directory);
    fs::path canonical;
    if (!IsAllowedOwnedPhysicalPath(
            mod, requested, true, &canonical)) {
        return Fail(mod, WOTBMOD_V3_E_PERMISSION_DENIED,
                    "package mount directory is outside owned roots");
    }
    std::error_code ec;
    if (!fs::is_directory(canonical, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "package mount source is not a directory");
    }
    std::unique_ptr<VfsMount> mount(new VfsMount(mod));
    mount->kind = WOTBMOD_V3_VFS_MOUNT_PACKAGE;
    mount->priority = priority;
    mount->provider_id = provider_id;
    mount->physical_root = canonical;
    const WotbModV3Result reserved =
        ReserveVfsProviderSlot(mod, mount->provider_id);
    if (reserved != WOTBMOD_V3_OK) return reserved;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        mount.get(),
        DestroyObject<VfsMount>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    mount->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        g_mounts.push_back(mount.get());
    }
    mount.release();
    *out_mount = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsMountOverlay(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* target_game_uri,
    const char* source_mod_uri,
    int32_t priority,
    WotbModV3Handle* out_mount) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsOverlayAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!provider_id || !target_game_uri || !source_mod_uri ||
        !out_mount ||
        !IsAsciiIdentifier(
            provider_id, WOTBMOD_V3_MAX_ID, true) ||
        priority < -100000 || priority > 100000) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid overlay mount descriptor");
    }
    ParsedUri target;
    ParsedUri source;
    WotbModV3Result parsed =
        ParseUri(mod, target_game_uri, &target);
    if (parsed != WOTBMOD_V3_OK) return parsed;
    parsed = ParseUri(mod, source_mod_uri, &source);
    if (parsed != WOTBMOD_V3_OK) return parsed;
    if (target.scheme != "game" ||
        (source.scheme != "mod" &&
         source.scheme != "data" &&
         source.scheme != "cache")) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "overlay target/source schemes are invalid");
    }
    fs::path source_path;
    if (ResolveBaseUri(
            mod, source, true, &source_path, nullptr) !=
        WOTBMOD_V3_OK) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "overlay source is not resolvable");
    }
    std::unique_ptr<VfsMount> mount(new VfsMount(mod));
    mount->kind = WOTBMOD_V3_VFS_MOUNT_OVERLAY;
    mount->priority = priority;
    mount->provider_id = provider_id;
    mount->target_uri = target.normalized;
    mount->source_uri = source.normalized;
    const WotbModV3Result reserved =
        ReserveVfsProviderSlot(mod, mount->provider_id);
    if (reserved != WOTBMOD_V3_OK) return reserved;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        mount.get(),
        DestroyObject<VfsMount>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    mount->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        g_mounts.push_back(mount.get());
    }
    mount.release();
    *out_mount = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsUnmount(
    WotbModV3Handle mod,
    WotbModV3Handle mount_handle) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    VfsMount* mount = nullptr;
    WotbModV3Result found = GetObject(
        mod, mount_handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::VfsMount, &mount);
    if (found != WOTBMOD_V3_OK) return found;
    if (mount->kind == WOTBMOD_V3_VFS_MOUNT_OVERLAY) {
        access = VfsOverlayAccess(mod);
        if (access != WOTBMOD_V3_OK) return access;
    }
    return ReleaseOwnedHandle(mod, mount_handle);
}

/*
 * VFS V2 - the four write routes.
 *
 * Every one of them takes BOTH gates: `VfsAccess` for the interface and
 * `VfsWriteAccess` for the act of writing. Two gates rather than one because
 * `resources.mod` is held by anything that reads a game file at all, and
 * creating files is not the same authority as reading them. A mod that only
 * reads keeps working with the permissions it already declared.
 */
WotbModV3Result VfsWriteAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckNamedPermission(
        mod,
        "resources.write.mod_data",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result ValidWriteBuffer(
    WotbModV3Handle mod,
    const WotbModV3ConstBuffer* data) {
    if (!ValidStruct(
            data, data ? data->struct_size : 0,
            sizeof(WotbModV3ConstBuffer)) ||
        (data->size != 0 && !data->data) ||
        data->size > WOTBMOD_V3_VFS_MAX_WRITE_BYTES) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid vfs write buffer");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsWriteFile(
    WotbModV3Handle mod,
    const char* uri,
    const WotbModV3ConstBuffer* data) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsWriteAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    const WotbModV3Result valid = ValidWriteBuffer(mod, data);
    if (valid != WOTBMOD_V3_OK) return valid;
    fs::path destination;
    const WotbModV3Result writable =
        ResolveWritableUri(mod, uri, &destination);
    if (writable != WOTBMOD_V3_OK) return writable;
    return AtomicWrite(
        mod,
        destination,
        static_cast<const uint8_t*>(data->data),
        data->size);
}

WotbModV3Result WOTBMOD_V3_CALL VfsAppendFile(
    WotbModV3Handle mod,
    const char* uri,
    const WotbModV3ConstBuffer* data) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsWriteAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    const WotbModV3Result valid = ValidWriteBuffer(mod, data);
    if (valid != WOTBMOD_V3_OK) return valid;
    fs::path destination;
    const WotbModV3Result writable =
        ResolveWritableUri(mod, uri, &destination);
    if (writable != WOTBMOD_V3_OK) return writable;
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot create the append destination directory");
    }
    /*
     * The ceiling is on the RESULT, not on the argument. Without it a caller
     * that appends in a loop is bounded by nothing this interface states, and
     * the per-call limit would be a fence with a gate in it.
     */
    ec.clear();
    const uintmax_t existing = fs::exists(destination, ec) && !ec
                                   ? fs::file_size(destination, ec)
                                   : 0u;
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot measure the append destination");
    }
    if (existing + data->size > WOTBMOD_V3_VFS_MAX_WRITE_BYTES) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "the appended file would exceed the write ceiling");
    }
    std::ofstream output(
        destination, std::ios::binary | std::ios::app);
    if (!output) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot open the append destination");
    }
    if (data->size != 0) {
        output.write(
            static_cast<const char*>(data->data),
            static_cast<std::streamsize>(data->size));
    }
    output.flush();
    if (!output) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot flush the appended bytes");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsCopyFile(
    WotbModV3Handle mod,
    const char* source_uri,
    const char* destination_uri) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsWriteAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    /*
     * The DESTINATION is resolved first, so a copy that could never be written
     * refuses before spending a 900 KB read on finding that out.
     */
    fs::path destination;
    const WotbModV3Result writable =
        ResolveWritableUri(mod, destination_uri, &destination);
    if (writable != WOTBMOD_V3_OK) return writable;
    std::vector<uint8_t> bytes;
    const WotbModV3Result read = ReadUri(
        mod,
        source_uri,
        WOTBMOD_V3_VFS_MAX_WRITE_BYTES,
        &bytes);
    if (read != WOTBMOD_V3_OK) return read;
    return AtomicWrite(
        mod, destination, bytes.data(), bytes.size());
}

WotbModV3Result WOTBMOD_V3_CALL VfsRemoveFile(
    WotbModV3Handle mod,
    const char* uri) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    access = VfsWriteAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    fs::path target;
    const WotbModV3Result writable =
        ResolveWritableUri(mod, uri, &target);
    if (writable != WOTBMOD_V3_OK) return writable;
    std::error_code ec;
    if (fs::is_directory(target, ec) && !ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "vfs.remove_file refuses a directory");
    }
    ec.clear();
    if (!fs::exists(target, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "there is no file at that URI");
    }
    ec.clear();
    if (!fs::remove(target, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "the file could not be removed");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsResolve(
    WotbModV3Handle mod,
    const char* uri,
    char* physical_path,
    uint32_t* inout_size) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    fs::path path;
    WotbModV3Result resolved = ResolveUriInternal(
        mod, uri, true, &path, nullptr, nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    return CopyOutString(
        mod, path.string(), physical_path, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL VfsOpen(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Handle* out_file) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_file) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS file output is null");
    }
    std::vector<uint8_t> bytes;
    std::string normalized;
    WotbModV3Result read =
        ReadUri(mod, uri, kDefaultReadLimit, &bytes, &normalized);
    if (read != WOTBMOD_V3_OK) return read;
    std::unique_ptr<VfsFile> file(
        new VfsFile(std::move(normalized), std::move(bytes)));
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        file.get(),
        DestroyObject<VfsFile>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    file.release();
    *out_file = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsRead(
    WotbModV3Handle mod,
    WotbModV3Handle file_handle,
    uint64_t offset,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    VfsFile* file = nullptr;
    WotbModV3Result found = GetObject(
        mod, file_handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::VfsFile, &file);
    if (found != WOTBMOD_V3_OK) return found;
    if (offset > file->data.size()) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS read offset exceeds file size");
    }
    return CopyOutBytes(
        mod,
        file->data.data() + static_cast<size_t>(offset),
        file->data.size() - static_cast<size_t>(offset),
        inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL VfsList(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3VfsListEntry* entries,
    uint32_t* inout_count) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!inout_count) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS list count pointer is null");
    }
    ParsedUri parsed;
    WotbModV3Result parsed_result = ParseUri(mod, uri, &parsed);
    if (parsed_result != WOTBMOD_V3_OK) return parsed_result;
    fs::path directory;
    WotbModV3Result resolved = ResolveUriInternal(
        mod, uri, true, &directory, nullptr, nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS list target is not a directory");
    }
    std::vector<WotbModV3VfsListEntry> listed;
    for (fs::directory_iterator iterator(directory, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (listed.size() >= kMaxVfsListEntries) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "VFS directory entry limit exceeded");
        }
        WotbModV3VfsListEntry entry = {};
        entry.struct_size = sizeof(entry);
        entry.api_version = WOTBMOD_V3_VFS_VERSION;
        entry.type = iterator->is_directory(ec)
            ? WOTBMOD_V3_VFS_ENTRY_DIRECTORY
            : WOTBMOD_V3_VFS_ENTRY_FILE;
        if (ec) break;
        if (entry.type == WOTBMOD_V3_VFS_ENTRY_FILE) {
            entry.size = iterator->file_size(ec);
            if (ec) break;
        }
        const std::string name =
            iterator->path().filename().string();
        const std::string child_uri =
            parsed.normalized + "/" + name;
        if (!CopyFixed(entry.name, sizeof(entry.name), name) ||
            !CopyFixed(entry.uri, sizeof(entry.uri), child_uri)) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "VFS list entry path exceeds ABI limits");
        }
        listed.push_back(entry);
    }
    if (ec) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "VFS directory enumeration failed");
    }
    std::sort(
        listed.begin(), listed.end(),
        [](const auto& left, const auto& right) {
            return std::strcmp(left.name, right.name) < 0;
        });
    const uint32_t required =
        static_cast<uint32_t>(listed.size());
    if (!entries || *inout_count < required) {
        *inout_count = required;
        return required == 0 ? WOTBMOD_V3_OK :
               WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::copy(listed.begin(), listed.end(), entries);
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

uint64_t FileTimeUnixMs(const fs::file_time_type& time) {
    const auto system_time =
        std::chrono::time_point_cast<std::chrono::milliseconds>(
            time - fs::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    return static_cast<uint64_t>(
        system_time.time_since_epoch().count());
}

WotbModV3Result WOTBMOD_V3_CALL VfsStat(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3VfsStat* out_stat) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            out_stat, out_stat ? out_stat->struct_size : 0,
            sizeof(WotbModV3VfsStat))) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid VFS stat output");
    }
    fs::path path;
    std::string normalized;
    std::string provider;
    WotbModV3Result resolved = ResolveUriInternal(
        mod, uri, true, &path, &normalized, &provider);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    std::error_code ec;
    const bool directory = fs::is_directory(path, ec);
    if (ec) return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot inspect VFS path");
    out_stat->type = directory
        ? WOTBMOD_V3_VFS_ENTRY_DIRECTORY
        : WOTBMOD_V3_VFS_ENTRY_FILE;
    out_stat->read_only = 1u;
    out_stat->size = directory ? 0u : fs::file_size(path, ec);
    if (ec) return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot query VFS file size");
    out_stat->modified_unix_ms =
        FileTimeUnixMs(fs::last_write_time(path, ec));
    if (ec) return Fail(mod, WOTBMOD_V3_E_IO,
                        "cannot query VFS modification time");
    if (!CopyFixed(
            out_stat->resolved_uri,
            sizeof(out_stat->resolved_uri), normalized) ||
        !CopyFixed(
            out_stat->provider,
            sizeof(out_stat->provider), provider)) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "VFS stat strings exceed ABI limits");
    }
    return WOTBMOD_V3_OK;
}

struct WatchFingerprint {
    bool exists = false;
    bool is_directory = false;
    bool hash_valid = false;
    uint64_t size = 0u;
    uint64_t modified_unix_ms = 0u;
    int64_t write_time_ticks = 0;
    std::string sha256;
};

enum class WatchCaptureResult {
    Ready,
    Deferred,
    Unavailable
};

enum class PortableWatchKind {
    Vfs,
    Resource
};

struct PortableWatchState {
    std::mutex mutex;
    std::atomic<bool> active{true};
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3ResourceHandle resource =
        WOTBMOD_V3_INVALID_HANDLE;
    PortableWatchKind kind = PortableWatchKind::Vfs;
    std::string uri;
    fs::path physical_path;
    WatchFingerprint fingerprint;
};

std::mutex g_watch_mutex;
std::vector<std::shared_ptr<PortableWatchState>> g_watches;
size_t g_watch_cursor = 0u;

bool HashWatchFile(
    const fs::path& path,
    std::string* out_hash) {
    if (!out_hash) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    Sha256 hash;
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hash.Update(buffer, static_cast<size_t>(count));
        }
    }
    if (!input.eof()) return false;
    *out_hash = hash.FinalHex();
    return true;
}

WatchCaptureResult CaptureWatchFingerprint(
    const fs::path& path,
    uint64_t* inout_hash_budget,
    WatchFingerprint* out) {
    if (!inout_hash_budget || !out) {
        return WatchCaptureResult::Unavailable;
    }
    std::error_code ec;
    WatchFingerprint value;
    value.exists = fs::exists(path, ec);
    if (ec) return WatchCaptureResult::Unavailable;
    if (!value.exists) {
        *out = std::move(value);
        return WatchCaptureResult::Ready;
    }
    value.is_directory = fs::is_directory(path, ec);
    if (ec) return WatchCaptureResult::Unavailable;
    const fs::file_time_type write_time =
        fs::last_write_time(path, ec);
    if (ec) return WatchCaptureResult::Unavailable;
    value.write_time_ticks = static_cast<int64_t>(
        write_time.time_since_epoch().count());
    value.modified_unix_ms = FileTimeUnixMs(write_time);
    if (value.is_directory) {
        *out = std::move(value);
        return WatchCaptureResult::Ready;
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        *out = std::move(value);
        return WatchCaptureResult::Ready;
    }
    value.size = fs::file_size(path, ec);
    if (ec) return WatchCaptureResult::Unavailable;
    if (value.size > kMaxWatchFileBytes) {
        *out = std::move(value);
        return WatchCaptureResult::Ready;
    }
    if (value.size > *inout_hash_budget) {
        return WatchCaptureResult::Deferred;
    }
    if (!HashWatchFile(path, &value.sha256)) {
        return WatchCaptureResult::Unavailable;
    }
    const uint64_t verified_size = fs::file_size(path, ec);
    if (ec) return WatchCaptureResult::Unavailable;
    const fs::file_time_type verified_time =
        fs::last_write_time(path, ec);
    if (ec ||
        verified_size != value.size ||
        verified_time != write_time) {
        return WatchCaptureResult::Unavailable;
    }
    value.hash_valid = true;
    *inout_hash_budget -= value.size;
    *out = std::move(value);
    return WatchCaptureResult::Ready;
}

bool WatchMetadataEqual(
    const WatchFingerprint& left,
    const WatchFingerprint& right) {
    return left.exists == right.exists &&
           left.is_directory == right.is_directory &&
           left.size == right.size &&
           left.write_time_ticks == right.write_time_ticks;
}

bool WatchFingerprintEqual(
    const WatchFingerprint& left,
    const WatchFingerprint& right) {
    if (!WatchMetadataEqual(left, right)) return false;
    if (!left.exists || left.is_directory) return true;
    if (left.hash_valid && right.hash_valid) {
        return left.sha256 == right.sha256;
    }
    return true;
}

uint32_t WatchChangeKind(
    const WatchFingerprint& previous,
    const WatchFingerprint& current) {
    if (!previous.exists && current.exists) {
        return WOTBMOD_V3_VFS_CHANGE_CREATED;
    }
    if (previous.exists && !current.exists) {
        return WOTBMOD_V3_VFS_CHANGE_DELETED;
    }
    return WOTBMOD_V3_VFS_CHANGE_MODIFIED;
}

struct PortableWatchHandle final : TaggedObject {
    explicit PortableWatchHandle(
        std::shared_ptr<PortableWatchState> value)
        : TaggedObject(ObjectKind::PortableWatch),
          state(std::move(value)) {}

    ~PortableWatchHandle() override {
        if (!state) return;
        state->active.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_watch_mutex);
            g_watches.erase(
                std::remove_if(
                    g_watches.begin(),
                    g_watches.end(),
                    [&](const std::shared_ptr<PortableWatchState>& item) {
                        return item.get() == state.get();
                    }),
                g_watches.end());
            if (g_watches.empty()) {
                g_watch_cursor = 0u;
            } else {
                g_watch_cursor %= g_watches.size();
            }
        }
        if (state->resource != WOTBMOD_V3_INVALID_HANDLE) {
            ReleaseOwnedHandle(state->owner, state->resource);
        }
    }

    std::shared_ptr<PortableWatchState> state;
};

WotbModV3Result CreatePortableWatch(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3ResourceHandle resource,
    WotbModV3Token* out_token) {
    if (!uri || !out_token) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid portable watch request");
    }
    *out_token = WOTBMOD_V3_INVALID_HANDLE;
    fs::path physical;
    std::string normalized;
    WotbModV3Result resolved = ResolveUriInternal(
        mod, uri, true, &physical, &normalized, nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;

    uint64_t initial_budget = kMaxWatchFileBytes;
    WatchFingerprint initial;
    const WatchCaptureResult captured =
        CaptureWatchFingerprint(
            physical, &initial_budget, &initial);
    if (captured != WatchCaptureResult::Ready) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "cannot capture the initial watch fingerprint");
    }

    {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        if (g_watches.size() >= kMaxWatchersGlobal) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "global portable watch limit reached");
        }
        const uint32_t owned = static_cast<uint32_t>(
            std::count_if(
                g_watches.begin(),
                g_watches.end(),
                [mod](const std::shared_ptr<PortableWatchState>& item) {
                    return item && item->owner == mod;
                }));
        if (owned >= kMaxWatchersPerMod) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "per-mod portable watch limit reached");
        }
    }

    if (resource != WOTBMOD_V3_INVALID_HANDLE) {
        const WotbModV3Result retained =
            RetainOwnedHandle(mod, resource);
        if (retained != WOTBMOD_V3_OK) return retained;
    }

    std::shared_ptr<PortableWatchState> state;
    try {
        state = std::make_shared<PortableWatchState>();
    } catch (...) {
        if (resource != WOTBMOD_V3_INVALID_HANDLE) {
            ReleaseOwnedHandle(mod, resource);
        }
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "portable watch allocation failed");
    }
    state->owner = mod;
    state->resource = resource;
    state->kind =
        resource == WOTBMOD_V3_INVALID_HANDLE
        ? PortableWatchKind::Vfs
        : PortableWatchKind::Resource;
    state->uri = std::move(normalized);
    state->physical_path = physical.lexically_normal();
    state->fingerprint = std::move(initial);

    std::unique_ptr<PortableWatchHandle> holder(
        new (std::nothrow) PortableWatchHandle(state));
    if (!holder) {
        if (resource != WOTBMOD_V3_INVALID_HANDLE) {
            ReleaseOwnedHandle(mod, resource);
        }
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "portable watch allocation failed");
    }
    WotbModV3Handle token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        holder.get(),
        DestroyObject<PortableWatchHandle>,
        &token);
    if (created != WOTBMOD_V3_OK) {
        return created;
    }
    state->token = token;
    bool watch_limit_reached = false;
    try {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        const uint32_t owned = static_cast<uint32_t>(
            std::count_if(
                g_watches.begin(),
                g_watches.end(),
                [mod](const std::shared_ptr<PortableWatchState>& item) {
                    return item && item->owner == mod;
                }));
        if (g_watches.size() >= kMaxWatchersGlobal ||
            owned >= kMaxWatchersPerMod) {
            watch_limit_reached = true;
        } else {
            g_watches.push_back(state);
        }
    } catch (...) {
        holder.release();
        ReleaseOwnedHandle(mod, token);
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "portable watch registry allocation failed");
    }
    if (watch_limit_reached) {
        holder.release();
        ReleaseOwnedHandle(mod, token);
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "portable watch limit reached");
    }
    holder.release();
    *out_token = token;
    return WOTBMOD_V3_OK;
}

void PublishVfsInvalidated(
    const PortableWatchState& watch,
    const WatchFingerprint& previous,
    const WatchFingerprint& current,
    uint32_t change_kind) {
    WotbModV3VfsWatchEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_VFS_WATCH_EVENT_VERSION;
    event.watch_token = watch.token;
    event.change_kind = change_kind;
    event.previous_exists = previous.exists ? 1u : 0u;
    event.exists = current.exists ? 1u : 0u;
    event.is_directory = current.exists
        ? (current.is_directory ? 1u : 0u)
        : (previous.is_directory ? 1u : 0u);
    event.previous_size = previous.size;
    event.size = current.size;
    event.previous_modified_unix_ms =
        previous.modified_unix_ms;
    event.modified_unix_ms = current.modified_unix_ms;
    CopyFixed(event.uri, sizeof(event.uri), watch.uri);
    PublishSystemEvent(
        WOTBMOD_V3_EVENT_VFS_INVALIDATED,
        &event,
        sizeof(event),
        0u);
}

bool PublishResourceWatchEvent(
    const PortableWatchState& watch,
    uint32_t change_kind,
    const char* topic) {
    ResourceObject* resource = nullptr;
    if (GetResource(
            watch.owner, watch.resource, &resource) !=
        WOTBMOD_V3_OK) {
        return false;
    }
    WotbModV3ResourceWatchEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version =
        WOTBMOD_V3_RESOURCE_WATCH_EVENT_VERSION;
    event.watch_token = watch.token;
    event.resource = watch.resource;
    event.change_kind = change_kind;
    bool copied = false;
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        event.type = resource->type;
        event.state = resource->state;
        event.memory_bytes = resource->bytes.size();
        copied =
            CopyFixed(
                event.uri, sizeof(event.uri), resource->uri) &&
            CopyFixed(
                event.sha256, sizeof(event.sha256),
                resource->sha256);
    }
    ReleaseOwnedHandle(watch.owner, watch.resource);
    if (!copied) {
        return false;
    }
    return PublishSystemEvent(
               topic, &event, sizeof(event), 0u) ==
           WOTBMOD_V3_OK;
}

void ProcessPortableWatch(
    const std::shared_ptr<PortableWatchState>& watch,
    uint64_t* inout_hash_budget,
    bool* out_deferred) {
    if (out_deferred) *out_deferred = false;
    if (!watch ||
        !watch->active.load(std::memory_order_acquire)) {
        return;
    }

    fs::path physical;
    WatchFingerprint previous;
    {
        std::lock_guard<std::mutex> lock(watch->mutex);
        if (!watch->active.load(std::memory_order_acquire)) {
            return;
        }
        physical = watch->physical_path;
        previous = watch->fingerprint;
    }

    WatchFingerprint current;
    const WatchCaptureResult captured =
        CaptureWatchFingerprint(
            physical, inout_hash_budget, &current);
    if (captured == WatchCaptureResult::Deferred) {
        if (out_deferred) *out_deferred = true;
        return;
    }
    if (captured != WatchCaptureResult::Ready) return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(watch->mutex);
        if (!watch->active.load(std::memory_order_acquire)) {
            return;
        }
        previous = watch->fingerprint;
        changed = !WatchFingerprintEqual(previous, current);
        if (changed ||
            (!previous.hash_valid && current.hash_valid)) {
            watch->fingerprint = current;
        }
    }
    if (!changed ||
        !watch->active.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t change_kind =
        WatchChangeKind(previous, current);
    if (watch->kind == PortableWatchKind::Vfs) {
        PublishVfsInvalidated(
            *watch, previous, current, change_kind);
        return;
    }

    if (RetainOwnedHandle(
            watch->owner, watch->resource) != WOTBMOD_V3_OK) {
        return;
    }
    if (watch->active.load(std::memory_order_acquire)) {
        PublishResourceWatchEvent(
            *watch,
            change_kind,
            WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED);
    }
    WotbModV3Result reloaded = WOTBMOD_V3_E_CANCELLED;
    if (watch->active.load(std::memory_order_acquire)) {
        reloaded = ResourcesReload(
            watch->owner, watch->resource);
    }
    if (reloaded == WOTBMOD_V3_OK &&
        watch->active.load(std::memory_order_acquire)) {
        PublishResourceWatchEvent(
            *watch,
            change_kind,
            WOTBMOD_V3_EVENT_RESOURCE_RELOADED);
    }
    ReleaseOwnedHandle(watch->owner, watch->resource);
}

void PumpNativeInput(uint64_t frame_index);
void StopInputOwner(WotbModV3Handle owner);

void DataServicesFramePump(uint64_t frame_index, double) {
    PumpNativeInput(frame_index);
    std::vector<std::shared_ptr<PortableWatchState>> watches;
    size_t start = 0u;
    {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        if (g_watches.empty()) return;
        watches = g_watches;
        start = g_watch_cursor % watches.size();
    }

    uint64_t hash_budget = kMaxWatchHashBytesPerFrame;
    size_t processed = 0u;
    const size_t scan_count = std::min<size_t>(
        watches.size(), kMaxWatchScansPerFrame);
    for (; processed < scan_count; ++processed) {
        const size_t index =
            (start + processed) % watches.size();
        bool deferred = false;
        ProcessPortableWatch(
            watches[index], &hash_budget, &deferred);
        if (deferred) break;
    }
    {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        if (!g_watches.empty()) {
            g_watch_cursor =
                (start + processed) % g_watches.size();
        } else {
            g_watch_cursor = 0u;
        }
    }
}

void DataServicesOwnerStopping(WotbModV3Handle owner) {
    StopInputOwner(owner);
    std::vector<std::shared_ptr<PortableWatchState>> watches;
    {
        std::lock_guard<std::mutex> lock(g_watch_mutex);
        watches = g_watches;
    }
    for (const auto& watch : watches) {
        if (watch && watch->owner == owner) {
            watch->active.store(false, std::memory_order_release);
        }
    }
}

WotbModV3Result WOTBMOD_V3_CALL VfsWatch(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3Token* out_token) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!uri || !out_token) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid VFS watch request");
    }
    return CreatePortableWatch(
        mod,
        uri,
        WOTBMOD_V3_INVALID_HANDLE,
        out_token);
}

std::vector<VfsMount*> ProvidersForUri(
    const std::string& normalized) {
    std::vector<VfsMount*> providers;
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    for (VfsMount* mount : g_mounts) {
        if (mount->kind != WOTBMOD_V3_VFS_MOUNT_OVERLAY) continue;
        if (OverlayMatches(
                mount->target_uri, normalized, nullptr)) {
            providers.push_back(mount);
        }
    }
    std::sort(
        providers.begin(), providers.end(),
        [](const VfsMount* left, const VfsMount* right) {
            if (left->priority != right->priority)
                return left->priority > right->priority;
            if (left->provider_id != right->provider_id)
                return left->provider_id < right->provider_id;
            return left->owner < right->owner;
        });
    return providers;
}

WotbModV3Result WOTBMOD_V3_CALL VfsGetProviders(
    WotbModV3Handle mod,
    const char* uri,
    WotbModV3VfsProvider* providers,
    uint32_t* inout_count) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!inout_count) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "provider count pointer is null");
    }
    ParsedUri parsed;
    WotbModV3Result result = ParseUri(mod, uri, &parsed);
    if (result != WOTBMOD_V3_OK) return result;
    std::vector<VfsMount*> matches =
        parsed.scheme == "game"
        ? ProvidersForUri(parsed.normalized)
        : std::vector<VfsMount*>();
    const uint32_t required =
        static_cast<uint32_t>(matches.size());
    if (!providers || *inout_count < required) {
        *inout_count = required;
        return required == 0 ? WOTBMOD_V3_OK :
               WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    for (uint32_t i = 0; i < required; ++i) {
        WotbModV3VfsProvider provider = {};
        provider.struct_size = sizeof(provider);
        provider.api_version = WOTBMOD_V3_VFS_VERSION;
        provider.mount = matches[i]->handle;
        provider.owner_mod = matches[i]->owner;
        provider.priority = matches[i]->priority;
        provider.kind = matches[i]->kind;
        CopyFixed(
            provider.provider_id,
            sizeof(provider.provider_id),
            matches[i]->provider_id);
        CopyFixed(
            provider.target_uri,
            sizeof(provider.target_uri),
            matches[i]->target_uri);
        providers[i] = provider;
    }
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsSetProviderPriority(
    WotbModV3Handle mod,
    WotbModV3Handle mount_handle,
    int32_t priority) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (priority < -100000 || priority > 100000) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS provider priority is outside bounds");
    }
    VfsMount* mount = nullptr;
    WotbModV3Result found = GetObject(
        mod, mount_handle, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::VfsMount, &mount);
    if (found != WOTBMOD_V3_OK) return found;
    if (mount->kind == WOTBMOD_V3_VFS_MOUNT_OVERLAY) {
        access = VfsOverlayAccess(mod);
        if (access != WOTBMOD_V3_OK) return access;
    }
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    mount->priority = priority;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VfsGetConflicts(
    WotbModV3Handle mod,
    WotbModV3VfsConflict* conflicts,
    uint32_t* inout_count) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!inout_count) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "VFS conflict count pointer is null");
    }
    std::map<std::string, uint32_t> counts;
    {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        for (const VfsMount* mount : g_mounts) {
            if (mount->kind ==
                WOTBMOD_V3_VFS_MOUNT_OVERLAY) {
                ++counts[mount->target_uri];
            }
        }
    }
    std::vector<WotbModV3VfsConflict> found;
    for (const auto& item : counts) {
        if (item.second < 2u) continue;
        WotbModV3VfsConflict conflict = {};
        conflict.struct_size = sizeof(conflict);
        conflict.api_version = WOTBMOD_V3_VFS_VERSION;
        conflict.provider_count = item.second;
        CopyFixed(conflict.uri, sizeof(conflict.uri), item.first);
        found.push_back(conflict);
    }
    const uint32_t required =
        static_cast<uint32_t>(found.size());
    if (!conflicts || *inout_count < required) {
        *inout_count = required;
        return required == 0 ? WOTBMOD_V3_OK :
               WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::copy(found.begin(), found.end(), conflicts);
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

}  // namespace

WotbModV3Result MountVfsPackageForRuntime(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* physical_directory,
    int32_t priority,
    WotbModV3Handle* out_mount) {
    return VfsMountPackage(
        mod,
        provider_id,
        physical_directory,
        priority,
        out_mount);
}

WotbModV3Result MountVfsOverlayForRuntime(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* target_game_uri,
    const char* source_mod_uri,
    int32_t priority,
    WotbModV3Handle* out_mount) {
    return VfsMountOverlay(
        mod,
        provider_id,
        target_game_uri,
        source_mod_uri,
        priority,
        out_mount);
}

WotbModV3Result UnmountVfsForRuntime(
    WotbModV3Handle mod,
    WotbModV3Handle mount) {
    return VfsUnmount(mod, mount);
}

/* 2026-09-05: the physical file behind a mod:// or game:// URI for a caller
 * that already holds a stronger grant (the stock HUD overlays run under
 * gameplay.tweak.hud); the URI still has to name the mod's own namespace and
 * an existing file, only the VFS named permission is not asked for. */
WotbModV3Result ResolveUriPhysicalForOwner(
    WotbModV3Handle mod,
    const char* uri,
    std::string* out_path) {
    if (!out_path) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out_path->clear();
    fs::path path;
    const WotbModV3Result resolved = ResolveUriInternal(
        mod,
        uri,
        true,
        &path,
        nullptr,
        nullptr);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    *out_path = path.u8string();
    return WOTBMOD_V3_OK;
}

WotbModV3Result ResolveVfsUriPhysical(
    WotbModV3Handle mod,
    const char* uri,
    char* physical_path,
    uint32_t* inout_size) {
    WotbModV3Result access = VfsAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!inout_size) {
        return Fail(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "VFS physical path size pointer is null");
    }
    fs::path path;
    WotbModV3Result resolved = ResolveUriInternal(
        mod,
        uri,
        true,
        &path,
        nullptr,
        nullptr);
    if (resolved != WOTBMOD_V3_OK) {
        return resolved;
    }
    return CopyOutString(
        mod,
        path.u8string(),
        physical_path,
        inout_size);
}

WotbModV3Result ResolveActiveGameOverlayPath(
    const char* requested_path,
    char* physical_path,
    uint32_t* inout_size) {
    if (!requested_path || !inout_size) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const char* relative = requested_path;
    if (_strnicmp(relative, "~res:/", 6u) == 0) {
        relative += 6u;
    } else if (_strnicmp(relative, "~res:\\", 6u) == 0) {
        relative += 6u;
    } else if (_strnicmp(relative, "game://", 7u) == 0) {
        relative = nullptr;
    } else {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    std::string game_uri;
    if (!relative) {
        game_uri = requested_path;
    } else {
        std::string normalized_relative(relative);
        std::replace(
            normalized_relative.begin(),
            normalized_relative.end(),
            '\\',
            '/');
        while (!normalized_relative.empty() &&
               normalized_relative.front() == '/') {
            normalized_relative.erase(
                normalized_relative.begin());
        }
        const size_t separator =
            normalized_relative.find('/');
        if (normalized_relative.empty() ||
            separator == 0u ||
            normalized_relative.find("..") !=
                std::string::npos ||
            normalized_relative.find(':') !=
                std::string::npos) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        game_uri = "game://";
        game_uri += normalized_relative;
    }

    ParsedUri parsed;
    const WotbModV3Result parsed_result =
        ParseUri(
            WOTBMOD_V3_INVALID_HANDLE,
            game_uri.c_str(),
            &parsed);
    if (parsed_result != WOTBMOD_V3_OK ||
        parsed.scheme != "game") {
        return parsed_result;
    }

    struct OverlaySnapshot {
        WotbModV3Handle owner;
        int32_t priority;
        std::string provider;
        std::string target;
        std::string source;
    };
    std::vector<OverlaySnapshot> overlays;
    {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        for (const VfsMount* mount : g_mounts) {
            if (mount->kind !=
                WOTBMOD_V3_VFS_MOUNT_OVERLAY ||
                parsed.normalized.size() <
                    mount->target_uri.size() ||
                _strnicmp(
                    parsed.normalized.c_str(),
                    mount->target_uri.c_str(),
                    mount->target_uri.size()) != 0 ||
                (parsed.normalized.size() >
                     mount->target_uri.size() &&
                 parsed.normalized[
                     mount->target_uri.size()] != '/')) {
                continue;
            }
            overlays.push_back(
                {mount->owner,
                 mount->priority,
                 mount->provider_id,
                 mount->target_uri,
                 mount->source_uri});
        }
    }
    std::sort(
        overlays.begin(),
        overlays.end(),
        [](const OverlaySnapshot& left,
           const OverlaySnapshot& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            if (left.provider != right.provider) {
                return left.provider < right.provider;
            }
            return left.owner < right.owner;
        });
    for (const OverlaySnapshot& overlay : overlays) {
        std::string source = overlay.source;
        if (parsed.normalized.size() > overlay.target.size()) {
            source += "/";
            source += parsed.normalized.substr(
                overlay.target.size() + 1u);
        }
        ParsedUri source_uri;
        if (ParseUri(
                overlay.owner,
                source.c_str(),
                &source_uri) != WOTBMOD_V3_OK) {
            continue;
        }
        fs::path candidate;
        if (ResolveBaseUri(
                overlay.owner,
                source_uri,
                true,
                &candidate,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        const std::string result = candidate.u8string();
        if (result.size() + 1u >
            std::numeric_limits<uint32_t>::max()) {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        const uint32_t required =
            static_cast<uint32_t>(result.size() + 1u);
        if (!physical_path || *inout_size < required) {
            *inout_size = required;
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        }
        std::memcpy(
            physical_path,
            result.c_str(),
            required);
        *inout_size = required;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

namespace {

struct InputSubscription;
struct InputAction;
std::mutex g_input_mutex;
std::unordered_map<WotbModV3Handle, std::vector<InputAction*>>
    g_actions;

struct InputAction final : TaggedObject {
    InputAction(
        WotbModV3Handle owner_value,
        const WotbModV3InputActionDesc& desc)
        : TaggedObject(ObjectKind::InputAction),
          owner(owner_value),
          value_type(desc.value_type),
          contexts(desc.contexts),
          id(desc.id),
          display_name(desc.display_name),
          description(desc.description) {}
    ~InputAction() override {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        auto found = g_actions.find(owner);
        if (found == g_actions.end()) return;
        auto& actions = found->second;
        actions.erase(
            std::remove(actions.begin(), actions.end(), this),
            actions.end());
    }
    WotbModV3Handle owner;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t value_type;
    uint64_t contexts;
    std::string id;
    std::string display_name;
    std::string description;
    std::vector<WotbModV3InputBinding> bindings;
    std::vector<InputSubscription*> subscriptions;
    float value = 0.0f;
    uint64_t pressed_frame = 0u;
    uint64_t value_frame = 0u;
    bool down = false;
};

struct InputSubscription final : TaggedObject {
    InputSubscription(
        WotbModV3Handle owner_value,
        InputAction* action_value,
        WotbModV3InputActionCallback callback_value,
        void* user_value)
        : TaggedObject(ObjectKind::InputSubscription),
          owner(owner_value),
          action(action_value),
          callback(callback_value),
          user_data(user_value) {}
    ~InputSubscription() override {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        auto found = g_actions.find(owner);
        if (found == g_actions.end()) return;
        for (InputAction* candidate : found->second) {
            if (candidate == action) {
                auto& subscriptions = candidate->subscriptions;
                subscriptions.erase(
                    std::remove(
                        subscriptions.begin(),
                        subscriptions.end(),
                        this),
                    subscriptions.end());
                break;
            }
        }
    }
    WotbModV3Handle owner;
    InputAction* action;
    WotbModV3InputActionCallback callback;
    void* user_data;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
};

struct NativeInputEvent {
    uint32_t kind = 0u;
    uint32_t device = 0u;
    uint32_t code = 0u;
    uint32_t modifiers = 0u;
    float value = 0.0f;
    uint32_t down = 0u;
};

struct InputCaptureState {
    uint64_t contexts = 0u;
    WotbModV3InputBinding binding = {};
    bool captured = false;
};

struct PendingInputCallback {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle action = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3InputActionCallback callback = nullptr;
    void* user_data = nullptr;
    float value = 0.0f;
    uint32_t pressed = 0u;
    bool coalescible = false;
};

std::mutex g_native_input_mutex;
std::deque<NativeInputEvent> g_native_input_events;
std::unordered_map<WotbModV3Handle, InputCaptureState>
    g_input_captures;

std::unordered_map<
    WotbModV3Handle,
    std::map<std::string, std::vector<WotbModV3InputBinding>>>
    g_saved_bindings;
std::set<WotbModV3Handle> g_loaded_bindings;

bool ValidInputBinding(const WotbModV3InputBinding& binding) {
    return ValidStruct(
               &binding, binding.struct_size,
               sizeof(WotbModV3InputBinding)) &&
           binding.device >= WOTBMOD_V3_INPUT_DEVICE_KEYBOARD &&
           binding.device <= WOTBMOD_V3_INPUT_DEVICE_TOUCH &&
           binding.code != 0 &&
           (binding.modifiers &
            ~(WOTBMOD_V3_INPUT_MOD_SHIFT |
              WOTBMOD_V3_INPUT_MOD_CONTROL |
              WOTBMOD_V3_INPUT_MOD_ALT |
              WOTBMOD_V3_INPUT_MOD_META)) == 0 &&
           std::isfinite(binding.scale) &&
           std::fabs(binding.scale) <= 100.0f;
}

bool SameInputChord(
    const WotbModV3InputBinding& left,
    const WotbModV3InputBinding& right) {
    return left.device == right.device &&
           left.code == right.code &&
           left.modifiers == right.modifiers;
}

fs::path InputBindingsFile(WotbModV3Handle mod) {
    return fs::path(ModConfigPath(mod)) / "input.bindings.v3.bin";
}

void LoadInputBindingsLocked(WotbModV3Handle mod) {
    if (!g_loaded_bindings.insert(mod).second) {
        return;
    }
    std::error_code ec;
    if (!fs::exists(InputBindingsFile(mod), ec) || ec) {
        return;
    }
    std::vector<uint8_t> bytes;
    if (ReadPhysicalFile(
            mod, InputBindingsFile(mod), 1024u * 1024u,
            &bytes) != WOTBMOD_V3_OK ||
        bytes.size() < 12u ||
        std::memcmp(bytes.data(), "W3IN01", 6u) != 0) {
        return;
    }
    size_t position = 8u;
    uint32_t action_count = 0;
    if (!ReadPod(bytes, &position, &action_count) ||
        action_count > kMaxActionsPerMod) {
        return;
    }
    std::map<std::string, std::vector<WotbModV3InputBinding>> parsed;
    for (uint32_t i = 0; i < action_count; ++i) {
        std::string id;
        uint32_t binding_count = 0;
        if (!ReadString16(
                bytes, &position, &id,
                WOTBMOD_V3_INPUT_ACTION_ID_MAX - 1u) ||
            !IsAsciiIdentifier(
                id.c_str(),
                WOTBMOD_V3_INPUT_ACTION_ID_MAX,
                true) ||
            !ReadPod(bytes, &position, &binding_count) ||
            binding_count > WOTBMOD_V3_INPUT_BINDINGS_MAX) {
            return;
        }
        std::vector<WotbModV3InputBinding> bindings;
        for (uint32_t j = 0; j < binding_count; ++j) {
            WotbModV3InputBinding binding = {};
            if (!ReadPod(bytes, &position, &binding) ||
                !ValidInputBinding(binding)) {
                return;
            }
            bindings.push_back(binding);
        }
        parsed.emplace(std::move(id), std::move(bindings));
    }
    if (position == bytes.size()) {
        g_saved_bindings[mod] = std::move(parsed);
    }
}

WotbModV3Result SaveInputBindingsLocked(WotbModV3Handle mod) {
    std::vector<uint8_t> bytes;
    const char magic[8] = {'W','3','I','N','0','1','\0','\0'};
    bytes.insert(bytes.end(), magic, magic + sizeof(magic));
    std::map<std::string, std::vector<WotbModV3InputBinding>> values;
    auto actions = g_actions.find(mod);
    if (actions != g_actions.end()) {
        for (const InputAction* action : actions->second) {
            values[action->id] = action->bindings;
        }
    }
    g_saved_bindings[mod] = values;
    const uint32_t count = static_cast<uint32_t>(values.size());
    AppendPod(&bytes, count);
    for (const auto& item : values) {
        AppendString16(&bytes, item.first);
        const uint32_t binding_count =
            static_cast<uint32_t>(item.second.size());
        AppendPod(&bytes, binding_count);
        for (const auto& binding : item.second) {
            AppendPod(&bytes, binding);
        }
    }
    return AtomicWrite(
        mod, InputBindingsFile(mod), bytes.data(), bytes.size());
}

WotbModV3Result InputAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "input.actions");
}

WotbModV3Result WOTBMOD_V3_CALL InputRegisterAction(
    WotbModV3Handle mod,
    const WotbModV3InputActionDesc* desc,
    WotbModV3Handle* out_action) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStruct(
            desc, desc ? desc->struct_size : 0,
            sizeof(WotbModV3InputActionDesc)) ||
        !out_action ||
        !FixedTerminated(desc->id, sizeof(desc->id)) ||
        !FixedTerminated(
            desc->display_name, sizeof(desc->display_name)) ||
        !FixedTerminated(
            desc->description, sizeof(desc->description)) ||
        !IsAsciiIdentifier(
            desc->id, sizeof(desc->id), true) ||
        (desc->value_type != WOTBMOD_V3_INPUT_VALUE_BUTTON &&
         desc->value_type != WOTBMOD_V3_INPUT_VALUE_AXIS) ||
        desc->contexts == 0 ||
        (desc->contexts & ~static_cast<uint64_t>(
            WOTBMOD_V3_CONTEXT_ALL)) != 0 ||
        desc->default_binding_count >
            WOTBMOD_V3_INPUT_BINDINGS_MAX ||
        (desc->default_binding_count != 0 &&
         !desc->default_bindings)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid input action descriptor");
    }
    std::vector<WotbModV3InputBinding> bindings;
    for (uint32_t i = 0; i < desc->default_binding_count; ++i) {
        if (!ValidInputBinding(desc->default_bindings[i])) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid default input binding");
        }
        for (const auto& existing : bindings) {
            if (SameInputChord(existing, desc->default_bindings[i])) {
                return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                            "duplicate default input binding");
            }
        }
        bindings.push_back(desc->default_bindings[i]);
    }
    std::unique_ptr<InputAction> action(new InputAction(mod, *desc));
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        LoadInputBindingsLocked(mod);
        auto& actions = g_actions[mod];
        if (actions.size() >= kMaxActionsPerMod) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "input action limit reached");
        }
        for (const InputAction* existing : actions) {
            if (existing->id == action->id) {
                return Fail(mod, WOTBMOD_V3_E_ALREADY_EXISTS,
                            "input action id is already registered");
            }
        }
        auto saved_owner = g_saved_bindings.find(mod);
        if (saved_owner != g_saved_bindings.end()) {
            auto saved = saved_owner->second.find(action->id);
            if (saved != saved_owner->second.end()) {
                bindings = saved->second;
            }
        }
    }
    action->bindings = std::move(bindings);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_INPUT_ACTION,
        action.get(),
        DestroyObject<InputAction>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    action->handle = handle;
    WotbModV3Result saved = WOTBMOD_V3_OK;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_actions[mod].push_back(action.get());
        saved = SaveInputBindingsLocked(mod);
        if (saved != WOTBMOD_V3_OK) {
            g_actions[mod].pop_back();
        }
    }
    if (saved != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
        return saved;
    }
    action.release();
    *out_action = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetInputAction(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    InputAction** out) {
    return GetObject(
        mod, handle, WOTBMOD_V3_HANDLE_INPUT_ACTION,
        ObjectKind::InputAction, out);
}

WotbModV3Result WOTBMOD_V3_CALL InputUnregisterAction(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        if (!action->subscriptions.empty()) {
            return Fail(mod, WOTBMOD_V3_E_BUSY,
                        "unsubscribe input callbacks before unregistering action");
        }
        auto owner = g_actions.find(mod);
        if (owner != g_actions.end()) {
            auto& actions = owner->second;
            actions.erase(
                std::remove(actions.begin(), actions.end(), action),
                actions.end());
        }
        WotbModV3Result saved = SaveInputBindingsLocked(mod);
        if (saved != WOTBMOD_V3_OK) return saved;
    }
    return ReleaseOwnedHandle(mod, action_handle);
}

WotbModV3Result WOTBMOD_V3_CALL InputSetContexts(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    uint64_t contexts) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (contexts == 0 ||
        (contexts & ~static_cast<uint64_t>(
            WOTBMOD_V3_CONTEXT_ALL)) != 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid input action context mask");
    }
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    action->contexts = contexts;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputGetBindings(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    WotbModV3InputBinding* bindings,
    uint32_t* inout_count) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!inout_count) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "binding count pointer is null");
    }
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const uint32_t required =
        static_cast<uint32_t>(action->bindings.size());
    if (!bindings || *inout_count < required) {
        *inout_count = required;
        return required == 0 ? WOTBMOD_V3_OK :
               WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::copy(
        action->bindings.begin(), action->bindings.end(), bindings);
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputSetBindings(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    const WotbModV3InputBinding* bindings,
    uint32_t binding_count) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (binding_count > WOTBMOD_V3_INPUT_BINDINGS_MAX ||
        (binding_count != 0 && !bindings)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid input binding array");
    }
    std::vector<WotbModV3InputBinding> copied;
    for (uint32_t i = 0; i < binding_count; ++i) {
        if (!ValidInputBinding(bindings[i])) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid input binding");
        }
        for (const auto& existing : copied) {
            if (SameInputChord(existing, bindings[i])) {
                return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                            "duplicate input binding");
            }
        }
        copied.push_back(bindings[i]);
    }
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const std::vector<WotbModV3InputBinding> previous =
        action->bindings;
    action->bindings = std::move(copied);
    WotbModV3Result saved = SaveInputBindingsLocked(mod);
    if (saved != WOTBMOD_V3_OK) {
        action->bindings = previous;
    }
    return saved;
}

WotbModV3Result WOTBMOD_V3_CALL InputSubscribe(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    WotbModV3InputActionCallback callback,
    void* user_data,
    WotbModV3Token* out_token) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!callback || !out_token) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "input subscription callback/output is null");
    }
    *out_token = WOTBMOD_V3_INVALID_HANDLE;
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::unique_ptr<InputSubscription> subscription(
        new InputSubscription(mod, action, callback, user_data));
    WotbModV3Handle token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SUBSCRIPTION,
        subscription.get(),
        DestroyObject<InputSubscription>,
        &token);
    if (created != WOTBMOD_V3_OK) return created;
    subscription->token = token;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        action->subscriptions.push_back(subscription.get());
    }
    subscription.release();
    *out_token = token;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputIsActionDown(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    uint32_t* out_down) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_down) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                               "input state output is null");
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    *out_down = action->down ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputIsActionPressed(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    uint32_t* out_pressed) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_pressed) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                  "input state output is null");
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_input_mutex);
    *out_pressed = action->pressed_frame == CurrentFrameIndex()
        ? 1u
        : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputGetAxis(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    float* out_value) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "input axis output is null");
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    if (action->value_type != WOTBMOD_V3_INPUT_VALUE_AXIS) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "input action is not an axis");
    }
    std::lock_guard<std::mutex> lock(g_input_mutex);
    *out_value = action->value_frame == CurrentFrameIndex()
        ? action->value
        : 0.0f;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputCaptureBegin(
    WotbModV3Handle mod,
    uint64_t contexts) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (contexts == 0 ||
        (contexts & ~static_cast<uint64_t>(
            WOTBMOD_V3_CONTEXT_ALL)) != 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid capture context mask");
    }
    std::lock_guard<std::mutex> lock(g_input_mutex);
    if (g_input_captures.find(mod) != g_input_captures.end()) {
        return Fail(mod, WOTBMOD_V3_E_BUSY,
                    "input capture is already active");
    }
    InputCaptureState capture = {};
    capture.contexts = contexts;
    g_input_captures.emplace(mod, capture);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputCaptureEnd(
    WotbModV3Handle mod,
    WotbModV3InputBinding* out_binding) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_binding) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "captured binding output is null");
    }
    std::lock_guard<std::mutex> lock(g_input_mutex);
    const auto found = g_input_captures.find(mod);
    if (found == g_input_captures.end()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "input capture is not active");
    }
    if (!found->second.captured) {
        return Fail(mod, WOTBMOD_V3_E_BUSY,
                    "input capture has not received a binding");
    }
    *out_binding = found->second.binding;
    g_input_captures.erase(found);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL InputFindConflicts(
    WotbModV3Handle mod,
    WotbModV3Handle action_handle,
    WotbModV3InputConflict* conflicts,
    uint32_t* inout_count) {
    WotbModV3Result access = InputAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!inout_count) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "conflict count pointer is null");
    }
    InputAction* action = nullptr;
    WotbModV3Result found =
        GetInputAction(mod, action_handle, &action);
    if (found != WOTBMOD_V3_OK) return found;
    std::vector<WotbModV3InputConflict> found_conflicts;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        for (const auto& owner : g_actions) {
            for (const InputAction* other : owner.second) {
                if (other == action) continue;
                const uint64_t overlap =
                    action->contexts & other->contexts;
                if (overlap == 0) continue;
                for (const auto& binding : action->bindings) {
                    for (const auto& other_binding : other->bindings) {
                        if (!SameInputChord(binding, other_binding)) {
                            continue;
                        }
                        WotbModV3InputConflict conflict = {};
                        conflict.struct_size = sizeof(conflict);
                        conflict.api_version = WOTBMOD_V3_INPUT_VERSION;
                        conflict.owner_mod = other->owner;
                        conflict.action = other->handle;
                        conflict.binding = binding;
                        conflict.overlapping_contexts = overlap;
                        CopyFixed(
                            conflict.action_id,
                            sizeof(conflict.action_id),
                            other->id);
                        found_conflicts.push_back(conflict);
                    }
                }
            }
        }
    }
    const uint32_t required =
        static_cast<uint32_t>(found_conflicts.size());
    if (!conflicts || *inout_count < required) {
        *inout_count = required;
        return required == 0 ? WOTBMOD_V3_OK :
               WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::copy(
        found_conflicts.begin(), found_conflicts.end(), conflicts);
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

bool InputEventMatchesBinding(
    const NativeInputEvent& event,
    const WotbModV3InputBinding& binding) {
    return event.device == binding.device &&
           event.code == binding.code &&
           event.modifiers == binding.modifiers;
}

void QueueInputCallbacksLocked(
    InputAction* action,
    float value,
    uint32_t pressed,
    bool coalescible,
    std::vector<PendingInputCallback>* callbacks) {
    if (!action || !callbacks) return;
    for (const InputSubscription* subscription :
         action->subscriptions) {
        if (!subscription || !subscription->callback) continue;
        PendingInputCallback pending = {};
        pending.owner = subscription->owner;
        pending.action = action->handle;
        pending.callback = subscription->callback;
        pending.user_data = subscription->user_data;
        pending.value = value;
        pending.pressed = pressed;
        pending.coalescible = coalescible;
        callbacks->push_back(pending);
    }
}

void PumpNativeInput(uint64_t frame_index) {
    std::deque<NativeInputEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_native_input_mutex);
        events.swap(g_native_input_events);
    }

    std::vector<PendingInputCallback> callbacks;
    const uint64_t current_context = CurrentContext();
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        for (auto& owner : g_actions) {
            for (InputAction* action : owner.second) {
                if (action &&
                    action->value_type ==
                        WOTBMOD_V3_INPUT_VALUE_AXIS &&
                    action->value_frame != frame_index) {
                    action->value = 0.0f;
                }
            }
        }

        for (const NativeInputEvent& event : events) {
            if (event.kind == NATIVE_INPUT_EVENT_RESET) {
                for (auto& owner : g_actions) {
                    for (InputAction* action : owner.second) {
                        if (!action) continue;
                        const bool notify = action->down ||
                            std::fabs(action->value) > 0.00001f;
                        action->down = false;
                        action->value = 0.0f;
                        action->value_frame = frame_index;
                        if (notify) {
                            QueueInputCallbacksLocked(
                                action, 0.0f, 0u, false, &callbacks);
                        }
                    }
                }
                continue;
            }

            if ((event.kind == NATIVE_INPUT_EVENT_BUTTON &&
                 event.down != 0u) ||
                (event.kind == NATIVE_INPUT_EVENT_AXIS &&
                 std::fabs(event.value) > 0.00001f)) {
                for (auto& capture : g_input_captures) {
                    if (capture.second.captured ||
                        (current_context != WOTBMOD_V3_CONTEXT_NONE &&
                         (capture.second.contexts & current_context) ==
                             0u)) {
                        continue;
                    }
                    WotbModV3InputBinding binding = {};
                    binding.struct_size = sizeof(binding);
                    binding.api_version = WOTBMOD_V3_INPUT_VERSION;
                    binding.device = event.device;
                    binding.code = event.code;
                    binding.modifiers = event.modifiers;
                    binding.scale = 1.0f;
                    capture.second.binding = binding;
                    capture.second.captured = true;
                }
            }

            for (auto& owner : g_actions) {
                for (InputAction* action : owner.second) {
                    if (!action ||
                        (current_context != WOTBMOD_V3_CONTEXT_NONE &&
                         (action->contexts & current_context) == 0u)) {
                        continue;
                    }
                    const WotbModV3InputBinding* matched = nullptr;
                    for (const auto& binding : action->bindings) {
                        if (InputEventMatchesBinding(event, binding)) {
                            matched = &binding;
                            break;
                        }
                    }
                    if (!matched) continue;

                    if (action->value_type ==
                        WOTBMOD_V3_INPUT_VALUE_BUTTON) {
                        const bool down = event.down != 0u;
                        if (action->down == down) continue;
                        action->down = down;
                        action->value = down ? matched->scale : 0.0f;
                        action->value_frame = frame_index;
                        if (down) action->pressed_frame = frame_index;
                        QueueInputCallbacksLocked(
                            action,
                            action->value,
                            down ? 1u : 0u,
                            false,
                            &callbacks);
                    } else {
                        float value = event.kind ==
                                NATIVE_INPUT_EVENT_AXIS
                            ? event.value * matched->scale
                            : (event.down != 0u
                                   ? matched->scale
                                   : 0.0f);
                        if (!std::isfinite(value)) value = 0.0f;
                        action->value = value;
                        action->value_frame = frame_index;
                        action->down = std::fabs(value) > 0.00001f;
                        if (action->down) {
                            action->pressed_frame = frame_index;
                        }
                        QueueInputCallbacksLocked(
                            action,
                            value,
                            action->down ? 1u : 0u,
                            true,
                            &callbacks);
                    }
                }
            }
        }
    }

    for (const PendingInputCallback& pending : callbacks) {
        if (!pending.callback) continue;
        const WotbModV3Result callback_entry = pending.coalescible
            ? EnterModCallbackCoalescible(pending.owner)
            : EnterModCallback(pending.owner);
        if (callback_entry != WOTBMOD_V3_OK) {
            continue;
        }
        try {
            pending.callback(
                pending.owner,
                pending.action,
                pending.value,
                pending.pressed,
                pending.user_data);
        } catch (...) {
            SetError(
                pending.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "input action callback raised an exception");
        }
        LeaveModCallback(pending.owner);
    }
}

void StopInputOwner(WotbModV3Handle owner) {
    std::vector<WotbModV3Token> subscriptions;
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_input_captures.erase(owner);
        const auto found = g_actions.find(owner);
        if (found != g_actions.end()) {
            for (InputAction* action : found->second) {
                if (!action) continue;
                for (InputSubscription* subscription :
                     action->subscriptions) {
                    if (subscription &&
                        subscription->token !=
                            WOTBMOD_V3_INVALID_HANDLE) {
                        subscriptions.push_back(subscription->token);
                    }
                }
            }
        }
    }
    for (WotbModV3Token token : subscriptions) {
        ReleaseOwnedHandle(owner, token);
    }
}

}  // namespace

WotbModV3Result NotifyNativeInput(
    uint32_t event_kind,
    uint32_t device,
    uint32_t code,
    uint32_t modifiers,
    float value,
    uint32_t down) {
    if (event_kind < NATIVE_INPUT_EVENT_BUTTON ||
        event_kind > NATIVE_INPUT_EVENT_RESET ||
        (event_kind != NATIVE_INPUT_EVENT_RESET &&
         (device < WOTBMOD_V3_INPUT_DEVICE_KEYBOARD ||
          device > WOTBMOD_V3_INPUT_DEVICE_TOUCH ||
          code == 0u || !std::isfinite(value))) ||
        (modifiers &
         ~(WOTBMOD_V3_INPUT_MOD_SHIFT |
           WOTBMOD_V3_INPUT_MOD_CONTROL |
           WOTBMOD_V3_INPUT_MOD_ALT |
           WOTBMOD_V3_INPUT_MOD_META)) != 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    NativeInputEvent event = {};
    event.kind = event_kind;
    event.device = device;
    event.code = code;
    event.modifiers = modifiers;
    event.value = value;
    event.down = down != 0u ? 1u : 0u;
    std::lock_guard<std::mutex> lock(g_native_input_mutex);
    if (event_kind == NATIVE_INPUT_EVENT_RESET) {
        g_native_input_events.clear();
        g_native_input_events.push_back(event);
        return WOTBMOD_V3_OK;
    }
    if (event_kind == NATIVE_INPUT_EVENT_AXIS) {
        for (auto it = g_native_input_events.rbegin();
             it != g_native_input_events.rend(); ++it) {
            if (it->kind == event.kind &&
                it->device == event.device &&
                it->code == event.code &&
                it->modifiers == event.modifiers) {
                it->value += event.value;
                if (!std::isfinite(it->value)) it->value = event.value;
                return WOTBMOD_V3_OK;
            }
        }
    }
    if (g_native_input_events.size() >= kMaxNativeInputEvents) {
        const auto coalescible = std::find_if(
            g_native_input_events.begin(),
            g_native_input_events.end(),
            [](const NativeInputEvent& queued) {
                return queued.kind == NATIVE_INPUT_EVENT_AXIS;
            });
        if (coalescible != g_native_input_events.end()) {
            g_native_input_events.erase(coalescible);
        } else {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
    }
    g_native_input_events.push_back(event);
    return WOTBMOD_V3_OK;
}

void ResetNativeInputState(void) {
    NotifyNativeInput(
        NATIVE_INPUT_EVENT_RESET,
        WOTBMOD_V3_INPUT_DEVICE_KEYBOARD,
        0u,
        WOTBMOD_V3_INPUT_MOD_NONE,
        0.0f,
        0u);
}

namespace {

struct StorageValue {
    bool json = false;
    std::vector<uint8_t> bytes;
};

struct StorageState {
    bool loaded = false;
    std::map<std::string, StorageValue> values;
};

std::mutex g_storage_mutex;
std::unordered_map<WotbModV3Handle, StorageState> g_storage;

fs::path StorageFile(WotbModV3Handle mod) {
    return fs::path(ModDataPath(mod)) / "storage.v3.bin";
}

bool ValidStorageKey(const char* key) {
    return key &&
           IsAsciiIdentifier(key, WOTBMOD_V3_STORAGE_KEY_MAX, true);
}

uint64_t StorageTotal(const std::map<std::string, StorageValue>& values) {
    uint64_t total = 0;
    for (const auto& item : values) {
        total += item.first.size();
        total += item.second.bytes.size();
    }
    return total;
}

WotbModV3Result SerializeStorage(
    WotbModV3Handle mod,
    const std::map<std::string, StorageValue>& values,
    std::vector<uint8_t>* out) {
    if (!out || values.size() > kMaxStorageEntries ||
        StorageTotal(values) > kMaxStorageTotal) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "storage entry or total size limit exceeded");
    }
    out->clear();
    const char magic[8] = {'W','3','K','V','0','1','\0','\0'};
    out->insert(out->end(), magic, magic + sizeof(magic));
    const uint32_t count = static_cast<uint32_t>(values.size());
    AppendPod(out, count);
    for (const auto& item : values) {
        AppendString16(out, item.first);
        const uint8_t kind = item.second.json ? 1u : 2u;
        AppendPod(out, kind);
        const uint32_t size =
            static_cast<uint32_t>(item.second.bytes.size());
        AppendPod(out, size);
        out->insert(
            out->end(),
            item.second.bytes.begin(),
            item.second.bytes.end());
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result FlushStorageMap(
    WotbModV3Handle mod,
    const std::map<std::string, StorageValue>& values) {
    std::vector<uint8_t> bytes;
    WotbModV3Result serialized =
        SerializeStorage(mod, values, &bytes);
    if (serialized != WOTBMOD_V3_OK) {
        return serialized;
    }
    return AtomicWrite(
        mod, StorageFile(mod), bytes.data(), bytes.size());
}

void LoadStorageLocked(
    WotbModV3Handle mod,
    StorageState* state) {
    if (!state || state->loaded) {
        return;
    }
    state->loaded = true;
    std::error_code ec;
    if (!fs::exists(StorageFile(mod), ec) || ec) {
        return;
    }
    std::vector<uint8_t> bytes;
    if (ReadPhysicalFile(
            mod, StorageFile(mod), kMaxStorageTotal + 1024u * 1024u,
            &bytes) != WOTBMOD_V3_OK ||
        bytes.size() < 12u ||
        std::memcmp(bytes.data(), "W3KV01", 6u) != 0) {
        return;
    }
    size_t position = 8u;
    uint32_t count = 0;
    if (!ReadPod(bytes, &position, &count) ||
        count > kMaxStorageEntries) {
        return;
    }
    std::map<std::string, StorageValue> parsed;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        uint8_t kind = 0;
        uint32_t size = 0;
        if (!ReadString16(
                bytes, &position, &key,
                WOTBMOD_V3_STORAGE_KEY_MAX - 1u) ||
            !ValidStorageKey(key.c_str()) ||
            !ReadPod(bytes, &position, &kind) ||
            (kind != 1u && kind != 2u) ||
            !ReadPod(bytes, &position, &size) ||
            size > kMaxStorageValue ||
            position > bytes.size() ||
            bytes.size() - position < size) {
            return;
        }
        StorageValue value;
        value.json = kind == 1u;
        value.bytes.assign(
            bytes.begin() + static_cast<ptrdiff_t>(position),
            bytes.begin() + static_cast<ptrdiff_t>(position + size));
        position += size;
        parsed.emplace(std::move(key), std::move(value));
    }
    if (position == bytes.size() &&
        StorageTotal(parsed) <= kMaxStorageTotal) {
        state->values = std::move(parsed);
    }
}

WotbModV3Result StorageAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) return owner;
    WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "filesystem.mod_data");
    if (access != WOTBMOD_V3_OK) return access;
    std::lock_guard<std::mutex> lock(g_storage_mutex);
    LoadStorageLocked(mod, &g_storage[mod]);
    return WOTBMOD_V3_OK;
}

bool ValidateJsonText(const char* json, std::string* error) {
    if (!json) {
        if (error) *error = "JSON value is null";
        return false;
    }
    const size_t size = std::strlen(json);
    if (size == 0 || size > kMaxStorageValue) {
        if (error) *error = "JSON value is empty or oversized";
        return false;
    }
    JsonValue value;
    JsonParser parser(json, size);
    return parser.Parse(&value, error);
}

struct StorageTransaction final : TaggedObject {
    explicit StorageTransaction(WotbModV3Handle owner_value)
        : TaggedObject(ObjectKind::StorageTransaction),
          owner(owner_value) {}
    WotbModV3Handle owner;
    std::map<std::string, StorageValue> changes;
    std::set<std::string> erasures;
    bool completed = false;
};

WotbModV3Result StorageLookup(
    WotbModV3Handle mod,
    const char* key,
    StorageValue* out) {
    if (!ValidStorageKey(key) || !out) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid storage key or output");
    }
    std::lock_guard<std::mutex> lock(g_storage_mutex);
    auto state = g_storage.find(mod);
    if (state == g_storage.end()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "storage state is unavailable");
    }
    auto value = state->second.values.find(key);
    if (value == state->second.values.end()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "storage key was not found");
    }
    *out = value->second;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL StorageGetJson(
    WotbModV3Handle mod,
    const char* key,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    StorageValue value;
    WotbModV3Result found = StorageLookup(mod, key, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (!value.json) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "storage value is binary, not JSON");
    }
    return CopyOutString(
        mod,
        std::string(
            reinterpret_cast<const char*>(value.bytes.data()),
            value.bytes.size()),
        buffer,
        inout_size);
}

WotbModV3Result MutateStorage(
    WotbModV3Handle mod,
    const char* key,
    const StorageValue* value) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStorageKey(key)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "storage key is invalid");
    }
    std::lock_guard<std::mutex> lock(g_storage_mutex);
    StorageState& state = g_storage[mod];
    if (value) {
        if (value->bytes.size() > kMaxStorageValue) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "storage value exceeds per-value limit");
        }
        state.values[key] = *value;
        if (state.values.size() > kMaxStorageEntries ||
            StorageTotal(state.values) > kMaxStorageTotal) {
            state.values.erase(key);
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "storage total limit exceeded");
        }
    } else {
        if (state.values.erase(key) == 0) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "storage key was not found");
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL StorageSetJson(
    WotbModV3Handle mod,
    const char* key,
    const char* json_utf8) {
    std::string error;
    if (!ValidateJsonText(json_utf8, &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    StorageValue value;
    value.json = true;
    value.bytes.assign(
        json_utf8, json_utf8 + std::strlen(json_utf8));
    return MutateStorage(mod, key, &value);
}

WotbModV3Result WOTBMOD_V3_CALL StorageGetBytes(
    WotbModV3Handle mod,
    const char* key,
    WotbModV3Buffer* inout_buffer) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    StorageValue value;
    WotbModV3Result found = StorageLookup(mod, key, &value);
    if (found != WOTBMOD_V3_OK) return found;
    return CopyOutBytes(
        mod, value.bytes.data(), value.bytes.size(), inout_buffer);
}

WotbModV3Result WOTBMOD_V3_CALL StorageSetBytes(
    WotbModV3Handle mod,
    const char* key,
    const WotbModV3ConstBuffer* input) {
    if (!ValidStruct(
            input, input ? input->struct_size : 0,
            sizeof(WotbModV3ConstBuffer)) ||
        (input->size != 0 && !input->data) ||
        input->size > kMaxStorageValue) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid storage binary buffer");
    }
    StorageValue value;
    value.json = false;
    const uint8_t* data =
        static_cast<const uint8_t*>(input->data);
    if (input->size != 0) {
        value.bytes.assign(data, data + input->size);
    }
    return MutateStorage(mod, key, &value);
}

WotbModV3Result WOTBMOD_V3_CALL StorageErase(
    WotbModV3Handle mod,
    const char* key) {
    return MutateStorage(mod, key, nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL StorageContains(
    WotbModV3Handle mod,
    const char* key,
    uint32_t* out_contains) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStorageKey(key) || !out_contains) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid storage contains request");
    }
    std::lock_guard<std::mutex> lock(g_storage_mutex);
    auto state = g_storage.find(mod);
    *out_contains =
        state != g_storage.end() &&
        state->second.values.find(key) != state->second.values.end()
            ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL StorageFlush(
    WotbModV3Handle mod) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::map<std::string, StorageValue> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_storage_mutex);
        snapshot = g_storage[mod].values;
    }
    return FlushStorageMap(mod, snapshot);
}

WotbModV3Result WOTBMOD_V3_CALL StorageBeginTransaction(
    WotbModV3Handle mod,
    WotbModV3Token* out_transaction) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_transaction) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "transaction output is null");
    }
    std::unique_ptr<StorageTransaction> transaction(
        new StorageTransaction(mod));
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        transaction.get(),
        DestroyObject<StorageTransaction>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    transaction.release();
    *out_transaction = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetStorageTransaction(
    WotbModV3Handle mod,
    WotbModV3Token token,
    StorageTransaction** out) {
    WotbModV3Result result = GetObject(
        mod, token, WOTBMOD_V3_HANDLE_RESOURCE,
        ObjectKind::StorageTransaction, out);
    if (result != WOTBMOD_V3_OK) return result;
    if ((*out)->completed) {
        return Fail(mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
                    "storage transaction is already completed");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result TransactionSet(
    WotbModV3Handle mod,
    WotbModV3Token token,
    const char* key,
    const StorageValue* value,
    bool erase) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!ValidStorageKey(key) || (!erase && !value)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid transaction mutation");
    }
    StorageTransaction* transaction = nullptr;
    WotbModV3Result found =
        GetStorageTransaction(mod, token, &transaction);
    if (found != WOTBMOD_V3_OK) return found;
    if (erase) {
        transaction->changes.erase(key);
        transaction->erasures.insert(key);
    } else {
        if (value->bytes.size() > kMaxStorageValue) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "transaction value exceeds per-value limit");
        }
        transaction->erasures.erase(key);
        transaction->changes[key] = *value;
    }
    if (transaction->changes.size() +
        transaction->erasures.size() > kMaxStorageEntries) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "transaction mutation limit exceeded");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL StorageTransactionSetJson(
    WotbModV3Handle mod,
    WotbModV3Token transaction,
    const char* key,
    const char* json_utf8) {
    std::string error;
    if (!ValidateJsonText(json_utf8, &error)) {
        return Fail(mod, WOTBMOD_V3_E_PARSE, error.c_str());
    }
    StorageValue value;
    value.json = true;
    value.bytes.assign(
        json_utf8, json_utf8 + std::strlen(json_utf8));
    return TransactionSet(
        mod, transaction, key, &value, false);
}

WotbModV3Result WOTBMOD_V3_CALL StorageTransactionSetBytes(
    WotbModV3Handle mod,
    WotbModV3Token transaction,
    const char* key,
    const WotbModV3ConstBuffer* input) {
    if (!ValidStruct(
            input, input ? input->struct_size : 0,
            sizeof(WotbModV3ConstBuffer)) ||
        (input->size != 0 && !input->data) ||
        input->size > kMaxStorageValue) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid transaction binary buffer");
    }
    StorageValue value;
    const uint8_t* data =
        static_cast<const uint8_t*>(input->data);
    if (input->size != 0) {
        value.bytes.assign(data, data + input->size);
    }
    return TransactionSet(
        mod, transaction, key, &value, false);
}

WotbModV3Result WOTBMOD_V3_CALL StorageTransactionErase(
    WotbModV3Handle mod,
    WotbModV3Token transaction,
    const char* key) {
    return TransactionSet(
        mod, transaction, key, nullptr, true);
}

WotbModV3Result WOTBMOD_V3_CALL StorageCommit(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    StorageTransaction* transaction = nullptr;
    WotbModV3Result found =
        GetStorageTransaction(mod, token, &transaction);
    if (found != WOTBMOD_V3_OK) return found;
    std::map<std::string, StorageValue> candidate;
    {
        std::lock_guard<std::mutex> lock(g_storage_mutex);
        candidate = g_storage[mod].values;
    }
    for (const std::string& key : transaction->erasures) {
        candidate.erase(key);
    }
    for (const auto& item : transaction->changes) {
        candidate[item.first] = item.second;
    }
    if (candidate.size() > kMaxStorageEntries ||
        StorageTotal(candidate) > kMaxStorageTotal) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "committed storage would exceed limits");
    }
    WotbModV3Result flushed = FlushStorageMap(mod, candidate);
    if (flushed != WOTBMOD_V3_OK) return flushed;
    {
        std::lock_guard<std::mutex> lock(g_storage_mutex);
        g_storage[mod].values = std::move(candidate);
    }
    transaction->completed = true;
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result WOTBMOD_V3_CALL StorageRollback(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    StorageTransaction* transaction = nullptr;
    WotbModV3Result found =
        GetStorageTransaction(mod, token, &transaction);
    if (found != WOTBMOD_V3_OK) return found;
    transaction->completed = true;
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result WOTBMOD_V3_CALL StorageGetPath(
    WotbModV3Handle mod,
    uint32_t path_kind,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = StorageAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::string path;
    switch (path_kind) {
        case WOTBMOD_V3_STORAGE_PATH_DATA:
            path = ModDataPath(mod);
            break;
        case WOTBMOD_V3_STORAGE_PATH_CONFIG:
            path = ModConfigPath(mod);
            break;
        case WOTBMOD_V3_STORAGE_PATH_CACHE:
            path = ModCachePath(mod);
            break;
        case WOTBMOD_V3_STORAGE_PATH_TEMP: {
            fs::path temporary =
                fs::path(ModCachePath(mod)) / "temp";
            std::error_code ec;
            fs::create_directories(temporary, ec);
            if (ec) {
                return Fail(mod, WOTBMOD_V3_E_IO,
                            "cannot create mod temp directory");
            }
            path = temporary.string();
            break;
        }
        default:
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "unknown storage path kind");
    }
    if (path.empty()) {
        return Fail(mod, WOTBMOD_V3_E_IO,
                    "runtime did not provide a mod storage path");
    }
    return CopyOutString(mod, path, buffer, inout_size);
}

}  // namespace

namespace {

struct SettingValue {
    uint32_t type = WOTBMOD_V3_SETTING_STRING;
    bool boolean = false;
    int64_t integer = 0;
    double floating = 0.0;
    WotbModV3Color color = {};
    std::string text;
};

struct StoredSettingDefinition {
    WotbModV3SettingDefinition abi = {};
};

struct StoredPreset {
    std::string id;
    std::string name;
    std::vector<WotbModV3SettingPresetValue> values;
};

struct SettingsSubscription;

struct SettingsState {
    uint32_t declared_version = 0;
    uint32_t stored_version = 0;
    bool loaded = false;
    std::map<std::string, StoredSettingDefinition> definitions;
    std::map<std::string, SettingValue> values;
    std::map<std::string, StoredPreset> presets;
    std::vector<SettingsSubscription*> subscriptions;
};

std::mutex g_settings_mutex;
std::unordered_map<WotbModV3Handle, SettingsState> g_settings;
std::mutex g_owner_mutex;
std::unordered_map<WotbModV3Handle, WotbModV3Handle> g_owner_anchors;

void CleanupOwnerState(WotbModV3Handle owner);

// This destructor takes g_owner_mutex, so an OwnerAnchor must never be
// destroyed by a thread that already holds it. g_owner_mutex is a plain
// std::mutex: re-locking it on the owning thread makes MSVC throw
// std::system_error, the throw escapes this implicitly-noexcept
// destructor, and the process dies in terminate()/abort() -- exit
// 0xC0000409 with no log line and no unwinding. Inside the loader DLL
// that is the player's game vanishing mid-session. Every code path that
// can end an OwnerAnchor's lifetime has to release g_owner_mutex first;
// see EnsureDataOwner below for how that is kept structural.
struct OwnerAnchor final : TaggedObject {
    explicit OwnerAnchor(WotbModV3Handle value)
        : TaggedObject(ObjectKind::OwnerAnchor), owner(value) {}
    ~OwnerAnchor() override {
        // Teardown belongs to the anchor the handle registry adopted, and
        // only to that one. Both actions below are keyed by owner, not by
        // anchor identity, so an anchor the registry refused must do
        // nothing at all: it owns none of this owner's state, and by the
        // time it is destroyed the owner may have been re-enabled and a
        // different anchor adopted. Tearing down here would then erase
        // that live anchor's settings, storage, input bindings, mounts
        // and resources, and drop its g_owner_anchors entry while its
        // handle is still alive in the registry -- silent data loss plus
        // an orphaned handle, inside the DLL injected into the game.
        if (!adopted.load()) {
            return;
        }
        CleanupOwnerState(owner);
        // This lock is why an OwnerAnchor must never be destroyed by a
        // thread that already holds g_owner_mutex. It is a plain
        // std::mutex, so re-locking it on the owning thread makes MSVC
        // throw std::system_error; the throw escapes this
        // implicitly-noexcept destructor and the process dies in
        // terminate()/abort() -- exit 0xC0000409, no log line, no
        // unwinding. See EnsureDataOwner for how that is kept impossible.
        std::lock_guard<std::mutex> lock(g_owner_mutex);
        g_owner_anchors.erase(owner);
    }
    WotbModV3Handle owner;
    // True while this anchor is the one the handle registry holds, and so
    // the one that owes this owner's teardown. It is set just *before* the
    // registry can observe the object and cleared again if the registry
    // refuses it -- see EnsureDataOwner, which explains why that is the
    // only safe order. Read from ~OwnerAnchor without g_owner_mutex held,
    // hence atomic: from the moment CreateOwnedHandle succeeds, another
    // thread can destroy an adopted anchor.
    std::atomic<bool> adopted{false};
};

WotbModV3Result EnsureDataOwner(WotbModV3Handle mod) {
    WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) {
        return check;
    }
    {
        std::lock_guard<std::mutex> lock(g_owner_mutex);
        if (g_owner_anchors.find(mod) != g_owner_anchors.end()) {
            return WOTBMOD_V3_OK;
        }
    }
    // The anchor is built before the lock, and that is load-bearing
    // rather than stylistic. ~OwnerAnchor takes g_owner_mutex, so an
    // anchor destroyed inside the critical section below would re-lock a
    // plain std::mutex on this thread and abort the process. Constructing
    // it out here means the guarded block contains no construction, no
    // reset and no assignment of an owning pointer -- only release(),
    // which cannot destroy anything -- so there is no statement in it
    // that can end an anchor's lifetime, whatever anyone adds later.
    // Declaring the pointer before the guard and constructing inside it
    // would have been enough for today's exit paths, but only those: a
    // later reset() or reassignment within the scope would destroy under
    // the lock and bring the abort back. Making g_owner_mutex recursive
    // would merely hide all of this, leaving a destructor re-entering a
    // lock it does not know it holds.
    //
    // The cost is one allocation on the miss path only; the hit path
    // above returns without allocating, and a miss happens once per owner
    // per enable.
    std::unique_ptr<OwnerAnchor> anchor(new OwnerAnchor(mod));
    {
        std::lock_guard<std::mutex> lock(g_owner_mutex);
        // Re-checked: another thread may have adopted an anchor for this
        // owner while this one was being built.
        if (g_owner_anchors.find(mod) != g_owner_anchors.end()) {
            return WOTBMOD_V3_OK;
        }
        // Marked before publication, and this is the only correct place
        // for it. CreateOwnedHandle stores the object into the handle
        // registry and drops g_state_mutex before returning, so from the
        // instant it succeeds another thread releasing this owner's
        // handles can already be inside ~OwnerAnchor. Two things follow.
        // Reading this flag as false there would skip the teardown an
        // adopted anchor owes; and the destructor would then return
        // without waiting on g_owner_mutex, so operator delete runs while
        // this function is still executing -- which makes any touch of
        // `anchor` after the call a write to freed memory. Setting it
        // first removes both: nothing below dereferences the object, and
        // an anchor destroyed in that window blocks on the mutex held
        // here until this scope ends.
        anchor->adopted.store(true);
        WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3Result result = CreateOwnedHandle(
            mod,
            WOTBMOD_V3_HANDLE_RESOURCE,
            anchor.get(),
            DestroyObject<OwnerAnchor>,
            &handle);
        if (result != WOTBMOD_V3_OK) {
            // Refused -- owner stopping, disabled, or the registry full.
            // Every failure path in CreateOwnedHandle returns before it
            // stores the object, so this anchor was never published and
            // is still private to this thread: clearing the flag here
            // races with nobody, and it is what makes the anchor inert
            // when it dies below, after the guard.
            anchor->adopted.store(false);
            return result;
        }
        // Neither of these touches the object: release() only gives up
        // ownership, and emplace stores a handle value. Keep it that way.
        anchor.release();
        g_owner_anchors.emplace(mod, handle);
    }
    return WOTBMOD_V3_OK;
}

SettingValue DefaultSettingValue(const WotbModV3SettingDefinition& definition) {
    SettingValue value;
    value.type = definition.type;
    value.boolean = definition.default_bool != 0;
    value.integer = definition.default_int;
    value.floating = definition.default_float;
    value.color = definition.default_color;
    value.text = definition.default_text;
    return value;
}

std::vector<std::string> SplitEnumValues(const char* text) {
    std::vector<std::string> values;
    std::string current;
    const std::string source = text ? text : "";
    for (char c : source) {
        if (c == ',') {
            if (!current.empty()) {
                values.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        values.push_back(current);
    }
    return values;
}

bool SafeRelativeValue(const std::string& value) {
    if (value.empty()) {
        return true;
    }
    if (value.size() >= WOTBMOD_V3_MAX_PATH ||
        value.find('\\') != std::string::npos ||
        value.find('%') != std::string::npos ||
        fs::path(value).is_absolute()) {
        return false;
    }
    std::istringstream stream(value);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
    }
    return true;
}

bool ValidateSettingDefinition(
    const WotbModV3SettingDefinition& definition,
    std::string* error) {
    if (!ValidStruct(&definition, definition.struct_size,
                     sizeof(WotbModV3SettingDefinition)) ||
        !FixedTerminated(definition.key, sizeof(definition.key)) ||
        !FixedTerminated(definition.title, sizeof(definition.title)) ||
        !FixedTerminated(
            definition.description, sizeof(definition.description)) ||
        !FixedTerminated(
            definition.default_text, sizeof(definition.default_text)) ||
        !FixedTerminated(
            definition.enum_values, sizeof(definition.enum_values)) ||
        !FixedTerminated(
            definition.visible_if, sizeof(definition.visible_if)) ||
        !FixedTerminated(
            definition.enabled_if, sizeof(definition.enabled_if))) {
        if (error) {
            *error = "setting definition has invalid struct or unterminated text";
        }
        return false;
    }
    if (!IsAsciiIdentifier(
            definition.key,
            sizeof(definition.key),
            true)) {
        if (error) {
            *error = "setting key is not a valid namespaced identifier";
        }
        return false;
    }
    if (definition.type < WOTBMOD_V3_SETTING_BOOL ||
        definition.type > WOTBMOD_V3_SETTING_CUSTOM) {
        if (error) {
            *error = "setting type is outside the supported range";
        }
        return false;
    }
    if (definition.type == WOTBMOD_V3_SETTING_INT) {
        if (definition.min_int > definition.max_int ||
            definition.default_int < definition.min_int ||
            definition.default_int > definition.max_int ||
            definition.step_int < 0) {
            if (error) {
                *error = "integer setting bounds/default/step are invalid";
            }
            return false;
        }
    }
    if (definition.type == WOTBMOD_V3_SETTING_FLOAT) {
        if (!std::isfinite(definition.default_float) ||
            !std::isfinite(definition.min_float) ||
            !std::isfinite(definition.max_float) ||
            !std::isfinite(definition.step_float) ||
            definition.min_float > definition.max_float ||
            definition.default_float < definition.min_float ||
            definition.default_float > definition.max_float ||
            definition.step_float < 0.0) {
            if (error) {
                *error = "floating setting bounds/default/step are invalid";
            }
            return false;
        }
    }
    if (definition.type == WOTBMOD_V3_SETTING_ENUM) {
        const std::vector<std::string> values =
            SplitEnumValues(definition.enum_values);
        if (values.empty() ||
            std::find(
                values.begin(), values.end(),
                std::string(definition.default_text)) == values.end()) {
            if (error) {
                *error = "enum values do not contain the default";
            }
            return false;
        }
    }
    if ((definition.type == WOTBMOD_V3_SETTING_FILE ||
         definition.type == WOTBMOD_V3_SETTING_FOLDER) &&
        !SafeRelativeValue(definition.default_text)) {
        if (error) {
            *error = "file/folder default escapes the mod namespace";
        }
        return false;
    }
    if (definition.type == WOTBMOD_V3_SETTING_COLOR) {
        const float values[] = {
            definition.default_color.r,
            definition.default_color.g,
            definition.default_color.b,
            definition.default_color.a
        };
        for (float value : values) {
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
                if (error) {
                    *error = "color default is outside [0,1]";
                }
                return false;
            }
        }
    }
    return true;
}

bool ValidateSettingValue(
    const WotbModV3SettingDefinition& definition,
    const SettingValue& value) {
    if (definition.type != value.type) {
        return false;
    }
    switch (value.type) {
        case WOTBMOD_V3_SETTING_BOOL:
            return true;
        case WOTBMOD_V3_SETTING_INT:
            return value.integer >= definition.min_int &&
                   value.integer <= definition.max_int &&
                   (definition.step_int == 0 ||
                    (value.integer - definition.min_int) %
                        definition.step_int == 0);
        case WOTBMOD_V3_SETTING_FLOAT: {
            if (!std::isfinite(value.floating) ||
                value.floating < definition.min_float ||
                value.floating > definition.max_float) {
                return false;
            }
            if (definition.step_float == 0.0) {
                return true;
            }
            const double steps =
                (value.floating - definition.min_float) /
                definition.step_float;
            return std::fabs(steps - std::round(steps)) < 1e-7;
        }
        case WOTBMOD_V3_SETTING_COLOR: {
            const float values[] = {
                value.color.r, value.color.g,
                value.color.b, value.color.a
            };
            return std::all_of(
                std::begin(values), std::end(values),
                [](float component) {
                    return std::isfinite(component) &&
                           component >= 0.0f && component <= 1.0f;
                });
        }
        case WOTBMOD_V3_SETTING_ENUM: {
            const std::vector<std::string> values =
                SplitEnumValues(definition.enum_values);
            return std::find(
                values.begin(), values.end(), value.text) != values.end();
        }
        case WOTBMOD_V3_SETTING_FILE:
        case WOTBMOD_V3_SETTING_FOLDER:
            return SafeRelativeValue(value.text);
        case WOTBMOD_V3_SETTING_STRING:
        case WOTBMOD_V3_SETTING_KEYBIND:
        case WOTBMOD_V3_SETTING_CUSTOM:
            return value.text.size() < WOTBMOD_V3_SETTING_TEXT_MAX;
        case WOTBMOD_V3_SETTING_TITLE:
        case WOTBMOD_V3_SETTING_BUTTON:
            return false;
        default:
            return false;
    }
}

std::vector<uint8_t> SerializeSettingValue(const SettingValue& value) {
    std::vector<uint8_t> bytes;
    switch (value.type) {
        case WOTBMOD_V3_SETTING_BOOL: {
            const uint8_t boolean = value.boolean ? 1u : 0u;
            AppendPod(&bytes, boolean);
            break;
        }
        case WOTBMOD_V3_SETTING_INT:
            AppendPod(&bytes, value.integer);
            break;
        case WOTBMOD_V3_SETTING_FLOAT:
            AppendPod(&bytes, value.floating);
            break;
        case WOTBMOD_V3_SETTING_COLOR:
            AppendPod(&bytes, value.color);
            break;
        default:
            bytes.assign(value.text.begin(), value.text.end());
            break;
    }
    return bytes;
}

bool DeserializeSettingValue(
    uint32_t type,
    const uint8_t* data,
    size_t size,
    SettingValue* out) {
    if (!out) {
        return false;
    }
    SettingValue value;
    value.type = type;
    switch (type) {
        case WOTBMOD_V3_SETTING_BOOL:
            if (size != 1u || data[0] > 1u) {
                return false;
            }
            value.boolean = data[0] != 0;
            break;
        case WOTBMOD_V3_SETTING_INT:
            if (size != sizeof(value.integer)) {
                return false;
            }
            std::memcpy(&value.integer, data, size);
            break;
        case WOTBMOD_V3_SETTING_FLOAT:
            if (size != sizeof(value.floating)) {
                return false;
            }
            std::memcpy(&value.floating, data, size);
            break;
        case WOTBMOD_V3_SETTING_COLOR:
            if (size != sizeof(value.color)) {
                return false;
            }
            std::memcpy(&value.color, data, size);
            break;
        default:
            if (size >= WOTBMOD_V3_SETTING_TEXT_MAX) {
                return false;
            }
            value.text.assign(reinterpret_cast<const char*>(data), size);
            break;
    }
    *out = std::move(value);
    return true;
}

fs::path SettingsFile(WotbModV3Handle mod) {
    return fs::path(ModConfigPath(mod)) / "settings.v3.bin";
}

WotbModV3Result SaveSettingsLocked(
    WotbModV3Handle mod,
    const SettingsState& state) {
    std::vector<uint8_t> bytes;
    const char magic[8] = {'W','3','S','E','T','0','1','\0'};
    bytes.insert(bytes.end(), magic, magic + sizeof(magic));
    AppendPod(&bytes, state.stored_version);
    const uint32_t count = static_cast<uint32_t>(state.values.size());
    AppendPod(&bytes, count);
    for (const auto& item : state.values) {
        if (item.first.size() > std::numeric_limits<uint16_t>::max()) {
            return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                        "setting key cannot be serialized");
        }
        AppendString16(&bytes, item.first);
        AppendPod(&bytes, item.second.type);
        const std::vector<uint8_t> payload =
            SerializeSettingValue(item.second);
        const uint32_t payload_size =
            static_cast<uint32_t>(payload.size());
        AppendPod(&bytes, payload_size);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    return AtomicWrite(
        mod, SettingsFile(mod), bytes.data(), bytes.size());
}

void LoadSettingsLocked(
    WotbModV3Handle mod,
    SettingsState* state) {
    if (!state || state->loaded) {
        return;
    }
    state->loaded = true;
    std::vector<uint8_t> bytes;
    if (ReadPhysicalFile(
            mod, SettingsFile(mod), 4u * 1024u * 1024u,
            &bytes) != WOTBMOD_V3_OK) {
        state->stored_version = state->declared_version;
        return;
    }
    if (bytes.size() < 16u ||
        std::memcmp(bytes.data(), "W3SET01", 7u) != 0) {
        state->stored_version = state->declared_version;
        return;
    }
    size_t position = 8u;
    uint32_t stored_version = 0;
    uint32_t count = 0;
    if (!ReadPod(bytes, &position, &stored_version) ||
        !ReadPod(bytes, &position, &count) ||
        count > kMaxSettings) {
        state->stored_version = state->declared_version;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        uint32_t type = 0;
        uint32_t size = 0;
        if (!ReadString16(
                bytes, &position, &key,
                WOTBMOD_V3_SETTING_KEY_MAX - 1u) ||
            !ReadPod(bytes, &position, &type) ||
            !ReadPod(bytes, &position, &size) ||
            position > bytes.size() ||
            bytes.size() - position < size) {
            return;
        }
        auto definition = state->definitions.find(key);
        if (definition != state->definitions.end() &&
            definition->second.abi.type == type) {
            SettingValue value;
            if (DeserializeSettingValue(
                    type, bytes.data() + position, size, &value) &&
                ValidateSettingValue(definition->second.abi, value)) {
                state->values[key] = std::move(value);
            }
        }
        position += size;
    }
    if (position == bytes.size()) {
        state->stored_version = stored_version;
    }
}

struct SettingsSubscription final : TaggedObject {
    SettingsSubscription(
        WotbModV3Handle owner_value,
        WotbModV3SettingChangedCallback callback_value,
        void* user_value)
        : TaggedObject(ObjectKind::SettingsSubscription),
          owner(owner_value),
          callback(callback_value),
          user_data(user_value) {}
    ~SettingsSubscription() override {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        auto found = g_settings.find(owner);
        if (found == g_settings.end()) {
            return;
        }
        auto& subscriptions = found->second.subscriptions;
        subscriptions.erase(
            std::remove(
                subscriptions.begin(), subscriptions.end(), this),
            subscriptions.end());
    }
    WotbModV3Handle owner;
    WotbModV3SettingChangedCallback callback;
    void* user_data;
};

void NotifySettingChanged(
    WotbModV3Handle mod,
    const std::string& key,
    uint32_t type,
    const std::vector<SettingsSubscription*>& subscriptions) {
    for (SettingsSubscription* subscription : subscriptions) {
        if (subscription && subscription->callback) {
            subscription->callback(
                mod, key.c_str(), type, subscription->user_data);
        }
    }
}

WotbModV3Result SettingsAccess(WotbModV3Handle mod) {
    WotbModV3Result owner = EnsureDataOwner(mod);
    if (owner != WOTBMOD_V3_OK) {
        return owner;
    }
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "settings");
}

WotbModV3Result WOTBMOD_V3_CALL SettingsRegisterSchema(
    WotbModV3Handle mod,
    uint32_t schema_version,
    const WotbModV3SettingDefinition* definitions,
    uint32_t definition_count) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (schema_version == 0 || definition_count > kMaxSettings ||
        (definition_count != 0 && !definitions)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid settings schema descriptor");
    }
    std::map<std::string, StoredSettingDefinition> copied;
    for (uint32_t i = 0; i < definition_count; ++i) {
        std::string error;
        if (!ValidateSettingDefinition(definitions[i], &error)) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT, error.c_str());
        }
        const std::string key = definitions[i].key;
        if (!copied.emplace(
                key,
                StoredSettingDefinition{definitions[i]}).second) {
            return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                        "settings schema contains a duplicate key");
        }
    }
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingsState& state = g_settings[mod];
    if (state.declared_version != 0 &&
        schema_version < state.declared_version) {
        return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                    "settings schema version cannot move backwards");
    }
    std::map<std::string, SettingValue> values;
    for (const auto& item : copied) {
        auto previous = state.values.find(item.first);
        if (previous != state.values.end() &&
            ValidateSettingValue(item.second.abi, previous->second)) {
            values.emplace(item.first, previous->second);
        } else {
            values.emplace(
                item.first, DefaultSettingValue(item.second.abi));
        }
    }
    state.declared_version = schema_version;
    state.definitions = std::move(copied);
    state.values = std::move(values);
    LoadSettingsLocked(mod, &state);
    if (state.stored_version == 0) {
        state.stored_version = schema_version;
    }
    return SaveSettingsLocked(mod, state);
}

SettingValue PresetValueToInternal(
    const WotbModV3SettingPresetValue& input) {
    SettingValue value;
    value.type = input.type;
    value.boolean = input.bool_value != 0;
    value.integer = input.int_value;
    value.floating = input.float_value;
    value.color = input.color_value;
    value.text = input.text_value;
    return value;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsRegisterPreset(
    WotbModV3Handle mod,
    const WotbModV3SettingPreset* preset) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!ValidStruct(
            preset, preset ? preset->struct_size : 0,
            sizeof(WotbModV3SettingPreset)) ||
        !FixedTerminated(preset->id, sizeof(preset->id)) ||
        !FixedTerminated(preset->name, sizeof(preset->name)) ||
        !IsAsciiIdentifier(preset->id, sizeof(preset->id), true) ||
        preset->value_count > kMaxSettings ||
        (preset->value_count != 0 && !preset->values)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "invalid settings preset");
    }
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingsState& state = g_settings[mod];
    if (state.declared_version == 0) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "register a settings schema before presets");
    }
    if (state.presets.size() >= kMaxPresets &&
        state.presets.find(preset->id) == state.presets.end()) {
        return Fail(mod, WOTBMOD_V3_E_LIMIT_REACHED,
                    "settings preset limit reached");
    }
    StoredPreset copy;
    copy.id = preset->id;
    copy.name = preset->name;
    std::set<std::string> keys;
    for (uint32_t i = 0; i < preset->value_count; ++i) {
        const WotbModV3SettingPresetValue& value = preset->values[i];
        if (!ValidStruct(
                &value, value.struct_size,
                sizeof(WotbModV3SettingPresetValue)) ||
            !FixedTerminated(value.key, sizeof(value.key)) ||
            !FixedTerminated(value.text_value, sizeof(value.text_value)) ||
            !keys.insert(value.key).second) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "invalid or duplicate preset value");
        }
        auto definition = state.definitions.find(value.key);
        if (definition == state.definitions.end() ||
            !ValidateSettingValue(
                definition->second.abi,
                PresetValueToInternal(value))) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "preset value does not match its setting definition");
        }
        copy.values.push_back(value);
    }
    state.presets[copy.id] = std::move(copy);
    return WOTBMOD_V3_OK;
}

WotbModV3Result FindSettingLocked(
    WotbModV3Handle mod,
    const char* key,
    uint32_t expected_type,
    SettingsState** out_state,
    SettingValue** out_value) {
    if (!key || !*key) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting key is empty");
    }
    auto state = g_settings.find(mod);
    if (state == g_settings.end() || state->second.declared_version == 0) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "settings schema is not registered");
    }
    auto value = state->second.values.find(key);
    if (value == state->second.values.end()) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "setting key is not registered");
    }
    if (expected_type != 0 && value->second.type != expected_type) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting type does not match the requested accessor");
    }
    if (out_state) {
        *out_state = &state->second;
    }
    if (out_value) {
        *out_value = &value->second;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetSchemaVersion(
    WotbModV3Handle mod,
    uint32_t* out_version) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!out_version) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "schema version output is null");
    }
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    auto item = g_settings.find(mod);
    if (item == g_settings.end() ||
        item->second.declared_version == 0) {
        return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                    "settings schema is not registered");
    }
    *out_version = item->second.stored_version;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetBool(
    WotbModV3Handle mod,
    const char* key,
    uint32_t* out_value) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "bool output is null");
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingValue* value = nullptr;
    WotbModV3Result found = FindSettingLocked(
        mod, key, WOTBMOD_V3_SETTING_BOOL, nullptr, &value);
    if (found == WOTBMOD_V3_OK) *out_value = value->boolean ? 1u : 0u;
    return found;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetInt(
    WotbModV3Handle mod,
    const char* key,
    int64_t* out_value) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "integer output is null");
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingValue* value = nullptr;
    WotbModV3Result found = FindSettingLocked(
        mod, key, WOTBMOD_V3_SETTING_INT, nullptr, &value);
    if (found == WOTBMOD_V3_OK) *out_value = value->integer;
    return found;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetFloat(
    WotbModV3Handle mod,
    const char* key,
    double* out_value) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "float output is null");
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingValue* value = nullptr;
    WotbModV3Result found = FindSettingLocked(
        mod, key, WOTBMOD_V3_SETTING_FLOAT, nullptr, &value);
    if (found == WOTBMOD_V3_OK) *out_value = value->floating;
    return found;
}

bool IsStringSettingType(uint32_t type) {
    return type == WOTBMOD_V3_SETTING_STRING ||
           type == WOTBMOD_V3_SETTING_ENUM ||
           type == WOTBMOD_V3_SETTING_KEYBIND ||
           type == WOTBMOD_V3_SETTING_FILE ||
           type == WOTBMOD_V3_SETTING_FOLDER ||
           type == WOTBMOD_V3_SETTING_CUSTOM;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetString(
    WotbModV3Handle mod,
    const char* key,
    char* buffer,
    uint32_t* inout_size) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingValue* value = nullptr;
    WotbModV3Result found =
        FindSettingLocked(mod, key, 0, nullptr, &value);
    if (found != WOTBMOD_V3_OK) return found;
    if (!IsStringSettingType(value->type)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting is not string-compatible");
    }
    return CopyOutString(mod, value->text, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL SettingsGetColor(
    WotbModV3Handle mod,
    const char* key,
    WotbModV3Color* out_value) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!out_value) return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                                "color output is null");
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    SettingValue* value = nullptr;
    WotbModV3Result found = FindSettingLocked(
        mod, key, WOTBMOD_V3_SETTING_COLOR, nullptr, &value);
    if (found == WOTBMOD_V3_OK) *out_value = value->color;
    return found;
}

WotbModV3Result SetSettingValue(
    WotbModV3Handle mod,
    const char* key,
    SettingValue new_value) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::vector<SettingsSubscription*> subscriptions;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        SettingsState* state = nullptr;
        SettingValue* old_value = nullptr;
        WotbModV3Result found =
            FindSettingLocked(mod, key, 0, &state, &old_value);
        if (found != WOTBMOD_V3_OK) {
            return found;
        }
        const auto definition = state->definitions.find(key);
        if (definition == state->definitions.end() ||
            (definition->second.abi.flags &
             WOTBMOD_V3_SETTING_FLAG_READ_ONLY) != 0 ||
            !ValidateSettingValue(definition->second.abi, new_value)) {
            return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                        "setting value violates its schema");
        }
        *old_value = std::move(new_value);
        WotbModV3Result saved = SaveSettingsLocked(mod, *state);
        if (saved != WOTBMOD_V3_OK) {
            return saved;
        }
        subscriptions = state->subscriptions;
    }
    NotifySettingChanged(
        mod, key, new_value.type, subscriptions);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSetBool(
    WotbModV3Handle mod, const char* key, uint32_t value) {
    SettingValue setting;
    setting.type = WOTBMOD_V3_SETTING_BOOL;
    setting.boolean = value != 0;
    return SetSettingValue(mod, key, std::move(setting));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSetInt(
    WotbModV3Handle mod, const char* key, int64_t value) {
    SettingValue setting;
    setting.type = WOTBMOD_V3_SETTING_INT;
    setting.integer = value;
    return SetSettingValue(mod, key, std::move(setting));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSetFloat(
    WotbModV3Handle mod, const char* key, double value) {
    SettingValue setting;
    setting.type = WOTBMOD_V3_SETTING_FLOAT;
    setting.floating = value;
    return SetSettingValue(mod, key, std::move(setting));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSetString(
    WotbModV3Handle mod, const char* key, const char* value) {
    if (!value || std::strlen(value) >= WOTBMOD_V3_SETTING_TEXT_MAX) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting string is null or too long");
    }
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    uint32_t type = 0;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        SettingValue* old = nullptr;
        WotbModV3Result found =
            FindSettingLocked(mod, key, 0, nullptr, &old);
        if (found != WOTBMOD_V3_OK) return found;
        type = old->type;
    }
    if (!IsStringSettingType(type)) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting is not string-compatible");
    }
    SettingValue setting;
    setting.type = type;
    setting.text = value;
    return SetSettingValue(mod, key, std::move(setting));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSetColor(
    WotbModV3Handle mod,
    const char* key,
    const WotbModV3Color* value) {
    if (!value) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "setting color is null");
    }
    SettingValue setting;
    setting.type = WOTBMOD_V3_SETTING_COLOR;
    setting.color = *value;
    return SetSettingValue(mod, key, std::move(setting));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsReset(
    WotbModV3Handle mod,
    const char* key) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    SettingValue value;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        auto state = g_settings.find(mod);
        if (state == g_settings.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "settings schema is not registered");
        }
        auto definition = state->second.definitions.find(key ? key : "");
        if (definition == state->second.definitions.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "setting key is not registered");
        }
        value = DefaultSettingValue(definition->second.abi);
    }
    return SetSettingValue(mod, key, std::move(value));
}

WotbModV3Result WOTBMOD_V3_CALL SettingsResetAll(
    WotbModV3Handle mod) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    std::vector<std::pair<std::string, uint32_t>> changed;
    std::vector<SettingsSubscription*> subscriptions;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        auto state = g_settings.find(mod);
        if (state == g_settings.end() ||
            state->second.declared_version == 0) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "settings schema is not registered");
        }
        for (const auto& definition : state->second.definitions) {
            state->second.values[definition.first] =
                DefaultSettingValue(definition.second.abi);
            changed.emplace_back(
                definition.first, definition.second.abi.type);
        }
        WotbModV3Result saved =
            SaveSettingsLocked(mod, state->second);
        if (saved != WOTBMOD_V3_OK) return saved;
        subscriptions = state->second.subscriptions;
    }
    for (const auto& item : changed) {
        NotifySettingChanged(
            mod, item.first, item.second, subscriptions);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsApplyPreset(
    WotbModV3Handle mod,
    const char* preset_id) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!preset_id || !*preset_id) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "preset id is empty");
    }
    std::vector<std::pair<std::string, uint32_t>> changed;
    std::vector<SettingsSubscription*> subscriptions;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        auto state = g_settings.find(mod);
        if (state == g_settings.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "settings schema is not registered");
        }
        auto preset = state->second.presets.find(preset_id);
        if (preset == state->second.presets.end()) {
            return Fail(mod, WOTBMOD_V3_E_NOT_FOUND,
                        "settings preset is not registered");
        }
        std::map<std::string, SettingValue> candidate =
            state->second.values;
        for (const auto& value : preset->second.values) {
            auto definition =
                state->second.definitions.find(value.key);
            SettingValue converted = PresetValueToInternal(value);
            if (definition == state->second.definitions.end() ||
                !ValidateSettingValue(
                    definition->second.abi, converted)) {
                return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                            "registered preset no longer matches schema");
            }
            candidate[value.key] = std::move(converted);
            changed.emplace_back(value.key, value.type);
        }
        state->second.values = std::move(candidate);
        WotbModV3Result saved =
            SaveSettingsLocked(mod, state->second);
        if (saved != WOTBMOD_V3_OK) return saved;
        subscriptions = state->second.subscriptions;
    }
    for (const auto& item : changed) {
        NotifySettingChanged(
            mod, item.first, item.second, subscriptions);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsSubscribe(
    WotbModV3Handle mod,
    WotbModV3SettingChangedCallback callback,
    void* user_data,
    WotbModV3Token* out_token) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!callback || !out_token) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "settings subscription callback/output is null");
    }
    std::unique_ptr<SettingsSubscription> subscription(
        new SettingsSubscription(mod, callback, user_data));
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SUBSCRIPTION,
        subscription.get(),
        DestroyObject<SettingsSubscription>,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        g_settings[mod].subscriptions.push_back(subscription.get());
    }
    subscription.release();
    *out_token = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    SettingsSubscription* subscription = nullptr;
    WotbModV3Result found = GetObject(
        mod, token, WOTBMOD_V3_HANDLE_SUBSCRIPTION,
        ObjectKind::SettingsSubscription, &subscription);
    if (found != WOTBMOD_V3_OK) return found;
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result WOTBMOD_V3_CALL SettingsRunMigration(
    WotbModV3Handle mod,
    uint32_t target_version,
    WotbModV3SettingsMigrationCallback callback,
    void* user_data) {
    WotbModV3Result access = SettingsAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    if (!callback || target_version == 0) {
        return Fail(mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                    "migration callback or target version is invalid");
    }
    uint32_t old_version = 0;
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        auto state = g_settings.find(mod);
        if (state == g_settings.end() ||
            target_version > state->second.declared_version ||
            target_version < state->second.stored_version) {
            return Fail(mod, WOTBMOD_V3_E_CONFLICT,
                        "migration target is outside the schema version range");
        }
        old_version = state->second.stored_version;
    }
    if (old_version == target_version) {
        return WOTBMOD_V3_OK;
    }
    WotbModV3Result migrated =
        callback(mod, old_version, target_version, user_data);
    if (migrated != WOTBMOD_V3_OK) {
        return SetError(
            mod, migrated, "settings migration callback failed");
    }
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    auto state = g_settings.find(mod);
    if (state == g_settings.end()) {
        return Fail(mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
                    "settings state disappeared during migration");
    }
    state->second.stored_version = target_version;
    return SaveSettingsLocked(mod, state->second);
}

}  // namespace

namespace {

void CleanupOwnerState(WotbModV3Handle owner) {
    (void)InstalledDavaNativeReleaseOwner(owner);
    {
        std::lock_guard<std::mutex> lock(g_settings_mutex);
        g_settings.erase(owner);
    }
    {
        std::lock_guard<std::mutex> lock(g_storage_mutex);
        g_storage.erase(owner);
    }
    {
        std::lock_guard<std::mutex> lock(g_input_mutex);
        g_actions.erase(owner);
        g_saved_bindings.erase(owner);
        g_loaded_bindings.erase(owner);
    }
    {
        std::lock_guard<std::mutex> lock(g_vfs_mutex);
        g_mounts.erase(
            std::remove_if(
                g_mounts.begin(), g_mounts.end(),
                [owner](const VfsMount* mount) {
                    return mount && mount->owner == owner;
                }),
            g_mounts.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_resource_mutex);
        g_resources.erase(owner);
        g_resource_groups.erase(owner);
    }
}

const WotbModV3SettingsApiV1 kSettingsApi = {
    sizeof(WotbModV3SettingsApiV1),
    WOTBMOD_V3_SETTINGS_VERSION,
    SettingsRegisterSchema,
    SettingsRegisterPreset,
    SettingsGetSchemaVersion,
    SettingsGetBool,
    SettingsGetInt,
    SettingsGetFloat,
    SettingsGetString,
    SettingsGetColor,
    SettingsSetBool,
    SettingsSetInt,
    SettingsSetFloat,
    SettingsSetString,
    SettingsSetColor,
    SettingsReset,
    SettingsResetAll,
    SettingsApplyPreset,
    SettingsSubscribe,
    SettingsUnsubscribe,
    SettingsRunMigration
};

const WotbModV3StorageApiV1 kStorageApi = {
    sizeof(WotbModV3StorageApiV1),
    WOTBMOD_V3_STORAGE_VERSION,
    StorageGetJson,
    StorageSetJson,
    StorageGetBytes,
    StorageSetBytes,
    StorageErase,
    StorageContains,
    StorageFlush,
    StorageBeginTransaction,
    StorageTransactionSetJson,
    StorageTransactionSetBytes,
    StorageTransactionErase,
    StorageCommit,
    StorageRollback,
    StorageGetPath
};

const WotbModV3InputApiV1 kInputApi = {
    sizeof(WotbModV3InputApiV1),
    WOTBMOD_V3_INPUT_VERSION,
    InputRegisterAction,
    InputUnregisterAction,
    InputSetContexts,
    InputGetBindings,
    InputSetBindings,
    InputSubscribe,
    InputIsActionDown,
    InputIsActionPressed,
    InputGetAxis,
    InputCaptureBegin,
    InputCaptureEnd,
    InputFindConflicts
};

const WotbModV3VfsApiV1 kVfsApi = {
    sizeof(WotbModV3VfsApiV1),
    WOTBMOD_V3_VFS_VERSION,
    VfsGetNamespace,
    VfsNormalizeUri,
    VfsMountPackage,
    VfsMountOverlay,
    VfsUnmount,
    VfsResolve,
    VfsOpen,
    VfsRead,
    VfsList,
    VfsStat,
    VfsWatch,
    VfsGetProviders,
    VfsSetProviderPriority,
    VfsGetConflicts
};

/*
 * V2 embeds V1 by value rather than pointing at it, which is what makes the
 * ABI freeze hold: a V1 consumer keeps querying version 1 and gets `kVfsApi`
 * untouched, and a V2 consumer gets the same fifteen slots at the same offsets
 * plus four more. Both are registered - see RegisterDataServices.
 */
const WotbModV3VfsApiV2 kVfsApiV2 = {
    kVfsApi,
    VfsWriteFile,
    VfsAppendFile,
    VfsCopyFile,
    VfsRemoveFile
};

const WotbModV3ResourcesApiV1 kResourcesApi = {
    sizeof(WotbModV3ResourcesApiV1),
    WOTBMOD_V3_RESOURCES_VERSION,
    ResourcesLoad,
    ResourcesLoadAsync,
    ResourcesGetInfo,
    ResourcesCopyData,
    ResourcesRetain,
    ResourcesRelease,
    ResourcesPreloadGroup,
    ResourcesUnloadGroup,
    ResourcesReload,
    ResourcesWatch,
    ResourcesGetMemoryUsage
};

const WotbModV3YamlApiV1 kYamlApi = {
    sizeof(WotbModV3YamlApiV1),
    WOTBMOD_V3_YAML_VERSION,
    YamlParse,
    YamlParseUri,
    YamlGetRoot,
    YamlGetType,
    YamlGetSize,
    YamlMapGet,
    YamlSequenceGet,
    YamlGetString,
    YamlGetBool,
    YamlGetInt,
    YamlGetFloat
};

const WotbModV3ArchiveApiV1 kArchiveApi = {
    sizeof(WotbModV3ArchiveApiV1),
    WOTBMOD_V3_ARCHIVE_VERSION,
    ArchiveOpenDirectory,
    ArchiveOpenPackageFile,
    ArchiveGetEntryCount,
    ArchiveGetEntry,
    ArchiveReadEntry,
    ArchiveExtractEntry,
    ArchiveExtractAll,
    ArchiveGetSha256,
    ArchiveVerifySha256,
    ArchiveCancel
};

const WotbModV3LoadersApiV1 kLoadersApi = {
    sizeof(WotbModV3LoadersApiV1),
    WOTBMOD_V3_LOADERS_VERSION,
    LoadersLoadText,
    LoadersLoadBinary,
    LoadersLoadYaml,
    LoadersLoadDavaYaml,
    LoadersUnpackDvpl,
    LoadersOpenDavaArchive,
    LoadersGetBackendInfo
};

const WotbModV3ManifestApiV1 kManifestApi = {
    sizeof(WotbModV3ManifestApiV1),
    WOTBMOD_V3_MANIFEST_VERSION,
    ManifestParseJson,
    ManifestParseUri,
    ManifestValidate,
    ManifestGetInfo,
    ManifestGetApiRequirement,
    ManifestGetDependency,
    ManifestGetPermission,
    ManifestGetResourcePattern,
    ManifestGetLocalePattern,
    ManifestGetEntrypoint,
    ManifestGetClientBuild,
    ManifestGetClientExecutableHash,
    ManifestResolveDependencies,
    ManifestVerifyContentSha256,
    ManifestVerifySignature
};

const WotbModV3CatalogApiV1 kCatalogApi = {
    sizeof(WotbModV3CatalogApiV1),
    WOTBMOD_V3_CATALOG_VERSION,
    CatalogValidateRecord,
    CatalogEvaluateInstall,
    CatalogStatusName
};

const WotbModV3ContentApiV1 kContentApi = {
    sizeof(WotbModV3ContentApiV1),
    WOTBMOD_V3_CONTENT_VERSION,
    ContentParseJson,
    ContentParseUri,
    ContentValidate,
    ContentGetInfo,
    ContentGetOverride,
    ContentApply,
    ContentUnapply
};

void RegisterDataInterface(
    const char* name,
    const void* table,
    const char* capability,
    uint32_t version = WOTBMOD_V3_IFACE_VERSION_1) {
    InterfaceRegistration registration = {};
    registration.name = name;
    registration.version = version;
    registration.table = table;
    registration.required_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    registration.allowed_contexts = WOTBMOD_V3_CONTEXT_ALL;
    registration.capability_name = capability;
    const WotbModV3Result result =
        RegisterInterface(registration);
    if (result != WOTBMOD_V3_OK) {
        const std::string message =
            std::string("failed to register ") + name +
            " result=" + std::to_string(result);
        RuntimeLog(
            4u, "v3.data.registration", message.c_str());
    }
}

}  // namespace

void RegisterDataServices() {
    const WotbModV3Result frame_pump =
        RegisterFramePump(&DataServicesFramePump);
    if (frame_pump != WOTBMOD_V3_OK) {
        RuntimeLog(
            4u,
            "v3.data.registration",
            "failed to register the portable watch frame pump");
    }
    const WotbModV3Result owner_stopping =
        RegisterOwnerStoppingHook(&DataServicesOwnerStopping);
    if (owner_stopping != WOTBMOD_V3_OK) {
        RuntimeLog(
            4u,
            "v3.data.registration",
            "failed to register the portable watch owner barrier");
    }
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_SETTINGS,
        &kSettingsApi, "settings");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_STORAGE,
        &kStorageApi, "filesystem.mod_data");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_INPUT,
        &kInputApi, "input.actions");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_VFS,
        &kVfsApi, "resources");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_VFS,
        &kVfsApiV2, "resources",
        WOTBMOD_V3_VFS_VERSION_2);
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_RESOURCES,
        &kResourcesApi, "resources");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_YAML,
        &kYamlApi, "resources");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_ARCHIVE,
        &kArchiveApi, "resources");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_LOADERS,
        &kLoadersApi, "resources");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_MANIFEST,
        &kManifestApi, "manifest");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_CATALOG,
        &kCatalogApi, "catalog");
    RegisterDataInterface(
        WOTBMOD_V3_IFACE_CONTENT,
        &kContentApi, "content");
}

}  // namespace v3
}  // namespace wotbmod
