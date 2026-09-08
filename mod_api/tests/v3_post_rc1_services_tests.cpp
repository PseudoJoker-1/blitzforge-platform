/*
 * Focused regression for the PORTABLE half of the five interfaces declared
 * 2026-08-16: wotbmod.ui.read, wotbmod.camera.state, wotbmod.audio.intercept,
 * wotbmod.scene.enumerate and wotbmod.tracer.
 *
 * What this suite is about
 * -----------------------
 * A portable build -- and therefore every CI run -- has NO native backend for
 * these five. `ClientHostDeclaredBackend` is filled by the loader from the
 * exact-fingerprint binding pack, and nothing in this test tree can produce
 * one. So the state under test is exactly the state the shipped portable
 * runtime is in, and the first guarantee is the honest one: every slot refuses
 * with WOTBMOD_V3_E_NOT_SUPPORTED and a reason that says WHY, and
 * query_interface refuses before a mod ever holds the table.
 *
 * Some of the contract cannot be observed from a build with no backend at all
 * -- "not set is distinguishable from set to zero" and "a backend that knows
 * one camera half cannot make the other look known" are statements ABOUT a
 * backend's answers. Those are exercised against a PORTABLE TEST DOUBLE: an
 * ordinary C function table that dereferences nothing, resolves no RVA and
 * knows no struct offset. A test double is not a native backend, and every
 * section below says which of the two it is running under.
 *
 * The three phases, in order:
 *   1. NO BACKEND AT ALL -- registration truth, the frozen UNAVAILABLE
 *      reasons, and the fingerprint / all-or-nothing-group gates.
 *   2. TEST DOUBLE INSTALLED -- the per-interface semantics that only exist
 *      once something answers.
 *   3. DOUBLE REMOVED -- the frozen reasons come back byte-identical and every
 *      slot refuses again, with the double's counters proving that no slot
 *      reached a backend that was not there.
 */

#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/audio_v3.h"
#include "../include/wotbmod/camera_v2.h"
#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/interface_ids.h"
#include "../include/wotbmod/scene_v2.h"
#include "../include/wotbmod/tracer_v1.h"
#include "../include/wotbmod/ui_v2.h"
#include "../include/wotbmod/ui_v4.h"
#include "../src/v3/client_services_backend.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

namespace {

using wotbmod::v3::ClientHostBackend;
using wotbmod::v3::ClientHostDeclaredBackend;
using wotbmod::v3::ClientHostObjectResponse;
using wotbmod::v3::ClientHostUiReadRequest;
using wotbmod::v3::ClientHostUiReadString;

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL: %s\n", label ? label : "(null)");
}

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

/* ==================================================================
 * The portable test double. Nine C functions over plain memory: no
 * engine pointer, no RVA, no struct offset, nothing that could be
 * mistaken for a native backend.
 * ================================================================== */

uint32_t g_uiStringCalls = 0u;
uint32_t g_uiStyleCalls = 0u;
uint32_t g_cameraCalls = 0u;
uint32_t g_publishCalls = 0u;
uint32_t g_disarmCalls = 0u;
uint32_t g_sceneLimitCalls = 0u;
uint32_t g_sceneWalkCalls = 0u;
uint32_t g_tracerCountCalls = 0u;
uint32_t g_tracerStyleCalls = 0u;

/* ui.read double state */
WotbModV3Result g_uiStringResult = WOTBMOD_V3_OK;
uint32_t g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
uint32_t g_uiStringLength = 0u;
char g_uiStringValue[WOTBMOD_V3_MAX_PATH] = {};
bool g_uiStringUnterminated = false;
uint32_t g_uiLastField = 0u;
uint32_t g_uiLastGameOwned = 0xFFFFFFFFu;
uint32_t g_uiStyleFlags = 0u;
float g_uiStyleOpacity = 0.0f;
float g_uiStyleFontSize = 0.0f;

/* camera.state double state */
WotbModV3Result g_cameraResult = WOTBMOD_V3_OK;
uint32_t g_cameraAnimation = 0u;
uint32_t g_cameraAnimationValid = 0u;
uint32_t g_cameraView = 0u;
uint32_t g_cameraViewValid = 0u;

/* audio.intercept double state */
WotbModV3Result g_publishResult = WOTBMOD_V3_OK;
char g_publishedName[WOTBMOD_V3_MAX_NAME] = {};

/* scene.enumerate double state */
WotbModV3Result g_sceneLimitsResult = WOTBMOD_V3_OK;
WotbModV3Result g_sceneWalkResult = WOTBMOD_V3_OK;
uint32_t g_sceneProviderDepth = 0u;
uint32_t g_sceneProviderChildren = 0u;
uint32_t g_sceneProviderNodes = 0u;
uint32_t g_sceneProviderRecordSize = 0u;
uint32_t g_sceneTreeSize = 5u;
uint32_t g_sceneObservedDepth = 0u;
uint32_t g_sceneObservedChildren = 0u;
uint32_t g_sceneObservedNodes = 0u;
uint32_t g_sceneObservedCapacity = 0xFFFFFFFFu;
bool g_sceneOverReport = false;
bool g_sceneOverDepth = false;
bool g_sceneOverChildren = false;
bool g_sceneOverChildrenFlagged = false;
bool g_sceneBadParent = false;
bool g_sceneUnterminatedName = false;
bool g_sceneNonFiniteMatrix = false;
bool g_sceneUnknownFlag = false;

/* tracer double state */
WotbModV3Result g_tracerCountResult = WOTBMOD_V3_OK;
WotbModV3Result g_tracerStyleResult = WOTBMOD_V3_OK;
uint32_t g_tracerCount = WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT;
uint32_t g_tracerLastShellType = 0xFFFFFFFFu;
uint32_t g_tracerFlags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID;
uint32_t g_tracerStyleId = WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING;
WotbModV3Color g_tracerColor = {0.0f, 0.0f, 0.0f, 0.0f};
bool g_tracerEmptyName = false;

WotbModV3Result DoubleUiReadString(
    void*,
    const ClientHostUiReadRequest* request,
    ClientHostUiReadString* value) {
    ++g_uiStringCalls;
    if (!request || !value) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    g_uiLastField = request->field;
    g_uiLastGameOwned = request->game_owned;
    if (g_uiStringResult != WOTBMOD_V3_OK) {
        return g_uiStringResult;
    }
    value->flags = g_uiStringFlags;
    value->length = g_uiStringLength;
    std::memcpy(value->value, g_uiStringValue, sizeof(value->value));
    if (g_uiStringUnterminated) {
        std::memset(value->value, 'x', sizeof(value->value));
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleUiReadStyle(
    void*,
    const ClientHostUiReadRequest* request,
    WotbModV3UiStyleSnapshot* style) {
    ++g_uiStyleCalls;
    if (!request || !style) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    g_uiLastGameOwned = request->game_owned;
    style->flags = g_uiStyleFlags;
    style->opacity = g_uiStyleOpacity;
    style->font_size = g_uiStyleFontSize;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleCameraState(
    void*,
    WotbModV3CameraObservedState* state) {
    ++g_cameraCalls;
    if (!state) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_cameraResult != WOTBMOD_V3_OK) {
        return g_cameraResult;
    }
    state->animation_state = g_cameraAnimation;
    state->animation_state_valid = g_cameraAnimationValid;
    state->view_mode = g_cameraView;
    state->view_mode_valid = g_cameraViewValid;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleInterceptPublish(void*, const char* eventName) {
    ++g_publishCalls;
    if (!eventName) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    Copy(g_publishedName, sizeof(g_publishedName), eventName);
    return g_publishResult;
}

WotbModV3Result DoubleInterceptDisarm(void*) {
    ++g_disarmCalls;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleSceneLimits(
    void*,
    WotbModV3SceneWalkLimits* limits) {
    ++g_sceneLimitCalls;
    if (!limits) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_sceneLimitsResult != WOTBMOD_V3_OK) {
        return g_sceneLimitsResult;
    }
    limits->max_depth = g_sceneProviderDepth;
    limits->max_children_per_node = g_sceneProviderChildren;
    limits->max_nodes = g_sceneProviderNodes;
    limits->node_record_size = g_sceneProviderRecordSize;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleSceneWalk(
    void*,
    const WotbModV3SceneWalkRequest* request,
    WotbModV3SceneNodeRecord* nodes,
    uint32_t capacity,
    uint32_t* reached) {
    ++g_sceneWalkCalls;
    if (!request || !reached) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    g_sceneObservedDepth = request->max_depth;
    g_sceneObservedChildren = request->max_children_per_node;
    g_sceneObservedNodes = request->max_nodes;
    g_sceneObservedCapacity = capacity;
    if (g_sceneWalkResult != WOTBMOD_V3_OK) {
        return g_sceneWalkResult;
    }
    /*
     * A deterministic binary tree: node i has parent (i-1)/2, so depths run
     * 0,1,1,2,2,... A node whose depth would exceed the effective cap is not
     * emitted, which is what an honest provider does.
     */
    uint32_t depths[16] = {};
    uint32_t emitted = 0u;
    const uint32_t tree = g_sceneTreeSize < 16u ? g_sceneTreeSize : 16u;
    for (uint32_t index = 0u; index < tree; ++index) {
        const uint32_t parent = (index == 0u) ? 0u : (index - 1u) / 2u;
        const uint32_t depth = (index == 0u) ? 0u : depths[parent] + 1u;
        if (depth > request->max_depth) {
            break;
        }
        depths[index] = depth;
        ++emitted;
    }
    if (emitted > request->max_nodes) {
        emitted = request->max_nodes;
    }
    *reached = g_sceneOverReport ? request->max_nodes + 1u : emitted;
    const uint32_t written = emitted < capacity ? emitted : capacity;
    for (uint32_t index = 0u; index < written; ++index) {
        WotbModV3SceneNodeRecord& record = nodes[index];
        record = {};
        WOTBMOD_V3_INIT_STRUCT(record, WOTBMOD_V3_SCENE_VERSION_2);
        record.index = index;
        record.parent_index = (index == 0u)
            ? WOTBMOD_V3_SCENE_NODE_NO_PARENT
            : (index - 1u) / 2u;
        record.depth = depths[index];
        record.child_count = 0u;
        for (uint32_t child = index + 1u; child < emitted; ++child) {
            if ((child - 1u) / 2u == index) {
                ++record.child_count;
            }
        }
        record.flags = (index == 0u)
            ? WOTBMOD_V3_SCENE_NODE_IS_SCENE
            : 0u;
        for (uint32_t component = 0u; component < 16u; ++component) {
            record.world_matrix.values[component] =
                (component % 5u == 0u) ? 1.0f : 0.0f;
        }
        Copy(record.name, sizeof(record.name), "node");
        if (index + 1u == written) {
            if (g_sceneOverDepth) {
                record.depth = request->max_depth + 1u;
            }
            if (g_sceneOverChildren) {
                record.child_count = request->max_children_per_node + 1u;
                if (g_sceneOverChildrenFlagged) {
                    record.flags |=
                        WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED;
                }
            }
            if (g_sceneBadParent && index != 0u) {
                record.parent_index = index;
            }
            if (g_sceneUnterminatedName) {
                std::memset(record.name, 'n', sizeof(record.name));
            }
            if (g_sceneNonFiniteMatrix) {
                record.world_matrix.values[3] =
                    std::numeric_limits<float>::quiet_NaN();
            }
            if (g_sceneUnknownFlag) {
                record.flags |= 1u << 20u;
            }
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleTracerCount(void*, uint32_t* count) {
    ++g_tracerCountCalls;
    if (!count) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_tracerCountResult != WOTBMOD_V3_OK) {
        return g_tracerCountResult;
    }
    *count = g_tracerCount;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DoubleTracerStyle(
    void*,
    uint32_t shellType,
    WotbModV3TracerStyle* style) {
    ++g_tracerStyleCalls;
    g_tracerLastShellType = shellType;
    if (!style) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_tracerStyleResult != WOTBMOD_V3_OK) {
        return g_tracerStyleResult;
    }
    style->shell_type = shellType;
    style->style_id = g_tracerStyleId;
    style->flags = g_tracerFlags;
    style->color = g_tracerColor;
    Copy(
        style->style_name,
        sizeof(style->style_name),
        g_tracerEmptyName ? "" : "ARMOR_PIERCING");
    return WOTBMOD_V3_OK;
}

ClientHostDeclaredBackend MakeDeclaredBackend() {
    ClientHostDeclaredBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    backend.binding_pack_version = 111900834u;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.ui_read_string = &DoubleUiReadString;
    backend.ui_read_style = &DoubleUiReadStyle;
    backend.camera_read_observed_state = &DoubleCameraState;
    backend.audio_intercept_publish_name = &DoubleInterceptPublish;
    backend.audio_intercept_disarm = &DoubleInterceptDisarm;
    backend.scene_get_walk_limits = &DoubleSceneLimits;
    backend.scene_walk_active = &DoubleSceneWalk;
    backend.tracer_get_shell_type_count = &DoubleTracerCount;
    backend.tracer_get_style = &DoubleTracerStyle;
    return backend;
}

void ResetDoubleState() {
    g_uiStringResult = WOTBMOD_V3_OK;
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    g_uiStringLength = 0u;
    std::memset(g_uiStringValue, 0, sizeof(g_uiStringValue));
    g_uiStringUnterminated = false;
    g_uiStyleFlags = 0u;
    g_uiStyleOpacity = 0.0f;
    g_uiStyleFontSize = 0.0f;
    g_cameraResult = WOTBMOD_V3_OK;
    g_cameraAnimation = 0u;
    g_cameraAnimationValid = 0u;
    g_cameraView = 0u;
    g_cameraViewValid = 0u;
    g_publishResult = WOTBMOD_V3_OK;
    g_sceneLimitsResult = WOTBMOD_V3_OK;
    g_sceneWalkResult = WOTBMOD_V3_OK;
    g_sceneProviderDepth = 0u;
    g_sceneProviderChildren = 0u;
    g_sceneProviderNodes = 0u;
    g_sceneProviderRecordSize =
        static_cast<uint32_t>(sizeof(WotbModV3SceneNodeRecord));
    g_sceneTreeSize = 5u;
    g_sceneOverReport = false;
    g_sceneOverDepth = false;
    g_sceneOverChildren = false;
    g_sceneOverChildrenFlagged = false;
    g_sceneBadParent = false;
    g_sceneUnterminatedName = false;
    g_sceneNonFiniteMatrix = false;
    g_sceneUnknownFlag = false;
    g_tracerCountResult = WOTBMOD_V3_OK;
    g_tracerStyleResult = WOTBMOD_V3_OK;
    g_tracerCount = WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT;
    g_tracerFlags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID;
    g_tracerStyleId = WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING;
    g_tracerColor = {0.0f, 0.0f, 0.0f, 0.0f};
    g_tracerEmptyName = false;
}

/* ==================================================================
 * The generic ClientHostBackend. It exists only so a UI control can be
 * created: ui.read reads back a control this API owns, and without a
 * control there is nothing to ask about.
 * ================================================================== */

uint64_t g_nextUiObject = UINT64_C(0x5100);

WotbModV3Result WOTBMOD_V3_CALL InvokeHost(
    void*,
    WotbModV3Handle,
    const char* operation,
    const void*,
    uint32_t,
    void* response,
    uint32_t responseSize) {
    if (!operation) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "ui_control_create") == 0) {
        if (!response ||
            responseSize < sizeof(ClientHostObjectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ClientHostObjectResponse* typed =
            static_cast<ClientHostObjectResponse*>(response);
        typed->object = g_nextUiObject++;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_OK;
}

/* ================================================================== */

const char* g_entryId = "post-rc1-services";

WotbModV3Result WOTBMOD_V3_CALL Entry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *info = {};
    WOTBMOD_V3_INIT_STRUCT(*info, WOTBMOD_V3_ABI_VERSION);
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), g_entryId);
    Copy(info->name, sizeof(info->name), g_entryId);
    Copy(info->version, sizeof(info->version), "1.0.0");
    return WOTBMOD_V3_OK;
}

WotbModV3Handle CreateEnabledMod(
    const char* id,
    uint32_t tier,
    const char* const* grants,
    uint32_t grantCount) {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        WotbModV3Runtime_CreateMod(id, tier, &mod) == WOTBMOD_V3_OK,
        "the test mod is created");
    Check(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            grants,
            grantCount,
            1u) == WOTBMOD_V3_OK,
        "named permission grants are installed on the test mod");
    g_entryId = id;
    WotbModV3RuntimeModuleInfo module = {};
    WOTBMOD_V3_INIT_STRUCT(module, WOTBMOD_V3_ABI_VERSION);
    Check(
        WotbModV3Runtime_InvokeEntry(mod, &Entry, &module) ==
            WOTBMOD_V3_OK,
        "the test mod entry point is invoked");
    Check(
        WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK,
        "the test mod is enabled");
    return mod;
}

WotbModV3InterfaceInfo Info(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name) {
    WotbModV3InterfaceInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    if (bootstrap->get_interface_info(mod, name, &info) !=
        WOTBMOD_V3_OK) {
        info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    }
    return info;
}

std::string LastError(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    WotbModV3ErrorInfo error = {};
    WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
    if (bootstrap->get_last_error(mod, &error) != WOTBMOD_V3_OK) {
        return std::string();
    }
    return std::string(error.message);
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

struct DeclaredInterface {
    const char* name;
    uint32_t version;
};

const DeclaredInterface kDeclared[] = {
    {WOTBMOD_V3_IFACE_UI_READ, WOTBMOD_V3_UI_VERSION_4},
    {WOTBMOD_V3_IFACE_CAMERA_STATE, WOTBMOD_V3_CAMERA_VERSION_2},
    {WOTBMOD_V3_IFACE_AUDIO_INTERCEPT, WOTBMOD_V3_AUDIO_VERSION_3},
    {WOTBMOD_V3_IFACE_SCENE_ENUMERATE, WOTBMOD_V3_SCENE_VERSION_2},
    {WOTBMOD_V3_IFACE_TRACER, WOTBMOD_V3_TRACER_VERSION}};

constexpr uint32_t kDeclaredCount =
    sizeof(kDeclared) / sizeof(kDeclared[0]);

std::string g_frozenReason[kDeclaredCount];

/* Tables captured while the double is installed; see the file header. */
const WotbModV3UiApiV4* g_ui = nullptr;
const WotbModV3CameraApiV2* g_camera = nullptr;
const WotbModV3AudioApiV3* g_audio = nullptr;
const WotbModV3SceneApiV2* g_scene = nullptr;
const WotbModV3TracerApiV1* g_tracer = nullptr;

/* ==================================================================
 * PHASE 1 -- no backend at all.
 * ================================================================== */

void TestFrozenRegistration(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    for (uint32_t index = 0u; index < kDeclaredCount; ++index) {
        const WotbModV3InterfaceInfo info =
            Info(bootstrap, mod, kDeclared[index].name);
        Check(
            info.status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
            "a declared interface with no backend is registered UNAVAILABLE");
        Check(
            info.unavailable_reason[0] != '\0',
            "an UNAVAILABLE declared interface carries a reason string");
        g_frozenReason[index] = info.unavailable_reason;
        const void* table = reinterpret_cast<const void*>(1u);
        Check(
            bootstrap->query_interface(
                mod,
                kDeclared[index].name,
                kDeclared[index].version,
                &table) == WOTBMOD_V3_E_NOT_SUPPORTED,
            "query_interface refuses a declared interface that has no backend");
        Check(
            table == nullptr,
            "a refused query_interface clears the caller's table pointer");
        Check(
            LastError(bootstrap, mod) == g_frozenReason[index],
            "the refusal reports the interface's own frozen reason, not a generic message");
    }

    Check(
        Contains(g_frozenReason[0], "readback") &&
            Contains(g_frozenReason[0], "NOT_SUPPORTED"),
        "the ui.read reason names UI readback specifically");
    Check(
        Contains(g_frozenReason[1], "animation-state") &&
            Contains(g_frozenReason[1], "arcade/sniper"),
        "the camera.state reason names both halves it cannot read");
    Check(
        Contains(g_frozenReason[2], "sound-event interception"),
        "the audio.intercept reason names sound-event interception");
    Check(
        Contains(g_frozenReason[3], "scene enumeration"),
        "the scene.enumerate reason names scene enumeration");
    Check(
        Contains(g_frozenReason[4], "tracer") &&
            Contains(g_frozenReason[4], "shell-type"),
        "the tracer reason names the shell-type style lookup");

    bool distinct = true;
    for (uint32_t left = 0u; left < kDeclaredCount; ++left) {
        for (uint32_t right = left + 1u; right < kDeclaredCount;
             ++right) {
            if (g_frozenReason[left] == g_frozenReason[right]) {
                distinct = false;
            }
        }
    }
    Check(
        distinct,
        "each of the five frozen reasons is different from the other four");
}

void TestFingerprintGate(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    const uint32_t refused[] = {
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN,
        WOTBMOD_V3_CLIENT_COMPATIBILITY_BINDINGS_MISSING,
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH};
    for (const uint32_t state : refused) {
        ClientHostDeclaredBackend backend = MakeDeclaredBackend();
        backend.compatibility_state = state;
        wotbmod::v3::SetClientHostDeclaredBackend(&backend);
        bool allUnavailable = true;
        for (const DeclaredInterface& declared : kDeclared) {
            if (Info(bootstrap, mod, declared.name).status !=
                WOTBMOD_V3_CAPABILITY_UNAVAILABLE) {
                allUnavailable = false;
            }
        }
        Check(
            allUnavailable,
            "a backend that cannot prove the client fingerprint is refused whole, fail-closed");
    }

    ClientHostDeclaredBackend futureVersion = MakeDeclaredBackend();
    futureVersion.api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION + 1u;
    wotbmod::v3::SetClientHostDeclaredBackend(&futureVersion);
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_TRACER).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "a backend built against a future declared-backend version is refused");

    ClientHostDeclaredBackend shortTable = MakeDeclaredBackend();
    shortTable.struct_size =
        static_cast<uint32_t>(sizeof(shortTable) - 1u);
    wotbmod::v3::SetClientHostDeclaredBackend(&shortTable);
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_TRACER).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "a backend table shorter than the contract struct is refused");

    wotbmod::v3::SetClientHostDeclaredBackend(nullptr);
}

void TestGroupsAreAllOrNothing(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ClientHostDeclaredBackend backend = MakeDeclaredBackend();
    backend.ui_read_style = nullptr;
    backend.audio_intercept_disarm = nullptr;
    backend.scene_walk_active = nullptr;
    backend.tracer_get_style = nullptr;
    wotbmod::v3::SetClientHostDeclaredBackend(&backend);
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_UI_READ).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "a ui.read group missing one member is dropped whole");
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_AUDIO_INTERCEPT).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "an audio.intercept group with no disarm is dropped whole");
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_SCENE_ENUMERATE).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "a scene.enumerate group with limits but no walk is dropped whole");
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_TRACER).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "a tracer group with a count but no style lookup is dropped whole");
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_CAMERA_STATE).status ==
            WOTBMOD_V3_CAPABILITY_DEGRADED,
        "the single-member camera.state group installs independently of the broken four");
    wotbmod::v3::SetClientHostDeclaredBackend(nullptr);
    Check(
        Info(bootstrap, mod, WOTBMOD_V3_IFACE_CAMERA_STATE).status ==
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "removing the backend takes camera.state back to UNAVAILABLE");
}

/* ==================================================================
 * PHASE 2 -- portable test double installed.
 * ================================================================== */

void CaptureTables(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle allMod,
    WotbModV3Handle uiMod,
    WotbModV3Handle cameraMod,
    WotbModV3Handle audioMod,
    WotbModV3Handle sceneMod,
    WotbModV3Handle tracerMod) {
    /*
     * Availability is asked with allMod, not uiMod: every mod here is
     * named-permission-restricted, and get_interface_info is refused for an
     * interface whose permission family the asking mod does not hold. Each
     * per-interface table below is still queried with the mod that owns that
     * family, because that is what proves the door opens for the right key.
     */
    for (const DeclaredInterface& declared : kDeclared) {
        const WotbModV3InterfaceInfo info =
            Info(bootstrap, allMod, declared.name);
        Check(
            info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
            "a declared interface with a complete backend is DEGRADED, never AVAILABLE");
        Check(
            info.unavailable_reason[0] != '\0',
            "a DEGRADED declared interface still explains what is out of reach");
    }
    Check(
        Contains(
            std::string(
                Info(bootstrap, allMod, WOTBMOD_V3_IFACE_CAMERA_STATE)
                    .unavailable_reason),
            "no setter"),
        "the camera.state degraded reason states there is deliberately no setter");
    Check(
        Contains(
            std::string(
                Info(bootstrap, allMod, WOTBMOD_V3_IFACE_TRACER)
                    .unavailable_reason),
            "creation and mutation are not represented"),
        "the tracer degraded reason states creation and mutation stay unrepresented");

    const void* table = nullptr;
    Check(
        bootstrap->query_interface(
            uiMod,
            WOTBMOD_V3_IFACE_UI_READ,
            WOTBMOD_V3_UI_VERSION_4,
            &table) == WOTBMOD_V3_OK,
        "ui.read becomes queryable once its group is installed");
    g_ui = static_cast<const WotbModV3UiApiV4*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            cameraMod,
            WOTBMOD_V3_IFACE_CAMERA_STATE,
            WOTBMOD_V3_CAMERA_VERSION_2,
            &table) == WOTBMOD_V3_OK,
        "camera.state becomes queryable once its group is installed");
    g_camera = static_cast<const WotbModV3CameraApiV2*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            audioMod,
            WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
            WOTBMOD_V3_AUDIO_VERSION_3,
            &table) == WOTBMOD_V3_OK,
        "audio.intercept becomes queryable once its group is installed");
    g_audio = static_cast<const WotbModV3AudioApiV3*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            sceneMod,
            WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
            WOTBMOD_V3_SCENE_VERSION_2,
            &table) == WOTBMOD_V3_OK,
        "scene.enumerate becomes queryable once its group is installed");
    g_scene = static_cast<const WotbModV3SceneApiV2*>(table);
    table = nullptr;
    Check(
        bootstrap->query_interface(
            tracerMod,
            WOTBMOD_V3_IFACE_TRACER,
            WOTBMOD_V3_TRACER_VERSION,
            &table) == WOTBMOD_V3_OK,
        "tracer becomes queryable once its group is installed");
    g_tracer = static_cast<const WotbModV3TracerApiV1*>(table);
    Check(
        g_ui && g_camera && g_audio && g_scene && g_tracer,
        "all five declared interface tables were obtained");
}

void TestUiReadProvenance(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3UiHandle foreignControl) {
    char buffer[64] = {};
    uint32_t size = sizeof(buffer);

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    Copy(g_uiStringValue, sizeof(g_uiStringValue), "hello");
    g_uiStringLength = 5u;
    size = sizeof(buffer);
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_OK,
        "a field this API committed reads back as OK");
    Check(
        std::strcmp(buffer, "hello") == 0 && size == 6u,
        "the committed value is copied out with its terminator counted");
    Check(
        g_uiLastField ==
            wotbmod::v3::CLIENT_HOST_UI_READ_FIELD_TEXT,
        "control_get_text asks the backend for the TEXT field and no other");

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    g_uiStringLength = 0u;
    g_uiStringValue[0] = '\0';
    size = sizeof(buffer);
    buffer[0] = 'x';
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_OK,
        "a field this API deliberately blanked reads back as OK, not as an error");
    Check(
        buffer[0] == '\0' && size == 1u,
        "a field committed as the empty string yields an empty string");

    ResetDoubleState();
    g_uiStringFlags = 0u;
    g_uiStringLength = 0u;
    size = sizeof(buffer);
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_E_NOT_FOUND,
        "a field this API never wrote answers NOT_FOUND, so 'never set' is not the empty string");

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    Copy(g_uiStringValue, sizeof(g_uiStringValue), "hello");
    g_uiStringLength = 5u;
    size = 2u;
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
        "a short readback buffer is refused rather than truncated");
    Check(
        size == 6u,
        "a refused readback reports the size the caller actually needs");

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    g_uiStringLength =
        static_cast<uint32_t>(sizeof(g_uiStringValue));
    size = sizeof(buffer);
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_E_PLATFORM,
        "a backend string longer than the ABI allows is a host bug, not a mod answer");

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXT_SET;
    g_uiStringLength = 8u;
    g_uiStringUnterminated = true;
    size = sizeof(buffer);
    Check(
        g_ui->control_get_text(mod, control, buffer, &size) ==
            WOTBMOD_V3_E_PLATFORM,
        "an unterminated backend string is refused instead of being copied");

    ResetDoubleState();
    size = sizeof(buffer);
    Check(
        g_ui->control_get_text(mod, foreignControl, buffer, &size) !=
            WOTBMOD_V3_OK,
        "one mod cannot read back another mod's UI control");

    ResetDoubleState();
    const uint32_t stringCallsBefore = g_uiStringCalls;
    Check(
        g_ui->control_get_text(mod, control, buffer, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a null size pointer is an argument error");
    Check(
        g_uiStringCalls == stringCallsBefore,
        "argument validation runs in the runtime and never reaches the backend");

    ResetDoubleState();
    g_uiStringFlags = WOTBMOD_V3_UI_READ_TEXTURE_SET;
    Copy(g_uiStringValue, sizeof(g_uiStringValue), "mod://a.png");
    g_uiStringLength = 11u;
    size = sizeof(buffer);
    Check(
        g_ui->control_get_texture(mod, control, buffer, &size) ==
            WOTBMOD_V3_OK &&
            g_uiLastField ==
                wotbmod::v3::CLIENT_HOST_UI_READ_FIELD_TEXTURE,
        "control_get_texture reads the TEXTURE field with its own provenance bit");
    size = sizeof(buffer);
    Check(
        g_ui->control_get_font(mod, control, buffer, &size) ==
            WOTBMOD_V3_E_NOT_FOUND,
        "a TEXTURE_SET bit does not make the font field look written");

    (void)bootstrap;
}

void TestUiReadStyle(
    WotbModV3Handle mod,
    WotbModV3UiHandle control) {
    WotbModV3UiStyleSnapshot style = {};

    ResetDoubleState();
    g_uiStyleFlags = WOTBMOD_V3_UI_READ_OPACITY_SET;
    g_uiStyleOpacity = 0.0f;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(mod, control, &style) == WOTBMOD_V3_OK,
        "a style snapshot with a committed opacity of zero succeeds");
    Check(
        (style.flags & WOTBMOD_V3_UI_READ_OPACITY_SET) != 0u &&
            style.opacity == 0.0f,
        "opacity committed as zero is reported as zero WITH its _SET flag");

    ResetDoubleState();
    g_uiStyleFlags = 0u;
    g_uiStyleOpacity = 0.0f;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(mod, control, &style) == WOTBMOD_V3_OK,
        "a style snapshot with nothing committed still succeeds");
    Check(
        (style.flags & WOTBMOD_V3_UI_READ_OPACITY_SET) == 0u &&
            style.opacity == 0.0f,
        "an opacity that was never set carries the same zero with the _SET flag CLEAR, so the flag is the only thing that distinguishes them");

    ResetDoubleState();
    g_uiStyleFlags = WOTBMOD_V3_UI_READ_GAME_OWNED;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(mod, control, &style) == WOTBMOD_V3_OK,
        "a style snapshot on a mod-owned control succeeds");
    Check(
        (style.flags & WOTBMOD_V3_UI_READ_GAME_OWNED) == 0u,
        "the runtime owns GAME_OWNED and clears it for a mod-owned control even when the backend set it");
    Check(
        g_uiLastGameOwned == 0u,
        "the runtime tells the backend the control is mod-owned rather than asking it");

    ResetDoubleState();
    g_uiStyleFlags = 1u << 20u;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(mod, control, &style) ==
            WOTBMOD_V3_E_PLATFORM,
        "a style snapshot carrying an unknown flag bit is refused");

    ResetDoubleState();
    g_uiStyleFlags = WOTBMOD_V3_UI_READ_OPACITY_SET;
    g_uiStyleOpacity = std::numeric_limits<float>::quiet_NaN();
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(mod, control, &style) ==
            WOTBMOD_V3_E_PLATFORM,
        "a non-finite style value is refused instead of reaching a mod");
}

void TestCameraIndependentValidity(WotbModV3Handle mod) {
    uint32_t value = 0xFFFFFFFFu;
    WotbModV3CameraObservedState state = {};

    ResetDoubleState();
    g_cameraAnimation = WOTBMOD_V3_CAMERA_ANIMATION_POST_MORTEM;
    g_cameraAnimationValid = 1u;
    g_cameraView = WOTBMOD_V3_CAMERA_VIEW_SNIPER;
    g_cameraViewValid = 1u;
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) == WOTBMOD_V3_OK,
        "both camera halves readable succeeds");
    Check(
        state.animation_state ==
                WOTBMOD_V3_CAMERA_ANIMATION_POST_MORTEM &&
            state.animation_state_valid == 1u,
        "the RTTI-proven POSTMORTEM animation state is reported as state 3");
    Check(
        state.view_mode == WOTBMOD_V3_CAMERA_VIEW_SNIPER &&
            state.view_mode_valid == 1u,
        "raw view-mode 0 is SNIPER, not ARCADE -- the pre-2026-08-15 mapping was inverted");

    ResetDoubleState();
    g_cameraAnimation = WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT;
    g_cameraAnimationValid = 1u;
    g_cameraViewValid = 0u;
    g_cameraView = 0u;
    const uint32_t callsBefore = g_cameraCalls;
    value = 0xFFFFFFFFu;
    Check(
        g_camera->get_animation_state(mod, &value) == WOTBMOD_V3_OK &&
            value == WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT,
        "the readable animation half answers on its own");
    Check(
        g_camera->get_view_mode(mod, &value) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "a backend that knows the animation state cannot make the view mode look known");
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) == WOTBMOD_V3_OK,
        "one readable half is enough for the combined read to succeed");
    Check(
        state.view_mode_valid == 0u && state.view_mode == 0u,
        "the unread view mode is zeroed, so only its validity flag separates it from SNIPER");
    Check(
        g_cameraCalls == callsBefore + 3u,
        "each public camera slot costs exactly one backend read, so the halves cannot disagree");

    ResetDoubleState();
    g_cameraViewValid = 1u;
    g_cameraView = WOTBMOD_V3_CAMERA_VIEW_ARCADE;
    g_cameraAnimationValid = 0u;
    g_cameraAnimation = WOTBMOD_V3_CAMERA_ANIMATION_NONE;
    value = 0xFFFFFFFFu;
    Check(
        g_camera->get_view_mode(mod, &value) == WOTBMOD_V3_OK &&
            value == WOTBMOD_V3_CAMERA_VIEW_ARCADE,
        "the readable view half answers on its own");
    Check(
        g_camera->get_animation_state(mod, &value) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "an unread animation state is never reported as the initial NONE state");
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) == WOTBMOD_V3_OK &&
            state.animation_state_valid == 0u &&
            state.animation_state == 0u,
        "the unread animation half is zeroed with its validity flag clear");

    ResetDoubleState();
    g_cameraAnimationValid = 0u;
    g_cameraViewValid = 0u;
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "neither half readable is NOT_SUPPORTED, exactly as the frozen header mandates");

    ResetDoubleState();
    g_cameraResult = WOTBMOD_V3_E_NOT_FOUND;
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "a backend with no camera controller resolvable maps to NOT_SUPPORTED");

    ResetDoubleState();
    g_cameraAnimation = 7u;
    g_cameraAnimationValid = 1u;
    g_cameraView = 9u;
    g_cameraViewValid = 1u;
    state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(mod, &state) == WOTBMOD_V3_OK,
        "a value the frozen enums do not name is still a successful read");
    Check(
        state.animation_state ==
                WOTBMOD_V3_CAMERA_ANIMATION_UNKNOWN &&
            state.animation_state_valid == 1u,
        "an unnamed animation value degrades to UNKNOWN while staying valid");
    Check(
        state.view_mode == WOTBMOD_V3_CAMERA_VIEW_UNKNOWN &&
            state.view_mode_valid == 1u,
        "an unnamed view value degrades to UNKNOWN while staying valid");
}

struct InterceptContext {
    uint32_t decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    const char* substitute = nullptr;
    uint32_t calls = 0u;
    uint32_t order = 0u;
    bool probeReentry = false;
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result reentrantPriority = WOTBMOD_V3_OK;
    WotbModV3Result reentrantActive = WOTBMOD_V3_OK;
    uint32_t reentrantActiveValue = 0xFFFFFFFFu;
    char observedName[WOTBMOD_V3_MAX_NAME] = {};
    uint32_t observedThreadRole = 0xFFFFFFFFu;
};

uint32_t g_interceptOrder = 0u;

void WOTBMOD_V3_CALL InterceptCallback(
    WotbModV3Handle,
    const WotbModV3SoundInterceptRequest* request,
    WotbModV3SoundInterceptResponse* response,
    void* userData) {
    InterceptContext* context =
        static_cast<InterceptContext*>(userData);
    if (!context || !request || !response) {
        return;
    }
    ++context->calls;
    context->order = ++g_interceptOrder;
    Copy(
        context->observedName,
        sizeof(context->observedName),
        request->event_name);
    context->observedThreadRole = request->thread_role;
    if (context->probeReentry) {
        context->reentrantPriority = g_audio->intercept_set_priority(
            context->mod,
            context->token,
            5);
        context->reentrantActive = g_audio->is_intercept_active(
            context->mod,
            &context->reentrantActiveValue);
    }
    response->decision = context->decision;
    if (context->substitute) {
        Copy(
            response->substitute_event_name,
            sizeof(response->substitute_event_name),
            context->substitute);
    }
}

void TestAudioInterception(WotbModV3Handle mod) {
    ResetDoubleState();
    const char* const kName = "guns/tracers/tracer_hard";
    uint32_t active = 0xFFFFFFFFu;
    Check(
        g_audio->is_intercept_active(mod, &active) == WOTBMOD_V3_OK &&
            active == 0u,
        "outside a callback the thread-local re-entry flag reads as zero");

    WotbModV3Token rejected = UINT64_C(0x1234);
    const uint32_t publishBefore = g_publishCalls;
    Check(
        g_audio->intercept_register(
            mod,
            "",
            0,
            &InterceptCallback,
            nullptr,
            &rejected) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "an empty event name is refused: match-everything filters are not expressible");
    Check(
        g_audio->intercept_register(
            mod,
            nullptr,
            0,
            &InterceptCallback,
            nullptr,
            &rejected) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a null event name is refused");
    Check(
        g_audio->intercept_register(
            mod,
            kName,
            0,
            nullptr,
            nullptr,
            &rejected) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a registration with no callback is refused");
    Check(
        g_publishCalls == publishBefore,
        "a refused registration never publishes a name to the detour");

    InterceptContext low;
    InterceptContext high;
    WotbModV3Token lowToken = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Token highToken = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        g_audio->intercept_register(
            mod,
            kName,
            0,
            &InterceptCallback,
            &low,
            &lowToken) == WOTBMOD_V3_OK,
        "an exact-name interception registers against a real backend");
    Check(
        g_publishCalls == publishBefore + 1u &&
            std::strcmp(g_publishedName, kName) == 0,
        "registration publishes the exact name to the detour before the subscription exists");
    Check(
        g_audio->intercept_register(
            mod,
            kName,
            10,
            &InterceptCallback,
            &high,
            &highToken) == WOTBMOD_V3_OK,
        "a second mod-side interception on the same name registers too");
    Check(
        g_publishCalls == publishBefore + 2u,
        "publishing the same name again is idempotent at the backend and is not skipped by the runtime");

    WotbModV3SoundInterceptResponse response = {};
    g_interceptOrder = 0u;
    low.calls = 0u;
    high.calls = 0u;
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_OK,
        "the detour ingress dispatches a published name");
    Check(
        response.decision ==
            WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH,
        "callbacks that decide nothing leave the game's own behaviour untouched");
    Check(
        low.calls == 1u && high.calls == 1u,
        "every interception on the name is offered the event when none of them decides");
    Check(
        high.order < low.order,
        "higher priority runs first and registration order only breaks ties");
    Check(
        std::strcmp(high.observedName, kName) == 0,
        "the callback receives a bounded copy of the interned event name");

    high.decision = WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS;
    low.calls = 0u;
    high.calls = 0u;
    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_OK &&
            response.decision ==
                WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS,
        "the first non-pass-through decision wins");
    Check(
        low.calls == 0u,
        "a decision ends the chain and the lower-priority interception is not consulted");

    high.decision = WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE;
    high.substitute = "";
    low.decision = WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS;
    low.calls = 0u;
    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_OK &&
            response.decision ==
                WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS,
        "a SUBSTITUTE with an empty name is discarded and the chain continues instead of dropping the sound");
    Check(
        low.calls == 1u,
        "the mod behind an unusable substitution still gets its decision");

    high.decision = WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE;
    high.substitute = "guns/tracers/tracer_soft";
    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_OK &&
            response.decision ==
                WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE &&
            std::strcmp(
                response.substitute_event_name,
                "guns/tracers/tracer_soft") == 0,
        "a SUBSTITUTE with a real name carries that name back to the detour");

    high.decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    high.substitute = nullptr;
    low.decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    high.probeReentry = true;
    high.mod = mod;
    high.token = highToken;
    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_OK,
        "the re-entry probe dispatch completes");
    Check(
        high.reentrantPriority == WOTBMOD_V3_E_BUSY,
        "an ordinary audio operation inside an interception callback answers BUSY instead of recursing");
    Check(
        high.reentrantActive == WOTBMOD_V3_OK &&
            high.reentrantActiveValue == 1u,
        "is_intercept_active answers inside the callback -- it is the one slot the re-entry guard must not silence");
    high.probeReentry = false;

    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            "sounds/nobody/wants/this",
            &response) == WOTBMOD_V3_E_NOT_FOUND,
        "a name nobody subscribed to answers NOT_FOUND, which the detour must treat as pass-through");
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            nullptr,
            &response) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "the detour ingress refuses a null event name");
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(kName, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "the detour ingress refuses a null response buffer");

    const uint32_t disarmBefore = g_disarmCalls;
    Check(
        g_audio->intercept_unregister(mod, lowToken) == WOTBMOD_V3_OK &&
            g_audio->intercept_unregister(mod, highToken) ==
                WOTBMOD_V3_OK,
        "both interceptions unregister");
    Check(
        g_disarmCalls == disarmBefore,
        "unregistering the last interception does NOT disarm the detour: the published match set is append-only and immortal");
    response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            kName,
            &response) == WOTBMOD_V3_E_NOT_FOUND,
        "after the last unregister the published name costs one dispatch that answers NOT_FOUND, so the sound plays exactly as stock");
    Check(
        g_audio->intercept_unregister(mod, highToken) !=
            WOTBMOD_V3_OK,
        "a token cannot be unregistered twice");
}

void TestSceneWalk(WotbModV3Handle mod) {
    WotbModV3SceneWalkLimits limits = {};
    WotbModV3SceneWalkRequest request = {};
    WotbModV3SceneNodeRecord nodes[8] = {};
    uint32_t count = 0u;

    ResetDoubleState();
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    Check(
        g_scene->get_limits(mod, &limits) == WOTBMOD_V3_OK,
        "a provider with no opinion on the caps reports the contract ceilings");
    Check(
        limits.max_depth == WOTBMOD_V3_SCENE_WALK_MAX_DEPTH &&
            limits.max_children_per_node ==
                WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN &&
            limits.max_nodes == WOTBMOD_V3_SCENE_WALK_MAX_NODES,
        "the reported ceilings are exactly the frozen header constants");
    Check(
        limits.node_record_size ==
            static_cast<uint32_t>(sizeof(WotbModV3SceneNodeRecord)),
        "the reported node record size is this runtime's own");

    ResetDoubleState();
    g_sceneProviderDepth = 4u;
    g_sceneProviderChildren = 8u;
    g_sceneProviderNodes = 6u;
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    Check(
        g_scene->get_limits(mod, &limits) == WOTBMOD_V3_OK &&
            limits.max_depth == 4u &&
            limits.max_children_per_node == 8u &&
            limits.max_nodes == 6u,
        "a provider that enforces lower caps than the ceilings is reported as it is");

    ResetDoubleState();
    g_sceneProviderRecordSize =
        static_cast<uint32_t>(sizeof(WotbModV3SceneNodeRecord)) + 4u;
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    Check(
        g_scene->get_limits(mod, &limits) == WOTBMOD_V3_E_PLATFORM,
        "a provider whose node record size disagrees with this runtime is refused");

    /* Cap clamping on the way in: the ceilings. */
    ResetDoubleState();
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_depth = 999u;
    request.max_children_per_node = 999999u;
    request.max_nodes = 999999u;
    count = 0u;
    Check(
        g_scene->walk_active_scene(mod, &request, nullptr, &count) ==
            WOTBMOD_V3_OK,
        "a counting pass with a null buffer succeeds");
    Check(
        g_sceneObservedDepth == WOTBMOD_V3_SCENE_WALK_MAX_DEPTH &&
            g_sceneObservedChildren ==
                WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN &&
            g_sceneObservedNodes == WOTBMOD_V3_SCENE_WALK_MAX_NODES,
        "a request above the contract ceilings is clamped to the ceilings before the provider sees it");
    Check(
        g_sceneObservedCapacity == 0u,
        "the counting pass hands the provider no buffer at all");
    Check(
        count == 5u,
        "the counting pass reports how many nodes the walk reached");

    /* Cap clamping on the way in: the provider's own lower caps. */
    ResetDoubleState();
    g_sceneProviderDepth = 4u;
    g_sceneProviderChildren = 8u;
    g_sceneProviderNodes = 6u;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_depth = 999u;
    request.max_children_per_node = 999999u;
    request.max_nodes = 999999u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_OK,
        "a walk under the provider's own caps succeeds");
    Check(
        g_sceneObservedDepth == 4u &&
            g_sceneObservedChildren == 8u &&
            g_sceneObservedNodes == 6u,
        "a provider that enforces lower caps than the ceilings gets its own three caps, not the ceilings");

    /* The mod's own request wins when it is the smallest of the three. */
    ResetDoubleState();
    g_sceneProviderDepth = 4u;
    g_sceneProviderChildren = 8u;
    g_sceneProviderNodes = 6u;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_depth = 1u;
    request.max_children_per_node = 3u;
    request.max_nodes = 2u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_OK,
        "a walk asking for less than the caps allow succeeds");
    Check(
        g_sceneObservedDepth == 1u &&
            g_sceneObservedChildren == 3u &&
            g_sceneObservedNodes == 2u,
        "a request below every cap is passed through unchanged");
    Check(
        g_sceneObservedCapacity == 2u,
        "the staging buffer handed to the provider is never larger than the effective node cap");
    Check(
        count == 2u,
        "the node cap really bounds the walk");

    /* Two-pass protocol. */
    ResetDoubleState();
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = 2u;
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
        "a buffer smaller than the walk reached is a return code the caller cannot ignore");
    Check(
        count == 5u,
        "a too-small buffer reports the count the walk actually reached, not the count written");
    Check(
        nodes[0].parent_index == WOTBMOD_V3_SCENE_NODE_NO_PARENT &&
            nodes[0].depth == 0u && nodes[0].index == 0u,
        "the root is written at index 0 with no parent");
    Check(
        (nodes[0].flags & WOTBMOD_V3_SCENE_NODE_IS_SCENE) != 0u,
        "the root record carries the IS_SCENE flag the provider set");
    Check(
        nodes[1].parent_index == 0u && nodes[1].depth == 1u,
        "a child's parent_index points backwards inside the caller's own buffer");

    ResetDoubleState();
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_OK &&
            count == 5u,
        "the second pass with a big enough buffer returns the whole walk");

    /* Cap enforcement on the way out. */
    ResetDoubleState();
    g_sceneOverReport = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_nodes = 3u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a provider reporting more nodes than the cap it was given is refused");

    ResetDoubleState();
    g_sceneOverDepth = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_depth = 2u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a record deeper than the depth cap it was given is refused");

    ResetDoubleState();
    g_sceneOverChildren = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_children_per_node = 2u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a record claiming more children than the cap without the truncation flag is refused");

    ResetDoubleState();
    g_sceneOverChildren = true;
    g_sceneOverChildrenFlagged = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.max_children_per_node = 2u;
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_OK,
        "a record over the children cap IS allowed once it admits the truncation, because child_count is what the engine reported");

    ResetDoubleState();
    g_sceneBadParent = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a parent_index that is not strictly before its child is refused, so a mod can never be walked into a cycle");

    ResetDoubleState();
    g_sceneUnterminatedName = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "an unterminated node name is refused instead of being handed to a mod");

    ResetDoubleState();
    g_sceneNonFiniteMatrix = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a non-finite world matrix is refused");

    ResetDoubleState();
    g_sceneUnknownFlag = true;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a node record carrying an unknown flag bit is refused");

    ResetDoubleState();
    g_sceneWalkResult = WOTBMOD_V3_E_NOT_FOUND;
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    count = static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(mod, &request, nodes, &count) ==
            WOTBMOD_V3_E_NOT_FOUND,
        "no active scene is NOT_FOUND -- the normal answer on a UI-only screen -- and never NOT_SUPPORTED");
}

void TestSceneThreadDiscipline(WotbModV3Handle mod) {
    ResetDoubleState();
    const uint32_t walkBefore = g_sceneWalkCalls;
    const uint32_t limitsBefore = g_sceneLimitCalls;
    WotbModV3Result walkResult = WOTBMOD_V3_OK;
    WotbModV3Result limitsResult = WOTBMOD_V3_E_PLATFORM;
    std::thread worker([&]() {
        WotbModV3SceneWalkRequest request = {};
        WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
        WotbModV3SceneNodeRecord nodes[8] = {};
        uint32_t count =
            static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
        walkResult =
            g_scene->walk_active_scene(mod, &request, nodes, &count);
        WotbModV3SceneWalkLimits limits = {};
        WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
        limitsResult = g_scene->get_limits(mod, &limits);
    });
    worker.join();
    Check(
        walkResult == WOTBMOD_V3_E_WRONG_THREAD,
        "walking the active scene off the main thread is refused: AddNode/RemoveNode memmove the child vector with no lock");
    Check(
        g_sceneWalkCalls == walkBefore,
        "the off-thread walk is refused BEFORE the provider is called, so no engine pointer is ever reached");
    Check(
        g_sceneLimitCalls == limitsBefore + 1u,
        "the thread gate runs before even the limits round-trip that the walk would need");
    Check(
        limitsResult == WOTBMOD_V3_OK,
        "get_limits stays callable from any thread because it touches no engine memory");
}

void TestTracerBound(WotbModV3Handle mod) {
    uint32_t count = 0u;
    WotbModV3TracerStyle style = {};

    ResetDoubleState();
    Check(
        g_tracer->get_shell_type_count(mod, &count) == WOTBMOD_V3_OK &&
            count == WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT,
        "the client's shell-type table length is reported as the proven 25 entries");

    ResetDoubleState();
    g_tracerCount = 99u;
    count = 0u;
    Check(
        g_tracer->get_shell_type_count(mod, &count) == WOTBMOD_V3_OK &&
            count == WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT,
        "a provider claiming more entries than the frozen bound is clamped, so a mod's loop cannot walk past the table");

    ResetDoubleState();
    g_tracerCount = 0u;
    Check(
        g_tracer->get_shell_type_count(mod, &count) ==
            WOTBMOD_V3_E_PLATFORM,
        "a provider reporting an empty shell-type table is a host bug");

    ResetDoubleState();
    bool allAccepted = true;
    bool allEchoed = true;
    for (uint32_t shellType = 0u;
         shellType < WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT;
         ++shellType) {
        style = {};
        WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
        if (g_tracer->get_style_for_shell_type(
                mod,
                shellType,
                &style) != WOTBMOD_V3_OK) {
            allAccepted = false;
        }
        if (style.shell_type != shellType ||
            (style.flags & WOTBMOD_V3_TRACER_STYLE_NAME_VALID) == 0u ||
            style.style_name[0] == '\0') {
            allEchoed = false;
        }
    }
    Check(
        allAccepted,
        "every shell type inside the proven bound 0..24 is accepted");
    Check(
        allEchoed,
        "every accepted shell type echoes its own code back with a valid style name");

    ResetDoubleState();
    const uint32_t styleCallsBefore = g_tracerStyleCalls;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(
            mod,
            WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT,
            &style) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "shell type 25 is an argument error, never a clamp to the last entry");
    Check(
        g_tracerStyleCalls == styleCallsBefore,
        "an out-of-range shell type is refused before the provider is consulted at all");
    Check(
        style.style_name[0] == '\0' && style.style_id == 0u,
        "a refused shell type leaves the caller's style struct untouched");
    Check(
        g_tracer->get_style_for_shell_type(
            mod,
            0xFFFFFFFFu,
            &style) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a wildly out-of-range shell type is the same argument error");

    ResetDoubleState();
    g_tracerCount = 8u;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 7u, &style) ==
            WOTBMOD_V3_OK,
        "the last entry of a shorter client table is accepted");
    Check(
        g_tracer->get_style_for_shell_type(mod, 8u, &style) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a shell type past THIS client's table is refused even though the frozen bound would allow it");

    ResetDoubleState();
    g_tracerFlags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID;
    g_tracerColor = {1.0f, 1.0f, 1.0f, 1.0f};
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_OK,
        "a style with a name but no colour still succeeds");
    Check(
        (style.flags & WOTBMOD_V3_TRACER_STYLE_COLOR_VALID) == 0u &&
            style.color.r == 0.0f && style.color.a == 0.0f,
        "a colour whose record was never read is forced to zero: record+0x34 is INFERRED and a guessed colour must not look measured");

    ResetDoubleState();
    g_tracerFlags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID |
        WOTBMOD_V3_TRACER_STYLE_COLOR_VALID;
    g_tracerColor = {0.25f, 0.5f, 0.75f, 1.0f};
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_OK &&
            (style.flags & WOTBMOD_V3_TRACER_STYLE_COLOR_VALID) != 0u &&
            style.color.g == 0.5f,
        "a colour that really was read survives with its own validity flag");

    ResetDoubleState();
    g_tracerFlags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID |
        WOTBMOD_V3_TRACER_STYLE_COLOR_VALID;
    g_tracerColor.r = std::numeric_limits<float>::quiet_NaN();
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_OK,
        "a non-finite colour costs the caller the colour, not the style name it asked for");
    Check(
        (style.flags & WOTBMOD_V3_TRACER_STYLE_COLOR_VALID) == 0u &&
            (style.flags & WOTBMOD_V3_TRACER_STYLE_NAME_VALID) != 0u,
        "the name and the colour are independently valid");

    ResetDoubleState();
    g_tracerFlags = 0u;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_E_PLATFORM,
        "OK without NAME_VALID is a contract violation and reads to the mod as a host bug");

    ResetDoubleState();
    g_tracerEmptyName = true;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_E_PLATFORM,
        "a NAME_VALID flag over an empty name is refused");

    ResetDoubleState();
    g_tracerStyleId = WOTBMOD_V3_TRACER_STYLE_STT_TRACER + 1u;
    style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(mod, 0u, &style) ==
            WOTBMOD_V3_OK &&
            style.style_id == WOTBMOD_V3_TRACER_STYLE_UNKNOWN &&
            std::strcmp(style.style_name, "ARMOR_PIERCING") == 0,
        "a ninth style in a future build degrades to UNKNOWN while the real name stays authoritative");
}

void TestStructValidation(
    WotbModV3Handle uiMod,
    WotbModV3UiHandle control,
    WotbModV3Handle cameraMod,
    WotbModV3Handle sceneMod,
    WotbModV3Handle tracerMod) {
    ResetDoubleState();

    WotbModV3UiStyleSnapshot style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    style.struct_size = static_cast<uint32_t>(sizeof(style) - 1u);
    Check(
        g_ui->control_get_style(uiMod, control, &style) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a short UI style snapshot is refused");
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    style.struct_size = 0u;
    Check(
        g_ui->control_get_style(uiMod, control, &style) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a UI style snapshot lying about its own size is refused");
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4 + 1u);
    Check(
        g_ui->control_get_style(uiMod, control, &style) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a future-versioned UI style snapshot is refused rather than reinterpreted");
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION);
    Check(
        g_ui->control_get_style(uiMod, control, &style) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a UI style snapshot stamped with another UI interface's version is refused");
    Check(
        g_ui->control_get_style(uiMod, control, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a null UI style snapshot is refused");

    WotbModV3CameraObservedState state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    state.struct_size = static_cast<uint32_t>(sizeof(state) - 1u);
    Check(
        g_camera->get_observed_state(cameraMod, &state) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a short camera observed-state struct is refused");
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    state.struct_size = 0u;
    Check(
        g_camera->get_observed_state(cameraMod, &state) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a camera observed-state struct lying about its own size is refused");
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2 + 1u);
    Check(
        g_camera->get_observed_state(cameraMod, &state) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a future-versioned camera observed-state struct is refused");
    Check(
        g_camera->get_animation_state(cameraMod, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT &&
            g_camera->get_view_mode(cameraMod, nullptr) ==
                WOTBMOD_V3_E_INVALID_ARGUMENT,
        "the two scalar camera slots refuse a null output");

    WotbModV3SceneWalkLimits limits = {};
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    limits.struct_size = static_cast<uint32_t>(sizeof(limits) - 1u);
    Check(
        g_scene->get_limits(sceneMod, &limits) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a short scene walk limits struct is refused");
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    limits.struct_size = 0u;
    Check(
        g_scene->get_limits(sceneMod, &limits) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a scene walk limits struct lying about its own size is refused");
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2 + 1u);
    Check(
        g_scene->get_limits(sceneMod, &limits) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a future-versioned scene walk limits struct is refused");

    WotbModV3SceneWalkRequest request = {};
    WotbModV3SceneNodeRecord nodes[4] = {};
    uint32_t count =
        static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.struct_size = static_cast<uint32_t>(sizeof(request) - 1u);
    Check(
        g_scene->walk_active_scene(sceneMod, &request, nodes, &count) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a short scene walk request is refused");
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    request.struct_size = 0u;
    Check(
        g_scene->walk_active_scene(sceneMod, &request, nodes, &count) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a scene walk request lying about its own size is refused");
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2 + 1u);
    Check(
        g_scene->walk_active_scene(sceneMod, &request, nodes, &count) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a future-versioned scene walk request is refused");
    request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    Check(
        g_scene->walk_active_scene(sceneMod, &request, nodes, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a scene walk with no count pointer is refused: the two-pass protocol needs it in both directions");

    WotbModV3TracerStyle style2 = {};
    WOTBMOD_V3_INIT_STRUCT(style2, WOTBMOD_V3_TRACER_VERSION);
    style2.struct_size = static_cast<uint32_t>(sizeof(style2) - 1u);
    Check(
        g_tracer->get_style_for_shell_type(tracerMod, 0u, &style2) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a short tracer style struct is refused");
    WOTBMOD_V3_INIT_STRUCT(style2, WOTBMOD_V3_TRACER_VERSION);
    style2.struct_size = 0u;
    Check(
        g_tracer->get_style_for_shell_type(tracerMod, 0u, &style2) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a tracer style struct lying about its own size is refused");
    WOTBMOD_V3_INIT_STRUCT(style2, WOTBMOD_V3_TRACER_VERSION + 1u);
    Check(
        g_tracer->get_style_for_shell_type(tracerMod, 0u, &style2) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a future-versioned tracer style struct is refused");
    Check(
        g_tracer->get_shell_type_count(tracerMod, nullptr) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "the tracer count slot refuses a null output");
}

void TestNamedGrantsAtTheSlot(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle ungranted) {
    ResetDoubleState();
    const void* table = nullptr;
    uint32_t value = 0u;
    if (bootstrap->query_interface(
            ungranted,
            WOTBMOD_V3_IFACE_CAMERA_STATE,
            WOTBMOD_V3_CAMERA_VERSION_2,
            &table) == WOTBMOD_V3_OK) {
        const WotbModV3CameraApiV2* api =
            static_cast<const WotbModV3CameraApiV2*>(table);
        Check(
            api->get_animation_state(ungranted, &value) ==
                WOTBMOD_V3_E_PERMISSION_DENIED,
            "camera.state enforces the camera.battle.read grant inside the slot, not only at query time");
    }
    table = nullptr;
    if (bootstrap->query_interface(
            ungranted,
            WOTBMOD_V3_IFACE_TRACER,
            WOTBMOD_V3_TRACER_VERSION,
            &table) == WOTBMOD_V3_OK) {
        const WotbModV3TracerApiV1* api =
            static_cast<const WotbModV3TracerApiV1*>(table);
        Check(
            api->get_shell_type_count(ungranted, &value) ==
                WOTBMOD_V3_E_PERMISSION_DENIED,
            "tracer enforces the visible.projectile.events grant inside the slot");
    }
    table = nullptr;
    if (bootstrap->query_interface(
            ungranted,
            WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
            WOTBMOD_V3_SCENE_VERSION_2,
            &table) == WOTBMOD_V3_OK) {
        const WotbModV3SceneApiV2* api =
            static_cast<const WotbModV3SceneApiV2*>(table);
        WotbModV3SceneWalkLimits limits = {};
        WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
        Check(
            api->get_limits(ungranted, &limits) ==
                WOTBMOD_V3_E_PERMISSION_DENIED,
            "scene.enumerate enforces the game.entity.public grant inside the slot");
    }
    table = nullptr;
    if (bootstrap->query_interface(
            ungranted,
            WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
            WOTBMOD_V3_AUDIO_VERSION_3,
            &table) == WOTBMOD_V3_OK) {
        const WotbModV3AudioApiV3* api =
            static_cast<const WotbModV3AudioApiV3*>(table);
        WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
        Check(
            api->intercept_register(
                ungranted,
                "guns/tracers/tracer_hard",
                0,
                &InterceptCallback,
                nullptr,
                &token) == WOTBMOD_V3_E_PERMISSION_DENIED,
            "audio.intercept enforces the audio.events grant inside the slot");
    }
}

void TestCameraContextGate(WotbModV3Handle mod) {
    ResetDoubleState();
    g_cameraAnimationValid = 1u;
    g_cameraAnimation = WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT;
    uint32_t value = 0u;
    Check(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
            WOTBMOD_V3_OK,
        "the runtime context can be moved to the hangar");
    Check(
        g_camera->get_animation_state(mod, &value) ==
            WOTBMOD_V3_E_INCOMPATIBLE,
        "in the hangar there is no battle CameraController, so the answer is INCOMPATIBLE rather than a fabricated state");
    Check(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
            WOTBMOD_V3_OK,
        "the runtime context can be moved to battle");
    Check(
        g_camera->get_animation_state(mod, &value) == WOTBMOD_V3_OK,
        "in battle the same read succeeds");
    Check(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_NONE) ==
            WOTBMOD_V3_OK,
        "the runtime context is restored");
}

/* ==================================================================
 * PHASE 3 -- the double is gone. This is the portable build's state.
 * ================================================================== */

void TestEverySlotRefusesWithoutABackend(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle uiMod,
    WotbModV3UiHandle control,
    WotbModV3Handle cameraMod,
    WotbModV3Handle audioMod,
    WotbModV3Handle sceneMod,
    WotbModV3Handle tracerMod) {
    char buffer[64] = {};
    uint32_t size = sizeof(buffer);
    Check(
        g_ui->control_get_text(uiMod, control, buffer, &size) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "ui.read control_get_text refuses with no backend");
    Check(
        Contains(
            LastError(bootstrap, uiMod),
            "ui_control_get_text") &&
            Contains(
                LastError(bootstrap, uiMod),
                "native client backend is not installed"),
        "the refusal names the operation and says the backend is missing, not that the mod did something wrong");
    size = sizeof(buffer);
    Check(
        g_ui->control_get_texture(uiMod, control, buffer, &size) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "ui.read control_get_texture refuses with no backend");
    size = sizeof(buffer);
    Check(
        g_ui->control_get_font(uiMod, control, buffer, &size) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "ui.read control_get_font refuses with no backend");
    WotbModV3UiStyleSnapshot style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION_4);
    Check(
        g_ui->control_get_style(uiMod, control, &style) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "ui.read control_get_style refuses with no backend");

    uint32_t value = 0u;
    Check(
        g_camera->get_animation_state(cameraMod, &value) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "camera.state get_animation_state refuses with no backend");
    Check(
        g_camera->get_view_mode(cameraMod, &value) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "camera.state get_view_mode refuses with no backend");
    WotbModV3CameraObservedState state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_CAMERA_VERSION_2);
    Check(
        g_camera->get_observed_state(cameraMod, &state) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "camera.state get_observed_state refuses with no backend");
    Check(
        Contains(
            LastError(bootstrap, cameraMod),
            "camera_get_observed_state"),
        "the camera refusal names the operation that could not be performed");

    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        g_audio->intercept_register(
            audioMod,
            "guns/tracers/tracer_hard",
            0,
            &InterceptCallback,
            nullptr,
            &token) == WOTBMOD_V3_E_NOT_SUPPORTED,
        "audio.intercept intercept_register refuses with no backend");
    Check(
        token == WOTBMOD_V3_INVALID_HANDLE,
        "a refused registration hands back no token");
    Check(
        g_audio->intercept_unregister(audioMod, UINT64_C(0x99)) ==
            WOTBMOD_V3_E_INVALID_HANDLE,
        "with no backend no interception token can exist, so unregister can only refuse the handle");
    Check(
        g_audio->intercept_set_priority(
            audioMod,
            UINT64_C(0x99),
            1) == WOTBMOD_V3_E_INVALID_HANDLE,
        "with no backend intercept_set_priority can only refuse the handle");
    uint32_t active = 0xFFFFFFFFu;
    Check(
        g_audio->is_intercept_active(audioMod, &active) ==
            WOTBMOD_V3_OK &&
            active == 0u,
        "is_intercept_active answers OK with zero even with no backend: it reports this thread's own re-entry flag, not a native capability");
    WotbModV3SoundInterceptResponse response = {};
    Check(
        wotbmod::v3::DispatchClientHostSoundIntercept(
            "guns/tracers/tracer_hard",
            &response) == WOTBMOD_V3_E_NOT_SUPPORTED,
        "with the detour gate cleared the ingress refuses every dispatch");

    WotbModV3SceneWalkLimits limits = {};
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_SCENE_VERSION_2);
    Check(
        g_scene->get_limits(sceneMod, &limits) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "scene.enumerate get_limits refuses with no backend");
    WotbModV3SceneWalkRequest request = {};
    WOTBMOD_V3_INIT_STRUCT(request, WOTBMOD_V3_SCENE_VERSION_2);
    WotbModV3SceneNodeRecord nodes[4] = {};
    uint32_t count =
        static_cast<uint32_t>(sizeof(nodes) / sizeof(nodes[0]));
    Check(
        g_scene->walk_active_scene(sceneMod, &request, nodes, &count) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "scene.enumerate walk_active_scene refuses with no backend");
    Check(
        Contains(
            LastError(bootstrap, sceneMod),
            "scene_walk_active_scene"),
        "the scene refusal names the operation that could not be performed");

    Check(
        g_tracer->get_shell_type_count(tracerMod, &value) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "tracer get_shell_type_count refuses with no backend");
    WotbModV3TracerStyle tracerStyle = {};
    WOTBMOD_V3_INIT_STRUCT(tracerStyle, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(
            tracerMod,
            0u,
            &tracerStyle) == WOTBMOD_V3_E_NOT_SUPPORTED,
        "tracer get_style_for_shell_type refuses with no backend");
    WOTBMOD_V3_INIT_STRUCT(tracerStyle, WOTBMOD_V3_TRACER_VERSION);
    Check(
        g_tracer->get_style_for_shell_type(
            tracerMod,
            WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT,
            &tracerStyle) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "the shell-type bound is checked before the backend, so an out-of-range code is an argument error whether or not a backend exists");
}

void TestFrozenReasonsRestored(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    for (uint32_t index = 0u; index < kDeclaredCount; ++index) {
        const WotbModV3InterfaceInfo info =
            Info(bootstrap, mod, kDeclared[index].name);
        Check(
            info.status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
            "removing the backend takes the interface back to UNAVAILABLE");
        Check(
            g_frozenReason[index] == info.unavailable_reason,
            "the restored reason is byte-identical to the one a fresh process publishes");
        const void* table = reinterpret_cast<const void*>(1u);
        Check(
            bootstrap->query_interface(
                mod,
                kDeclared[index].name,
                kDeclared[index].version,
                &table) == WOTBMOD_V3_E_NOT_SUPPORTED &&
                table == nullptr,
            "query_interface refuses the interface again once its backend is gone");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1
        ? std::string(argv[1])
        : std::string("build\\v3_post_rc1_services\\env");
    const std::string mods = root + "\\mods";
    const std::string cache = root + "\\cache";
    const std::string config = root + "\\config";

    WotbModV3RuntimeOptions options = {};
    WOTBMOD_V3_INIT_STRUCT(options, WOTBMOD_V3_ABI_VERSION);
    options.game_directory = ".";
    options.mods_directory = mods.c_str();
    options.cache_directory = cache.c_str();
    options.config_directory = config.c_str();
    options.client_version = "post-rc1-services-test";
    options.executable_sha256 = "post-rc1-services-sha";
    options.process_architecture = 32u;
    Check(
        WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK,
        "the runtime initializes");
    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    Check(bootstrap != nullptr, "the bootstrap table is available");
    if (!bootstrap) {
        WotbModV3Runtime_Shutdown();
        std::printf(
            "V3 post-RC1 declared services tests: %u passed, %u failed\n",
            g_passed,
            g_failed);
        return 1;
    }

    ClientHostBackend hostBackend = {};
    hostBackend.struct_size = sizeof(hostBackend);
    hostBackend.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    hostBackend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    hostBackend.invoke = &InvokeHost;
    wotbmod::v3::SetClientHostBackend(&hostBackend);

    const char* const uiGrants[] = {"ui.create", "ui.modify.own"};
    const char* const cameraGrants[] = {"camera.battle.read"};
    const char* const audioGrants[] = {"audio.events"};
    const char* const sceneGrants[] = {"game.entity.public"};
    const char* const tracerGrants[] = {"visible.projectile.events"};
    const char* const coreGrants[] = {"core"};

    const WotbModV3Handle uiMod = CreateEnabledMod(
        "post-rc1-ui",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        uiGrants,
        2u);
    const WotbModV3Handle uiOtherMod = CreateEnabledMod(
        "post-rc1-ui-other",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        uiGrants,
        2u);
    const WotbModV3Handle cameraMod = CreateEnabledMod(
        "post-rc1-camera",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        cameraGrants,
        1u);
    const WotbModV3Handle audioMod = CreateEnabledMod(
        "post-rc1-audio",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        audioGrants,
        1u);
    const WotbModV3Handle sceneMod = CreateEnabledMod(
        "post-rc1-scene",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        sceneGrants,
        1u);
    const WotbModV3Handle tracerMod = CreateEnabledMod(
        "post-rc1-tracer",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        tracerGrants,
        1u);
    const WotbModV3Handle ungrantedMod = CreateEnabledMod(
        "post-rc1-ungranted",
        WOTBMOD_V3_PERMISSION_UNSAFE,
        coreGrants,
        1u);

    /*
     * A mod holding every permission family the five declared interfaces
     * reuse. The phases below enumerate all five at once, and every mod here
     * is named-permission-restricted, so a mod is refused at the interface
     * door for any family it does not hold. That refusal is the gate working
     * as designed, not a failure -- but it makes a single-family mod the
     * wrong instrument for a question about all five.
     *
     * uiMod deliberately keeps only the UI family, because the phases where
     * the refusal itself is the subject use it, and ungrantedMod holds only
     * "core" so the slot-level named-grant checks have something that is
     * refused everywhere.
     */
    const char* const allGrants[] = {
        "ui.create",
        "ui.modify.own",
        "camera.battle.read",
        "audio.events",
        "game.entity.public",
        "visible.projectile.events"};
    const WotbModV3Handle allMod = CreateEnabledMod(
        "post-rc1-all",
        WOTBMOD_V3_PERMISSION_UNSAFE,
        allGrants,
        static_cast<uint32_t>(
            sizeof(allGrants) / sizeof(allGrants[0])));

    /* Phase 1: nothing installed. */
    TestFrozenRegistration(bootstrap, allMod);
    TestFingerprintGate(bootstrap, allMod);
    TestGroupsAreAllOrNothing(bootstrap, allMod);

    /*
     * The interface-level named-permission gate, which the five declared ids
     * were missing until 2026-08-16. Without an entry in the runtime's match
     * table a restricted mod fell through to "granted" and was only stopped
     * later, per slot. Both directions are asserted: holding the family opens
     * the door, and holding a different family does not.
     */
    {
        const void* gated = nullptr;
        Check(
            bootstrap->query_interface(
                uiMod,
                WOTBMOD_V3_IFACE_CAMERA_STATE,
                WOTBMOD_V3_CAMERA_VERSION_2,
                &gated) == WOTBMOD_V3_E_PERMISSION_DENIED &&
                gated == nullptr,
            "a restricted mod holding only the UI family is refused "
            "camera.state at the interface door, not at the slot");
        gated = nullptr;
        Check(
            bootstrap->query_interface(
                uiMod,
                WOTBMOD_V3_IFACE_TRACER,
                WOTBMOD_V3_TRACER_VERSION,
                &gated) == WOTBMOD_V3_E_PERMISSION_DENIED &&
                gated == nullptr,
            "the same mod is refused tracer at the interface door");
        gated = nullptr;
        Check(
            bootstrap->query_interface(
                allMod,
                WOTBMOD_V3_IFACE_CAMERA_STATE,
                WOTBMOD_V3_CAMERA_VERSION_2,
                &gated) == WOTBMOD_V3_E_NOT_SUPPORTED,
            "a mod that does hold the camera family gets past the door and "
            "is refused for the honest reason instead: no backend");
    }

    /* Two UI controls: one for the reader, one owned by another mod. */
    const void* uiTable = nullptr;
    Check(
        bootstrap->query_interface(
            uiMod,
            WOTBMOD_V3_IFACE_UI,
            WOTBMOD_V3_UI_VERSION,
            &uiTable) == WOTBMOD_V3_OK,
        "the existing wotbmod.ui interface is queryable");
    const WotbModV3UiApiV2* uiApi =
        static_cast<const WotbModV3UiApiV2*>(uiTable);
    uiTable = nullptr;
    Check(
        bootstrap->query_interface(
            uiOtherMod,
            WOTBMOD_V3_IFACE_UI,
            WOTBMOD_V3_UI_VERSION,
            &uiTable) == WOTBMOD_V3_OK,
        "the second mod can query wotbmod.ui as well");
    const WotbModV3UiApiV2* uiOtherApi =
        static_cast<const WotbModV3UiApiV2*>(uiTable);

    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.visible = 1u;
    descriptor.id = "post-rc1-readback";
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle foreignControl = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        uiApi && uiApi->control_create(uiMod, &descriptor, &control) ==
            WOTBMOD_V3_OK,
        "a mod-owned UI control is created for readback");
    Check(
        uiOtherApi &&
            uiOtherApi->control_create(
                uiOtherMod,
                &descriptor,
                &foreignControl) == WOTBMOD_V3_OK,
        "a second mod-owned UI control is created for the ownership check");

    /* Phase 2: portable test double installed. */
    ResetDoubleState();
    ClientHostDeclaredBackend declared = MakeDeclaredBackend();
    wotbmod::v3::SetClientHostDeclaredBackend(&declared);
    CaptureTables(
        bootstrap,
        allMod,
        uiMod,
        cameraMod,
        audioMod,
        sceneMod,
        tracerMod);
    if (g_ui && g_camera && g_audio && g_scene && g_tracer) {
        TestUiReadProvenance(bootstrap, uiMod, control, foreignControl);
        TestUiReadStyle(uiMod, control);
        TestCameraIndependentValidity(cameraMod);
        TestCameraContextGate(cameraMod);
        TestAudioInterception(audioMod);
        TestSceneWalk(sceneMod);
        TestSceneThreadDiscipline(sceneMod);
        TestTracerBound(tracerMod);
        TestStructValidation(
            uiMod,
            control,
            cameraMod,
            sceneMod,
            tracerMod);
        TestNamedGrantsAtTheSlot(bootstrap, ungrantedMod);
    }

    /* Phase 3: the double is removed; this is the shipped portable state. */
    const uint32_t disarmBefore = g_disarmCalls;
    wotbmod::v3::SetClientHostDeclaredBackend(nullptr);
    Check(
        g_disarmCalls == disarmBefore + 1u,
        "removing a backend whose detour was armed disarms it exactly once");
    TestFrozenReasonsRestored(bootstrap, allMod);
    if (g_ui && g_camera && g_audio && g_scene && g_tracer) {
        const uint32_t uiCalls = g_uiStringCalls + g_uiStyleCalls;
        const uint32_t cameraCalls = g_cameraCalls;
        const uint32_t sceneCalls = g_sceneLimitCalls + g_sceneWalkCalls;
        const uint32_t tracerCalls =
            g_tracerCountCalls + g_tracerStyleCalls;
        const uint32_t publishCalls = g_publishCalls;
        TestEverySlotRefusesWithoutABackend(
            bootstrap,
            uiMod,
            control,
            cameraMod,
            audioMod,
            sceneMod,
            tracerMod);
        Check(
            g_uiStringCalls + g_uiStyleCalls == uiCalls &&
                g_cameraCalls == cameraCalls &&
                g_sceneLimitCalls + g_sceneWalkCalls == sceneCalls &&
                g_tracerCountCalls + g_tracerStyleCalls == tracerCalls &&
                g_publishCalls == publishCalls,
            "with the backend removed not one slot reached a function pointer the runtime no longer holds");
    }

    wotbmod::v3::SetClientHostBackend(nullptr);
    WotbModV3Runtime_Shutdown();
    std::printf(
        "V3 post-RC1 declared services tests: %u passed, %u failed\n",
        g_passed,
        g_failed);
    return g_failed == 0u ? 0 : 1;
}
