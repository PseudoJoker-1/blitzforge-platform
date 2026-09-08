#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "v3_native_client_services.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/ui_v2.h"
#include "anchor_rvas.h"

namespace wotbmod {
namespace loader {
namespace {

using v3::ClientHostObjectRequest;
using v3::ClientHostObjectResponse;

const uint32_t kSceneMagic = 0x334E4353u; /* SCN3 */
const uint32_t kAudioMagic = 0x33445541u; /* AUD3 */
const uint32_t kUiMagic = 0x33495555u; /* UUI3 */

enum class AudioKind : uint32_t {
    Clip = 1u,
    SoundEvent = 2u
};

struct NativeScene {
    uint32_t magic = kSceneMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    void* resource = nullptr;
    NativeScene* parent = nullptr;
    WotbModV3Transform local = {};
    char resolved_path[WOTBMOD_V3_MAX_PATH] = {};
    bool transient_attached = false;
};

struct TransientSceneAttachment {
    NativeScene* scene = nullptr;
    void* active_scene_resource = nullptr;
    void* child_resource = nullptr;
};

struct NativeUi {
    uint32_t magic = kUiMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    void* resource = nullptr;
    void* content_resource = nullptr;
    NativeUi* parent = nullptr;
    void* slot_parent_resource = nullptr;
    WotbModUiControlGeometry geometry = {};
    uint32_t visible = 1u;
    uint32_t enabled = 1u;
    uint32_t interactable = 1u;
    uint32_t type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    uint32_t text_alignment = WOTBMOD_V3_UI_TEXT_ALIGN_LEFT;
    uint32_t text_wrap = 0u;
    uint32_t rich_text = 0u;
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    WotbModV3Color background_color = {0.10f, 0.12f, 0.16f, 0.92f};
    float opacity = 1.0f;
    float font_size = 24.0f;
    /*
     * WotbModV3UiReadFlag bits for the fields THIS API committed.
     *
     * ui_v4.h makes "never written" and "written to the empty string" two
     * different answers, and this mask is the only thing that can tell them
     * apart: every mirrored value below has a construction default that is
     * indistinguishable from a real write. A bit is set only on a path that
     * actually returned WOTBMOD_V3_OK, so the mask never claims a write the
     * engine refused. WOTBMOD_V3_UI_READ_GAME_OWNED is deliberately absent:
     * the runtime owns that bit and ORs its own in, so a loader bug cannot
     * drop the staleness warning.
     */
    uint32_t mirror_flags = 0u;
    bool dynamic = false;
    bool template_backed = false;
    bool owned_created = false;
    bool rebuild_pending = false;
    uint32_t lifetime_leases = 0u;
    bool destroy_requested = false;
    std::string text;
    std::string texture;
    std::string font;
    std::string source_path;
    std::string source_object;
};

struct RetiredUiResource {
    void* resource = nullptr;
    uint64_t release_after_frame = 0u;
};

struct NativeAudio {
    uint32_t magic = kAudioMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    AudioKind kind = AudioKind::Clip;
    void* clip = nullptr;
    void* playback = nullptr;
    void* sound_event = nullptr;
    uint32_t state = WOTBMOD_V3_AUDIO_STATE_CREATED;
    WotbModAudioPlayInfo parameters = {};
    char name[WOTBMOD_V3_MAX_PATH] = {};
};

struct ServicesState {
    std::mutex mutex;
    std::condition_variable operations_finished;
    WotbModRuntimeResourceBackend resource = {};
    WotbModRuntimeAudioBackend audio = {};
    WotbModRuntimeSoundBackend sound = {};
    void* (WOTBMOD_CALL* resource_identity)(void*, void*) = nullptr;
    void* resource_identity_user_data = nullptr;
    WotbModResult (WOTBMOD_CALL* resource_bring_ui_to_front)(
        void*, void*) = nullptr;
    void* resource_bring_ui_to_front_user_data = nullptr;
    std::vector<NativeUi*> ui_objects;
    std::vector<RetiredUiResource> retired_ui_resources;
    std::vector<NativeScene*> scenes;
    std::vector<TransientSceneAttachment> transient_scenes;
    std::vector<NativeAudio*> audio_objects;
    /*
     * Image window and fingerprint verdict for the read-only scene walk.
     * Captured once at initialization; never mutated afterwards, so the walk
     * reads them without the mutex.
     */
    const uint8_t* game_base = nullptr;
    size_t game_image_size = 0u;
    uint32_t compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
    uint32_t active_ui_operations = 0u;
    uint32_t active_service_invocations = 0u;
    uint64_t ui_frame_epoch = 0u;
    uint64_t ui_mutation_resume_frame = 0u;
    bool ui_input_active = false;
    bool shutdown_requested = false;
    bool shutdown_running = false;
    bool initialized = false;
};

ServicesState g_services;
constexpr uint64_t kUiRetirementFrames = 4u;
constexpr uint64_t kUiInputIdleFrames = 2u;
volatile LONG g_generated_ui_sequence = 0;
/*
 * PROVEN main-thread identity, published by the
 * EngineBackend::UpdateAndDrawWindows detour. Zero means "never observed",
 * which is not the same as "this is not the main thread" but must be treated
 * the same way: an unproven main thread walking an unlocked child vector is
 * exactly the use-after-free scene_v2.h exists to make unrepresentable.
 */
volatile LONG g_main_thread_id = 0;
volatile LONG64 g_main_frame_index = 0;
thread_local uint32_t g_service_invocation_depth = 0u;
thread_local uint32_t g_ui_operation_depth = 0u;
thread_local bool g_service_shutdown_running = false;

class ScopedServiceInvocation {
public:
    ScopedServiceInvocation() {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (!g_services.initialized ||
            g_services.shutdown_requested ||
            g_services.shutdown_running) {
            return;
        }
        ++g_services.active_service_invocations;
        ++g_service_invocation_depth;
        active_ = true;
    }

    ~ScopedServiceInvocation() {
        if (!active_) return;
        bool run_deferred_shutdown = false;
        if (g_service_invocation_depth > 0u) {
            --g_service_invocation_depth;
        }
        {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            if (g_services.active_service_invocations > 0u) {
                --g_services.active_service_invocations;
            }
            run_deferred_shutdown =
                g_services.active_service_invocations == 0u &&
                g_services.active_ui_operations == 0u &&
                g_services.shutdown_requested &&
                !g_services.shutdown_running;
            g_services.operations_finished.notify_all();
        }
        if (run_deferred_shutdown) {
            ShutdownV3NativeClientServices();
        }
    }

    bool active() const { return active_; }

    ScopedServiceInvocation(const ScopedServiceInvocation&) = delete;
    ScopedServiceInvocation& operator=(
        const ScopedServiceInvocation&) = delete;

private:
    bool active_ = false;
};

WotbModV3Result ConvertResult(WotbModResult result) {
    switch (result) {
        case WOTBMOD_OK:
            return WOTBMOD_V3_OK;
        case WOTBMOD_ERROR_INVALID_ARGUMENT:
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        case WOTBMOD_ERROR_UNSUPPORTED_ABI:
            return WOTBMOD_V3_E_INCOMPATIBLE;
        case WOTBMOD_ERROR_NOT_FOUND:
            return WOTBMOD_V3_E_NOT_FOUND;
        case WOTBMOD_ERROR_ALREADY_EXISTS:
            return WOTBMOD_V3_E_ALREADY_EXISTS;
        case WOTBMOD_ERROR_PLATFORM:
            return WOTBMOD_V3_E_PLATFORM;
        case WOTBMOD_ERROR_ACCESS_DENIED:
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        case WOTBMOD_ERROR_BUFFER_TOO_SMALL:
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        case WOTBMOD_ERROR_DISABLED:
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        case WOTBMOD_ERROR_LIMIT_REACHED:
            return WOTBMOD_V3_E_LIMIT_REACHED;
        case WOTBMOD_ERROR_CALLBACK_FAULT:
            return WOTBMOD_V3_E_CALLBACK_FAULT;
        case WOTBMOD_ERROR_WRONG_THREAD:
            return WOTBMOD_V3_E_WRONG_THREAD;
        default:
            return WOTBMOD_V3_E_PLATFORM;
    }
}

bool CopyText(char* output, size_t capacity, const char* value) {
    if (!output || capacity == 0u || !value) return false;
    const size_t length = std::strlen(value);
    if (length >= capacity) return false;
#if defined(_MSC_VER)
    return strcpy_s(output, capacity, value) == 0;
#else
    std::memcpy(output, value, length + 1u);
    return true;
#endif
}

bool ValidObjectRequest(
    const void* request,
    uint32_t request_size,
    const ClientHostObjectRequest** out_request) {
    if (!request || request_size < sizeof(ClientHostObjectRequest) ||
        !out_request) {
        return false;
    }
    const ClientHostObjectRequest* typed =
        static_cast<const ClientHostObjectRequest*>(request);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *out_request = typed;
    return true;
}

bool ValidObjectResponse(
    void* response,
    uint32_t response_size,
    ClientHostObjectResponse** out_response) {
    if (!response ||
        response_size < sizeof(ClientHostObjectResponse) ||
        !out_response) {
        return false;
    }
    ClientHostObjectResponse* typed =
        static_cast<ClientHostObjectResponse*>(response);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *out_response = typed;
    return true;
}

WotbModResult CallResourceCreate(
    WotbModRuntimeResourceCreate function,
    void** output) {
    if (!function || !output) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(g_services.resource.user_data, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallResourceLoad(
    const char* virtual_path,
    const char* resolved_path,
    void** output) {
    if (!output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resolved_path && resolved_path[0] &&
        g_services.resource.load_resolved) {
        __try {
            return g_services.resource.load_resolved(
                g_services.resource.user_data,
                WOTBMOD_RESOURCE_SCENE,
                resolved_path,
                nullptr,
                0u,
                output);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    if (!virtual_path || !virtual_path[0] ||
        !g_services.resource.load) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    WotbModResourceLoadRequest load = {};
    load.struct_size = sizeof(load);
    load.type = WOTBMOD_RESOURCE_SCENE;
    load.virtual_path = virtual_path;
    __try {
        return g_services.resource.load(
            g_services.resource.user_data,
            &load,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiLoadResolved(
    const char* resolved_path,
    const char* object_name,
    void** output) {
    if (!resolved_path || !resolved_path[0] ||
        !object_name || !object_name[0] || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!g_services.resource.load_resolved) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.load_resolved(
            g_services.resource.user_data,
            WOTBMOD_RESOURCE_UI_CONTROL,
            resolved_path,
            object_name,
            0u,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallResourceRelease(void* resource) {
    if (!g_services.resource.release || !resource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return g_services.resource.release(
            g_services.resource.user_data,
            resource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

void* CallResourceIdentity(void* resource) {
    if (!resource) return nullptr;
    if (!g_services.resource_identity) return resource;
    __try {
        return g_services.resource_identity(
            g_services.resource_identity_user_data,
            resource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

WotbModResult CallResourceBringUiToFront(void* resource) {
    if (!g_services.resource_bring_ui_to_front || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource_bring_ui_to_front(
            g_services.resource_bring_ui_to_front_user_data,
            resource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallResourceClone(
    void* resource,
    void** output) {
    if (!g_services.resource.clone || !resource || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.clone(
            g_services.resource.user_data,
            resource,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiCreate(
    const WotbModUiControlGeometry* geometry,
    void** output) {
    if (!g_services.resource.ui_create || !geometry || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_create(
            g_services.resource.user_data,
            geometry,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiSetGeometry(
    void* resource,
    const WotbModUiControlGeometry* geometry) {
    if (!g_services.resource.ui_set_geometry ||
        !resource || !geometry) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_set_geometry(
            g_services.resource.user_data,
            resource,
            geometry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiSetVisible(
    void* resource,
    uint32_t visible) {
    if (!g_services.resource.ui_set_visible || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_set_visible(
            g_services.resource.user_data,
            resource,
            visible != 0u ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiPair(
    WotbModRuntimeResourcePairOperation function,
    void* parent,
    void* child) {
    if (!function || !parent || !child) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return function(
            g_services.resource.user_data,
            parent,
            child);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetIdentity(
    void* resource,
    char* name,
    uint32_t name_capacity,
    char* class_name,
    uint32_t class_capacity) {
    if (!g_services.resource.ui_get_identity || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_get_identity(
            g_services.resource.user_data,
            resource,
            name,
            name_capacity,
            class_name,
            class_capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetBackgroundColor(void* resource, float* out_rgba) {
    if (!g_services.resource.ui_get_background_color || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_get_background_color(
            g_services.resource.user_data, resource, out_rgba);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiSetBackgroundColor(void* resource, const float* rgba) {
    if (!g_services.resource.ui_set_background_color || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_set_background_color(
            g_services.resource.user_data, resource, rgba);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetText(
    void* resource,
    char* buffer,
    uint32_t* inout_size) {
    if (!g_services.resource.ui_get_text || !resource || !inout_size) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_get_text(
            g_services.resource.user_data,
            resource,
            buffer,
            inout_size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetState(
    void* resource,
    WotbModUiControlState* output) {
    if (!g_services.resource.ui_get_state ||
        !resource || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.ui_get_state(
            g_services.resource.user_data,
            resource,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiFindByName(
    void* root,
    const char* name,
    uint32_t recursive,
    void** output) {
    if (!g_services.resource.ui_find_by_name || !root || !name ||
        !name[0] || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return g_services.resource.ui_find_by_name(
            g_services.resource.user_data,
            root,
            name,
            recursive != 0u ? 1 : 0,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetRelated(
    WotbModRuntimeResourceGetRelated function,
    void* resource,
    void** output) {
    if (!function || !resource || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return function(
            g_services.resource.user_data,
            resource,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetCount(
    void* resource,
    uint32_t* output) {
    if (!g_services.resource.ui_get_child_count || !resource || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return g_services.resource.ui_get_child_count(
            g_services.resource.user_data,
            resource,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiGetAt(
    void* resource,
    uint32_t index,
    void** output) {
    if (!g_services.resource.ui_get_child_at || !resource || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return g_services.resource.ui_get_child_at(
            g_services.resource.user_data,
            resource,
            index,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallUiSetFlag(
    WotbModRuntimeUiSetFlag function,
    void* resource,
    uint32_t value) {
    if (!function || !resource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return function(
            g_services.resource.user_data,
            resource,
            value != 0u ? 1 : 0,
            0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSceneTransform(
    void* resource,
    const WotbModSceneTransform* transform) {
    if (!g_services.resource.scene_set_transform ||
        !resource || !transform) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.resource.scene_set_transform(
            g_services.resource.user_data,
            resource,
            transform);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallScenePair(
    WotbModRuntimeResourcePairOperation function,
    void* parent,
    void* child) {
    if (!function || !parent || !child) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return function(
            g_services.resource.user_data,
            parent,
            child);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

NativeScene* FindSceneLocked(
    uint64_t token,
    WotbModV3Handle owner) {
    NativeScene* scene = reinterpret_cast<NativeScene*>(
        static_cast<uintptr_t>(token));
    if (!scene) return nullptr;
    const auto found = std::find(
        g_services.scenes.begin(),
        g_services.scenes.end(),
        scene);
    return found != g_services.scenes.end() &&
                   scene->magic == kSceneMagic &&
                   scene->owner == owner
        ? scene
        : nullptr;
}

NativeUi* FindUiLocked(
    uint64_t token,
    WotbModV3Handle owner) {
    NativeUi* ui = reinterpret_cast<NativeUi*>(
        static_cast<uintptr_t>(token));
    if (!ui) return nullptr;
    const auto found = std::find(
        g_services.ui_objects.begin(),
        g_services.ui_objects.end(),
        ui);
    return found != g_services.ui_objects.end() &&
                   ui->magic == kUiMagic &&
                   ui->owner == owner
        ? ui
        : nullptr;
}

NativeAudio* FindAudioLocked(
    uint64_t token,
    WotbModV3Handle owner,
    AudioKind kind) {
    NativeAudio* audio = reinterpret_cast<NativeAudio*>(
        static_cast<uintptr_t>(token));
    if (!audio) return nullptr;
    const auto found = std::find(
        g_services.audio_objects.begin(),
        g_services.audio_objects.end(),
        audio);
    return found != g_services.audio_objects.end() &&
                   audio->magic == kAudioMagic &&
                   audio->owner == owner &&
                   audio->kind == kind
        ? audio
        : nullptr;
}

WotbModUiControlGeometry ToLegacyGeometry(
    const WotbModV3Rect& geometry) {
    WotbModUiControlGeometry output = {};
    output.struct_size = sizeof(output);
    output.x = geometry.x;
    output.y = geometry.y;
    output.width = geometry.width;
    output.height = geometry.height;
    return output;
}

const char kRuntimeUiObjectName[] = "WotbModRuntimeControl";

void AppendYamlQuoted(std::string* output, const std::string& value) {
    if (!output) return;
    output->push_back('"');
    for (unsigned char character : value) {
        switch (character) {
            case '\\': output->append("\\\\"); break;
            case '"': output->append("\\\""); break;
            case '\n': output->append("\\n"); break;
            case '\r': output->append("\\r"); break;
            case '\t': output->append("\\t"); break;
            default:
                if (character >= 0x20u) {
                    output->push_back(static_cast<char>(character));
                }
                break;
        }
    }
    output->push_back('"');
}

void AppendFloat(std::string* output, float value) {
    char number[64] = {};
    _snprintf_s(number, sizeof(number), _TRUNCATE, "%.6f", value);
    output->append(number);
}

void AppendColor(
    std::string* output,
    const WotbModV3Color& color) {
    output->push_back('[');
    AppendFloat(output, color.r);
    output->append(", ");
    AppendFloat(output, color.g);
    output->append(", ");
    AppendFloat(output, color.b);
    output->append(", ");
    AppendFloat(output, color.a);
    output->push_back(']');
}

const char* TextAlignmentName(uint32_t alignment) {
    switch (alignment) {
        case WOTBMOD_V3_UI_TEXT_ALIGN_CENTER: return "HCENTER";
        case WOTBMOD_V3_UI_TEXT_ALIGN_RIGHT: return "RIGHT";
        case WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY: return "LEFT";
        default: return "LEFT";
    }
}

bool HasCaption(uint32_t type) {
    return type == WOTBMOD_V3_UI_CONTROL_BUTTON ||
           type == WOTBMOD_V3_UI_CONTROL_CHECKBOX ||
           type == WOTBMOD_V3_UI_CONTROL_SLIDER ||
           type == WOTBMOD_V3_UI_CONTROL_DROPDOWN ||
           type == WOTBMOD_V3_UI_CONTROL_TABS;
}

bool HasBackground(uint32_t type, const NativeUi& ui) {
    return type != WOTBMOD_V3_UI_CONTROL_TEXT ||
           !ui.texture.empty() || ui.background_color.a > 0.0f;
}

void AppendBackgroundComponent(
    std::string* yaml,
    const NativeUi& ui,
    const char* indent) {
    yaml->append(indent);
    yaml->append("Background:\n");
    yaml->append(indent);
    yaml->append(ui.texture.empty()
        ? "    drawType: \"DRAW_FILL\"\n"
        : "    drawType: \"DRAW_SCALE_TO_RECT\"\n");
    if (!ui.texture.empty()) {
        yaml->append(indent);
        yaml->append("    sprite: ");
        AppendYamlQuoted(yaml, ui.texture);
        yaml->push_back('\n');
    }
    yaml->append(indent);
    yaml->append("    color: ");
    AppendColor(yaml, ui.background_color);
    yaml->push_back('\n');
}

void AppendTextComponent(
    std::string* yaml,
    const NativeUi& ui,
    const char* indent) {
    yaml->append(indent);
    yaml->append("UITextComponent:\n");
    yaml->append(indent);
    yaml->append("    text: ");
    AppendYamlQuoted(yaml, ui.text);
    yaml->push_back('\n');
    if (!ui.font.empty()) {
        yaml->append(indent);
        yaml->append("    fontPath: ");
        AppendYamlQuoted(yaml, ui.font);
        yaml->push_back('\n');
    }
    yaml->append(indent);
    yaml->append("    fontSize: ");
    AppendFloat(yaml, ui.font_size);
    yaml->push_back('\n');
    yaml->append(indent);
    yaml->append("    color: ");
    AppendColor(yaml, ui.color);
    yaml->push_back('\n');
    yaml->append(indent);
    yaml->append("    colorInheritType: \"COLOR_IGNORE_PARENT\"\n");
    yaml->append(indent);
    yaml->append("    align: [\"");
    yaml->append(TextAlignmentName(ui.text_alignment));
    yaml->append("\", \"VCENTER\"]\n");
    if (ui.text_wrap != 0u) {
        yaml->append(indent);
        yaml->append("    multiline: \"MULTILINE_ENABLED\"\n");
    }
}

std::string BuildDynamicUiYaml(const NativeUi& ui) {
    const float width = ui.geometry.width > 0.0f
        ? ui.geometry.width
        : 100.0f;
    const float height = ui.geometry.height > 0.0f
        ? ui.geometry.height
        : 40.0f;
    const bool scroll =
        ui.type == WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW ||
        ui.type == WOTBMOD_V3_UI_CONTROL_LIST;
    const bool text_input =
        ui.type == WOTBMOD_V3_UI_CONTROL_TEXT_INPUT;
    const bool rich_text =
        ui.type == WOTBMOD_V3_UI_CONTROL_TEXT && ui.rich_text != 0u;
    const char* class_name =
        ui.type == WOTBMOD_V3_UI_CONTROL_BUTTON
            ? "UIButton"
            : (ui.type == WOTBMOD_V3_UI_CONTROL_TEXT && !rich_text
                   ? "UIStaticText"
                   : (text_input
                          ? "UITextField"
                          : (scroll ? "UIScrollView" : "UIControl")));

    std::string yaml;
    yaml.reserve(4096u + ui.text.size() + ui.texture.size() + ui.font.size());
    yaml.append("Header:\n    version: 155\nPrototypes:\n-   class: \"");
    yaml.append(class_name);
    yaml.append("\"\n    name: \"");
    yaml.append(kRuntimeUiObjectName);
    yaml.append("\"\n    position: [");
    AppendFloat(&yaml, ui.geometry.x);
    yaml.append(", ");
    AppendFloat(&yaml, ui.geometry.y);
    yaml.append("]\n    size: [");
    AppendFloat(&yaml, width);
    yaml.append(", ");
    AppendFloat(&yaml, height);
    yaml.append("]\n    input: ");
    yaml.append(ui.interactable != 0u ? "true\n" : "false\n");
    yaml.append("    visible: ");
    yaml.append(ui.visible != 0u ? "true\n" : "false\n");

    if (text_input) {
        yaml.append("    text: ");
        AppendYamlQuoted(&yaml, ui.text);
        yaml.push_back('\n');
        if (!ui.font.empty()) {
            yaml.append("    fontPath: ");
            AppendYamlQuoted(&yaml, ui.font);
            yaml.push_back('\n');
        }
        yaml.append("    fontSize: ");
        AppendFloat(&yaml, ui.font_size);
        yaml.append("\n    textcolor: ");
        AppendColor(&yaml, ui.color);
        yaml.append("\n    textalign: [\"");
        yaml.append(TextAlignmentName(ui.text_alignment));
        yaml.append("\", \"VCENTER\"]\n");
    }

    yaml.append("    components:\n");
    if (HasBackground(ui.type, ui)) {
        AppendBackgroundComponent(&yaml, ui, "        ");
    }
    yaml.append("        UIOpacityComponent:\n            opacity: ");
    AppendFloat(&yaml, ui.opacity);
    yaml.push_back('\n');
    if (ui.type == WOTBMOD_V3_UI_CONTROL_TEXT && !rich_text) {
        AppendTextComponent(&yaml, ui, "        ");
    }
    if (rich_text) {
        yaml.append("        FlowLayout:\n            hSpacing: 2.000000\n");
        yaml.append("        RichContent:\n            text: ");
        AppendYamlQuoted(&yaml, ui.text);
        yaml.append("\n            baseClasses: \"text-color-white text1\"\n");
    }
    if (text_input) {
        yaml.append("        Focus:\n            state: \"Enabled\"\n");
    }

    if (scroll) {
        yaml.append("    autoUpdate: true\n    children:\n");
        yaml.append("    -   class: \"UIScrollViewContainer\"\n");
        yaml.append("        name: \"WotbModRuntimeContent\"\n");
        yaml.append("        size: [");
        AppendFloat(&yaml, width);
        yaml.append(", 4096.000000]\n        input: false\n");
    } else if (HasCaption(ui.type)) {
        yaml.append("    children:\n    -   class: \"UIStaticText\"\n");
        yaml.append("        name: \"Caption\"\n        size: [");
        AppendFloat(&yaml, width);
        yaml.append(", ");
        AppendFloat(&yaml, height);
        yaml.append("]\n        input: false\n        components:\n");
        AppendTextComponent(&yaml, ui, "            ");
        yaml.append("            Anchor:\n");
        yaml.append("                leftAnchorEnabled: true\n");
        yaml.append("                rightAnchorEnabled: true\n");
        yaml.append("                topAnchorEnabled: true\n");
        yaml.append("                bottomAnchorEnabled: true\n");
    }
    return yaml;
}

bool WriteGeneratedUiYaml(
    const std::string& contents,
    std::string* output_path) {
    if (!output_path || contents.size() > MAXDWORD) return false;
    char temporary[MAX_PATH] = {};
    const DWORD length = GetTempPathA(MAX_PATH, temporary);
    if (length == 0u || length >= MAX_PATH) return false;
    char directory[MAX_PATH] = {};
    if (_snprintf_s(
            directory,
            sizeof(directory),
            _TRUNCATE,
            "%swotbmod-ui-%lu",
            temporary,
            static_cast<unsigned long>(GetCurrentProcessId())) < 0) {
        return false;
    }
    if (!CreateDirectoryA(directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    char path[MAX_PATH] = {};
    const LONG sequence = InterlockedIncrement(&g_generated_ui_sequence);
    if (_snprintf_s(
            path,
            sizeof(path),
            _TRUNCATE,
            "%s\\control-%ld.yaml",
            directory,
            static_cast<long>(sequence)) < 0) {
        return false;
    }
    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD contents_size = static_cast<DWORD>(contents.size());
    DWORD written = 0u;
    const BOOL success = WriteFile(
        file,
        contents.data(),
        contents_size,
        &written,
        nullptr);
    CloseHandle(file);
    if (!success || written != contents_size) {
        DeleteFileA(path);
        return false;
    }
    *output_path = path;
    return true;
}

WotbModResult LoadDynamicUiResource(
    const NativeUi& ui,
    void** output) {
    if (!output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    const std::string yaml = BuildDynamicUiYaml(ui);
    std::string path;
    if (!WriteGeneratedUiYaml(yaml, &path)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    const WotbModResult result = CallUiLoadResolved(
        path.c_str(), kRuntimeUiObjectName, output);
    DeleteFileA(path.c_str());
    return result;
}

void* UiAttachmentResource(const NativeUi* ui) {
    if (!ui) return nullptr;
    return ui->content_resource
        ? ui->content_resource
        : ui->resource;
}

bool UiAliveLocked(NativeUi* ui);

WotbModV3Result WrapUi(
    WotbModV3Handle owner,
    void* resource,
    const WotbModUiControlGeometry* geometry,
    uint32_t visible,
    ClientHostObjectResponse* response,
    const NativeUi* definition = nullptr) {
    if (!resource || !response) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    NativeUi* ui = new (std::nothrow) NativeUi();
    if (!ui) {
        CallResourceRelease(resource);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    ui->owner = owner;
    ui->resource = resource;
    ui->geometry.struct_size = sizeof(ui->geometry);
    if (geometry) ui->geometry = *geometry;
    ui->visible = visible != 0u ? 1u : 0u;
    if (definition) {
        ui->enabled = definition->enabled;
        ui->interactable = definition->interactable;
        ui->type = definition->type;
        ui->text_alignment = definition->text_alignment;
        ui->text_wrap = definition->text_wrap;
        ui->rich_text = definition->rich_text;
        ui->color = definition->color;
        ui->background_color = definition->background_color;
        ui->opacity = definition->opacity;
        ui->font_size = definition->font_size;
        ui->dynamic = definition->dynamic;
        ui->template_backed = definition->template_backed;
        ui->owned_created = definition->owned_created;
        /* A clone inherits the provenance of the values it copied. */
        ui->mirror_flags = definition->mirror_flags;
        ui->text = definition->text;
        ui->texture = definition->texture;
        ui->font = definition->font;
        ui->source_path = definition->source_path;
        ui->source_object = definition->source_object;
    }
    if ((ui->type == WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW ||
         ui->type == WOTBMOD_V3_UI_CONTROL_LIST) &&
        g_services.resource.ui_find_by_name) {
        void* content = nullptr;
        if (CallUiFindByName(
                resource,
                "WotbModRuntimeContent",
                1u,
                &content) == WOTBMOD_OK) {
            ui->content_resource = content;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.ui_objects.push_back(ui);
    }
    response->object = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(ui));
    return WOTBMOD_V3_OK;
}

WotbModV3Result WrapUiWithCurrentState(
    WotbModV3Handle owner,
    void* resource,
    ClientHostObjectResponse* response,
    NativeUi* parent = nullptr,
    const NativeUi* definition = nullptr) {
    if (!resource || !response) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    state.geometry.struct_size = sizeof(state.geometry);
    const WotbModResult state_result =
        CallUiGetState(resource, &state);
    if (state_result != WOTBMOD_OK) {
        CallResourceRelease(resource);
        return ConvertResult(state_result);
    }
    const uint32_t visible =
        (state.flags & WOTBMOD_UI_CONTROL_VISIBLE) != 0u ? 1u : 0u;
    const WotbModV3Result wrapped = WrapUi(
        owner,
        resource,
        &state.geometry,
        visible,
        response,
        definition);
    if (wrapped != WOTBMOD_V3_OK) {
        return wrapped;
    }
    NativeUi* ui = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        ui = FindUiLocked(response->object, owner);
        if (ui) {
            ui->parent = UiAliveLocked(parent) ? parent : nullptr;
            ui->interactable =
                (state.flags & WOTBMOD_UI_CONTROL_INPUT_ENABLED) != 0u
                    ? 1u
                    : 0u;
            ui->enabled =
                (state.flags & WOTBMOD_UI_CONTROL_DISABLED) == 0u
                    ? 1u
                    : 0u;
        }
    }
    response->rect = {
        state.geometry.x,
        state.geometry.y,
        state.geometry.width,
        state.geometry.height};
    response->value_u32 =
        (visible != 0u ? 1u : 0u) |
        (ui && ui->interactable != 0u ? 2u : 0u) |
        (ui && ui->enabled != 0u ? 4u : 0u);
    /* Best effort: a backend without the identity slot leaves both empty. */
    response->name[0] = '\0';
    response->class_name[0] = '\0';
    CallUiGetIdentity(
        resource,
        response->name, static_cast<uint32_t>(sizeof(response->name)),
        response->class_name, static_cast<uint32_t>(sizeof(response->class_name)));
    return WOTBMOD_V3_OK;
}

WotbModV3Result FinalizeUiDestroy(NativeUi* ui);
void ReleaseUiLease(NativeUi* ui);

void AcquireUiLeaseLocked(NativeUi* ui) {
    if (!ui) return;
    ++ui->lifetime_leases;
    ++g_services.active_ui_operations;
    ++g_ui_operation_depth;
}

bool UiAliveLocked(NativeUi* ui) {
    return ui && !ui->destroy_requested &&
        std::find(
            g_services.ui_objects.begin(),
            g_services.ui_objects.end(),
            ui) != g_services.ui_objects.end();
}

bool UiDestroyRequested(NativeUi* ui) {
    if (!ui) return true;
    std::lock_guard<std::mutex> lock(g_services.mutex);
    return ui->destroy_requested;
}

class ScopedUiLease {
public:
    explicit ScopedUiLease(NativeUi* ui = nullptr) : ui_(ui) {}
    ~ScopedUiLease() { Reset(); }

    ScopedUiLease(const ScopedUiLease&) = delete;
    ScopedUiLease& operator=(const ScopedUiLease&) = delete;

    void Reset() {
        NativeUi* const ui = ui_;
        ui_ = nullptr;
        ReleaseUiLease(ui);
    }

private:
    NativeUi* ui_ = nullptr;
};

void RetireUiResourceLocked(void* resource) {
    if (!resource) return;
    g_services.retired_ui_resources.push_back(
        {resource, g_services.ui_frame_epoch + kUiRetirementFrames});
}

WotbModResult ReplaceUiResource(
    NativeUi* ui,
    void* replacement) {
    if (!ui || !ui->resource || !replacement) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    NativeUi* parent = nullptr;
    void* slot_parent = nullptr;
    void* parent_resource = nullptr;
    struct LeasedChild {
        NativeUi* ui = nullptr;
        void* resource = nullptr;
    };
    std::vector<LeasedChild> children;
    void* old_resource = nullptr;
    void* old_content_resource = nullptr;
    void* old_child_parent = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        parent = ui->parent;
        slot_parent = ui->slot_parent_resource;
        if (parent &&
            std::find(
                g_services.ui_objects.begin(),
                g_services.ui_objects.end(),
                parent) != g_services.ui_objects.end() &&
            !parent->destroy_requested) {
            AcquireUiLeaseLocked(parent);
            parent_resource = UiAttachmentResource(parent);
        } else {
            parent = nullptr;
            parent_resource = slot_parent;
        }
        for (NativeUi* candidate : g_services.ui_objects) {
            if (candidate && candidate->parent == ui &&
                !candidate->destroy_requested) {
                AcquireUiLeaseLocked(candidate);
                children.push_back({candidate, candidate->resource});
            }
        }
        old_resource = ui->resource;
        old_content_resource = ui->content_resource;
        old_child_parent = UiAttachmentResource(ui);
    }
    void* new_content_resource = nullptr;
    if ((ui->type == WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW ||
         ui->type == WOTBMOD_V3_UI_CONTROL_LIST) &&
        g_services.resource.ui_find_by_name) {
        CallUiFindByName(
            replacement,
            "WotbModRuntimeContent",
            1u,
            &new_content_resource);
    }
    void* const new_child_parent = new_content_resource
        ? new_content_resource
        : replacement;
    WotbModResult result = WOTBMOD_OK;
    if (parent_resource && g_services.resource.ui_remove_child) {
        result = CallUiPair(
            g_services.resource.ui_remove_child,
            parent_resource,
            old_resource);
        if (result != WOTBMOD_OK) {
            if (new_content_resource) {
                CallResourceRelease(new_content_resource);
            }
            CallResourceRelease(replacement);
            for (const LeasedChild& child : children) {
                ReleaseUiLease(child.ui);
            }
            ReleaseUiLease(parent);
            return result;
        }
    }
    size_t detached_children = 0u;
    for (; detached_children < children.size(); ++detached_children) {
        result = CallUiPair(
            g_services.resource.ui_remove_child,
            old_child_parent,
            children[detached_children].resource);
        if (result != WOTBMOD_OK) break;
    }
    if (result == WOTBMOD_OK && parent_resource) {
        result = CallUiPair(
            g_services.resource.ui_add_child,
            parent_resource,
            replacement);
    }
    size_t attached_children = 0u;
    if (result == WOTBMOD_OK) {
        for (; attached_children < children.size(); ++attached_children) {
            result = CallUiPair(
                g_services.resource.ui_add_child,
                new_child_parent,
                children[attached_children].resource);
            if (result != WOTBMOD_OK) break;
        }
    }
    if (result != WOTBMOD_OK) {
        while (attached_children > 0u) {
            --attached_children;
            CallUiPair(
                g_services.resource.ui_remove_child,
                new_child_parent,
                children[attached_children].resource);
        }
        if (parent_resource) {
            CallUiPair(
                g_services.resource.ui_remove_child,
                parent_resource,
                replacement);
            CallUiPair(
                g_services.resource.ui_add_child,
                parent_resource,
                old_resource);
        }
        for (size_t index = 0u; index < detached_children; ++index) {
            CallUiPair(
                g_services.resource.ui_add_child,
                old_child_parent,
                children[index].resource);
        }
        if (new_content_resource) {
            CallResourceRelease(new_content_resource);
        }
        CallResourceRelease(replacement);
        for (const LeasedChild& child : children) {
            ReleaseUiLease(child.ui);
        }
        ReleaseUiLease(parent);
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        ui->resource = replacement;
        ui->content_resource = new_content_resource;
        /* DAVA keeps raw UIControl pointers for pressed/drag state and for
         * frame-local hierarchy traversal. Rebuilds are held while input is
         * active, and replaced controls survive several complete idle frames
         * so none of those caches can observe a freed object. */
        if (old_content_resource &&
            old_content_resource != old_resource) {
            RetireUiResourceLocked(old_content_resource);
        }
        RetireUiResourceLocked(old_resource);
    }
    for (const LeasedChild& child : children) {
        ReleaseUiLease(child.ui);
    }
    ReleaseUiLease(parent);
    return WOTBMOD_OK;
}

WotbModResult RebuildDynamicUi(NativeUi* ui) {
    if (!ui || !ui->owned_created || ui->template_backed) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void* replacement = nullptr;
    const WotbModResult loaded = LoadDynamicUiResource(*ui, &replacement);
    if (loaded != WOTBMOD_OK || !replacement) {
        return loaded == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : loaded;
    }
    bool rebuild_disabled = false;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        rebuild_disabled =
            ui->destroy_requested || !g_services.initialized ||
            g_services.shutdown_requested || g_services.shutdown_running;
    }
    if (rebuild_disabled) {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        RetireUiResourceLocked(replacement);
        return WOTBMOD_ERROR_DISABLED;
    }
    return ReplaceUiResource(ui, replacement);
}

WotbModV3Result QueueUpdatedUiRebuild(NativeUi* ui) {
    if (!ui || !ui->owned_created || ui->template_backed ||
        !g_services.resource.load_resolved) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (std::find(
                g_services.ui_objects.begin(),
                g_services.ui_objects.end(),
                ui) == g_services.ui_objects.end()) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        ui->dynamic = true;
        ui->rebuild_pending = true;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateUiString(
    NativeUi* ui,
    const char* value,
    std::string NativeUi::*member,
    uint32_t mirror_flag) {
    if (!ui || !value || !member) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (ui->*member == value) {
        /*
         * A no-op write is still a write as far as provenance goes. Skipping
         * the flag here would make "set the text to what it already is" read
         * back as WOTBMOD_V3_E_NOT_FOUND, which is a different answer from
         * the identical call on a control whose text differed - the mirror
         * would depend on history rather than on what the API committed.
         */
        ui->mirror_flags |= mirror_flag;
        return WOTBMOD_V3_OK;
    }
    const std::string previous = ui->*member;
    ui->*member = value;
    const WotbModV3Result result = QueueUpdatedUiRebuild(ui);
    if (result != WOTBMOD_V3_OK) {
        ui->*member = previous;
        return result;
    }
    ui->mirror_flags |= mirror_flag;
    return result;
}

void UnregisterUiLocked(NativeUi* ui) {
    if (!ui) return;
    g_services.ui_objects.erase(
        std::remove(
            g_services.ui_objects.begin(),
            g_services.ui_objects.end(),
            ui),
        g_services.ui_objects.end());
    ui->destroy_requested = true;
    ui->rebuild_pending = false;
    for (NativeUi* candidate : g_services.ui_objects) {
        if (candidate && candidate->parent == ui) {
            candidate->parent = nullptr;
        }
    }
}

WotbModV3Result FinalizeUiDestroy(NativeUi* ui) {
    if (!ui) return WOTBMOD_V3_E_INVALID_HANDLE;
    NativeUi* parent = nullptr;
    void* slot_parent = nullptr;
    void* parent_resource = nullptr;
    bool was_attached = false;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        parent = ui->parent;
        slot_parent = ui->slot_parent_resource;
        was_attached = parent != nullptr || slot_parent != nullptr;
        if (parent &&
            std::find(
                g_services.ui_objects.begin(),
                g_services.ui_objects.end(),
                parent) != g_services.ui_objects.end() &&
            !parent->destroy_requested) {
            AcquireUiLeaseLocked(parent);
            parent_resource = UiAttachmentResource(parent);
        } else {
            parent = nullptr;
        }
        ui->parent = nullptr;
        ui->slot_parent_resource = nullptr;
    }

    WotbModResult result = WOTBMOD_OK;
    if (parent && g_services.resource.ui_remove_child) {
        result = CallUiPair(
            g_services.resource.ui_remove_child,
            parent_resource,
            ui->resource);
    } else if (slot_parent &&
               g_services.resource.ui_remove_child) {
        result = CallUiPair(
            g_services.resource.ui_remove_child,
            slot_parent,
            ui->resource);
    }
    if (slot_parent) {
        const WotbModResult release_parent =
            CallResourceRelease(slot_parent);
        if (result == WOTBMOD_OK) result = release_parent;
    }
    if (was_attached) {
        /* The control has just left the hierarchy, but DAVA can still hold
         * this raw pointer for pressed/drag state or for a frame-local
         * traversal that started before the destroy, and a mod unmount tears
         * a whole tree down inside one frame. Hand both objects to the same
         * retirement queue a replaced resource uses instead of freeing them
         * in the destroying frame. A control that was never attached has
         * never been reachable from the hierarchy or from input state, so it
         * needs no window. */
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (ui->content_resource &&
            ui->content_resource != ui->resource) {
            RetireUiResourceLocked(ui->content_resource);
        }
        RetireUiResourceLocked(ui->resource);
    } else {
        if (ui->content_resource) {
            const WotbModResult release_content =
                CallResourceRelease(ui->content_resource);
            if (result == WOTBMOD_OK) result = release_content;
        }
        const WotbModResult release_ui =
            CallResourceRelease(ui->resource);
        if (result == WOTBMOD_OK) result = release_ui;
    }
    ui->resource = nullptr;
    ui->content_resource = nullptr;
    ui->magic = 0u;
    delete ui;
    ReleaseUiLease(parent);
    return ConvertResult(result);
}

void ReleaseUiLease(NativeUi* ui) {
    if (!ui) return;
    bool finalize_destroy = false;
    bool run_deferred_shutdown = false;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (ui->lifetime_leases == 0u) return;
        --ui->lifetime_leases;
        finalize_destroy =
            ui->destroy_requested && ui->lifetime_leases == 0u;
        if (!finalize_destroy) {
            if (g_services.active_ui_operations > 0u) {
                --g_services.active_ui_operations;
            }
            if (g_ui_operation_depth > 0u) {
                --g_ui_operation_depth;
            }
            run_deferred_shutdown =
                g_services.active_ui_operations == 0u &&
                g_services.active_service_invocations == 0u &&
                g_services.shutdown_requested &&
                !g_services.shutdown_running;
            g_services.operations_finished.notify_all();
        }
    }
    if (finalize_destroy) {
        FinalizeUiDestroy(ui);
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (g_services.active_ui_operations > 0u) {
            --g_services.active_ui_operations;
        }
        if (g_ui_operation_depth > 0u) {
            --g_ui_operation_depth;
        }
        run_deferred_shutdown =
            g_services.active_ui_operations == 0u &&
            g_services.active_service_invocations == 0u &&
            g_services.shutdown_requested &&
            !g_services.shutdown_running;
        g_services.operations_finished.notify_all();
    }
    if (run_deferred_shutdown) {
        ShutdownV3NativeClientServices();
    }
}

WotbModV3Result DestroyUi(
    WotbModV3Handle owner,
    uint64_t token) {
    NativeUi* ui = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        ui = FindUiLocked(token, owner);
        if (!ui) return WOTBMOD_V3_E_INVALID_HANDLE;
        UnregisterUiLocked(ui);
        if (ui->lifetime_leases != 0u) {
            return WOTBMOD_V3_OK;
        }
        ++g_services.active_ui_operations;
        ++g_ui_operation_depth;
    }
    const WotbModV3Result result = FinalizeUiDestroy(ui);
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        if (g_services.active_ui_operations > 0u) {
            --g_services.active_ui_operations;
        }
        if (g_ui_operation_depth > 0u) {
            --g_ui_operation_depth;
        }
        g_services.operations_finished.notify_all();
    }
    return result;
}

WotbModV3Result InvokeUi(
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size) {
    const ClientHostObjectRequest* object_request = nullptr;
    ClientHostObjectResponse* object_response = nullptr;

    if (std::strcmp(operation, "ui_get_active_screen") == 0) {
        if (!ValidObjectResponse(
                response, response_size, &object_response) ||
            !g_services.resource.ui_get_active_screen ||
            !g_services.resource.release) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        void* resource = nullptr;
        const WotbModResult result = CallResourceCreate(
            g_services.resource.ui_get_active_screen,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_NOT_FOUND
                    : result);
        }
        return WrapUiWithCurrentState(
            mod,
            resource,
            object_response);
    }

    if (!ValidObjectRequest(
            request, request_size, &object_request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (response &&
        !ValidObjectResponse(
            response, response_size, &object_response)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    if (std::strcmp(operation, "ui_control_create") == 0) {
        if (!object_response || !object_request->payload ||
            object_request->payload_size <
                sizeof(WotbModV3UiControlDescriptor) ||
            !g_services.resource.release) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        const WotbModV3UiControlDescriptor* descriptor =
            static_cast<const WotbModV3UiControlDescriptor*>(
                object_request->payload);
        const WotbModUiControlGeometry geometry =
            ToLegacyGeometry(descriptor->geometry);
        NativeUi definition;
        definition.owner = mod;
        definition.geometry = geometry;
        definition.visible = descriptor->visible != 0u ? 1u : 0u;
        definition.type = descriptor->type;
        definition.owned_created = true;
        definition.text = descriptor->text ? descriptor->text : "";
        definition.texture = descriptor->texture_uri
            ? descriptor->texture_uri
            : "";
        /*
         * A supplied descriptor field is a committed write, even when it is
         * the empty string; a NULL field is the caller declining to write and
         * must stay readable as "never written".
         */
        if (descriptor->text) {
            definition.mirror_flags |= WOTBMOD_V3_UI_READ_TEXT_SET;
        }
        if (descriptor->texture_uri) {
            definition.mirror_flags |= WOTBMOD_V3_UI_READ_TEXTURE_SET;
        }
        if (definition.type == WOTBMOD_V3_UI_CONTROL_TEXT) {
            definition.background_color.a = 0.0f;
        } else if (definition.type == WOTBMOD_V3_UI_CONTROL_BUTTON ||
                   definition.type == WOTBMOD_V3_UI_CONTROL_CHECKBOX ||
                   definition.type == WOTBMOD_V3_UI_CONTROL_SLIDER ||
                   definition.type == WOTBMOD_V3_UI_CONTROL_DROPDOWN ||
                   definition.type == WOTBMOD_V3_UI_CONTROL_TABS) {
            definition.background_color = {0.93f, 0.43f, 0.06f, 1.0f};
        }
        void* resource = nullptr;
        const bool load_template =
            object_request->secondary_name &&
            object_request->secondary_name[0];
        definition.template_backed = load_template;
        definition.dynamic = !load_template &&
            descriptor->type != WOTBMOD_V3_UI_CONTROL_CONTAINER;
        if (load_template) {
            definition.source_path = object_request->secondary_name;
            definition.source_object = object_request->name
                ? object_request->name
                : "";
        }
        WotbModResult result = WOTBMOD_ERROR_PLATFORM;
        if (load_template) {
            result = CallUiLoadResolved(
                object_request->secondary_name,
                object_request->name,
                &resource);
        } else if (definition.dynamic) {
            if (!g_services.resource.load_resolved) {
                return WOTBMOD_V3_E_NOT_SUPPORTED;
            }
            result = LoadDynamicUiResource(definition, &resource);
        } else if (g_services.resource.ui_create) {
            result = CallUiCreate(&geometry, &resource);
        } else {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        const bool has_geometry =
            descriptor->geometry.x != 0.0f ||
            descriptor->geometry.y != 0.0f ||
            descriptor->geometry.width != 0.0f ||
            descriptor->geometry.height != 0.0f;
        if (load_template && has_geometry) {
            result = CallUiSetGeometry(resource, &geometry);
            if (result != WOTBMOD_OK) {
                CallResourceRelease(resource);
                return ConvertResult(result);
            }
        }
        result = CallUiSetVisible(
            resource, descriptor->visible);
        if (result != WOTBMOD_OK) {
            CallResourceRelease(resource);
            return ConvertResult(result);
        }
        if (load_template) {
            return WrapUiWithCurrentState(
                mod,
                resource,
                object_response,
                nullptr,
                &definition);
        }
        return WrapUi(
            mod,
            resource,
            &geometry,
            descriptor->visible,
            object_response,
            &definition);
    }

    if (std::strcmp(operation, "ui_control_clone") == 0) {
        if (!object_response || !g_services.resource.release) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        NativeUi* source = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            source = FindUiLocked(object_request->object, mod);
            if (source) AcquireUiLeaseLocked(source);
        }
        if (!source) return WOTBMOD_V3_E_INVALID_HANDLE;
        ScopedUiLease source_lease(source);
        void* resource = nullptr;
        WotbModResult result = WOTBMOD_ERROR_PLATFORM;
        if (source->dynamic) {
            result = LoadDynamicUiResource(*source, &resource);
        } else if (source->template_backed &&
                   !source->source_path.empty() &&
                   !source->source_object.empty()) {
            result = CallUiLoadResolved(
                source->source_path.c_str(),
                source->source_object.c_str(),
                &resource);
        } else if (source->owned_created &&
                   g_services.resource.ui_create) {
            result = CallUiCreate(&source->geometry, &resource);
        } else {
            /*
             * The backend clone callback retains the same DAVA object. A
             * second public handle to that pointer is not an independent
             * control, so game-owned controls without a reproducible source
             * are truthfully not cloneable.
             */
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        if (UiDestroyRequested(source)) {
            CallResourceRelease(resource);
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        return WrapUi(
            mod,
            resource,
            &source->geometry,
            source->visible,
            object_response,
            source);
    }

    if (std::strcmp(operation, "ui_control_destroy") == 0) {
        return DestroyUi(mod, object_request->object);
    }

    NativeUi* ui = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        ui = FindUiLocked(object_request->object, mod);
        if (ui) AcquireUiLeaseLocked(ui);
    }
    if (!ui) return WOTBMOD_V3_E_INVALID_HANDLE;
    ScopedUiLease ui_lease(ui);

    if (std::strcmp(operation, "ui_control_set_text") == 0) {
        return UpdateUiString(
            ui,
            object_request->name,
            &NativeUi::text,
            WOTBMOD_V3_UI_READ_TEXT_SET);
    }
    if (std::strcmp(operation, "ui_control_set_texture") == 0) {
        return UpdateUiString(
            ui,
            object_request->name,
            &NativeUi::texture,
            WOTBMOD_V3_UI_READ_TEXTURE_SET);
    }
    if (std::strcmp(operation, "ui_control_set_font") == 0) {
        return UpdateUiString(
            ui,
            object_request->name,
            &NativeUi::font,
            WOTBMOD_V3_UI_READ_FONT_SET);
    }
    /*
     * 2026-09-05: a game-owned control has no YAML to rebuild; its
     * UIControlBackground colour is read and written in place through the
     * DAVA backend. This is what tints the stock reticle and fades the stock
     * minimap; the mod-owned path below keeps rebuilding from YAML.
     */
    if (!ui->owned_created &&
        (std::strcmp(operation, "ui_control_get_background_color") == 0 ||
         std::strcmp(operation, "ui_control_set_background_color") == 0)) {
        if (std::strcmp(operation, "ui_control_get_background_color") == 0) {
            if (!object_response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
            float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const WotbModResult result =
                CallUiGetBackgroundColor(ui->resource, rgba);
            if (result != WOTBMOD_OK) return ConvertResult(result);
            object_response->vector = {rgba[0], rgba[1], rgba[2], rgba[3]};
            return WOTBMOD_V3_OK;
        }
        const float rgba[4] = {
            object_request->vector.x,
            object_request->vector.y,
            object_request->vector.z,
            object_request->vector.w};
        for (float channel : rgba) {
            if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) {
                return WOTBMOD_V3_E_INVALID_ARGUMENT;
            }
        }
        return ConvertResult(CallUiSetBackgroundColor(ui->resource, rgba));
    }
    if (std::strcmp(operation, "ui_control_set_color") == 0 ||
        std::strcmp(
            operation,
            "ui_control_set_background_color") == 0) {
        const WotbModV3Color value = {
            object_request->vector.x,
            object_request->vector.y,
            object_request->vector.z,
            object_request->vector.w};
        if (!std::isfinite(value.r) || !std::isfinite(value.g) ||
            !std::isfinite(value.b) || !std::isfinite(value.a)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const bool foreground =
            std::strcmp(operation, "ui_control_set_color") == 0;
        const uint32_t mirror_flag = foreground
            ? static_cast<uint32_t>(WOTBMOD_V3_UI_READ_COLOR_SET)
            : static_cast<uint32_t>(
                  WOTBMOD_V3_UI_READ_BACKGROUND_COLOR_SET);
        WotbModV3Color* target =
            foreground ? &ui->color : &ui->background_color;
        if (target->r == value.r && target->g == value.g &&
            target->b == value.b && target->a == value.a) {
            ui->mirror_flags |= mirror_flag;
            return WOTBMOD_V3_OK;
        }
        const WotbModV3Color previous = *target;
        *target = value;
        const WotbModV3Result result = QueueUpdatedUiRebuild(ui);
        if (result != WOTBMOD_V3_OK) {
            *target = previous;
            return result;
        }
        ui->mirror_flags |= mirror_flag;
        return result;
    }
    if (std::strcmp(operation, "ui_control_set_opacity") == 0 ||
        std::strcmp(operation, "ui_control_set_font_size") == 0) {
        const double scalar = object_request->scalar0;
        if (!std::isfinite(scalar) ||
            (std::strcmp(operation, "ui_control_set_opacity") == 0 &&
             (scalar < 0.0 || scalar > 1.0)) ||
            (std::strcmp(operation, "ui_control_set_font_size") == 0 &&
             (scalar <= 0.0 || scalar > 512.0))) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const float value = static_cast<float>(scalar);
        const bool is_opacity =
            std::strcmp(operation, "ui_control_set_opacity") == 0;
        const uint32_t mirror_flag = is_opacity
            ? static_cast<uint32_t>(WOTBMOD_V3_UI_READ_OPACITY_SET)
            : static_cast<uint32_t>(WOTBMOD_V3_UI_READ_FONT_SIZE_SET);
        float* target = is_opacity ? &ui->opacity : &ui->font_size;
        if (*target == value) {
            ui->mirror_flags |= mirror_flag;
            return WOTBMOD_V3_OK;
        }
        const float previous = *target;
        *target = value;
        const WotbModV3Result result = QueueUpdatedUiRebuild(ui);
        if (result != WOTBMOD_V3_OK) {
            *target = previous;
            return result;
        }
        ui->mirror_flags |= mirror_flag;
        return result;
    }
    if (std::strcmp(operation, "ui_control_set_text_alignment") == 0) {
        if (object_request->selector > WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        if (ui->text_alignment == object_request->selector) {
            ui->mirror_flags |= WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;
            return WOTBMOD_V3_OK;
        }
        const uint32_t previous = ui->text_alignment;
        ui->text_alignment = object_request->selector;
        const WotbModV3Result result = QueueUpdatedUiRebuild(ui);
        if (result != WOTBMOD_V3_OK) {
            ui->text_alignment = previous;
            return result;
        }
        ui->mirror_flags |= WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;
        return result;
    }
    if (std::strcmp(operation, "ui_control_set_text_wrap") == 0 ||
        std::strcmp(operation, "ui_control_set_rich_text") == 0) {
        if (object_request->flags > 1u) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        uint32_t* target =
            std::strcmp(operation, "ui_control_set_text_wrap") == 0
                ? &ui->text_wrap
                : &ui->rich_text;
        if (*target == object_request->flags) {
            ui->mirror_flags |= WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;
            return WOTBMOD_V3_OK;
        }
        const uint32_t previous = *target;
        *target = object_request->flags;
        const WotbModV3Result result = QueueUpdatedUiRebuild(ui);
        if (result != WOTBMOD_V3_OK) {
            *target = previous;
            return result;
        }
        ui->mirror_flags |= WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;
        return result;
    }

    if (std::strcmp(operation, "ui_control_find_by_name") == 0) {
        if (!object_response || !object_request->name ||
            !object_request->name[0]) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        void* resource = nullptr;
        const WotbModResult result = CallUiFindByName(
            ui->resource,
            object_request->name,
            object_request->flags,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_NOT_FOUND
                    : result);
        }
        const WotbModV3Result wrapped = WrapUiWithCurrentState(
            mod,
            resource,
            object_response,
            object_request->flags == 0u ? ui : nullptr);
        if (wrapped == WOTBMOD_V3_OK &&
            object_request->flags == 0u &&
            UiDestroyRequested(ui)) {
            DestroyUi(mod, object_response->object);
            object_response->object = 0u;
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        return wrapped;
    }
    if (std::strcmp(operation, "ui_control_get_parent") == 0) {
        if (!object_response) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        void* resource = nullptr;
        const WotbModResult result = CallUiGetRelated(
            g_services.resource.ui_get_parent,
            ui->resource,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_NOT_FOUND
                    : result);
        }
        return WrapUiWithCurrentState(
            mod,
            resource,
            object_response);
    }
    if (std::strcmp(operation, "ui_control_get_child_count") == 0) {
        if (!object_response) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        uint32_t count = 0u;
        const WotbModResult result =
            CallUiGetCount(ui->resource, &count);
        if (result != WOTBMOD_OK) {
            return ConvertResult(result);
        }
        object_response->value_u32 = count;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_get_child_at") == 0) {
        if (!object_response) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        void* resource = nullptr;
        const WotbModResult result = CallUiGetAt(
            ui->resource,
            object_request->selector,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_NOT_FOUND
                    : result);
        }
        const WotbModV3Result wrapped = WrapUiWithCurrentState(
            mod,
            resource,
            object_response,
            ui);
        if (wrapped == WOTBMOD_V3_OK && UiDestroyRequested(ui)) {
            DestroyUi(mod, object_response->object);
            object_response->object = 0u;
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        return wrapped;
    }

    if (std::strcmp(operation, "ui_control_set_geometry") == 0) {
        WotbModUiControlGeometry geometry = {};
        geometry.struct_size = sizeof(geometry);
        geometry.x = object_request->vector.x;
        geometry.y = object_request->vector.y;
        geometry.width = object_request->vector.z;
        geometry.height = object_request->vector.w;
        const WotbModResult result =
            CallUiSetGeometry(ui->resource, &geometry);
        if (result == WOTBMOD_OK) ui->geometry = geometry;
        return ConvertResult(result);
    }
    if (std::strcmp(operation, "ui_control_set_visible") == 0) {
        const WotbModResult result = CallUiSetVisible(
            ui->resource, object_request->flags);
        if (result == WOTBMOD_OK) {
            ui->visible = object_request->flags != 0u ? 1u : 0u;
        }
        return ConvertResult(result);
    }
    if (std::strcmp(operation, "ui_control_set_enabled") == 0) {
        const WotbModResult result = CallUiSetFlag(
            g_services.resource.ui_set_disabled,
            ui->resource,
            object_request->flags == 0u ? 1u : 0u);
        if (result == WOTBMOD_OK) {
            ui->enabled = object_request->flags != 0u ? 1u : 0u;
        }
        return ConvertResult(result);
    }
    if (std::strcmp(
            operation, "ui_control_set_interactable") == 0) {
        const WotbModResult result = CallUiSetFlag(
            g_services.resource.ui_set_input_enabled,
            ui->resource,
            object_request->flags);
        if (result == WOTBMOD_OK) {
            ui->interactable =
                object_request->flags != 0u ? 1u : 0u;
        }
        return ConvertResult(result);
    }
    if (std::strcmp(operation, "ui_control_get_state") == 0) {
        if (!object_response) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        WotbModUiControlState state = {};
        state.struct_size = sizeof(state);
        state.geometry.struct_size = sizeof(state.geometry);
        const WotbModResult result =
            CallUiGetState(ui->resource, &state);
        if (result != WOTBMOD_OK) return ConvertResult(result);
        ui->geometry = state.geometry;
        ui->visible =
            (state.flags & WOTBMOD_UI_CONTROL_VISIBLE) != 0u
                ? 1u
                : 0u;
        ui->interactable =
            (state.flags &
             WOTBMOD_UI_CONTROL_INPUT_ENABLED) != 0u
                ? 1u
                : 0u;
        ui->enabled =
            (state.flags & WOTBMOD_UI_CONTROL_DISABLED) == 0u
                ? 1u
                : 0u;
        object_response->rect = {
            state.geometry.x,
            state.geometry.y,
            state.geometry.width,
            state.geometry.height};
        object_response->value_u32 =
            (ui->visible != 0u ? 1u : 0u) |
            (ui->interactable != 0u ? 2u : 0u) |
            (ui->enabled != 0u ? 4u : 0u);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_parent") == 0) {
        NativeUi* parent = nullptr;
        NativeUi* previous_parent = nullptr;
        void* previous_slot = nullptr;
        void* parent_resource = nullptr;
        void* previous_resource = nullptr;
        void* ui_resource = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            if (object_request->related_object != 0u) {
                parent = FindUiLocked(
                    object_request->related_object, mod);
                if (!parent) return WOTBMOD_V3_E_INVALID_HANDLE;
                if (parent == ui) return WOTBMOD_V3_E_CONFLICT;
                AcquireUiLeaseLocked(parent);
                parent_resource = UiAttachmentResource(parent);
            }
            previous_parent = ui->parent;
            previous_slot = ui->slot_parent_resource;
            if (UiAliveLocked(previous_parent)) {
                AcquireUiLeaseLocked(previous_parent);
                previous_resource =
                    UiAttachmentResource(previous_parent);
            } else {
                previous_parent = nullptr;
                previous_resource = previous_slot;
            }
            ui_resource = ui->resource;
        }
        ScopedUiLease parent_lease(parent);
        ScopedUiLease previous_parent_lease(previous_parent);

        /*
         * ui_get_active_screen returns a fresh retained resource wrapper on
         * every probe. Compare the wrapped native object, then ask DAVA for
         * the child's real parent. A still-attached child only transfers its
         * managed wrapper. If the game detached the child, AddControl restores
         * the attachment. If the game only appended HUD controls above it,
         * DAVA's same-parent reorder moves it to the tail/front. Avoiding a
         * separate RemoveControl keeps the Lua panel from disappearing for a
         * rendered frame.
         */
        const void* const parent_identity =
            CallResourceIdentity(parent_resource);
        const void* const previous_identity =
            CallResourceIdentity(previous_resource);
        if (parent && previous_parent && parent_identity &&
            parent_identity == previous_identity) {
            bool attached_to_parent = true;
            if (g_services.resource.ui_get_parent &&
                g_services.resource.release) {
                void* actual_parent_resource = nullptr;
                const WotbModResult actual_result = CallUiGetRelated(
                    g_services.resource.ui_get_parent,
                    ui_resource,
                    &actual_parent_resource);
                if (actual_result == WOTBMOD_ERROR_NOT_FOUND) {
                    attached_to_parent = false;
                } else if (actual_result != WOTBMOD_OK ||
                           !actual_parent_resource) {
                    return ConvertResult(
                        actual_result == WOTBMOD_OK
                            ? WOTBMOD_ERROR_NOT_FOUND
                            : actual_result);
                } else {
                    attached_to_parent =
                        CallResourceIdentity(actual_parent_resource) ==
                        parent_identity;
                    const WotbModResult released =
                        CallResourceRelease(actual_parent_resource);
                    if (released != WOTBMOD_OK) {
                        return ConvertResult(released);
                    }
                }
            }
            bool should_add = !attached_to_parent;
            if (attached_to_parent &&
                g_services.resource.ui_get_child_count &&
                g_services.resource.ui_get_child_at &&
                g_services.resource.release) {
                uint32_t child_count = 0u;
                const WotbModResult count_result = CallUiGetCount(
                    parent_resource,
                    &child_count);
                if (count_result != WOTBMOD_OK) {
                    return ConvertResult(count_result);
                }
                if (child_count == 0u) {
                    should_add = true;
                } else {
                    void* last_child_resource = nullptr;
                    const WotbModResult child_result = CallUiGetAt(
                        parent_resource,
                        child_count - 1u,
                        &last_child_resource);
                    if (child_result != WOTBMOD_OK ||
                        !last_child_resource) {
                        return ConvertResult(
                            child_result == WOTBMOD_OK
                                ? WOTBMOD_ERROR_NOT_FOUND
                                : child_result);
                    }
                    should_add =
                        CallResourceIdentity(last_child_resource) !=
                        CallResourceIdentity(ui_resource);
                    const WotbModResult released =
                        CallResourceRelease(last_child_resource);
                    if (released != WOTBMOD_OK) {
                        return ConvertResult(released);
                    }
                }
            }
            if (should_add && attached_to_parent &&
                g_services.resource_bring_ui_to_front) {
                const WotbModResult raised =
                    CallResourceBringUiToFront(ui_resource);
                if (raised != WOTBMOD_OK) {
                    return ConvertResult(raised);
                }
                should_add = false;
            }
            if (should_add) {
                if (UiDestroyRequested(ui) ||
                    UiDestroyRequested(parent)) {
                    return WOTBMOD_V3_E_OBJECT_DESTROYED;
                }
                const WotbModResult added = CallUiPair(
                    g_services.resource.ui_add_child,
                    parent_resource,
                    ui_resource);
                if (added != WOTBMOD_OK) {
                    return UiDestroyRequested(ui)
                        ? WOTBMOD_V3_E_OBJECT_DESTROYED
                        : ConvertResult(added);
                }
                if (UiDestroyRequested(ui) ||
                    UiDestroyRequested(parent)) {
                    CallUiPair(
                        g_services.resource.ui_remove_child,
                        parent_resource,
                        ui_resource);
                    return WOTBMOD_V3_E_OBJECT_DESTROYED;
                }
            }
            bool committed = false;
            {
                std::lock_guard<std::mutex> lock(g_services.mutex);
                if (UiAliveLocked(ui) && UiAliveLocked(parent)) {
                    ui->parent = parent;
                    ui->slot_parent_resource = nullptr;
                    committed = true;
                }
            }
            if (!committed && !attached_to_parent) {
                CallUiPair(
                    g_services.resource.ui_remove_child,
                    parent_resource,
                    ui_resource);
            }
            return committed
                ? WOTBMOD_V3_OK
                : WOTBMOD_V3_E_OBJECT_DESTROYED;
        }

        const auto previous_parent_alive = [&]() {
            if (!previous_parent) return previous_slot != nullptr;
            std::lock_guard<std::mutex> lock(g_services.mutex);
            return UiAliveLocked(previous_parent);
        };
        const auto rollback_previous = [&]() {
            if (!previous_resource || !previous_parent_alive()) return;
            CallUiPair(
                g_services.resource.ui_add_child,
                previous_resource,
                ui_resource);
        };
        const auto finish_destroyed_ui = [&]() {
            {
                std::lock_guard<std::mutex> lock(g_services.mutex);
                if (ui->destroy_requested) ui->parent = nullptr;
            }
            ui_lease.Reset();
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        };

        if (previous_resource &&
            g_services.resource.ui_remove_child) {
            const WotbModResult removed = CallUiPair(
                g_services.resource.ui_remove_child,
                previous_resource,
                ui_resource);
            if (removed != WOTBMOD_OK) {
                if (UiDestroyRequested(ui)) {
                    return finish_destroyed_ui();
                }
                return ConvertResult(removed);
            }
        }
        if (UiDestroyRequested(ui)) {
            return finish_destroyed_ui();
        }
        if (parent && UiDestroyRequested(parent)) {
            rollback_previous();
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        if (parent) {
            const WotbModResult added = CallUiPair(
                g_services.resource.ui_add_child,
                parent_resource,
                ui_resource);
            if (added != WOTBMOD_OK) {
                if (UiDestroyRequested(ui)) {
                    return finish_destroyed_ui();
                }
                rollback_previous();
                return ConvertResult(added);
            }
        }
        const bool ui_destroyed = UiDestroyRequested(ui);
        const bool parent_destroyed =
            parent && UiDestroyRequested(parent);
        if (ui_destroyed || parent_destroyed) {
            if (parent) {
                CallUiPair(
                    g_services.resource.ui_remove_child,
                    parent_resource,
                    ui_resource);
            }
            if (!ui_destroyed) rollback_previous();
            if (ui_destroyed) return finish_destroyed_ui();
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }

        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            if (UiAliveLocked(ui) &&
                (!parent || UiAliveLocked(parent))) {
                ui->parent = parent;
                ui->slot_parent_resource = nullptr;
                committed = true;
            }
        }
        if (!committed) {
            if (parent) {
                CallUiPair(
                    g_services.resource.ui_remove_child,
                    parent_resource,
                    ui_resource);
            }
            if (!UiDestroyRequested(ui)) rollback_previous();
            if (UiDestroyRequested(ui)) {
                return finish_destroyed_ui();
            }
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        if (previous_slot) {
            CallResourceRelease(previous_slot);
        }
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_slot_attach") == 0) {
        /*
         * A semantic slot must resolve to that exact extension point.
         * The active screen is a different object and must not be used as a
         * success-producing fallback.
         */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (std::strcmp(operation, "ui_slot_detach") == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Transform IdentityTransform() {
    WotbModV3Transform transform = {};
    transform.struct_size = sizeof(transform);
    transform.api_version = WOTBMOD_V3_ABI_VERSION;
    transform.rotation.w = 1.0f;
    transform.scale = {1.0f, 1.0f, 1.0f};
    return transform;
}

WotbModV3Vec4 NormalizeQuat(const WotbModV3Vec4& value);

bool MatrixToTransform(
    const WotbModV3Matrix4& matrix,
    WotbModV3Transform* output) {
    if (!output) return false;
    for (float value : matrix.values) {
        if (!std::isfinite(value)) return false;
    }
    const float* m = matrix.values;
    if (std::fabs(m[3]) > 0.0001f ||
        std::fabs(m[7]) > 0.0001f ||
        std::fabs(m[11]) > 0.0001f ||
        std::fabs(m[15] - 1.0f) > 0.0001f) {
        return false;
    }
    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] +
                         m[2] * m[2]);
    const float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] +
                               m[6] * m[6]);
    const float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] +
                               m[10] * m[10]);
    if (!(sx > 0.000001f) || !(sy > 0.000001f) ||
        !(sz > 0.000001f)) {
        return false;
    }

    float r00 = m[0] / sx;
    float r01 = m[1] / sx;
    float r02 = m[2] / sx;
    const float r10 = m[4] / sy;
    const float r11 = m[5] / sy;
    const float r12 = m[6] / sy;
    const float r20 = m[8] / sz;
    const float r21 = m[9] / sz;
    const float r22 = m[10] / sz;
    const float determinant =
        r00 * (r11 * r22 - r12 * r21) -
        r01 * (r10 * r22 - r12 * r20) +
        r02 * (r10 * r21 - r11 * r20);
    if (determinant < 0.0f) {
        sx = -sx;
        r00 = -r00;
        r01 = -r01;
        r02 = -r02;
    }

    WotbModV3Vec4 rotation = {};
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        rotation.w = 0.25f * s;
        rotation.x = (r21 - r12) / s;
        rotation.y = (r02 - r20) / s;
        rotation.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        rotation.w = (r21 - r12) / s;
        rotation.x = 0.25f * s;
        rotation.y = (r01 + r10) / s;
        rotation.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        rotation.w = (r02 - r20) / s;
        rotation.x = (r01 + r10) / s;
        rotation.y = 0.25f * s;
        rotation.z = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        rotation.w = (r10 - r01) / s;
        rotation.x = (r02 + r20) / s;
        rotation.y = (r12 + r21) / s;
        rotation.z = 0.25f * s;
    }

    *output = IdentityTransform();
    output->position = {m[12], m[13], m[14]};
    output->rotation = NormalizeQuat(rotation);
    output->scale = {sx, sy, sz};
    return true;
}

WotbModSceneTransform ToLegacyTransform(
    const WotbModV3Transform& transform) {
    WotbModSceneTransform output = {};
    output.struct_size = sizeof(output);
    output.position_x = transform.position.x;
    output.position_y = transform.position.y;
    output.position_z = transform.position.z;
    output.rotation_x = transform.rotation.x;
    output.rotation_y = transform.rotation.y;
    output.rotation_z = transform.rotation.z;
    output.rotation_w = transform.rotation.w;
    output.scale_x = transform.scale.x;
    output.scale_y = transform.scale.y;
    output.scale_z = transform.scale.z;
    return output;
}

WotbModV3Vec4 MultiplyQuat(
    const WotbModV3Vec4& left,
    const WotbModV3Vec4& right) {
    return {
        left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z};
}

WotbModV3Vec4 NormalizeQuat(const WotbModV3Vec4& value) {
    const double length = std::sqrt(
        static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w);
    if (!(length > 0.000001)) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inverse = static_cast<float>(1.0 / length);
    return {
        value.x * inverse,
        value.y * inverse,
        value.z * inverse,
        value.w * inverse};
}

WotbModV3Vec3 RotateVector(
    const WotbModV3Vec4& rotation,
    const WotbModV3Vec3& value) {
    const WotbModV3Vec4 normalized = NormalizeQuat(rotation);
    const WotbModV3Vec4 vector = {
        value.x, value.y, value.z, 0.0f};
    const WotbModV3Vec4 inverse = {
        -normalized.x,
        -normalized.y,
        -normalized.z,
        normalized.w};
    const WotbModV3Vec4 result =
        MultiplyQuat(MultiplyQuat(normalized, vector), inverse);
    return {result.x, result.y, result.z};
}

WotbModV3Transform ComposeTransform(
    const WotbModV3Transform& parent,
    const WotbModV3Transform& local) {
    WotbModV3Transform output = IdentityTransform();
    const WotbModV3Vec3 scaled = {
        local.position.x * parent.scale.x,
        local.position.y * parent.scale.y,
        local.position.z * parent.scale.z};
    const WotbModV3Vec3 rotated =
        RotateVector(parent.rotation, scaled);
    output.position = {
        parent.position.x + rotated.x,
        parent.position.y + rotated.y,
        parent.position.z + rotated.z};
    output.rotation = NormalizeQuat(
        MultiplyQuat(parent.rotation, local.rotation));
    output.scale = {
        parent.scale.x * local.scale.x,
        parent.scale.y * local.scale.y,
        parent.scale.z * local.scale.z};
    return output;
}

WotbModV3Transform RelativeTransform(
    const WotbModV3Transform& parent,
    const WotbModV3Transform& world) {
    WotbModV3Transform output = IdentityTransform();
    const WotbModV3Vec4 parent_rotation =
        NormalizeQuat(parent.rotation);
    const WotbModV3Vec4 inverse_rotation = {
        -parent_rotation.x,
        -parent_rotation.y,
        -parent_rotation.z,
        parent_rotation.w};
    const WotbModV3Vec3 delta = {
        world.position.x - parent.position.x,
        world.position.y - parent.position.y,
        world.position.z - parent.position.z};
    const WotbModV3Vec3 unrotated =
        RotateVector(inverse_rotation, delta);
    output.position = {
        parent.scale.x != 0.0f
            ? unrotated.x / parent.scale.x : 0.0f,
        parent.scale.y != 0.0f
            ? unrotated.y / parent.scale.y : 0.0f,
        parent.scale.z != 0.0f
            ? unrotated.z / parent.scale.z : 0.0f};
    output.rotation = NormalizeQuat(
        MultiplyQuat(inverse_rotation, world.rotation));
    output.scale = {
        parent.scale.x != 0.0f
            ? world.scale.x / parent.scale.x : world.scale.x,
        parent.scale.y != 0.0f
            ? world.scale.y / parent.scale.y : world.scale.y,
        parent.scale.z != 0.0f
            ? world.scale.z / parent.scale.z : world.scale.z};
    return output;
}

bool WorldTransformLocked(
    NativeScene* scene,
    WotbModV3Transform* output) {
    if (!scene || !output) return false;
    NativeScene* lineage[128] = {};
    uint32_t count = 0u;
    for (NativeScene* current = scene;
         current && count < 128u;
         current = current->parent) {
        lineage[count++] = current;
    }
    if (count == 128u && lineage[count - 1u]->parent) {
        return false;
    }
    WotbModV3Transform world = lineage[count - 1u]->local;
    while (count > 1u) {
        --count;
        world = ComposeTransform(world, lineage[count - 1u]->local);
    }
    *output = world;
    return true;
}

WotbModV3Result WrapScene(
    WotbModV3Handle owner,
    void* resource,
    const char* resolved_path,
    const WotbModV3Transform* transform,
    ClientHostObjectResponse* response) {
    if (!resource || !response) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    NativeScene* scene = new (std::nothrow) NativeScene();
    if (!scene) {
        CallResourceRelease(resource);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    scene->owner = owner;
    scene->resource = resource;
    scene->local = transform ? *transform : IdentityTransform();
    if (resolved_path && resolved_path[0] &&
        !CopyText(
            scene->resolved_path,
            sizeof(scene->resolved_path),
            resolved_path)) {
        CallResourceRelease(resource);
        delete scene;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.scenes.push_back(scene);
    }
    response->object = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(scene));
    return WOTBMOD_V3_OK;
}

WotbModV3Result DestroyScene(
    WotbModV3Handle owner,
    uint64_t token) {
    NativeScene* scene = nullptr;
    std::vector<TransientSceneAttachment> transient;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        scene = FindSceneLocked(token, owner);
        if (!scene) return WOTBMOD_V3_E_INVALID_HANDLE;
        g_services.scenes.erase(
            std::remove(
                g_services.scenes.begin(),
                g_services.scenes.end(),
                scene),
            g_services.scenes.end());
        for (NativeScene* candidate : g_services.scenes) {
            if (candidate && candidate->parent == scene) {
                candidate->parent = nullptr;
            }
        }
        for (auto it = g_services.transient_scenes.begin();
             it != g_services.transient_scenes.end();) {
            if (it->scene == scene) {
                transient.push_back(*it);
                it = g_services.transient_scenes.erase(it);
            } else {
                ++it;
            }
        }
        scene->transient_attached = false;
    }
    for (const auto& attachment : transient) {
        if (attachment.active_scene_resource &&
            attachment.child_resource) {
            CallScenePair(
                g_services.resource.scene_remove_child,
                attachment.active_scene_resource,
                attachment.child_resource);
        }
        if (attachment.active_scene_resource) {
            CallResourceRelease(attachment.active_scene_resource);
        }
    }
    if (scene->parent && g_services.resource.scene_remove_child) {
        CallScenePair(
            g_services.resource.scene_remove_child,
            scene->parent->resource,
            scene->resource);
    }
    const WotbModResult released =
        CallResourceRelease(scene->resource);
    scene->resource = nullptr;
    scene->magic = 0u;
    delete scene;
    return ConvertResult(released);
}

WotbModV3Result InvokeScene(
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size) {
    const ClientHostObjectRequest* object_request = nullptr;
    ClientHostObjectResponse* object_response = nullptr;

    if (std::strcmp(operation, "scene_entity_create") == 0) {
        if (!ValidObjectRequest(
                request, request_size, &object_request) ||
            !ValidObjectResponse(
                response, response_size, &object_response) ||
            !object_request->payload ||
            object_request->payload_size <
                sizeof(WotbModV3SceneEntityDescriptor) ||
            !g_services.resource.scene_entity_create) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        const WotbModV3SceneEntityDescriptor* descriptor =
            static_cast<const WotbModV3SceneEntityDescriptor*>(
                object_request->payload);
        void* resource = nullptr;
        WotbModResult result = CallResourceCreate(
            g_services.resource.scene_entity_create,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        const WotbModSceneTransform native_transform =
            ToLegacyTransform(descriptor->transform);
        result = CallSceneTransform(
            resource, &native_transform);
        if (result != WOTBMOD_OK) {
            CallResourceRelease(resource);
            return ConvertResult(result);
        }
        return WrapScene(
            mod,
            resource,
            nullptr,
            &descriptor->transform,
            object_response);
    }

    if (std::strcmp(operation, "scene_entity_load") == 0) {
        if (!ValidObjectRequest(
                request, request_size, &object_request) ||
            !ValidObjectResponse(
                response, response_size, &object_response)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        void* resource = nullptr;
        const WotbModResult result = CallResourceLoad(
            object_request->name,
            object_request->secondary_name,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        const WotbModV3Transform identity = IdentityTransform();
        return WrapScene(
            mod,
            resource,
            object_request->secondary_name,
            &identity,
            object_response);
    }

    if (std::strcmp(operation, "scene_get_active") == 0) {
        if (!ValidObjectResponse(
                response, response_size, &object_response) ||
            !g_services.resource.scene_get_active) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        void* resource = nullptr;
        const WotbModResult result = CallResourceCreate(
            g_services.resource.scene_get_active,
            &resource);
        if (result != WOTBMOD_OK || !resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        const WotbModV3Transform identity = IdentityTransform();
        return WrapScene(
            mod, resource, nullptr, &identity, object_response);
    }

    if (!ValidObjectRequest(
            request, request_size, &object_request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    NativeScene* scene = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        scene = FindSceneLocked(object_request->object, mod);
    }
    if (!scene) return WOTBMOD_V3_E_INVALID_HANDLE;

    if (std::strcmp(operation, "scene_entity_destroy") == 0) {
        return DestroyScene(mod, object_request->object);
    }

    if (std::strcmp(operation, "scene_entity_clone") == 0) {
        if (!ValidObjectResponse(
                response, response_size, &object_response)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        void* cloned_resource = nullptr;
        WotbModResult result = WOTBMOD_ERROR_PLATFORM;
        if (scene->resolved_path[0]) {
            result = CallResourceLoad(
                nullptr,
                scene->resolved_path,
                &cloned_resource);
        } else if (g_services.resource.scene_entity_create) {
            result = CallResourceCreate(
                g_services.resource.scene_entity_create,
                &cloned_resource);
        } else {
            /*
             * ResourceClone retains the same DAVA object, which is not a
             * semantic entity clone, so it is deliberately not used here.
             */
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (result != WOTBMOD_OK || !cloned_resource) {
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        const WotbModSceneTransform transform =
            ToLegacyTransform(scene->local);
        result = CallSceneTransform(
            cloned_resource, &transform);
        if (result != WOTBMOD_OK) {
            CallResourceRelease(cloned_resource);
            return ConvertResult(result);
        }
        return WrapScene(
            mod,
            cloned_resource,
            scene->resolved_path,
            &scene->local,
            object_response);
    }

    if (std::strcmp(operation, "scene_entity_set_parent") == 0 ||
        std::strcmp(operation, "scene_entity_attach_ex") == 0) {
        NativeScene* parent = nullptr;
        if (object_request->related_object != 0u) {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            parent = FindSceneLocked(
                object_request->related_object, mod);
        }
        if (object_request->related_object != 0u && !parent) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        if (std::strcmp(operation, "scene_entity_attach_ex") == 0 &&
            object_request->name && object_request->name[0]) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        if (scene->parent && scene->parent != parent) {
            const WotbModResult removed = CallScenePair(
                g_services.resource.scene_remove_child,
                scene->parent->resource,
                scene->resource);
            if (removed != WOTBMOD_OK) {
                return ConvertResult(removed);
            }
        }
        if (parent && scene->parent != parent) {
            const WotbModResult added = CallScenePair(
                g_services.resource.scene_add_child,
                parent->resource,
                scene->resource);
            if (added != WOTBMOD_OK) {
                return ConvertResult(added);
            }
        }
        scene->parent = parent;
        return WOTBMOD_V3_OK;
    }

    if (std::strcmp(operation, "scene_entity_set_transform") == 0 ||
        std::strcmp(operation, "scene_entity_set_world_transform") == 0) {
        if (!object_request->payload ||
            object_request->payload_size <
                sizeof(WotbModV3Transform)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const WotbModV3Transform requested =
            *static_cast<const WotbModV3Transform*>(
                object_request->payload);
        WotbModV3Transform local = requested;
        if (std::strcmp(
                operation,
                "scene_entity_set_world_transform") == 0 &&
            scene->parent) {
            WotbModV3Transform parent_world = {};
            {
                std::lock_guard<std::mutex> lock(g_services.mutex);
                if (!WorldTransformLocked(
                        scene->parent, &parent_world)) {
                    return WOTBMOD_V3_E_LIMIT_REACHED;
                }
            }
            local = RelativeTransform(parent_world, requested);
        }
        const WotbModSceneTransform native_transform =
            ToLegacyTransform(local);
        const WotbModResult result = CallSceneTransform(
            scene->resource, &native_transform);
        if (result == WOTBMOD_OK) {
            scene->local = local;
        }
        return ConvertResult(result);
    }

    if (std::strcmp(operation, "scene_entity_get_transform") == 0 ||
        std::strcmp(operation, "scene_entity_get_world_transform") == 0) {
        if (!response ||
            response_size < sizeof(WotbModV3Transform)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        WotbModV3Transform result = scene->local;
        if (std::strcmp(
                operation,
                "scene_entity_get_world_transform") == 0) {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            if (!WorldTransformLocked(scene, &result)) {
                return WOTBMOD_V3_E_LIMIT_REACHED;
            }
        }
        *static_cast<WotbModV3Transform*>(response) = result;
        return WOTBMOD_V3_OK;
    }

    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InvokeDrawMesh(
    WotbModV3Handle mod,
    const void* request,
    uint32_t request_size) {
    const ClientHostObjectRequest* object_request = nullptr;
    if (!ValidObjectRequest(
            request, request_size, &object_request) ||
        !object_request->payload ||
        object_request->payload_size < sizeof(WotbModV3DrawMesh)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (object_request->related_object != 0u) {
        /* D3D11 managed materials are not DAVA NMaterial objects. */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const WotbModV3DrawMesh* draw =
        static_cast<const WotbModV3DrawMesh*>(
            object_request->payload);
    WotbModV3Transform transform = {};
    if (draw->struct_size < sizeof(*draw) ||
        draw->api_version != WOTBMOD_V3_RENDER_VERSION ||
        !MatrixToTransform(draw->world, &transform)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_services.resource.scene_get_active ||
        !g_services.resource.scene_add_child ||
        !g_services.resource.scene_remove_child ||
        !g_services.resource.scene_set_transform ||
        !g_services.resource.release) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    std::lock_guard<std::mutex> lock(g_services.mutex);
    NativeScene* scene = FindSceneLocked(
        object_request->object, mod);
    if (!scene) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (scene->parent) {
        return WOTBMOD_V3_E_BUSY;
    }
    const WotbModSceneTransform native_transform =
        ToLegacyTransform(transform);
    const WotbModResult transformed = CallSceneTransform(
        scene->resource, &native_transform);
    if (transformed != WOTBMOD_OK) {
        return ConvertResult(transformed);
    }
    scene->local = transform;
    if (scene->transient_attached) {
        /* Multiple submissions in one frame coalesce to the last matrix. */
        return WOTBMOD_V3_OK;
    }

    void* active_scene = nullptr;
    const WotbModResult acquired = CallResourceCreate(
        g_services.resource.scene_get_active,
        &active_scene);
    if (acquired != WOTBMOD_OK || !active_scene) {
        return ConvertResult(
            acquired == WOTBMOD_OK
                ? WOTBMOD_ERROR_PLATFORM
                : acquired);
    }
    const WotbModResult attached = CallScenePair(
        g_services.resource.scene_add_child,
        active_scene,
        scene->resource);
    if (attached != WOTBMOD_OK) {
        CallResourceRelease(active_scene);
        return ConvertResult(attached);
    }
    TransientSceneAttachment attachment = {};
    attachment.scene = scene;
    attachment.active_scene_resource = active_scene;
    attachment.child_resource = scene->resource;
    g_services.transient_scenes.push_back(attachment);
    scene->transient_attached = true;
    return WOTBMOD_V3_OK;
}

WotbModResult CallAudioLoadClip(
    const char* path,
    void** output) {
    if (!g_services.audio.load_clip || !path || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.audio.load_clip(
            g_services.audio.user_data,
            path,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioClipOperation(
    WotbModRuntimeAudioClipOperation function,
    void* clip) {
    if (!function || !clip) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(g_services.audio.user_data, clip);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioPlay(
    void* clip,
    const WotbModAudioPlayInfo* parameters,
    void** output) {
    if (!g_services.audio.play || !clip || !parameters || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.audio.play(
            g_services.audio.user_data,
            clip,
            parameters,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioOperation(
    WotbModRuntimeAudioOperation function,
    void* playback) {
    if (!function || !playback) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(
            g_services.audio.user_data,
            playback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioSetParameters(
    void* playback,
    const WotbModAudioPlayInfo* parameters) {
    if (!g_services.audio.set_parameters ||
        !playback || !parameters) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.audio.set_parameters(
            g_services.audio.user_data,
            playback,
            parameters);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioGetState(
    void* playback,
    WotbModAudioState* output) {
    if (!g_services.audio.get_state ||
        !playback || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.audio.get_state(
            g_services.audio.user_data,
            playback,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioSeek(
    void* playback,
    double seconds) {
    if (!g_services.audio.seek || !playback) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.audio.seek(
            g_services.audio.user_data,
            playback,
            seconds);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallAudioGetTime(
    WotbModRuntimeAudioGetTime function,
    void* object,
    double* output) {
    if (!function || !object || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return function(
            g_services.audio.user_data,
            object,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundCreate(
    const char* name,
    void** output) {
    if (!g_services.sound.create || !name || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.create(
            g_services.sound.user_data,
            name,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundOperation(
    WotbModRuntimeSoundEventOperation function,
    void* event) {
    if (!function || !event) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(g_services.sound.user_data, event);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundStop(void* event, int32_t force) {
    if (!event || (force != 0 && force != 1)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (g_services.sound.stop_with_force) {
        __try {
            return g_services.sound.stop_with_force(
                g_services.sound.user_data,
                event,
                force);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    /*
     * Preserve ABI compatibility for older backends. Only the appended
     * callback can distinguish force=false from force=true.
     */
    return CallSoundOperation(g_services.sound.stop, event);
}

WotbModResult CallSoundScalar(
    WotbModRuntimeSoundEventSetScalar function,
    void* event,
    float value) {
    if (!function || !event) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(
            g_services.sound.user_data,
            event,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundVector(
    WotbModRuntimeSoundEventSetVector3 function,
    void* event,
    const WotbModV3Vec4& value) {
    if (!function || !event) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(
            g_services.sound.user_data,
            event,
            value.x,
            value.y,
            value.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundInteger(
    WotbModRuntimeSoundEventSetInteger function,
    void* event,
    int32_t value) {
    if (!function || !event) return WOTBMOD_ERROR_PLATFORM;
    __try {
        return function(
            g_services.sound.user_data,
            event,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundPaused(void* event, int32_t value) {
    if (!g_services.sound.set_paused || !event) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.set_paused(
            g_services.sound.user_data,
            event,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundVolume(void* event, float value) {
    if (!g_services.sound.set_volume || !event) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.set_volume(
            g_services.sound.user_data,
            event,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundPosition(
    void* event,
    const WotbModV3Vec4& value) {
    if (!g_services.sound.set_position || !event) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.set_position(
            g_services.sound.user_data,
            event,
            value.x,
            value.y,
            value.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundSetParameter(
    void* event,
    const char* name,
    float value) {
    if (!g_services.sound.set_parameter || !event || !name) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.set_parameter(
            g_services.sound.user_data,
            event,
            name,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundGetParameter(
    void* event,
    const char* name,
    float* output) {
    if (!g_services.sound.get_parameter ||
        !event || !name || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.get_parameter(
            g_services.sound.user_data,
            event,
            name,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModResult CallSoundHasParameter(
    void* event,
    const char* name,
    int32_t* output) {
    if (!g_services.sound.has_parameter ||
        !event || !name || !output) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_services.sound.has_parameter(
            g_services.sound.user_data,
            event,
            name,
            output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

WotbModV3Result DestroyAudio(
    WotbModV3Handle owner,
    uint64_t token,
    AudioKind kind) {
    NativeAudio* audio = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        audio = FindAudioLocked(token, owner, kind);
        if (!audio) return WOTBMOD_V3_E_INVALID_HANDLE;
        g_services.audio_objects.erase(
            std::remove(
                g_services.audio_objects.begin(),
                g_services.audio_objects.end(),
                audio),
            g_services.audio_objects.end());
    }
    WotbModResult result = WOTBMOD_OK;
    if (audio->kind == AudioKind::Clip) {
        if (audio->playback) {
            result = CallAudioOperation(
                g_services.audio.release,
                audio->playback);
            audio->playback = nullptr;
        }
        if (audio->clip) {
            const WotbModResult clip_result =
                CallAudioClipOperation(
                    g_services.audio.release_clip,
                    audio->clip);
            if (result == WOTBMOD_OK) result = clip_result;
            audio->clip = nullptr;
        }
    } else if (audio->sound_event) {
        result = CallSoundOperation(
            g_services.sound.release,
            audio->sound_event);
        audio->sound_event = nullptr;
    }
    audio->magic = 0u;
    delete audio;
    return ConvertResult(result);
}

WotbModV3Result CreateClip(
    WotbModV3Handle mod,
    const char* operation,
    const ClientHostObjectRequest& request,
    ClientHostObjectResponse* response) {
    if (!response || !request.secondary_name ||
        !request.secondary_name[0] ||
        !g_services.audio.load_clip ||
        !g_services.audio.release_clip) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (std::strcmp(operation, "audio_create_stream") == 0) {
        /*
         * The current Windows backend decodes complete files. Reporting
         * stream creation as successful would violate the streaming contract.
         */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    void* clip = nullptr;
    WotbModResult result = CallAudioLoadClip(
        request.secondary_name, &clip);
    if (result != WOTBMOD_OK || !clip) {
        return ConvertResult(
            result == WOTBMOD_OK
                ? WOTBMOD_ERROR_PLATFORM
                : result);
    }
    NativeAudio* audio = new (std::nothrow) NativeAudio();
    if (!audio) {
        CallAudioClipOperation(
            g_services.audio.release_clip, clip);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    audio->owner = mod;
    audio->clip = clip;
    audio->parameters.struct_size = sizeof(audio->parameters);
    audio->parameters.volume =
        static_cast<float>(request.scalar0);
    audio->parameters.pitch =
        static_cast<float>(request.scalar1);
    audio->parameters.pan = 0.0f;
    audio->parameters.min_distance = 1.0f;
    audio->parameters.max_distance = 100.0f;
    if ((request.flags & WOTBMOD_V3_AUDIO_CREATE_LOOP) != 0u) {
        audio->parameters.flags |= WOTBMOD_AUDIO_PLAY_LOOP;
    }
    if ((request.flags & WOTBMOD_V3_AUDIO_CREATE_SPATIAL) != 0u) {
        audio->parameters.flags |= WOTBMOD_AUDIO_PLAY_SPATIAL;
    }
    if (!CopyText(
            audio->name,
            sizeof(audio->name),
            request.name ? request.name : request.secondary_name)) {
        CallAudioClipOperation(
            g_services.audio.release_clip, clip);
        delete audio;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.audio_objects.push_back(audio);
    }
    response->object = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(audio));
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyClipParameters(
    NativeAudio* audio,
    const WotbModAudioPlayInfo& previous) {
    if (!audio || !audio->playback) return WOTBMOD_V3_OK;
    const WotbModResult result = CallAudioSetParameters(
        audio->playback, &audio->parameters);
    if (result != WOTBMOD_OK) {
        audio->parameters = previous;
    }
    return ConvertResult(result);
}

WotbModV3Result InvokeClip(
    WotbModV3Handle mod,
    const char* operation,
    const ClientHostObjectRequest& request,
    ClientHostObjectResponse* response) {
    NativeAudio* audio = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        audio = FindAudioLocked(
            request.object, mod, AudioKind::Clip);
    }
    if (!audio) return WOTBMOD_V3_E_INVALID_HANDLE;

    if (std::strcmp(operation, "audio_destroy") == 0) {
        return DestroyAudio(
            mod, request.object, AudioKind::Clip);
    }
    if (std::strcmp(operation, "audio_preload") == 0) {
        audio->state = WOTBMOD_V3_AUDIO_STATE_PRELOADED;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "audio_play") == 0) {
        if (audio->playback) {
            const WotbModResult release_result =
                CallAudioOperation(
                    g_services.audio.release,
                    audio->playback);
            if (release_result != WOTBMOD_OK) {
                return ConvertResult(release_result);
            }
            audio->playback = nullptr;
        }
        void* playback = nullptr;
        const WotbModResult result = CallAudioPlay(
            audio->clip, &audio->parameters, &playback);
        if (result != WOTBMOD_OK || !playback) {
            audio->state = WOTBMOD_V3_AUDIO_STATE_ERROR;
            return ConvertResult(
                result == WOTBMOD_OK
                    ? WOTBMOD_ERROR_PLATFORM
                    : result);
        }
        audio->playback = playback;
        audio->state =
            (audio->parameters.flags &
             WOTBMOD_AUDIO_PLAY_START_PAUSED) != 0u
                ? WOTBMOD_V3_AUDIO_STATE_PAUSED
                : WOTBMOD_V3_AUDIO_STATE_PLAYING;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "audio_pause") == 0 ||
        std::strcmp(operation, "audio_resume") == 0 ||
        std::strcmp(operation, "audio_stop") == 0) {
        if (!audio->playback) return WOTBMOD_V3_E_NOT_FOUND;
        WotbModRuntimeAudioOperation function = nullptr;
        uint32_t target_state = WOTBMOD_V3_AUDIO_STATE_STOPPED;
        if (operation[6] == 'p') {
            function = g_services.audio.pause;
            target_state = WOTBMOD_V3_AUDIO_STATE_PAUSED;
        } else if (operation[6] == 'r') {
            function = g_services.audio.resume;
            target_state = WOTBMOD_V3_AUDIO_STATE_PLAYING;
        } else {
            if (request.scalar0 != 0.0) {
                return WOTBMOD_V3_E_NOT_SUPPORTED;
            }
            function = g_services.audio.stop;
        }
        const WotbModResult result =
            CallAudioOperation(function, audio->playback);
        if (result == WOTBMOD_OK) audio->state = target_state;
        return ConvertResult(result);
    }
    if (std::strcmp(operation, "audio_get_state") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        if (audio->playback && g_services.audio.get_state) {
            WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
            const WotbModResult result =
                CallAudioGetState(audio->playback, &state);
            if (result != WOTBMOD_OK) return ConvertResult(result);
            audio->state =
                state == WOTBMOD_AUDIO_PLAYING
                    ? WOTBMOD_V3_AUDIO_STATE_PLAYING
                    : state == WOTBMOD_AUDIO_PAUSED
                        ? WOTBMOD_V3_AUDIO_STATE_PAUSED
                        : WOTBMOD_V3_AUDIO_STATE_STOPPED;
        }
        response->value_u32 = audio->state;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "audio_seek") == 0) {
        if (!audio->playback) return WOTBMOD_V3_E_NOT_FOUND;
        if (!g_services.audio.seek) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        return ConvertResult(CallAudioSeek(
            audio->playback, request.scalar0));
    }
    if (std::strcmp(operation, "audio_get_position") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        if (!audio->playback) return WOTBMOD_V3_E_NOT_FOUND;
        if (!g_services.audio.get_position) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        double seconds = 0.0;
        const WotbModResult result = CallAudioGetTime(
            g_services.audio.get_position,
            audio->playback,
            &seconds);
        if (result == WOTBMOD_OK) {
            response->value_f64 = seconds;
        }
        return ConvertResult(result);
    }
    if (std::strcmp(operation, "audio_get_duration") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        if (!g_services.audio.get_duration) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        double seconds = 0.0;
        const WotbModResult result = CallAudioGetTime(
            g_services.audio.get_duration,
            audio->clip,
            &seconds);
        if (result == WOTBMOD_OK) {
            response->value_f64 = seconds;
        }
        return ConvertResult(result);
    }

    const WotbModAudioPlayInfo previous = audio->parameters;
    if (std::strcmp(operation, "audio_set_volume") == 0) {
        audio->parameters.volume =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_pitch") == 0) {
        audio->parameters.pitch =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_pan") == 0) {
        audio->parameters.pan =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_position_3d") == 0) {
        audio->parameters.position_x = request.vector.x;
        audio->parameters.position_y = request.vector.y;
        audio->parameters.position_z = request.vector.z;
        audio->parameters.flags |= WOTBMOD_AUDIO_PLAY_SPATIAL;
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_min_distance") == 0) {
        audio->parameters.min_distance =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_max_distance") == 0) {
        audio->parameters.max_distance =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_set_loop") == 0) {
        if (request.flags != 0u) {
            audio->parameters.flags |= WOTBMOD_AUDIO_PLAY_LOOP;
        } else {
            audio->parameters.flags &= ~WOTBMOD_AUDIO_PLAY_LOOP;
        }
        return ApplyClipParameters(audio, previous);
    }
    if (std::strcmp(operation, "audio_fade_to") == 0) {
        if (request.scalar1 != 0.0) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        audio->parameters.volume =
            static_cast<float>(request.scalar0);
        return ApplyClipParameters(audio, previous);
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result CreateSoundEvent(
    WotbModV3Handle mod,
    const ClientHostObjectRequest& request,
    ClientHostObjectResponse* response) {
    if (!request.name || !request.name[0] || !response ||
        !g_services.sound.create ||
        !g_services.sound.release) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    void* event = nullptr;
    const WotbModResult result =
        CallSoundCreate(request.name, &event);
    if (result != WOTBMOD_OK || !event) {
        return ConvertResult(
            result == WOTBMOD_OK
                ? WOTBMOD_ERROR_PLATFORM
                : result);
    }
    NativeAudio* audio = new (std::nothrow) NativeAudio();
    if (!audio) {
        CallSoundOperation(g_services.sound.release, event);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    audio->owner = mod;
    audio->kind = AudioKind::SoundEvent;
    audio->sound_event = event;
    if (!CopyText(audio->name, sizeof(audio->name), request.name)) {
        CallSoundOperation(g_services.sound.release, event);
        delete audio;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.audio_objects.push_back(audio);
    }
    response->object = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(audio));
    return WOTBMOD_V3_OK;
}

WotbModV3Result InvokeSoundEvent(
    WotbModV3Handle mod,
    const char* operation,
    const ClientHostObjectRequest& request,
    ClientHostObjectResponse* response) {
    NativeAudio* audio = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        audio = FindAudioLocked(
            request.object, mod, AudioKind::SoundEvent);
    }
    if (!audio) return WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    if (std::strcmp(operation, "sound_event_destroy") == 0) {
        return DestroyAudio(
            mod, request.object, AudioKind::SoundEvent);
    }
    if (std::strcmp(operation, "sound_event_trigger") == 0) {
        result = CallSoundOperation(
            g_services.sound.trigger, audio->sound_event);
    } else if (std::strcmp(operation, "sound_event_stop") == 0) {
        if (request.flags > 1u) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        result = CallSoundStop(
            audio->sound_event,
            request.flags != 0u ? 1 : 0);
    } else if (std::strcmp(
                   operation, "sound_event_set_paused") == 0) {
        result = CallSoundPaused(
            audio->sound_event,
            request.flags != 0u ? 1 : 0);
    } else if (std::strcmp(
                   operation, "sound_event_set_volume") == 0) {
        result = CallSoundVolume(
            audio->sound_event,
            static_cast<float>(request.scalar0));
    } else if (std::strcmp(
                   operation, "sound_event_set_position") == 0) {
        result = CallSoundPosition(
            audio->sound_event, request.vector);
    } else if (std::strcmp(
                   operation, "sound_event_set_parameter") == 0) {
        result = CallSoundSetParameter(
            audio->sound_event,
            request.name,
            static_cast<float>(request.scalar0));
    } else if (std::strcmp(
                   operation, "sound_event_get_parameter") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        float value = 0.0f;
        result = CallSoundGetParameter(
            audio->sound_event, request.name, &value);
        if (result == WOTBMOD_OK) {
            response->value_f64 = static_cast<double>(value);
        }
    } else if (std::strcmp(
                   operation, "sound_event_has_parameter") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        int32_t has = 0;
        result = CallSoundHasParameter(
            audio->sound_event, request.name, &has);
        if (result == WOTBMOD_OK) {
            response->value_u32 = has != 0 ? 1u : 0u;
        }
    } else if (std::strcmp(
                   operation, "sound_event_set_speed") == 0) {
        if (!g_services.sound.set_speed) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = CallSoundScalar(
            g_services.sound.set_speed,
            audio->sound_event,
            static_cast<float>(request.scalar0));
    } else if (std::strcmp(
                   operation, "sound_event_set_direction") == 0) {
        if (!g_services.sound.set_direction) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = CallSoundVector(
            g_services.sound.set_direction,
            audio->sound_event,
            request.vector);
    } else if (std::strcmp(
                   operation, "sound_event_set_velocity") == 0) {
        if (!g_services.sound.set_velocity) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = CallSoundVector(
            g_services.sound.set_velocity,
            audio->sound_event,
            request.vector);
    } else if (std::strcmp(
                   operation, "sound_event_set_loop_count") == 0) {
        if (!g_services.sound.set_loop_count) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = CallSoundInteger(
            g_services.sound.set_loop_count,
            audio->sound_event,
            request.signed_value);
    } else if (std::strcmp(
                   operation, "sound_event_set_priority") == 0) {
        if (!g_services.sound.set_priority) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = CallSoundInteger(
            g_services.sound.set_priority,
            audio->sound_event,
            request.signed_value);
    } else {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return ConvertResult(result);
}

WotbModV3Result InvokeAudio(
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size) {
    const bool subscribe_started =
        std::strcmp(operation, "audio_subscribe_started") == 0;
    const bool subscribe_finished =
        std::strcmp(operation, "audio_subscribe_finished") == 0;
    const bool subscribe_error =
        std::strcmp(operation, "audio_subscribe_error") == 0;
    if (subscribe_started || subscribe_finished || subscribe_error) {
        if (!request ||
            request_size <
                sizeof(v3::ClientHostAudioSubscriptionRequest)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const v3::ClientHostAudioSubscriptionRequest* subscription =
            static_cast<
                const v3::ClientHostAudioSubscriptionRequest*>(request);
        const uint32_t expected_kind =
            subscribe_started
                ? v3::CLIENT_HOST_AUDIO_STARTED
                : (subscribe_finished
                       ? v3::CLIENT_HOST_AUDIO_FINISHED
                       : v3::CLIENT_HOST_AUDIO_ERROR);
        if (subscription->struct_size <
                sizeof(v3::ClientHostAudioSubscriptionRequest) ||
            subscription->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
            subscription->object == 0u ||
            subscription->mod != mod ||
            subscription->lifecycle_kind != expected_kind) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        /*
         * Playback control is available, but neither the Windows audio
         * backend nor the DAVA sound bridge exposes an authoritative
         * asynchronous lifecycle source. Do not manufacture a token.
         */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    const ClientHostObjectRequest* object_request = nullptr;
    ClientHostObjectResponse* object_response = nullptr;
    if (!ValidObjectRequest(
            request, request_size, &object_request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (response &&
        !ValidObjectResponse(
            response, response_size, &object_response)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "audio_create") == 0 ||
        std::strcmp(operation, "audio_create_stream") == 0) {
        return CreateClip(
            mod, operation, *object_request, object_response);
    }
    if (std::strncmp(operation, "sound_event_", 12u) == 0) {
        if (std::strcmp(operation, "sound_event_create") == 0) {
            return CreateSoundEvent(
                mod, *object_request, object_response);
        }
        return InvokeSoundEvent(
            mod, operation, *object_request, object_response);
    }
    if (std::strcmp(operation, "audio_unsubscribe") == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return InvokeClip(
        mod, operation, *object_request, object_response);
}

/* =========================================================================
 * wotbmod.ui.read -- read back the mirror, and nothing else.
 *
 * There is no engine access anywhere below. ui_v4.h is explicit that no
 * proven path exists to read the live DAVA UIStaticText string out of a
 * game-owned control on this fingerprint, so this backend does not pretend
 * to have one: it answers with exactly the values this API committed, and
 * the per-field _SET bits say which of them were ever committed at all.
 * ========================================================================= */

const char* MirrorStringForField(
    const NativeUi& ui,
    uint32_t field,
    uint32_t* out_set_flag) {
    switch (field) {
        case v3::CLIENT_HOST_UI_READ_FIELD_TEXT:
            *out_set_flag = WOTBMOD_V3_UI_READ_TEXT_SET;
            return ui.text.c_str();
        case v3::CLIENT_HOST_UI_READ_FIELD_TEXTURE:
            *out_set_flag = WOTBMOD_V3_UI_READ_TEXTURE_SET;
            return ui.texture.c_str();
        case v3::CLIENT_HOST_UI_READ_FIELD_FONT:
            *out_set_flag = WOTBMOD_V3_UI_READ_FONT_SET;
            return ui.font.c_str();
        default:
            *out_set_flag = 0u;
            return nullptr;
    }
}

/* =========================================================================
 * wotbmod.scene.enumerate -- bounded, read-only, MAIN THREAD ONLY.
 *
 * Layout, all PROVEN on 11.19.0.834 / 41960DBD...E0AD rather than assumed:
 *
 *   Entity::AddNode (0x00CD6C10) retains the child, detaches it from
 *   [child+0x18] through vtable slot +0x18, appends at [this+0x0C] against
 *   the capacity end [this+0x10] with the begin at [this+0x08], writes
 *   [child+0x18] = this, then reaches the child's TransformComponent at
 *   [child+0x3C] and stores the parent entity at TC+0xA4 and the parent's
 *   world Transform (parentTC+0x38) at TC+0xA0. Entity::RemoveNode
 *   (0x00D10C90) is its exact mirror.
 *
 *   The real Entity(const FastName&) constructor (0x00CCE250) installs
 *   vtable 0x036125E8, zeroes +0x08/+0x0C/+0x10 (children), +0x14 (Scene*)
 *   and +0x18 (parent), and stores ONE dword - the interned const char* the
 *   FastName holds - at +0x1C. Scene::Scene (0x00CCF190) calls that same
 *   constructor before swapping in vtable 0x03612764, which is why a Scene
 *   is walked with the identical code.
 *
 *   TransformComponent::SetLocalTransform (0x00D16070) copies exactly 40
 *   bytes into TC+0x10, so the local Transform is 40 bytes at +0x10 and the
 *   world Transform is 40 bytes at +0x38; +0x38 + 40 = +0x60, and the 64-byte
 *   Matrix4 there ends exactly at +0xA0 where AddNode proved the parent
 *   pointers live. The layout closes on itself.
 *
 * THE HAZARD, restated because it is the reason for every refusal below:
 * AddNode and RemoveNode memmove that child vector with NO lock, and
 * RemoveNode Releases the child immediately after compacting. A walker on
 * another thread can read a pointer that is freed one instruction later.
 * There is no lock and no version counter that could detect it. Thread
 * discipline is the only defence that exists, so the walk refuses every
 * thread it has not seen running EngineBackend::UpdateAndDrawWindows -
 * including, deliberately, the case where no frame has been observed yet.
 * ========================================================================= */

/*
 * Provider caps, deliberately BELOW the scene_v2.h ceilings.
 *
 * The walk holds the main thread for its whole duration, inside the frame.
 * 8192 nodes x 160 bytes is 1.3 MB of staging plus the same again in the
 * mod's buffer, and every one of those nodes costs several guarded reads of
 * engine memory. Capping at 4096/1024 keeps the worst-case stall bounded by
 * something the host chose rather than by how large a battle scene happens to
 * be; the runtime clamps a caller's request to whichever of the two is lower.
 */
constexpr uint32_t kSceneWalkMaxDepth = WOTBMOD_V3_SCENE_WALK_MAX_DEPTH;
constexpr uint32_t kSceneWalkMaxChildren = 1024u;
constexpr uint32_t kSceneWalkMaxNodes = 4096u;

const uint32_t kEntityChildrenBeginOffset = 0x08u;
const uint32_t kEntityChildrenEndOffset = 0x0Cu;
const uint32_t kEntityNameOffset = 0x1Cu;
const uint32_t kEntityTransformOffset = 0x3Cu;
const uint32_t kTransformWorldMatrixOffset = 0x60u;

/*
 * Raw reads live in their own frames on purpose. Under /EHsc MSVC forbids
 * __try in a function that owns unwindable objects (C2712), and the walk
 * itself owns std::vectors - so every dereference of engine memory is a call
 * into one of these, never an inline access.
 */
bool SceneSafeRead(const void* address, void* output, size_t size) {
    if (!address || !output || size == 0u) return false;
    __try {
        std::memcpy(output, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/*
 * Copies at most `capacity - 1` bytes of a NUL-terminated string and reports
 * whether it had to cut. A fault mid-string is a failure, not a truncation:
 * a partially copied name would be indistinguishable from a real one.
 */
bool SceneSafeReadString(
    const char* address,
    char* output,
    size_t capacity,
    bool* out_truncated) {
    if (!address || !output || capacity == 0u || !out_truncated) {
        return false;
    }
    *out_truncated = false;
    __try {
        for (size_t index = 0u; index + 1u < capacity; ++index) {
            const char value = address[index];
            output[index] = value;
            if (value == '\0') return true;
        }
        output[capacity - 1u] = '\0';
        /* Only a real extra byte counts as truncation, not the terminator. */
        *out_truncated = address[capacity - 1u] != '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SceneAddressReadable(const void* address, size_t size) {
    if (!address || size == 0u) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    const uint8_t* const start = static_cast<const uint8_t*>(address);
    const uint8_t* const region_end =
        static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    return start + size <= region_end;
}

const void* SceneImageAddress(uint32_t rva) {
    if (!g_services.game_base || rva >= g_services.game_image_size) {
        return nullptr;
    }
    return g_services.game_base + rva;
}

bool SceneWalkAvailable() {
    return g_services.game_base != nullptr &&
        g_services.game_image_size != 0u &&
        (g_services.compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED ||
         g_services.compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED) &&
        g_services.resource.scene_get_active != nullptr &&
        g_services.resource.release != nullptr;
}

struct SceneWalkNode {
    const uint8_t* entity;
    uint32_t parent_index;
    uint32_t depth;
};

/*
 * One record. Every engine read is individually guarded and individually
 * optional: a node whose TransformComponent cannot be read still produces a
 * valid record with HAS_WORLD_MATRIX clear and an identity filler matrix,
 * because scene_v2.h says that flag is the difference between a measurement
 * and a placeholder.
 */
void FillSceneNodeRecord(
    const uint8_t* entity,
    uint32_t index,
    uint32_t parent_index,
    uint32_t depth,
    uint32_t child_count,
    uint32_t max_children,
    bool depth_limited,
    bool children_unreadable,
    const void* scene_vtable,
    WotbModV3SceneNodeRecord* record) {
    std::memset(record, 0, sizeof(*record));
    record->struct_size = sizeof(*record);
    record->api_version = WOTBMOD_V3_SCENE_VERSION_2;
    record->index = index;
    record->parent_index = parent_index;
    record->depth = depth;
    record->child_count = child_count;
    record->world_matrix.values[0] = 1.0f;
    record->world_matrix.values[5] = 1.0f;
    record->world_matrix.values[10] = 1.0f;
    record->world_matrix.values[15] = 1.0f;

    if (child_count > max_children || children_unreadable) {
        /*
         * Also set when the child vector could not be read at all. Reporting
         * "0 children" with no flag would be a silent lie - the node would
         * look like a leaf - and this flag already means exactly "children of
         * this node are absent from the answer".
         */
        record->flags |= WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED;
    }
    if (depth_limited) {
        record->flags |= WOTBMOD_V3_SCENE_NODE_DEPTH_LIMITED;
    }

    const void* vtable = nullptr;
    if (SceneSafeRead(entity, &vtable, sizeof(vtable)) &&
        scene_vtable != nullptr && vtable == scene_vtable) {
        record->flags |= WOTBMOD_V3_SCENE_NODE_IS_SCENE;
    }

    /*
     * DAVA FastName is ONE dword holding an interned const char*, and it is
     * not refcounted, so reading it is a dword load plus a bounded string
     * copy - no allocation and no FastNameDB lock on a path that is holding
     * the main thread.
     */
    const char* name = nullptr;
    if (SceneSafeRead(
            entity + kEntityNameOffset, &name, sizeof(name)) &&
        name != nullptr) {
        bool truncated = false;
        if (SceneSafeReadString(
                name,
                record->name,
                sizeof(record->name),
                &truncated)) {
            if (truncated) {
                record->flags |= WOTBMOD_V3_SCENE_NODE_NAME_TRUNCATED;
            }
        } else {
            record->name[0] = '\0';
        }
    }

    const uint8_t* transform = nullptr;
    if (!SceneSafeRead(
            entity + kEntityTransformOffset,
            &transform,
            sizeof(transform)) ||
        transform == nullptr) {
        return;
    }
    WotbModV3Matrix4 world = {};
    if (!SceneSafeRead(
            transform + kTransformWorldMatrixOffset,
            world.values,
            sizeof(world.values))) {
        return;
    }
    for (size_t component = 0u;
         component < sizeof(world.values) / sizeof(world.values[0]);
         ++component) {
        if (!std::isfinite(world.values[component])) return;
    }
    record->world_matrix = world;
    record->flags |= WOTBMOD_V3_SCENE_NODE_HAS_WORLD_MATRIX;
}

/*
 * Reads the child vector as a single consistent pair and rejects anything
 * that is not a plausible contiguous Entity*[begin, end). The pointers are
 * torn by construction - the engine writes them without a lock - so this is
 * a sanity filter, not a synchronisation primitive; the real defence is that
 * only the thread that mutates them is allowed to be here at all.
 */
bool ReadSceneChildren(
    const uint8_t* entity,
    const uint8_t** out_begin,
    uint32_t* out_count) {
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    *out_begin = nullptr;
    *out_count = 0u;
    if (!SceneSafeRead(
            entity + kEntityChildrenBeginOffset, &begin, sizeof(begin)) ||
        !SceneSafeRead(
            entity + kEntityChildrenEndOffset, &end, sizeof(end))) {
        return false;
    }
    if (begin == nullptr || end == nullptr) return true;
    if (end < begin) return false;
    const size_t span = static_cast<size_t>(end - begin);
    if ((span % sizeof(void*)) != 0u) return false;
    const size_t count = span / sizeof(void*);
    /*
     * A sanity ceiling far above the per-node cap, not the cap itself: a node
     * with more children than the walk will emit is a legitimate node whose
     * real child_count belongs in the record, and refusing it here would
     * report it as a leaf. What actually rejects a torn pair is the
     * readability check below - a bogus span will not be backed by one
     * committed region.
     */
    if (count > 0x10000u) return false;
    if (count != 0u && !SceneAddressReadable(begin, span)) return false;
    *out_begin = begin;
    *out_count = static_cast<uint32_t>(count);
    return true;
}

}  // namespace

WotbModV3Result InitializeV3NativeClientServices(
    const V3NativeClientServicesOptions* options) {
    if (!options ||
        options->struct_size < sizeof(*options)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_services.mutex);
    if (g_services.initialized ||
        g_services.shutdown_requested ||
        g_services.shutdown_running) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    if (options->resource_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->resource_backend->struct_size),
            sizeof(g_services.resource));
        std::memcpy(
            &g_services.resource,
            options->resource_backend,
            size);
    }
    if (options->audio_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->audio_backend->struct_size),
            sizeof(g_services.audio));
        std::memcpy(
            &g_services.audio,
            options->audio_backend,
            size);
    }
    if (options->sound_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->sound_backend->struct_size),
            sizeof(g_services.sound));
        std::memcpy(
            &g_services.sound,
            options->sound_backend,
            size);
    }
    g_services.resource_identity = options->resource_identity;
    g_services.resource_identity_user_data =
        options->resource_identity_user_data;
    g_services.resource_bring_ui_to_front =
        options->resource_bring_ui_to_front;
    g_services.resource_bring_ui_to_front_user_data =
        options->resource_bring_ui_to_front_user_data;
    g_services.game_base = options->game_base;
    g_services.game_image_size = options->game_image_size;
    g_services.compatibility_state = options->compatibility_state;
    g_services.ui_frame_epoch = 0u;
    g_services.ui_mutation_resume_frame = 0u;
    g_services.ui_input_active = false;
    g_services.initialized = true;
    return WOTBMOD_V3_OK;
}

WotbModV3Result InvokeV3NativeClientServices(
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size) {
    if (!operation || !operation[0]) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ScopedServiceInvocation invocation;
    if (!invocation.active()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (std::strncmp(operation, "ui_", 3u) == 0) {
        return InvokeUi(
            mod,
            operation,
            request,
            request_size,
            response,
            response_size);
    }
    if (std::strncmp(operation, "scene_", 6u) == 0) {
        return InvokeScene(
            mod,
            operation,
            request,
            request_size,
            response,
            response_size);
    }
    if (std::strcmp(operation, "render_draw_mesh") == 0) {
        return InvokeDrawMesh(mod, request, request_size);
    }
    if (std::strncmp(operation, "audio_", 6u) == 0 ||
        std::strncmp(operation, "sound_event_", 12u) == 0) {
        return InvokeAudio(
            mod,
            operation,
            request,
            request_size,
            response,
            response_size);
    }
    if (std::strncmp(operation, "sound_override_", 15u) == 0) {
        /*
         * Stock-event replacement needs a loader-owned SoundSystem creation
         * interception and an owner/priority conflict chain. No such backend
         * is installed, so this remains an explicit truthful refusal.
         */
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

void NotifyV3NativeClientServicesUiInputPhase(uint32_t phase) {
    std::lock_guard<std::mutex> lock(g_services.mutex);
    if (!g_services.initialized || g_services.shutdown_requested ||
        g_services.shutdown_running) {
        return;
    }
    if (phase == 1u || phase == 2u) {
        g_services.ui_input_active = true;
        return;
    }
    if (phase == 3u || phase == 6u) {
        g_services.ui_input_active = false;
        g_services.ui_mutation_resume_frame =
            g_services.ui_frame_epoch + kUiInputIdleFrames;
    }
}

void ResetV3NativeClientServicesUiInputState() {
    std::lock_guard<std::mutex> lock(g_services.mutex);
    if (!g_services.initialized || g_services.shutdown_requested ||
        g_services.shutdown_running) {
        return;
    }
    g_services.ui_input_active = false;
    g_services.ui_mutation_resume_frame =
        g_services.ui_frame_epoch + kUiInputIdleFrames;
}

void PumpV3NativeClientServicesFrame() {
    ScopedServiceInvocation invocation;
    if (!invocation.active()) return;

    std::vector<TransientSceneAttachment> transient;
    std::vector<NativeUi*> pending_ui;
    std::vector<void*> retired_ui_resources;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        ++g_services.ui_frame_epoch;
        const bool ui_mutation_safe =
            !g_services.ui_input_active &&
            g_services.ui_frame_epoch >=
                g_services.ui_mutation_resume_frame;
        transient.swap(g_services.transient_scenes);
        if (ui_mutation_safe) {
            auto first_pending = std::stable_partition(
                g_services.retired_ui_resources.begin(),
                g_services.retired_ui_resources.end(),
                [](const RetiredUiResource& retired) {
                    return retired.release_after_frame >
                        g_services.ui_frame_epoch;
                });
            for (auto current = first_pending;
                 current != g_services.retired_ui_resources.end();
                 ++current) {
                if (current->resource) {
                    retired_ui_resources.push_back(current->resource);
                }
            }
            g_services.retired_ui_resources.erase(
                first_pending,
                g_services.retired_ui_resources.end());
            for (NativeUi* ui : g_services.ui_objects) {
                if (ui && ui->rebuild_pending &&
                    ui->lifetime_leases == 0u) {
                    ui->rebuild_pending = false;
                    AcquireUiLeaseLocked(ui);
                    pending_ui.push_back(ui);
                }
            }
        }
        for (const auto& attachment : transient) {
            if (attachment.scene &&
                std::find(
                    g_services.scenes.begin(),
                    g_services.scenes.end(),
                    attachment.scene) != g_services.scenes.end()) {
                attachment.scene->transient_attached = false;
            }
        }
    }
    for (const auto& attachment : transient) {
        if (attachment.active_scene_resource &&
            attachment.child_resource) {
            CallScenePair(
                g_services.resource.scene_remove_child,
                attachment.active_scene_resource,
                attachment.child_resource);
        }
        if (attachment.active_scene_resource) {
            CallResourceRelease(attachment.active_scene_resource);
        }
    }
    for (void* resource : retired_ui_resources) {
        if (resource) CallResourceRelease(resource);
    }
    for (NativeUi* ui : pending_ui) {
        bool rebuild_disabled = false;
        {
            std::lock_guard<std::mutex> lock(g_services.mutex);
            rebuild_disabled =
                ui->destroy_requested || !g_services.initialized ||
                g_services.shutdown_requested ||
                g_services.shutdown_running;
        }
        if (!rebuild_disabled &&
            RebuildDynamicUi(ui) != WOTBMOD_OK) {
            OutputDebugStringA(
                "[wotbmod] deferred DAVA UI rebuild failed\n");
        }
        ReleaseUiLease(ui);
    }
}

void NotifyV3NativeClientServicesMainThread(
    uint32_t thread_id,
    uint64_t main_frame_index) {
    if (thread_id == 0u) return;
    InterlockedExchange(
        &g_main_thread_id, static_cast<LONG>(thread_id));
    InterlockedExchange64(
        &g_main_frame_index, static_cast<LONG64>(main_frame_index));
}

bool IsV3NativeClientServicesMainThread() {
    const LONG observed =
        InterlockedCompareExchange(&g_main_thread_id, 0, 0);
    return observed != 0 &&
        static_cast<DWORD>(observed) == GetCurrentThreadId();
}

WotbModV3Result V3NativeUiReadString(
    void*,
    const v3::ClientHostUiReadRequest* request,
    v3::ClientHostUiReadString* out_value) {
    if (!request || !out_value ||
        request->struct_size < sizeof(*request) ||
        request->api_version !=
            WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION ||
        request->object == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ScopedServiceInvocation invocation;
    if (!invocation.active()) return WOTBMOD_V3_E_NOT_SUPPORTED;

    out_value->struct_size = sizeof(*out_value);
    out_value->api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    out_value->flags = 0u;
    out_value->length = 0u;
    out_value->value[0] = '\0';

    std::lock_guard<std::mutex> lock(g_services.mutex);
    NativeUi* ui = FindUiLocked(request->object, request->mod);
    if (!ui) {
        /*
         * A token the loader does not know is a control destroyed behind the
         * runtime's back. client_services_backend.h mandates NOT_FOUND here
         * rather than an empty answer, so the two cannot be confused.
         */
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    if (request->field == v3::CLIENT_HOST_UI_READ_FIELD_LIVE_TEXT) {
        /*
         * The engine's own string, through the DAVA provider. No text
         * component is not an error at this boundary: an empty value with
         * the _SET bit clear is the answer, exactly as for an unwritten
         * mirror field, and the runtime turns it into NOT_FOUND for the mod.
         */
        if (!g_services.resource.ui_get_text) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        uint32_t size = static_cast<uint32_t>(sizeof(out_value->value));
        const WotbModResult live =
            CallUiGetText(ui->resource, out_value->value, &size);
        if (live == WOTBMOD_ERROR_NOT_FOUND) {
            out_value->value[0] = '\0';
            out_value->length = 0u;
            return WOTBMOD_V3_OK;
        }
        if (live != WOTBMOD_OK) {
            out_value->value[0] = '\0';
            return ConvertResult(live);
        }
        if (size >= sizeof(out_value->value)) return WOTBMOD_V3_E_PLATFORM;
        out_value->value[size] = '\0';
        out_value->length = size;
        out_value->flags = WOTBMOD_V3_UI_READ_LIVE_TEXT_SET;
        return WOTBMOD_V3_OK;
    }
    uint32_t set_flag = 0u;
    const char* mirrored =
        MirrorStringForField(*ui, request->field, &set_flag);
    if (!mirrored || set_flag == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if ((ui->mirror_flags & set_flag) == 0u) {
        /*
         * Never written. The empty value plus the CLEAR bit is the answer;
         * the runtime turns it into WOTBMOD_V3_E_NOT_FOUND for the mod. This
         * is not an error at this boundary and must not be reported as one.
         */
        return WOTBMOD_V3_OK;
    }
    const size_t length = std::strlen(mirrored);
    if (length >= sizeof(out_value->value)) {
        /*
         * Cannot happen through the setters, which bound every field; if it
         * ever does, the honest answer is a host fault rather than a silently
         * cut string the mod would read back as the whole value.
         */
        return WOTBMOD_V3_E_PLATFORM;
    }
    std::memcpy(out_value->value, mirrored, length + 1u);
    out_value->length = static_cast<uint32_t>(length);
    out_value->flags = set_flag;
    return WOTBMOD_V3_OK;
}

WotbModV3Result V3NativeUiReadStyle(
    void*,
    const v3::ClientHostUiReadRequest* request,
    WotbModV3UiStyleSnapshot* out_style) {
    if (!request || !out_style ||
        request->struct_size < sizeof(*request) ||
        request->api_version !=
            WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION ||
        request->object == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ScopedServiceInvocation invocation;
    if (!invocation.active()) return WOTBMOD_V3_E_NOT_SUPPORTED;

    std::lock_guard<std::mutex> lock(g_services.mutex);
    NativeUi* ui = FindUiLocked(request->object, request->mod);
    if (!ui) return WOTBMOD_V3_E_NOT_FOUND;

    /*
     * Only the five style bits. WOTBMOD_V3_UI_READ_GAME_OWNED is the
     * runtime's to set, and the three string bits belong to the string
     * getters; reporting them here would be answering a question this slot
     * was not asked.
     */
    const uint32_t style_flags =
        WOTBMOD_V3_UI_READ_COLOR_SET |
        WOTBMOD_V3_UI_READ_BACKGROUND_COLOR_SET |
        WOTBMOD_V3_UI_READ_OPACITY_SET |
        WOTBMOD_V3_UI_READ_FONT_SIZE_SET |
        WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;

    out_style->struct_size = sizeof(*out_style);
    out_style->api_version = WOTBMOD_V3_UI_VERSION_4;
    out_style->flags = ui->mirror_flags & style_flags;
    /*
     * Values whose flag is clear stay at the mirror's construction defaults
     * rather than being zeroed: ui_v4.h documents them as "not a
     * measurement", and zero is itself a legal opacity and a legal colour.
     */
    out_style->text_alignment = ui->text_alignment;
    out_style->text_wrap = ui->text_wrap;
    out_style->rich_text = ui->rich_text;
    out_style->opacity = ui->opacity;
    out_style->font_size = ui->font_size;
    out_style->color = ui->color;
    out_style->background_color = ui->background_color;
    return WOTBMOD_V3_OK;
}

WotbModV3Result V3NativeSceneWalkLimits(
    void*,
    WotbModV3SceneWalkLimits* out_limits) {
    if (!out_limits) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ScopedServiceInvocation invocation;
    if (!invocation.active()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (!SceneWalkAvailable()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    out_limits->struct_size = sizeof(*out_limits);
    out_limits->api_version = WOTBMOD_V3_SCENE_VERSION_2;
    out_limits->max_depth = kSceneWalkMaxDepth;
    out_limits->max_children_per_node = kSceneWalkMaxChildren;
    out_limits->max_nodes = kSceneWalkMaxNodes;
    out_limits->node_record_size =
        static_cast<uint32_t>(sizeof(WotbModV3SceneNodeRecord));
    return WOTBMOD_V3_OK;
}

WotbModV3Result V3NativeSceneWalkActive(
    void*,
    const WotbModV3SceneWalkRequest* request,
    WotbModV3SceneNodeRecord* out_nodes,
    uint32_t node_capacity,
    uint32_t* out_reached) {
    if (!request || !out_reached ||
        request->struct_size < sizeof(*request) ||
        request->api_version != WOTBMOD_V3_SCENE_VERSION_2 ||
        (node_capacity != 0u && !out_nodes)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_reached = 0u;
    /*
     * THREAD FIRST, before a single engine pointer is read - and before the
     * active scene is even asked for. The runtime already refused every
     * other thread; the backend checks again because trusting a caller on
     * this particular question is a use-after-free, and because "no frame
     * has been observed yet" has to fail the same way as "wrong thread".
     */
    if (!IsV3NativeClientServicesMainThread()) {
        return WOTBMOD_V3_E_WRONG_THREAD;
    }
    ScopedServiceInvocation invocation;
    if (!invocation.active()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (!SceneWalkAvailable()) return WOTBMOD_V3_E_NOT_SUPPORTED;

    const uint32_t max_depth =
        request->max_depth != 0u && request->max_depth <= kSceneWalkMaxDepth
            ? request->max_depth
            : kSceneWalkMaxDepth;
    const uint32_t max_children =
        request->max_children_per_node != 0u &&
                request->max_children_per_node <= kSceneWalkMaxChildren
            ? request->max_children_per_node
            : kSceneWalkMaxChildren;
    const uint32_t max_nodes =
        request->max_nodes != 0u && request->max_nodes <= kSceneWalkMaxNodes
            ? request->max_nodes
            : kSceneWalkMaxNodes;

    /*
     * scene_get_active validates the Scene vtable slot and the active byte
     * and RETAINS the object, so the root cannot be released underneath the
     * walk. Everything reached from it is protected only by the thread rule.
     */
    void* resource = nullptr;
    const WotbModResult acquired = CallResourceCreate(
        g_services.resource.scene_get_active, &resource);
    if (acquired != WOTBMOD_OK || !resource) {
        if (resource) CallResourceRelease(resource);
        /*
         * No active scene is the normal state during login, loading and on
         * UI-only screens. scene_v2.h requires NOT_FOUND, never
         * NOT_SUPPORTED, so a mod can tell "not now" from "not on this
         * build".
         */
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    const uint8_t* root =
        static_cast<const uint8_t*>(CallResourceIdentity(resource));
    const void* scene_vtable =
        SceneImageAddress(kDefaultSceneVtableRva);
    const void* entity_vtable =
        SceneImageAddress(kDefaultEntityVtableRva);
    const void* root_vtable = nullptr;
    if (!root ||
        !SceneSafeRead(root, &root_vtable, sizeof(root_vtable)) ||
        (root_vtable != scene_vtable && root_vtable != entity_vtable)) {
        /*
         * Fail closed. The identity of the root is the one thing the walk
         * cannot recover from being wrong about, so an object that is not
         * one of the two known containers is reported as "no scene" rather
         * than walked on the assumption that it is laid out like one.
         */
        CallResourceRelease(resource);
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    WotbModV3Result result = WOTBMOD_V3_OK;
    std::vector<SceneWalkNode> queue;
    try {
        queue.reserve(64u);
        queue.push_back({root, WOTBMOD_V3_SCENE_NODE_NO_PARENT, 0u});
    } catch (...) {
        CallResourceRelease(resource);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }

    uint32_t reached = 0u;
    for (size_t cursor = 0u;
         cursor < queue.size() && reached < max_nodes;
         ++cursor) {
        const SceneWalkNode node = queue[cursor];
        const uint32_t index = reached++;

        const uint8_t* children = nullptr;
        uint32_t child_count = 0u;
        const bool children_ok =
            ReadSceneChildren(node.entity, &children, &child_count);
        if (!children_ok) {
            children = nullptr;
            child_count = 0u;
        }
        const bool depth_limited =
            child_count != 0u && node.depth + 1u > max_depth;

        if (index < node_capacity) {
            FillSceneNodeRecord(
                node.entity,
                index,
                node.parent_index,
                node.depth,
                child_count,
                max_children,
                depth_limited,
                !children_ok,
                scene_vtable,
                &out_nodes[index]);
        }
        if (depth_limited || child_count == 0u || !children) continue;

        const uint32_t emitted = child_count < max_children
            ? child_count
            : max_children;
        for (uint32_t child = 0u; child < emitted; ++child) {
            if (queue.size() >= max_nodes) break;
            const uint8_t* entry = nullptr;
            if (!SceneSafeRead(
                    children + child * sizeof(void*),
                    &entry,
                    sizeof(entry)) ||
                entry == nullptr) {
                continue;
            }
            try {
                queue.push_back({entry, index, node.depth + 1u});
            } catch (...) {
                result = WOTBMOD_V3_E_LIMIT_REACHED;
                break;
            }
        }
        if (result != WOTBMOD_V3_OK) break;
    }

    CallResourceRelease(resource);
    if (result != WOTBMOD_V3_OK) return result;
    /*
     * `reached` is what the walk reached, which may exceed node_capacity.
     * Turning that into WOTBMOD_V3_E_BUFFER_TOO_SMALL is the runtime's job;
     * a short write is not an error at this boundary.
     */
    *out_reached = reached;
    return WOTBMOD_V3_OK;
}

void ShutdownV3NativeClientServices() {
    if (g_service_shutdown_running) return;
    if (g_service_invocation_depth > 0u ||
        g_ui_operation_depth > 0u) {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.initialized = false;
        g_services.shutdown_requested = true;
        g_services.operations_finished.notify_all();
        return;
    }

    std::vector<NativeUi*> ui_objects;
    std::vector<NativeScene*> scenes;
    std::vector<NativeAudio*> audio_objects;
    std::vector<TransientSceneAttachment> transient;
    std::vector<RetiredUiResource> retired_ui_resources;
    {
        std::unique_lock<std::mutex> lock(g_services.mutex);
        if (g_services.shutdown_running) {
            g_services.operations_finished.wait(
                lock,
                []() { return !g_services.shutdown_running; });
            return;
        }
        if (!g_services.initialized &&
            !g_services.shutdown_requested) {
            return;
        }
        g_services.initialized = false;
        g_services.shutdown_requested = true;
        g_services.shutdown_running = true;
        g_services.operations_finished.wait(
            lock,
            []() {
                return g_services.active_service_invocations == 0u &&
                       g_services.active_ui_operations == 0u;
            });
        ui_objects.swap(g_services.ui_objects);
        scenes.swap(g_services.scenes);
        audio_objects.swap(g_services.audio_objects);
        transient.swap(g_services.transient_scenes);
        retired_ui_resources.swap(
            g_services.retired_ui_resources);
    }
    g_service_shutdown_running = true;
    for (const auto& attachment : transient) {
        if (attachment.active_scene_resource &&
            attachment.child_resource) {
            CallScenePair(
                g_services.resource.scene_remove_child,
                attachment.active_scene_resource,
                attachment.child_resource);
        }
        if (attachment.active_scene_resource) {
            CallResourceRelease(attachment.active_scene_resource);
        }
    }
    for (const RetiredUiResource& retired : retired_ui_resources) {
        if (retired.resource) CallResourceRelease(retired.resource);
    }
    for (NativeAudio* audio : audio_objects) {
        if (!audio) continue;
        if (audio->kind == AudioKind::Clip) {
            if (audio->playback) {
                CallAudioOperation(
                    g_services.audio.release,
                    audio->playback);
            }
            if (audio->clip) {
                CallAudioClipOperation(
                    g_services.audio.release_clip,
                    audio->clip);
            }
        } else if (audio->sound_event) {
            CallSoundOperation(
                g_services.sound.release,
                audio->sound_event);
        }
        audio->magic = 0u;
        delete audio;
    }
    for (NativeUi* ui : ui_objects) {
        if (!ui) continue;
        if (ui->parent &&
            g_services.resource.ui_remove_child) {
            CallUiPair(
                g_services.resource.ui_remove_child,
                UiAttachmentResource(ui->parent),
                ui->resource);
        } else if (ui->slot_parent_resource &&
                   g_services.resource.ui_remove_child) {
            CallUiPair(
                g_services.resource.ui_remove_child,
                ui->slot_parent_resource,
                ui->resource);
        }
    }
    for (NativeUi* ui : ui_objects) {
        if (!ui) continue;
        if (ui->slot_parent_resource) {
            CallResourceRelease(ui->slot_parent_resource);
        }
        if (ui->content_resource) {
            CallResourceRelease(ui->content_resource);
        }
        if (ui->resource) {
            CallResourceRelease(ui->resource);
        }
        ui->magic = 0u;
        delete ui;
    }
    for (NativeScene* scene : scenes) {
        if (!scene) continue;
        if (scene->resource) {
            CallResourceRelease(scene->resource);
        }
        scene->magic = 0u;
        delete scene;
    }
    std::memset(
        &g_services.resource, 0, sizeof(g_services.resource));
    std::memset(
        &g_services.audio, 0, sizeof(g_services.audio));
    std::memset(
        &g_services.sound, 0, sizeof(g_services.sound));
    g_services.resource_identity = nullptr;
    g_services.resource_identity_user_data = nullptr;
    g_services.resource_bring_ui_to_front = nullptr;
    g_services.resource_bring_ui_to_front_user_data = nullptr;
    g_services.game_base = nullptr;
    g_services.game_image_size = 0u;
    g_services.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
    /*
     * Forget the observed main thread. The detour that proved it is about to
     * be removed, so from here the walk must refuse everyone again rather
     * than trust an identity nothing is republishing.
     */
    InterlockedExchange(&g_main_thread_id, 0);
    g_service_shutdown_running = false;
    {
        std::lock_guard<std::mutex> lock(g_services.mutex);
        g_services.shutdown_running = false;
        g_services.shutdown_requested = false;
        g_services.ui_frame_epoch = 0u;
        g_services.ui_mutation_resume_frame = 0u;
        g_services.ui_input_active = false;
        g_services.operations_finished.notify_all();
    }
}

}  // namespace loader
}  // namespace wotbmod
