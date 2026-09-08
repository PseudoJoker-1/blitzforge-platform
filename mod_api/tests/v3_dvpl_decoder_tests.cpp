#include "../src/v3/dvpl_decoder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;

#define CHECK(expression)                                                \
    do {                                                                 \
        if (expression) {                                                \
            ++g_passed;                                                  \
        } else {                                                         \
            ++g_failed;                                                  \
            std::fprintf(                                                \
                stderr,                                                  \
                "check failed at line %d: %s\n",                        \
                __LINE__,                                                \
                #expression);                                            \
        }                                                                \
    } while (0)

uint32_t Crc32(const uint8_t* bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask =
                0u - static_cast<uint32_t>(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void AppendLe32(std::vector<uint8_t>* bytes, uint32_t value) {
    bytes->push_back(static_cast<uint8_t>(value));
    bytes->push_back(static_cast<uint8_t>(value >> 8u));
    bytes->push_back(static_cast<uint8_t>(value >> 16u));
    bytes->push_back(static_cast<uint8_t>(value >> 24u));
}

std::vector<uint8_t> MakeDvpl(
    const std::vector<uint8_t>& payload,
    uint32_t uncompressed_size,
    uint32_t compression_type) {
    std::vector<uint8_t> bytes = payload;
    AppendLe32(&bytes, uncompressed_size);
    AppendLe32(
        &bytes, static_cast<uint32_t>(payload.size()));
    AppendLe32(&bytes, Crc32(payload.data(), payload.size()));
    AppendLe32(&bytes, compression_type);
    AppendLe32(&bytes, 0x4C505644u);
    return bytes;
}

void TestRaw() {
    const std::vector<uint8_t> payload = {'r', 'a', 'w'};
    const std::vector<uint8_t> file =
        MakeDvpl(payload, 3u, 0u);
    std::vector<uint8_t> output;
    std::string error;
    wotbmod::v3::DvplDecodeInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_DVPL_DECODER_VERSION;
    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            1024u,
            &output,
            &info,
            &error) == WOTBMOD_V3_OK);
    CHECK(output == payload);
    CHECK(info.compression_type == 0u);
    CHECK(info.compressed_size == 3u);
    CHECK(info.uncompressed_size == 3u);
    CHECK(error.empty());
}

void TestLz4LiteralOnly() {
    const std::vector<uint8_t> payload = {
        0xF0u, 0x01u,
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::vector<uint8_t> file =
        MakeDvpl(payload, 16u, 2u);
    std::vector<uint8_t> output;
    std::string error;
    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            16u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_OK);
    CHECK(
        std::string(output.begin(), output.end()) ==
        "0123456789abcdef");
}

void TestLz4Match() {
    const std::vector<uint8_t> payload = {
        0x35u, 'a', 'b', 'c', 0x03u, 0x00u};
    const std::vector<uint8_t> file =
        MakeDvpl(payload, 12u, 1u);
    std::vector<uint8_t> output;
    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            12u,
            &output,
            nullptr,
            nullptr) == WOTBMOD_V3_OK);
    CHECK(
        std::string(output.begin(), output.end()) ==
        "abcabcabcabc");
}

void TestFailures() {
    const std::vector<uint8_t> payload = {
        0x30u, 'a', 'b', 'c'};
    std::vector<uint8_t> file =
        MakeDvpl(payload, 3u, 2u);
    std::vector<uint8_t> output;
    std::string error;

    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            2u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_LIMIT_REACHED);
    CHECK(output.empty());

    file[file.size() - 1u] ^= 0xFFu;
    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            32u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_PARSE);
    file[file.size() - 1u] ^= 0xFFu;

    file[0] ^= 1u;
    CHECK(
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            32u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_HASH_MISMATCH);
    file[0] ^= 1u;

    std::vector<uint8_t> streaming =
        MakeDvpl(payload, 3u, 4u);
    CHECK(
        wotbmod::v3::DecodeDvpl(
            streaming.data(),
            streaming.size(),
            32u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_NOT_SUPPORTED);

    std::vector<uint8_t> invalid_offset = {
        0x00u, 0x00u, 0x00u};
    std::vector<uint8_t> invalid_file =
        MakeDvpl(invalid_offset, 4u, 2u);
    CHECK(
        wotbmod::v3::DecodeDvpl(
            invalid_file.data(),
            invalid_file.size(),
            32u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_PARSE);

    CHECK(
        wotbmod::v3::DecodeDvpl(
            nullptr,
            0u,
            32u,
            &output,
            nullptr,
            &error) == WOTBMOD_V3_E_INVALID_ARGUMENT);
}

void TestExternalFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    if (!input) return;
    const std::vector<uint8_t> file(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::vector<uint8_t> output;
    std::string error;
    wotbmod::v3::DvplDecodeInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_DVPL_DECODER_VERSION;
    const WotbModV3Result result =
        wotbmod::v3::DecodeDvpl(
            file.data(),
            file.size(),
            512u * 1024u * 1024u,
            &output,
            &info,
            &error);
    if (result != WOTBMOD_V3_OK) {
        std::fprintf(
            stderr,
            "external DVPL decode failed: result=%d error=%s\n",
            static_cast<int>(result),
            error.c_str());
    }
    CHECK(result == WOTBMOD_V3_OK);
    CHECK(output.size() == info.uncompressed_size);
    CHECK(!output.empty());
    if (result == WOTBMOD_V3_OK) {
        std::printf(
            "Decoded external DVPL: type=%u compressed=%llu unpacked=%llu\n",
            info.compression_type,
            static_cast<unsigned long long>(info.compressed_size),
            static_cast<unsigned long long>(info.uncompressed_size));
    }
}

}  // namespace

int main(int argc, char** argv) {
    TestRaw();
    TestLz4LiteralOnly();
    TestLz4Match();
    TestFailures();
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        TestExternalFile(argv[1]);
    }
    std::printf(
        "V3 DVPL decoder: %u passed, %u failed\n",
        g_passed,
        g_failed);
    return g_failed == 0u ? 0 : 1;
}
