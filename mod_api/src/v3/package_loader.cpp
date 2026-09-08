#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "package_loader.h"

#include "data_services_backend.h"
#include "package_trust.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace wotbmod {
namespace v3 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kNoLoadOrder =
    (std::numeric_limits<uint32_t>::max)();
constexpr uint64_t kManifestLimit = 4ull * 1024ull * 1024ull;

template <typename T>
bool ValidStruct(const T* value, uint32_t required_version) {
    return value &&
           value->struct_size >= sizeof(T) &&
           value->api_version == required_version;
}

bool FixedTerminated(const char* text, size_t capacity) {
    return text &&
           std::memchr(text, '\0', capacity) != nullptr;
}

template <size_t N>
void CopyFixed(char (&destination)[N], const std::string& value) {
    static_assert(N != 0u, "fixed buffer cannot be empty");
    const size_t count = (std::min)(value.size(), N - 1u);
    if (count != 0u) {
        std::memcpy(destination, value.data(), count);
    }
    destination[count] = '\0';
}

std::string LowerAscii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
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

bool IsPackageId(const char* value) {
    if (!value || !*value) return false;
    const size_t length = std::strlen(value);
    if (length >= WOTBMOD_V3_MAX_ID ||
        value[0] == '.' || value[length - 1u] == '.') {
        return false;
    }
    bool previous_dot = false;
    for (size_t i = 0u; i < length; ++i) {
        const unsigned char c =
            static_cast<unsigned char>(value[i]);
        const bool accepted =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.';
        if (!accepted || (c == '.' && previous_dot)) {
            return false;
        }
        previous_dot = c == '.';
    }
    return true;
}

std::string PathUtf8(const fs::path& path) {
    return path.generic_u8string();
}

bool IsReparsePoint(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
}

bool PathWithin(
    const fs::path& root,
    const fs::path& candidate) {
    auto equal_component = [](const fs::path& left,
                              const fs::path& right) {
        std::wstring left_text = left.native();
        std::wstring right_text = right.native();
        std::transform(
            left_text.begin(), left_text.end(), left_text.begin(),
            [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
        std::transform(
            right_text.begin(), right_text.end(), right_text.begin(),
            [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
        });
        return left_text == right_text;
    };
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() ||
            !equal_component(*root_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

bool CanonicalExisting(
    const fs::path& path,
    const fs::path& allowed_root,
    bool regular_file,
    fs::path* out) {
    std::error_code ec;
    if (IsReparsePoint(path)) {
        return false;
    }
    const fs::path canonical = fs::canonical(path, ec);
    if (ec || !PathWithin(allowed_root, canonical)) {
        return false;
    }
    if (regular_file) {
        if (!fs::is_regular_file(canonical, ec) || ec) {
            return false;
        }
    } else if (!fs::is_directory(canonical, ec) || ec) {
        return false;
    }
    if (out) {
        *out = canonical;
    }
    return true;
}

bool HasUnsafePathComponent(const fs::path& base,
                            const fs::path& target) {
    std::error_code ec;
    fs::path current = base;
    const fs::path relative = fs::relative(target, base, ec);
    if (ec || relative.empty()) {
        return true;
    }
    for (const fs::path& part : relative) {
        current /= part;
        if (IsReparsePoint(current)) {
            return true;
        }
    }
    return false;
}

class Sha256 {
public:
    Sha256() { Reset(); }

    void Update(const void* input, size_t length) {
        const uint8_t* bytes =
            static_cast<const uint8_t*>(input);
        total_bytes_ += length;
        while (length != 0u) {
            const size_t available = 64u - buffered_;
            const size_t count = (std::min)(available, length);
            std::memcpy(buffer_ + buffered_, bytes, count);
            buffered_ += count;
            bytes += count;
            length -= count;
            if (buffered_ == 64u) {
                Transform(buffer_);
                buffered_ = 0u;
            }
        }
    }

    std::string FinalHex() {
        const uint64_t bit_length = total_bytes_ * 8u;
        buffer_[buffered_++] = 0x80u;
        if (buffered_ > 56u) {
            while (buffered_ < 64u) {
                buffer_[buffered_++] = 0u;
            }
            Transform(buffer_);
            buffered_ = 0u;
        }
        while (buffered_ < 56u) {
            buffer_[buffered_++] = 0u;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffered_++] = static_cast<uint8_t>(
                (bit_length >> shift) & 0xffu);
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
        return (value >> count) |
               (value << (32u - count));
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
        total_bytes_ = 0u;
        buffered_ = 0u;
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    void Transform(const uint8_t block[64]) {
        static const uint32_t constants[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu,
            0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u,
            0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
            0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
            0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u,
            0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u,
            0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
            0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
            0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu,
            0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu,
            0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
            0xc67178f2u
        };
        uint32_t words[64] = {};
        for (uint32_t i = 0u; i < 16u; ++i) {
            words[i] =
                (static_cast<uint32_t>(block[i * 4u]) << 24u) |
                (static_cast<uint32_t>(block[i * 4u + 1u])
                 << 16u) |
                (static_cast<uint32_t>(block[i * 4u + 2u])
                 << 8u) |
                static_cast<uint32_t>(block[i * 4u + 3u]);
        }
        for (uint32_t i = 16u; i < 64u; ++i) {
            const uint32_t s0 =
                Rotate(words[i - 15u], 7u) ^
                Rotate(words[i - 15u], 18u) ^
                (words[i - 15u] >> 3u);
            const uint32_t s1 =
                Rotate(words[i - 2u], 17u) ^
                Rotate(words[i - 2u], 19u) ^
                (words[i - 2u] >> 10u);
            words[i] = words[i - 16u] + s0 +
                       words[i - 7u] + s1;
        }
        uint32_t a = state_[0];
        uint32_t b = state_[1];
        uint32_t c = state_[2];
        uint32_t d = state_[3];
        uint32_t e = state_[4];
        uint32_t f = state_[5];
        uint32_t g = state_[6];
        uint32_t h = state_[7];
        for (uint32_t i = 0u; i < 64u; ++i) {
            const uint32_t sum1 =
                Rotate(e, 6u) ^ Rotate(e, 11u) ^
                Rotate(e, 25u);
            const uint32_t choice =
                (e & f) ^ ((~e) & g);
            const uint32_t temp1 =
                h + sum1 + choice + constants[i] + words[i];
            const uint32_t sum0 =
                Rotate(a, 2u) ^ Rotate(a, 13u) ^
                Rotate(a, 22u);
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
    uint64_t total_bytes_ = 0u;
    size_t buffered_ = 0u;
};

bool UpdateDigestFromStream(
    std::istream* input,
    Sha256* digest) {
    if (!input || !digest) return false;
    std::vector<char> buffer(64u * 1024u);
    while (*input) {
        input->read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input->gcount();
        if (count > 0) {
            digest->Update(
                buffer.data(),
                static_cast<size_t>(count));
        }
    }
    return input->eof();
}

WotbModV3Result HashFile(
    const fs::path& path,
    std::string* out_hash) {
    if (!out_hash) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return WOTBMOD_V3_E_IO;
    }
    Sha256 hash;
    if (!UpdateDigestFromStream(&input, &hash)) {
        return WOTBMOD_V3_E_IO;
    }
    *out_hash = hash.FinalHex();
    return WOTBMOD_V3_OK;
}

struct SemVer {
    uint32_t major = 0u;
    uint32_t minor = 0u;
    uint32_t patch = 0u;
    std::string prerelease;
};

std::string Trim(const std::string& value) {
    size_t first = 0u;
    while (first < value.size() &&
           std::isspace(
               static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(
               static_cast<unsigned char>(value[last - 1u]))) {
        --last;
    }
    return value.substr(first, last - first);
}

bool ParseUnsigned(const std::string& text, uint32_t* out) {
    if (!out || text.empty() ||
        (text.size() > 1u && text[0] == '0')) {
        return false;
    }
    uint32_t value = 0u;
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
        ? text : text.substr(0u, plus);
    const size_t dash = without_build.find('-');
    const std::string core =
        dash == std::string::npos
        ? without_build
        : without_build.substr(0u, dash);
    std::vector<std::string> parts;
    size_t start = 0u;
    while (start <= core.size()) {
        const size_t dot = core.find('.', start);
        parts.push_back(core.substr(
            start,
            dot == std::string::npos
            ? core.size() - start
            : dot - start));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1u;
    }
    if (parts.empty() || parts.size() > 3u) {
        return false;
    }
    SemVer version;
    if (!ParseUnsigned(parts[0], &version.major) ||
        (parts.size() > 1u &&
         !ParseUnsigned(parts[1], &version.minor)) ||
        (parts.size() > 2u &&
         !ParseUnsigned(parts[2], &version.patch))) {
        return false;
    }
    if (dash != std::string::npos) {
        version.prerelease =
            without_build.substr(dash + 1u);
        if (version.prerelease.empty()) {
            return false;
        }
        for (unsigned char c : version.prerelease) {
            if (!(std::isalnum(c) || c == '.' || c == '-')) {
                return false;
            }
        }
    }
    *out = std::move(version);
    return true;
}

int CompareSemVer(const SemVer& left, const SemVer& right) {
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    if (left.prerelease.empty() != right.prerelease.empty()) {
        return left.prerelease.empty() ? 1 : -1;
    }
    if (left.prerelease == right.prerelease) {
        return 0;
    }
    return left.prerelease < right.prerelease ? -1 : 1;
}

bool MatchComparator(
    const SemVer& version,
    std::string comparator) {
    std::string operation;
    if (comparator.rfind(">=", 0u) == 0u ||
        comparator.rfind("<=", 0u) == 0u) {
        operation = comparator.substr(0u, 2u);
        comparator = Trim(comparator.substr(2u));
    } else if (!comparator.empty() &&
               (comparator[0] == '>' ||
                comparator[0] == '<' ||
                comparator[0] == '=')) {
        operation = comparator.substr(0u, 1u);
        comparator = Trim(comparator.substr(1u));
    } else {
        operation = "=";
    }
    SemVer expected;
    if (!ParseSemVer(comparator, &expected)) {
        return false;
    }
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
    if (!ParseSemVer(version_text, &version)) {
        return false;
    }
    const std::string range = Trim(range_text);
    if (range.empty() || range == "*") {
        return true;
    }
    if (range[0] == '^' || range[0] == '~') {
        SemVer lower;
        if (!ParseSemVer(Trim(range.substr(1u)), &lower) ||
            CompareSemVer(version, lower) < 0) {
            return false;
        }
        SemVer upper = lower;
        if (range[0] == '^') {
            if (lower.major != 0u) {
                ++upper.major;
                upper.minor = 0u;
                upper.patch = 0u;
            } else {
                ++upper.minor;
                upper.patch = 0u;
            }
        } else {
            ++upper.minor;
            upper.patch = 0u;
        }
        upper.prerelease.clear();
        return CompareSemVer(version, upper) < 0;
    }
    std::string normalized = range;
    std::replace(
        normalized.begin(), normalized.end(), ',', ' ');
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

uint16_t ReadU16(
    const std::vector<uint8_t>& bytes,
    size_t offset) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(bytes[offset]) |
        (static_cast<uint16_t>(bytes[offset + 1u]) << 8u));
}

uint32_t ReadU32(
    const std::vector<uint8_t>& bytes,
    size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool RangeFits(
    size_t offset,
    size_t length,
    size_t total) {
    return offset <= total && length <= total - offset;
}

bool IsValidUtf8(const std::string& value) {
    size_t index = 0u;
    while (index < value.size()) {
        const uint8_t first =
            static_cast<uint8_t>(value[index]);
        if (first <= 0x7fu) {
            ++index;
            continue;
        }
        uint32_t codepoint = 0u;
        size_t continuation_count = 0u;
        if ((first & 0xe0u) == 0xc0u) {
            codepoint = first & 0x1fu;
            continuation_count = 1u;
            if (codepoint < 2u) return false;
        } else if ((first & 0xf0u) == 0xe0u) {
            codepoint = first & 0x0fu;
            continuation_count = 2u;
        } else if ((first & 0xf8u) == 0xf0u) {
            codepoint = first & 0x07u;
            continuation_count = 3u;
        } else {
            return false;
        }
        if (!RangeFits(
                index + 1u, continuation_count,
                value.size())) {
            return false;
        }
        for (size_t i = 0u; i < continuation_count; ++i) {
            const uint8_t next = static_cast<uint8_t>(
                value[index + 1u + i]);
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((continuation_count == 2u &&
             codepoint < 0x800u) ||
            (continuation_count == 3u &&
             codepoint < 0x10000u) ||
            codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u &&
             codepoint <= 0xdfffu)) {
            return false;
        }
        index += continuation_count + 1u;
    }
    return true;
}

bool FoldWindowsUtf8(
    const std::string& value,
    std::wstring* out) {
    if (!out || value.empty()) return false;
    const int wide_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (wide_size <= 0) return false;
    std::wstring wide(
        static_cast<size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            &wide[0], wide_size) != wide_size) {
        return false;
    }
    const int folded_size = LCMapStringW(
        LOCALE_INVARIANT, LCMAP_LOWERCASE,
        wide.data(), wide_size,
        nullptr, 0);
    if (folded_size <= 0) return false;
    std::wstring folded(
        static_cast<size_t>(folded_size), L'\0');
    if (LCMapStringW(
            LOCALE_INVARIANT, LCMAP_LOWERCASE,
            wide.data(), wide_size,
            &folded[0], folded_size) != folded_size) {
        return false;
    }
    *out = std::move(folded);
    return true;
}

bool IsWindowsReservedSegment(const std::string& segment) {
    std::string base = segment;
    const size_t dot = base.find('.');
    if (dot != std::string::npos) {
        base.resize(dot);
    }
    base = LowerAscii(base);
    if (base == "con" || base == "prn" ||
        base == "aux" || base == "nul") {
        return true;
    }
    if (base.size() == 4u &&
        (base.rfind("com", 0u) == 0u ||
         base.rfind("lpt", 0u) == 0u) &&
        base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

bool SafeArchivePath(
    const std::string& text,
    uint32_t max_depth,
    bool* out_directory) {
    if (out_directory) *out_directory = false;
    if (text.empty() ||
        text.size() >= WOTBMOD_V3_MAX_PATH ||
        !IsValidUtf8(text) ||
        text.front() == '/' ||
        text.find("//") != std::string::npos ||
        text.find('\\') != std::string::npos ||
        text.find('\0') != std::string::npos) {
        return false;
    }
    const bool directory = text.back() == '/';
    const size_t logical_size =
        directory ? text.size() - 1u : text.size();
    if (logical_size == 0u) {
        return false;
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
            IsWindowsReservedSegment(segment)) {
            return false;
        }
        for (unsigned char c : segment) {
            if (c < 0x20u || c == 0x7fu ||
                c == '<' || c == '>' || c == ':' ||
                c == '"' || c == '|' || c == '?' ||
                c == '*') {
                return false;
            }
        }
        ++depth;
        if (depth > max_depth) {
            return false;
        }
        if (end == logical_size) {
            break;
        }
        start = end + 1u;
    }
    if (out_directory) *out_directory = directory;
    return true;
}

uint32_t Crc32(const uint8_t* bytes, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0u; i < size; ++i) {
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask =
                0u - (crc & 1u);
            crc = (crc >> 1u) ^
                  (0xedb88320u & mask);
        }
    }
    return ~crc;
}

struct ZipEntry {
    std::string path;
    std::wstring folded_path;
    uint16_t flags = 0u;
    uint16_t method = 0u;
    uint32_t crc32 = 0u;
    uint32_t compressed_size = 0u;
    uint32_t unpacked_size = 0u;
    uint32_t local_header_offset = 0u;
    size_t data_offset = 0u;
    bool directory = false;
};

struct ZipPackage {
    std::vector<uint8_t> bytes;
    std::vector<ZipEntry> entries;
    size_t manifest_index =
        (std::numeric_limits<size_t>::max)();
};

WotbModV3Result ReadBoundedFile(
    const fs::path& path,
    uint64_t max_bytes,
    std::vector<uint8_t>* out,
    std::string* error) {
    if (!out) {
        if (error) *error = "file output is null";
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec) {
        if (error) *error = "cannot query file size";
        return WOTBMOD_V3_E_IO;
    }
    if (size == 0u || size > max_bytes ||
        size > static_cast<uint64_t>(
            (std::numeric_limits<size_t>::max)())) {
        if (error) *error = "file size exceeds preflight limit";
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    out->resize(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "cannot open file";
        return WOTBMOD_V3_E_IO;
    }
    input.read(
        reinterpret_cast<char*>(out->data()),
        static_cast<std::streamsize>(out->size()));
    if (!input ||
        static_cast<size_t>(input.gcount()) != out->size()) {
        if (error) *error = "short file read";
        return WOTBMOD_V3_E_IO;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ParseStoredZip(
    const fs::path& archive,
    const PackagePreflightLimits& limits,
    ZipPackage* out,
    std::string* error) {
    if (!out) {
        if (error) *error = "archive output is null";
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModV3Result read = ReadBoundedFile(
        archive, limits.max_archive_bytes,
        &out->bytes, error);
    if (read != WOTBMOD_V3_OK) {
        return read;
    }
    const size_t size = out->bytes.size();
    if (size < 22u) {
        if (error) *error = "wotbmod is not a ZIP archive";
        return WOTBMOD_V3_E_PARSE;
    }
    const size_t search_start =
        size > 22u + 65535u ? size - (22u + 65535u) : 0u;
    size_t eocd = (std::numeric_limits<size_t>::max)();
    for (size_t position = size - 22u;; --position) {
        if (ReadU32(out->bytes, position) == 0x06054b50u) {
            eocd = position;
            break;
        }
        if (position == search_start) {
            break;
        }
    }
    if (eocd == (std::numeric_limits<size_t>::max)() ||
        !RangeFits(eocd, 22u, size)) {
        if (error) *error = "ZIP end record is missing";
        return WOTBMOD_V3_E_PARSE;
    }
    const uint16_t disk = ReadU16(out->bytes, eocd + 4u);
    const uint16_t central_disk =
        ReadU16(out->bytes, eocd + 6u);
    const uint16_t disk_entries =
        ReadU16(out->bytes, eocd + 8u);
    const uint16_t total_entries =
        ReadU16(out->bytes, eocd + 10u);
    const uint32_t central_size =
        ReadU32(out->bytes, eocd + 12u);
    const uint32_t central_offset =
        ReadU32(out->bytes, eocd + 16u);
    const uint16_t comment_size =
        ReadU16(out->bytes, eocd + 20u);
    if (disk != 0u || central_disk != 0u ||
        disk_entries != total_entries) {
        if (error) *error = "multi-disk ZIP is unsupported";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (total_entries == 0xffffu ||
        central_size == 0xffffffffu ||
        central_offset == 0xffffffffu) {
        if (error) *error = "ZIP64 wotbmod is unsupported";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (total_entries == 0u ||
        total_entries > limits.max_archive_entries) {
        if (error) *error = "archive entry count exceeds limit";
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (!RangeFits(
            eocd + 22u, comment_size, size) ||
        eocd + 22u + comment_size != size ||
        !RangeFits(
            static_cast<size_t>(central_offset),
            static_cast<size_t>(central_size),
            eocd) ||
        static_cast<size_t>(central_offset) +
            static_cast<size_t>(central_size) != eocd) {
        if (error) *error = "ZIP central directory bounds are invalid";
        return WOTBMOD_V3_E_PARSE;
    }

    size_t cursor = central_offset;
    uint64_t unpacked_total = 0u;
    std::set<std::wstring> unique_paths;
    std::vector<std::pair<size_t, size_t>> occupied_ranges;
    out->entries.clear();
    out->entries.reserve(total_entries);
    for (uint32_t index = 0u;
         index < total_entries; ++index) {
        if (!RangeFits(cursor, 46u, eocd) ||
            ReadU32(out->bytes, cursor) != 0x02014b50u) {
            if (error) *error = "ZIP central entry is invalid";
            return WOTBMOD_V3_E_PARSE;
        }
        ZipEntry entry;
        entry.flags = ReadU16(out->bytes, cursor + 8u);
        entry.method = ReadU16(out->bytes, cursor + 10u);
        entry.crc32 = ReadU32(out->bytes, cursor + 16u);
        entry.compressed_size =
            ReadU32(out->bytes, cursor + 20u);
        entry.unpacked_size =
            ReadU32(out->bytes, cursor + 24u);
        const uint16_t name_size =
            ReadU16(out->bytes, cursor + 28u);
        const uint16_t extra_size =
            ReadU16(out->bytes, cursor + 30u);
        const uint16_t entry_comment_size =
            ReadU16(out->bytes, cursor + 32u);
        const uint16_t start_disk =
            ReadU16(out->bytes, cursor + 34u);
        entry.local_header_offset =
            ReadU32(out->bytes, cursor + 42u);
        const size_t variable_size =
            static_cast<size_t>(name_size) +
            static_cast<size_t>(extra_size) +
            static_cast<size_t>(entry_comment_size);
        if (name_size == 0u ||
            !RangeFits(cursor + 46u, variable_size, eocd) ||
            start_disk != 0u) {
            if (error) *error = "ZIP entry metadata is invalid";
            return WOTBMOD_V3_E_PARSE;
        }
        entry.path.assign(
            reinterpret_cast<const char*>(
                out->bytes.data() + cursor + 46u),
            name_size);
        if (!SafeArchivePath(
                entry.path, limits.max_archive_depth,
                &entry.directory)) {
            if (error) *error = "archive contains an unsafe path";
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
        if (!FoldWindowsUtf8(
                entry.path, &entry.folded_path)) {
            if (error) *error =
                "archive path cannot be normalized for Windows";
            return WOTBMOD_V3_E_PARSE;
        }
        if (!unique_paths.insert(entry.folded_path).second) {
            if (error) *error =
                "archive contains duplicate Windows paths";
            return WOTBMOD_V3_E_CONFLICT;
        }
        if ((entry.flags & ~0x0800u) != 0u) {
            if (error) {
                *error =
                    "encrypted, descriptor-backed, or patched ZIP entries are unsupported";
            }
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (entry.method != 0u) {
            if (error) {
                *error =
                    "compressed .wotbmod entries are unsupported; use ZIP store method";
            }
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (entry.compressed_size != entry.unpacked_size) {
            if (error) *error = "stored ZIP entry size mismatch";
            return WOTBMOD_V3_E_PARSE;
        }
        if (entry.unpacked_size >
            limits.max_archive_single_file_bytes) {
            if (error) *error = "archive single-file limit exceeded";
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        if (entry.unpacked_size >
                limits.max_archive_unpacked_bytes ||
            unpacked_total >
                limits.max_archive_unpacked_bytes -
                entry.unpacked_size) {
            if (error) *error = "archive unpacked-size limit exceeded";
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        unpacked_total += entry.unpacked_size;
        if (!RangeFits(
                entry.local_header_offset, 30u,
                central_offset) ||
            ReadU32(
                out->bytes,
                entry.local_header_offset) != 0x04034b50u) {
            if (error) *error = "ZIP local header is invalid";
            return WOTBMOD_V3_E_PARSE;
        }
        const uint16_t local_flags = ReadU16(
            out->bytes, entry.local_header_offset + 6u);
        const uint16_t local_method = ReadU16(
            out->bytes, entry.local_header_offset + 8u);
        const uint32_t local_crc = ReadU32(
            out->bytes, entry.local_header_offset + 14u);
        const uint32_t local_compressed_size = ReadU32(
            out->bytes, entry.local_header_offset + 18u);
        const uint32_t local_unpacked_size = ReadU32(
            out->bytes, entry.local_header_offset + 22u);
        const uint16_t local_name_size = ReadU16(
            out->bytes, entry.local_header_offset + 26u);
        const uint16_t local_extra_size = ReadU16(
            out->bytes, entry.local_header_offset + 28u);
        const size_t local_name_offset =
            static_cast<size_t>(entry.local_header_offset) + 30u;
        if (local_flags != entry.flags ||
            local_method != entry.method ||
            local_crc != entry.crc32 ||
            local_compressed_size !=
                entry.compressed_size ||
            local_unpacked_size != entry.unpacked_size ||
            local_name_size != name_size ||
            !RangeFits(
                local_name_offset,
                static_cast<size_t>(local_name_size) +
                static_cast<size_t>(local_extra_size),
                central_offset) ||
            std::memcmp(
                out->bytes.data() + local_name_offset,
                entry.path.data(),
                name_size) != 0) {
            if (error) *error = "ZIP local/central metadata mismatch";
            return WOTBMOD_V3_E_PARSE;
        }
        entry.data_offset =
            local_name_offset +
            static_cast<size_t>(local_name_size) +
            static_cast<size_t>(local_extra_size);
        if (!RangeFits(
                entry.data_offset,
                entry.compressed_size,
                central_offset)) {
            if (error) *error = "ZIP entry data exceeds archive bounds";
            return WOTBMOD_V3_E_PARSE;
        }
        occupied_ranges.emplace_back(
            static_cast<size_t>(entry.local_header_offset),
            entry.data_offset + entry.compressed_size);
        if (!entry.directory &&
            Crc32(
                out->bytes.data() + entry.data_offset,
                entry.unpacked_size) != entry.crc32) {
            if (error) *error = "ZIP entry CRC32 mismatch";
            return WOTBMOD_V3_E_HASH_MISMATCH;
        }
        if (entry.folded_path == L"manifest.json") {
            if (entry.directory ||
                out->manifest_index !=
                    (std::numeric_limits<size_t>::max)()) {
                if (error) *error =
                    "archive must contain one root manifest.json";
                return WOTBMOD_V3_E_CONFLICT;
            }
            out->manifest_index = out->entries.size();
        }
        out->entries.push_back(std::move(entry));
        cursor += 46u + variable_size;
    }
    std::sort(
        occupied_ranges.begin(), occupied_ranges.end());
    for (size_t i = 1u;
         i < occupied_ranges.size(); ++i) {
        if (occupied_ranges[i - 1u].second >
            occupied_ranges[i].first) {
            if (error) {
                *error =
                    "ZIP entries overlap in the archive";
            }
            return WOTBMOD_V3_E_CONFLICT;
        }
    }
    if (cursor != eocd ||
        out->manifest_index ==
            (std::numeric_limits<size_t>::max)()) {
        if (error) {
            *error = cursor != eocd
                ? "ZIP central directory has trailing records"
                : "archive root manifest.json is missing";
        }
        return WOTBMOD_V3_E_PARSE;
    }
    const ZipEntry& manifest =
        out->entries[out->manifest_index];
    if (manifest.unpacked_size == 0u ||
        manifest.unpacked_size > kManifestLimit) {
        if (error) *error = "archive manifest size is invalid";
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    return WOTBMOD_V3_OK;
}

struct PackageCandidate {
    PackagePlanEntry plan = {};
    std::unique_ptr<DataManifestPreflightSummary> manifest;
    fs::path package_root;
    fs::path manifest_file;
    fs::path payload_file;
    fs::path archive_file;
    std::vector<WotbModV3ManifestDependency> dependencies;
    ZipPackage zip;
};

struct DetachedPackageSignature {
    std::string algorithm;
    std::string key_id;
    std::string sha256;
    std::string value;
};

struct PackageRevocations {
    std::set<std::string> key_ids;
    std::set<std::string> releases;
};

void InitializePlan(PackagePlanEntry* plan) {
    if (!plan) return;
    std::memset(plan, 0, sizeof(*plan));
    plan->struct_size = sizeof(*plan);
    plan->api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    plan->status = PACKAGE_PLAN_BLOCKED;
    plan->result = WOTBMOD_V3_E_PARSE;
    plan->catalog_status =
        WOTBMOD_V3_CATALOG_UNREVIEWED;
    plan->load_order = kNoLoadOrder;
    plan->signature_status = WOTBMOD_V3_SIGNATURE_UNSIGNED;
}

fs::path PackageSignatureSidecar(
    const PackageCandidate& candidate) {
    if (!candidate.archive_file.empty()) {
        return fs::u8path(
            PathUtf8(candidate.archive_file) + ".sig");
    }
    if (candidate.plan.source_kind == PACKAGE_SOURCE_DIRECTORY) {
        return candidate.package_root.parent_path() /
            fs::u8path(
                candidate.package_root.filename().u8string() +
                ".wotbmod.sig");
    }
    return fs::u8path(
        PathUtf8(candidate.manifest_file) + ".sig");
}

WotbModV3Result ReadDetachedPackageSignature(
    const fs::path& path,
    DetachedPackageSignature* out,
    std::string* error) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::error_code ec;
    const uint64_t size = fs::file_size(path, ec);
    if (ec || size == 0u || size > 16u * 1024u ||
        IsReparsePoint(path)) {
        if (error) *error = ec
            ? "signature sidecar cannot be queried"
            : "signature sidecar is empty, oversized, or reparse-backed";
        return ec ? WOTBMOD_V3_E_IO
                  : WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "signature sidecar cannot be read";
        return WOTBMOD_V3_E_IO;
    }
    std::string line;
    if (!std::getline(input, line) ||
        PackageTrustTrim(line) != "WOTBMOD-SIGNATURE-V1") {
        if (error) *error = "signature sidecar header is invalid";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    std::set<std::string> seen;
    while (std::getline(input, line)) {
        line = PackageTrustTrim(line);
        if (line.empty()) continue;
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            if (error) *error = "signature sidecar entry is invalid";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
        const std::string name =
            PackageTrustTrim(line.substr(0u, separator));
        const std::string value =
            PackageTrustTrim(line.substr(separator + 1u));
        if (value.empty() || !seen.insert(name).second) {
            if (error) *error = "signature sidecar field is empty or duplicated";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
        if (name == "algorithm") out->algorithm = value;
        else if (name == "key_id") out->key_id = value;
        else if (name == "sha256") out->sha256 = value;
        else if (name == "signature") out->value = value;
        else {
            if (error) *error = "signature sidecar contains an unknown field";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
    }
    if (out->algorithm.empty() || out->key_id.empty() ||
        out->sha256.empty() || out->value.empty() ||
        seen.size() != 4u) {
        if (error) *error = "signature sidecar is incomplete";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ReadSignedPackageRevocations(
    const fs::path& trust_root,
    PackageRevocations* out,
    std::string* error) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    out->key_ids.clear();
    out->releases.clear();
    const fs::path list_path = trust_root / "revocations.list";
    std::error_code ec;
    if (!fs::exists(list_path, ec) && !ec) return WOTBMOD_V3_OK;
    const uint64_t list_size = fs::file_size(list_path, ec);
    if (ec || list_size == 0u || list_size > 1024u * 1024u ||
        !fs::is_regular_file(list_path, ec) || ec ||
        IsReparsePoint(list_path)) {
        if (error) *error =
            "revocation list is unreadable, oversized, or reparse-backed";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    std::ifstream input(list_path, std::ios::binary);
    if (!input) {
        if (error) *error = "revocation list cannot be read";
        return WOTBMOD_V3_E_IO;
    }
    const std::string bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.size() != static_cast<size_t>(list_size)) {
        if (error) *error = "revocation list was truncated while reading";
        return WOTBMOD_V3_E_IO;
    }

    Sha256 digest;
    digest.Update(bytes.data(), bytes.size());
    const std::string digest_hex = digest.FinalHex();
    DetachedPackageSignature signature;
    WotbModV3Result result = ReadDetachedPackageSignature(
        fs::u8path(PathUtf8(list_path) + ".sig"),
        &signature,
        error);
    if (result != WOTBMOD_V3_OK) return result;
    if (LowerAscii(signature.algorithm) != "ecdsa-p256-sha256" ||
        !IsSha256(signature.sha256) ||
        LowerAscii(signature.sha256) != digest_hex) {
        if (error) *error =
            "revocation-list signature metadata does not match its bytes";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    const PackageTrustResult verified = VerifyTrustedP256Sha256(
        trust_root / "keys",
        signature.key_id,
        digest_hex,
        signature.value,
        error);
    if (verified != PackageTrustResult::Valid) {
        if (error && error->empty()) {
            *error = "revocation list is not signed by a trusted key";
        }
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }

    std::istringstream lines(bytes);
    std::string line;
    if (!std::getline(lines, line) ||
        PackageTrustTrim(line) != "WOTBMOD-REVOCATIONS-V1") {
        if (error) *error = "revocation-list header is invalid";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    while (std::getline(lines, line)) {
        line = PackageTrustTrim(line);
        if (line.empty()) continue;
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            if (error) *error = "revocation-list entry is invalid";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
        const std::string name = PackageTrustTrim(
            line.substr(0u, separator));
        const std::string value = PackageTrustTrim(
            line.substr(separator + 1u));
        if (name == "revoke_key") {
            if (!PackageTrustKeyIdValid(value) ||
                !out->key_ids.insert(LowerAscii(value)).second) {
                if (error) *error =
                    "revocation-list key entry is invalid or duplicated";
                return WOTBMOD_V3_E_SIGNATURE_INVALID;
            }
        } else if (name == "revoke_release") {
            const size_t at = value.find('@');
            if (at == std::string::npos ||
                value.find('@', at + 1u) != std::string::npos) {
                if (error) *error = "revoked release must be id@version";
                return WOTBMOD_V3_E_SIGNATURE_INVALID;
            }
            const std::string id = value.substr(0u, at);
            const std::string version = value.substr(at + 1u);
            if (!IsPackageId(id.c_str()) || version.empty() ||
                version.size() >= WOTBMOD_V3_MAX_VERSION ||
                !out->releases.insert(
                    LowerAscii(id) + "@" + version).second) {
                if (error) *error =
                    "revoked release is invalid or duplicated";
                return WOTBMOD_V3_E_SIGNATURE_INVALID;
            }
        } else {
            if (error) *error =
                "revocation list contains an unknown field";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
    }
    if (out->key_ids.count(LowerAscii(signature.key_id)) != 0u) {
        if (error) *error =
            "revocation list was signed by a key it revokes";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyPackageTrust(
    const PackagePreflightOptions& options,
    PackageCandidate* candidate,
    std::string* error) {
    if (!candidate) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const fs::path sidecar = PackageSignatureSidecar(*candidate);
    std::error_code ec;
    if (!fs::is_regular_file(sidecar, ec) || ec) {
        candidate->plan.warning_flags |= PACKAGE_WARNING_UNSIGNED;
        if (candidate->manifest &&
            candidate->manifest->has_signature != 0u) {
            candidate->plan.signature_status =
                WOTBMOD_V3_SIGNATURE_DECLARED;
            candidate->plan.warning_flags |=
                PACKAGE_WARNING_EMBEDDED_SIGNATURE_UNVERIFIED;
        }
        if ((options.flags &
             PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE) != 0u) {
            if (error) *error =
                "trusted detached package signature is required";
            return WOTBMOD_V3_E_SIGNATURE_INVALID;
        }
        return WOTBMOD_V3_OK;
    }

    DetachedPackageSignature signature;
    WotbModV3Result parsed = ReadDetachedPackageSignature(
        sidecar, &signature, error);
    if (parsed != WOTBMOD_V3_OK) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_INVALID;
        return parsed;
    }
    CopyFixed(candidate->plan.signature_key_id, signature.key_id);
    if (LowerAscii(signature.algorithm) !=
        "ecdsa-p256-sha256") {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_UNSUPPORTED;
        if (error) *error = "package signature algorithm is unsupported";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (!IsSha256(signature.sha256) ||
        LowerAscii(signature.sha256) !=
            LowerAscii(candidate->plan.package_sha256)) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_INVALID;
        if (error) *error = "signed package SHA-256 does not match content";
        return WOTBMOD_V3_E_HASH_MISMATCH;
    }

    PackageRevocations revocations;
    const WotbModV3Result revocation_result =
        ReadSignedPackageRevocations(
            fs::u8path(options.mods_root) / "trust",
            &revocations,
            error);
    if (revocation_result != WOTBMOD_V3_OK) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_INVALID;
        return revocation_result;
    }
    if (revocations.key_ids.count(
            LowerAscii(signature.key_id)) != 0u) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_INVALID;
        if (error) *error = "package signer is revoked";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    const std::string release =
        LowerAscii(candidate->plan.id) + "@" +
        candidate->plan.version;
    if (revocations.releases.count(release) != 0u) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_INVALID;
        if (error) *error = "package release is revoked";
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }

    const PackageTrustResult verified = VerifyTrustedP256Sha256(
        fs::u8path(options.mods_root) / "trust" / "keys",
        signature.key_id,
        LowerAscii(signature.sha256),
        signature.value,
        error);
    if (verified == PackageTrustResult::Valid) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_VALID;
        return WOTBMOD_V3_OK;
    }
    if (verified == PackageTrustResult::Untrusted) {
        candidate->plan.signature_status =
            WOTBMOD_V3_SIGNATURE_DECLARED;
        candidate->plan.warning_flags |=
            PACKAGE_WARNING_UNTRUSTED_SIGNER;
        if ((options.flags &
             PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE) == 0u) {
            return WOTBMOD_V3_OK;
        }
        return WOTBMOD_V3_E_SIGNATURE_INVALID;
    }
    candidate->plan.signature_status =
        verified == PackageTrustResult::Unsupported
        ? WOTBMOD_V3_SIGNATURE_UNSUPPORTED
        : WOTBMOD_V3_SIGNATURE_INVALID;
    return verified == PackageTrustResult::IoError
        ? WOTBMOD_V3_E_IO
        : WOTBMOD_V3_E_SIGNATURE_INVALID;
}

void Block(
    PackageCandidate* candidate,
    WotbModV3Result result,
    const std::string& reason) {
    if (!candidate) return;
    candidate->plan.status = PACKAGE_PLAN_BLOCKED;
    candidate->plan.result = result;
    candidate->plan.load_order = kNoLoadOrder;
    CopyFixed(candidate->plan.reason, reason);
}

/*
 * PACKAGE_PLAN_READY is deliberately a loadability state, not a signer-trust
 * verdict. Trust/authenticity limitations must remain visible in reason and
 * warning_flags for every candidate that reaches this state.
 */
void MarkLoadable(
    PackageCandidate* candidate,
    const std::string& reason) {
    if (!candidate) return;
    candidate->plan.status = PACKAGE_PLAN_READY;
    candidate->plan.result = WOTBMOD_V3_OK;
    candidate->plan.load_order = kNoLoadOrder;
    CopyFixed(candidate->plan.reason, reason);
}

bool CandidateReady(const PackageCandidate& candidate) {
    return candidate.plan.status == PACKAGE_PLAN_READY &&
           candidate.plan.result == WOTBMOD_V3_OK;
}

void CopyManifestToPlan(PackageCandidate* candidate) {
    if (!candidate || !candidate->manifest) return;
    candidate->plan.package_type =
        candidate->manifest->package_type;
    candidate->plan.requested_permission_tier =
        candidate->manifest->requested_permission_tier;
    candidate->plan.permission_count =
        candidate->manifest->permission_count;
    candidate->plan.dependency_count =
        candidate->manifest->dependency_count;
    CopyFixed(candidate->plan.id, candidate->manifest->id);
    CopyFixed(candidate->plan.name, candidate->manifest->name);
    CopyFixed(
        candidate->plan.version,
        candidate->manifest->version);
    CopyFixed(
        candidate->plan.developer,
        candidate->manifest->developer);
    for (uint32_t i = 0u;
         i < candidate->manifest->permission_count; ++i) {
        CopyFixed(
            candidate->plan.permissions[i],
            candidate->manifest->permissions[i]);
    }
    candidate->dependencies.assign(
        candidate->manifest->dependencies,
        candidate->manifest->dependencies +
            candidate->manifest->dependency_count);
}

bool ClientAllowed(
    const PackagePreflightOptions& options,
    const DataManifestPreflightSummary& manifest,
    std::string* reason) {
    if (manifest.client_build_count != 0u) {
        bool found = false;
        for (uint32_t i = 0u;
             i < manifest.client_build_count; ++i) {
            if (std::strcmp(
                    manifest.client_builds[i],
                    options.client_build) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (reason) {
                *reason =
                    "client build is not in manifest allowlist";
            }
            return false;
        }
    }
    if (manifest.client_executable_hash_count != 0u) {
        const std::string actual =
            LowerAscii(options.client_executable_sha256);
        bool found = false;
        for (uint32_t i = 0u;
             i < manifest.client_executable_hash_count; ++i) {
            if (LowerAscii(
                    manifest.client_executable_hashes[i]) ==
                actual) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (reason) {
                *reason =
                    "client executable SHA-256 is not in manifest allowlist";
            }
            return false;
        }
    }
    return true;
}

WotbModV3Result HashDirectoryPackage(
    const fs::path& root,
    const PackagePreflightLimits& limits,
    std::string* out_hash,
    std::string* error) {
    struct File {
        std::string relative;
        fs::path physical;
        uint64_t size = 0u;
    };
    std::vector<File> files;
    uint64_t total = 0u;
    std::error_code ec;
    for (fs::recursive_directory_iterator iterator(
             root,
             fs::directory_options::skip_permission_denied,
             ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        const fs::path path = iterator->path();
        if (IsReparsePoint(path) ||
            iterator->is_symlink(ec)) {
            if (error) {
                *error =
                    "directory package contains a reparse point";
            }
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
        if (ec) break;
        if (iterator->is_directory(ec)) {
            if (ec) break;
            continue;
        }
        if (!iterator->is_regular_file(ec) || ec) {
            if (error) {
                *error =
                    "directory package contains a non-regular file";
            }
            return ec
                ? WOTBMOD_V3_E_IO
                : WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        const uint64_t size = iterator->file_size(ec);
        if (ec) break;
        if (size >
                limits.max_archive_single_file_bytes ||
            size >
                limits.max_directory_package_bytes ||
            total >
                limits.max_directory_package_bytes - size) {
            if (error) {
                *error =
                    "directory package size limit exceeded";
            }
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        total += size;
        fs::path relative = fs::relative(path, root, ec);
        if (ec) break;
        File file;
        file.relative = PathUtf8(relative);
        file.physical = path;
        file.size = size;
        files.push_back(std::move(file));
        if (files.size() > limits.max_archive_entries) {
            if (error) {
                *error =
                    "directory package file-count limit exceeded";
            }
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
    }
    if (ec) {
        if (error) *error = "directory package enumeration failed";
        return WOTBMOD_V3_E_IO;
    }
    std::sort(
        files.begin(), files.end(),
        [](const File& left, const File& right) {
            return left.relative < right.relative;
        });
    Sha256 digest;
    for (const File& file : files) {
        const uint32_t path_size =
            static_cast<uint32_t>(file.relative.size());
        digest.Update(&path_size, sizeof(path_size));
        digest.Update(
            file.relative.data(), file.relative.size());
        digest.Update(&file.size, sizeof(file.size));
        std::ifstream input(file.physical, std::ios::binary);
        if (!input) {
            if (error) *error =
                "cannot read directory package file";
            return WOTBMOD_V3_E_IO;
        }
        if (!UpdateDigestFromStream(&input, &digest)) {
            if (error) *error =
                "directory package hashing failed";
            return WOTBMOD_V3_E_IO;
        }
    }
    *out_hash = digest.FinalHex();
    return WOTBMOD_V3_OK;
}

WotbModV3Result HashLoosePackage(
    const fs::path& manifest,
    const fs::path& payload,
    uint64_t max_bytes,
    std::string* out_hash,
    std::string* error) {
    struct File {
        std::string label;
        fs::path path;
    };
    std::vector<File> files = {
        {"manifest.json", manifest},
        {"payload", payload}
    };
    Sha256 digest;
    uint64_t total = 0u;
    for (const File& file : files) {
        std::error_code ec;
        const uint64_t size = fs::file_size(file.path, ec);
        if (ec || size > max_bytes ||
            total > max_bytes - size) {
            if (error) *error =
                ec ? "cannot query loose package file"
                   : "loose package size limit exceeded";
            return ec ? WOTBMOD_V3_E_IO
                      : WOTBMOD_V3_E_LIMIT_REACHED;
        }
        total += size;
        const uint32_t label_size =
            static_cast<uint32_t>(file.label.size());
        digest.Update(&label_size, sizeof(label_size));
        digest.Update(file.label.data(), file.label.size());
        digest.Update(&size, sizeof(size));
        std::ifstream input(file.path, std::ios::binary);
        if (!input) {
            if (error) *error =
                "cannot read loose package file";
            return WOTBMOD_V3_E_IO;
        }
        if (!UpdateDigestFromStream(&input, &digest)) {
            if (error) *error =
                "loose package hashing failed";
            return WOTBMOD_V3_E_IO;
        }
    }
    *out_hash = digest.FinalHex();
    return WOTBMOD_V3_OK;
}

bool ValidCatalogRecord(
    const WotbModV3CatalogRecord& record) {
    SemVer parsed_version;
    if (record.struct_size < sizeof(record) ||
        record.api_version != WOTBMOD_V3_CATALOG_VERSION ||
        record.status < WOTBMOD_V3_CATALOG_VERIFIED ||
        record.status > WOTBMOD_V3_CATALOG_BANNED ||
        !FixedTerminated(record.id, sizeof(record.id)) ||
        !FixedTerminated(
            record.version, sizeof(record.version)) ||
        !FixedTerminated(
            record.sha256, sizeof(record.sha256)) ||
        !FixedTerminated(
            record.reason, sizeof(record.reason)) ||
        !FixedTerminated(
            record.signer_id, sizeof(record.signer_id)) ||
        !IsPackageId(record.id) ||
        !ParseSemVer(record.version, &parsed_version)) {
        return false;
    }
    const bool hash_required =
        record.status == WOTBMOD_V3_CATALOG_VERIFIED ||
        record.status == WOTBMOD_V3_CATALOG_COMMUNITY ||
        record.status == WOTBMOD_V3_CATALOG_BANNED;
    if (hash_required && !IsSha256(record.sha256)) {
        return false;
    }
    if (record.status == WOTBMOD_V3_CATALOG_VERIFIED &&
        record.signer_id[0] == '\0') {
        return false;
    }
    return true;
}

WotbModV3Result EvaluateCatalog(
    const PackagePreflightOptions& options,
    PackageCandidate* candidate,
    std::string* reason) {
    const std::string package_hash =
        LowerAscii(candidate->plan.package_sha256);
    const WotbModV3CatalogRecord* exact = nullptr;
    for (uint32_t i = 0u;
         i < options.catalog_record_count; ++i) {
        const WotbModV3CatalogRecord& record =
            options.catalog_records[i];
        if (!ValidCatalogRecord(record)) {
            if (reason) *reason =
                "catalog contains an invalid record";
            return WOTBMOD_V3_E_PARSE;
        }
        if (record.status == WOTBMOD_V3_CATALOG_BANNED &&
            (LowerAscii(record.sha256) == package_hash ||
             std::strcmp(record.id, candidate->plan.id) == 0)) {
            candidate->plan.catalog_status =
                WOTBMOD_V3_CATALOG_BANNED;
            if (reason) {
                *reason = record.reason[0] != '\0'
                    ? record.reason
                    : "package is blocked by catalog";
            }
            return WOTBMOD_V3_E_INCOMPATIBLE;
        }
        if (std::strcmp(record.id, candidate->plan.id) == 0 &&
            std::strcmp(
                record.version,
                candidate->plan.version) == 0) {
            if (exact) {
                if (reason) *reason =
                    "catalog contains duplicate id/version records";
                return WOTBMOD_V3_E_CONFLICT;
            }
            exact = &record;
        }
    }
    if (!exact) {
        candidate->plan.catalog_status =
            WOTBMOD_V3_CATALOG_UNREVIEWED;
        candidate->plan.warning_flags |=
            PACKAGE_WARNING_UNREVIEWED;
        if ((options.flags &
             PACKAGE_PREFLIGHT_REQUIRE_CATALOG_RECORD) != 0u) {
            if (reason) *reason =
                "catalog record is required but missing";
            return WOTBMOD_V3_E_NOT_FOUND;
        }
        if ((options.flags &
             PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED) == 0u) {
            if (reason) *reason =
                "unreviewed sideload is disabled";
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
        return WOTBMOD_V3_OK;
    }
    candidate->plan.catalog_status = exact->status;
    if (exact->sha256[0] != '\0' &&
        LowerAscii(exact->sha256) != package_hash) {
        if (reason) *reason =
            "catalog package SHA-256 mismatch";
        return WOTBMOD_V3_E_HASH_MISMATCH;
    }
    if (exact->status == WOTBMOD_V3_CATALOG_UNREVIEWED) {
        candidate->plan.warning_flags |=
            PACKAGE_WARNING_UNREVIEWED;
        if ((options.flags &
             PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED) == 0u) {
            if (reason) *reason =
                "unreviewed catalog package is disabled";
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ResolveManifestPayload(
    PackageCandidate* candidate,
    std::string* error) {
    const char* relative_text =
        candidate->manifest->package_type ==
            WOTBMOD_V3_PACKAGE_NATIVE
        ? candidate->manifest->windows_x86_entrypoint
        : candidate->manifest->content_descriptor_path;
    if (!relative_text || *relative_text == '\0') {
        if (error) *error = "manifest payload path is empty";
        return WOTBMOD_V3_E_PARSE;
    }
    fs::path resolved;
    const fs::path requested =
        candidate->package_root / fs::u8path(relative_text);
    if (!CanonicalExisting(
            requested, candidate->package_root,
            true, &resolved) ||
        HasUnsafePathComponent(
            candidate->package_root, resolved)) {
        if (error) {
            *error =
                "manifest payload is missing, outside package root, or reparse-backed";
        }
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    if (candidate->manifest->package_type ==
        WOTBMOD_V3_PACKAGE_NATIVE) {
        if (LowerAscii(
                resolved.extension().u8string()) != ".dll") {
            if (error) {
                *error =
                    "windows-x86 entrypoint must be a DLL";
            }
            return WOTBMOD_V3_E_PLATFORM;
        }
        if (PathUtf8(resolved).size() >=
            WOTBMOD_V3_MAX_PATH) {
            if (error) *error =
                "resolved entrypoint path exceeds ABI limit";
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        CopyFixed(
            candidate->plan.entrypoint_path,
            PathUtf8(resolved));
    } else {
        if (PathUtf8(resolved).size() >=
            WOTBMOD_V3_MAX_PATH) {
            if (error) *error =
                "resolved content path exceeds ABI limit";
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        CopyFixed(
            candidate->plan.content_path,
            PathUtf8(resolved));
    }
    candidate->payload_file = resolved;
    std::string payload_hash;
    const WotbModV3Result hashed =
        HashFile(resolved, &payload_hash);
    if (hashed != WOTBMOD_V3_OK) {
        if (error) *error = "cannot hash package payload";
        return hashed;
    }
    CopyFixed(candidate->plan.payload_sha256, payload_hash);
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyCommonPolicy(
    const PackagePreflightOptions& options,
    PackageCandidate* candidate,
    std::string* error) {
    std::string client_reason;
    if (!ClientAllowed(
            options, *candidate->manifest,
            &client_reason)) {
        if (error) *error = client_reason;
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    uint32_t granted_tier = options.max_permission_tier;
    for (uint32_t i = 0u;
         i < options.permission_grant_count; ++i) {
        if (_stricmp(
                options.permission_grants[i].id,
                candidate->plan.id) == 0) {
            granted_tier =
                options.permission_grants[i]
                    .max_permission_tier;
            break;
        }
    }
    candidate->plan.granted_permission_tier =
        granted_tier;
    if (candidate->manifest->requested_permission_tier >
        granted_tier) {
        if (error) {
            *error =
                "manifest permission tier exceeds user grant";
        }
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    if (candidate->manifest->requested_permission_tier ==
        WOTBMOD_V3_PERMISSION_UNSAFE) {
        candidate->plan.warning_flags |=
            PACKAGE_WARNING_UNSAFE_PERMISSION_TIER;
    }
    std::string catalog_reason;
    const WotbModV3Result catalog = EvaluateCatalog(
        options, candidate, &catalog_reason);
    if (catalog != WOTBMOD_V3_OK) {
        if (error) *error = catalog_reason;
        return catalog;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result PreflightPhysicalManifest(
    const PackagePreflightOptions& options,
    const fs::path& manifest_path,
    const fs::path& package_root,
    uint32_t source_kind,
    bool directory_package,
    PackageCandidate* candidate) {
    InitializePlan(&candidate->plan);
    candidate->plan.source_kind = source_kind;
    candidate->package_root = package_root;
    candidate->manifest_file = manifest_path;
    if (PathUtf8(package_root).size() >=
            WOTBMOD_V3_MAX_PATH ||
        PathUtf8(manifest_path).size() >=
            WOTBMOD_V3_MAX_PATH) {
        Block(
            candidate, WOTBMOD_V3_E_LIMIT_REACHED,
            "physical package path exceeds ABI limit");
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    CopyFixed(
        candidate->plan.source_path,
        PathUtf8(directory_package
                 ? package_root : manifest_path));
    CopyFixed(
        candidate->plan.manifest_path,
        PathUtf8(manifest_path));
    candidate->manifest.reset(
        new DataManifestPreflightSummary());
    std::memset(
        candidate->manifest.get(), 0,
        sizeof(*candidate->manifest));
    candidate->manifest->struct_size =
        sizeof(*candidate->manifest);
    const WotbModV3Result parsed =
        PreflightManifestFile(
            PathUtf8(manifest_path).c_str(),
            candidate->manifest.get());
    if (parsed != WOTBMOD_V3_OK) {
        Block(
            candidate, parsed,
            candidate->manifest->error[0] != '\0'
            ? candidate->manifest->error
            : "manifest preflight failed");
        return parsed;
    }
    CopyManifestToPlan(candidate);
    std::string error;
    WotbModV3Result payload =
        ResolveManifestPayload(candidate, &error);
    if (payload != WOTBMOD_V3_OK) {
        Block(candidate, payload, error);
        return payload;
    }
    std::string package_hash;
    WotbModV3Result hashed = WOTBMOD_V3_OK;
    if (directory_package) {
        hashed = HashDirectoryPackage(
            package_root, options.limits,
            &package_hash, &error);
    } else {
        hashed = HashLoosePackage(
            manifest_path, candidate->payload_file,
            options.limits.max_directory_package_bytes,
            &package_hash, &error);
    }
    if (hashed != WOTBMOD_V3_OK) {
        Block(candidate, hashed, error);
        return hashed;
    }
    CopyFixed(candidate->plan.package_sha256, package_hash);
    const WotbModV3Result trusted =
        ApplyPackageTrust(options, candidate, &error);
    if (trusted != WOTBMOD_V3_OK) {
        Block(candidate, trusted, error);
        return trusted;
    }
    const WotbModV3Result policy =
        ApplyCommonPolicy(options, candidate, &error);
    if (policy != WOTBMOD_V3_OK) {
        Block(candidate, policy, error);
        return policy;
    }
    MarkLoadable(
        candidate,
        candidate->plan.signature_status ==
            WOTBMOD_V3_SIGNATURE_VALID
        ? "loadable: package preflight, policy, and trusted signature passed"
        : "loadable with explicit package trust warnings");
    return WOTBMOD_V3_OK;
}

bool ZipContainsPayload(
    const ZipPackage& zip,
    const char* relative,
    size_t* out_index) {
    if (!relative || !*relative) return false;
    std::wstring folded;
    if (!FoldWindowsUtf8(relative, &folded)) {
        return false;
    }
    for (size_t i = 0u; i < zip.entries.size(); ++i) {
        if (!zip.entries[i].directory &&
            zip.entries[i].folded_path == folded) {
            if (out_index) *out_index = i;
            return true;
        }
    }
    return false;
}

bool ReadMarker(
    const fs::path& marker,
    std::string* out) {
    std::ifstream input(marker, std::ios::binary);
    if (!input) return false;
    std::string value;
    std::getline(input, value);
    if (!input.eof() && !input) return false;
    if (out) *out = Trim(value);
    return true;
}

WotbModV3Result VerifyMaterializedZip(
    const ZipPackage& zip,
    const fs::path& root,
    const std::string& package_hash,
    std::string* error) {
    std::string marker;
    if (!ReadMarker(
            root / ".wotbmod-source.sha256",
            &marker) ||
        LowerAscii(marker) != LowerAscii(package_hash)) {
        if (error) {
            *error =
                "archive staging cache marker is missing or mismatched";
        }
        return WOTBMOD_V3_E_HASH_MISMATCH;
    }
    for (const ZipEntry& entry : zip.entries) {
        const fs::path target =
            root / fs::u8path(entry.path);
        if (entry.directory) {
            std::error_code ec;
            if (!fs::is_directory(target, ec) || ec ||
                IsReparsePoint(target)) {
                if (error) {
                    *error =
                        "archive staging directory is invalid";
                }
                return WOTBMOD_V3_E_HASH_MISMATCH;
            }
            continue;
        }
        fs::path canonical;
        if (!CanonicalExisting(
                target, root, true, &canonical) ||
            HasUnsafePathComponent(root, canonical)) {
            if (error) {
                *error =
                    "archive staging file is unsafe or missing";
            }
            return WOTBMOD_V3_E_HASH_MISMATCH;
        }
        std::error_code ec;
        if (fs::file_size(canonical, ec) !=
                entry.unpacked_size ||
            ec) {
            if (error) {
                *error =
                    "archive staging file size mismatch";
            }
            return WOTBMOD_V3_E_HASH_MISMATCH;
        }
        std::vector<uint8_t> bytes;
        WotbModV3Result read = ReadBoundedFile(
            canonical,
            entry.unpacked_size == 0u
                ? 1u : entry.unpacked_size,
            &bytes, error);
        if (entry.unpacked_size == 0u) {
            std::ifstream empty(canonical, std::ios::binary);
            if (!empty || empty.peek() !=
                    std::char_traits<char>::eof()) {
                if (error) *error =
                    "archive staging empty file mismatch";
                return WOTBMOD_V3_E_HASH_MISMATCH;
            }
        } else if (read != WOTBMOD_V3_OK ||
                   Crc32(bytes.data(), bytes.size()) !=
                       entry.crc32) {
            if (error) *error =
                "archive staging CRC32 mismatch";
            return WOTBMOD_V3_E_HASH_MISMATCH;
        }
    }
    return WOTBMOD_V3_OK;
}

bool SafeRemoveTemporary(
    const fs::path& staging_root,
    const fs::path& temporary) {
    if (temporary.parent_path() != staging_root ||
        temporary.filename().native().rfind(
            L".wotbmod-tmp-", 0u) != 0u) {
        return false;
    }
    std::error_code ec;
    fs::remove_all(temporary, ec);
    return !ec;
}

WotbModV3Result WriteZipToNewDirectory(
    const ZipPackage& zip,
    const fs::path& temporary,
    const std::string& package_hash,
    std::string* error) {
    std::error_code ec;
    if (!fs::create_directory(temporary, ec) || ec) {
        if (error) *error =
            "cannot create archive staging directory";
        return WOTBMOD_V3_E_IO;
    }
    for (const ZipEntry& entry : zip.entries) {
        const fs::path target =
            temporary / fs::u8path(entry.path);
        if (entry.directory) {
            fs::create_directories(target, ec);
            if (ec) {
                if (error) *error =
                    "cannot create archive staging subdirectory";
                return WOTBMOD_V3_E_IO;
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (error) *error =
                "cannot create archive staging parent";
            return WOTBMOD_V3_E_IO;
        }
        std::ofstream output(
            target, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error) *error =
                "cannot create archive staging file";
            return WOTBMOD_V3_E_IO;
        }
        if (entry.unpacked_size != 0u) {
            output.write(
                reinterpret_cast<const char*>(
                    zip.bytes.data() + entry.data_offset),
                static_cast<std::streamsize>(
                    entry.unpacked_size));
        }
        output.flush();
        if (!output) {
            if (error) *error =
                "cannot write archive staging file";
            return WOTBMOD_V3_E_IO;
        }
    }
    std::ofstream marker(
        temporary / ".wotbmod-source.sha256",
        std::ios::binary | std::ios::trunc);
    marker << package_hash << "\n";
    marker.flush();
    if (!marker) {
        if (error) *error =
            "cannot write archive staging marker";
        return WOTBMOD_V3_E_IO;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result MaterializeStoredZip(
    const PackagePreflightOptions& options,
    PackageCandidate* candidate,
    std::string* error) {
    if (options.archive_staging_root[0] == '\0') {
        if (error) *error =
            "archive materialization requires archive_staging_root";
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    fs::path staging =
        fs::u8path(options.archive_staging_root);
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (ec || IsReparsePoint(staging)) {
        if (error) *error =
            "archive staging root is unavailable or reparse-backed";
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    staging = fs::canonical(staging, ec);
    if (ec) {
        if (error) *error =
            "cannot canonicalize archive staging root";
        return WOTBMOD_V3_E_IO;
    }
    const fs::path mods_root =
        fs::canonical(fs::u8path(options.mods_root), ec);
    if (ec || staging == staging.root_path() ||
        staging == mods_root) {
        if (error) {
            *error =
                "archive staging root must be a dedicated non-root directory";
        }
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    const std::string package_hash =
        candidate->plan.package_sha256;
    const fs::path final =
        staging / fs::u8path(package_hash);
    if (fs::exists(final, ec) && !ec) {
        if (IsReparsePoint(final) ||
            !fs::is_directory(final, ec) || ec) {
            if (error) *error =
                "archive staging destination is unsafe";
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
        WotbModV3Result verified =
            VerifyMaterializedZip(
                candidate->zip, final,
                package_hash, error);
        if (verified != WOTBMOD_V3_OK) {
            return verified;
        }
    } else {
        if (ec) {
            if (error) *error =
                "cannot inspect archive staging destination";
            return WOTBMOD_V3_E_IO;
        }
        static std::atomic<uint32_t> sequence{0u};
        const std::wstring suffix =
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(sequence.fetch_add(1u));
        const fs::path temporary =
            staging / (L".wotbmod-tmp-" + suffix);
        WotbModV3Result written =
            WriteZipToNewDirectory(
                candidate->zip, temporary,
                package_hash, error);
        if (written != WOTBMOD_V3_OK) {
            SafeRemoveTemporary(staging, temporary);
            return written;
        }
        constexpr uint32_t kPublishAttempts = 8u;
        for (uint32_t attempt = 0u;
             attempt < kPublishAttempts;
             ++attempt) {
            ec.clear();
            fs::rename(temporary, final, ec);
            if (!ec) {
                break;
            }
            std::error_code published_error;
            if (fs::is_directory(final, published_error) &&
                !published_error) {
                break;
            }
            const int code = ec.value();
            const bool transient =
                code == ERROR_ACCESS_DENIED ||
                code == ERROR_SHARING_VIOLATION ||
                code == ERROR_LOCK_VIOLATION;
            if (!transient || attempt + 1u == kPublishAttempts) {
                break;
            }
            Sleep(50u * (attempt + 1u));
        }
        if (ec) {
            std::error_code exists_error;
            if (!fs::is_directory(final, exists_error) ||
                exists_error) {
                SafeRemoveTemporary(staging, temporary);
                if (error) {
                    *error =
                        "cannot publish archive staging directory "
                        "(win32=" +
                        std::to_string(ec.value()) + ")";
                }
                return WOTBMOD_V3_E_IO;
            }
            SafeRemoveTemporary(staging, temporary);
        }
        WotbModV3Result verified =
            VerifyMaterializedZip(
                candidate->zip, final,
                package_hash, error);
        if (verified != WOTBMOD_V3_OK) {
            return verified;
        }
    }
    candidate->package_root = final;
    candidate->manifest_file = final / "manifest.json";
    CopyFixed(
        candidate->plan.manifest_path,
        PathUtf8(candidate->manifest_file));
    const char* relative =
        candidate->manifest->package_type ==
            WOTBMOD_V3_PACKAGE_NATIVE
        ? candidate->manifest->windows_x86_entrypoint
        : candidate->manifest->content_descriptor_path;
    candidate->payload_file =
        final / fs::u8path(relative);
    if (PathUtf8(candidate->manifest_file).size() >=
            WOTBMOD_V3_MAX_PATH ||
        PathUtf8(candidate->payload_file).size() >=
            WOTBMOD_V3_MAX_PATH) {
        if (error) *error =
            "materialized archive path exceeds ABI limit";
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (candidate->manifest->package_type ==
        WOTBMOD_V3_PACKAGE_NATIVE) {
        CopyFixed(
            candidate->plan.entrypoint_path,
            PathUtf8(candidate->payload_file));
    } else {
        CopyFixed(
            candidate->plan.content_path,
            PathUtf8(candidate->payload_file));
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result PreflightArchive(
    const PackagePreflightOptions& options,
    const fs::path& archive_path,
    PackageCandidate* candidate) {
    InitializePlan(&candidate->plan);
    candidate->plan.source_kind =
        PACKAGE_SOURCE_STORED_WOTBMOD_ARCHIVE;
    candidate->plan.archive_compression_method = 0u;
    candidate->archive_file = archive_path;
    if (PathUtf8(archive_path).size() >=
        WOTBMOD_V3_MAX_PATH) {
        Block(
            candidate, WOTBMOD_V3_E_LIMIT_REACHED,
            "archive path exceeds ABI limit");
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    CopyFixed(
        candidate->plan.source_path,
        PathUtf8(archive_path));
    std::string error;
    WotbModV3Result parsed = ParseStoredZip(
        archive_path, options.limits,
        &candidate->zip, &error);
    if (parsed != WOTBMOD_V3_OK) {
        Block(candidate, parsed, error);
        return parsed;
    }
    Sha256 package_digest;
    package_digest.Update(
        candidate->zip.bytes.data(),
        candidate->zip.bytes.size());
    CopyFixed(
        candidate->plan.package_sha256,
        package_digest.FinalHex());
    const ZipEntry& manifest_entry =
        candidate->zip.entries[
            candidate->zip.manifest_index];
    candidate->manifest.reset(
        new DataManifestPreflightSummary());
    std::memset(
        candidate->manifest.get(), 0,
        sizeof(*candidate->manifest));
    candidate->manifest->struct_size =
        sizeof(*candidate->manifest);
    parsed = PreflightManifestUtf8(
        candidate->zip.bytes.data() +
            manifest_entry.data_offset,
        manifest_entry.unpacked_size,
        candidate->manifest.get());
    if (parsed != WOTBMOD_V3_OK) {
        Block(
            candidate, parsed,
            candidate->manifest->error[0] != '\0'
            ? candidate->manifest->error
            : "archive manifest preflight failed");
        return parsed;
    }
    CopyManifestToPlan(candidate);
    const char* payload_relative =
        candidate->manifest->package_type ==
            WOTBMOD_V3_PACKAGE_NATIVE
        ? candidate->manifest->windows_x86_entrypoint
        : candidate->manifest->content_descriptor_path;
    size_t payload_index = 0u;
    if (!ZipContainsPayload(
            candidate->zip,
            payload_relative,
            &payload_index)) {
        Block(
            candidate, WOTBMOD_V3_E_NOT_FOUND,
            "archive manifest payload is missing");
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    if (candidate->manifest->package_type ==
            WOTBMOD_V3_PACKAGE_NATIVE &&
        LowerAscii(
            fs::u8path(payload_relative)
                .extension().u8string()) != ".dll") {
        Block(
            candidate, WOTBMOD_V3_E_PLATFORM,
            "windows-x86 archive entrypoint must be a DLL");
        return WOTBMOD_V3_E_PLATFORM;
    }
    const ZipEntry& payload =
        candidate->zip.entries[payload_index];
    Sha256 payload_digest;
    payload_digest.Update(
        candidate->zip.bytes.data() + payload.data_offset,
        payload.unpacked_size);
    CopyFixed(
        candidate->plan.payload_sha256,
        payload_digest.FinalHex());
    parsed = ApplyPackageTrust(
        options, candidate, &error);
    if (parsed != WOTBMOD_V3_OK) {
        Block(candidate, parsed, error);
        return parsed;
    }
    parsed = ApplyCommonPolicy(
        options, candidate, &error);
    if (parsed != WOTBMOD_V3_OK) {
        Block(candidate, parsed, error);
        return parsed;
    }
    if ((options.flags &
         PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES) == 0u) {
        Block(
            candidate, WOTBMOD_V3_E_NOT_SUPPORTED,
            "stored .wotbmod validated, but materialization is disabled");
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    parsed = MaterializeStoredZip(
        options, candidate, &error);
    if (parsed != WOTBMOD_V3_OK) {
        Block(candidate, parsed, error);
        return parsed;
    }
    MarkLoadable(
        candidate,
        candidate->plan.signature_status ==
            WOTBMOD_V3_SIGNATURE_VALID
        ? "loadable after validation, trusted signature verification, and materialization"
        : "loadable after validation and materialization with explicit package trust warnings");
    return WOTBMOD_V3_OK;
}

bool IsLooseManifestName(const fs::path& path) {
    const std::string name =
        LowerAscii(path.filename().u8string());
    if (name == "manifest.json") return true;
    static const std::string manifest_suffix =
        ".manifest.json";
    return name.size() > manifest_suffix.size() &&
            name.compare(
                name.size() - manifest_suffix.size(),
                manifest_suffix.size(),
                manifest_suffix) == 0;
}

WotbModV3Result DiscoverCandidates(
    const PackagePreflightOptions& options,
    const fs::path& root,
    std::vector<PackageCandidate>* candidates,
    std::string* error) {
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator iterator(
             root,
             fs::directory_options::skip_permission_denied,
             ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        children.push_back(iterator->path());
    }
    if (ec) {
        if (error) *error = "cannot enumerate mods root";
        return WOTBMOD_V3_E_IO;
    }
    std::sort(
        children.begin(), children.end(),
        [](const fs::path& left, const fs::path& right) {
            return LowerAscii(PathUtf8(left)) <
                   LowerAscii(PathUtf8(right));
        });
    for (const fs::path& child : children) {
        if (IsReparsePoint(child)) {
            continue;
        }
        if (fs::is_directory(child, ec) && !ec) {
            const fs::path manifest = child / "manifest.json";
            if (!fs::is_regular_file(manifest, ec) || ec) {
                ec.clear();
                continue;
            }
            fs::path canonical_directory;
            fs::path canonical_manifest;
            if (!CanonicalExisting(
                    child, root, false,
                    &canonical_directory) ||
                !CanonicalExisting(
                    manifest, canonical_directory, true,
                    &canonical_manifest) ||
                HasUnsafePathComponent(
                    canonical_directory,
                    canonical_manifest)) {
                if (candidates->size() >=
                    options.limits.max_candidates) {
                    if (error) *error =
                        "package candidate limit exceeded";
                    return WOTBMOD_V3_E_LIMIT_REACHED;
                }
                std::unique_ptr<PackageCandidate> candidate(
                    new PackageCandidate());
                InitializePlan(&candidate->plan);
                candidate->plan.source_kind =
                    PACKAGE_SOURCE_DIRECTORY;
                CopyFixed(
                    candidate->plan.source_path,
                    PathUtf8(child));
                Block(
                    candidate.get(),
                    WOTBMOD_V3_E_PERMISSION_DENIED,
                    "directory package path is unsafe");
                candidates->push_back(
                    std::move(*candidate));
                continue;
            }
            if (candidates->size() >=
                options.limits.max_candidates) {
                if (error) *error =
                    "package candidate limit exceeded";
                return WOTBMOD_V3_E_LIMIT_REACHED;
            }
            std::unique_ptr<PackageCandidate> candidate(
                new PackageCandidate());
            PreflightPhysicalManifest(
                options, canonical_manifest,
                canonical_directory,
                PACKAGE_SOURCE_DIRECTORY,
                true, candidate.get());
            candidates->push_back(
                std::move(*candidate));
            continue;
        }
        ec.clear();
        if (!fs::is_regular_file(child, ec) || ec) {
            ec.clear();
            continue;
        }
        fs::path canonical_file;
        if (!CanonicalExisting(
                child, root, true, &canonical_file)) {
            continue;
        }
        const std::string extension =
            LowerAscii(canonical_file.extension().u8string());
        if (extension == ".wotbmod") {
            if (candidates->size() >=
                options.limits.max_candidates) {
                if (error) *error =
                    "package candidate limit exceeded";
                return WOTBMOD_V3_E_LIMIT_REACHED;
            }
            std::unique_ptr<PackageCandidate> candidate(
                new PackageCandidate());
            PreflightArchive(
                options, canonical_file,
                candidate.get());
            candidates->push_back(
                std::move(*candidate));
        } else if (IsLooseManifestName(canonical_file)) {
            if (candidates->size() >=
                options.limits.max_candidates) {
                if (error) *error =
                    "package candidate limit exceeded";
                return WOTBMOD_V3_E_LIMIT_REACHED;
            }
            std::unique_ptr<PackageCandidate> candidate(
                new PackageCandidate());
            PreflightPhysicalManifest(
                options, canonical_file, root,
                PACKAGE_SOURCE_SIDECAR_MANIFEST,
                false, candidate.get());
            candidates->push_back(
                std::move(*candidate));
        }
    }
    return WOTBMOD_V3_OK;
}

struct InstalledVersion {
    std::string version;
};

WotbModV3Result BuildInstalledMap(
    const PackagePreflightOptions& options,
    std::map<std::string, InstalledVersion>* installed,
    std::string* error) {
    for (uint32_t i = 0u;
         i < options.installed_mod_count; ++i) {
        const WotbModV3InstalledMod& mod =
            options.installed_mods[i];
        SemVer parsed;
        if (mod.struct_size < sizeof(mod) ||
            mod.api_version != WOTBMOD_V3_MANIFEST_VERSION ||
            !FixedTerminated(mod.id, sizeof(mod.id)) ||
            !FixedTerminated(
                mod.version, sizeof(mod.version)) ||
            mod.id[0] == '\0' ||
            !ParseSemVer(mod.version, &parsed)) {
            if (error) *error =
                "installed mod list contains an invalid record";
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        if (mod.enabled == 0u) continue;
        if (!installed->emplace(
                mod.id,
                InstalledVersion{mod.version}).second) {
            if (error) *error =
                "installed mod list contains duplicate ids";
            return WOTBMOD_V3_E_CONFLICT;
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ValidatePermissionGrants(
    const PackagePreflightOptions& options,
    std::string* error) {
    std::set<std::string> ids;
    for (uint32_t i = 0u;
         i < options.permission_grant_count; ++i) {
        const PackagePermissionGrant& grant =
            options.permission_grants[i];
        if (grant.struct_size < sizeof(grant) ||
            grant.api_version !=
                WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION ||
            grant.max_permission_tier >
                WOTBMOD_V3_PERMISSION_UNSAFE ||
            !FixedTerminated(grant.id, sizeof(grant.id)) ||
            !IsPackageId(grant.id)) {
            if (error) *error =
                "permission grant list contains an invalid record";
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        if (!ids.insert(LowerAscii(grant.id)).second) {
            if (error) *error =
                "permission grant list contains duplicate ids";
            return WOTBMOD_V3_E_CONFLICT;
        }
    }
    return WOTBMOD_V3_OK;
}

void BlockDuplicateIds(
    std::vector<PackageCandidate>* candidates) {
    std::map<std::string, std::vector<size_t>> by_id;
    for (size_t i = 0u; i < candidates->size(); ++i) {
        if (CandidateReady((*candidates)[i]) &&
            (*candidates)[i].plan.id[0] != '\0') {
            by_id[(*candidates)[i].plan.id].push_back(i);
        }
    }
    for (const auto& item : by_id) {
        if (item.second.size() < 2u) continue;
        for (size_t index : item.second) {
            Block(
                &(*candidates)[index],
                WOTBMOD_V3_E_CONFLICT,
                "duplicate package id discovered");
        }
    }
}

bool FindReadyCandidate(
    const std::vector<PackageCandidate>& candidates,
    const char* id,
    size_t* out_index) {
    for (size_t i = 0u; i < candidates.size(); ++i) {
        if (CandidateReady(candidates[i]) &&
            std::strcmp(candidates[i].plan.id, id) == 0) {
            if (out_index) *out_index = i;
            return true;
        }
    }
    return false;
}

bool DependencySatisfied(
    const std::vector<PackageCandidate>& candidates,
    const std::map<std::string, InstalledVersion>& installed,
    const WotbModV3ManifestDependency& dependency,
    size_t* candidate_index,
    bool* present) {
    size_t index = 0u;
    if (FindReadyCandidate(
            candidates, dependency.id, &index)) {
        if (candidate_index) *candidate_index = index;
        if (present) *present = true;
        return VersionMatchesRange(
            candidates[index].plan.version,
            dependency.version_range);
    }
    const auto existing = installed.find(dependency.id);
    if (existing != installed.end()) {
        if (present) *present = true;
        return VersionMatchesRange(
            existing->second.version,
            dependency.version_range);
    }
    if (present) *present = false;
    return false;
}

bool ResolveDependencyFailures(
    const std::map<std::string, InstalledVersion>& installed,
    std::vector<PackageCandidate>* candidates) {
    bool changed = false;
    for (PackageCandidate& candidate : *candidates) {
        if (!CandidateReady(candidate)) continue;
        for (const WotbModV3ManifestDependency& dependency :
             candidate.dependencies) {
            bool present = false;
            const bool matches = DependencySatisfied(
                *candidates, installed, dependency,
                nullptr, &present);
            if (dependency.kind ==
                WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE) {
                if (present && matches) {
                    Block(
                        &candidate,
                        WOTBMOD_V3_E_INCOMPATIBLE,
                        std::string(
                            "incompatible package is active: ") +
                            dependency.id);
                    changed = true;
                    break;
                }
            } else if (
                dependency.kind ==
                    WOTBMOD_V3_DEPENDENCY_REQUIRED ||
                (dependency.kind ==
                    WOTBMOD_V3_DEPENDENCY_OPTIONAL &&
                 present)) {
                if (!present || !matches) {
                    Block(
                        &candidate,
                        WOTBMOD_V3_E_DEPENDENCY_MISSING,
                        std::string(
                            present
                            ? "dependency version mismatch: "
                            : "required dependency missing: ") +
                            dependency.id);
                    changed = true;
                    break;
                }
            }
        }
    }
    return changed;
}

bool CandidateLess(
    const PackageCandidate& left,
    const PackageCandidate& right) {
    const int id = std::strcmp(
        left.plan.id, right.plan.id);
    if (id != 0) return id < 0;
    const int version = std::strcmp(
        left.plan.version, right.plan.version);
    if (version != 0) return version < 0;
    return LowerAscii(left.plan.source_path) <
           LowerAscii(right.plan.source_path);
}

bool AssignTopologicalOrder(
    const std::map<std::string, InstalledVersion>& installed,
    std::vector<PackageCandidate>* candidates) {
    const size_t count = candidates->size();
    std::vector<std::vector<size_t>> dependents(count);
    std::vector<uint32_t> indegree(count, 0u);
    uint32_t ready_count = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (!CandidateReady((*candidates)[i])) continue;
        ++ready_count;
        for (const WotbModV3ManifestDependency& dependency :
             (*candidates)[i].dependencies) {
            if (dependency.kind ==
                WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE) {
                continue;
            }
            size_t dependency_index = 0u;
            bool present = false;
            const bool matches = DependencySatisfied(
                *candidates, installed, dependency,
                &dependency_index, &present);
            if (present && matches &&
                CandidateReady(
                    (*candidates)[dependency_index]) &&
                std::strcmp(
                    (*candidates)[dependency_index].plan.id,
                    dependency.id) == 0) {
                dependents[dependency_index].push_back(i);
                ++indegree[i];
            }
        }
    }
    std::vector<size_t> available;
    for (size_t i = 0u; i < count; ++i) {
        if (CandidateReady((*candidates)[i]) &&
            indegree[i] == 0u) {
            available.push_back(i);
        }
    }
    auto sort_available = [&]() {
        std::sort(
            available.begin(), available.end(),
            [&](size_t left, size_t right) {
                return CandidateLess(
                    (*candidates)[right],
                    (*candidates)[left]);
            });
    };
    sort_available();
    uint32_t order = 0u;
    while (!available.empty()) {
        const size_t index = available.back();
        available.pop_back();
        (*candidates)[index].plan.load_order = order++;
        for (size_t dependent : dependents[index]) {
            if (--indegree[dependent] == 0u) {
                available.push_back(dependent);
                sort_available();
            }
        }
    }
    if (order == ready_count) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (CandidateReady((*candidates)[i]) &&
            indegree[i] != 0u) {
            Block(
                &(*candidates)[i],
                WOTBMOD_V3_E_CONFLICT,
                "dependency cycle detected");
        }
    }
    return true;
}

void ResolveDependenciesAndOrder(
    const std::map<std::string, InstalledVersion>& installed,
    std::vector<PackageCandidate>* candidates) {
    while (ResolveDependencyFailures(
        installed, candidates)) {
    }
    while (AssignTopologicalOrder(
        installed, candidates)) {
        while (ResolveDependencyFailures(
            installed, candidates)) {
        }
    }
}

bool ValidateLimits(
    const PackagePreflightLimits& limits) {
    return limits.struct_size >= sizeof(limits) &&
           limits.api_version ==
               WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION &&
           limits.max_candidates != 0u &&
           limits.max_candidates <=
               WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES &&
           limits.max_archive_entries != 0u &&
           limits.max_archive_entries <= 65534u &&
           limits.max_archive_depth != 0u &&
           limits.max_archive_depth <= 64u &&
           limits.max_archive_bytes >= 22u &&
           limits.max_archive_unpacked_bytes != 0u &&
           limits.max_archive_single_file_bytes != 0u &&
           limits.max_archive_single_file_bytes <=
               limits.max_archive_unpacked_bytes &&
           limits.max_directory_package_bytes != 0u;
}

WotbModV3Result ValidateOptions(
    const PackagePreflightOptions* options,
    PackagePlanSummary* summary,
    PackagePreflightOptions* normalized) {
    if (!options || !summary || !normalized) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!ValidStruct(
            options,
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION) ||
        !ValidStruct(
            summary,
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION) ||
        !FixedTerminated(
            options->mods_root,
            sizeof(options->mods_root)) ||
        !FixedTerminated(
            options->archive_staging_root,
            sizeof(options->archive_staging_root)) ||
        !FixedTerminated(
            options->client_build,
            sizeof(options->client_build)) ||
        !FixedTerminated(
            options->client_executable_sha256,
            sizeof(options->client_executable_sha256)) ||
        options->mods_root[0] == '\0' ||
        (options->flags &
         ~(PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED |
           PACKAGE_PREFLIGHT_REQUIRE_CATALOG_RECORD |
           PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES |
           PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE)) != 0u ||
        options->max_permission_tier >
            WOTBMOD_V3_PERMISSION_UNSAFE ||
        (options->installed_mod_count != 0u &&
         !options->installed_mods) ||
        (options->permission_grant_count != 0u &&
         !options->permission_grants) ||
        (options->catalog_record_count != 0u &&
         !options->catalog_records) ||
        options->installed_mod_count >
            WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES ||
        options->permission_grant_count >
            WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES ||
        options->catalog_record_count > 65536u ||
        (options->client_executable_sha256[0] != '\0' &&
         !IsSha256(
             options->client_executable_sha256))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *normalized = *options;
    if (options->limits.struct_size == 0u) {
        normalized->limits =
            DefaultPackagePreflightLimits();
    } else if (!ValidateLimits(options->limits)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if ((options->flags &
         PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES) != 0u &&
        options->archive_staging_root[0] == '\0') {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return WOTBMOD_V3_OK;
}

}  // namespace

PackagePreflightLimits DefaultPackagePreflightLimits() {
    PackagePreflightLimits limits = {};
    limits.struct_size = sizeof(limits);
    limits.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    limits.max_candidates =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES;
    limits.max_archive_entries = 4096u;
    limits.max_archive_depth = 16u;
    limits.max_archive_bytes =
        512ull * 1024ull * 1024ull;
    limits.max_archive_unpacked_bytes =
        512ull * 1024ull * 1024ull;
    limits.max_archive_single_file_bytes =
        128ull * 1024ull * 1024ull;
    limits.max_directory_package_bytes =
        512ull * 1024ull * 1024ull;
    return limits;
}

static WotbModV3Result Sha256FileUtf8Unchecked(
    const char* physical_file,
    char out_sha256[65]) {
    if (!physical_file || !*physical_file ||
        !out_sha256) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const fs::path path = fs::u8path(physical_file);
    std::error_code ec;
    if (IsReparsePoint(path) ||
        !fs::is_regular_file(path, ec) || ec) {
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    std::string hash;
    const WotbModV3Result result =
        HashFile(path, &hash);
    if (result == WOTBMOD_V3_OK) {
        std::memcpy(out_sha256, hash.c_str(), 65u);
    }
    return result;
}

static WotbModV3Result BuildPackageLoadPlanUnchecked(
    const PackagePreflightOptions* options,
    PackagePlanEntry* out_entries,
    uint32_t entry_capacity,
    PackagePlanSummary* out_summary) {
    if (!out_summary) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint32_t caller_summary_size =
        out_summary->struct_size;
    const uint32_t caller_summary_version =
        out_summary->api_version;
    if (caller_summary_size < sizeof(*out_summary) ||
        caller_summary_version !=
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memset(out_summary, 0, sizeof(*out_summary));
    out_summary->struct_size = sizeof(*out_summary);
    out_summary->api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    PackagePreflightOptions normalized = {};
    WotbModV3Result validated = ValidateOptions(
        options, out_summary, &normalized);
    if (validated != WOTBMOD_V3_OK) {
        CopyFixed(
            out_summary->reason,
            "invalid package preflight options");
        return validated;
    }
    std::error_code ec;
    if (IsReparsePoint(
            fs::u8path(normalized.mods_root))) {
        CopyFixed(
            out_summary->reason,
            "mods root cannot be a reparse point");
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    const fs::path root =
        fs::canonical(
            fs::u8path(normalized.mods_root), ec);
    if (ec || !fs::is_directory(root, ec) || ec) {
        CopyFixed(
            out_summary->reason,
            "mods root is not an accessible directory");
        return WOTBMOD_V3_E_IO;
    }
    if (root == root.root_path()) {
        CopyFixed(
            out_summary->reason,
            "filesystem root cannot be used as mods root");
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    std::map<std::string, InstalledVersion> installed;
    std::string error;
    validated = ValidatePermissionGrants(
        normalized, &error);
    if (validated != WOTBMOD_V3_OK) {
        CopyFixed(out_summary->reason, error);
        return validated;
    }
    validated = BuildInstalledMap(
        normalized, &installed, &error);
    if (validated != WOTBMOD_V3_OK) {
        CopyFixed(out_summary->reason, error);
        return validated;
    }
    std::vector<PackageCandidate> candidates;
    PackagePreflightOptions discovery_options = normalized;
    if (!out_entries && entry_capacity == 0u) {
        discovery_options.flags &=
            ~PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
    }
    validated = DiscoverCandidates(
        discovery_options, root, &candidates, &error);
    if (validated != WOTBMOD_V3_OK) {
        CopyFixed(out_summary->reason, error);
        return validated;
    }
    BlockDuplicateIds(&candidates);
    ResolveDependenciesAndOrder(installed, &candidates);
    std::sort(
        candidates.begin(), candidates.end(),
        [](const PackageCandidate& left,
           const PackageCandidate& right) {
            if (CandidateReady(left) !=
                CandidateReady(right)) {
                return CandidateReady(left);
            }
            if (CandidateReady(left) &&
                left.plan.load_order !=
                    right.plan.load_order) {
                return left.plan.load_order <
                       right.plan.load_order;
            }
            return CandidateLess(left, right);
        });
    out_summary->discovered_count =
        static_cast<uint32_t>(candidates.size());
    out_summary->required_capacity =
        out_summary->discovered_count;
    for (const PackageCandidate& candidate : candidates) {
        if (CandidateReady(candidate)) {
            ++out_summary->ready_count;
        } else {
            ++out_summary->blocked_count;
        }
        if (candidate.plan.warning_flags != 0u) {
            ++out_summary->warning_count;
        }
    }
    const uint32_t writable =
        (std::min)(
            entry_capacity,
            out_summary->discovered_count);
    if (writable != 0u && !out_entries) {
        CopyFixed(
            out_summary->reason,
            "package plan output buffer is null");
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0u; i < writable; ++i) {
        out_entries[i] = candidates[i].plan;
    }
    if (entry_capacity <
        out_summary->discovered_count) {
        CopyFixed(
            out_summary->reason,
            "package plan output buffer is too small");
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    CopyFixed(
        out_summary->reason,
        out_summary->blocked_count == 0u
        ? "package load plan is ready"
        : "package load plan built; blocked entries contain reasons");
    return WOTBMOD_V3_OK;
}

static void SetExceptionalSummary(
    PackagePlanSummary* summary,
    const char* reason) {
    if (!summary ||
        summary->struct_size < sizeof(*summary) ||
        summary->api_version !=
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION) {
        return;
    }
    std::memset(summary, 0, sizeof(*summary));
    summary->struct_size = sizeof(*summary);
    summary->api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    CopyFixed(
        summary->reason,
        reason ? reason : "package preflight failed");
}

WotbModV3Result Sha256FileUtf8(
    const char* physical_file,
    char out_sha256[65]) {
    try {
        return Sha256FileUtf8Unchecked(
            physical_file, out_sha256);
    } catch (const std::bad_alloc&) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    } catch (const fs::filesystem_error&) {
        return WOTBMOD_V3_E_IO;
    } catch (...) {
        return WOTBMOD_V3_E_IO;
    }
}

WotbModV3Result BuildPackageLoadPlan(
    const PackagePreflightOptions* options,
    PackagePlanEntry* out_entries,
    uint32_t entry_capacity,
    PackagePlanSummary* out_summary) {
    try {
        return BuildPackageLoadPlanUnchecked(
            options, out_entries,
            entry_capacity, out_summary);
    } catch (const std::bad_alloc&) {
        SetExceptionalSummary(
            out_summary,
            "package preflight memory limit reached");
        return WOTBMOD_V3_E_LIMIT_REACHED;
    } catch (const fs::filesystem_error&) {
        SetExceptionalSummary(
            out_summary,
            "package preflight filesystem failure");
        return WOTBMOD_V3_E_IO;
    } catch (...) {
        SetExceptionalSummary(
            out_summary,
            "package preflight unexpected failure");
        return WOTBMOD_V3_E_IO;
    }
}

}  // namespace v3
}  // namespace wotbmod
