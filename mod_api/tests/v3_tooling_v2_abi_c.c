#include "../include/wotb_mod_api_v3.h"

_Static_assert(
    offsetof(WotbModV3DiagnosticsApiV2, crash_add_context) ==
        sizeof(WotbModV3DiagnosticsApiV1),
    "diagnostics V1 ABI prefix changed");
_Static_assert(
    offsetof(WotbModV3DevtoolsApiV2, inspect_ui) ==
        sizeof(WotbModV3DevtoolsApiV1),
    "devtools V1 ABI prefix changed");
_Static_assert(
    offsetof(WotbModV3DevtoolsApiV3, get_callback_profile) ==
        sizeof(WotbModV3DevtoolsApiV2),
    "devtools V2 ABI prefix changed");

uint32_t WotbModV3ToolingV2AbiSizes(void) {
    return (uint32_t)(
        sizeof(WotbModV3DiagnosticsApiV2) +
        sizeof(WotbModV3DevtoolsApiV2) +
        sizeof(WotbModV3DevtoolsApiV3) +
        sizeof(WotbModV3CallbackProfile) +
        sizeof(WotbModV3ModHealth) +
        sizeof(WotbModV3ProfilerAggregate) +
        sizeof(WotbModV3ProfilerMemory));
}
