#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DEVICE_VERSION 1u

typedef enum WotbModV3ProcessorArchitecture {
    WOTBMOD_V3_ARCH_UNKNOWN = 0,
    WOTBMOD_V3_ARCH_X86 = 1,
    WOTBMOD_V3_ARCH_X64 = 2,
    WOTBMOD_V3_ARCH_ARM32 = 3,
    WOTBMOD_V3_ARCH_ARM64 = 4
} WotbModV3ProcessorArchitecture;

typedef struct WotbModV3DeviceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t process_architecture;
    uint32_t operating_system_architecture;
    uint32_t logical_processor_count;
    uint32_t graphics_adapter_count;
    uint64_t physical_memory_bytes;
    uint64_t available_memory_bytes;
    char operating_system[WOTBMOD_V3_MAX_NAME];
    char processor[WOTBMOD_V3_MAX_NAME];
    char primary_graphics_adapter[WOTBMOD_V3_MAX_NAME];
} WotbModV3DeviceInfo;

typedef struct WotbModV3GraphicsAdapterInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t index;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsystem_id;
    uint32_t revision;
    uint32_t software_adapter;
    uint64_t dedicated_video_memory_bytes;
    uint64_t dedicated_system_memory_bytes;
    uint64_t shared_system_memory_bytes;
    char name[WOTBMOD_V3_MAX_NAME];
} WotbModV3GraphicsAdapterInfo;

typedef struct WotbModV3DeviceApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3DeviceInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_graphics_adapter_count)(
        WotbModV3Handle mod,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_graphics_adapter_at)(
        WotbModV3Handle mod,
        uint32_t index,
        WotbModV3GraphicsAdapterInfo* out_info);
} WotbModV3DeviceApiV1;

#ifdef __cplusplus
}
#endif
