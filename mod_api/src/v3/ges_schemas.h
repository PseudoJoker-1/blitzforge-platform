#pragma once
#include <cstddef>
#include <cstdint>

#include "../../include/wotbmod/ges_v1.h"

namespace wotbmod {
namespace v3 {

struct GesSchemaField {
    const char* name;
    uint32_t offset;
    uint32_t kind;   /* WotbModV3GesFieldKind */
    uint32_t size;
};

struct GesSchema {
    const char* type_name;   /* "Owner::Name" */
    uint32_t payload_size;
    const GesSchemaField* fields;
    uint32_t field_count;
};

/* ".?AUName@Owner@GES@@" -> "Owner::Name". False for anything that is not a
 * GES struct descriptor or does not fit. */
bool GesTypeNameFromMangled(const char* mangled, char* out, size_t capacity);
/* "Owner::Name" -> "wotbmod.ges.Owner.Name". */
bool GesTopicFromTypeName(const char* type_name, char* out, size_t capacity);
/* "wotbmod.ges.Owner.Name" -> "Owner::Name"; false without the prefix. */
bool GesTypeNameFromTopic(const char* topic, char* out, size_t capacity);

/* Schema ids are 1-based indices into the static table; 0 = none. */
const GesSchema* GesFindSchema(const char* type_name, uint32_t* out_schema_id);
const GesSchema* GesSchemaById(uint32_t schema_id);

}  // namespace v3
}  // namespace wotbmod
