#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_YAML_VERSION 1u
typedef uint32_t WotbModV3YamlNodeId;
#define WOTBMOD_V3_YAML_INVALID_NODE 0xFFFFFFFFu

typedef enum WotbModV3YamlNodeType {
    WOTBMOD_V3_YAML_NULL = 0,
    WOTBMOD_V3_YAML_MAP = 1,
    WOTBMOD_V3_YAML_SEQUENCE = 2,
    WOTBMOD_V3_YAML_STRING = 3,
    WOTBMOD_V3_YAML_BOOL = 4,
    WOTBMOD_V3_YAML_INT = 5,
    WOTBMOD_V3_YAML_FLOAT = 6
} WotbModV3YamlNodeType;

typedef struct WotbModV3YamlLimits {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_depth;
    uint32_t max_nodes;
    uint64_t max_bytes;
} WotbModV3YamlLimits;

typedef struct WotbModV3YamlApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* parse)(
        WotbModV3Handle mod,
        const WotbModV3ConstBuffer* yaml_utf8,
        const WotbModV3YamlLimits* limits,
        WotbModV3Handle* out_document);
    WotbModV3Result(WOTBMOD_V3_CALL* parse_uri)(
        WotbModV3Handle mod,
        const char* uri,
        const WotbModV3YamlLimits* limits,
        WotbModV3Handle* out_document);
    WotbModV3Result(WOTBMOD_V3_CALL* get_root)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId* out_node);
    WotbModV3Result(WOTBMOD_V3_CALL* get_type)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        uint32_t* out_type);
    WotbModV3Result(WOTBMOD_V3_CALL* get_size)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        uint32_t* out_size);
    WotbModV3Result(WOTBMOD_V3_CALL* map_get)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        const char* key,
        WotbModV3YamlNodeId* out_child);
    WotbModV3Result(WOTBMOD_V3_CALL* sequence_get)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        uint32_t index,
        WotbModV3YamlNodeId* out_child);
    WotbModV3Result(WOTBMOD_V3_CALL* get_string)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_bool)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        uint32_t* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* get_int)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        int64_t* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* get_float)(
        WotbModV3Handle mod,
        WotbModV3Handle document,
        WotbModV3YamlNodeId node,
        double* out_value);
} WotbModV3YamlApiV1;

#ifdef __cplusplus
}
#endif
