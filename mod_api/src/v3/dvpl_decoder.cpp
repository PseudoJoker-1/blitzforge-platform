#include "dvpl_decoder.h"

#include <cstring>
#include <limits>

namespace wotbmod {
namespace v3 {
namespace {

constexpr size_t kFooterSize = 20u;
constexpr uint32_t kDvplMarker = 0x4C505644u;
constexpr uint32_t kCompressionNone = 0u;
constexpr uint32_t kCompressionLz4 = 1u;
constexpr uint32_t kCompressionLz4Hc = 2u;
constexpr uint32_t kCompressionLz4Stream = 4u;

uint32_t ReadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8u) |
           (static_cast<uint32_t>(bytes[2]) << 16u) |
           (static_cast<uint32_t>(bytes[3]) << 24u);
}

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

bool AddExtendedLength(
    const uint8_t* input,
    size_t input_size,
    size_t* input_position,
    size_t* length) {
    if (!input_position || !length) return false;
    for (;;) {
        if (*input_position >= input_size) return false;
        const uint8_t value = input[(*input_position)++];
        if (*length >
            std::numeric_limits<size_t>::max() -
                static_cast<size_t>(value)) {
            return false;
        }
        *length += value;
        if (value != 255u) return true;
    }
}

bool DecodeLz4Block(
    const uint8_t* input,
    size_t input_size,
    uint8_t* output,
    size_t output_size) {
    if ((!input && input_size != 0u) ||
        (!output && output_size != 0u)) {
        return false;
    }
    size_t input_position = 0u;
    size_t output_position = 0u;
    while (input_position < input_size) {
        const uint8_t token = input[input_position++];
        size_t literal_length =
            static_cast<size_t>(token >> 4u);
        if (literal_length == 15u &&
            !AddExtendedLength(
                input,
                input_size,
                &input_position,
                &literal_length)) {
            return false;
        }
        if (literal_length > input_size - input_position ||
            literal_length > output_size - output_position) {
            return false;
        }
        if (literal_length != 0u) {
            std::memcpy(
                output + output_position,
                input + input_position,
                literal_length);
            input_position += literal_length;
            output_position += literal_length;
        }
        if (input_position == input_size) {
            return output_position == output_size;
        }
        if (input_size - input_position < 2u) return false;
        const size_t offset =
            static_cast<size_t>(input[input_position]) |
            (static_cast<size_t>(input[input_position + 1u])
             << 8u);
        input_position += 2u;
        if (offset == 0u || offset > output_position) {
            return false;
        }
        size_t match_length =
            static_cast<size_t>(token & 0x0Fu);
        if (match_length == 15u &&
            !AddExtendedLength(
                input,
                input_size,
                &input_position,
                &match_length)) {
            return false;
        }
        if (match_length >
            std::numeric_limits<size_t>::max() - 4u) {
            return false;
        }
        match_length += 4u;
        if (match_length > output_size - output_position) {
            return false;
        }
        const size_t match_position = output_position - offset;
        for (size_t index = 0u; index < match_length; ++index) {
            output[output_position + index] =
                output[match_position + index];
        }
        output_position += match_length;
    }
    return output_position == output_size;
}

WotbModV3Result Fail(
    WotbModV3Result result,
    const char* message,
    std::vector<uint8_t>* out_data,
    std::string* out_error) {
    if (out_data) out_data->clear();
    if (out_error) *out_error = message ? message : "";
    return result;
}

}  // namespace

WotbModV3Result DecodeDvpl(
    const void* file_data,
    size_t file_size,
    uint64_t max_uncompressed_bytes,
    std::vector<uint8_t>* out_data,
    DvplDecodeInfo* out_info,
    std::string* out_error) {
    if (!out_data || !file_data || file_size < kFooterSize ||
        max_uncompressed_bytes == 0u) {
        return Fail(
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "invalid DVPL decode request",
            out_data,
            out_error);
    }
    out_data->clear();
    if (out_error) out_error->clear();
    if (out_info) {
        if (out_info->struct_size < sizeof(DvplDecodeInfo) ||
            out_info->api_version !=
                WOTBMOD_V3_DVPL_DECODER_VERSION) {
            return Fail(
                WOTBMOD_V3_E_INVALID_ARGUMENT,
                "invalid DVPL info structure header",
                out_data,
                out_error);
        }
        const uint32_t struct_size = out_info->struct_size;
        std::memset(out_info, 0, sizeof(*out_info));
        out_info->struct_size = struct_size;
        out_info->api_version =
            WOTBMOD_V3_DVPL_DECODER_VERSION;
    }

    const uint8_t* bytes =
        static_cast<const uint8_t*>(file_data);
    const uint8_t* footer = bytes + file_size - kFooterSize;
    const uint32_t uncompressed_size = ReadLe32(footer);
    const uint32_t compressed_size = ReadLe32(footer + 4u);
    const uint32_t expected_crc = ReadLe32(footer + 8u);
    const uint32_t compression_type = ReadLe32(footer + 12u);
    const uint32_t marker = ReadLe32(footer + 16u);

    if (marker != kDvplMarker) {
        return Fail(
            WOTBMOD_V3_E_PARSE,
            "DVPL footer marker is invalid",
            out_data,
            out_error);
    }
    if (compressed_size != file_size - kFooterSize) {
        return Fail(
            WOTBMOD_V3_E_PARSE,
            "DVPL compressed size does not match the file",
            out_data,
            out_error);
    }
    if (uncompressed_size > max_uncompressed_bytes ||
        uncompressed_size >
            static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
        return Fail(
            WOTBMOD_V3_E_LIMIT_REACHED,
            "DVPL output exceeds the configured limit",
            out_data,
            out_error);
    }
    if (Crc32(bytes, compressed_size) != expected_crc) {
        return Fail(
            WOTBMOD_V3_E_HASH_MISMATCH,
            "DVPL compressed payload CRC32 mismatch",
            out_data,
            out_error);
    }
    if (out_info) {
        out_info->compression_type = compression_type;
        out_info->crc32_compressed = expected_crc;
        out_info->compressed_size = compressed_size;
        out_info->uncompressed_size = uncompressed_size;
    }

    try {
        out_data->resize(static_cast<size_t>(uncompressed_size));
    } catch (...) {
        return Fail(
            WOTBMOD_V3_E_LIMIT_REACHED,
            "DVPL output allocation failed",
            out_data,
            out_error);
    }
    if (compression_type == kCompressionNone) {
        if (compressed_size != uncompressed_size) {
            return Fail(
                WOTBMOD_V3_E_PARSE,
                "uncompressed DVPL sizes do not match",
                out_data,
                out_error);
        }
        if (uncompressed_size != 0u) {
            std::memcpy(
                out_data->data(),
                bytes,
                static_cast<size_t>(uncompressed_size));
        }
        return WOTBMOD_V3_OK;
    }
    if (compression_type == kCompressionLz4 ||
        compression_type == kCompressionLz4Hc) {
        if (!DecodeLz4Block(
                bytes,
                compressed_size,
                out_data->data(),
                out_data->size())) {
            return Fail(
                WOTBMOD_V3_E_PARSE,
                "DVPL LZ4 block is malformed or has a size mismatch",
                out_data,
                out_error);
        }
        return WOTBMOD_V3_OK;
    }
    if (compression_type == kCompressionLz4Stream) {
        return Fail(
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "streaming LZ4 DVPL requires the guarded native stream backend",
            out_data,
            out_error);
    }
    return Fail(
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "DVPL compression type is not supported",
        out_data,
        out_error);
}

}  // namespace v3
}  // namespace wotbmod
