#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "../../include/wotbmod/base.h"

namespace wotbmod {
namespace v3 {

#define WOTBMOD_V3_DVPL_DECODER_VERSION 1u

struct DvplDecodeInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t compression_type;
    uint32_t crc32_compressed;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
};

WotbModV3Result DecodeDvpl(
    const void* file_data,
    size_t file_size,
    uint64_t max_uncompressed_bytes,
    std::vector<uint8_t>* out_data,
    DvplDecodeInfo* out_info,
    std::string* out_error);

}  // namespace v3
}  // namespace wotbmod
