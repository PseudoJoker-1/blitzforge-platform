#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmath>
#include <new>
#include <string>
#include <vector>
#include <atomic>

#include "../include/wotb_mod_dava_sound.h"
#include "../include/wotb_mod_dava_resources.h"
#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotb_mod_windows_audio.h"
#include "../include/wotbmod/client_v1.h"
#include "../src/v3/client_services_backend.h"
#include "../src/v3/dava_native_registry.h"
#include "../src/v3/wotb_mod_v3_internal.h"
#include "v3_native_bindings.h"
#include "v3_native_client_services.h"
#include "../../proxy_dll/third_party/minhook/include/MinHook.h"
#include "anchor_rvas.h"
#include "v3_native_ges.h"
#include "v3_native_session_cluster.h"
#include "../src/v3/session_cluster_services.h"
#include "v3_hook_observers.h"
namespace hobs = wotbmod::loader::observers;
#include "../src/v3/ges_services.h"
#include "../src/v3/client_services_backend.h"

#ifndef WOTBMOD_DEVELOPER_UNSAFE_LEGACY_API
#define WOTBMOD_DEVELOPER_UNSAFE_LEGACY_API 0
#endif

namespace {

HMODULE g_loaderModule = nullptr;
HANDLE g_logHandle = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
volatile LONG g_runtimeReady = 0;
volatile LONG g_presentWarmupFrames = 0;
volatile LONG g_dispatchFrameSerial = 0;
volatile LONG g_davaNativeLiveProbeState = 0;
volatile LONG g_davaNativeTracerLiveProbeState = 0;
volatile LONG g_davaNativeLiveProbeAttempts = 0;
volatile LONG g_battleContextActive = 0;
volatile LONG g_uiContextProbePending = 1;
volatile LONG g_uiDerivedContextMask = WOTBMOD_V3_CONTEXT_NONE;
volatile LONG g_lastLoggedUiContextMask = -1;
volatile LONG g_uiInputDpi = USER_DEFAULT_SCREEN_DPI;
LARGE_INTEGER g_lastFrameCounter = {};
LARGE_INTEGER g_counterFrequency = {};
WotbModResult g_audioBackendResult = WOTBMOD_ERROR_PLATFORM;
WotbModResult g_soundBackendResult = WOTBMOD_ERROR_PLATFORM;
WotbModResult g_resourceBackendResult = WOTBMOD_ERROR_PLATFORM;
WotbModV3Result g_v3NativeBindingsResult = WOTBMOD_V3_E_NOT_SUPPORTED;
/*
 * Set once, before the first fixed-RVA detour is installed. Every binding
 * derived from a hardcoded RVA is only valid for the exact client build the
 * pack was generated from, so this gate - not the later Create() call - is
 * what keeps a detour off a build we did not generate for.
 */
bool g_nativeFingerprintVerified = false;
WotbModRuntimeV3ClientBackend g_v3ClientBackend = {};
WotbModDavaResourcesHandle g_davaResourcesHandle = nullptr;
WotbModRuntimeResourceBackend g_liveResourceBackend = {};
void* g_lastUiScreenResource = nullptr;
void* g_lastUiScreenObject = nullptr;
void* g_cachedCatalogMarkerResource = nullptr;
WotbModRuntimeResourceRegistryChanged g_forwardResourceRegistryChanged =
    nullptr;
volatile LONG g_uiResourceRegistryChanged = 0;
PVOID volatile g_lastNotifiedScene = nullptr;
volatile LONG g_sceneDrawHits = 0;
volatile LONG g_sceneActivationHits = 0;
SRWLOCK g_vehicleLock = SRWLOCK_INIT;
volatile LONG g_localVehicleEntityId = 0;
volatile LONG g_gameplayHooksInstalled = 0;
volatile LONG g_vehicleTeamVerifiedLogged = 0;
volatile LONG g_vehicleTeamRejectedLogged = 0;
volatile LONG g_confirmedAllyHighWater = 0;
volatile LONG g_avatarFallbackResolved = 0;
uint64_t g_eventSourceMask = 0u;

/* GES::GameEventSystem::SubscribeImpl - thiscall, 0x30 bytes of arguments:
 * owner, type_index and a std::function by value (0x28 bytes = 10 dwords). */
typedef void(__thiscall* GesSubscribeImplFn)(
    void* bus, const void* owner, const void* typeIndex,
    uint32_t f0, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t f4,
    uint32_t f5, uint32_t f6, uint32_t f7, uint32_t f8, uint32_t f9);
GesSubscribeImplFn g_originalGesSubscribeImpl = nullptr;
uint8_t* g_gesSubscribeImplTarget = nullptr;
bool g_gesInstalled = false;
/* Defined below DllMain's neighbours; handed to the bindings unit as the
 * declared-backend ges_* slots once the bus hook is in. */
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesListTypes(
    void*, wotbmod::v3::ClientHostGesTypeVisitFn visit, void* data);
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesObserve(
    void*, const char* typeName, uint32_t observe);
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesPublish(
    void*, const char* typeName, const void* payload, uint32_t size, uint32_t flags);

/* LoginManager::OnHostChosen - thiscall(int, int, ClusterHost*), called on
 * every login with the host the client picked; captured for
 * wotbmod.session.cluster (loader/v3_native_session_cluster.h). */
typedef void(__fastcall* LoginOnHostChosenFn)(
    void* self, void* edx, int a2, int a3, void* host);
LoginOnHostChosenFn g_originalLoginOnHostChosen = nullptr;
uint8_t* g_loginOnHostChosenTarget = nullptr;
bool g_sessionClusterInstalled = false;
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterEnumerate(
    void*, WotbModV3ClusterInfo* items, uint32_t* inoutCount);
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterGetCurrent(
    void*, WotbModV3ClusterInfo* outInfo);
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterChange(
    void*, int32_t clusterId);
extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterSetManual(
    void*, uint32_t manual);

const uint32_t kVehicleTokenMagic = 0x4B4F5456u; /* VTOK */
const uint32_t kMaxLiveVehicles = 128u;
const uint32_t kVehicleEntityIdOffset = 0x1Cu;
const uint32_t kVehicleHealthOffset = 0xB8u;
const uint32_t kVehicleMaxHealthOffset = 0x11Cu;
const uint32_t kVehiclePrimaryVtableRva = 0x033056A4u;
const uint32_t kVehicleObjectEntityIdOffset = 0x1Cu;
const uint32_t kVehicleObjectTeamOffset = 0xB0u;
const uint64_t kGameplayEventSourceMask =
    WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD |
    WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD |
    WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
    WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH |
    WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE |
    WOTBMOD_V3_EVENT_SOURCE_RELOAD_STATE |
    WOTBMOD_V3_EVENT_SOURCE_UI_INPUT |
    WOTBMOD_V3_EVENT_SOURCE_AMMO_CHANGED |
    WOTBMOD_V3_EVENT_SOURCE_AIM_TARGET |
    WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS |
    WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT |
    WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE |
    WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED;

static void WriteLogLine(const char* prefix, const char* message);

/* Frame profile accumulators (2026-09-05): QPC ticks spent inside the
 * loader's own hooks since the last hitch report, so a slow frame can be
 * charged to the loader or to the engine from the log alone. */
extern "C" {
/* the engine/pump counters are defined in v3_native_bindings.cpp next to the
 * EngineUpdateAndDraw detour that feeds them, so the gameplay bridge test
 * links without this file */
extern volatile LONGLONG g_wotbProfEngineTicks;
extern volatile LONGLONG g_wotbProfPumpTicks;
volatile LONGLONG g_wotbProfSceneDrawTicks = 0;
volatile LONGLONG g_wotbProfFileCreateTicks = 0;
volatile LONGLONG g_wotbProfFileCreateCalls = 0;
extern volatile LONGLONG g_wotbProfEngineCalls;
volatile LONGLONG g_wotbProfUiInputTicks = 0;
volatile LONGLONG g_wotbProfUiInputCalls = 0;
/* the two GES counters are defined in v3_native_ges.cpp, which the GES
 * native test links without this file */
extern volatile LONGLONG g_wotbProfGesTicks;
extern volatile LONGLONG g_wotbProfGesCalls;
volatile LONGLONG g_wotbProfPresentHookTicks = 0;
volatile LONGLONG g_wotbProfPollUiTicks = 0;
volatile LONGLONG g_wotbProfUpdateFrameTicks = 0;
volatile LONGLONG g_wotbProfGesRetryTicks = 0;
volatile LONGLONG g_wotbProfDispatchTicks = 0;
volatile LONGLONG g_wotbProfPollGetScreenTicks = 0;
volatile LONGLONG g_wotbProfPollUpdateTicks = 0;
volatile LONGLONG g_wotbProfPollUpdateCalls = 0;
volatile LONGLONG g_wotbProfPollUpdateFullCalls = 0;
volatile LONGLONG g_wotbProfPollApplyTicks = 0;
volatile LONGLONG g_wotbProfPollReleaseTicks = 0;
}
static LONGLONG ProfNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
static void ProfAdd(volatile LONGLONG* slot, LONGLONG ticks) {
    InterlockedExchangeAdd64(slot, ticks);
}

struct LiveVehicleRecord {
    uint32_t entity_id;
    int32_t health;
    int32_t max_health;
    uint32_t team;
    uint32_t flags;
    void* game_logic;
    void* entity;
    bool visible_to_player;
    bool confirmed_ally;
    bool active;
    /* World pose from the PlayerController pose block; only the controlled
     * vehicle gets one, and it keeps the last value once the hull is dead. */
    bool pose_valid;
    float position[3];
    float direction[3];
};

struct VehicleSnapshotToken {
    uint32_t magic;
    WotbModVehicleInfo info;
};

LiveVehicleRecord g_liveVehicles[kMaxLiveVehicles] = {};

/*
 * Arena roster: what ClientArena knows about every player of the battle from
 * the server's VehicleInfo packets, keyed by vehicle id. Filled by the
 * ClientArena hooks (anchor comments in anchor_rvas.h), merged into the
 * public snapshot by CopyNativePublicVehicleLocked. Vehicle ids are unique
 * across battles, so stale rows are simply recycled when the table is full.
 */
const uint32_t kMaxArenaPlayers = 64u;
struct ArenaPlayerRecord {
    uint32_t vehicle_id;
    int32_t kills;
    bool has_kills;
    bool alive;
    bool has_name;
    int64_t account_id;
    char name[WOTBMOD_V3_MAX_NAME];
    char clan_tag[32];
    char vehicle_name[WOTBMOD_V3_MAX_NAME];
    char vehicle_display_name[WOTBMOD_V3_MAX_NAME];
};
static ArenaPlayerRecord g_arenaPlayers[kMaxArenaPlayers] = {};
static uint32_t g_arenaPlayerNext = 0u;
static SRWLOCK g_arenaLock = SRWLOCK_INIT;

static ArenaPlayerRecord* FindArenaPlayerLocked(uint32_t vehicleId) {
    if (vehicleId == 0u) return nullptr;
    for (uint32_t index = 0u; index < kMaxArenaPlayers; ++index) {
        if (g_arenaPlayers[index].vehicle_id == vehicleId) {
            return &g_arenaPlayers[index];
        }
    }
    return nullptr;
}

static ArenaPlayerRecord* ReserveArenaPlayerLocked(uint32_t vehicleId) {
    ArenaPlayerRecord* existing = FindArenaPlayerLocked(vehicleId);
    if (existing) return existing;
    for (uint32_t index = 0u; index < kMaxArenaPlayers; ++index) {
        if (g_arenaPlayers[index].vehicle_id == 0u) {
            g_arenaPlayers[index] = {};
            g_arenaPlayers[index].vehicle_id = vehicleId;
            return &g_arenaPlayers[index];
        }
    }
    ArenaPlayerRecord* slot = &g_arenaPlayers[g_arenaPlayerNext];
    g_arenaPlayerNext = (g_arenaPlayerNext + 1u) % kMaxArenaPlayers;
    *slot = {};
    slot->vehicle_id = vehicleId;
    return slot;
}

typedef void(__thiscall* VehicleVoidFn)(void*);
typedef void(__thiscall* VehicleShootingFn)(
    void*, const uint8_t*);
typedef void(__thiscall* VehicleSetHealthFn)(
    void*, const int16_t*);
typedef void(__thiscall* AvatarUpdateHealthFn)(
    void*, const int16_t*, const uint8_t*);
typedef void(__thiscall* ReloadSetStateFn)(
    void*, int32_t, float);
typedef bool(__thiscall* UiSystemInputFn)(void*, const void*);
typedef int32_t(__thiscall* AmmoChangedFn)(
    void*, int32_t, int32_t);
typedef void(__thiscall* AimTargetSetFn)(
    void*, const uint32_t*, uint8_t);
typedef void(__thiscall* AimTargetClearFn)(void*, uint8_t);
typedef void(__thiscall* ObservedStatusFn)(void*, void*);
typedef void*(__cdecl* ObservedPayloadGetterFn)(void);
typedef uint8_t(__thiscall* VehicleHitDamageFn)(
    void*,
    uint32_t,
    uint32_t,
    const float*,
    const void*,
    const void*,
    uint8_t,
    uint8_t,
    uint32_t);
/* __thiscall(this, float delta) reached through a __fastcall thunk: ecx is
 * `this`, edx is scratch, the delta stays on the stack and both conventions
 * let the callee pop it. */
typedef void(__fastcall* PoseUpdateFn)(void* self, void* edx, float delta);
/* ClientArena handlers, all __thiscall(this, pointer) behind __fastcall thunks */
typedef void*(__fastcall* ArenaAddVehicleInfoFn)(void* self, void* edx, void* info);
typedef void(__fastcall* ArenaApplyStatsFn)(void* self, void* edx, void* stats);
typedef void(__fastcall* ArenaVehicleKilledFn)(void* self, void* edx, void* message);
typedef void(__fastcall* ArenaPlayerNameFn)(void* self, void* edx, void* message);

struct DavaFilePath32 {
    std::string absolute_pathname;
    int32_t path_type;

    explicit DavaFilePath32(const char* path)
        : absolute_pathname(path ? path : ""), path_type(0) {
    }
};

#if defined(_M_IX86)
static_assert(sizeof(std::string) == 24u, "DAVA String ABI changed");
static_assert(sizeof(DavaFilePath32) == 28u, "DAVA FilePath ABI changed");
#endif

typedef void*(__cdecl* DavaFileCreateFn)(
    const DavaFilePath32*, uint32_t);

VehicleVoidFn g_originalOnEnterWorld = nullptr;
VehicleVoidFn g_originalOnLeaveWorld = nullptr;
VehicleShootingFn g_originalShowShooting = nullptr;
VehicleSetHealthFn g_originalSetHealth = nullptr;
AvatarUpdateHealthFn g_originalUpdateVehicleHealth = nullptr;
ReloadSetStateFn g_originalReloadSetState = nullptr;
UiSystemInputFn g_originalUiSystemInput = nullptr;
AmmoChangedFn g_originalAmmoChanged = nullptr;
AimTargetSetFn g_originalAimTargetSet = nullptr;
AimTargetClearFn g_originalAimTargetClear = nullptr;
ObservedStatusFn g_originalObservedStatus = nullptr;
VehicleHitDamageFn g_originalVehicleHitDamage = nullptr;
PoseUpdateFn g_originalPoseUpdate = nullptr;
ArenaAddVehicleInfoFn g_originalArenaAddVehicleInfo = nullptr;
ArenaApplyStatsFn g_originalArenaApplyStats = nullptr;
ArenaVehicleKilledFn g_originalArenaVehicleKilled = nullptr;
ArenaPlayerNameFn g_originalArenaPlayerName = nullptr;
DavaFileCreateFn g_originalDavaFileCreate = nullptr;

void* g_onEnterWorldTarget = nullptr;
void* g_onLeaveWorldTarget = nullptr;
void* g_showShootingTarget = nullptr;
void* g_setHealthTarget = nullptr;
void* g_updateVehicleHealthTarget = nullptr;
void* g_reloadSetStateTarget = nullptr;
void* g_uiSystemInputTarget = nullptr;
void* g_ammoChangedTarget = nullptr;
void* g_aimTargetSetTarget = nullptr;
void* g_aimTargetClearTarget = nullptr;
void* g_observedStatusTarget = nullptr;
void* g_vehicleHitDamageTarget = nullptr;
void* g_poseUpdateTarget = nullptr;
void* g_arenaAddVehicleInfoTarget = nullptr;
void* g_arenaApplyStatsTarget = nullptr;
void* g_arenaVehicleKilledTarget = nullptr;
void* g_arenaPlayerNameTarget = nullptr;
void* g_davaFileCreateTarget = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags);
PresentFn g_originalPresent = nullptr;
typedef void(__thiscall* SceneDrawFn)(void* scene);
SceneDrawFn g_originalSceneDraw = nullptr;
SceneDrawFn g_originalSceneActivate = nullptr;
SceneDrawFn g_originalSceneDeactivate = nullptr;
void* g_sceneDrawTarget = nullptr;
void* g_sceneActivateTarget = nullptr;
void* g_sceneDeactivateTarget = nullptr;

static uint32_t ActiveVehicleCountLocked() {
    uint32_t count = 0;
    for (uint32_t index = 0; index < kMaxLiveVehicles; ++index) {
        if (g_liveVehicles[index].active) ++count;
    }
    return count;
}

static LiveVehicleRecord* FindVehicleLocked(uint32_t entityId) {
    if (entityId == 0) return nullptr;
    for (uint32_t index = 0; index < kMaxLiveVehicles; ++index) {
        if (g_liveVehicles[index].entity_id == entityId) {
            return &g_liveVehicles[index];
        }
    }
    return nullptr;
}

static LiveVehicleRecord* ReserveVehicleLocked(uint32_t entityId) {
    LiveVehicleRecord* existing = FindVehicleLocked(entityId);
    if (existing) return existing;
    for (uint32_t index = 0; index < kMaxLiveVehicles; ++index) {
        if (g_liveVehicles[index].entity_id == 0) {
            g_liveVehicles[index].entity_id = entityId;
            return &g_liveVehicles[index];
        }
    }
    for (uint32_t index = 0; index < kMaxLiveVehicles; ++index) {
        if (!g_liveVehicles[index].active) {
            ZeroMemory(
                &g_liveVehicles[index],
                sizeof(g_liveVehicles[index]));
            g_liveVehicles[index].entity_id = entityId;
            return &g_liveVehicles[index];
        }
    }
    return nullptr;
}

static void FillVehicleInfoLocked(
    const LiveVehicleRecord* record,
    WotbModVehicleInfo* outInfo) {
    ZeroMemory(outInfo, sizeof(*outInfo));
    outInfo->struct_size = sizeof(*outInfo);
    if (!record) return;
    outInfo->entity_id = record->entity_id;
    outInfo->flags = record->flags;
    if (record->active) {
        outInfo->flags |= WOTBMOD_VEHICLE_ALIVE;
    } else {
        outInfo->flags &= ~WOTBMOD_VEHICLE_ALIVE;
    }
    if (record->entity_id ==
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0))) {
        outInfo->flags |= WOTBMOD_VEHICLE_LOCAL;
    } else {
        outInfo->flags &= ~WOTBMOD_VEHICLE_LOCAL;
    }
    outInfo->health = record->health;
    outInfo->max_health = record->max_health;
}

static bool IsVehiclePublicLocked(
    const LiveVehicleRecord* record) {
    if (!record || !record->active) return false;
    const uint32_t localId =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    uint32_t publicFlags =
        record->visible_to_player
            ? WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE
            : 0u;
    if (record->entity_id == localId) {
        publicFlags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL;
    }
    if (record->confirmed_ally) {
        publicFlags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY;
    }
    return WotbModV3NativeBindings_IsPublicVehicleFlags(
               publicFlags) != 0;
}

static uint32_t PublicVehicleCountLocked() {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < kMaxLiveVehicles; ++index) {
        if (IsVehiclePublicLocked(&g_liveVehicles[index])) {
            ++count;
        }
    }
    return count;
}

static bool CopyNativePublicVehicleLocked(
    const LiveVehicleRecord* record,
    WotbModV3NativePublicVehicle* outVehicle) {
    if (!outVehicle || !IsVehiclePublicLocked(record)) {
        return false;
    }
    ZeroMemory(outVehicle, sizeof(*outVehicle));
    outVehicle->struct_size = sizeof(*outVehicle);
    outVehicle->public_id = record->entity_id;
    outVehicle->health = record->health > 0
                             ? record->health
                             : 0;
    outVehicle->max_health =
        record->max_health > outVehicle->health
            ? record->max_health
            : outVehicle->health;
    outVehicle->valid_fields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE;
    if (record->team == 1u || record->team == 2u) {
        outVehicle->team = record->team;
        outVehicle->valid_fields |=
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM;
    }
    if (record->pose_valid) {
        memcpy(outVehicle->position, record->position,
               sizeof(outVehicle->position));
        memcpy(outVehicle->direction, record->direction,
               sizeof(outVehicle->direction));
        outVehicle->valid_fields |=
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION |
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION;
    }
    /* lock order: g_vehicleLock, then g_arenaLock; the arena hooks never
     * hold g_arenaLock while publishing */
    AcquireSRWLockShared(&g_arenaLock);
    const ArenaPlayerRecord* player =
        FindArenaPlayerLocked(record->entity_id);
    if (player) {
        if (player->has_name && player->name[0]) {
            strcpy_s(outVehicle->display_name, sizeof(outVehicle->display_name),
                     player->name);
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME;
        }
        if (player->account_id != 0) {
            outVehicle->account_id = player->account_id;
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ACCOUNT_ID;
        }
        if (player->has_kills) {
            outVehicle->kills = player->kills;
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS;
        }
        /* the roster row itself is the source: a player without a clan gets
         * an empty tag, not NOT_SUPPORTED */
        if (player->has_name) {
            strcpy_s(outVehicle->clan_tag, sizeof(outVehicle->clan_tag),
                     player->clan_tag);
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG;
        }
        if (player->vehicle_name[0]) {
            strcpy_s(outVehicle->vehicle_name, sizeof(outVehicle->vehicle_name),
                     player->vehicle_name);
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_NAME;
        }
        if (player->vehicle_display_name[0]) {
            strcpy_s(outVehicle->vehicle_display_name,
                     sizeof(outVehicle->vehicle_display_name),
                     player->vehicle_display_name);
            outVehicle->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME;
        }
    }
    ReleaseSRWLockShared(&g_arenaLock);
    outVehicle->native_entity = record->entity;
    outVehicle->native_game_logic = record->game_logic;
    if (record->visible_to_player) {
        outVehicle->flags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE;
    }
    if (record->confirmed_ally) {
        outVehicle->flags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY;
    }
    if (record->entity_id ==
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0))) {
        outVehicle->flags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL;
    }
    if ((record->flags & WOTBMOD_VEHICLE_DESTROYED) != 0u ||
        record->health <= 0) {
        outVehicle->flags |=
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_DESTROYED;
    }
    return true;
}

static bool IsPublicVehicleId(uint32_t entityId) {
    if (!entityId) return false;
    bool isPublic = false;
    AcquireSRWLockShared(&g_vehicleLock);
    const LiveVehicleRecord* record =
        FindVehicleLocked(entityId);
    isPublic = IsVehiclePublicLocked(record);
    ReleaseSRWLockShared(&g_vehicleLock);
    return isPublic;
}

static WotbModV3Result UpsertPublicVehicle(
    uint32_t entityId,
    uint32_t reason) {
    WotbModV3NativePublicVehicle vehicle = {};
    bool copied = false;
    AcquireSRWLockShared(&g_vehicleLock);
    const LiveVehicleRecord* record =
        FindVehicleLocked(entityId);
    copied = CopyNativePublicVehicleLocked(record, &vehicle);
    ReleaseSRWLockShared(&g_vehicleLock);
    return copied
               ? WotbModV3NativeBindings_UpsertPublicVehicle(
                     &vehicle, reason)
               : WOTBMOD_V3_E_NOT_FOUND;
}

static void RefreshConfirmedAllies(
    uint32_t localEntityId,
    uint32_t reason) {
    uint32_t publishIds[kMaxLiveVehicles] = {};
    uint32_t removeIds[kMaxLiveVehicles] = {};
    uint32_t publishCount = 0u;
    uint32_t removeCount = 0u;
    uint32_t confirmedCount = 0u;
    uint32_t localTeam = 0u;

    AcquireSRWLockExclusive(&g_vehicleLock);
    const LiveVehicleRecord* local =
        FindVehicleLocked(localEntityId);
    if (local && local->active &&
        (local->team == 1u || local->team == 2u)) {
        localTeam = local->team;
    }
    for (uint32_t index = 0u; index < kMaxLiveVehicles; ++index) {
        LiveVehicleRecord& record = g_liveVehicles[index];
        const bool wasConfirmed = record.confirmed_ally;
        const bool isConfirmed =
            record.active && localTeam != 0u &&
            record.team == localTeam;
        record.confirmed_ally = isConfirmed;
        if (isConfirmed) ++confirmedCount;
        if (isConfirmed && !wasConfirmed &&
            publishCount < kMaxLiveVehicles) {
            publishIds[publishCount++] = record.entity_id;
        } else if (wasConfirmed && !isConfirmed &&
                   !record.visible_to_player &&
                   record.entity_id != localEntityId &&
                   removeCount < kMaxLiveVehicles) {
            removeIds[removeCount++] = record.entity_id;
        }
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);

    if (localTeam == 0u) {
        InterlockedExchange(&g_confirmedAllyHighWater, 0);
    } else {
        LONG previousHighWater = InterlockedCompareExchange(
            &g_confirmedAllyHighWater, 0, 0);
        while (confirmedCount >
               static_cast<uint32_t>(previousHighWater)) {
            const LONG observed = InterlockedCompareExchange(
                &g_confirmedAllyHighWater,
                static_cast<LONG>(confirmedCount),
                previousHighWater);
            if (observed == previousHighWater) {
                char status[128] = {};
                _snprintf_s(
                    status,
                    sizeof(status),
                    _TRUNCATE,
                    "confirmed own-team public vehicles=%u team=%u",
                    confirmedCount,
                    localTeam);
                WriteLogLine("[native-v3] ", status);
                break;
            }
            previousHighWater = observed;
        }
    }

    /* Registry calls can publish callbacks; never make them under our lock. */
    for (uint32_t index = 0u; index < removeCount; ++index) {
        WotbModV3NativeBindings_RemovePublicVehicle(
            removeIds[index],
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN);
    }
    for (uint32_t index = 0u; index < publishCount; ++index) {
        UpsertPublicVehicle(publishIds[index], reason);
    }
}

static void ObservePublicVehicleRpc(
    uint32_t entityId,
    const char* methodName) {
    if (!IsPublicVehicleId(entityId)) return;
    WotbModV3NativeBindings_ObserveRpcMetadata(
        WOTBMOD_V3_RPC_INCOMING,
        entityId,
        "Vehicle",
        methodName);
}

static WotbModResult AllocateVehicleTokenLocked(
    const LiveVehicleRecord* record,
    void** outToken) {
    if (!record || !outToken) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outToken = nullptr;
    VehicleSnapshotToken* token =
        static_cast<VehicleSnapshotToken*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(VehicleSnapshotToken)));
    if (!token) return WOTBMOD_ERROR_LIMIT_REACHED;
    token->magic = kVehicleTokenMagic;
    FillVehicleInfoLocked(record, &token->info);
    *outToken = token;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetByEntityId(
    void*,
    uint32_t entityId,
    void** outToken) {
    if (!outToken || entityId == 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outToken = nullptr;
    AcquireSRWLockShared(&g_vehicleLock);
    LiveVehicleRecord* record = FindVehicleLocked(entityId);
    WotbModResult result =
        IsVehiclePublicLocked(record)
            ? AllocateVehicleTokenLocked(record, outToken)
            : WOTBMOD_ERROR_NOT_FOUND;
    ReleaseSRWLockShared(&g_vehicleLock);
    return result;
}

static WotbModResult WOTBMOD_CALL VehicleGetLocal(
    void* userData,
    void** outToken) {
    const uint32_t entityId =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    if (entityId == 0) {
        if (outToken) *outToken = nullptr;
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    return VehicleGetByEntityId(
        userData, entityId, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleGetCount(
    void*,
    uint32_t* outCount) {
    if (!outCount) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    AcquireSRWLockShared(&g_vehicleLock);
    *outCount = PublicVehicleCountLocked();
    ReleaseSRWLockShared(&g_vehicleLock);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetAt(
    void*,
    uint32_t requestedIndex,
    void** outToken) {
    if (!outToken) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outToken = nullptr;
    AcquireSRWLockShared(&g_vehicleLock);
    uint32_t activeIndex = 0;
    LiveVehicleRecord* found = nullptr;
    for (uint32_t index = 0; index < kMaxLiveVehicles; ++index) {
        if (!IsVehiclePublicLocked(&g_liveVehicles[index])) continue;
        if (activeIndex == requestedIndex) {
            found = &g_liveVehicles[index];
            break;
        }
        ++activeIndex;
    }
    WotbModResult result =
        found
            ? AllocateVehicleTokenLocked(found, outToken)
            : WOTBMOD_ERROR_NOT_FOUND;
    ReleaseSRWLockShared(&g_vehicleLock);
    return result;
}

static VehicleSnapshotToken* ValidVehicleToken(void* token) {
    VehicleSnapshotToken* snapshot =
        static_cast<VehicleSnapshotToken*>(token);
    return snapshot && snapshot->magic == kVehicleTokenMagic
               ? snapshot
               : nullptr;
}

static WotbModResult WOTBMOD_CALL VehicleClone(
    void*,
    void* token,
    void** outToken) {
    VehicleSnapshotToken* source = ValidVehicleToken(token);
    if (!source || !outToken) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outToken = nullptr;
    VehicleSnapshotToken* clone =
        static_cast<VehicleSnapshotToken*>(HeapAlloc(
            GetProcessHeap(),
            0,
            sizeof(VehicleSnapshotToken)));
    if (!clone) return WOTBMOD_ERROR_LIMIT_REACHED;
    memcpy(clone, source, sizeof(*clone));
    *outToken = clone;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleRelease(
    void*,
    void* token) {
    VehicleSnapshotToken* snapshot = ValidVehicleToken(token);
    if (!snapshot) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    snapshot->magic = 0;
    return HeapFree(GetProcessHeap(), 0, snapshot)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL VehicleGetInfo(
    void*,
    void* token,
    WotbModVehicleInfo* outInfo) {
    VehicleSnapshotToken* snapshot = ValidVehicleToken(token);
    if (!snapshot || !outInfo) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t callerSize = outInfo->struct_size;
    const uint32_t minimumSize = static_cast<uint32_t>(
        offsetof(WotbModVehicleInfo, flags) +
        sizeof(outInfo->flags));
    if (callerSize < minimumSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t infoSize =
        static_cast<uint32_t>(sizeof(snapshot->info));
    const uint32_t copySize =
        callerSize < infoSize ? callerSize : infoSize;
    memcpy(outInfo, &snapshot->info, copySize);
    return callerSize < infoSize
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

struct FileResource {
    uint32_t magic;
    WotbModResourceType type;
    uint64_t size;
    char path[WOTBMOD_MAX_RESOURCE_PATH];
};

const uint32_t kFileResourceMagic = 0x43525352u; /* RSRC */

static void ParentDirectory(char* path) {
    if (!path) return;
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
}

static bool JoinPath(
    const char* left,
    const char* right,
    char* output,
    size_t outputCapacity) {
    if (!left || !right || !output || outputCapacity == 0) return false;
    return _snprintf_s(
               output,
               outputCapacity,
               _TRUNCATE,
               "%s\\%s",
               left,
               right) >= 0;
}

static void WriteLogLine(const char* prefix, const char* message) {
    if (g_logHandle == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char line[2304] = {};
    const int length = _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%02u:%02u:%02u.%03u] %s%s\n",
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        prefix ? prefix : "",
        message ? message : "");
    if (length > 0) {
        DWORD written = 0;
        WriteFile(
            g_logHandle,
            line,
            static_cast<DWORD>(length),
            &written,
            nullptr);
        FlushFileBuffers(g_logHandle);
    }
    LeaveCriticalSection(&g_logLock);
}

static void WOTBMOD_CALL RuntimeLog(
    WotbModLogLevel level,
    const char* message,
    void*) {
    const char* prefix = "[info] ";
    if (level == WOTBMOD_LOG_TRACE) prefix = "[trace] ";
    if (level == WOTBMOD_LOG_WARNING) prefix = "[warning] ";
    if (level == WOTBMOD_LOG_ERROR) prefix = "[error] ";
    WriteLogLine(prefix, message);
}

static void WOTBMOD_CALL NativeBindingsLog(
    const char* message,
    void*) {
    WriteLogLine("[native-v3] ", message);
}

typedef void*(__thiscall* VehicleEntityGetterFn)(void*);
typedef void*(__thiscall* AvatarVehicleGetterFn)(void*);
typedef void*(__thiscall* ResolveAvatarVehicleFn)(void*);

static void* ResolveEntityFromGameLogic(void* gameLogic) {
    if (!gameLogic) return nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(gameLogic);
        VehicleEntityGetterFn getEntity =
            vtable
                ? reinterpret_cast<VehicleEntityGetterFn>(vtable[1])
                : nullptr;
        return getEntity ? getEntity(gameLogic) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool ReadEntitySnapshot(
    void* gameLogic,
    uint32_t* outEntityId,
    int32_t* outHealth,
    void** outEntity) {
    if (!outEntityId || !outHealth || !outEntity) return false;
    *outEntityId = 0;
    *outHealth = 0;
    *outEntity = nullptr;
    void* entity = ResolveEntityFromGameLogic(gameLogic);
    if (!entity) return false;
    __try {
        const uint8_t* bytes =
            static_cast<const uint8_t*>(entity);
        const uint32_t entityId =
            *reinterpret_cast<const uint32_t*>(
                bytes + kVehicleEntityIdOffset);
        if (entityId == 0) return false;
        *outEntityId = entityId;
        *outHealth = static_cast<int32_t>(
            *reinterpret_cast<const int16_t*>(
                bytes + kVehicleHealthOffset));
        *outEntity = entity;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* ResolveLocalEntityFromAvatar(void* avatar) {
    if (!avatar) return nullptr;
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule) return nullptr;
    __try {
        void* owner = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(avatar) + 0x1F8u);
        if (!owner) return nullptr;
        void** ownerVtable = *reinterpret_cast<void***>(owner);
        AvatarVehicleGetterFn getVehicle =
            ownerVtable
                ? reinterpret_cast<AvatarVehicleGetterFn>(
                      ownerVtable[0x188u / sizeof(void*)])
                : nullptr;
        if (!getVehicle) return nullptr;
        void* vehicle = getVehicle(owner);
        ResolveAvatarVehicleFn resolve =
            reinterpret_cast<ResolveAvatarVehicleFn>(
                reinterpret_cast<uint8_t*>(gameModule) +
                kResolveAvatarVehicleRva);
        void* wrapper = resolve ? resolve(vehicle) : nullptr;
        return ResolveEntityFromGameLogic(wrapper);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool ReadEntityIdAndHealth(
    void* entity,
    uint32_t* outEntityId,
    int32_t* outHealth) {
    if (!entity || !outEntityId || !outHealth) return false;
    __try {
        const uint8_t* bytes =
            static_cast<const uint8_t*>(entity);
        *outEntityId =
            *reinterpret_cast<const uint32_t*>(
                bytes + kVehicleEntityIdOffset);
        *outHealth = static_cast<int32_t>(
            *reinterpret_cast<const int16_t*>(
                bytes + kVehicleHealthOffset));
        return *outEntityId != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outEntityId = 0;
        *outHealth = 0;
        return false;
    }
}

static void QueueGameplayEvent(
    uint32_t type,
    uint32_t entityId,
    uint32_t otherEntityId,
    const void* payload,
    uint32_t payloadSize) {
    WotbModRuntimeClientEvent event = {};
    event.struct_size = sizeof(event);
    event.type = type;
    event.resource_type = WOTBMOD_RESOURCE_GENERIC;
    event.primary_entity_id = entityId;
    event.other_entity_id = otherEntityId;
    event.payload_size = payloadSize;
    if (payload && payloadSize > 0) {
        const uint32_t capacity =
            static_cast<uint32_t>(sizeof(event.payload));
        const uint32_t copySize =
            payloadSize < capacity ? payloadSize : capacity;
        memcpy(&event.payload, payload, copySize);
        event.payload_size = copySize;
    }
    const WotbModResult queued = WotbModRuntime_NotifyClientEvent(&event);
    if (queued != WOTBMOD_OK && type == WOTBMOD_EVENT_VEHICLE_KILLED) {
        char line[120] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "vehicle killed event refused by the runtime: result=%d",
                    static_cast<int>(queued));
        WriteLogLine("[native-v3] ", line);
    }
}

static void QueueBattleBoundary(uint32_t type, uint32_t state) {
    if (type == WOTBMOD_EVENT_BATTLE_ENTERED ||
        type == WOTBMOD_EVENT_BATTLE_STARTED) {
        InterlockedExchange(&g_battleContextActive, 1);
        InterlockedExchange(
            &g_uiDerivedContextMask,
            WOTBMOD_V3_CONTEXT_NONE);
        InterlockedExchange(&g_uiContextProbePending, 1);
    } else if (type == WOTBMOD_EVENT_BATTLE_LEFT) {
        InterlockedExchange(&g_battleContextActive, 0);
        InterlockedExchange(
            &g_uiDerivedContextMask,
            WOTBMOD_V3_CONTEXT_NONE);
        InterlockedExchange(&g_uiContextProbePending, 1);
    }
    WotbModBattleEventData payload = {};
    payload.state = state;
    QueueGameplayEvent(
        type, 0, 0, &payload, sizeof(payload));
}

struct OptionalEntityPublicFields {
    int32_t max_health;
    uint32_t team;
    uint64_t valid_fields;
};

static bool ReadVerifiedVehicleTeam(
    void* entity,
    uint32_t entityId,
    uint32_t* output) {
    if (!g_nativeFingerprintVerified || !entity || !entityId || !output) {
        return false;
    }
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule) return false;
    const void* expectedVtable =
        static_cast<const uint8_t*>(
            static_cast<const void*>(gameModule)) +
        kVehiclePrimaryVtableRva;
    __try {
        const uint8_t* vehicle = static_cast<const uint8_t*>(entity);
        void* actualVtable =
            *reinterpret_cast<void* const*>(vehicle);
        if (actualVtable != expectedVtable) return false;

        const uint32_t vehicleEntityId =
            *reinterpret_cast<const uint32_t*>(
                vehicle + kVehicleObjectEntityIdOffset);
        if (vehicleEntityId != entityId) return false;

        const uint8_t nativeTeam =
            *(vehicle + kVehicleObjectTeamOffset);
        if (nativeTeam < 1u || nativeTeam > 2u) return false;

        // The fingerprinted client and public API both use 1/2; 0 is unknown.
        *output = static_cast<uint32_t>(nativeTeam);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ReadOptionalEntityPublicFields(
    void* entity,
    uint32_t entityId,
    int32_t health,
    OptionalEntityPublicFields* output) {
    if (!output) return;
    ZeroMemory(output, sizeof(*output));
    if (!entity) return;
    __try {
        const uint8_t* bytes = static_cast<const uint8_t*>(entity);

        // VehicleGameLogic::set_maxHealth in the fingerprinted client reads
        // this exact int16 field from pEntity(). Do not reuse offsets from the
        // distinct BWEntity/Vehicle layout for team or player name.
        const int32_t nativeMaxHealth = static_cast<int32_t>(
            *reinterpret_cast<const int16_t*>(
                bytes + kVehicleMaxHealthOffset));
        if (nativeMaxHealth > 0 && nativeMaxHealth <= 100000 &&
            nativeMaxHealth >= health) {
            output->max_health = nativeMaxHealth;
            output->valid_fields |=
                WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output->max_health = 0;
        output->valid_fields &=
            ~WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH;
    }

    uint32_t team = 0u;
    if (ReadVerifiedVehicleTeam(entity, entityId, &team)) {
        output->team = team;
        output->valid_fields |=
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM;
    }
}

static bool RegisterVehicle(
    void* gameLogic,
    uint32_t entityId,
    int32_t health,
    void* entity,
    bool* outFirstActive) {
    if (!entityId || !outFirstActive) return false;
    OptionalEntityPublicFields optional = {};
    ReadOptionalEntityPublicFields(
        entity, entityId, health, &optional);
    if ((optional.valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM) != 0u) {
        if (InterlockedCompareExchange(
                &g_vehicleTeamVerifiedLogged, 1, 0) == 0) {
            WriteLogLine(
                "[native-v3] ",
                "verified Vehicle layout; public team field enabled");
        }
    } else if (InterlockedCompareExchange(
                   &g_vehicleTeamRejectedLogged, 1, 0) == 0) {
        WriteLogLine(
            "[native-v3] ",
            "Vehicle layout validation rejected team; field remains unknown");
    }
    AcquireSRWLockExclusive(&g_vehicleLock);
    const uint32_t previousActive = ActiveVehicleCountLocked();
    LiveVehicleRecord* record = ReserveVehicleLocked(entityId);
    const bool wasActive = record && record->active;
    if (record) {
        if (!wasActive) {
            record->visible_to_player = false;
            record->confirmed_ally = false;
            record->team = 0u;
        }
        record->game_logic = gameLogic;
        record->entity = entity;
        record->health = health;
        if ((optional.valid_fields &
             WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH) != 0u) {
            record->max_health = optional.max_health;
        } else if (health > record->max_health) {
            record->max_health = health;
        }
        if ((optional.valid_fields &
             WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM) != 0u) {
            record->team = optional.team;
        }
        record->active = true;
        record->flags &= ~WOTBMOD_VEHICLE_DESTROYED;
        record->flags |= WOTBMOD_VEHICLE_ALIVE;
    }
    *outFirstActive =
        record && !wasActive && previousActive == 0;
    ReleaseSRWLockExclusive(&g_vehicleLock);
    if (*outFirstActive) {
        /* Arm the replay fallback once for this battle, not during teardown. */
        InterlockedExchange(&g_avatarFallbackResolved, 0);
    }
    const uint32_t localEntityId =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    if (record && localEntityId != 0u) {
        RefreshConfirmedAllies(
            localEntityId,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    }
    return record && !wasActive;
}

static bool DeactivateVehicle(
    uint32_t entityId,
    bool* outLastActive) {
    if (!entityId || !outLastActive) return false;
    AcquireSRWLockExclusive(&g_vehicleLock);
    LiveVehicleRecord* record = FindVehicleLocked(entityId);
    const bool wasActive = record && record->active;
    if (record) {
        record->active = false;
        record->visible_to_player = false;
        record->confirmed_ally = false;
        record->flags &= ~WOTBMOD_VEHICLE_ALIVE;
        if (record->health <= 0) {
            record->flags |= WOTBMOD_VEHICLE_DESTROYED;
        }
        record->game_logic = nullptr;
        record->entity = nullptr;
    }
    *outLastActive =
        wasActive && ActiveVehicleCountLocked() == 0;
    ReleaseSRWLockExclusive(&g_vehicleLock);
    return wasActive;
}

static void UpdateVehicleHealthRecord(
    uint32_t entityId,
    int32_t health,
    bool destroyed) {
    AcquireSRWLockExclusive(&g_vehicleLock);
    LiveVehicleRecord* record = ReserveVehicleLocked(entityId);
    if (record) {
        record->health = health;
        if (health > record->max_health) {
            record->max_health = health;
        }
        if (destroyed) {
            record->flags |= WOTBMOD_VEHICLE_DESTROYED;
            record->flags &= ~WOTBMOD_VEHICLE_ALIVE;
        }
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);
}

static void SetLocalVehicle(uint32_t entityId) {
    const uint32_t previousId =
        static_cast<uint32_t>(InterlockedExchange(
            &g_localVehicleEntityId,
            static_cast<LONG>(entityId)));
    RefreshConfirmedAllies(
        entityId,
        entityId != 0u
            ? WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER
            : WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN);
    if (previousId == entityId) return;
    if (previousId != 0u) {
        if (IsPublicVehicleId(previousId)) {
            UpsertPublicVehicle(
                previousId,
                WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
        } else {
            WotbModV3NativeBindings_RemovePublicVehicle(
                previousId,
                WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN);
        }
    }
    if (entityId != 0u) {
        UpsertPublicVehicle(
            entityId,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER);
    }
    WotbModVehicleEventData payload = {};
    payload.entity_id = entityId;
    payload.other_entity_id = previousId;
    payload.flags =
        entityId ? WOTBMOD_VEHICLE_LOCAL : 0u;
    QueueGameplayEvent(
        WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED,
        entityId,
        previousId,
        &payload,
        sizeof(payload));
    if (entityId != 0u) {
        ObservePublicVehicleRpc(
            entityId, "Avatar.updateVehicleHealth");
    }
}

static void WOTBMOD_CALL ObserveAvatarVehicle(
    void* avatar,
    void*) {
    bool hasActiveVehicle = false;
    AcquireSRWLockShared(&g_vehicleLock);
    hasActiveVehicle = ActiveVehicleCountLocked() != 0u;
    ReleaseSRWLockShared(&g_vehicleLock);
    if (!hasActiveVehicle ||
        InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0) != 0 ||
        InterlockedCompareExchange(
            &g_avatarFallbackResolved, 1, 0) != 0) {
        return;
    }
    void* entity = ResolveLocalEntityFromAvatar(avatar);
    uint32_t entityId = 0u;
    int32_t health = 0;
    if (!ReadEntityIdAndHealth(entity, &entityId, &health)) {
        InterlockedExchange(&g_avatarFallbackResolved, 0);
        return;
    }
    UpdateVehicleHealthRecord(entityId, health, health <= 0);
    SetLocalVehicle(entityId);
    UpsertPublicVehicle(
        entityId,
        WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
}

static void __fastcall OnEnterWorldDetour(
    void* gameLogic,
    void*) {
    if (g_originalOnEnterWorld) {
        hobs::Before<VehicleVoidFn>(g_onEnterWorldTarget, gameLogic);
        g_originalOnEnterWorld(gameLogic);
        hobs::After<VehicleVoidFn>(g_onEnterWorldTarget, gameLogic);
    }
    uint32_t entityId = 0;
    int32_t health = 0;
    void* entity = nullptr;
    if (!ReadEntitySnapshot(
            gameLogic, &entityId, &health, &entity)) {
        return;
    }
    bool firstActive = false;
    const bool spawned = RegisterVehicle(
        gameLogic,
        entityId,
        health,
        entity,
        &firstActive);
    if (!spawned) return;
    if (firstActive) {
        QueueBattleBoundary(
            WOTBMOD_EVENT_BATTLE_ENTERED, 1u);
        QueueBattleBoundary(
            WOTBMOD_EVENT_BATTLE_STARTED, 2u);
    }
    if (!IsPublicVehicleId(entityId)) return;
    UpsertPublicVehicle(
        entityId,
        WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE);
    ObservePublicVehicleRpc(entityId, "Vehicle.onEnterWorld");
    WotbModVehicleEventData payload = {};
    payload.entity_id = entityId;
    payload.health = health;
    payload.flags = WOTBMOD_VEHICLE_ALIVE;
    QueueGameplayEvent(
        WOTBMOD_EVENT_VEHICLE_SPAWNED,
        entityId,
        0,
        &payload,
        sizeof(payload));
}

static void __fastcall OnLeaveWorldDetour(
    void* gameLogic,
    void*) {
    uint32_t entityId = 0;
    int32_t health = 0;
    void* entity = nullptr;
    const bool hasSnapshot = ReadEntitySnapshot(
        gameLogic, &entityId, &health, &entity);
    const bool wasPublic =
        hasSnapshot && IsPublicVehicleId(entityId);
    if (wasPublic) {
        ObservePublicVehicleRpc(entityId, "Vehicle.onLeaveWorld");
        WotbModVehicleEventData payload = {};
        payload.entity_id = entityId;
        payload.health = health;
        QueueGameplayEvent(
            WOTBMOD_EVENT_VEHICLE_DESPAWNED,
            entityId,
            0,
            &payload,
            sizeof(payload));
    }
    if (g_originalOnLeaveWorld) {
        hobs::Before<VehicleVoidFn>(g_onLeaveWorldTarget, gameLogic);
        g_originalOnLeaveWorld(gameLogic);
        hobs::After<VehicleVoidFn>(g_onLeaveWorldTarget, gameLogic);
    }
    if (!hasSnapshot) return;
    if (wasPublic) {
        WotbModV3NativeBindings_RemovePublicVehicle(
            entityId,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_LEFT_WORLD);
    }
    bool lastActive = false;
    DeactivateVehicle(entityId, &lastActive);
    const uint32_t localId =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    if (localId == entityId) SetLocalVehicle(0);
    if (lastActive) {
        QueueBattleBoundary(
            WOTBMOD_EVENT_BATTLE_ENDED, 3u);
        QueueBattleBoundary(
            WOTBMOD_EVENT_BATTLE_LEFT, 0u);
        WotbModV3NativeBindings_ResetPublicGameplayState(
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_LEFT_WORLD);
    }
}

/*
 * GES::GameEventSystem::SubscribeImpl is called by every engine subscriber
 * at start-up. Only `this` is recorded; the std::function is forwarded
 * untouched as ten dwords so the callee's `ret 0x30` still balances.
 */
static void __fastcall GesSubscribeImplDetour(
    void* bus, void*, const void* owner, const void* typeIndex,
    uint32_t f0, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t f4,
    uint32_t f5, uint32_t f6, uint32_t f7, uint32_t f8, uint32_t f9) {
    if (bus) wotbmod::loader::ges::GesSetBus(bus);
    if (g_originalGesSubscribeImpl) {
        g_originalGesSubscribeImpl(
            bus, owner, typeIndex, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9);
    }
}

static void GesDeliverToRuntime(
    void*, const char* typeName, const void* payload, uint32_t publisherRva,
    uint32_t flags) {
    wotbmod::v3::GesHostPublish(typeName, payload, publisherRva, flags);
}

static void GesLog(const char* message) {
    WriteLogLine("[loader] ", message);
}

static void __fastcall ShowShootingDetour(
    void* gameLogic,
    void*,
    const uint8_t* shotCode) {
    uint32_t entityId = 0;
    int32_t health = 0;
    void* entity = nullptr;
    ReadEntitySnapshot(
        gameLogic, &entityId, &health, &entity);
    uint32_t code = 0;
    if (shotCode) {
        __try {
            code = *shotCode;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            code = 0;
        }
    }
    if (g_originalShowShooting) {
        hobs::Before<VehicleShootingFn>(g_showShootingTarget, gameLogic, shotCode);
        g_originalShowShooting(gameLogic, shotCode);
        hobs::After<VehicleShootingFn>(g_showShootingTarget, gameLogic, shotCode);
    }
    if (!entityId || !IsPublicVehicleId(entityId)) return;
    ObservePublicVehicleRpc(entityId, "Vehicle.showShooting");
    WotbModShotEventData payload = {};
    payload.shot_code = code;
    QueueGameplayEvent(
        WOTBMOD_EVENT_SHOT_FIRED,
        entityId,
        0,
        &payload,
        sizeof(payload));
    WotbModV3NativeBindings_ObserveShot(entityId, code);
}

static void __fastcall SetHealthDetour(
    void* gameLogic,
    void*,
    const int16_t* previousHealthPointer) {
    int32_t previousHealth = 0;
    if (previousHealthPointer) {
        __try {
            previousHealth =
                static_cast<int32_t>(*previousHealthPointer);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            previousHealth = 0;
        }
    }
    if (g_originalSetHealth) {
        hobs::Before<VehicleSetHealthFn>(
            g_setHealthTarget, gameLogic, previousHealthPointer);
        g_originalSetHealth(gameLogic, previousHealthPointer);
        hobs::After<VehicleSetHealthFn>(
            g_setHealthTarget, gameLogic, previousHealthPointer);
    }
    uint32_t entityId = 0;
    int32_t health = 0;
    void* entity = nullptr;
    if (!ReadEntitySnapshot(
            gameLogic, &entityId, &health, &entity)) {
        return;
    }
    const bool destroyed = previousHealth > 0 && health <= 0;
    UpdateVehicleHealthRecord(entityId, health, destroyed);
    if (!IsPublicVehicleId(entityId)) return;
    UpsertPublicVehicle(
        entityId,
        WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    ObservePublicVehicleRpc(entityId, "Vehicle.set_health");

    WotbModVehicleEventData healthPayload = {};
    healthPayload.entity_id = entityId;
    healthPayload.previous_health = previousHealth;
    healthPayload.health = health;
    healthPayload.flags =
        destroyed ? WOTBMOD_VEHICLE_DESTROYED : 0u;
    QueueGameplayEvent(
        WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED,
        entityId,
        0,
        &healthPayload,
        sizeof(healthPayload));

    WotbModDamageEventData damagePayload = {};
    damagePayload.damage =
        health < previousHealth ? previousHealth - health : 0;
    damagePayload.previous_health = previousHealth;
    damagePayload.health = health;
    if (health < previousHealth) {
        QueueGameplayEvent(
            WOTBMOD_EVENT_VEHICLE_DAMAGED,
            entityId,
            0,
            &damagePayload,
            sizeof(damagePayload));
    }
    if (destroyed) {
        QueueGameplayEvent(
            WOTBMOD_EVENT_VEHICLE_DESTROYED,
            entityId,
            0,
            &healthPayload,
            sizeof(healthPayload));
    }
}

static void __fastcall UpdateVehicleHealthDetour(
    void* avatar,
    void*,
    const int16_t* previousHealth,
    const uint8_t* flags) {
    if (g_originalUpdateVehicleHealth) {
        hobs::Before<AvatarUpdateHealthFn>(
            g_updateVehicleHealthTarget, avatar, previousHealth, flags);
        g_originalUpdateVehicleHealth(
            avatar, previousHealth, flags);
        hobs::After<AvatarUpdateHealthFn>(
            g_updateVehicleHealthTarget, avatar, previousHealth, flags);
    }
    void* entity = ResolveLocalEntityFromAvatar(avatar);
    uint32_t entityId = 0;
    int32_t health = 0;
    if (ReadEntityIdAndHealth(
            entity, &entityId, &health)) {
        UpdateVehicleHealthRecord(
            entityId, health, health <= 0);
        SetLocalVehicle(entityId);
        UpsertPublicVehicle(
            entityId,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    }
}

static void __fastcall ReloadSetStateDetour(
    void* reloadTimer,
    void*,
    int32_t state,
    float value) {
    if (g_originalReloadSetState) {
        hobs::Before<ReloadSetStateFn>(
            g_reloadSetStateTarget, reloadTimer, state, value);
        g_originalReloadSetState(
            reloadTimer, state, value);
        hobs::After<ReloadSetStateFn>(
            g_reloadSetStateTarget, reloadTimer, state, value);
    }
    if (!reloadTimer) return;
    WotbModReloadEventData payload = {};
    __try {
        const uint8_t* bytes =
            static_cast<const uint8_t*>(reloadTimer);
        payload.state =
            *reinterpret_cast<const uint32_t*>(bytes + 0x3Cu);
        payload.progress =
            *reinterpret_cast<const float*>(bytes + 0x40u);
        payload.duration_seconds =
            *reinterpret_cast<const float*>(bytes + 0x44u);
        payload.paused =
            *reinterpret_cast<const uint8_t*>(bytes + 0x4Cu)
                ? 1u
                : 0u;
        payload.remaining_seconds =
            payload.duration_seconds > 0.0f
                ? payload.duration_seconds *
                      (1.0f - payload.progress)
                : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const uint32_t localId =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    QueueGameplayEvent(
        WOTBMOD_EVENT_RELOAD_STATE_CHANGED,
        localId,
        0,
        &payload,
        sizeof(payload));
}

static bool __fastcall UiSystemInputDetourInner(
    void* control,
    void*,
    const void* uiEvent);
static thread_local LONGLONG t_uiInputOriginalTicks = 0;

static bool __fastcall UiSystemInputDetour(
    void* control,
    void* edx,
    const void* uiEvent) {
    const LONGLONG started = ProfNow();
    const LONGLONG outerOriginalTicks = t_uiInputOriginalTicks;
    t_uiInputOriginalTicks = 0;
    const bool result = UiSystemInputDetourInner(control, edx, uiEvent);
    const LONGLONG total = ProfNow() - started;
    ProfAdd(&g_wotbProfUiInputTicks, total - t_uiInputOriginalTicks);
    InterlockedIncrement64(&g_wotbProfUiInputCalls);
    /* The caller's own original-call span already covers this whole nested
     * call, so just hand its accumulator back untouched. */
    t_uiInputOriginalTicks = outerOriginalTicks;
    return result;
}

static bool __fastcall UiSystemInputDetourInner(
    void* control,
    void*,
    const void* uiEvent) {
    WotbModUiInputEventData payload = {};
    bool captured = false;
    bool consumeForApiUi = false;
    static thread_local bool apiPointerGesture = false;
    if (uiEvent) {
        __try {
            const uint8_t* bytes =
                static_cast<const uint8_t*>(uiEvent);
            payload.action =
                *reinterpret_cast<const uint32_t*>(
                    bytes + 0x38u);
            payload.screen_x =
                *reinterpret_cast<const float*>(
                    bytes + 0x10u);
            payload.screen_y =
                *reinterpret_cast<const float*>(
                    bytes + 0x14u);
            payload.local_x = payload.screen_x;
            payload.local_y = payload.screen_y;
            captured = true;
            const bool overApiUi =
                wotbmod::v3::ShouldCaptureClientHostUiInput(
                    payload.screen_x, payload.screen_y);
            switch (payload.action) {
                case 1u:  // DAVA UIEvent::Phase::BEGAN
                    apiPointerGesture = overApiUi;
                    consumeForApiUi = apiPointerGesture;
                    break;
                case 2u:  // DRAG
                    consumeForApiUi = apiPointerGesture;
                    break;
                case 3u:  // ENDED
                case 6u:  // CANCELLED
                    consumeForApiUi = apiPointerGesture;
                    apiPointerGesture = false;
                    break;
                case 4u:  // MOVE
                case 5u:  // WHEEL
                    consumeForApiUi = overApiUi;
                    break;
                default:
                    break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            captured = false;
            consumeForApiUi = false;
            apiPointerGesture = false;
        }
    }
    // DAVA emits the same pointer independently after the Win32 window
    // procedure. Lua UI has already received that phase through the client
    // host, so consume before traversing the stock tree. Besides preventing
    // click-through, this avoids an O(game controls * mod controls) scan on
    // every pointer phase.
    const bool callOriginal =
        !consumeForApiUi && g_originalUiSystemInput != nullptr;
    if (callOriginal) {
        hobs::Before<UiSystemInputFn>(g_uiSystemInputTarget, control, uiEvent);
    }
    const LONGLONG originalStarted = ProfNow();
    const bool result = callOriginal
        ? g_originalUiSystemInput(control, uiEvent)
        : consumeForApiUi;
    t_uiInputOriginalTicks += ProfNow() - originalStarted;
    if (callOriginal) {
        hobs::After<UiSystemInputFn>(g_uiSystemInputTarget, control, uiEvent);
    }
    static thread_local const void* lastEvent = nullptr;
    static thread_local LONG lastFrame = -1;
    static thread_local uint32_t lastAction = UINT32_MAX;
    static thread_local float lastX = 0.0f;
    static thread_local float lastY = 0.0f;
    const LONG currentFrame = InterlockedCompareExchange(
        &g_dispatchFrameSerial, 0, 0);
    if (captured &&
        (lastFrame != currentFrame ||
         lastEvent != uiEvent ||
         lastAction != payload.action ||
         lastX != payload.screen_x ||
         lastY != payload.screen_y)) {
        lastFrame = currentFrame;
        lastEvent = uiEvent;
        lastAction = payload.action;
        lastX = payload.screen_x;
        lastY = payload.screen_y;
        QueueGameplayEvent(
            WOTBMOD_EVENT_UI_INPUT,
            0,
            0,
            &payload,
            sizeof(payload));
        WotbModV3NativeBindings_ObserveUiInput(
            payload.action,
            payload.screen_x,
            payload.screen_y,
            0.0f,
            0.0f,
            0u);
    }
    return result;
}

static bool TryReadAmmoShellId(
    const void* selector,
    int32_t* outShellId) {
    if (!selector || !outShellId) return false;
    __try {
        *outShellId =
            *reinterpret_cast<const int32_t*>(
                static_cast<const uint8_t*>(selector) +
                0x20u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outShellId = -1;
        return false;
    }
}

static uint32_t NormalizeAmmoShellId(int32_t shellId) {
    return shellId < 0
               ? 0u
               : static_cast<uint32_t>(shellId);
}

static int32_t __fastcall AmmoChangedDetour(
    void* selector,
    void*,
    int32_t shellId,
    int32_t context) {
    int32_t previousShellId = -1;
    const bool readPrevious =
        TryReadAmmoShellId(selector, &previousShellId);
    const bool hasOriginal = g_originalAmmoChanged != nullptr;
    if (hasOriginal) {
        hobs::Before<AmmoChangedFn>(
            g_ammoChangedTarget, selector, shellId, context);
    }
    const int32_t result =
        hasOriginal
            ? g_originalAmmoChanged(
                  selector, shellId, context)
            : 0;
    if (hasOriginal) {
        hobs::After<AmmoChangedFn>(
            g_ammoChangedTarget, selector, shellId, context);
    }
    int32_t currentShellId = -1;
    const bool readCurrent =
        hasOriginal &&
        TryReadAmmoShellId(selector, &currentShellId);
    if (readPrevious &&
        readCurrent &&
        previousShellId != currentShellId) {
        WotbModAmmoEventData payload = {};
        payload.previous_shell_id =
            NormalizeAmmoShellId(previousShellId);
        payload.shell_id =
            NormalizeAmmoShellId(currentShellId);
        payload.count = -1;
        const uint32_t localId =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &g_localVehicleEntityId, 0, 0));
        QueueGameplayEvent(
            WOTBMOD_EVENT_AMMO_CHANGED,
            localId,
            0,
            &payload,
            sizeof(payload));
    }
    return result;
}

static void __fastcall AimTargetSetDetour(
    void* avatar,
    void*,
    const uint32_t* targetEntityId,
    uint8_t flags) {
    uint32_t previous = 0;
    uint32_t target = 0;
    __try {
        if (avatar) {
            previous =
                *reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(avatar) +
                    0x5C8u);
        }
        if (targetEntityId) target = *targetEntityId;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        previous = 0;
        target = 0;
    }
    if (g_originalAimTargetSet) {
        g_originalAimTargetSet(
            avatar, targetEntityId, flags);
    }
    if (target == previous) return;
    if (target != 0u && !IsPublicVehicleId(target)) {
        target = 0u;
    }
    if (previous != 0u && !IsPublicVehicleId(previous)) {
        previous = 0u;
    }
    if (target == previous) return;
    WotbModVehicleEventData payload = {};
    payload.entity_id =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    payload.other_entity_id = target;
    payload.flags = flags;
    QueueGameplayEvent(
        WOTBMOD_EVENT_AIM_TARGET_CHANGED,
        payload.entity_id,
        target,
        &payload,
        sizeof(payload));
}

static void __fastcall AimTargetClearDetour(
    void* avatar,
    void*,
    uint8_t flags) {
    uint32_t previous = 0;
    if (avatar) {
        __try {
            previous =
                *reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(avatar) +
                    0x5C8u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            previous = 0;
        }
    }
    if (g_originalAimTargetClear) {
        g_originalAimTargetClear(avatar, flags);
    }
    if (!previous || !IsPublicVehicleId(previous)) return;
    WotbModVehicleEventData payload = {};
    payload.entity_id =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_localVehicleEntityId, 0, 0));
    payload.other_entity_id = 0;
    payload.flags = flags;
    QueueGameplayEvent(
        WOTBMOD_EVENT_AIM_TARGET_CHANGED,
        payload.entity_id,
        0,
        &payload,
        sizeof(payload));
}

static void __fastcall ObservedStatusDetour(
    void* arena,
    void*,
    void* message) {
    uint32_t entityId = 0;
    bool observed = false;
    bool captured = false;
    if (message) {
        __try {
            const uint8_t* wrapper =
                static_cast<const uint8_t*>(message);
            const uint8_t* payload = nullptr;
            if (*reinterpret_cast<const uint32_t*>(
                    wrapper + 0x14u) == 0x0Fu) {
                payload =
                    *reinterpret_cast<const uint8_t* const*>(
                        wrapper + 0x0Cu);
            } else {
                HMODULE gameModule = GetModuleHandleA(nullptr);
                ObservedPayloadGetterFn getter =
                    gameModule
                        ? reinterpret_cast<
                              ObservedPayloadGetterFn>(
                              reinterpret_cast<uint8_t*>(
                                  gameModule) +
                              kObservedPayloadGetterRva)
                        : nullptr;
                payload = getter
                              ? static_cast<const uint8_t*>(
                                    getter())
                              : nullptr;
            }
            if (payload) {
                entityId =
                    *reinterpret_cast<const uint32_t*>(
                        payload + 0x0Cu);
                observed =
                    *reinterpret_cast<const uint8_t*>(
                        payload + 0x10u) != 0;
                captured = entityId != 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            captured = false;
        }
    }
    if (g_originalObservedStatus) {
        g_originalObservedStatus(arena, message);
    }
    if (!captured) return;
    bool active = false;
    bool local = false;
    bool wasPublic = false;
    bool visibilityChanged = false;
    AcquireSRWLockExclusive(&g_vehicleLock);
    LiveVehicleRecord* record =
        FindVehicleLocked(entityId);
    if (record && record->active) {
        active = true;
        local =
            record->entity_id ==
            static_cast<uint32_t>(InterlockedCompareExchange(
                &g_localVehicleEntityId, 0, 0));
        wasPublic = local || record->visible_to_player ||
                    record->confirmed_ally;
        visibilityChanged =
            record->visible_to_player != observed;
        record->visible_to_player = observed;
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);
    if (!active || !visibilityChanged ||
        (!observed && !wasPublic)) {
        return;
    }
    if (observed) {
        UpsertPublicVehicle(
            entityId,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE);
        ObservePublicVehicleRpc(
            entityId, "ClientArena.observed_status");
    } else {
        /*
         * The unspot transition itself is public because this entity was
         * visible immediately before the transition. No later entity data is
         * emitted until a new spot, unless this is the local vehicle.
         */
        WotbModV3NativeBindings_ObserveRpcMetadata(
            WOTBMOD_V3_RPC_INCOMING,
            entityId,
            "Vehicle",
            "ClientArena.observed_status");
    }
    WotbModVehicleEventData payload = {};
    payload.entity_id = entityId;
    payload.flags = observed ? 1u : 0u;
    QueueGameplayEvent(
        observed
            ? WOTBMOD_EVENT_VEHICLE_SPOTTED
            : WOTBMOD_EVENT_VEHICLE_UNSPOTTED,
        entityId,
        0,
        &payload,
        sizeof(payload));
    if (!observed) {
        bool remainsPublic = false;
        AcquireSRWLockShared(&g_vehicleLock);
        const LiveVehicleRecord* currentRecord =
            FindVehicleLocked(entityId);
        remainsPublic = IsVehiclePublicLocked(currentRecord);
        ReleaseSRWLockShared(&g_vehicleLock);
        if (remainsPublic) {
            UpsertPublicVehicle(
                entityId,
                WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
        } else {
            WotbModV3NativeBindings_RemovePublicVehicle(
                entityId,
                WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN);
        }
    }
}

/*
 * Local vehicle pose. PlayerController's per-frame pose update (anchor
 * kPlayerControllerPoseUpdateRva) rewrites a 232-byte block at this+0x5C
 * that the ReplayRecorder later reads under the spinlock at +224. The 4x4
 * matrix at +108 is the controlled vehicle's appearance world transform:
 * rows 0..2 are the hull axes, row 3 the hull origin (DAVA::Matrix4 is
 * row-major with the translation in _30.._32). The detour copies the block
 * under the same spinlock (bounded spin, released on any fault), publishes
 * the origin as `position` and the unit forward row as `direction` for the
 * local vehicle while it is alive, throttled to kPoseUpdateIntervalMs and
 * skipped while the hull neither moved 5 cm nor turned 1 degree. Which row
 * is "forward" follows the world's up axis, read off the matrix itself once
 * per vehicle: a Y-up world (BigWorld convention) has forward on row 2, a
 * Z-up one (DAVA convention) on row 1.
 *
 * Verified live 2026-09-06 01:19 in a random battle (11.20.0.887): the world
 * is Y-up, forward is +row 2; holding W moved the origin 30 m along row 2
 * with x unchanged, holding D swung row 2 from (0,0,1) to (1,0,0.03) with the
 * origin still, a second W moved x by 20 m along the new row. The other
 * fields are camera state: +60 pivot and +88 eye in DAVA Z-up order
 * (x, z, y+h), +172 yaw and +176 pitch in radians, +104 stays zero. The
 * first kPoseDumpLimit samples per vehicle are still logged as a trace.
 */
const uint32_t kPoseBlockOffset = 0x5Cu;
const uint32_t kPoseBlockLockOffset = 224u;
const uint32_t kPoseBlockSize = 232u;
const uint32_t kPoseMatrixOffset = 108u;
const uint32_t kPoseVehicleIdOffset = 104u;
const uint32_t kPoseUpdateIntervalMs = 100u;
const uint32_t kPoseDumpIntervalMs = 1000u;
const uint32_t kPoseDumpLimit = 3u;

static ULONGLONG g_poseLastPublishMs = 0u;
static ULONGLONG g_poseLastDumpMs = 0u;
static uint32_t g_poseDumpCount = 0u;
static uint32_t g_poseForwardRow = 0u; /* 0 = undecided */
static uint32_t g_poseDumpVehicleId = 0u;
static uint32_t g_poseLastEntityId = 0u;
static float g_poseLastPosition[3] = {};
static float g_poseLastDirection[3] = {};

static bool CopyPoseBlock(const uint8_t* block, uint8_t* out) {
    volatile LONG* lock = nullptr;
    bool locked = false;
    __try {
        lock = reinterpret_cast<volatile LONG*>(
            const_cast<uint8_t*>(block) + kPoseBlockLockOffset);
        for (uint32_t spin = 0u;; ++spin) {
            if (InterlockedCompareExchange(lock, 1, 0) == 0) break;
            if (spin > 20000u) return false;
            YieldProcessor();
        }
        locked = true;
        memcpy(out, block, kPoseBlockSize);
        InterlockedExchange(lock, 0);
        locked = false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (locked && lock) InterlockedExchange(lock, 0);
        return false;
    }
}

static bool FiniteVec3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) &&
           std::isfinite(v[2]);
}

static void DumpPoseBlock(const uint8_t* copy, uint32_t localId) {
    const float* f = reinterpret_cast<const float*>(copy);
    const float* m = reinterpret_cast<const float*>(copy + kPoseMatrixOffset);
    char line[900] = {};
    _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "pose #%u local=%u id104=%u a60=(%.2f,%.2f,%.2f) b72=(%.2f,%.2f,%.2f) "
        "f84=%.3f c88=(%.2f,%.2f,%.2f) r0=(%.3f,%.3f,%.3f) r1=(%.3f,%.3f,%.3f) "
        "r2=(%.3f,%.3f,%.3f) t=(%.2f,%.2f,%.2f) f172=%.3f f176=%.3f b192=%u "
        "ray4=(%.2f,%.2f,%.2f) ray28=(%.2f,%.2f,%.2f) f48=%.2f b56=%u fwdrow=%u",
        g_poseDumpCount, localId,
        *reinterpret_cast<const uint32_t*>(copy + kPoseVehicleIdOffset),
        f[15], f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23], f[24],
        m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10],
        m[12], m[13], m[14], f[43], f[44], copy[192],
        f[1], f[2], f[3], f[7], f[8], f[9], f[12], copy[56], g_poseForwardRow);
    WriteLogLine("[native-v3] ", line);
}

static bool ReadPoseFromBlock(
    const uint8_t* copy,
    float* outPosition,
    float* outDirection) {
    const float* m = reinterpret_cast<const float*>(copy + kPoseMatrixOffset);
    if (!FiniteVec3(m) || !FiniteVec3(m + 4) || !FiniteVec3(m + 8) ||
        !FiniteVec3(m + 12)) {
        return false;
    }
    if (g_poseForwardRow == 0u) {
        /* the up row of a hull on roughly level ground points along the
         * world's up axis: row 1 along Y means a Y-up world (forward = row
         * 2), row 2 along Z a Z-up world (forward = row 1) */
        if (fabsf(m[5]) > 0.85f) {
            g_poseForwardRow = 2u;
        } else if (fabsf(m[10]) > 0.85f) {
            g_poseForwardRow = 1u;
        } else {
            return false;
        }
    }
    const float* forward = m + 4u * g_poseForwardRow;
    const float len = sqrtf(
        forward[0] * forward[0] + forward[1] * forward[1] +
        forward[2] * forward[2]);
    if (!(len > 0.5f && len < 2.0f)) return false;
    outPosition[0] = m[12];
    outPosition[1] = m[13];
    outPosition[2] = m[14];
    outDirection[0] = forward[0] / len;
    outDirection[1] = forward[1] / len;
    outDirection[2] = forward[2] / len;
    return true;
}

static void PublishOtherVehiclePoses(
    ULONGLONG now, uint32_t localId, const float* localPosition, bool localValid);

static void PublishLocalVehiclePose(void* self) {
    const uint8_t* block = nullptr;
    __try {
        block = *reinterpret_cast<const uint8_t* const*>(
            static_cast<const uint8_t*>(self) + kPoseBlockOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        block = nullptr;
    }
    if (!block) return;
    uint8_t copy[kPoseBlockSize] = {};
    if (!CopyPoseBlock(block, copy)) return;
    const uint32_t localId = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_localVehicleEntityId, 0, 0));
    const ULONGLONG now = GetTickCount64();
    if (localId != g_poseDumpVehicleId) {
        /* a new controlled vehicle: fresh trace, re-read the up axis */
        g_poseDumpVehicleId = localId;
        g_poseDumpCount = 0u;
        g_poseForwardRow = 0u;
    }
    if (localId != 0u && g_poseDumpCount < kPoseDumpLimit &&
        now - g_poseLastDumpMs >= kPoseDumpIntervalMs) {
        g_poseLastDumpMs = now;
        ++g_poseDumpCount;
        DumpPoseBlock(copy, localId);
    }
    if (localId == 0u) return;
    float position[3] = {};
    float direction[3] = {};
    const bool haveLocalPose = ReadPoseFromBlock(copy, position, direction);
    PublishOtherVehiclePoses(now, localId, position, haveLocalPose);
    if (!haveLocalPose || now - g_poseLastPublishMs < kPoseUpdateIntervalMs) {
        return;
    }
    if (g_poseLastEntityId == localId) {
        const float dx = position[0] - g_poseLastPosition[0];
        const float dy = position[1] - g_poseLastPosition[1];
        const float dz = position[2] - g_poseLastPosition[2];
        const float dot =
            direction[0] * g_poseLastDirection[0] +
            direction[1] * g_poseLastDirection[1] +
            direction[2] * g_poseLastDirection[2];
        if (dx * dx + dy * dy + dz * dz < 0.0025f && dot > 0.99985f) return;
    }
    bool publishable = false;
    AcquireSRWLockExclusive(&g_vehicleLock);
    LiveVehicleRecord* record = FindVehicleLocked(localId);
    if (record && record->active && record->health > 0 &&
        (record->flags & WOTBMOD_VEHICLE_DESTROYED) == 0u) {
        memcpy(record->position, position, sizeof(record->position));
        memcpy(record->direction, direction, sizeof(record->direction));
        record->pose_valid = true;
        publishable = IsVehiclePublicLocked(record);
    }
    ReleaseSRWLockExclusive(&g_vehicleLock);
    if (!publishable) return;
    g_poseLastPublishMs = now;
    g_poseLastEntityId = localId;
    memcpy(g_poseLastPosition, position, sizeof(g_poseLastPosition));
    memcpy(g_poseLastDirection, direction, sizeof(g_poseLastDirection));
    UpsertPublicVehicle(localId, WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
}

/* ---- other vehicles: appearance chain -------------------------------- */
/*
 * The pose update reads the controlled vehicle's matrix as
 * `*(appearance + 8) + 1344` with appearance = `*(entity + 56)`, entity
 * being what VehicleGameLogic's virtual slot 1 (pEntity) returns - the same
 * object the loader already holds as LiveVehicleRecord::entity. The same
 * chain is read for every other registered vehicle. It is trusted only after
 * the chain of the local vehicle reproduces the pose block's own origin
 * (self-check once per vehicle), so a wrong offset can never publish
 * garbage for the others.
 */
const uint32_t kVehicleAppearanceOffset = 56u;
const uint32_t kAppearanceObjectOffset = 8u;
const uint32_t kAppearanceMatrixOffset = 1344u;
const uint32_t kPoseChainDiagLimit = 12u;

static bool g_poseChainVerified = false;
static uint32_t g_poseChainCheckedVehicle = 0u;
static uint32_t g_poseChainDiagCount = 0u;
static ULONGLONG g_otherPoseLastMs = 0u;

static bool ReadAppearanceMatrix(
    const void* entity, float* outMatrix, const void** outObject) {
    __try {
        const uint8_t* appearance = *reinterpret_cast<const uint8_t* const*>(
            static_cast<const uint8_t*>(entity) + kVehicleAppearanceOffset);
        if (!appearance) return false;
        const uint8_t* object = *reinterpret_cast<const uint8_t* const*>(
            appearance + kAppearanceObjectOffset);
        if (!object) return false;
        memcpy(outMatrix, object + kAppearanceMatrixOffset, 16u * sizeof(float));
        if (outObject) *outObject = object;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool PoseFromMatrix(
    const float* m, float* outPosition, float* outDirection) {
    if (!FiniteVec3(m) || !FiniteVec3(m + 4) || !FiniteVec3(m + 8) ||
        !FiniteVec3(m + 12) || g_poseForwardRow == 0u) {
        return false;
    }
    for (uint32_t row = 0u; row < 3u; ++row) {
        const float* r = m + 4u * row;
        const float len2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (!(len2 > 0.25f && len2 < 4.0f)) return false;
    }
    if (fabsf(m[12]) > 5000.0f || fabsf(m[13]) > 5000.0f ||
        fabsf(m[14]) > 5000.0f) {
        return false;
    }
    const float* forward = m + 4u * g_poseForwardRow;
    const float len = sqrtf(
        forward[0] * forward[0] + forward[1] * forward[1] +
        forward[2] * forward[2]);
    outPosition[0] = m[12];
    outPosition[1] = m[13];
    outPosition[2] = m[14];
    outDirection[0] = forward[0] / len;
    outDirection[1] = forward[1] / len;
    outDirection[2] = forward[2] / len;
    return true;
}

struct PoseCandidate {
    uint32_t id;
    void* entity;
    bool local;
};

static void PublishOtherVehiclePoses(
    ULONGLONG now, uint32_t localId, const float* localPosition, bool localValid) {
    if (now - g_otherPoseLastMs < kPoseUpdateIntervalMs) return;
    g_otherPoseLastMs = now;
    PoseCandidate candidates[kMaxLiveVehicles];
    uint32_t count = 0u;
    AcquireSRWLockShared(&g_vehicleLock);
    for (uint32_t index = 0u; index < kMaxLiveVehicles; ++index) {
        const LiveVehicleRecord& record = g_liveVehicles[index];
        if (!record.active || !record.entity || !IsVehiclePublicLocked(&record)) {
            continue;
        }
        candidates[count++] = {record.entity_id, record.entity,
                              record.entity_id == localId};
    }
    ReleaseSRWLockShared(&g_vehicleLock);
    for (uint32_t index = 0u; index < count; ++index) {
        const PoseCandidate& candidate = candidates[index];
        float m[16] = {};
        const void* object = nullptr;
        float position[3] = {};
        float direction[3] = {};
        const bool haveMatrix =
            ReadAppearanceMatrix(candidate.entity, m, &object) &&
            PoseFromMatrix(m, position, direction);
        if (g_poseChainDiagCount < kPoseChainDiagLimit) {
            ++g_poseChainDiagCount;
            char line[320] = {};
            _snprintf_s(
                line, sizeof(line), _TRUNCATE,
                "pose chain id=%u local=%u object=%p ok=%u t=(%.2f,%.2f,%.2f) "
                "fwd=(%.3f,%.3f,%.3f) verified=%u",
                candidate.id, candidate.local ? 1u : 0u, object,
                haveMatrix ? 1u : 0u, position[0], position[1], position[2],
                direction[0], direction[1], direction[2],
                g_poseChainVerified ? 1u : 0u);
            WriteLogLine("[native-v3] ", line);
        }
        if (candidate.local) {
            if (g_poseChainCheckedVehicle == candidate.id) continue;
            if (!localValid) continue;
            g_poseChainCheckedVehicle = candidate.id;
            const float dx = position[0] - localPosition[0];
            const float dy = position[1] - localPosition[1];
            const float dz = position[2] - localPosition[2];
            g_poseChainVerified =
                haveMatrix && dx * dx + dy * dy + dz * dz < 0.25f;
            char line[200] = {};
            _snprintf_s(
                line, sizeof(line), _TRUNCATE,
                "appearance chain %s for vehicle %u: chain=(%.2f,%.2f,%.2f) block=(%.2f,%.2f,%.2f)",
                g_poseChainVerified ? "verified" : "REJECTED", candidate.id,
                position[0], position[1], position[2],
                localPosition[0], localPosition[1], localPosition[2]);
            WriteLogLine("[native-v3] ", line);
            continue;
        }
        if (!g_poseChainVerified || !haveMatrix) continue;
        bool publish = false;
        AcquireSRWLockExclusive(&g_vehicleLock);
        LiveVehicleRecord* record = FindVehicleLocked(candidate.id);
        if (record && record->active && record->entity == candidate.entity &&
            record->health > 0 &&
            (record->flags & WOTBMOD_VEHICLE_DESTROYED) == 0u) {
            bool changed = !record->pose_valid;
            if (!changed) {
                const float dx = position[0] - record->position[0];
                const float dy = position[1] - record->position[1];
                const float dz = position[2] - record->position[2];
                const float dot =
                    direction[0] * record->direction[0] +
                    direction[1] * record->direction[1] +
                    direction[2] * record->direction[2];
                changed = dx * dx + dy * dy + dz * dz >= 0.0025f || dot <= 0.99985f;
            }
            if (changed) {
                memcpy(record->position, position, sizeof(record->position));
                memcpy(record->direction, direction, sizeof(record->direction));
                record->pose_valid = true;
                publish = IsVehiclePublicLocked(record);
            }
        }
        ReleaseSRWLockExclusive(&g_vehicleLock);
        if (publish) {
            UpsertPublicVehicle(candidate.id, WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
        }
    }
}

/* ---- arena roster hooks --------------------------------------------- */
/*
 * ArenaVehicleInfo, read off the live dumps of 2026-09-06 02:38 (ids
 * 5407732xx): +24 vehicle id, +28 shared_ptr to the player model (name
 * std::string at +8, display name at +56), +36 vehicle descriptor built from
 * serialized_veh_data, +40 team, +48 int64 account id, +56 std::string clan
 * tag (empty without a clan), +80 int64 clan id, +96 is_alive, +97 avatar
 * ready, +112 vector<string> boosters, +124 kills (-100 until the first
 * statistics packet). The VehicleInfo protobuf it was built from: +16
 * vehicle id, +20 team, +24 std::string* name, +28 is_alive, +32 clan id,
 * +40 int64 account id, +48 std::string* clan tag.
 */
const uint32_t kArenaVehicleInfoIdOffset = 24u;
const uint32_t kArenaVehicleInfoPlayerOffset = 28u;
const uint32_t kArenaPlayerNameOffset = 8u;
const uint32_t kArenaVehicleInfoDescrOffset = 36u;
const uint32_t kArenaVehicleInfoTeamOffset = 40u;
const uint32_t kArenaVehicleInfoAccountOffset = 48u;
const uint32_t kArenaVehicleInfoClanTagOffset = 56u;
const uint32_t kArenaVehicleInfoAliveOffset = 96u;
const uint32_t kArenaVehicleInfoKillsOffset = 124u;
const uint32_t kArenaVehicleInfoSize = 184u;
const uint32_t kVehicleInfoNameOffset = 24u;
/* the descriptor at +36 points (at +52) to the vehicle type record whose
 * std::strings are the tag (+8, "F127_ELC_AMX_901_Proto"), the user string
 * key (+32, "#france_vehicles:F127_ELC_AMX_901_Proto"), its short form
 * (+56) and the two HUD icon paths (+80, +108) - live dump 2026-09-06 02:48 */
const uint32_t kVehicleDescrTypeOffset = 52u;
const uint32_t kVehicleTypeTagOffset = 8u;
const uint32_t kVehicleTypeUserStringOffset = 32u;
const int32_t kArenaKillsUnknown = -100;
const uint32_t kVehicleStatsIdOffset = 12u;
const uint32_t kVehicleStatsKillsOffset = 16u;
const uint32_t kUpdateArenaPayloadOffset = 12u;
const uint32_t kUpdateArenaCaseOffset = 20u;
const uint32_t kUpdateArenaCaseVehicleKilled = 6u;
const uint32_t kUpdateArenaCasePlayerName = 13u;
const uint32_t kPlayerNameNewNameOffset = 12u;
const uint32_t kPlayerNameVehicleIdOffset = 16u;
const uint32_t kArenaDiagLimit = 32u;
static uint32_t g_arenaDiagCount = 0u;
static uint32_t g_arenaEventDiagCount = 0u;

/* MSVC std::string on x86: 16-byte buffer or heap pointer, size at +16,
 * capacity at +20. */
static bool ReadStdString(const uint8_t* object, char* out, size_t outSize) {
    out[0] = 0;
    __try {
        const uint32_t size = *reinterpret_cast<const uint32_t*>(object + 16);
        const uint32_t capacity = *reinterpret_cast<const uint32_t*>(object + 20);
        if (capacity < 15u || size > capacity || size > 4096u) return false;
        const char* data =
            capacity > 15u
                ? *reinterpret_cast<const char* const*>(object)
                : reinterpret_cast<const char*>(object);
        if (!data) return false;
        const size_t count = size < outSize - 1u ? size : outSize - 1u;
        memcpy(out, data, count);
        out[count] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

static bool PrintableText(const char* text) {
    if (!text[0]) return false;
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20u) return false;
    }
    return true;
}

static const uint8_t* ReadPointerAt(const uint8_t* object, uint32_t offset) {
    __try {
        return *reinterpret_cast<const uint8_t* const*>(object + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/* ---- localization: the client's own string tables ------------------- */
const uint32_t kLocalizationFilesOffset = 80u;
const uint32_t kLocalizationFilesCountOffset = 84u;
const uint32_t kStringFileMapOffset = 52u;
const uint32_t kMapNodeIsNilOffset = 13u;
const uint32_t kMapNodeKeyOffset = 16u;
const uint32_t kMapNodeValueOffset = 40u;
static uint32_t g_localizeDiagCount = 0u;

/* std::map<std::string, std::string>::find on the game's own tree: walk from
 * the root comparing bytes like std::less<std::string> does. Every read is
 * guarded; a torn tree simply yields "not found". */
static bool FindInStringMap(
    const uint8_t* mapHead, const char* key, char* out, size_t outSize) {
    const uint8_t* node = ReadPointerAt(mapHead, 4u); /* root = head->parent */
    for (uint32_t depth = 0u; node && depth < 64u; ++depth) {
        uint8_t isNil = 1u;
        __try {
            isNil = node[kMapNodeIsNilOffset];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (isNil) return false;
        char nodeKey[160] = {};
        if (!ReadStdString(node + kMapNodeKeyOffset, nodeKey, sizeof(nodeKey))) {
            return false;
        }
        const int order = strcmp(key, nodeKey);
        if (order == 0) {
            return ReadStdString(node + kMapNodeValueOffset, out, outSize) && out[0];
        }
        node = ReadPointerAt(node, order < 0 ? 0u : 8u);
    }
    return false;
}

static bool LocalizeKey(const char* key, char* out, size_t outSize) {
    out[0] = 0;
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule || !key || !key[0]) return false;
    const uint8_t* system = ReadPointerAt(
        reinterpret_cast<const uint8_t*>(gameModule), kLocalizationInstanceRva);
    if (!system || reinterpret_cast<uintptr_t>(system) < 0x10000u) return false;
    const uint8_t* head = ReadPointerAt(system, kLocalizationFilesOffset);
    uint32_t count = 0u;
    __try {
        count = *reinterpret_cast<const uint32_t*>(system + kLocalizationFilesCountOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (g_localizeDiagCount < 4u) {
        /* one-time trace of the structures the walk relies on */
        const uint8_t* first = head ? ReadPointerAt(head, 0u) : nullptr;
        const uint8_t* file0 = first ? ReadPointerAt(first, 8u) : nullptr;
        const uint8_t* map0 = file0 ? ReadPointerAt(file0, kStringFileMapOffset) : nullptr;
        const uint8_t* root = map0 ? ReadPointerAt(map0, 4u) : nullptr;
        char rootKey[96] = {};
        char path[96] = {};
        uint32_t mapSize = 0u;
        if (root) ReadStdString(root + kMapNodeKeyOffset, rootKey, sizeof(rootKey));
        if (file0) ReadStdString(file0 + 28u, path, sizeof(path));
        __try {
            if (file0) mapSize = *reinterpret_cast<const uint32_t*>(file0 + kStringFileMapOffset + 4u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mapSize = 0u;
        }
        char line[400] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "localize trace: system=%p files=%u head=%p file0=%p path=\"%s\" map0=%p size=%u root=%p rootkey=\"%s\"",
                    system, count, head, file0, path, map0, mapSize, root, rootKey);
        WriteLogLine("[native-v3] ", line);
    }
    if (!head || count == 0u || count > 64u) return false;
    const char* bare = key[0] == '#' ? key + 1 : key;
    const uint8_t* node = ReadPointerAt(head, 0u); /* first element */
    for (uint32_t index = 0u; node && node != head && index < count; ++index) {
        const uint8_t* file = ReadPointerAt(node, 8u);
        const uint8_t* map = file ? ReadPointerAt(file, kStringFileMapOffset) : nullptr;
        if (map && (FindInStringMap(map, key, out, outSize) ||
                    FindInStringMap(map, bare, out, outSize))) {
            return true;
        }
        node = ReadPointerAt(node, 0u);
    }
    return false;
}

static void RecordArenaVehicleInfo(const uint8_t* info, const uint8_t* message) {
    uint32_t vehicleId = 0u;
    uint32_t team = 0u;
    int64_t accountId = 0;
    int32_t kills = kArenaKillsUnknown;
    bool alive = false;
    char name[WOTBMOD_V3_MAX_NAME] = {};
    char clanTag[32] = {};
    bool ok = false;
    __try {
        vehicleId = *reinterpret_cast<const uint32_t*>(info + kArenaVehicleInfoIdOffset);
        team = *reinterpret_cast<const uint32_t*>(info + kArenaVehicleInfoTeamOffset);
        accountId = *reinterpret_cast<const int64_t*>(info + kArenaVehicleInfoAccountOffset);
        kills = *reinterpret_cast<const int32_t*>(info + kArenaVehicleInfoKillsOffset);
        alive = info[kArenaVehicleInfoAliveOffset] != 0u;
        ok = vehicleId != 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;
    /* the name lives in the packet; the player model the arena keeps at +28
     * carries the same string at +8 and is the fallback */
    bool hasName = false;
    const uint8_t* packetName =
        message ? ReadPointerAt(message, kVehicleInfoNameOffset) : nullptr;
    if (packetName && reinterpret_cast<uintptr_t>(packetName) > 0x10000u) {
        hasName = ReadStdString(packetName, name, sizeof(name)) && PrintableText(name);
    }
    if (!hasName) {
        const uint8_t* playerModel = ReadPointerAt(info, kArenaVehicleInfoPlayerOffset);
        if (playerModel && reinterpret_cast<uintptr_t>(playerModel) > 0x10000u) {
            hasName = ReadStdString(playerModel + kArenaPlayerNameOffset, name,
                                    sizeof(name)) &&
                      PrintableText(name);
        }
    }
    const bool hasClan =
        ReadStdString(info + kArenaVehicleInfoClanTagOffset, clanTag, sizeof(clanTag)) &&
        PrintableText(clanTag);
    /* vehicle name as "nation:tag", the nation taken from the user string
     * key ("#france_vehicles:..."), the tag from the type record */
    char vehicleName[WOTBMOD_V3_MAX_NAME] = {};
    char vehicleDisplayName[WOTBMOD_V3_MAX_NAME] = {};
    const uint8_t* descr = ReadPointerAt(info, kArenaVehicleInfoDescrOffset);
    const uint8_t* type =
        descr && reinterpret_cast<uintptr_t>(descr) > 0x10000u
            ? ReadPointerAt(descr, kVehicleDescrTypeOffset)
            : nullptr;
    if (type && reinterpret_cast<uintptr_t>(type) > 0x10000u) {
        char tag[96] = {};
        char key[128] = {};
        if (ReadStdString(type + kVehicleTypeTagOffset, tag, sizeof(tag)) &&
            PrintableText(tag)) {
            char nation[32] = {};
            if (ReadStdString(type + kVehicleTypeUserStringOffset, key, sizeof(key)) &&
                key[0] == '#') {
                const char* cut = strstr(key, "_vehicles:");
                if (cut && cut - key - 1 < static_cast<ptrdiff_t>(sizeof(nation))) {
                    memcpy(nation, key + 1, static_cast<size_t>(cut - key - 1));
                }
            }
            if (nation[0]) {
                _snprintf_s(vehicleName, sizeof(vehicleName), _TRUNCATE, "%s:%s", nation, tag);
            } else {
                strcpy_s(vehicleName, sizeof(vehicleName), tag);
            }
            if (key[0] && !LocalizeKey(key, vehicleDisplayName, sizeof(vehicleDisplayName))) {
                vehicleDisplayName[0] = 0;
            }
            if (g_localizeDiagCount < 4u) {
                ++g_localizeDiagCount;
                char line[300] = {};
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "localize: key=\"%s\" -> \"%s\"", key,
                            vehicleDisplayName[0] ? vehicleDisplayName : "(not found)");
                WriteLogLine("[native-v3] ", line);
            }
        }
    }
    AcquireSRWLockExclusive(&g_arenaLock);
    ArenaPlayerRecord* player = ReserveArenaPlayerLocked(vehicleId);
    if (hasName) {
        strcpy_s(player->name, sizeof(player->name), name);
        player->has_name = true;
    }
    if (hasClan) strcpy_s(player->clan_tag, sizeof(player->clan_tag), clanTag);
    if (vehicleName[0]) {
        strcpy_s(player->vehicle_name, sizeof(player->vehicle_name), vehicleName);
    }
    if (vehicleDisplayName[0]) {
        strcpy_s(player->vehicle_display_name, sizeof(player->vehicle_display_name),
                 vehicleDisplayName);
    }
    player->account_id = accountId;
    player->alive = alive;
    if (kills != kArenaKillsUnknown && kills >= 0) {
        player->kills = kills;
        player->has_kills = true;
    }
    ReleaseSRWLockExclusive(&g_arenaLock);
    if (g_arenaDiagCount < kArenaDiagLimit) {
        ++g_arenaDiagCount;
        char line[400] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "arena vehicle info: id=%u team=%u name=\"%s\" clan=\"%s\" account=%lld vehicle=\"%s\" display=\"%s\" alive=%u kills=%d",
                    vehicleId, team, hasName ? name : "?", hasClan ? clanTag : "",
                    accountId, vehicleName, vehicleDisplayName, alive ? 1u : 0u, kills);
        WriteLogLine("[native-v3] ", line);
    }
    if (IsPublicVehicleId(vehicleId)) {
        UpsertPublicVehicle(vehicleId, WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    }
}

static void* __fastcall ArenaAddVehicleInfoDetour(void* self, void* edx, void* info) {
    void* result = g_originalArenaAddVehicleInfo
                       ? g_originalArenaAddVehicleInfo(self, edx, info)
                       : nullptr;
    if (result) {
        RecordArenaVehicleInfo(static_cast<const uint8_t*>(result),
                               static_cast<const uint8_t*>(info));
    }
    return result;
}

static void __fastcall ArenaApplyStatsDetour(void* self, void* edx, void* stats) {
    if (g_originalArenaApplyStats) g_originalArenaApplyStats(self, edx, stats);
    uint32_t vehicleId = 0u;
    int32_t kills = 0;
    bool ok = false;
    __try {
        const uint8_t* bytes = static_cast<const uint8_t*>(stats);
        vehicleId = *reinterpret_cast<const uint32_t*>(bytes + kVehicleStatsIdOffset);
        kills = *reinterpret_cast<const int32_t*>(bytes + kVehicleStatsKillsOffset);
        ok = vehicleId != 0u && kills >= 0 && kills < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;
    AcquireSRWLockExclusive(&g_arenaLock);
    ArenaPlayerRecord* player = ReserveArenaPlayerLocked(vehicleId);
    const bool changed = !player->has_kills || player->kills != kills;
    player->kills = kills;
    player->has_kills = true;
    ReleaseSRWLockExclusive(&g_arenaLock);
    if (g_arenaEventDiagCount < 40u) {
        ++g_arenaEventDiagCount;
        char line[120] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE, "arena stats: id=%u kills=%d",
                    vehicleId, kills);
        WriteLogLine("[native-v3] ", line);
    }
    if (changed && IsPublicVehicleId(vehicleId)) {
        UpsertPublicVehicle(vehicleId, WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    }
}

static void __fastcall ArenaVehicleKilledDetour(void* self, void* edx, void* message) {
    if (g_originalArenaVehicleKilled) g_originalArenaVehicleKilled(self, edx, message);
    uint32_t victim = 0u, killer = 0u, assist = 0u, reason = 0u;
    uint8_t ammoBay = 0u;
    bool ok = false;
    __try {
        const uint8_t* wrapper = static_cast<const uint8_t*>(message);
        if (*reinterpret_cast<const uint32_t*>(wrapper + kUpdateArenaCaseOffset) ==
            kUpdateArenaCaseVehicleKilled) {
            const uint8_t* payload = *reinterpret_cast<const uint8_t* const*>(
                wrapper + kUpdateArenaPayloadOffset);
            if (payload) {
                victim = *reinterpret_cast<const uint32_t*>(payload + 12u);
                killer = *reinterpret_cast<const uint32_t*>(payload + 16u);
                assist = *reinterpret_cast<const uint32_t*>(payload + 20u);
                reason = *reinterpret_cast<const uint32_t*>(payload + 24u);
                ammoBay = payload[28u];
                ok = victim != 0u;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;
    AcquireSRWLockExclusive(&g_arenaLock);
    ArenaPlayerRecord* player = FindArenaPlayerLocked(victim);
    if (player) player->alive = false;
    ReleaseSRWLockExclusive(&g_arenaLock);
    WotbModVehicleKillEventData payload = {};
    payload.victim_id = victim;
    payload.killer_id = killer;
    payload.assist_id = assist;
    payload.reason = reason;
    payload.ammo_bay_exploded = ammoBay ? 1u : 0u;
    QueueGameplayEvent(
        WOTBMOD_EVENT_VEHICLE_KILLED, victim, killer, &payload, sizeof(payload));
    if (g_arenaEventDiagCount < 40u) {
        ++g_arenaEventDiagCount;
        char line[160] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "arena killed: victim=%u killer=%u assist=%u reason=%u ammo_bay=%u",
                    victim, killer, assist, reason, ammoBay);
        WriteLogLine("[native-v3] ", line);
    }
}

/* UpdateArena.player_name: the server renames a vehicle's player mid-battle
 * (anonymizer reveal, name change); the roster follows and the public
 * display_name is republished. */
/* LoginManager::OnHostChosen: after the client's own handler, hand the login
 * manager to the session-cluster unit (first login captures it; every later
 * login re-verifies the same pointers) and log the chosen host. */
static void __fastcall LoginOnHostChosenDetour(
    void* self, void* edx, int a2, int a3, void* host) {
    if (g_originalLoginOnHostChosen) g_originalLoginOnHostChosen(self, edx, a2, a3, host);
    if (!wotbmod::loader::session_cluster::OnHostChosen(self, host)) return;
    WotbModV3ClusterInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_SESSION_CLUSTER_VERSION;
    char line[192] = {};
    if (wotbmod::loader::session_cluster::GetCurrent(&info) == WOTBMOD_V3_OK) {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "session.cluster: host chosen %s (id %d, alive=%u allowed=%u)",
                    info.name, info.cluster_id, info.alive, info.allowed);
    } else {
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "session.cluster: host chosen %p (connection manager has no current host yet)",
                    host);
    }
    WriteLogLine("[native-v3] ", line);
}

static void SessionClusterLog(const char* message) {
    WriteLogLine("[native-v3] ", message);
}

static void __fastcall ArenaPlayerNameDetour(void* self, void* edx, void* message) {
    if (g_originalArenaPlayerName) g_originalArenaPlayerName(self, edx, message);
    uint32_t vehicleId = 0u;
    const uint8_t* nameString = nullptr;
    bool ok = false;
    __try {
        const uint8_t* wrapper = static_cast<const uint8_t*>(message);
        if (*reinterpret_cast<const uint32_t*>(wrapper + kUpdateArenaCaseOffset) ==
            kUpdateArenaCasePlayerName) {
            const uint8_t* payload = *reinterpret_cast<const uint8_t* const*>(
                wrapper + kUpdateArenaPayloadOffset);
            if (payload) {
                vehicleId = *reinterpret_cast<const uint32_t*>(
                    payload + kPlayerNameVehicleIdOffset);
                nameString = *reinterpret_cast<const uint8_t* const*>(
                    payload + kPlayerNameNewNameOffset);
                ok = vehicleId != 0u && nameString != nullptr &&
                     reinterpret_cast<uintptr_t>(nameString) > 0x10000u;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;
    char name[WOTBMOD_V3_MAX_NAME] = {};
    if (!ReadStdString(nameString, name, sizeof(name)) || !PrintableText(name)) return;
    bool changed = false;
    AcquireSRWLockExclusive(&g_arenaLock);
    ArenaPlayerRecord* player = ReserveArenaPlayerLocked(vehicleId);
    changed = !player->has_name || strcmp(player->name, name) != 0;
    strcpy_s(player->name, sizeof(player->name), name);
    player->has_name = true;
    ReleaseSRWLockExclusive(&g_arenaLock);
    if (g_arenaEventDiagCount < 40u) {
        ++g_arenaEventDiagCount;
        char line[220] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "arena player name: id=%u name=\"%s\" changed=%u", vehicleId, name,
                    changed ? 1u : 0u);
        WriteLogLine("[native-v3] ", line);
    }
    if (changed && IsPublicVehicleId(vehicleId)) {
        UpsertPublicVehicle(vehicleId, WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED);
    }
}

static void __fastcall PoseUpdateDetour(void* self, void* edx, float delta) {
    if (g_originalPoseUpdate) g_originalPoseUpdate(self, edx, delta);
    PublishLocalVehiclePose(self);
}

static uint8_t __fastcall VehicleHitDamageDetour(
    void* controller,
    void*,
    uint32_t firstEntityId,
    uint32_t secondEntityId,
    const float* position,
    const void* firstDetails,
    const void* secondDetails,
    uint8_t shellKind,
    uint8_t flags,
    uint32_t shotId) {
    WotbModHitEventData payload = {};
    payload.shot_id = shotId;
    payload.shell_id = shellKind;
    payload.flags = flags;
    bool hasPosition = false;
    if (position) {
        __try {
            payload.position.x = position[0];
            payload.position.y = position[1];
            payload.position.z = position[2];
            hasPosition =
                std::isfinite(payload.position.x) &&
                std::isfinite(payload.position.y) &&
                std::isfinite(payload.position.z);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ZeroMemory(
                &payload.position,
                sizeof(payload.position));
            hasPosition = false;
        }
    }
    if (g_originalVehicleHitDamage) {
        hobs::Before<VehicleHitDamageFn>(
            g_vehicleHitDamageTarget, controller, firstEntityId, secondEntityId, position, firstDetails, secondDetails, shellKind, flags, shotId);
    }
    const uint8_t result =
        g_originalVehicleHitDamage
            ? g_originalVehicleHitDamage(
                  controller,
                  firstEntityId,
                  secondEntityId,
                  position,
                  firstDetails,
                  secondDetails,
                  shellKind,
                  flags,
                  shotId)
            : 0;
    if (g_originalVehicleHitDamage) {
        hobs::After<VehicleHitDamageFn>(
            g_vehicleHitDamageTarget, controller, firstEntityId, secondEntityId, position, firstDetails, secondDetails, shellKind, flags, shotId);
    }
    uint32_t primary = firstEntityId;
    uint32_t other = secondEntityId;
    if (!IsPublicVehicleId(primary) &&
        IsPublicVehicleId(other)) {
        primary = other;
        other = firstEntityId;
    }
    if (!IsPublicVehicleId(primary)) {
        return result;
    }
    if (!IsPublicVehicleId(other)) {
        other = 0u;
    }
    ObservePublicVehicleRpc(
        primary, "GameSceneController.OnVehicleHitDamage");
    QueueGameplayEvent(
        WOTBMOD_EVENT_SHELL_HIT,
        primary,
        other,
        &payload,
        sizeof(payload));
    if (hasPosition) {
        const WotbModV3Vec3 impactPosition = {
            payload.position.x,
            payload.position.y,
            payload.position.z};
        WotbModV3NativeBindings_ObserveImpact(
            firstEntityId,
            secondEntityId,
            &impactPosition,
            shellKind,
            flags,
            shotId);
    }
    return result;
}

static void StripDvplSuffix(char* path) {
    if (!path) return;
    const size_t length = strlen(path);
    if (length >= 5u &&
        _stricmp(path + length - 5u, ".dvpl") == 0) {
        path[length - 5u] = '\0';
    }
    for (char* current = path; *current; ++current) {
        if (*current == '\\') *current = '/';
    }
}

static bool ReadDavaFilePath(
    const DavaFilePath32* path,
    char* output,
    size_t outputCapacity) {
    if (!path || !output || outputCapacity == 0) return false;
    output[0] = '\0';
    __try {
        const size_t length =
            path->absolute_pathname.size();
        const size_t copySize =
            length < outputCapacity - 1u
                ? length
                : outputCapacity - 1u;
        if (copySize) {
            CopyMemory(
                output,
                path->absolute_pathname.c_str(),
                copySize);
        }
        output[copySize] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

/* Every call of the original goes through here so the wotbmod.hooks
 * observers on DAVA::File::Create see each one, redirected or not. */
static void* CallDavaFileCreate(
    const DavaFilePath32* path,
    uint32_t attributes) {
    hobs::Before<DavaFileCreateFn>(g_davaFileCreateTarget, path, attributes);
    void* const result = g_originalDavaFileCreate(path, attributes);
    hobs::After<DavaFileCreateFn>(g_davaFileCreateTarget, path, attributes);
    return result;
}

static void* __cdecl DavaFileCreateDetourInner(
    const DavaFilePath32* path,
    uint32_t attributes);

static void* __cdecl DavaFileCreateDetour(
    const DavaFilePath32* path,
    uint32_t attributes) {
    const LONGLONG started = ProfNow();
    void* const result = DavaFileCreateDetourInner(path, attributes);
    ProfAdd(&g_wotbProfFileCreateTicks, ProfNow() - started);
    InterlockedIncrement64(&g_wotbProfFileCreateCalls);
    return result;
}

static void* __cdecl DavaFileCreateDetourInner(
    const DavaFilePath32* path,
    uint32_t attributes) {
    if (!g_originalDavaFileCreate || !path) {
        return g_originalDavaFileCreate
                   ? CallDavaFileCreate(path, attributes)
                   : nullptr;
    }
    static thread_local bool resolving = false;
    if (resolving) {
        return CallDavaFileCreate(path, attributes);
    }

    char requested[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!ReadDavaFilePath(
            path, requested, sizeof(requested))) {
        return CallDavaFileCreate(path, attributes);
    }

    resolving = true;
    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t resolvedSize = sizeof(resolved);
    WotbModResult result =
        WotbModRuntime_ResolveVehicleSkinPath(
            requested, resolved, &resolvedSize);
    if (result == WOTBMOD_ERROR_NOT_FOUND) {
        resolvedSize = sizeof(resolved);
        result = WotbModRuntime_ResolveResourcePath(
            requested, resolved, &resolvedSize);
    }
    resolving = false;
    if (result != WOTBMOD_OK || !resolved[0]) {
        return CallDavaFileCreate(path, attributes);
    }

    char redirectMessage[WOTBMOD_MAX_RESOURCE_PATH * 2u + 32u] = {};
    _snprintf_s(
        redirectMessage,
        sizeof(redirectMessage),
        _TRUNCATE,
        "redirect %s -> %s",
        requested,
        resolved);
    WriteLogLine("[skin] ", redirectMessage);

    StripDvplSuffix(resolved);
    DavaFilePath32 redirected(resolved);
    return CallDavaFileCreate(&redirected, attributes);
}

/*
 * The camouflage probe: two detours over the customization editor's data
 * pipeline, enabled ONLY by the marker file
 * mods\data\wotbmod_lua_host\atelier\camo_probe.on existing at loader init.
 *
 *   [camo] state this=.. a1=.. a2=.. -> <lock state>
 *       CamouflagesAccountModelImpl's per-name query (slot +0x08). The log is
 *       bounded to the first 400 calls per session; it answers "which names
 *       does the editor ask about at all".
 *
 *   [camo] filter#N args=.. in=<n> out=<m> dump=camo_filter_dump_N.bin
 *       The masked list filter (slot +0x10). Its first argument points at a
 *       {begin,end,cap} vector of element dwords; each element's pointee gets
 *       0x40 bytes captured into the dump BEFORE the original compacts the
 *       vector, so offline grepping for 'atelier_demo' inside those objects
 *       answers "did our name enter the filter" - and comparing in/out counts
 *       answers whether the mask let it through.
 *
 * When `camo_probe.on` carries no extra content (size 0 file) only logging
 * runs. If the file contains the word FAKE_OWNED, the state detour additionally
 * reports every SERVER-UNKNOWN name (state 2) as owned (1). State 2 never
 * describes anything purchasable - the server knows its own shop - so this lie
 * reaches exactly the locally authored content and nothing else.
 */
static bool g_camoProbeEnabled = false;
static bool g_camoFakeOwned = false;
static std::atomic<uint32_t> g_camoQueryLogCount{0};
static std::atomic<uint32_t> g_camoFilterCallIndex{0};
static void* g_originalCamoLockedState = nullptr;
static void* g_camoLockedStateTarget = nullptr;
static void* g_originalCamoStateFilter = nullptr;
static void* g_camoStateFilterTarget = nullptr;

/* Defined further below; forward-declared so the probe can sit beside its
 * data instead of inside the hook plumbing. Default arguments live HERE only,
 * because re-stating them at the definition is ill-formed. */
static bool HasExpectedGameplayPrologue(
    void* target,
    const uint8_t* expected,
    size_t expectedSize);
static bool InstallInternalHook(
    void* target,
    void* detour,
    void** original,
    const char* label);

typedef int(__fastcall* CamoLockedStateFn)(
    void* self, void* edx, uint32_t a1, uint32_t a2);
typedef int(__fastcall* CamoStateFilterFn)(
    void* self,
    void* edx,
    void* vectorPtr,
    uint32_t a2,
    uint32_t a3,
    uint32_t mask,
    uint32_t a5);

static int __fastcall CamoLockedStateDetour(
    void* self, void* edx, uint32_t a1, uint32_t a2) {
    const int result =
        reinterpret_cast<CamoLockedStateFn>(g_originalCamoLockedState)(
            self, edx, a1, a2);
    if (g_camoProbeEnabled && g_camoQueryLogCount.fetch_add(1) < 400u) {
        char line[128] = {};
        _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "state this=%p a1=0x%08X a2=0x%08X -> %d",
            self,
            a1,
            a2,
            result);
        WriteLogLine("[camo] ", line);
    }
    if (result == 2 && g_camoFakeOwned) return 1;
    return result;
}

/* Captures up to 256 elements' pointee bytes into one flat dump:
 * [u32 count][per element: u32 elem][0x40 bytes from elem]. Every deref is
 * SEH-guarded; an unreadable element contributes zeros rather than ending the
 * capture, because one bad slot must not cost the whole sample. */
static uint32_t CaptureFilterElements(
    void* vectorPtr,
    std::vector<uint8_t>& out) {
    uint32_t begin = 0u;
    uint32_t end = 0u;
    __try {
        begin = *reinterpret_cast<uint32_t*>(vectorPtr);
        end = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uint8_t*>(vectorPtr) + 4u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
    if (end < begin) return 0u;
    uint32_t count = (end - begin) / 4u;
    if (count > 256u) count = 256u;
    out.assign(4u + count * (4u + 0x40u), 0u);
    uint8_t* cursor = out.data();
    memcpy(cursor, &count, sizeof(count));
    cursor += 4u;
    for (uint32_t index = 0u; index < count; ++index) {
        uint32_t element = 0u;
        __try {
            element = reinterpret_cast<uint32_t*>(begin)[index];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            element = 0u;
        }
        memcpy(cursor, &element, sizeof(element));
        cursor += 4u;
        if (element >= 0x10000u && element < 0x7FFF0000u) {
            __try {
                memcpy(cursor, reinterpret_cast<const void*>(element), 0x40u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                /* leave zeros */
            }
        }
        cursor += 0x40u;
    }
    return count;
}

static int __fastcall CamoStateFilterDetour(
    void* self,
    void* edx,
    void* vectorPtr,
    uint32_t a2,
    uint32_t a3,
    uint32_t mask,
    uint32_t a5) {
    std::vector<uint8_t> inputDump;
    uint32_t inputCount = 0u;
    if (g_camoProbeEnabled && vectorPtr &&
        g_camoFilterCallIndex.load() < 24u) {
        inputCount = CaptureFilterElements(vectorPtr, inputDump);
    }
    const int result =
        reinterpret_cast<CamoStateFilterFn>(g_originalCamoStateFilter)(
            self, edx, vectorPtr, a2, a3, mask, a5);
    if (!g_camoProbeEnabled || !vectorPtr ||
        g_camoFilterCallIndex.fetch_add(1) + 1u > 24u) {
        return result;
    }
    const uint32_t callIndex = g_camoFilterCallIndex.load();
    char line[160] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "filter#%u self=%p vec=%p args=%08X,%08X mask=%08X,%08X in=%u -> %d",
        callIndex,
        self,
        vectorPtr,
        a2,
        a3,
        mask,
        a5,
        inputCount,
        result);
    WriteLogLine("[camo] ", line);
    if (!inputDump.empty()) {
        char path[MAX_PATH] = {};
        _snprintf_s(
            path,
            sizeof(path),
            _TRUNCATE,
            "camo_filter_dump_%02u.bin",
            callIndex);
        HANDLE file = CreateFileA(
            path,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0u;
            WriteFile(
                file,
                inputDump.data(),
                static_cast<DWORD>(inputDump.size()),
                &written,
                nullptr);
            CloseHandle(file);
        }
    }
    return result;
}

static bool InstallCamoProbeHooks() {
    /* Marker-file gated AND fingerprint gated, same as every fixed-RVA hook:
     * without both, none of this code touches the client. */
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule || !g_nativeFingerprintVerified) return false;
    char exePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(gameModule, exePath, MAX_PATH)) return false;
    char markerPath[MAX_PATH] = {};
    {
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) return false;
        const size_t dirLength =
            static_cast<size_t>(lastSlash - exePath);
        if (dirLength == 0u ||
            dirLength + 60u >= sizeof(markerPath)) {
            return false;
        }
        memcpy(markerPath, exePath, dirLength);
        memcpy(
            markerPath + dirLength,
            "\\mods\\data\\wotbmod_lua_host\\atelier\\camo_probe.on",
            53u);
        markerPath[dirLength + 53u] = '\0';
    }
    HANDLE marker = CreateFileA(
        markerPath,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (marker == INVALID_HANDLE_VALUE) return false;
    char content[16] = {};
    DWORD read = 0u;
    ReadFile(marker, content, sizeof(content) - 1u, &read, nullptr);
    CloseHandle(marker);
    g_camoProbeEnabled = true;
    content[read] = '\0';
    g_camoFakeOwned =
        strstr(content, "FAKE_OWNED") != nullptr;
    WriteLogLine(
        "[camo] ",
        g_camoFakeOwned
            ? "probe enabled: logging + fake-owned(state2->1)"
            : "probe enabled: logging only");

    const uint8_t prologue[] = {0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu};
    uint8_t* base = reinterpret_cast<uint8_t*>(gameModule);
    g_camoLockedStateTarget = base + kCamouflageLockedStateRva;
    g_camoStateFilterTarget = base + kCamouflagesStateFilterRva;
    const bool lockedOk = HasExpectedGameplayPrologue(
        g_camoLockedStateTarget, prologue, sizeof(prologue));
    const bool filterOk = HasExpectedGameplayPrologue(
        g_camoStateFilterTarget, prologue, sizeof(prologue));
    if (!lockedOk || !filterOk) {
        WriteLogLine(
            "[loader] ",
            "camouflage probe disabled: function prologue mismatch");
        return false;
    }
    const bool installedLocked = InstallInternalHook(
        g_camoLockedStateTarget,
        reinterpret_cast<void*>(&CamoLockedStateDetour),
        reinterpret_cast<void**>(&g_originalCamoLockedState),
        "camouflage state query hook failed");
    const bool installedFilter = InstallInternalHook(
        g_camoStateFilterTarget,
        reinterpret_cast<void*>(&CamoStateFilterDetour),
        reinterpret_cast<void**>(&g_originalCamoStateFilter),
        "camouflage filter hook failed");
    return installedLocked || installedFilter;
}

static WotbModResult MapHookStatus(MH_STATUS status) {
    switch (status) {
        case MH_OK:
            return WOTBMOD_OK;
        case MH_ERROR_ALREADY_CREATED:
        case MH_ERROR_ENABLED:
            return WOTBMOD_ERROR_ALREADY_EXISTS;
        case MH_ERROR_NOT_CREATED:
            return WOTBMOD_ERROR_NOT_FOUND;
        case MH_ERROR_MEMORY_ALLOC:
            return WOTBMOD_ERROR_LIMIT_REACHED;
        case MH_ERROR_NOT_EXECUTABLE:
        case MH_ERROR_UNSUPPORTED_FUNCTION:
            return WOTBMOD_ERROR_PLATFORM;
        default:
            return WOTBMOD_ERROR_ACCESS_DENIED;
    }
}

static WotbModV3Result MapV3HookStatus(MH_STATUS status) {
    switch (status) {
        case MH_OK:
            return WOTBMOD_V3_OK;
        case MH_ERROR_ALREADY_CREATED:
            return WOTBMOD_V3_E_CONFLICT;
        case MH_ERROR_ENABLED:
            return WOTBMOD_V3_E_ALREADY_EXISTS;
        case MH_ERROR_NOT_CREATED:
            return WOTBMOD_V3_E_NOT_FOUND;
        case MH_ERROR_MEMORY_ALLOC:
            return WOTBMOD_V3_E_LIMIT_REACHED;
        case MH_ERROR_NOT_EXECUTABLE:
        case MH_ERROR_UNSUPPORTED_FUNCTION:
            return WOTBMOD_V3_E_PLATFORM;
        default:
            return WOTBMOD_V3_E_PLATFORM;
    }
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookResolveSymbol(
    void*,
    const char* symbol,
    void** outTarget) {
    return WotbModV3NativeBindings_ResolveHookSymbol(
        symbol,
        outTarget);
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookCreate(
    void*,
    void* target,
    void* detour,
    void** original) {
    if (!target || !detour || !original) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    HMODULE gameModule = GetModuleHandleA(nullptr);
    bool belongsToGame = false;
    __try {
        const uint8_t* base =
            reinterpret_cast<const uint8_t*>(gameModule);
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);
        const uint8_t* address =
            reinterpret_cast<const uint8_t*>(target);
        belongsToGame =
            dos->e_magic == IMAGE_DOS_SIGNATURE &&
            nt->Signature == IMAGE_NT_SIGNATURE &&
            address >= base &&
            address < base + nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        belongsToGame = false;
    }
    if (!belongsToGame) {
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    return MapV3HookStatus(
        MH_CreateHook(target, detour, original));
}

/* wotbmod.hooks OBSERVE/AFTER: the runtime attaches mod callbacks to the
 * observer points this loader registers inside its own detours
 * (v3_hook_observers.h); see the 2026-09-04 hook-observers spec. */
static WotbModV3Result WOTBMOD_V3_CALL V3HookDescribeTarget(
    void*,
    void* target,
    uint32_t* outMask) {
    if (!target || !outMask) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *outMask = hobs::Describe(target);
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookAttach(
    void*,
    void* target,
    uint32_t mode,
    int32_t priority,
    void* detour,
    uint64_t* outToken) {
    return hobs::Attach(target, mode, priority, detour, outToken);
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookDetach(void*, uint64_t token) {
    return hobs::Detach(token);
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookSetAttachedEnabled(
    void*,
    uint64_t token,
    uint32_t enabled) {
    return hobs::SetEnabled(token, enabled != 0u);
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookEnable(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    return MapV3HookStatus(MH_EnableHook(target));
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookDisable(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const MH_STATUS status = MH_DisableHook(target);
    return status == MH_ERROR_DISABLED
               ? WOTBMOD_V3_OK
               : MapV3HookStatus(status);
}

static WotbModV3Result WOTBMOD_V3_CALL V3HookRemove(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    return MapV3HookStatus(MH_RemoveHook(target));
}

static bool ClientEventRuntimeReady() {
    return InterlockedCompareExchange(
               &g_runtimeReady, 0, 0) == 2 &&
           g_davaResourcesHandle &&
           g_liveResourceBackend.release;
}

static void ReleaseLiveResource(void* nativeResource) {
    if (!nativeResource || !g_liveResourceBackend.release) return;
    g_liveResourceBackend.release(
        g_liveResourceBackend.user_data,
        nativeResource);
}

static void NotifyTrackedSceneActivated(void* scene) {
    if (!scene ||
        !ClientEventRuntimeReady() ||
        InterlockedCompareExchangePointer(
            &g_lastNotifiedScene,
            nullptr,
            nullptr) == scene ||
        !g_liveResourceBackend.scene_get_active) {
        return;
    }
    void* sceneResource = nullptr;
    const WotbModResult getResult =
        g_liveResourceBackend.scene_get_active(
            g_liveResourceBackend.user_data,
            &sceneResource);
    if (getResult != WOTBMOD_OK || !sceneResource) return;
    const WotbModResult notifyResult =
        WotbModRuntime_NotifySceneActivated(sceneResource);
    ReleaseLiveResource(sceneResource);
    if (notifyResult == WOTBMOD_OK) {
        InterlockedExchangePointer(
            &g_lastNotifiedScene,
            scene);
    }
}

static void __fastcall SceneDrawDetour(void* scene, void*) {
    if (g_davaResourcesHandle && scene) {
        const WotbModResult trackResult =
            WotbModDavaResources_TrackActiveScene(
            g_davaResourcesHandle, scene);
        /*
         * EVERY DISTINCT SCENE, not just the first draw.
         *
         * `TrackActiveScene` above overwrites the tracked scene on every draw,
         * so a mod asking for "the active scene" gets whichever was drawn last.
         * A live probe kept receiving a 242-node hangar with no tank in it, and
         * the question that could not be answered from the log was whether the
         * client draws ONE scene or several - because only the first was ever
         * reported.
         *
         * Eight is a fixed table with no allocation and no lock beyond the
         * interlocked claim: this runs inside the draw path of every frame, and
         * a diagnostic that costs a lock there is a diagnostic that changes
         * what it measures.
         */
        {
            static void* volatile seen[8] = {};
            bool known = false;
            for (size_t slot = 0u; slot < 8u; ++slot) {
                void* current = InterlockedCompareExchangePointer(
                    &seen[slot], nullptr, nullptr);
                if (current == scene) { known = true; break; }
                if (current == nullptr) {
                    if (InterlockedCompareExchangePointer(
                            &seen[slot], scene, nullptr) == nullptr) {
                        char status[160] = {};
                        _snprintf_s(
                            status,
                            sizeof(status),
                            _TRUNCATE,
                            "Scene::Draw distinct scene #%u = %p track=%d",
                            static_cast<unsigned>(slot),
                            scene,
                            static_cast<int>(trackResult));
                        WriteLogLine("[loader] ", status);
                    }
                    known = true;
                    break;
                }
            }
            (void)known;
        }
        (void)InterlockedIncrement(&g_sceneDrawHits);
        if (trackResult == WOTBMOD_OK) {
            NotifyTrackedSceneActivated(scene);
        }
    }
    if (g_originalSceneDraw) {
        const LONGLONG before = ProfNow();
        hobs::Before<SceneDrawFn>(g_sceneDrawTarget, scene);
        const LONGLONG enter = ProfNow();
        g_originalSceneDraw(scene);
        const LONGLONG leave = ProfNow();
        hobs::After<SceneDrawFn>(g_sceneDrawTarget, scene);
        ProfAdd(&g_wotbProfSceneDrawTicks, (enter - before) + (ProfNow() - leave));
    }
}

static void __fastcall SceneActivateDetour(void* scene, void*) {
    if (g_originalSceneActivate) {
        hobs::Before<SceneDrawFn>(g_sceneActivateTarget, scene);
        g_originalSceneActivate(scene);
        hobs::After<SceneDrawFn>(g_sceneActivateTarget, scene);
    }
    const WotbModResult trackResult =
        WotbModDavaResources_TrackActiveScene(
            g_davaResourcesHandle, scene);
    if (trackResult == WOTBMOD_OK) {
        NotifyTrackedSceneActivated(scene);
    }
    if (InterlockedIncrement(&g_sceneActivationHits) == 1) {
        char status[160] = {};
        _snprintf_s(
            status,
            sizeof(status),
            _TRUNCATE,
            "Scene::Activate first hit scene=%p track=%d",
            scene,
            static_cast<int>(trackResult));
        WriteLogLine("[loader] ", status);
    }
}

static void __fastcall SceneDeactivateDetour(void* scene, void*) {
    if (scene &&
        ClientEventRuntimeReady() &&
        InterlockedCompareExchangePointer(
            &g_lastNotifiedScene,
            nullptr,
            nullptr) == scene &&
        g_liveResourceBackend.scene_get_active) {
        void* sceneResource = nullptr;
        if (g_liveResourceBackend.scene_get_active(
                g_liveResourceBackend.user_data,
                &sceneResource) == WOTBMOD_OK &&
            sceneResource) {
            WotbModRuntime_NotifySceneDeactivated(sceneResource);
            ReleaseLiveResource(sceneResource);
        }
    }
    InterlockedCompareExchangePointer(
        &g_lastNotifiedScene,
        nullptr,
        scene);
    if (g_originalSceneDeactivate) {
        hobs::Before<SceneDrawFn>(g_sceneDeactivateTarget, scene);
        g_originalSceneDeactivate(scene);
        hobs::After<SceneDrawFn>(g_sceneDeactivateTarget, scene);
    }
    WotbModDavaResources_UntrackActiveScene(
        g_davaResourcesHandle, scene);
}

static bool InstallInternalHook(
    void* target,
    void* detour,
    void** original,
    const char* label) {
    if (!target || !detour || !original) {
        WriteLogLine(
            "[loader] ",
            label ? label : "internal hook target validation failed");
        return false;
    }
    const MH_STATUS createStatus = MH_CreateHook(
        target,
        detour,
        original);
    if (createStatus != MH_OK) {
        WriteLogLine(
            "[loader] ",
            label ? label : "internal hook creation failed");
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
        WriteLogLine(
            "[loader] ",
            label ? label : "internal hook enable failed");
        return false;
    }
    return true;
}

static bool HasExpectedGameplayPrologue(
    void* target,
    const uint8_t* expected = nullptr,
    size_t expectedSize = 0) {
    if (!target) return false;
    const uint8_t defaultExpected[] = {
        0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu};
    if (!expected || expectedSize == 0) {
        expected = defaultExpected;
        expectedSize = sizeof(defaultExpected);
    }
    __try {
        return memcmp(target, expected, expectedSize) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InstallGameplayHook(
    void* target,
    void* detour,
    void** original,
    const char* label,
    const uint8_t* expected = nullptr,
    size_t expectedSize = 0) {
    if (!HasExpectedGameplayPrologue(
            target, expected, expectedSize)) {
        WriteLogLine(
            "[loader] ",
            label ? label : "gameplay hook signature mismatch");
        return false;
    }
    return InstallInternalHook(target, detour, original, label);
}

static uint8_t* FindDavaFileCreateBySignature(HMODULE gameModule) {
    if (!gameModule) return nullptr;
    __try {
        uint8_t* base = reinterpret_cast<uint8_t*>(gameModule);
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        const uint8_t prefixA[] = {
            0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu, 0x68u};
        const uint8_t prefixB[] = {
            0x64u, 0xA1u, 0x00u, 0x00u, 0x00u, 0x00u,
            0x50u, 0x81u, 0xECu, 0x38u, 0x02u, 0x00u, 0x00u};
        const uint8_t prefixC[] = {
            0x33u, 0xC5u, 0x89u, 0x45u, 0xF0u, 0x56u, 0x57u,
            0x50u, 0x8Du, 0x45u, 0xF4u, 0x64u, 0xA3u,
            0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0x35u};
        const uint8_t prefixD[] = {
            0x8Bu, 0x7Du, 0x0Cu, 0x8Du, 0x8Du, 0xD8u, 0xFDu,
            0xFFu, 0xFFu, 0x8Bu, 0x75u, 0x08u, 0x6Au, 0x25u,
            0x68u};

        const IMAGE_SECTION_HEADER* section =
            IMAGE_FIRST_SECTION(nt);
        for (WORD index = 0;
             index < nt->FileHeader.NumberOfSections;
             ++index, ++section) {
            if ((section->Characteristics & IMAGE_SCN_CNT_CODE) == 0) {
                continue;
            }
            uint8_t* begin = base + section->VirtualAddress;
            const size_t size =
                static_cast<size_t>(section->Misc.VirtualSize);
            if (size < 72u) continue;
            for (size_t offset = 0; offset + 72u <= size; ++offset) {
                uint8_t* candidate = begin + offset;
                if (memcmp(candidate, prefixA, sizeof(prefixA)) != 0 ||
                    memcmp(
                        candidate + 10u,
                        prefixB,
                        sizeof(prefixB)) != 0 ||
                    candidate[23] != 0xA1u ||
                    memcmp(
                        candidate + 28u,
                        prefixC,
                        sizeof(prefixC)) != 0 ||
                    memcmp(
                        candidate + 51u,
                        prefixD,
                        sizeof(prefixD)) != 0) {
                    continue;
                }
                const char* label =
                    *reinterpret_cast<const char* const*>(
                        candidate + 66u);
                if (label && strcmp(label, "File::Create") == 0) {
                    return candidate;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

/* Own frame on purpose: __try may not share one with unwindable C++ objects. */
static void FindGameDataSection(
    const uint8_t* base, const uint8_t** outBegin, const uint8_t** outEnd) {
    *outBegin = nullptr;
    *outEnd = nullptr;
    __try {
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0u; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (memcmp(section->Name, ".data\0\0\0", 8) == 0) {
                *outBegin = base + section->VirtualAddress;
                *outEnd = *outBegin + section->Misc.VirtualSize;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outBegin = nullptr;
        *outEnd = nullptr;
    }
}

static void ObserverLog(const char* message) {
    WriteLogLine("[loader] ", message);
}

static bool InstallGameplayHooks() {
    g_eventSourceMask &= ~kGameplayEventSourceMask;
    InterlockedExchange(&g_gameplayHooksInstalled, 0);
    WotbModV3NativeBindings_SetGameplayBridgeSources(0u);
    WotbModV3Runtime_SetEventSourceMask(g_eventSourceMask);
    if (g_v3NativeBindingsResult != WOTBMOD_V3_OK ||
        g_v3ClientBackend.compatibility_state ==
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        WriteLogLine(
            "[loader] ",
            "gameplay hooks disabled: client fingerprint was not verified");
        return false;
    }
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule) {
        WriteLogLine(
            "[loader] ",
            "gameplay hooks unavailable: game module missing");
        return false;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(gameModule);
    g_onEnterWorldTarget = base + kOnEnterWorldRva;
    g_onLeaveWorldTarget = base + kOnLeaveWorldRva;
    g_showShootingTarget = base + kShowShootingRva;
    g_setHealthTarget = base + kSetHealthRva;
    g_updateVehicleHealthTarget =
        base + kUpdateVehicleHealthRva;
    g_reloadSetStateTarget = base + kReloadSetStateRva;
    g_uiSystemInputTarget = base + kUiControlSystemInputRva;
    g_ammoChangedTarget = base + kAmmoChangedRva;
    g_aimTargetSetTarget = base + kAimTargetSetRva;
    g_aimTargetClearTarget = base + kAimTargetClearRva;
    g_observedStatusTarget = base + kObservedStatusRva;
    g_vehicleHitDamageTarget =
        base + kVehicleHitDamageRva;
    g_poseUpdateTarget = base + kPlayerControllerPoseUpdateRva;
    g_arenaAddVehicleInfoTarget = base + kArenaAddVehicleInfoRva;
    g_arenaApplyStatsTarget = base + kArenaApplyVehicleStatisticsRva;
    g_arenaVehicleKilledTarget = base + kArenaOnVehicleKilledRva;
    g_arenaPlayerNameTarget = base + kArenaOnPlayerNameRva;
    g_loginOnHostChosenTarget = base + kLoginManagerOnHostChosenRva;

    uint32_t installedMask = 0;
    hobs::SetLog(&ObserverLog);
    if (InstallGameplayHook(
            g_onEnterWorldTarget,
            reinterpret_cast<void*>(&OnEnterWorldDetour),
            reinterpret_cast<void**>(&g_originalOnEnterWorld),
            "Vehicle::onEnterWorld hook failed")) {
        installedMask |= 1u << 0;
        hobs::RegisterTarget(g_onEnterWorldTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_onLeaveWorldTarget,
            reinterpret_cast<void*>(&OnLeaveWorldDetour),
            reinterpret_cast<void**>(&g_originalOnLeaveWorld),
            "Vehicle::onLeaveWorld hook failed")) {
        installedMask |= 1u << 1;
        hobs::RegisterTarget(g_onLeaveWorldTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_showShootingTarget,
            reinterpret_cast<void*>(&ShowShootingDetour),
            reinterpret_cast<void**>(&g_originalShowShooting),
            "Vehicle::showShooting hook failed")) {
        installedMask |= 1u << 2;
        hobs::RegisterTarget(g_showShootingTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_setHealthTarget,
            reinterpret_cast<void*>(&SetHealthDetour),
            reinterpret_cast<void**>(&g_originalSetHealth),
            "Vehicle::set_health hook failed")) {
        installedMask |= 1u << 3;
        hobs::RegisterTarget(g_setHealthTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_updateVehicleHealthTarget,
            reinterpret_cast<void*>(&UpdateVehicleHealthDetour),
            reinterpret_cast<void**>(&g_originalUpdateVehicleHealth),
            "Avatar::updateVehicleHealth hook failed")) {
        installedMask |= 1u << 4;
        hobs::RegisterTarget(g_updateVehicleHealthTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_reloadSetStateTarget,
            reinterpret_cast<void*>(&ReloadSetStateDetour),
            reinterpret_cast<void**>(&g_originalReloadSetState),
            "ReloadTimer::setState hook failed")) {
        installedMask |= 1u << 5;
        hobs::RegisterTarget(g_reloadSetStateTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_uiSystemInputTarget,
            reinterpret_cast<void*>(&UiSystemInputDetour),
            reinterpret_cast<void**>(&g_originalUiSystemInput),
            "UIControl::SystemInput hook failed")) {
        installedMask |= 1u << 6;
        hobs::RegisterTarget(g_uiSystemInputTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_ammoChangedTarget,
            reinterpret_cast<void*>(&AmmoChangedDetour),
            reinterpret_cast<void**>(&g_originalAmmoChanged),
            "UIShellSelectorControl::OnCurrentAmmoChanged hook failed")) {
        installedMask |= 1u << 7;
        hobs::RegisterTarget(g_ammoChangedTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    const uint8_t aimSetPrologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x53u, 0x8Bu, 0x5Du, 0x08u};
    if (InstallGameplayHook(
            g_aimTargetSetTarget,
            reinterpret_cast<void*>(&AimTargetSetDetour),
            reinterpret_cast<void**>(&g_originalAimTargetSet),
            "AvatarGameLogic aim-target set hook failed",
            aimSetPrologue,
            sizeof(aimSetPrologue))) {
        installedMask |= 1u << 8;
    }
    const uint8_t aimClearPrologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x57u, 0x8Bu, 0xF9u};
    if (InstallGameplayHook(
            g_aimTargetClearTarget,
            reinterpret_cast<void*>(&AimTargetClearDetour),
            reinterpret_cast<void**>(&g_originalAimTargetClear),
            "AvatarGameLogic aim-target clear hook failed",
            aimClearPrologue,
            sizeof(aimClearPrologue))) {
        installedMask |= 1u << 9;
    }
    if (InstallGameplayHook(
            g_observedStatusTarget,
            reinterpret_cast<void*>(&ObservedStatusDetour),
            reinterpret_cast<void**>(&g_originalObservedStatus),
            "ClientArena OBSERVED_STATUS hook failed")) {
        installedMask |= 1u << 10;
    }
    if (InstallGameplayHook(
            g_vehicleHitDamageTarget,
            reinterpret_cast<void*>(&VehicleHitDamageDetour),
            reinterpret_cast<void**>(&g_originalVehicleHitDamage),
            "GameSceneController::OnVehicleHitDamage hook failed")) {
        installedMask |= 1u << 11;
        hobs::RegisterTarget(g_vehicleHitDamageTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (InstallGameplayHook(
            g_poseUpdateTarget,
            reinterpret_cast<void*>(&PoseUpdateDetour),
            reinterpret_cast<void**>(&g_originalPoseUpdate),
            "PlayerController pose update hook failed")) {
        installedMask |= 1u << 13;
        WriteLogLine(
            "[native-v3] ",
            "PlayerController pose hook installed; local vehicle position/direction enabled");
    }
    if (InstallGameplayHook(
            g_arenaAddVehicleInfoTarget,
            reinterpret_cast<void*>(&ArenaAddVehicleInfoDetour),
            reinterpret_cast<void**>(&g_originalArenaAddVehicleInfo),
            "ClientArena vehicle info hook failed")) {
        installedMask |= 1u << 14;
    }
    if (InstallGameplayHook(
            g_arenaApplyStatsTarget,
            reinterpret_cast<void*>(&ArenaApplyStatsDetour),
            reinterpret_cast<void**>(&g_originalArenaApplyStats),
            "ClientArena vehicle statistics hook failed")) {
        installedMask |= 1u << 15;
    }
    if (InstallGameplayHook(
            g_arenaVehicleKilledTarget,
            reinterpret_cast<void*>(&ArenaVehicleKilledDetour),
            reinterpret_cast<void**>(&g_originalArenaVehicleKilled),
            "ClientArena vehicle killed hook failed")) {
        installedMask |= 1u << 16;
    }
    if (InstallGameplayHook(
            g_arenaPlayerNameTarget,
            reinterpret_cast<void*>(&ArenaPlayerNameDetour),
            reinterpret_cast<void**>(&g_originalArenaPlayerName),
            "ClientArena player name hook failed")) {
        installedMask |= 1u << 17;
    }

    /* Login cluster switch: capture LoginManager on OnHostChosen, call the
     * client's own ChangeCluster later. Both prologues must match. */
    if (g_nativeFingerprintVerified &&
        HasExpectedGameplayPrologue(base + kLoginManagerChangeClusterRva) &&
        HasExpectedGameplayPrologue(base + kConnectionManagerDisconnectRva)) {
        wotbmod::loader::session_cluster::Layout layout = {};
        layout.services_offset = kLoginManagerServicesOffset;
        layout.cluster_to_login_offset = kLoginManagerClusterToLoginOffset;
        layout.manual_flag_offset = kLoginManagerManualClusterOffset;
        layout.services_appctx_slot = kServicesAppCtxSlotOffset;
        layout.services_connection_slot = kServicesConnectionManagerSlotOffset;
        layout.services_login_slot = kServicesLoginManagerSlotOffset;
        layout.appctx_owner_offset = kAppCtxOwnerOffset;
        layout.appctx_region_offset = kAppCtxRegionOffset;
        layout.connection_current_host_offset = kConnectionManagerCurrentHostOffset;
        layout.host_stride = kClusterHostStrideOffset;
        layout.host_descriptor_offset = kClusterHostDescriptorOffset;
        layout.host_addresses_begin_offset = kClusterHostAddressesBeginOffset;
        layout.host_addresses_end_offset = kClusterHostAddressesEndOffset;
        layout.host_state_offset = kClusterHostStateOffset;
        layout.address_stride = kClusterAddressStrideOffset;
        layout.address_ping_offset = kClusterAddressPingOffset;
        layout.descriptor_name_offset = kClusterDescriptorNameOffset;
        layout.descriptor_url_offset = kClusterDescriptorUrlOffset;
        layout.descriptor_id_offset = kClusterDescriptorIdOffset;
        wotbmod::loader::session_cluster::Init(
            layout,
            reinterpret_cast<wotbmod::loader::session_cluster::ChangeClusterFn>(
                base + kLoginManagerChangeClusterRva),
            reinterpret_cast<wotbmod::loader::session_cluster::DisconnectFn>(
                base + kConnectionManagerDisconnectRva),
            &SessionClusterLog);
        if (InstallGameplayHook(
                g_loginOnHostChosenTarget,
                reinterpret_cast<void*>(&LoginOnHostChosenDetour),
                reinterpret_cast<void**>(&g_originalLoginOnHostChosen),
                "LoginManager::OnHostChosen hook failed")) {
            installedMask |= 1u << 18;
            g_sessionClusterInstalled = true;
        }
    } else {
        WriteLogLine(
            "[loader] ",
            "session.cluster backend not installed: fingerprint or ChangeCluster prologue check failed");
    }
    if ((installedMask & (7u << 14)) == (7u << 14)) {
        WriteLogLine(
            "[native-v3] ",
            "ClientArena roster hooks installed; player names, account ids and kills enabled");
    }

    /* GES bus: type table from .data, engine entry points from anchors, one
     * capture hook on SubscribeImpl. Everything else is direct calls guarded
     * by GesReady(); see loader/v3_native_ges.h. */
    if (g_nativeFingerprintVerified) {
        const uint8_t* dataBegin = nullptr;
        const uint8_t* dataEnd = nullptr;
        FindGameDataSection(base, &dataBegin, &dataEnd);
        wotbmod::loader::ges::GesEngine engine = {};
        engine.image_base = base;
        engine.get_listeners =
            reinterpret_cast<wotbmod::loader::ges::GesGetListenersFn>(
                base + kGesGetListenersRva);
        engine.get_or_create_list =
            reinterpret_cast<wotbmod::loader::ges::GesGetOrCreateListFn>(
                base + kGesGetOrCreateListRva);
        engine.list_factory =
            reinterpret_cast<wotbmod::loader::ges::GesListFactoryFn>(
                base + kGesListenerListFactoryRva);
        engine.list_add =
            reinterpret_cast<wotbmod::loader::ges::GesListAddFn>(
                base + kGesListenerListAddRva);
        engine.list_erase =
            reinterpret_cast<wotbmod::loader::ges::GesListEraseFn>(
                base + kGesListenerListEraseRva);
        g_gesSubscribeImplTarget = base + kGesSubscribeImplRva;
        static const uint8_t kGetListenersPrologue[] = {
            0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x45u, 0x08u};
        const bool prologuesOk =
            HasExpectedGameplayPrologue(
                reinterpret_cast<void*>(engine.get_or_create_list)) &&
            HasExpectedGameplayPrologue(
                reinterpret_cast<void*>(engine.list_factory)) &&
            HasExpectedGameplayPrologue(
                reinterpret_cast<void*>(engine.list_add)) &&
            HasExpectedGameplayPrologue(
                reinterpret_cast<void*>(engine.list_erase)) &&
            HasExpectedGameplayPrologue(
                reinterpret_cast<void*>(engine.get_listeners),
                kGetListenersPrologue,
                sizeof(kGetListenersPrologue));
        if (dataBegin && prologuesOk &&
            wotbmod::loader::ges::GesInit(
                engine, dataBegin, dataEnd, &GesDeliverToRuntime, nullptr,
                &GesLog) &&
            InstallGameplayHook(
                g_gesSubscribeImplTarget,
                reinterpret_cast<void*>(&GesSubscribeImplDetour),
                reinterpret_cast<void**>(&g_originalGesSubscribeImpl),
                "GES::GameEventSystem::SubscribeImpl hook failed")) {
            g_gesInstalled = true;
            installedMask |= 1u << 12;
        } else {
            WriteLogLine(
                "[loader] ",
                "GES bus backend not installed: prologue, .data or init check failed");
        }
    }

    InterlockedExchange(
        &g_gameplayHooksInstalled,
        static_cast<LONG>(installedMask));
    const uint32_t rpcSourceMask =
        installedMask &
        ((1u << 0) | (1u << 1) | (1u << 2) |
         (1u << 3) | (1u << 4) | (1u << 10) |
         (1u << 11));
    WotbModV3NativeBindings_SetGameplayBridgeSources(
        rpcSourceMask);
    uint64_t eventSourceMask = 0u;
    if ((installedMask & (1u << 0)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD;
    }
    if ((installedMask & (1u << 1)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD;
    }
    if ((installedMask & (1u << 2)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING;
    }
    if ((installedMask & (1u << 3)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH;
    }
    if ((installedMask & (1u << 4)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE;
    }
    if ((installedMask & (1u << 5)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_RELOAD_STATE;
    }
    if ((installedMask & (1u << 6)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_UI_INPUT;
    }
    if ((installedMask & (1u << 7)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_AMMO_CHANGED;
    }
    if ((installedMask & ((1u << 8) | (1u << 9))) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_AIM_TARGET;
    }
    if ((installedMask & (1u << 10)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS;
    }
    if ((installedMask & (1u << 11)) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT;
    }
    const uint32_t nativeSourceMask =
        WotbModV3NativeBindings_GetInstalledSourceMask();
    if ((nativeSourceMask &
         WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_MODE) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE;
    }
    // Only the ShowTracer hook can announce a visible tracer, so the public
    // ingress bit follows that hook and not the manager ctor.
    if ((nativeSourceMask &
         WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_TRACER_SHOW) != 0u) {
        eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_VISIBLE_TRACER;
    }
    if ((installedMask & (1u << 12)) != 0u) {
        eventSourceMask |= WOTBMOD_V3_EVENT_SOURCE_GES;
    }
    if ((installedMask & (1u << 16)) != 0u) {
        eventSourceMask |= WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED;
    }
    if ((installedMask & (1u << 18)) != 0u) {
        eventSourceMask |= WOTBMOD_V3_EVENT_SOURCE_SESSION_CLUSTER;
    }
    g_eventSourceMask |= eventSourceMask;
    WotbModV3Runtime_SetEventSourceMask(g_eventSourceMask);
    char status[160] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "gameplay hooks installed mask=0x%05X (%u/19)",
        installedMask,
        static_cast<unsigned int>([](
            uint32_t value) {
                uint32_t count = 0;
                while (value) {
                    count += value & 1u;
                    value >>= 1u;
                }
                return count;
            }(installedMask)));
    WriteLogLine("[loader] ", status);

    const uint32_t registryMask = (1u << 0) | (1u << 1);
    return (installedMask & registryMask) == registryMask;
}

static bool InstallDavaFileResolverHook() {
    /*
     * The RVA below, and the signature scan used as its fallback, are both
     * build-specific: 55 8B EC 6A FF is the stock MSVC prologue and matches
     * thousands of functions, so on an unverified build the scan would
     * happily bind the wrong one. Fingerprint first.
     */
    if (!g_nativeFingerprintVerified) {
        WriteLogLine(
            "[loader] ",
            "DAVA::File::Create hook disabled: client fingerprint not verified");
        return false;
    }
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule) return false;
    const uint8_t expected[] = {
        0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu};
    uint8_t* base = reinterpret_cast<uint8_t*>(gameModule);
    g_davaFileCreateTarget = base + kDavaFileCreateRva;
    if (!HasExpectedGameplayPrologue(
            g_davaFileCreateTarget,
            expected,
            sizeof(expected))) {
        g_davaFileCreateTarget =
            FindDavaFileCreateBySignature(gameModule);
    }
    if (!g_davaFileCreateTarget) {
        WriteLogLine(
            "[loader] ",
            "DAVA::File::Create signature was not found");
        return false;
    }
    char targetMessage[96] = {};
    _snprintf_s(
        targetMessage,
        sizeof(targetMessage),
        _TRUNCATE,
        "DAVA::File::Create target RVA=0x%08lX",
        static_cast<unsigned long>(
            reinterpret_cast<uint8_t*>(
                g_davaFileCreateTarget) - base));
    WriteLogLine("[loader] ", targetMessage);
    const bool installed = InstallGameplayHook(
        g_davaFileCreateTarget,
        reinterpret_cast<void*>(&DavaFileCreateDetour),
        reinterpret_cast<void**>(&g_originalDavaFileCreate),
        "DAVA::File::Create overlay hook failed",
        expected,
        sizeof(expected));
    if (installed) {
        hobs::RegisterTarget(g_davaFileCreateTarget, hobs::kModeObserve | hobs::kModeAfter);
        WriteLogLine(
            "[loader] ",
            "DAVA native resource overlay enabled for UI/mesh/material/texture files");
    }
    return installed;
}

static bool InstallSceneTrackingHooks(
    WotbModDavaResourcesHandle resourcesHandle) {
    /*
     * These three targets come from the fixed-RVA table and, unlike the
     * gameplay hooks, carry no prologue guard of their own - IsExecutable is
     * the only screening below. The fingerprint is the real guard.
     */
    if (!g_nativeFingerprintVerified) {
        WriteLogLine(
            "[loader] ",
            "Scene trackers disabled: client fingerprint not verified");
        return false;
    }
    g_sceneDrawTarget =
        WotbModDavaResources_GetSceneDrawTarget(resourcesHandle);
    g_sceneActivateTarget =
        WotbModDavaResources_GetSceneActivateTarget(resourcesHandle);
    g_sceneDeactivateTarget =
        WotbModDavaResources_GetSceneDeactivateTarget(resourcesHandle);
    const bool drawInstalled = InstallInternalHook(
        g_sceneDrawTarget,
        reinterpret_cast<void*>(&SceneDrawDetour),
        reinterpret_cast<void**>(&g_originalSceneDraw),
        "Scene::Draw hook failed");
    const bool activateInstalled = InstallInternalHook(
        g_sceneActivateTarget,
        reinterpret_cast<void*>(&SceneActivateDetour),
        reinterpret_cast<void**>(&g_originalSceneActivate),
        "Scene::Activate hook failed");
    const bool deactivateInstalled = InstallInternalHook(
        g_sceneDeactivateTarget,
        reinterpret_cast<void*>(&SceneDeactivateDetour),
        reinterpret_cast<void**>(&g_originalSceneDeactivate),
        "Scene::Deactivate hook failed");
    if (drawInstalled) {
        hobs::RegisterTarget(g_sceneDrawTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (activateInstalled) {
        hobs::RegisterTarget(g_sceneActivateTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    if (deactivateInstalled) {
        hobs::RegisterTarget(g_sceneDeactivateTarget, hobs::kModeObserve | hobs::kModeAfter);
    }
    const bool activationSource =
        drawInstalled || activateInstalled;
    if (!activationSource) {
        return false;
    }
    if (g_liveResourceBackend.scene_get_active &&
        g_liveResourceBackend.release) {
        g_eventSourceMask |=
            WOTBMOD_V3_EVENT_SOURCE_SCENE_ACTIVATED;
        if (deactivateInstalled) {
            g_eventSourceMask |=
                WOTBMOD_V3_EVENT_SOURCE_SCENE_DEACTIVATED;
        }
    }
    WriteLogLine(
        "[loader] ",
        deactivateInstalled
            ? "Scene active-state tracker enabled"
            : "Scene tracker enabled without deactivate hook");
    return true;
}

static WotbModResult WOTBMOD_CALL HookCreate(
    void*,
    void* target,
    void* detour,
    void** original) {
    return MapHookStatus(MH_CreateHook(target, detour, original));
}

static WotbModResult WOTBMOD_CALL HookEnable(void*, void* target) {
    return MapHookStatus(MH_EnableHook(target));
}

static WotbModResult WOTBMOD_CALL HookDisable(void*, void* target) {
    const MH_STATUS status = MH_DisableHook(target);
    return status == MH_ERROR_DISABLED
               ? WOTBMOD_OK
               : MapHookStatus(status);
}

static WotbModResult WOTBMOD_CALL HookRemove(void*, void* target) {
    return MapHookStatus(MH_RemoveHook(target));
}

static WotbModResult ResolveMountedFile(
    const char* virtualPath,
    char* output,
    uint32_t outputCapacity) {
    uint32_t size = outputCapacity;
    return WotbModRuntime_ResolveResourcePath(
        virtualPath,
        output,
        &size);
}

static bool ReadFileSize(const char* path, uint64_t* outSize) {
    if (!path || !outSize) return false;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(
            path,
            GetFileExInfoStandard,
            &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    *outSize =
        (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
        data.nFileSizeLow;
    return true;
}

static WotbModResult WOTBMOD_CALL ResourceLoad(
    void*,
    const WotbModResourceLoadRequest* request,
    void** outNativeResource) {
    if (!request || !request->virtual_path || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    WotbModResult result = ResolveMountedFile(
        request->virtual_path,
        resolved,
        sizeof(resolved));
    if (result != WOTBMOD_OK) return result;

    uint64_t size = 0;
    if (!ReadFileSize(resolved, &size)) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    FileResource* resource = static_cast<FileResource*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FileResource)));
    if (!resource) return WOTBMOD_ERROR_LIMIT_REACHED;
    resource->magic = kFileResourceMagic;
    resource->type = request->type;
    resource->size = size;
    strcpy_s(resource->path, resolved);
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceReload(
    void*,
    void* nativeResource) {
    FileResource* resource = static_cast<FileResource*>(nativeResource);
    if (!resource || resource->magic != kFileResourceMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uint64_t size = 0;
    if (!ReadFileSize(resource->path, &size)) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    resource->size = size;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceRelease(
    void*,
    void* nativeResource) {
    FileResource* resource = static_cast<FileResource*>(nativeResource);
    if (!resource || resource->magic != kFileResourceMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    resource->magic = 0;
    return HeapFree(GetProcessHeap(), 0, resource)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

/* True on the frame thread while PollUiScreenChanged runs. The poll makes a
 * wrapper for the active screen every frame and its classification walks
 * make one per visited control; the runtime reports each of those as a
 * registry change. Until 2026-09-05 that change re-armed a FULL classification
 * (two whole-tree walks plus a parent climb) on the very next frame, so the
 * probe fed itself and the hangar ran at a frame every 2-3 seconds. Registry
 * changes made by the poll itself no longer arm anything. */
static thread_local bool t_uiPollSelfActivity = false;

static void WOTBMOD_CALL ResourceRegistryChanged(
    void* userData,
    uint64_t generation) {
    if (!t_uiPollSelfActivity) {
        InterlockedExchange(&g_uiResourceRegistryChanged, 1);
        InterlockedExchange(&g_uiContextProbePending, 1);
    }
    if (g_forwardResourceRegistryChanged) {
        g_forwardResourceRegistryChanged(
            userData,
            generation);
        return;
    }
    char line[128] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "resource registry generation=%llu",
        static_cast<unsigned long long>(generation));
    WriteLogLine("[resource] ", line);
}

enum class UiDescendantVisibility {
    kMissing,
    kHidden,
    kVisible,
    kUnreadable
};

static UiDescendantVisibility InspectUiEffectiveVisibility(
    void* resource) {
    if (!resource || !g_liveResourceBackend.ui_get_state) {
        return UiDescendantVisibility::kUnreadable;
    }
    void* current = resource;
    bool ownsCurrent = false;
    for (uint32_t depth = 0u; depth < 32u; ++depth) {
        WotbModUiControlState state = {};
        state.struct_size = sizeof(state);
        const WotbModResult stateResult =
            g_liveResourceBackend.ui_get_state(
                g_liveResourceBackend.user_data,
                current,
                &state);
        if (stateResult != WOTBMOD_OK) {
            if (ownsCurrent) ReleaseLiveResource(current);
            return UiDescendantVisibility::kUnreadable;
        }
        if ((state.flags & WOTBMOD_UI_CONTROL_VISIBLE) == 0u) {
            if (ownsCurrent) ReleaseLiveResource(current);
            return UiDescendantVisibility::kHidden;
        }
        if (!g_liveResourceBackend.ui_get_parent) {
            if (ownsCurrent) ReleaseLiveResource(current);
            return UiDescendantVisibility::kVisible;
        }
        void* parent = nullptr;
        const WotbModResult parentResult =
            g_liveResourceBackend.ui_get_parent(
                g_liveResourceBackend.user_data,
                current,
                &parent);
        if (ownsCurrent) ReleaseLiveResource(current);
        current = nullptr;
        ownsCurrent = false;
        if (parentResult == WOTBMOD_ERROR_NOT_FOUND || !parent) {
            if (parent) ReleaseLiveResource(parent);
            return UiDescendantVisibility::kVisible;
        }
        if (parentResult != WOTBMOD_OK) {
            if (parent) ReleaseLiveResource(parent);
            return UiDescendantVisibility::kUnreadable;
        }
        current = parent;
        ownsCurrent = true;
    }
    if (ownsCurrent) ReleaseLiveResource(current);
    return UiDescendantVisibility::kUnreadable;
}

static UiDescendantVisibility InspectUiDescendant(
    void* rootResource,
    const char* name) {
    if (!rootResource || !name || !name[0] ||
        !g_liveResourceBackend.ui_find_by_name ||
        !g_liveResourceBackend.ui_get_state) {
        return UiDescendantVisibility::kUnreadable;
    }
    void* controlResource = nullptr;
    const WotbModResult findResult =
        g_liveResourceBackend.ui_find_by_name(
            g_liveResourceBackend.user_data,
            rootResource,
            name,
            1,
            &controlResource);
    if (findResult == WOTBMOD_ERROR_NOT_FOUND) {
        return UiDescendantVisibility::kMissing;
    }
    if (findResult != WOTBMOD_OK || !controlResource) {
        return UiDescendantVisibility::kUnreadable;
    }

    const UiDescendantVisibility visibility =
        InspectUiEffectiveVisibility(controlResource);
    ReleaseLiveResource(controlResource);
    return visibility;
}

static UiDescendantVisibility InspectUiHierarchy(
    void* rootResource,
    const char* name) {
    UiDescendantVisibility best =
        InspectUiDescendant(rootResource, name);
    if (best == UiDescendantVisibility::kVisible ||
        best == UiDescendantVisibility::kHidden ||
        !rootResource || !g_liveResourceBackend.ui_get_parent) {
        return best;
    }

    void* current = rootResource;
    bool ownsCurrent = false;
    for (uint32_t depth = 0u; depth < 16u; ++depth) {
        void* parent = nullptr;
        const WotbModResult parentResult =
            g_liveResourceBackend.ui_get_parent(
                g_liveResourceBackend.user_data,
                current,
                &parent);
        if (ownsCurrent) ReleaseLiveResource(current);
        current = nullptr;
        ownsCurrent = false;
        if (parentResult == WOTBMOD_ERROR_NOT_FOUND || !parent) break;
        if (parentResult != WOTBMOD_OK) {
            if (parent) ReleaseLiveResource(parent);
            best = UiDescendantVisibility::kUnreadable;
            break;
        }
        current = parent;
        ownsCurrent = true;
        const UiDescendantVisibility candidate =
            InspectUiDescendant(current, name);
        if (candidate == UiDescendantVisibility::kVisible ||
            candidate == UiDescendantVisibility::kHidden) {
            best = candidate;
            break;
        }
        if (candidate == UiDescendantVisibility::kUnreadable) {
            best = candidate;
        }
    }
    if (ownsCurrent) ReleaseLiveResource(current);
    return best;
}

static UiDescendantVisibility InspectUiInputMarker(
    void* rootResource,
    const char* name,
    void** outMarkerResource) {
    if (outMarkerResource) *outMarkerResource = nullptr;
    if (!rootResource || !name || !name[0] ||
        !g_liveResourceBackend.ui_find_by_name ||
        !g_liveResourceBackend.ui_get_state) {
        return UiDescendantVisibility::kUnreadable;
    }
    void* markerResource = nullptr;
    const WotbModResult findResult =
        g_liveResourceBackend.ui_find_by_name(
            g_liveResourceBackend.user_data,
            rootResource,
            name,
            1,
            &markerResource);
    if (findResult == WOTBMOD_ERROR_NOT_FOUND) {
        return UiDescendantVisibility::kMissing;
    }
    if (findResult != WOTBMOD_OK || !markerResource) {
        if (markerResource) ReleaseLiveResource(markerResource);
        return UiDescendantVisibility::kUnreadable;
    }
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    const WotbModResult stateResult =
        g_liveResourceBackend.ui_get_state(
            g_liveResourceBackend.user_data,
            markerResource,
            &state);
    if (stateResult != WOTBMOD_OK) {
        ReleaseLiveResource(markerResource);
        return UiDescendantVisibility::kUnreadable;
    }
    if (outMarkerResource) {
        *outMarkerResource = markerResource;
    } else {
        ReleaseLiveResource(markerResource);
    }
    return (state.flags & WOTBMOD_UI_CONTROL_INPUT_ENABLED) != 0u
        ? UiDescendantVisibility::kVisible
        : UiDescendantVisibility::kHidden;
}

static UiDescendantVisibility InspectUiInputMarkerHierarchy(
    void* rootResource,
    const char* name,
    void** outMarkerResource) {
    if (outMarkerResource) *outMarkerResource = nullptr;
    UiDescendantVisibility best =
        InspectUiInputMarker(
            rootResource, name, outMarkerResource);
    if (best == UiDescendantVisibility::kVisible ||
        best == UiDescendantVisibility::kHidden ||
        !rootResource || !g_liveResourceBackend.ui_get_parent) {
        return best;
    }

    void* current = rootResource;
    bool ownsCurrent = false;
    for (uint32_t depth = 0u; depth < 16u; ++depth) {
        void* parent = nullptr;
        const WotbModResult parentResult =
            g_liveResourceBackend.ui_get_parent(
                g_liveResourceBackend.user_data,
                current,
                &parent);
        if (ownsCurrent) ReleaseLiveResource(current);
        current = nullptr;
        ownsCurrent = false;
        if (parentResult == WOTBMOD_ERROR_NOT_FOUND || !parent) break;
        if (parentResult != WOTBMOD_OK) {
            if (parent) ReleaseLiveResource(parent);
            best = UiDescendantVisibility::kUnreadable;
            break;
        }
        current = parent;
        ownsCurrent = true;
        const UiDescendantVisibility candidate =
            InspectUiInputMarker(
                current, name, outMarkerResource);
        if (candidate == UiDescendantVisibility::kVisible ||
            candidate == UiDescendantVisibility::kHidden) {
            best = candidate;
            break;
        }
        if (candidate == UiDescendantVisibility::kUnreadable) {
            best = candidate;
        }
    }
    if (ownsCurrent) ReleaseLiveResource(current);
    return best;
}

static UiDescendantVisibility InspectRetainedUiInputMarker(
    void* markerResource) {
    if (!markerResource || !g_liveResourceBackend.ui_get_state) {
        return UiDescendantVisibility::kUnreadable;
    }
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    const WotbModResult result =
        g_liveResourceBackend.ui_get_state(
            g_liveResourceBackend.user_data,
            markerResource,
            &state);
    if (result != WOTBMOD_OK) {
        return UiDescendantVisibility::kUnreadable;
    }
    return (state.flags & WOTBMOD_UI_CONTROL_INPUT_ENABLED) != 0u
        ? UiDescendantVisibility::kVisible
        : UiDescendantVisibility::kHidden;
}

static void ResetUiContextProbeResources() {
    ReleaseLiveResource(g_cachedCatalogMarkerResource);
    g_cachedCatalogMarkerResource = nullptr;
}

static UiDescendantVisibility InspectCachedCatalogMarker(
    void* rootResource,
    bool allowDiscovery) {
    if (g_cachedCatalogMarkerResource) {
        const UiDescendantVisibility cached =
            InspectRetainedUiInputMarker(
                g_cachedCatalogMarkerResource);
        if (cached != UiDescendantVisibility::kUnreadable) {
            return cached;
        }
        ResetUiContextProbeResources();
        InterlockedExchange(&g_uiContextProbePending, 1);
    }
    if (!allowDiscovery) {
        return UiDescendantVisibility::kUnreadable;
    }
    return InspectUiInputMarkerHierarchy(
        rootResource,
        "ModCatalogStateMarker",
        &g_cachedCatalogMarkerResource);
}

static void ApplyUiDerivedContext() {
    if (InterlockedCompareExchange(
            &g_battleContextActive, 0, 0) != 0) {
        return;
    }
    const LONG desired = InterlockedCompareExchange(
        &g_uiDerivedContextMask, 0, 0);
    if (WotbModV3Runtime_SetContext(
            static_cast<uint64_t>(desired)) != WOTBMOD_V3_OK) {
        return;
    }
    const LONG previous = InterlockedExchange(
        &g_lastLoggedUiContextMask, desired);
    if (previous != desired) {
        char line[160] = {};
        _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "V3 UI context=%ld hangar=%s catalog=%s",
            desired,
            (desired & WOTBMOD_V3_CONTEXT_HANGAR) != 0
                ? "yes"
                : "no",
            (desired & WOTBMOD_V3_CONTEXT_MOD_SCREEN) != 0
                ? "yes"
                : "no");
        WriteLogLine("[loader] ", line);
    }
}

static void UpdateUiDerivedContext(
    void* rootResource,
    bool screenChanged,
    bool fullProbe) {
    if (InterlockedCompareExchange(
            &g_battleContextActive, 0, 0) != 0) {
        return;
    }
    LONG desired = WOTBMOD_V3_CONTEXT_NONE;
    const LONG previous = InterlockedCompareExchange(
        &g_uiDerivedContextMask, 0, 0);
    const bool hangarProbe =
        fullProbe ||
        (previous & WOTBMOD_V3_CONTEXT_HANGAR) == 0;
    const UiDescendantVisibility hangar = rootResource && hangarProbe
        ? InspectUiHierarchy(rootResource, "Hangar")
        : UiDescendantVisibility::kMissing;
    // ui_find_by_name includes the supplied root itself, so this identifies
    // Hangar.yaml rather than a child that can be rebuilt during late lobby
    // initialization. An unchanged root object may briefly be unreadable;
    // retain the last trusted classification only across that same screen.
    if (hangar == UiDescendantVisibility::kVisible ||
        hangar == UiDescendantVisibility::kHidden) {
        desired = WOTBMOD_V3_CONTEXT_HANGAR;
    } else if (rootResource &&
               !screenChanged &&
               (previous & WOTBMOD_V3_CONTEXT_HANGAR) != 0) {
        desired = WOTBMOD_V3_CONTEXT_HANGAR;
    }
    /* Marker discovery is a whole-tree walk (about 2 s on a 2500-control
     * hangar, measured 2026-09-05). The stock hangar has no catalog marker,
     * so an uncached miss used to be re-walked on every light probe, i.e.
     * every 15 frames, and the hangar froze for 2 s at a time. A miss is now
     * remembered per screen object and retried at most every 300 frames
     * unless a full probe asks for it. */
    static void* s_markerMissScreen = nullptr;
    static LONG s_markerMissFrame = -100000;
    const LONG frameNow = InterlockedCompareExchange(&g_dispatchFrameSerial, 0, 0);
    void* const screenObject = rootResource
        ? WotbModDavaResources_GetNativeObject(g_davaResourcesHandle, rootResource)
        : nullptr;
    const bool missRemembered =
        screenObject && screenObject == s_markerMissScreen &&
        frameNow - s_markerMissFrame < 300;
    const bool discoverCatalogMarker =
        fullProbe ||
        ((desired & WOTBMOD_V3_CONTEXT_HANGAR) != 0 &&
         !g_cachedCatalogMarkerResource && !missRemembered);
    const UiDescendantVisibility catalogMarker = rootResource
        ? InspectCachedCatalogMarker(
              rootResource, discoverCatalogMarker)
        : UiDescendantVisibility::kMissing;
    if (discoverCatalogMarker && !g_cachedCatalogMarkerResource) {
        s_markerMissScreen = screenObject;
        s_markerMissFrame = frameNow;
    }
    const bool markerUnavailable =
        catalogMarker == UiDescendantVisibility::kMissing ||
        catalogMarker == UiDescendantVisibility::kUnreadable;
    const UiDescendantVisibility catalog =
        rootResource && fullProbe && markerUnavailable
            ? InspectUiHierarchy(rootResource, "ModCatalogScreen")
            : UiDescendantVisibility::kMissing;
    if (catalogMarker == UiDescendantVisibility::kVisible ||
        (markerUnavailable &&
         ((!fullProbe &&
            (previous & WOTBMOD_V3_CONTEXT_MOD_SCREEN) != 0) ||
          catalog == UiDescendantVisibility::kVisible ||
          (desired == WOTBMOD_V3_CONTEXT_HANGAR &&
           catalog == UiDescendantVisibility::kUnreadable)))) {
        desired |= WOTBMOD_V3_CONTEXT_MOD_SCREEN;
    }
    if (desired != previous && rootResource) {
        char line[224] = {};
        _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "UI classification changed screen-changed=%s hangar-root=%u catalog=%u marker=%u",
            screenChanged ? "yes" : "no",
            static_cast<unsigned>(hangar),
            static_cast<unsigned>(catalog),
            static_cast<unsigned>(catalogMarker));
        WriteLogLine("[loader] ", line);
    }
    InterlockedExchange(&g_uiDerivedContextMask, desired);
    ApplyUiDerivedContext();
}

static void PollUiScreenChangedInner();

static void PollUiScreenChanged() {
    t_uiPollSelfActivity = true;
    PollUiScreenChangedInner();
    t_uiPollSelfActivity = false;
}

static void PollUiScreenChangedInner() {
    if (!ClientEventRuntimeReady() ||
        !g_liveResourceBackend.ui_get_active_screen) {
        return;
    }

    if (InterlockedExchange(&g_uiResourceRegistryChanged, 0) != 0) {
        ResetUiContextProbeResources();
    }
    void* currentResource = nullptr;
    const LONGLONG getStarted = ProfNow();
    const WotbModResult getResult =
        g_liveResourceBackend.ui_get_active_screen(
            g_liveResourceBackend.user_data,
            &currentResource);
    ProfAdd(&g_wotbProfPollGetScreenTicks, ProfNow() - getStarted);
    if (getResult == WOTBMOD_ERROR_NOT_FOUND) {
        ResetUiContextProbeResources();
        UpdateUiDerivedContext(nullptr, true, true);
        wotbmod::loader::ResetV3NativeClientServicesUiInputState();
        if (!g_lastUiScreenResource) return;
        const WotbModResult notifyResult =
            WotbModRuntime_NotifyUiScreenChanged(
                g_lastUiScreenResource,
                nullptr);
        if (notifyResult == WOTBMOD_OK) {
            ReleaseLiveResource(g_lastUiScreenResource);
            g_lastUiScreenResource = nullptr;
            g_lastUiScreenObject = nullptr;
        }
        return;
    }
    if (getResult != WOTBMOD_OK || !currentResource) return;

    void* currentObject =
        WotbModDavaResources_GetNativeObject(
            g_davaResourcesHandle,
            currentResource);
    if (!currentObject) {
        ReleaseLiveResource(currentResource);
        return;
    }
    const LONG frameSerial = InterlockedCompareExchange(
        &g_dispatchFrameSerial, 0, 0);
    const bool screenChanged = currentObject != g_lastUiScreenObject;
    bool probeRequested = InterlockedExchange(
        &g_uiContextProbePending, 0) != 0;
    if (probeRequested && !screenChanged) {
        /* A full classification costs two whole-tree walks; on the same screen
         * it runs at most once per 60 frames, the request stays armed. */
        static LONG s_lastFullProbeFrame = -1000;
        if (frameSerial - s_lastFullProbeFrame < 60) {
            InterlockedExchange(&g_uiContextProbePending, 1);
            probeRequested = false;
        } else {
            s_lastFullProbeFrame = frameSerial;
        }
    }
    if (screenChanged) ResetUiContextProbeResources();
    if (screenChanged || probeRequested || frameSerial % 15 == 0) {
        const LONGLONG updateStarted = ProfNow();
        UpdateUiDerivedContext(
            currentResource,
            screenChanged,
            screenChanged || probeRequested);
        ProfAdd(&g_wotbProfPollUpdateTicks, ProfNow() - updateStarted);
        InterlockedIncrement64(&g_wotbProfPollUpdateCalls);
        if (screenChanged || probeRequested) InterlockedIncrement64(&g_wotbProfPollUpdateFullCalls);
    } else {
        // Battle boundary events also write the global V3 context. Reapply the
        // last trusted UI classification every frame outside battle so a
        // delayed BATTLE_LEFT event cannot leave a stale HANGAR context behind.
        const LONGLONG applyStarted = ProfNow();
        ApplyUiDerivedContext();
        ProfAdd(&g_wotbProfPollApplyTicks, ProfNow() - applyStarted);
    }
    if (currentObject == g_lastUiScreenObject) {
        const LONGLONG releaseStarted = ProfNow();
        ReleaseLiveResource(currentResource);
        ProfAdd(&g_wotbProfPollReleaseTicks, ProfNow() - releaseStarted);
        return;
    }

    wotbmod::loader::ResetV3NativeClientServicesUiInputState();

    const WotbModResult notifyResult =
        WotbModRuntime_NotifyUiScreenChanged(
            g_lastUiScreenResource,
            currentResource);
    if (notifyResult != WOTBMOD_OK) {
        ReleaseLiveResource(currentResource);
        return;
    }
    ReleaseLiveResource(g_lastUiScreenResource);
    g_lastUiScreenResource = currentResource;
    g_lastUiScreenObject = currentObject;
}

static bool DavaNativeLiveProbeEnabled() {
    char value[8] = {};
    return GetEnvironmentVariableA(
               "WOTBMOD_DAVA_LIVE_PROBE",
               value,
               static_cast<DWORD>(sizeof(value))) > 0 &&
           strcmp(value, "0") != 0;
}

static bool ProbeReviewedDavaClass(
    WotbModV3Handle owner,
    const char* className) {
    uint32_t registered = 0u;
    const WotbModV3Result registeredResult =
        wotbmod::v3::InstalledDavaNativeClassIsRegistered(
            className, &registered);

    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    request.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE;
    strcpy_s(request.class_name, className);

    WotbModDavaNativeToken object = 0u;
    const WotbModV3Result createResult =
        wotbmod::v3::InstalledDavaNativeClassCreate(
            owner, &request, &object);
    const WotbModV3Result releaseResult =
        createResult == WOTBMOD_V3_OK && object != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, object)
            : WOTBMOD_V3_E_INVALID_HANDLE;

    char status[240] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "DAVA private live probe class=%s registered=%u "
        "registered-result=%d create=%d release=%d token=%llu",
        className,
        registered,
        static_cast<int>(registeredResult),
        static_cast<int>(createResult),
        static_cast<int>(releaseResult),
        static_cast<unsigned long long>(object));
    WriteLogLine("[loader] ", status);

    return registeredResult == WOTBMOD_V3_OK && registered == 1u &&
           createResult == WOTBMOD_V3_OK && object != 0u &&
           releaseResult == WOTBMOD_V3_OK;
}

static WotbModV3Result ProbeDavaNativeStockTracer(
    WotbModV3Handle owner,
    WotbModDavaNativeToken* outTracer) {
    if (!outTracer) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *outTracer = 0u;

    WotbModDavaNativeTracerRequest request = {};
    request.struct_size = sizeof(request);
    request.shell_type = 0u;
    request.destination[0] = 1.0f;
    request.color[0] = 1.0f;
    request.color[1] = 1.0f;
    request.color[2] = 1.0f;
    request.color[3] = 1.0f;
    request.width = 1.0f;
    request.lifetime_seconds = 1.0f;
    return wotbmod::v3::InstalledDavaNativeTracerCreate(
        owner, &request, outTracer);
}

static bool RunDavaNativeLiveProbe();

static WotbModV3Result WOTBMOD_V3_CALL
RunDavaNativeStockTracerLiveProbeMain(void*);

static void ScheduleDavaNativeStockTracerLiveProbe() {
    if (InterlockedCompareExchange(
            &g_davaNativeTracerLiveProbeState, 1, 0) != 0) {
        return;
    }
    const WotbModV3Result postResult =
        wotbmod::v3::PostMainThread(
            &RunDavaNativeStockTracerLiveProbeMain, nullptr);
    if (postResult == WOTBMOD_V3_OK) return;

    char status[192] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "DAVA private live probe stock-tracer post=%d",
        static_cast<int>(postResult));
    WriteLogLine("[loader] ", status);
    InterlockedExchange(&g_davaNativeTracerLiveProbeState, 0);
}

static WotbModV3Result WOTBMOD_V3_CALL
RunDavaNativeStockTracerLiveProbeMain(void*) {
    const WotbModV3Handle owner = UINT64_C(0x4441564154524143);
    WotbModDavaNativeToken tracer = 0u;
    const WotbModV3Result createResult =
        ProbeDavaNativeStockTracer(owner, &tracer);
    const WotbModV3Result releaseResult =
        createResult == WOTBMOD_V3_OK && tracer != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, tracer)
            : WOTBMOD_V3_E_INVALID_HANDLE;

    if ((createResult == WOTBMOD_V3_E_NOT_FOUND ||
         createResult == WOTBMOD_V3_E_TIMEOUT ||
         createResult == WOTBMOD_V3_E_WRONG_THREAD) &&
        tracer == 0u) {
        InterlockedExchange(&g_davaNativeTracerLiveProbeState, 0);
        return createResult;
    }

    const bool passed =
        createResult == WOTBMOD_V3_OK && tracer != 0u &&
        releaseResult == WOTBMOD_V3_OK;

    char status[256] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "DAVA PRIVATE LIVE STATUS stock-tracer=%s create=%d release=%d "
        "token=%llu",
        passed ? "PASS" : "FAIL",
        static_cast<int>(createResult),
        static_cast<int>(releaseResult),
        static_cast<unsigned long long>(tracer));
    WriteLogLine("[loader] ", status);
    InterlockedExchange(&g_davaNativeTracerLiveProbeState, passed ? 2 : 3);
    return passed ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
}

static WotbModV3Result WOTBMOD_V3_CALL
RunDavaNativeLiveProbeMain(void*) {
    const bool passed = RunDavaNativeLiveProbe();
    InterlockedExchange(
        &g_davaNativeLiveProbeState, passed ? 2 : 0);
    return passed ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
}

static void ScheduleDavaNativeLiveProbe() {
    InterlockedIncrement(&g_davaNativeLiveProbeAttempts);
    const WotbModV3Result postResult =
        wotbmod::v3::PostMainThread(
            &RunDavaNativeLiveProbeMain, nullptr);
    if (postResult == WOTBMOD_V3_OK) return;

    char status[192] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "DAVA private live probe full post=%d",
        static_cast<int>(postResult));
    WriteLogLine("[loader] ", status);
    InterlockedExchange(&g_davaNativeLiveProbeState, 0);
}

static void LogDavaNativeLiveProbePhase(
    const char* phase,
    const char* edge,
    WotbModV3Result result,
    WotbModDavaNativeToken token) {
    char status[256] = {};
    _snprintf_s(
        status,
        sizeof(status),
        _TRUNCATE,
        "DAVA private live probe phase=%s edge=%s result=%d token=%llu",
        phase ? phase : "unknown",
        edge ? edge : "unknown",
        static_cast<int>(result),
        static_cast<unsigned long long>(token));
    WriteLogLine("[loader] ", status);
}

static bool RunDavaNativeLiveProbe() {
    const WotbModV3Handle owner = UINT64_C(0x4441564150524F42);
    const uint64_t capabilities =
        wotbmod::v3::InstalledDavaNativeCapabilities();

    const bool uiControlOk =
        ProbeReviewedDavaClass(owner, "DAVA::UIControl");
    const bool entityOk =
        ProbeReviewedDavaClass(owner, "DAVA::Entity");

    char executablePath[MAX_PATH] = {};
    char gameDirectory[MAX_PATH] = {};
    char yamlPath[MAX_PATH] = {};
    char archivePath[MAX_PATH] = {};
    char meshBasePath[MAX_PATH] = {};
    char meshReplacementPath[MAX_PATH] = {};
    GetModuleFileNameA(
        GetModuleHandleA(nullptr), executablePath, MAX_PATH);
    strcpy_s(gameDirectory, executablePath);
    ParentDirectory(gameDirectory);
    const bool fixturePathsOk =
        JoinPath(
            gameDirectory,
            "mods\\data\\native_live_test_mod\\fixtures\\probe.yaml",
            yamlPath,
            sizeof(yamlPath)) &&
        JoinPath(
            gameDirectory,
            "mods\\data\\native_live_test_mod\\fixtures\\probe.zip",
            archivePath,
            sizeof(archivePath));
    /* Use two distinct stock DVPL scenes for the exact-build live proof. The
     * resource bridge maps physical files under Data back to DAVA ~res:/
     * paths, preserving stock dependency and DVPL resolution. */
    const bool meshPathsOk =
        JoinPath(
            gameDirectory,
            "Data\\3d\\Tanks\\USSR\\T-34-85.sc2.dvpl",
            meshBasePath,
            sizeof(meshBasePath)) &&
        JoinPath(
            gameDirectory,
            "Data\\3d\\Tanks\\USA\\A124_T54E2.sc2.dvpl",
            meshReplacementPath,
            sizeof(meshReplacementPath));

    uint32_t materialRegistered = 0u;
    const WotbModV3Result materialRegisteredResult =
        wotbmod::v3::InstalledDavaNativeClassIsRegistered(
            "DAVA::NMaterial", &materialRegistered);
    WotbModDavaNativeClassRequest materialRequest = {};
    materialRequest.struct_size = sizeof(materialRequest);
    materialRequest.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL;
    strcpy_s(materialRequest.class_name, "DAVA::NMaterial");
    WotbModDavaNativeToken material = 0u;
    LogDavaNativeLiveProbePhase(
        "material-create", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result materialCreateResult =
        wotbmod::v3::InstalledDavaNativeClassCreate(
            owner, &materialRequest, &material);
    LogDavaNativeLiveProbePhase(
        "material-create", "end", materialCreateResult, material);

    WotbModDavaNativeToken yamlDocument = 0u;
    LogDavaNativeLiveProbePhase(
        "yaml-parse", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result yamlResult =
        fixturePathsOk
            ? wotbmod::v3::InstalledDavaNativeYamlParseFile(
                  owner, yamlPath, &yamlDocument)
            : WOTBMOD_V3_E_NOT_FOUND;
    LogDavaNativeLiveProbePhase(
        "yaml-parse", "end", yamlResult, yamlDocument);
    WotbModDavaNativeBuffer yamlQuery = {};
    yamlQuery.struct_size = sizeof(yamlQuery);
    const WotbModV3Result yamlQueryResult =
        yamlResult == WOTBMOD_V3_OK && yamlDocument != 0u
            ? wotbmod::v3::InstalledDavaNativeYamlExportUtf8(
                  owner, yamlDocument, &yamlQuery)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    uint8_t yamlBytes[4096] = {};
    WotbModDavaNativeBuffer yamlRead = {};
    yamlRead.struct_size = sizeof(yamlRead);
    yamlRead.data = yamlBytes;
    yamlRead.capacity = static_cast<uint32_t>(sizeof(yamlBytes));
    const WotbModV3Result yamlReadResult =
        yamlQueryResult == WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
                yamlQuery.size > 0u &&
                yamlQuery.size <= sizeof(yamlBytes)
            ? wotbmod::v3::InstalledDavaNativeYamlExportUtf8(
                  owner, yamlDocument, &yamlRead)
            : WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    const WotbModV3Result yamlReleaseResult =
        yamlDocument != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(
                  owner, yamlDocument)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModDavaNativeToken archive = 0u;
    LogDavaNativeLiveProbePhase(
        "archive-open", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result archiveResult =
        fixturePathsOk
            ? wotbmod::v3::InstalledDavaNativeArchiveOpenFile(
                  owner, archivePath, &archive)
            : WOTBMOD_V3_E_NOT_FOUND;
    LogDavaNativeLiveProbePhase(
        "archive-open", "end", archiveResult, archive);
    uint32_t archiveCount = 0u;
    const WotbModV3Result archiveCountResult =
        archiveResult == WOTBMOD_V3_OK && archive != 0u
            ? wotbmod::v3::InstalledDavaNativeArchiveGetEntryCount(
                  owner, archive, &archiveCount)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModDavaNativeArchiveEntry archiveEntry = {};
    archiveEntry.struct_size = sizeof(archiveEntry);
    const WotbModV3Result archiveEntryResult =
        archiveCountResult == WOTBMOD_V3_OK && archiveCount == 1u
            ? wotbmod::v3::InstalledDavaNativeArchiveGetEntry(
                  owner, archive, 0u, &archiveEntry)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    uint8_t archiveBytes[128] = {};
    WotbModDavaNativeBuffer archiveRead = {};
    archiveRead.struct_size = sizeof(archiveRead);
    archiveRead.data = archiveBytes;
    archiveRead.capacity = static_cast<uint32_t>(sizeof(archiveBytes));
    const WotbModV3Result archiveReadResult =
        archiveEntryResult == WOTBMOD_V3_OK &&
                archiveEntry.original_size <= sizeof(archiveBytes)
            ? wotbmod::v3::InstalledDavaNativeArchiveReadEntry(
                  owner, archive, 0u, &archiveRead)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    const WotbModV3Result archiveReleaseResult =
        archive != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, archive)
            : WOTBMOD_V3_E_INVALID_HANDLE;

    WotbModDavaNativeMaterialMutation mutation = {};
    mutation.struct_size = sizeof(mutation);
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY;
    mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT;
    mutation.array_size = 1u;
    strcpy_s(mutation.name, "blitzforgeLiveProbe");
    mutation.values[0] = 1.0f;
    LogDavaNativeLiveProbePhase(
        "material-mutate", "begin", WOTBMOD_V3_OK, material);
    const WotbModV3Result materialSetPropertyResult =
        materialCreateResult == WOTBMOD_V3_OK && material != 0u
            ? wotbmod::v3::InstalledDavaNativeMaterialMutate(
                  owner, material, &mutation)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_PROPERTY;
    mutation.array_size = 0u;
    const WotbModV3Result materialRemovePropertyResult =
        materialSetPropertyResult == WOTBMOD_V3_OK
            ? wotbmod::v3::InstalledDavaNativeMaterialMutate(
                  owner, material, &mutation)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FLAG;
    mutation.int_value = 1;
    const WotbModV3Result materialSetFlagResult =
        materialRemovePropertyResult == WOTBMOD_V3_OK
            ? wotbmod::v3::InstalledDavaNativeMaterialMutate(
                  owner, material, &mutation)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_FLAG;
    const WotbModV3Result materialRemoveFlagResult =
        materialSetFlagResult == WOTBMOD_V3_OK
            ? wotbmod::v3::InstalledDavaNativeMaterialMutate(
                  owner, material, &mutation)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    LogDavaNativeLiveProbePhase(
        "material-mutate",
        "end",
        materialRemoveFlagResult,
        material);
    WotbModV3Result materialReleaseResult =
        WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModDavaNativeClassRequest meshRequest = {};
    uint32_t meshRegistered = 0u;
    const WotbModV3Result meshRegisteredResult =
        wotbmod::v3::InstalledDavaNativeClassIsRegistered(
            "DAVA::Mesh", &meshRegistered);
    meshRequest.struct_size = sizeof(meshRequest);
    meshRequest.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_MESH;
    strcpy_s(meshRequest.class_name, "DAVA::Mesh");
    meshRequest.payload = meshReplacementPath;
    meshRequest.payload_size = meshPathsOk
        ? static_cast<uint32_t>(strlen(meshReplacementPath) + 1u) : 0u;
    WotbModDavaNativeToken mesh = 0u;
    LogDavaNativeLiveProbePhase(
        "mesh-create", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result meshCreateResult =
        meshPathsOk
            ? wotbmod::v3::InstalledDavaNativeClassCreate(
                  owner, &meshRequest, &mesh)
            : WOTBMOD_V3_E_NOT_FOUND;
    LogDavaNativeLiveProbePhase(
        "mesh-create", "end", meshCreateResult, mesh);

    WotbModDavaNativeClassRequest consumerRequest = {};
    uint32_t consumerRegistered = 0u;
    const WotbModV3Result consumerRegisteredResult =
        wotbmod::v3::InstalledDavaNativeClassIsRegistered(
            "DAVA::MeshConsumer", &consumerRegistered);
    consumerRequest.struct_size = sizeof(consumerRequest);
    consumerRequest.object_kind =
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER;
    strcpy_s(consumerRequest.class_name, "DAVA::MeshConsumer");
    consumerRequest.payload = meshBasePath;
    consumerRequest.payload_size = meshPathsOk
        ? static_cast<uint32_t>(strlen(meshBasePath) + 1u) : 0u;
    WotbModDavaNativeToken meshConsumer = 0u;
    LogDavaNativeLiveProbePhase(
        "mesh-consumer-create", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result consumerCreateResult =
        meshPathsOk
            ? wotbmod::v3::InstalledDavaNativeClassCreate(
                  owner, &consumerRequest, &meshConsumer)
            : WOTBMOD_V3_E_NOT_FOUND;
    LogDavaNativeLiveProbePhase(
        "mesh-consumer-create", "end", consumerCreateResult, meshConsumer);
    LogDavaNativeLiveProbePhase(
        "mesh-hot-swap", "begin", WOTBMOD_V3_OK, meshConsumer);
    const WotbModV3Result meshResult =
        meshCreateResult == WOTBMOD_V3_OK && mesh != 0u &&
                consumerCreateResult == WOTBMOD_V3_OK &&
                meshConsumer != 0u
            ? wotbmod::v3::InstalledDavaNativeMeshHotSwap(
                  owner, meshConsumer, mesh)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    LogDavaNativeLiveProbePhase(
        "mesh-hot-swap", "end", meshResult, meshConsumer);
    WotbModDavaNativeMaterialMutation applyMutation = {};
    applyMutation.struct_size = sizeof(applyMutation);
    applyMutation.mutation_kind =
        WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER;
    applyMutation.related_object = meshConsumer;
    LogDavaNativeLiveProbePhase(
        "material-apply", "begin", WOTBMOD_V3_OK, material);
    const WotbModV3Result materialApplyResult =
        meshResult == WOTBMOD_V3_OK &&
                materialCreateResult == WOTBMOD_V3_OK && material != 0u
            ? wotbmod::v3::InstalledDavaNativeMaterialMutate(
                  owner, material, &applyMutation)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    LogDavaNativeLiveProbePhase(
        "material-apply", "end", materialApplyResult, material);
    const WotbModV3Result consumerReleaseResult =
        meshConsumer != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(
                  owner, meshConsumer)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    const WotbModV3Result meshReleaseResult =
        mesh != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, mesh)
            : WOTBMOD_V3_E_INVALID_HANDLE;
    materialReleaseResult =
        materialCreateResult == WOTBMOD_V3_OK && material != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, material)
            : WOTBMOD_V3_E_INVALID_HANDLE;

    WotbModDavaNativeToken tracer = 0u;
    LogDavaNativeLiveProbePhase(
        "stock-tracer", "begin", WOTBMOD_V3_OK, 0u);
    const WotbModV3Result tracerResult =
        ProbeDavaNativeStockTracer(owner, &tracer);
    LogDavaNativeLiveProbePhase(
        "stock-tracer", "end", tracerResult, tracer);
    const WotbModV3Result tracerReleaseResult =
        tracerResult == WOTBMOD_V3_OK && tracer != 0u
            ? wotbmod::v3::InstalledDavaNativeRelease(owner, tracer)
            : WOTBMOD_V3_E_INVALID_HANDLE;

    uint32_t unknownRegistered = 1u;
    const WotbModV3Result unknownRegisteredResult =
        wotbmod::v3::InstalledDavaNativeClassIsRegistered(
            "DAVA::ArbitraryUnknownClass", &unknownRegistered);
    const bool unknownBlocked =
        unknownRegisteredResult == WOTBMOD_V3_OK && unknownRegistered == 0u;

    const bool tracerCapability =
        (capabilities & WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER) != 0u;
    const bool tracerOk =
        tracerCapability && tracerResult == WOTBMOD_V3_OK && tracer != 0u &&
        tracerReleaseResult == WOTBMOD_V3_OK;
    const bool tracerWaitingForBattle =
        tracerCapability && tracerResult == WOTBMOD_V3_E_NOT_FOUND &&
        tracer == 0u;
    if (tracerOk) {
        InterlockedExchange(&g_davaNativeTracerLiveProbeState, 2);
    }
    const bool yamlOk =
        yamlResult == WOTBMOD_V3_OK && yamlDocument != 0u &&
        yamlQueryResult == WOTBMOD_V3_E_BUFFER_TOO_SMALL &&
        yamlQuery.size > 0u && yamlQuery.size <= sizeof(yamlBytes) &&
        yamlReadResult == WOTBMOD_V3_OK &&
        yamlRead.size == yamlQuery.size &&
        yamlReleaseResult == WOTBMOD_V3_OK;
    static const char kExpectedArchiveBytes[] =
        "blitzforge-native-archive-probe\n";
    const bool archiveOk =
        archiveResult == WOTBMOD_V3_OK && archive != 0u &&
        archiveCountResult == WOTBMOD_V3_OK && archiveCount == 1u &&
        archiveEntryResult == WOTBMOD_V3_OK &&
        strcmp(archiveEntry.relative_path, "probe.txt") == 0 &&
        archiveEntry.original_size == sizeof(kExpectedArchiveBytes) - 1u &&
        archiveReadResult == WOTBMOD_V3_OK &&
        archiveRead.size == sizeof(kExpectedArchiveBytes) - 1u &&
        memcmp(
            archiveBytes,
            kExpectedArchiveBytes,
            sizeof(kExpectedArchiveBytes) - 1u) == 0 &&
        archiveReleaseResult == WOTBMOD_V3_OK;
    const bool materialOk =
        materialRegisteredResult == WOTBMOD_V3_OK &&
        materialRegistered == 1u &&
        materialCreateResult == WOTBMOD_V3_OK && material != 0u &&
        materialSetPropertyResult == WOTBMOD_V3_OK &&
        materialRemovePropertyResult == WOTBMOD_V3_OK &&
        materialSetFlagResult == WOTBMOD_V3_OK &&
        materialRemoveFlagResult == WOTBMOD_V3_OK &&
        materialApplyResult == WOTBMOD_V3_OK &&
        materialReleaseResult == WOTBMOD_V3_OK;
    const bool meshOk =
        meshRegisteredResult == WOTBMOD_V3_OK && meshRegistered == 1u &&
        consumerRegisteredResult == WOTBMOD_V3_OK &&
        consumerRegistered == 1u &&
        meshCreateResult == WOTBMOD_V3_OK && mesh != 0u &&
        consumerCreateResult == WOTBMOD_V3_OK && meshConsumer != 0u &&
        meshResult == WOTBMOD_V3_OK &&
        materialApplyResult == WOTBMOD_V3_OK &&
        consumerReleaseResult == WOTBMOD_V3_OK &&
        meshReleaseResult == WOTBMOD_V3_OK;
    const uint64_t expectedCapabilities =
        WOTBMOD_DAVA_NATIVE_CAP_YAML |
        WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE |
        WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL |
        WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP |
        WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER |
        WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;
    const bool exactCapabilityMask =
        capabilities == expectedCapabilities;
    const bool factoryOk = exactCapabilityMask && uiControlOk && entityOk &&
        meshRegisteredResult == WOTBMOD_V3_OK && meshRegistered == 1u &&
        consumerRegisteredResult == WOTBMOD_V3_OK &&
        consumerRegistered == 1u;
    const uint32_t ownerCleanupCount =
        wotbmod::v3::InstalledDavaNativeReleaseOwner(owner);

    char detail[640] = {};
    _snprintf_s(
        detail,
        sizeof(detail),
        _TRUNCATE,
        "DAVA private live probe detail caps=0x%llX "
        "material-reg-result=%d material-registered=%u "
        "material-create=%d set-prop=%d remove-prop=%d set-flag=%d "
        "remove-flag=%d apply=%d release=%d yaml=%d/%d/%d/%d "
        "archive=%d/%d/%d/%d/%d mesh=%d/%d/%d/%d/%d tracer=%d/%d "
        "owner-cleanup=%u",
        static_cast<unsigned long long>(capabilities),
        static_cast<int>(materialRegisteredResult),
        materialRegistered,
        static_cast<int>(materialCreateResult),
        static_cast<int>(materialSetPropertyResult),
        static_cast<int>(materialRemovePropertyResult),
        static_cast<int>(materialSetFlagResult),
        static_cast<int>(materialRemoveFlagResult),
        static_cast<int>(materialApplyResult),
        static_cast<int>(materialReleaseResult),
        static_cast<int>(yamlResult),
        static_cast<int>(yamlQueryResult),
        static_cast<int>(yamlReadResult),
        static_cast<int>(yamlReleaseResult),
        static_cast<int>(archiveResult),
        static_cast<int>(archiveCountResult),
        static_cast<int>(archiveEntryResult),
        static_cast<int>(archiveReadResult),
        static_cast<int>(archiveReleaseResult),
        static_cast<int>(meshCreateResult),
        static_cast<int>(consumerCreateResult),
        static_cast<int>(meshResult),
        static_cast<int>(consumerReleaseResult),
        static_cast<int>(meshReleaseResult),
        static_cast<int>(tracerResult),
        static_cast<int>(tracerReleaseResult),
        ownerCleanupCount);
    WriteLogLine("[loader] ", detail);

    char summary[320] = {};
    _snprintf_s(
        summary,
        sizeof(summary),
        _TRUNCATE,
        "DAVA PRIVATE LIVE STATUS class-factory=%s "
        "arbitrary-object-factory=%s native-yaml=%s "
        "resource-archive=%s nmaterial=%s mesh-hot-swap=%s "
        "stock-tracer=%s",
        factoryOk ? "PASS" : "FAIL",
        unknownBlocked ? "SAFE_REVIEWED_ONLY" : "FAIL",
        yamlOk ? "PASS" : "FAIL",
        archiveOk ? "PASS" : "FAIL",
        materialOk ? "PASS" : "FAIL",
        meshOk ? "PASS" : "FAIL",
        tracerOk ? "PASS"
                 : tracerWaitingForBattle ? "WAITING_BATTLE" : "FAIL");
    WriteLogLine("[loader] ", summary);
    return factoryOk && unknownBlocked && yamlOk && archiveOk &&
           materialOk && meshOk && tracerOk;
}

static HRESULT STDMETHODCALLTYPE PresentDetour(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags) {
    const LONG runtimeState =
        InterlockedCompareExchange(&g_runtimeReady, 0, 0);
    const bool warmupComplete =
        runtimeState == 1 &&
        InterlockedIncrement(&g_presentWarmupFrames) >= 30;
    if (warmupComplete &&
        InterlockedCompareExchange(
            &g_runtimeReady, -1, 1) == 1) {
        const WotbModResult loadResult = WotbModRuntime_LoadAll();
        char status[224] = {};
        _snprintf_s(
            status,
            sizeof(status),
            _TRUNCATE,
            "runtime ready load=%d custom-audio=%d native-sound=%d native-resources=%d",
            static_cast<int>(loadResult),
            static_cast<int>(g_audioBackendResult),
            static_cast<int>(g_soundBackendResult),
            static_cast<int>(g_resourceBackendResult));
        WriteLogLine("[loader] ", status);
        QueryPerformanceFrequency(&g_counterFrequency);
        InterlockedExchange(&g_runtimeReady, 2);
    }

    if (InterlockedCompareExchange(&g_runtimeReady, 0, 0) == 2 &&
        swapChain) {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        DXGI_SWAP_CHAIN_DESC description = {};
        uint32_t width = 0;
        uint32_t height = 0;
        bool deviceReady = false;

        if (SUCCEEDED(swapChain->GetDevice(
                __uuidof(ID3D11Device),
                reinterpret_cast<void**>(&device))) &&
            device) {
            if (SUCCEEDED(device->GetDeviceRemovedReason())) {
                device->GetImmediateContext(&context);
                deviceReady = context != nullptr;
            }
        }
        if (SUCCEEDED(swapChain->GetDesc(&description))) {
            width = description.BufferDesc.Width;
            height = description.BufferDesc.Height;
        }
        if (deviceReady) {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(swapChain->GetBuffer(
                    0u,
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&backBuffer))) &&
                backBuffer) {
                D3D11_TEXTURE2D_DESC backBufferDescription = {};
                backBuffer->GetDesc(&backBufferDescription);
                width = backBufferDescription.Width;
                height = backBufferDescription.Height;
                backBuffer->Release();
            }
        }
        uint32_t uiWidth = width;
        uint32_t uiHeight = height;
        UINT uiDpi = USER_DEFAULT_SCREEN_DPI;
        if (description.OutputWindow) {
            RECT clientArea = {};
            if (GetClientRect(description.OutputWindow, &clientArea)) {
                const LONG clientWidth = clientArea.right - clientArea.left;
                const LONG clientHeight = clientArea.bottom - clientArea.top;
                if (clientWidth > 0 && clientHeight > 0) {
                    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
                    static const GetDpiForWindowFn getDpiForWindow =
                        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(
                            GetModuleHandleW(L"user32.dll"),
                            "GetDpiForWindow"));
                    if (getDpiForWindow) {
                        const UINT windowDpi =
                            getDpiForWindow(description.OutputWindow);
                        if (windowDpi != 0u) {
                            uiDpi = windowDpi;
                        }
                    }
                    const int logicalWidth = MulDiv(
                        clientWidth,
                        USER_DEFAULT_SCREEN_DPI,
                        static_cast<int>(uiDpi));
                    const int logicalHeight = MulDiv(
                        clientHeight,
                        USER_DEFAULT_SCREEN_DPI,
                        static_cast<int>(uiDpi));
                    if (logicalWidth > 0 && logicalHeight > 0) {
                        uiWidth = static_cast<uint32_t>(logicalWidth);
                        uiHeight = static_cast<uint32_t>(logicalHeight);
                    }
                }
            }
        }
        static uint32_t lastUiWidth = 0u;
        static uint32_t lastUiHeight = 0u;
        static UINT lastUiDpi = 0u;
        if (uiWidth != lastUiWidth || uiHeight != lastUiHeight ||
            uiDpi != lastUiDpi) {
            char uiStatus[160] = {};
            _snprintf_s(
                uiStatus,
                sizeof(uiStatus),
                _TRUNCATE,
                "DAVA UI viewport=%ux%u backbuffer=%ux%u dpi=%u",
                uiWidth,
                uiHeight,
                width,
                height,
                uiDpi);
            WriteLogLine("[loader] ", uiStatus);
            lastUiWidth = uiWidth;
            lastUiHeight = uiHeight;
            lastUiDpi = uiDpi;
        }
        wotbmod::v3::SetClientHostUiViewportSize(
            uiWidth,
            uiHeight);
        InterlockedExchange(
            &g_uiInputDpi,
            static_cast<LONG>(uiDpi));

        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        double deltaSeconds = 0.0;
        if (g_lastFrameCounter.QuadPart != 0 &&
            g_counterFrequency.QuadPart != 0) {
            deltaSeconds =
                static_cast<double>(
                    now.QuadPart - g_lastFrameCounter.QuadPart) /
                static_cast<double>(g_counterFrequency.QuadPart);
        }
        g_lastFrameCounter = now;
        /* Frame hitch evidence (2026-09-05): a Present more than 200 ms after
         * the previous one is logged with its length, so a freeze the player
         * feels can be lined up with whatever the log shows just before it.
         * Rate-limited to one line per second; the first ten seconds after
         * start are skipped because loading legitimately stalls Present. */
        if (deltaSeconds > 0.2 && deltaSeconds <= 30.0) {
            static LONGLONG s_lastHitchLog = 0;
            static LONG s_hitchCount = 0;
            ++s_hitchCount;
            const LONGLONG sinceLast = now.QuadPart - s_lastHitchLog;
            if (g_counterFrequency.QuadPart != 0 &&
                sinceLast > g_counterFrequency.QuadPart &&
                InterlockedCompareExchange(&g_dispatchFrameSerial, 0, 0) > 600) {
                s_lastHitchLog = now.QuadPart;
                const double toMs = g_counterFrequency.QuadPart != 0
                    ? 1000.0 / static_cast<double>(g_counterFrequency.QuadPart) : 0.0;
                const LONGLONG engine = InterlockedExchange64(&g_wotbProfEngineTicks, 0);
                const LONGLONG engineCalls = InterlockedExchange64(&g_wotbProfEngineCalls, 0);
                const LONGLONG pump = InterlockedExchange64(&g_wotbProfPumpTicks, 0);
                const LONGLONG sceneDraw = InterlockedExchange64(&g_wotbProfSceneDrawTicks, 0);
                const LONGLONG fileCreate = InterlockedExchange64(&g_wotbProfFileCreateTicks, 0);
                const LONGLONG fileCalls = InterlockedExchange64(&g_wotbProfFileCreateCalls, 0);
                const LONGLONG uiInput = InterlockedExchange64(&g_wotbProfUiInputTicks, 0);
                const LONGLONG uiInputCalls = InterlockedExchange64(&g_wotbProfUiInputCalls, 0);
                const LONGLONG ges = InterlockedExchange64(&g_wotbProfGesTicks, 0);
                const LONGLONG gesCalls = InterlockedExchange64(&g_wotbProfGesCalls, 0);
                const LONGLONG presentHook = InterlockedExchange64(&g_wotbProfPresentHookTicks, 0);
                const LONGLONG pollUi = InterlockedExchange64(&g_wotbProfPollUiTicks, 0);
                const LONGLONG updateFrame = InterlockedExchange64(&g_wotbProfUpdateFrameTicks, 0);
                const LONGLONG gesRetry = InterlockedExchange64(&g_wotbProfGesRetryTicks, 0);
                const LONGLONG dispatch = InterlockedExchange64(&g_wotbProfDispatchTicks, 0);
                const LONGLONG pGet = InterlockedExchange64(&g_wotbProfPollGetScreenTicks, 0);
                const LONGLONG pUpd = InterlockedExchange64(&g_wotbProfPollUpdateTicks, 0);
                const LONGLONG pUpdCalls = InterlockedExchange64(&g_wotbProfPollUpdateCalls, 0);
                const LONGLONG pUpdFull = InterlockedExchange64(&g_wotbProfPollUpdateFullCalls, 0);
                const LONGLONG pApply = InterlockedExchange64(&g_wotbProfPollApplyTicks, 0);
                const LONGLONG pRel = InterlockedExchange64(&g_wotbProfPollReleaseTicks, 0);
                char hitch[700] = {};
                _snprintf_s(hitch, sizeof(hitch), _TRUNCATE,
                            "frame hitch: %.0f ms between presents (hitches so far %ld); since last report: engine-frame %.0f ms/%lld calls, main pump %.0f ms, scene-draw hooks %.0f ms, File::Create %lld calls %.0f ms, ui-input hook %lld calls %.0f ms, ges deliveries %lld %.0f ms, present hook %.0f ms (ui-poll %.0f [get-screen %.0f, classify %.0f in %lld calls/%lld full, apply %.0f, release %.0f], bindings-update %.0f, ges-retry %.0f, dispatch %.0f)",
                            deltaSeconds * 1000.0, s_hitchCount,
                            engine * toMs, engineCalls, pump * toMs, sceneDraw * toMs,
                            fileCalls, fileCreate * toMs,
                            uiInputCalls, uiInput * toMs, gesCalls, ges * toMs, presentHook * toMs,
                            pollUi * toMs, pGet * toMs, pUpd * toMs, pUpdCalls, pUpdFull, pApply * toMs, pRel * toMs,
                            updateFrame * toMs, gesRetry * toMs, dispatch * toMs);
                WriteLogLine("[loader] ", hitch);
            }
        }
        if (!(deltaSeconds > 0.0) || deltaSeconds > 1.0) {
            deltaSeconds = 1.0 / 60.0;
        }

        const LONG frameSerial =
            InterlockedIncrement(&g_dispatchFrameSerial);
        if (frameSerial > 120 && frameSerial % 300 == 0 &&
            DavaNativeLiveProbeEnabled()) {
            if (InterlockedCompareExchange(
                    &g_davaNativeTracerLiveProbeState, 0, 0) == 0) {
                ScheduleDavaNativeStockTracerLiveProbe();
            }
            if (InterlockedCompareExchange(
                    &g_davaNativeTracerLiveProbeState, 0, 0) == 2 &&
                InterlockedCompareExchange(
                    &g_davaNativeLiveProbeAttempts, 0, 0) < 3 &&
                InterlockedCompareExchange(
                    &g_davaNativeLiveProbeState, 1, 0) == 0) {
                ScheduleDavaNativeLiveProbe();
            }
        }
        IDXGISwapChain* observedSwapChain =
            deviceReady ? swapChain : nullptr;
        ID3D11Device* observedDevice =
            deviceReady ? device : nullptr;
        ID3D11DeviceContext* observedContext =
            deviceReady ? context : nullptr;
        const LONGLONG presentHookStarted = ProfNow();
        PollUiScreenChanged();
        const LONGLONG afterPoll = ProfNow();
        ProfAdd(&g_wotbProfPollUiTicks, afterPoll - presentHookStarted);
        WotbModV3NativeBindings_UpdateFrame(
            observedSwapChain,
            observedDevice,
            observedContext,
            width,
            height,
            static_cast<uint64_t>(
                frameSerial > 0 ? frameSerial : 0),
            deltaSeconds);
        /* A GES observation asked for before the bus was captured is retried
         * here, on the frame thread, never from inside SubscribeImpl. */
        const LONGLONG afterUpdate = ProfNow();
        ProfAdd(&g_wotbProfUpdateFrameTicks, afterUpdate - afterPoll);
        if (g_gesInstalled) wotbmod::v3::GesRetryPendingObservations();
        /* Judges an in-flight cluster change from the frame thread (context
         * HANGAR coming back, or the 60 s timeout). Cheap when idle. */
        if (g_sessionClusterInstalled) {
            /* The re-login's hangar is built into the existing screen object
             * and creates no UI resource, so no probe gets armed on its own;
             * about once a second while a change is in flight is enough for
             * CONNECTED to be seen within a few seconds (was 60 s late). */
            static unsigned s_clusterProbeFrames = 0u;
            if (wotbmod::v3::SessionClusterChangeInFlight()) {
                if (++s_clusterProbeFrames >= 60u) {
                    s_clusterProbeFrames = 0u;
                    InterlockedExchange(&g_uiContextProbePending, 1);
                }
            } else {
                s_clusterProbeFrames = 0u;
            }
            wotbmod::v3::SessionClusterFrameTick(static_cast<uint64_t>(GetTickCount64()));
        }
        const LONGLONG afterRetry = ProfNow();
        ProfAdd(&g_wotbProfGesRetryTicks, afterRetry - afterUpdate);
        WotbModRuntime_DispatchFrame(
            observedSwapChain,
            observedDevice,
            observedContext,
            width,
            height,
            deltaSeconds);
        ProfAdd(&g_wotbProfDispatchTicks, ProfNow() - afterRetry);
        ProfAdd(&g_wotbProfPresentHookTicks, ProfNow() - presentHookStarted);

        if (context) context->Release();
        if (device) device->Release();
    }
    return g_originalPresent(
        swapChain,
        syncInterval,
        flags);
}

static LRESULT CALLBACK DummyWindowProc(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    return DefWindowProcA(window, message, wParam, lParam);
}

static bool InstallPresentHook() {
    const char* className = "BlitzForgeLiveProbeWindow";
    WNDCLASSEXA windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DummyWindowProc;
    windowClass.hInstance = g_loaderModule;
    windowClass.lpszClassName = className;
    const ATOM atom = RegisterClassExA(&windowClass);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    HWND window = CreateWindowExA(
        0,
        className,
        "",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        g_loaderModule,
        nullptr);
    if (!window) return false;

    DXGI_SWAP_CHAIN_DESC swapDescription = {};
    swapDescription.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 1;
    swapDescription.OutputWindow = window;
    swapDescription.Windowed = TRUE;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const HRESULT createResult = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &swapDescription,
        &swapChain,
        &device,
        &featureLevel,
        &context);
    if (FAILED(createResult) || !swapChain) {
        if (context) context->Release();
        if (device) device->Release();
        DestroyWindow(window);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    void* presentTarget = vtable[8];
    const MH_STATUS createStatus = MH_CreateHook(
        presentTarget,
        &PresentDetour,
        reinterpret_cast<void**>(&g_originalPresent));
    const MH_STATUS enableStatus =
        createStatus == MH_OK ? MH_EnableHook(presentTarget) : createStatus;

    context->Release();
    device->Release();
    swapChain->Release();
    DestroyWindow(window);
    return createStatus == MH_OK && enableStatus == MH_OK;
}

static DWORD WINAPI LoaderThread(void*) {
    g_eventSourceMask = 0u;
    InterlockedExchange(&g_confirmedAllyHighWater, 0);
    InterlockedExchange(&g_avatarFallbackResolved, 0);
    char gameExecutablePath[MAX_PATH] = {};
    char gameDirectory[MAX_PATH] = {};
    GetModuleFileNameA(
        GetModuleHandleA(nullptr),
        gameExecutablePath,
        MAX_PATH);
    strcpy_s(
        gameDirectory,
        sizeof(gameDirectory),
        gameExecutablePath);
    ParentDirectory(gameDirectory);

    char loaderDirectory[MAX_PATH] = {};
    GetModuleFileNameA(g_loaderModule, loaderDirectory, MAX_PATH);
    ParentDirectory(loaderDirectory);

    char modsDirectory[MAX_PATH] = {};
    char logPath[MAX_PATH] = {};
    const DWORD modsDirectoryLength = GetEnvironmentVariableA(
        "WOTBMOD_MODS_DIRECTORY",
        modsDirectory,
        static_cast<DWORD>(sizeof(modsDirectory)));
    const DWORD logPathLength = GetEnvironmentVariableA(
        "WOTBMOD_LOG_PATH",
        logPath,
        static_cast<DWORD>(sizeof(logPath)));
    if ((modsDirectoryLength == 0 &&
         !JoinPath(
             gameDirectory,
             "mods",
             modsDirectory,
             sizeof(modsDirectory))) ||
        modsDirectoryLength >= sizeof(modsDirectory) ||
        (logPathLength == 0 &&
         !JoinPath(
             gameDirectory,
             "wotb_mod_loader.log",
             logPath,
             sizeof(logPath))) ||
        logPathLength >= sizeof(logPath)) {
        return 1;
    }

    CreateDirectoryA(modsDirectory, nullptr);
    /* Log rotation (2026-09-05): a log past 32 MB is renamed to .1 at start
     * (the previous .1 is dropped), so appends never land in a file that has
     * grown for weeks - the 127 MB one found behind a hangar stutter report. */
    {
        WIN32_FILE_ATTRIBUTE_DATA existing = {};
        if (GetFileAttributesExA(logPath, GetFileExInfoStandard, &existing) &&
            (existing.nFileSizeHigh != 0u || existing.nFileSizeLow > 32u * 1024u * 1024u)) {
            char rotated[MAX_PATH] = {};
            _snprintf_s(rotated, sizeof(rotated), _TRUNCATE, "%s.1", logPath);
            MoveFileExA(logPath, rotated, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_logHandle = CreateFileA(
        logPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    WriteLogLine("[loader] ", "BlitzForge live loader starting");
    char locationMessage[MAX_PATH * 2u + 32u] = {};
    _snprintf_s(
        locationMessage,
        sizeof(locationMessage),
        _TRUNCATE,
        "mods=%s log=%s",
        modsDirectory,
        logPath);
    WriteLogLine("[loader] ", locationMessage);

    int32_t crashLoopSafeMode = 0;
    const WotbModResult preflightResult =
        WotbModRuntime_PreflightCrashLoop(
            modsDirectory,
            &RuntimeLog,
            nullptr,
            &crashLoopSafeMode);
    if (preflightResult != WOTBMOD_OK) {
        WriteLogLine(
            "[loader] ",
            "crash-loop preflight failed; native setup is blocked");
        return 2;
    }
    if (crashLoopSafeMode != 0) {
        WotbModRuntimeOptions safeRuntimeOptions = {};
        safeRuntimeOptions.struct_size = sizeof(safeRuntimeOptions);
        safeRuntimeOptions.game_directory = gameDirectory;
        safeRuntimeOptions.mods_directory = modsDirectory;
        safeRuntimeOptions.game_module = GetModuleHandleA(nullptr);
        safeRuntimeOptions.log_sink = &RuntimeLog;
        const WotbModResult safeInitializeResult =
            WotbModRuntime_Initialize(&safeRuntimeOptions);
        if (safeInitializeResult != WOTBMOD_OK) {
            WriteLogLine(
                "[loader] ",
                "safe-mode runtime initialization failed");
            return 4;
        }
        WotbModV3Runtime_SetEventSourceMask(0u);
        const WotbModResult safeLoadResult =
            WotbModRuntime_LoadAll();
        char safeStatus[160] = {};
        _snprintf_s(
            safeStatus,
            sizeof(safeStatus),
            _TRUNCATE,
            "SAFE MODE active before native setup; "
            "MinHook, fixed-RVA bindings and third-party mods skipped "
            "(load=%d)",
            static_cast<int>(safeLoadResult));
        WriteLogLine("[loader] ", safeStatus);
        QueryPerformanceFrequency(&g_counterFrequency);
        InterlockedExchange(&g_runtimeReady, 2);
        return 0;
    }

    WotbModRuntime_SetCrashLoopPhase("native_backend_setup");
    if (MH_Initialize() != MH_OK) {
        WriteLogLine("[loader] ", "MinHook initialization failed");
        return 2;
    }
    if (!InstallPresentHook()) {
        WriteLogLine("[loader] ", "Present hook installation failed");
        return 3;
    }

    /*
     * Fixed-RVA gate.
     *
     * Everything below this point resolves game functions by hardcoded RVA,
     * so the client fingerprint is verified here - before the first such
     * detour is installed, not after. On a build we did not generate the pack
     * for, the loader keeps MinHook and the RVA-independent Present hook and
     * skips every RVA-derived backend; portable backends stay available.
     */
    const WotbModV3Result fingerprintResult =
        WotbModV3NativeBindings_VerifyClientFingerprint(
            GetModuleHandleA(nullptr),
            nullptr,
            &NativeBindingsLog,
            nullptr);
    g_nativeFingerprintVerified =
        fingerprintResult == WOTBMOD_V3_OK;
    if (!g_nativeFingerprintVerified) {
        char fingerprintStatus[192] = {};
        _snprintf_s(
            fingerprintStatus,
            sizeof(fingerprintStatus),
            _TRUNCATE,
            "client fingerprint not verified (result=%d); all fixed-RVA "
            "native bindings are disabled, portable backends remain",
            static_cast<int>(fingerprintResult));
        WriteLogLine("[loader] ", fingerprintStatus);
    }

    WotbModWindowsAudioOptions windowsAudioOptions = {};
    windowsAudioOptions.struct_size = sizeof(windowsAudioOptions);
    WotbModWindowsAudioHandle windowsAudioHandle = nullptr;
    WotbModRuntimeAudioBackend audioBackend = {};
    const WotbModResult audioResult = WotbModWindowsAudio_Create(
        &windowsAudioOptions,
        &windowsAudioHandle,
        &audioBackend);
    g_audioBackendResult = audioResult;

    /*
     * Both DAVA backends resolve their targets from the fixed-RVA table, and
     * InstallSceneTrackingHooks() below installs three detours on addresses
     * that table produced. All of it is therefore behind the fingerprint
     * gate; on a mismatch the resource path falls back to
     * fallbackResourceBackend (portable) further down.
     */
    WotbModDavaSoundHandle davaSoundHandle = nullptr;
    WotbModRuntimeSoundBackend soundBackend = {};
    WotbModResult soundResult = WOTBMOD_ERROR_PLATFORM;
    WotbModDavaResourcesHandle davaResourcesHandle = nullptr;
    WotbModRuntimeResourceBackend davaResourceBackend = {};
    WotbModResult davaResourcesResult = WOTBMOD_ERROR_PLATFORM;
    if (g_nativeFingerprintVerified) {
        WotbModDavaSoundOptions davaSoundOptions = {};
        davaSoundOptions.struct_size = sizeof(davaSoundOptions);
        davaSoundOptions.game_module = GetModuleHandleA(nullptr);
        soundResult = WotbModDavaSound_Create(
            &davaSoundOptions,
            &davaSoundHandle,
            &soundBackend);

        WotbModDavaResourcesOptions davaResourcesOptions = {};
        davaResourcesOptions.struct_size = sizeof(davaResourcesOptions);
        davaResourcesOptions.game_module = GetModuleHandleA(nullptr);
        davaResourcesOptions.log_sink = &RuntimeLog;
        davaResourcesOptions.stock_tracer_create =
            &WotbModV3NativeBindings_CreateStockTracer;
        davaResourcesOptions.is_main_thread =
            &WotbModV3NativeBindings_IsMainThread;
        davaResourcesOptions.invoke_main_thread =
            &WotbModV3NativeBindings_InvokeMainThread;
        davaResourcesResult = WotbModDavaResources_Create(
            &davaResourcesOptions,
            &davaResourcesHandle,
            &davaResourceBackend);
    } else {
        WriteLogLine(
            "[loader] ",
            "DAVA sound/resource bindings and Scene trackers skipped: "
            "client fingerprint not verified");
    }
    g_soundBackendResult = soundResult;
    g_resourceBackendResult = davaResourcesResult;
    if (davaResourcesResult == WOTBMOD_OK) {
        g_forwardResourceRegistryChanged =
            davaResourceBackend.registry_changed;
        davaResourceBackend.registry_changed =
            &ResourceRegistryChanged;
        g_davaResourcesHandle = davaResourcesHandle;
        g_liveResourceBackend = davaResourceBackend;
        if (g_liveResourceBackend.ui_get_active_screen &&
            g_liveResourceBackend.release) {
            g_eventSourceMask |=
                WOTBMOD_V3_EVENT_SOURCE_UI_SCREEN;
        }
        if (!InstallSceneTrackingHooks(davaResourcesHandle)) {
            WriteLogLine(
                "[loader] ",
                "active Scene API will remain unavailable");
        }
        WotbModDavaNativeBackend nativeDavaBackend = {};
        nativeDavaBackend.struct_size = sizeof(nativeDavaBackend);
        const WotbModV3Result nativeDavaBackendResult =
            WotbModDavaResources_GetNativeBackend(
                davaResourcesHandle,
                &nativeDavaBackend);
        const WotbModV3Result nativeDavaInstallResult =
            nativeDavaBackendResult == WOTBMOD_V3_OK
                ? wotbmod::v3::InstallDavaNativeBackend(
                      &nativeDavaBackend)
                : nativeDavaBackendResult;
        if (nativeDavaInstallResult != WOTBMOD_V3_OK) {
            char nativeDavaStatus[176] = {};
            _snprintf_s(
                nativeDavaStatus,
                sizeof(nativeDavaStatus),
                _TRUNCATE,
                "reviewed DAVA class provider not installed result=%d",
                static_cast<int>(nativeDavaInstallResult));
            WriteLogLine("[loader] ", nativeDavaStatus);
        }
    }

    WotbModRuntimeHookBackend hookBackend = {};
    hookBackend.struct_size = sizeof(hookBackend);
    hookBackend.create = &HookCreate;
    hookBackend.enable = &HookEnable;
    hookBackend.disable = &HookDisable;
    hookBackend.remove = &HookRemove;

    WotbModV3NativeHookBackend v3HookBackend = {};
    v3HookBackend.struct_size = sizeof(v3HookBackend);
    v3HookBackend.api_version =
        WOTBMOD_V3_NATIVE_HOOK_BACKEND_VERSION;
    v3HookBackend.resolve_symbol = &V3HookResolveSymbol;
    v3HookBackend.create = &V3HookCreate;
    v3HookBackend.enable = &V3HookEnable;
    v3HookBackend.disable = &V3HookDisable;
    v3HookBackend.remove = &V3HookRemove;
    v3HookBackend.describe_target = &V3HookDescribeTarget;
    v3HookBackend.attach = &V3HookAttach;
    v3HookBackend.detach = &V3HookDetach;
    v3HookBackend.set_attached_enabled = &V3HookSetAttachedEnabled;

    WotbModRuntimeGameplayBackend gameplayBackend = {};
    gameplayBackend.struct_size = sizeof(gameplayBackend);
    gameplayBackend.vehicle_get_local = &VehicleGetLocal;
    gameplayBackend.vehicle_get_by_entity_id =
        &VehicleGetByEntityId;
    gameplayBackend.vehicle_get_count = &VehicleGetCount;
    gameplayBackend.vehicle_get_at = &VehicleGetAt;
    gameplayBackend.vehicle_clone = &VehicleClone;
    gameplayBackend.vehicle_release = &VehicleRelease;
    gameplayBackend.vehicle_get_info = &VehicleGetInfo;

    WotbModRuntimeResourceBackend fallbackResourceBackend = {};
    fallbackResourceBackend.struct_size =
        sizeof(fallbackResourceBackend);
    fallbackResourceBackend.load = &ResourceLoad;
    fallbackResourceBackend.reload = &ResourceReload;
    fallbackResourceBackend.release = &ResourceRelease;
    fallbackResourceBackend.registry_changed =
        &ResourceRegistryChanged;
    const WotbModRuntimeResourceBackend* resourceBackend =
        davaResourcesResult == WOTBMOD_OK
            ? &davaResourceBackend
            : &fallbackResourceBackend;

    char bindingValidationReportPath[MAX_PATH] = {};
    _snprintf_s(
        bindingValidationReportPath,
        sizeof(bindingValidationReportPath),
        _TRUNCATE,
        "%s\\cache\\binding_pack_validation.json",
        modsDirectory);
    WotbModV3NativeBindingsOptions nativeBindingsOptions = {};
    nativeBindingsOptions.struct_size =
        sizeof(nativeBindingsOptions);
    nativeBindingsOptions.game_module = GetModuleHandleA(nullptr);
    nativeBindingsOptions.game_executable_path =
        gameExecutablePath;
    nativeBindingsOptions.binding_validation_report_path =
        bindingValidationReportPath;
    nativeBindingsOptions.log = &NativeBindingsLog;
    nativeBindingsOptions.resource_backend = resourceBackend;
    if (davaResourcesResult == WOTBMOD_OK) {
        nativeBindingsOptions.resource_identity =
            &WotbModDavaResources_GetNativeObject;
        nativeBindingsOptions.resource_identity_user_data =
            davaResourcesHandle;
        nativeBindingsOptions.resource_bring_ui_to_front =
            &WotbModDavaResources_BringUiToFront;
        nativeBindingsOptions.resource_bring_ui_to_front_user_data =
            davaResourcesHandle;
    }
    nativeBindingsOptions.audio_backend =
        audioResult == WOTBMOD_OK ? &audioBackend : nullptr;
    nativeBindingsOptions.sound_backend =
        soundResult == WOTBMOD_OK ? &soundBackend : nullptr;
    nativeBindingsOptions.gameplay_backend = &gameplayBackend;
    nativeBindingsOptions.avatar_observed =
        &ObserveAvatarVehicle;
    g_v3NativeBindingsResult =
        WotbModV3NativeBindings_Create(
            &nativeBindingsOptions,
            &g_v3ClientBackend);
    if (g_v3NativeBindingsResult != WOTBMOD_V3_OK) {
        char nativeStatus[160] = {};
        _snprintf_s(
            nativeStatus,
            sizeof(nativeStatus),
            _TRUNCATE,
            "V3 native binding pack unavailable result=%d",
            static_cast<int>(g_v3NativeBindingsResult));
        WriteLogLine("[loader] ", nativeStatus);
    }

    WotbModRuntimeOptions runtimeOptions = {};
    runtimeOptions.struct_size = sizeof(runtimeOptions);
    runtimeOptions.game_directory = gameDirectory;
    runtimeOptions.mods_directory = modsDirectory;
    runtimeOptions.game_module = GetModuleHandleA(nullptr);
    runtimeOptions.log_sink = &RuntimeLog;
    runtimeOptions.hook_backend = &hookBackend;
    runtimeOptions.resource_backend = resourceBackend;
    runtimeOptions.audio_backend =
        audioResult == WOTBMOD_OK ? &audioBackend : nullptr;
    runtimeOptions.sound_backend =
        soundResult == WOTBMOD_OK ? &soundBackend : nullptr;
    runtimeOptions.gameplay_backend = &gameplayBackend;
    runtimeOptions.v3_client_backend =
        g_v3NativeBindingsResult == WOTBMOD_V3_OK
            ? &g_v3ClientBackend
            : nullptr;
    runtimeOptions.v3_hook_backend =
        g_v3NativeBindingsResult == WOTBMOD_V3_OK &&
                g_v3ClientBackend.compatibility_state ==
                    WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED
            ? &v3HookBackend
            : nullptr;
#if WOTBMOD_DEVELOPER_UNSAFE_LEGACY_API
    char unsafeLegacyValue[16] = {};
    const DWORD unsafeLegacyLength = GetEnvironmentVariableA(
        "WOTBMOD_ENABLE_LEGACY_RAW_API",
        unsafeLegacyValue,
        static_cast<DWORD>(sizeof(unsafeLegacyValue)));
    if (unsafeLegacyLength > 0u &&
        unsafeLegacyLength < sizeof(unsafeLegacyValue) &&
        (strcmp(unsafeLegacyValue, "1") == 0 ||
         _stricmp(unsafeLegacyValue, "true") == 0)) {
        runtimeOptions.flags |=
            WOTBMOD_RUNTIME_OPTION_ALLOW_LEGACY_RAW_PROCESS_API;
        WriteLogLine(
            "[warning] ",
            "developer build enabled legacy raw process/hook API");
    }
#endif

    const WotbModResult initializeResult =
        WotbModRuntime_Initialize(&runtimeOptions);
    if (initializeResult != WOTBMOD_OK) {
        wotbmod::v3::RemoveDavaNativeBackend();
        WotbModV3NativeBindings_Shutdown();
        WriteLogLine("[loader] ", "runtime initialization failed");
        return 4;
    }
    WotbModV3Runtime_SetEventSourceMask(g_eventSourceMask);
    /*
     * The five interfaces declared 2026-08-16 are offered here and not from
     * WotbModV3NativeBindings_Create: installing the table republishes their
     * availability, which needs the runtime's interface registry to already
     * exist. A refusal leaves all five UNAVAILABLE with their frozen reasons,
     * which is a correct outcome and not a startup failure.
     */
    if (g_v3NativeBindingsResult == WOTBMOD_V3_OK) {
        const WotbModV3Result declaredResult =
            WotbModV3NativeBindings_InstallDeclaredBackend();
        if (declaredResult != WOTBMOD_V3_OK) {
            char declaredStatus[176] = {};
            _snprintf_s(
                declaredStatus,
                sizeof(declaredStatus),
                _TRUNCATE,
                "declared V3 client backend not installed result=%d; "
                "ui.read/camera.state/audio.intercept/scene.enumerate/"
                "tracer stay unavailable",
                static_cast<int>(declaredResult));
            WriteLogLine("[loader] ", declaredStatus);
        }
    }
    if (!InstallDavaFileResolverHook()) {
        WriteLogLine(
            "[loader] ",
            "native resource overlays and vehicle skins are unavailable");
    }
    if (!InstallCamoProbeHooks()) {
        /* Silent by design: the probe is a measurement tool, not a feature.
         * Its own installer logs the reason whenever it was ARMED (marker
         * file present) but could not install; when disarmed there is
         * nothing anyone needs to read. */
    }
    if (!InstallGameplayHooks()) {
        WriteLogLine(
            "[loader] ",
            "vehicle registry is incomplete; gameplay API will report only confirmed hooks");
    }
    /* The declared backend was installed before the gameplay hooks, so the
     * ges_* slots were not yet available; re-install it now that GesInit and
     * the capture hook have run. */
    if (g_v3NativeBindingsResult == WOTBMOD_V3_OK &&
        (g_gesInstalled || g_sessionClusterInstalled)) {
        if (g_gesInstalled) {
            WotbModV3NativeBindings_SetGesProvider(
                &WotbModLoader_GesListTypes, &WotbModLoader_GesObserve,
                &WotbModLoader_GesPublish);
        }
        if (g_sessionClusterInstalled) {
            WotbModV3NativeBindings_SetSessionClusterProvider(
                &WotbModLoader_SessionClusterEnumerate,
                &WotbModLoader_SessionClusterGetCurrent,
                &WotbModLoader_SessionClusterChange,
                &WotbModLoader_SessionClusterSetManual);
        }
        WotbModV3NativeBindings_InstallDeclaredBackend();
    }
    WriteLogLine(
        "[loader] ",
        "runtime initialized; selecting mod load phase");
    char earlyLoadValue[8] = {};
    const bool earlyLoad =
        GetEnvironmentVariableA(
            "WOTBMOD_EARLY_LOAD",
            earlyLoadValue,
            static_cast<DWORD>(sizeof(earlyLoadValue))) > 0 &&
        strcmp(earlyLoadValue, "0") != 0;
    if (earlyLoad) {
        const WotbModResult loadResult =
            WotbModRuntime_LoadAll();
        char status[192] = {};
        _snprintf_s(
            status,
            sizeof(status),
            _TRUNCATE,
            "early production mod load=%d",
            static_cast<int>(loadResult));
        WriteLogLine("[loader] ", status);
        QueryPerformanceFrequency(&g_counterFrequency);
        InterlockedExchange(&g_runtimeReady, 2);
    } else {
        InterlockedExchange(&g_runtimeReady, 1);
    }
    return 0;
}

struct PendingDavaNativeRelease {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModDavaNativeToken token = 0u;
};

WotbModV3Result WOTBMOD_V3_CALL RunPendingDavaNativeRelease(
    void* userData) {
    PendingDavaNativeRelease* pending =
        static_cast<PendingDavaNativeRelease*>(userData);
    if (!pending) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const WotbModV3Result result =
        wotbmod::v3::InstalledDavaNativeRelease(
            pending->owner, pending->token);
    delete pending;
    return result;
}

void WOTBMOD_V3_CALL CancelPendingDavaNativeRelease(void* userData) {
    delete static_cast<PendingDavaNativeRelease*>(userData);
}

} /* namespace */

/* Loader-private bridge used by the separately packaged Lua host. The public
 * V3 headers stay frozen: only opaque registry tokens cross this boundary and
 * every operation is still checked against the owning mod handle. */
extern "C" __declspec(dllexport) uint64_t WOTBMOD_CALL
WotbModLoader_DavaNativeCapabilities() {
    return wotbmod::v3::InstalledDavaNativeCapabilities();
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeClassIsRegistered(
    const char* className,
    uint32_t* outRegistered) {
    return wotbmod::v3::InstalledDavaNativeClassIsRegistered(
        className, outRegistered);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeClassCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* outObject) {
    return wotbmod::v3::InstalledDavaNativeClassCreate(
        owner, request, outObject);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeMaterialMutate(
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation) {
    return wotbmod::v3::InstalledDavaNativeMaterialMutate(
        owner, material, mutation);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeMeshHotSwap(
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacementMesh) {
    return wotbmod::v3::InstalledDavaNativeMeshHotSwap(
        owner, consumer, replacementMesh);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeTracerCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* outTracer) {
    return wotbmod::v3::InstalledDavaNativeTracerCreate(
        owner, request, outTracer);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeSceneMaterial(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* outMaterial) {
    return wotbmod::v3::InstalledDavaNativeSceneMaterial(
        owner, request, outMaterial);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeSceneNodes(
    char* buffer,
    uint32_t* inoutSize) {
    return wotbmod::v3::InstalledDavaNativeSceneNodes(buffer, inoutSize);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeSceneBatch(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* outBatch) {
    return wotbmod::v3::InstalledDavaNativeSceneBatch(
        owner, request, outBatch);
}

/*
 * The native customization editor's visibility mask - data patch, not a hook.
 *
 * The shipped client drops any camouflage whose server lock state falls
 * outside the bitmask at kCamouflagesVisibilityMaskRva before a card is ever
 * built for it, which is why a locally-authored entry never appeared no matter
 * how correct its yaml was. These two exports read and widen that one dword.
 * Everything about them is fail-closed: an unverified fingerprint, a missing
 * game module or a mask outside the three defined states leaves memory
 * untouched. The caller owns symmetry: Get before Set, restore what it read.
 */
namespace {

WotbModV3Result CamouflagesMaskAddress(uint32_t** outAddress) {
    if (!outAddress) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *outAddress = nullptr;
    if (!g_nativeFingerprintVerified) {
        WriteLogLine(
            "[loader] ",
            "camouflage visibility mask unavailable: client fingerprint "
            "not verified");
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    HMODULE gameModule = GetModuleHandleA(nullptr);
    if (!gameModule) return WOTBMOD_V3_E_PLATFORM;
    *outAddress = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(gameModule) +
        kCamouflagesVisibilityMaskRva);
    return WOTBMOD_V3_OK;
}

} /* namespace */

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_CamouflageVisibilityGet(uint32_t* outMask) {
    uint32_t* address = nullptr;
    const WotbModV3Result result = CamouflagesMaskAddress(&address);
    if (result != WOTBMOD_V3_OK) return result;
    if (!outMask) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    __try {
        *outMask = *address;
        return WOTBMOD_V3_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_V3_E_PLATFORM;
    }
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_CamouflageVisibilitySet(
    uint32_t mask,
    uint32_t* outPreviousMask) {
    /* Only the three defined lock states exist; anything wider would be a
     * write past the meaning of the field. */
    if ((mask & ~0x7u) != 0u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    uint32_t* address = nullptr;
    const WotbModV3Result result = CamouflagesMaskAddress(&address);
    if (result != WOTBMOD_V3_OK) return result;
    DWORD oldProtect = 0u;
    __try {
        if (!VirtualProtect(
                address,
                sizeof(*address),
                PAGE_READWRITE,
                &oldProtect)) {
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    const uint32_t previous = *address;
    *address = mask;
    DWORD ignored = 0u;
    VirtualProtect(address, sizeof(*address), oldProtect, &ignored);
    if (outPreviousMask) *outPreviousMask = previous;
    char message[96] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "camouflage visibility mask %u -> %u",
        previous,
        mask);
    WriteLogLine("[loader] ", message);
    return WOTBMOD_V3_OK;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeRelease(
    WotbModV3Handle owner,
    WotbModDavaNativeToken object) {
    return wotbmod::v3::InstalledDavaNativeRelease(owner, object);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_CALL
WotbModLoader_DavaNativeReleaseAsync(
    WotbModV3Handle owner,
    WotbModDavaNativeToken object) {
    if (owner == WOTBMOD_V3_INVALID_HANDLE || object == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    PendingDavaNativeRelease* pending =
        new (std::nothrow) PendingDavaNativeRelease();
    if (!pending) return WOTBMOD_V3_E_LIMIT_REACHED;
    pending->owner = owner;
    pending->token = object;
    const WotbModV3Result result = wotbmod::v3::PostMainThreadEx(
        &RunPendingDavaNativeRelease,
        pending,
        &CancelPendingDavaNativeRelease);
    if (result != WOTBMOD_V3_OK) delete pending;
    return result;
}

/* ---- wotbmod.ges declared-backend slots (see client_services_backend.h) ---- */

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesListTypes(
    void*, wotbmod::v3::ClientHostGesTypeVisitFn visit, void* data) {
    if (!visit) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!g_gesInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const uint32_t count = wotbmod::loader::ges::GesTypeCount();
    for (uint32_t i = 0u; i < count; ++i) {
        visit(data, wotbmod::loader::ges::GesTypeName(i));
    }
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesObserve(
    void*, const char* typeName, uint32_t observe) {
    if (!g_gesInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return wotbmod::loader::ges::GesObserve(typeName, observe != 0u);
}

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_GesPublish(
    void*, const char* typeName, const void* payload, uint32_t size, uint32_t flags) {
    if (!g_gesInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (WotbModV3NativeBindings_IsMainThread(nullptr) == 0u) {
        return WOTBMOD_V3_E_WRONG_THREAD;
    }
    return wotbmod::loader::ges::GesPublish(typeName, payload, size, flags);
}

extern "C" uint32_t WOTBMOD_CALL WotbModLoader_GesInstalled(void) {
    return g_gesInstalled ? 1u : 0u;
}

/* ---- wotbmod.session.cluster declared-backend slots ---- */

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterEnumerate(
    void*, WotbModV3ClusterInfo* items, uint32_t* inoutCount) {
    if (!g_sessionClusterInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return wotbmod::loader::session_cluster::Enumerate(items, inoutCount);
}

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterGetCurrent(
    void*, WotbModV3ClusterInfo* outInfo) {
    if (!g_sessionClusterInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return wotbmod::loader::session_cluster::GetCurrent(outInfo);
}

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterChange(
    void*, int32_t clusterId) {
    if (!g_sessionClusterInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return wotbmod::loader::session_cluster::Change(clusterId);
}

extern "C" WotbModV3Result WOTBMOD_CALL WotbModLoader_SessionClusterSetManual(
    void*, uint32_t manual) {
    if (!g_sessionClusterInstalled) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return wotbmod::loader::session_cluster::SetManual(manual != 0u);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_loaderModule = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_logLock);
        HANDLE thread = CreateThread(
            nullptr,
            0,
            &LoaderThread,
            nullptr,
            0,
            nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        /* Orderly ExitProcess fallback; crashes retain the forensic marker. */
        wotbmod::loader::ges::GesShutdown();
        wotbmod::loader::session_cluster::Shutdown();
        hobs::UnregisterAll();
        WotbModRuntime_ProcessDetach();
    }
    return TRUE;
}
