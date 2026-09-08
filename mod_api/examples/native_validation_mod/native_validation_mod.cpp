#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "wotb_mod_api_v3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kModId = "wotbmod.native_validation";
// Compiled into the V3 descriptor, and the loader requires it to match
// manifest.json exactly - a package whose manifest says one version and whose
// binary says another is refused outright. So this and the manifest move
// together, always. Bumping only the manifest ships a mod that cannot load.
constexpr const char* kModVersion = "1.1.1";
constexpr uint64_t kMaximumTraceBytes = 16ull * 1024ull * 1024ull;

enum class ValidationStatus : uint32_t {
    Discovered,
    StaticallyVerified,
    Bound,
    HostTested,
    LiveTestPending,
    LiveTested,
    StressTested,
    Supported,
    Failed,
    NotSupported
};

enum class ManualVerdict : uint32_t {
    None,
    Pass,
    Fail,
    Skip
};

enum class SafeTest : uint32_t {
    None,
    ClientFingerprint,
    DeviceInfo,
    LeaveToHangar,
    LifecycleInfo,
    DeferredReload,
    CameraSnapshot,
    EventQueue,
    EntitySnapshot,
    RpcPolicy,
    ProjectileInvalidHandle,
    RenderSnapshot,
    RenderNativeSnapshot,
    UiInspector,
    SceneInspector,
    MaterialInspector,
    AudioTone,
    ResourceText,
    PortableYaml,
    PortableArchive,
    PortableDvpl,
    NativeSoundEvent,
    NativeLoaderAvailability,
    ReloadWithSubscription,
    ReloadWithAsyncResource,
    ReloadFromDeferredCallback,
    CrashRecovery,
    LastError,
    OwnerCleanup,
    AsyncResourceLoad,
    HookSymbolProbe,
    GesProbe,
    UiLiveText,
    HudStock,
    HudReset,
    VehicleState,
    SessionCluster
};

struct CapabilitySpec {
    const char* section;
    const char* id;
    const char* interface_id;
    const char* binding_id;
    bool native;
    bool deliberately_unsupported;
    SafeTest safe_test;
    ValidationStatus baseline;
};

struct CapabilityState {
    ValidationStatus status = ValidationStatus::Discovered;
    ManualVerdict verdict = ManualVerdict::None;
    bool backend_available = false;
    bool crash_disabled = false;
    uint64_t call_count = 0u;
    uint64_t last_event_unix_ms = 0u;
    uint64_t first_seen_unix_ms = 0u;
    uint64_t handle = 0u;
    uint32_t generation = 0u;
    uint32_t callback_thread = 0u;
    uint32_t active_handles = 0u;
    WotbModV3Result last_code = WOTBMOD_V3_E_NOT_SUPPORTED;
    std::string last_result = "not run";
    std::string last_error = "";
    std::string comment = "";
    bool warning = false;
};

enum class LiveSeverity : uint32_t {
    Info,
    Pass,
    Warn,
    Fail
};

enum class SoundNotificationMode : uint32_t {
    Off,
    FailOnly,
    PassAndFail,
    SelectedEvents
};

struct LiveLogEntry {
    uint64_t timestamp_ms = 0u;
    size_t capability = 0u;
    LiveSeverity severity = LiveSeverity::Info;
    std::string category;
    std::string title;
    std::string api;
    std::string event_name;
    std::string payload;
    uint32_t thread_id = 0u;
};

struct QueuedLogLine {
    std::string path;
    std::string line;
};

struct ShotChain {
    uint64_t started_ms = 0u;
    uint64_t last_ms = 0u;
    bool shell_fired = false;
    bool projectile_created = false;
    bool in_flight = false;
    bool tracer_created = false;
    bool impacted = false;
    bool impact_visual = false;
    bool projectile_destroyed = false;
    bool tracer_destroyed = false;
    bool warned = false;
};

struct EntityObservation {
    uint32_t public_id = 0u;
    uint32_t type = WOTBMOD_V3_PUBLIC_ENTITY_UNKNOWN;
    WotbModV3EntityHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t generation = 0u;
    bool alive = false;
    bool preexisting = false;
    int32_t old_handle_invalidated = -1;
    uint64_t created_unix_ms = 0u;
    uint64_t destroyed_unix_ms = 0u;
};

struct EntityVisitContext {
    uint32_t count = 0u;
    bool preexisting = false;
};

constexpr CapabilitySpec kCapabilities[] = {
    {"Client", "client.fingerprint", WOTBMOD_V3_IFACE_CLIENT,
     "binding-pack/fingerprint", true, false,
     SafeTest::ClientFingerprint, ValidationStatus::StaticallyVerified},
    {"Client", "client.device_info", WOTBMOD_V3_IFACE_DEVICE,
     "win32-dxgi/device-info", false, false,
     SafeTest::DeviceInfo, ValidationStatus::HostTested},
    {"Client", "client.leave_to_hangar", WOTBMOD_V3_IFACE_CLIENT,
     "Client::LeaveToHangar", true, false,
     SafeTest::LeaveToHangar, ValidationStatus::StaticallyVerified},
    {"Lifecycle", "lifecycle.info", WOTBMOD_V3_IFACE_LIFECYCLE,
     "runtime/lifecycle-v1", false, false,
     SafeTest::LifecycleInfo, ValidationStatus::HostTested},
    {"Lifecycle", "lifecycle.cleanup", WOTBMOD_V3_IFACE_LIFECYCLE,
     "runtime/owner-cleanup", false, false,
     SafeTest::OwnerCleanup, ValidationStatus::HostTested},
    {"Lifecycle", "lifecycle.stress_30", WOTBMOD_V3_IFACE_LIFECYCLE,
     "runtime/deferred-unload", false, false,
     SafeTest::None, ValidationStatus::HostTested},
    {"Camera", "camera.active", WOTBMOD_V3_IFACE_CAMERA,
     "GameCamera::ctor/dtor", true, false,
     SafeTest::CameraSnapshot, ValidationStatus::StaticallyVerified},
    {"Camera", "camera.mode_change", WOTBMOD_V3_IFACE_CAMERA,
     "CameraModeChanged", true, false,
     SafeTest::CameraSnapshot, ValidationStatus::StaticallyVerified},
    {"Events", "events.loader_queue", WOTBMOD_V3_IFACE_EVENTS,
     "loader-owned/event-queue", true, false,
     SafeTest::EventQueue, ValidationStatus::HostTested},
    {"Events", "events.callback_thread", WOTBMOD_V3_IFACE_EVENTS,
     "runtime/event-dispatch", true, false,
     SafeTest::EventQueue, ValidationStatus::HostTested},
    {"BigWorld Entity", "entity.enumerate_visible", WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
     "BWEntity::ctor/dtor", true, false,
     SafeTest::EntitySnapshot, ValidationStatus::StaticallyVerified},
    {"BigWorld Entity", "vehicle.state", WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
     "public-registry/vehicle-state", true, false,
     SafeTest::VehicleState, ValidationStatus::HostTested},
    {"BigWorld Entity", "entity.lifecycle", WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
     "public-entity-lifecycle", true, false,
     SafeTest::EntitySnapshot, ValidationStatus::StaticallyVerified},
    {"RPC Metadata", "rpc.incoming_metadata", WOTBMOD_V3_IFACE_BIGWORLD_RPC,
     "curated-rpc/loader-queue", true, false,
     SafeTest::RpcPolicy, ValidationStatus::StaticallyVerified},
    {"Shell / Projectile", "shell.ingress", WOTBMOD_V3_IFACE_PROJECTILE,
     "showShooting/native-ingress", true, false,
     SafeTest::ProjectileInvalidHandle, ValidationStatus::StaticallyVerified},
    {"Shell / Projectile", "projectile.created", WOTBMOD_V3_IFACE_PROJECTILE,
     "projectile-lifecycle/created", true, false,
     SafeTest::ProjectileInvalidHandle, ValidationStatus::StaticallyVerified},
    {"Shell / Projectile", "projectile.updated", WOTBMOD_V3_IFACE_PROJECTILE,
     "projectile-lifecycle/updated", true, false,
     SafeTest::ProjectileInvalidHandle, ValidationStatus::StaticallyVerified},
    {"Shell / Projectile", "projectile.destroyed", WOTBMOD_V3_IFACE_PROJECTILE,
     "projectile-lifecycle/destroyed", true, false,
     SafeTest::ProjectileInvalidHandle, ValidationStatus::StaticallyVerified},
    {"Shell / Projectile", "impact.visual", WOTBMOD_V3_IFACE_PROJECTILE,
     "impact-visual/managed-native-scene", true, false,
     SafeTest::ProjectileInvalidHandle, ValidationStatus::HostTested},
    {"Shell / Projectile", "impact.confirmed", WOTBMOD_V3_IFACE_EVENTS,
     "vehicleHit/confirmed", true, false,
     SafeTest::None, ValidationStatus::StaticallyVerified},
    {"Tracer", "tracer.requested", WOTBMOD_V3_IFACE_PROJECTILE,
     "tracer/requested", true, true,
     SafeTest::None, ValidationStatus::NotSupported},
    {"Tracer", "tracer.created", WOTBMOD_V3_IFACE_PROJECTILE,
     "TracerManager::ctor", true, false,
     SafeTest::None, ValidationStatus::StaticallyVerified},
    {"Tracer", "tracer.visible", WOTBMOD_V3_IFACE_PROJECTILE,
     "visible-tracer/created", true, false,
     SafeTest::None, ValidationStatus::StaticallyVerified},
    {"Tracer", "tracer.destroyed", WOTBMOD_V3_IFACE_PROJECTILE,
     "visible-tracer/destroyed", true, false,
     SafeTest::None, ValidationStatus::StaticallyVerified},
    {"Tracer", "tracer.style", WOTBMOD_V3_IFACE_PROJECTILE,
     "native-tracer-style", true, true,
     SafeTest::None, ValidationStatus::NotSupported},
    {"Render / D3D11 / DXGI", "render.backend", WOTBMOD_V3_IFACE_RENDER,
     "IDXGISwapChain::Present", true, false,
     SafeTest::RenderSnapshot, ValidationStatus::StaticallyVerified},
    {"Render / D3D11 / DXGI", "render.native_borrowed", WOTBMOD_V3_IFACE_RENDER_NATIVE,
     "d3d11/borrowed-handles", true, false,
     SafeTest::RenderNativeSnapshot, ValidationStatus::HostTested},
    {"Render / D3D11 / DXGI", "render.resize", WOTBMOD_V3_IFACE_RENDER,
     "dxgi/swapchain-resized", true, false,
     SafeTest::RenderSnapshot, ValidationStatus::HostTested},
    {"Render / D3D11 / DXGI", "render.device_lifecycle", WOTBMOD_V3_IFACE_RENDER,
     "d3d11/device-lost-restored", true, false,
     SafeTest::RenderSnapshot, ValidationStatus::HostTested},
    {"Render / D3D11 / DXGI", "render.callback", WOTBMOD_V3_IFACE_RENDER,
     "render/PRESENT", true, false,
     SafeTest::RenderSnapshot, ValidationStatus::HostTested},
    {"UI", "ui.validation_panel", WOTBMOD_V3_IFACE_RENDER,
     "managed-renderer/text-overlay", true, false,
     SafeTest::None, ValidationStatus::HostTested},
    {"UI", "ui.readonly_inspector", WOTBMOD_V3_IFACE_UI,
     "ui-v3/active-tree", true, false,
     SafeTest::UiInspector, ValidationStatus::StaticallyVerified},
    {"UI", "ui.live_text", WOTBMOD_V3_IFACE_UI_READ,
     "DAVA::UITextComponent::text", true, false,
     SafeTest::UiLiveText, ValidationStatus::HostTested},
    // Battle only: the stock HUD controls (Minimap, Lamp, GunAim,
    // RibbonsContainer, DamageStatistics) are resolved on the battle screen,
    // tweaked through the gameplay.hud slots and restored with reset().
    {"UI", "hud.stock_controls", WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
     "battle-screen/stock-controls", true, false,
     SafeTest::HudStock, ValidationStatus::HostTested},
    // The row above leaves its tweaks on screen so they can be seen; this
    // one puts everything back. WOTBMOD_VALIDATION_ONLY=hud.stock_controls
    // runs the first without the second for a visual check.
    {"UI", "hud.stock_reset", WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
     "battle-screen/stock-controls", true, false,
     SafeTest::HudReset, ValidationStatus::HostTested},
    // Both inspectors now have a native enumeration backend behind them
    // (tooling_services.cpp, on the reviewed DAVA scene routes), so the rows
    // stop answering for the API and let it answer for itself.
    {"Scene", "scene.readonly_inspector", WOTBMOD_V3_IFACE_DEVTOOLS,
     "devtools/scene-enumerator", true, false,
     SafeTest::SceneInspector, ValidationStatus::LiveTestPending},
    {"Material", "material.readonly_inspector", WOTBMOD_V3_IFACE_DEVTOOLS,
     "devtools/material-enumerator", true, false,
     SafeTest::MaterialInspector, ValidationStatus::LiveTestPending},
    {"Audio", "audio.custom_file", WOTBMOD_V3_IFACE_AUDIO,
     "windows-audio/custom-clip", true, false,
     SafeTest::AudioTone, ValidationStatus::HostTested},
    {"Audio", "audio.native_sound_event", WOTBMOD_V3_IFACE_AUDIO,
     "DAVA::SoundSystem/event", true, false,
     SafeTest::NativeSoundEvent, ValidationStatus::StaticallyVerified},
    {"Resources", "resources.portable_text", WOTBMOD_V3_IFACE_RESOURCES,
     "vfs/resources-text", false, false,
     SafeTest::ResourceText, ValidationStatus::HostTested},
    // This row is "асинхронный ресурс загружен" and its binding is the async
    // worker - it is not about reloading the MOD. It ran
    // SafeTest::ReloadWithAsyncResource, which starts the async load and then
    // asks lifecycle to reload the module; the reload refusal was what got
    // recorded, so a working async loader was reported as NOT_SUPPORTED for a
    // reason that has nothing to do with it. The mod-reload half stays where it
    // belongs, on reload.async_resource.
    {"Resources", "resources.async_reload", WOTBMOD_V3_IFACE_RESOURCES,
     "resources/async-worker", false, false,
     SafeTest::AsyncResourceLoad, ValidationStatus::HostTested},
    {"Resources", "loaders.portable_yaml", WOTBMOD_V3_IFACE_YAML,
     "portable-yaml", false, false,
     SafeTest::PortableYaml, ValidationStatus::Supported},
    {"Resources", "loaders.dvpl", WOTBMOD_V3_IFACE_LOADERS,
     "portable-dvpl", false, false,
     SafeTest::PortableDvpl, ValidationStatus::Supported},
    {"Resources", "archive.zip", WOTBMOD_V3_IFACE_ARCHIVE,
     "portable-zip", false, false,
     SafeTest::PortableArchive, ValidationStatus::Supported},
    // NOT `deliberately_unsupported` any more, and the distinction matters.
    //
    // That flag makes RunSafeTest answer for the row instead of calling the
    // API, so both of these read NOT_SUPPORTED for every build since the flag
    // was set - including builds where the backend was present. The loader
    // publishes CAP_YAML|CAP_RESOURCE_ARCHIVE and InstallDavaNativeBackend
    // succeeds on this client, so `get_backend_info` is the only thing entitled
    // to answer. It reports `available` plus a reason when it is not, which is
    // exactly the honest verdict this row wanted in the first place.
    {"Resources", "loaders.native_dava_yaml", WOTBMOD_V3_IFACE_LOADERS,
     "DAVA::YamlParser", true, false,
     SafeTest::NativeLoaderAvailability, ValidationStatus::LiveTestPending},
    {"Resources", "loaders.native_dava_archive", WOTBMOD_V3_IFACE_LOADERS,
     "DAVA::ResourceArchive", true, false,
     SafeTest::NativeLoaderAvailability, ValidationStatus::LiveTestPending},
    {"Reload", "reload.hangar", WOTBMOD_V3_IFACE_LIFECYCLE,
     "lifecycle/request_reload", false, false,
     SafeTest::DeferredReload, ValidationStatus::HostTested},
    {"Reload", "reload.active_subscription", WOTBMOD_V3_IFACE_LIFECYCLE,
     "lifecycle/subscription-barrier", false, false,
     SafeTest::ReloadWithSubscription, ValidationStatus::HostTested},
    {"Reload", "reload.async_resource", WOTBMOD_V3_IFACE_LIFECYCLE,
     "lifecycle/async-cancel", false, false,
     SafeTest::ReloadWithAsyncResource, ValidationStatus::HostTested},
    {"Reload", "reload.callback_deferred", WOTBMOD_V3_IFACE_ASYNC,
     "async/deferred-main-queue", false, false,
     SafeTest::ReloadFromDeferredCallback, ValidationStatus::HostTested},
    {"Reload", "reload.rollback", WOTBMOD_V3_IFACE_RESOURCES,
     "resources/last-good-rollback", false, false,
     SafeTest::None, ValidationStatus::HostTested},
    {"Reload", "reload.cleanup", WOTBMOD_V3_IFACE_LIFECYCLE,
     "hooks/subscriptions/handles-cleanup", false, false,
     SafeTest::None, ValidationStatus::HostTested},
    {"Errors", "errors.crash_recovery", WOTBMOD_V3_IFACE_DIAGNOSTICS,
     "validation/active-native-test-marker", false, false,
     SafeTest::CrashRecovery, ValidationStatus::HostTested},
    {"Errors", "errors.last_error", WOTBMOD_V3_IFACE_DIAGNOSTICS,
     "bootstrap/get_last_error", false, false,
     SafeTest::LastError, ValidationStatus::HostTested},
    // One row per name the reviewed binding pack publishes through
    // resolve_symbol (loader/v3_native_bindings.cpp, kHookSymbols).  The 42
    // names cover 35 anchors: several functions are published under more than
    // one spelling, and every spelling is probed separately because a mod can
    // only ask for a name, not for an address.  binding_id is the exact symbol
    // string handed to hooks->create_symbol; SafeTest::HookSymbolProbe reads it
    // back, so this table is the only place the list is written.
    {"GES", "ges.observe_all", WOTBMOD_V3_IFACE_GES,
     "GES::GameEventSystem/ListenerList::Add", true, false,
     SafeTest::GesProbe, ValidationStatus::HostTested},
    {"GES", "ges.event_count", WOTBMOD_V3_IFACE_GES,
     "wotbmod.ges.*", true, false,
     SafeTest::GesProbe, ValidationStatus::HostTested},
    {"GES", "ges.publish_echo", WOTBMOD_V3_IFACE_GES,
     "ListenerList dispatch loop", true, false,
     SafeTest::GesProbe, ValidationStatus::HostTested},
    // wotbmod.session.cluster (API 1.1). The change row only acts when
    // WOTBMOD_VALIDATION_CLUSTER_TARGET=<id|auto> is set, so a plain sweep
    // never disconnects the client; its verdict is written by the changed
    // event (CONNECTED -> PASS, FAILED -> FAIL).
    {"Session", "session.cluster.enumerate", WOTBMOD_V3_IFACE_SESSION_CLUSTER,
     "LoginManager::OnHostChosen/Region", true, false,
     SafeTest::SessionCluster, ValidationStatus::HostTested},
    {"Session", "session.cluster.current", WOTBMOD_V3_IFACE_SESSION_CLUSTER,
     "ConnectionManager+12", true, false,
     SafeTest::SessionCluster, ValidationStatus::HostTested},
    {"Session", "session.cluster.refuse_outside_hangar", WOTBMOD_V3_IFACE_SESSION_CLUSTER,
     "change() outside HANGAR -> E_CONFLICT", true, false,
     SafeTest::SessionCluster, ValidationStatus::HostTested},
    {"Session", "session.cluster.change", WOTBMOD_V3_IFACE_SESSION_CLUSTER,
     "LoginManager::ChangeCluster", true, false,
     SafeTest::SessionCluster, ValidationStatus::HostTested},
    {"Hooks", "hooks.vehicle_on_enter_world", WOTBMOD_V3_IFACE_HOOKS,
     "Vehicle::onEnterWorld", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.vehicle_on_leave_world", WOTBMOD_V3_IFACE_HOOKS,
     "Vehicle::onLeaveWorld", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.vehicle_show_shooting", WOTBMOD_V3_IFACE_HOOKS,
     "Vehicle::showShooting", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.vehicle_set_health", WOTBMOD_V3_IFACE_HOOKS,
     "Vehicle::set_health", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.avatar_update_vehicle_health", WOTBMOD_V3_IFACE_HOOKS,
     "Avatar::updateVehicleHealth", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.reload_timer_set_state", WOTBMOD_V3_IFACE_HOOKS,
     "ReloadTimer::setState", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.scene_controller_on_vehicle_hit_damage",
     WOTBMOD_V3_IFACE_HOOKS,
     "GameSceneController::OnVehicleHitDamage", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.shell_selector_on_current_ammo_changed",
     WOTBMOD_V3_IFACE_HOOKS,
     "UIShellSelectorControl::OnCurrentAmmoChanged", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.client_initialize", WOTBMOD_V3_IFACE_HOOKS,
     "Client::Initialize", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.client_leave_to_hangar", WOTBMOD_V3_IFACE_HOOKS,
     "Client::LeaveToHangar", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.camera_set_fov", WOTBMOD_V3_IFACE_HOOKS,
     "Camera::setFOV", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_camera_set_fov_y", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Camera::SetFovY", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.game_camera_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "GameCamera::GameCamera", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.game_camera_ctor_alias", WOTBMOD_V3_IFACE_HOOKS,
     "GameCamera::ctor", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.game_camera_dtor", WOTBMOD_V3_IFACE_HOOKS,
     "GameCamera::~GameCamera", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.game_camera_dtor_alias", WOTBMOD_V3_IFACE_HOOKS,
     "GameCamera::dtor", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.ges_avatar_camera_mode_changed", WOTBMOD_V3_IFACE_HOOKS,
     "GES::Avatar::CameraModeChanged", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.camera_mode_changed_alias", WOTBMOD_V3_IFACE_HOOKS,
     "CameraModeChanged", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.bwentity_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "BWEntity::BWEntity", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.bwentity_ctor_alias", WOTBMOD_V3_IFACE_HOOKS,
     "BWEntity::ctor", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.bwentity_dtor", WOTBMOD_V3_IFACE_HOOKS,
     "BWEntity::~BWEntity", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.bwentity_dtor_alias", WOTBMOD_V3_IFACE_HOOKS,
     "BWEntity::dtor", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.tracer_manager_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "TracerManager::TracerManager", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.tracer_manager_ctor_alias", WOTBMOD_V3_IFACE_HOOKS,
     "TracerManager::ctor", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_scene_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Scene::Scene", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_scene_activate", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Scene::Activate", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_scene_deactivate", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Scene::Deactivate", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_scene_draw", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Scene::Draw", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_entity_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::Entity::Entity", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_entity_cache_load_entity_unsafe",
     WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::EntityCache::LoadEntityUnsafe", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_transform_set_local_transform",
     WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::TransformComponent::SetLocalTransform", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_control_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIControl::UIControl", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_control_system_input", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIControl::SystemInput", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_control_system_get_screen",
     WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIControlSystem::GetScreen", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_extract_control", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIPackage::ExtractControl", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_loader_load_package",
     WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIPackageLoader::LoadPackage", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_loader_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIPackageLoader::UIPackageLoader", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_loader_dtor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::UIPackageLoader::~UIPackageLoader", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_builder_ctor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::DefaultUIPackageBuilder::DefaultUIPackageBuilder", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_ui_package_builder_dtor", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::DefaultUIPackageBuilder::~DefaultUIPackageBuilder", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_engine_context_get_instance", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::EngineContext::GetInstance", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified},
    {"Hooks", "hooks.dava_file_create", WOTBMOD_V3_IFACE_HOOKS,
     "DAVA::File::Create", true, false,
     SafeTest::HookSymbolProbe, ValidationStatus::StaticallyVerified}
};

constexpr const char* kSections[] = {
    "Client", "Lifecycle", "Camera", "UI", "Scene", "Material",
    "Render / D3D11 / DXGI", "Input", "Audio", "BigWorld Entity",
    "RPC Metadata", "Shell", "Projectile", "Tracer", "Vehicle",
    "Resources", "VFS / Overlays", "Async", "Reload / Unload",
    "LeaveToHangar", "Diagnostics", "GES", "Session", "Hooks"
};

constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);
constexpr size_t kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

std::array<CapabilityState, kCapabilityCount> g_states;
std::recursive_mutex g_mutex;
const WotbModV3Bootstrap* g_bootstrap = nullptr;
WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
const WotbModV3CoreApiV1* g_core = nullptr;
const WotbModV3HandlesApiV1* g_handles = nullptr;
const WotbModV3LifecycleApiV1* g_lifecycle = nullptr;
const WotbModV3EventsApiV1* g_events = nullptr;
const WotbModV3AsyncApiV1* g_async = nullptr;
const WotbModV3VfsApiV1* g_vfs = nullptr;
const WotbModV3ResourcesApiV1* g_resources = nullptr;
const WotbModV3YamlApiV1* g_yaml = nullptr;
const WotbModV3ArchiveApiV1* g_archive = nullptr;
const WotbModV3LoadersApiV1* g_loaders = nullptr;
const WotbModV3RenderApiV1* g_render = nullptr;
const WotbModV3RenderNativeApiV1* g_render_native = nullptr;
WotbModV3RenderHandle g_panel_background_texture = WOTBMOD_V3_INVALID_HANDLE;
bool g_panel_background_attempted = false;
const WotbModV3CameraApiV1* g_camera = nullptr;
const WotbModV3AudioApiV2* g_audio = nullptr;
const WotbModV3EntityPublicApiV1* g_entities = nullptr;
const WotbModV3BigWorldRpcApiV1* g_rpc = nullptr;
const WotbModV3ProjectileApiV2* g_projectile = nullptr;
const WotbModV3TracerApiV1* g_tracer = nullptr;
const WotbModV3GameplayHudApiV1* g_hud = nullptr;
const WotbModV3UiApiV3* g_ui = nullptr;
const WotbModV3UiApiV4* g_ui_read = nullptr;
const WotbModV3ClientApiV1* g_client = nullptr;
const WotbModV3DeviceApiV1* g_device = nullptr;
const WotbModV3DiagnosticsApiV2* g_diagnostics = nullptr;
const WotbModV3DevtoolsApiV3* g_devtools = nullptr;
const WotbModV3InputApiV1* g_input = nullptr;
const WotbModV3HooksApiV1* g_hooks = nullptr;
const WotbModV3GesApiV1* g_ges = nullptr;
const WotbModV3SessionClusterApiV1* g_session_cluster = nullptr;
/* session.cluster.change in flight: unix ms of the change() call, 0 = none. */
std::atomic<uint64_t> g_session_change_started_ms{0u};
std::atomic<uint32_t> g_ges_events{0u};
std::atomic<uint32_t> g_ges_echo_seen{0u};
std::atomic<int32_t> g_ges_last_camera_mode{0};
std::set<std::string> g_ges_types_seen;   // guarded by g_mutex, capped at 1024

std::vector<WotbModV3EventToken> g_system_event_tokens;
WotbModV3EventToken g_self_event_token = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Token g_render_token = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Handle g_panel_action = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Token g_panel_action_token = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3ResourceHandle g_async_resource = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Handle g_probe_mount = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Handle g_audio_mount = WOTBMOD_V3_INVALID_HANDLE;
std::string g_data_directory;
std::string g_trace_path;
std::string g_ui_tree_path;
std::string g_vehicle_state_path;
std::string g_results_path;
std::string g_matrix_path;
std::string g_failures_path;
std::string g_marker_path;
std::string g_comment_path;
std::string g_live_log_path;
std::string g_settings_path;
std::string g_audio_directory;
std::string g_report_path;
std::string g_fingerprint_key;
std::string g_client_version = "unknown";
std::string g_client_sha = "unknown";
std::string g_client_state = "UNKNOWN";
uint32_t g_binding_pack_version = 0u;
uint64_t g_load_cycle_count = 0u;
/*
 * Hot-reload proof. g_load_cycle_count is a persisted boot generation (bumped on
 * every OnEnable). When a reload test fires request_reload it stamps the current
 * generation into reload_pending.txt; the freshly reloaded instance sees a
 * higher generation on boot and records a real PASS. This is how the reload rows
 * prove an actual unload+reload happened rather than just "request_reload
 * returned OK".
 */
uint64_t g_reload_pending_from = 0u;
std::string g_reload_pending_row;
std::string g_reload_pending_path;
size_t g_selected_section = 0u;
size_t g_selected_in_section = 0u;
size_t g_panel_focus = 1u;
uint32_t g_log_filter = 0u;
bool g_log_paused = false;
bool g_log_autoscroll = true;
bool g_log_selected_only = false;
bool g_battle_mute = false;
bool g_detail_view = false;
bool g_unsafe_confirmed = false;
int32_t g_panel_x = 16;
int32_t g_panel_y = 16;
int32_t g_panel_width = 1888;
int32_t g_panel_height = 1048;
SoundNotificationMode g_sound_mode = SoundNotificationMode::PassAndFail;
float g_sound_volume = 0.18f;
std::array<uint8_t, kCapabilityCount> g_test_enabled = {};
std::deque<LiveLogEntry> g_live_log;
std::mutex g_live_log_mutex;
std::map<std::string, ShotChain> g_shot_chains;
std::mutex g_shot_chains_mutex;
std::mutex g_log_queue_mutex;
std::condition_variable g_log_queue_cv;
std::deque<QueuedLogLine> g_log_queue;
std::thread g_log_worker;
std::atomic<bool> g_log_worker_running(false);
std::atomic<bool> g_log_worker_stop(false);
std::atomic<uint32_t> g_pending_pass_sound(0u);
std::atomic<uint32_t> g_pending_warn_sound(0u);
std::atomic<uint32_t> g_pending_fail_sound(0u);
std::atomic<uint64_t> g_last_sound_ms(0u);
std::atomic<uint64_t> g_last_live_event_ms(0u);
std::atomic<uint32_t> g_panel_visible(0u);
std::atomic<uint32_t> g_snapshots_dirty(0u);
// Non-zero only while a batch of tests runs back to back on one thread.  Each
// SaveSnapshots rewrites the whole matrix and results files, so a 42-symbol
// hook sweep would otherwise stall the render thread on ~126 file writes; the
// batch marks the snapshot dirty instead and writes once when it finishes.
std::atomic<uint32_t> g_snapshot_batch(0u);
std::atomic<uint32_t> g_panel_draw_diagnostic(0u);
std::atomic<int32_t> g_export_button_left(0);
std::atomic<int32_t> g_export_button_top(0);
std::atomic<int32_t> g_export_button_right(0);
std::atomic<int32_t> g_export_button_bottom(0);
std::atomic<uint64_t> g_render_calls(0u);
std::atomic<uint32_t> g_render_thread(0u);
std::atomic<uint32_t> g_render_backend(WOTBMOD_V3_RENDER_BACKEND_NONE);
std::atomic<uint32_t> g_render_width(0u);
std::atomic<uint32_t> g_render_height(0u);
std::atomic<uint64_t> g_render_resize_count(0u);
std::atomic<uint64_t> g_swapchain_recreation_count(0u);
std::atomic<uint64_t> g_device_lost_count(0u);
std::atomic<uint64_t> g_device_restored_count(0u);
std::atomic<uint32_t> g_native_device_present(0u);
std::atomic<uint32_t> g_native_context_present(0u);
std::atomic<uint32_t> g_native_swapchain_present(0u);
std::atomic<uint64_t> g_callback_count(0u);
std::atomic<uint64_t> g_callback_total_100ns(0u);
std::atomic<uint64_t> g_callback_max_100ns(0u);
std::atomic<uint64_t> g_trace_count(0u);
std::atomic<uint64_t> g_trace_bytes(0u);
std::atomic<uint64_t> g_snapshot_writes(0u);
std::atomic<uint64_t> g_draw_calls(0u);
double g_callbacks_per_second = 0.0;
double g_trace_per_second = 0.0;
double g_bytes_per_second = 0.0;
double g_snapshot_writes_per_minute = 0.0;
double g_draw_calls_per_frame = 0.0;
uint64_t g_perf_sample_ms = 0u;
uint64_t g_perf_sample_callbacks = 0u;
uint64_t g_perf_sample_trace = 0u;
uint64_t g_perf_sample_bytes = 0u;
uint64_t g_perf_sample_snapshots = 0u;
uint64_t g_camera_birth_ms = 0u;
uint64_t g_camera_last_handle = WOTBMOD_V3_INVALID_HANDLE;
uint32_t g_camera_previous_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
uint32_t g_camera_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
uint64_t g_camera_event_count = 0u;
uint64_t g_camera_duplicate_count = 0u;
std::vector<EntityObservation> g_entity_observations;

template <typename T>
const T* QueryApi(const char* name, uint32_t version) {
    const void* table = nullptr;
    if (!g_bootstrap || !g_bootstrap->query_interface ||
        g_bootstrap->query_interface(
            g_mod, name, version, &table) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return static_cast<const T*>(table);
}

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

const char* StatusName(ValidationStatus status) {
    switch (status) {
        case ValidationStatus::Discovered: return "DISCOVERED";
        case ValidationStatus::StaticallyVerified: return "STATICALLY_VERIFIED";
        case ValidationStatus::Bound: return "BOUND";
        case ValidationStatus::HostTested: return "HOST_TESTED";
        case ValidationStatus::LiveTestPending: return "LIVE_TEST_PENDING";
        case ValidationStatus::LiveTested: return "LIVE_TESTED";
        case ValidationStatus::StressTested: return "STRESS_TESTED";
        case ValidationStatus::Supported: return "SUPPORTED";
        case ValidationStatus::Failed: return "FAILED";
        case ValidationStatus::NotSupported: return "NOT_SUPPORTED";
        default: return "UNKNOWN";
    }
}

const char* CameraModeName(uint32_t mode) {
    switch (mode) {
        case WOTBMOD_V3_CAMERA_MODE_HANGAR: return "HANGAR";
        case WOTBMOD_V3_CAMERA_MODE_ARCADE: return "ARCADE";
        case WOTBMOD_V3_CAMERA_MODE_SNIPER: return "SNIPER";
        case WOTBMOD_V3_CAMERA_MODE_POSTMORTEM: return "POSTMORTEM";
        case WOTBMOD_V3_CAMERA_MODE_REPLAY: return "REPLAY";
        case WOTBMOD_V3_CAMERA_MODE_FREE: return "FREE";
        case WOTBMOD_V3_CAMERA_MODE_CINEMATIC: return "CINEMATIC";
        case WOTBMOD_V3_CAMERA_MODE_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char* VerdictName(ManualVerdict verdict) {
    switch (verdict) {
        case ManualVerdict::Pass: return "PASS";
        case ManualVerdict::Fail: return "FAIL";
        case ManualVerdict::Skip: return "SKIP";
        default: return "NONE";
    }
}

// A resource state printed as a bare number reads like progress. "state=3" is
// WOTBMOD_V3_RESOURCE_FAILED, and it sat in a verdict line for a long time
// looking like the loader had answered.
const char* ResourceStateName(uint32_t state) {
    switch (state) {
        case WOTBMOD_V3_RESOURCE_LOADING: return "LOADING";
        case WOTBMOD_V3_RESOURCE_READY: return "READY";
        case WOTBMOD_V3_RESOURCE_FAILED: return "FAILED";
        case WOTBMOD_V3_RESOURCE_CANCELLED: return "CANCELLED";
        default: return "UNKNOWN";
    }
}

const char* ResultName(WotbModV3Result result) {
    switch (result) {
        case WOTBMOD_V3_OK: return "WOTBMOD_V3_OK";
        case WOTBMOD_V3_E_INVALID_ARGUMENT: return "WOTBMOD_V3_E_INVALID_ARGUMENT";
        case WOTBMOD_V3_E_INVALID_HANDLE: return "WOTBMOD_V3_E_INVALID_HANDLE";
        case WOTBMOD_V3_E_NOT_SUPPORTED: return "WOTBMOD_V3_E_NOT_SUPPORTED";
        case WOTBMOD_V3_E_NOT_FOUND: return "WOTBMOD_V3_E_NOT_FOUND";
        case WOTBMOD_V3_E_ALREADY_EXISTS: return "WOTBMOD_V3_E_ALREADY_EXISTS";
        case WOTBMOD_V3_E_WRONG_THREAD: return "WOTBMOD_V3_E_WRONG_THREAD";
        case WOTBMOD_V3_E_PERMISSION_DENIED: return "WOTBMOD_V3_E_PERMISSION_DENIED";
        case WOTBMOD_V3_E_CLIENT_MISMATCH: return "WOTBMOD_V3_E_CLIENT_MISMATCH";
        case WOTBMOD_V3_E_OBJECT_DESTROYED: return "WOTBMOD_V3_E_OBJECT_DESTROYED";
        case WOTBMOD_V3_E_CONFLICT: return "WOTBMOD_V3_E_CONFLICT";
        case WOTBMOD_V3_E_CANCELLED: return "WOTBMOD_V3_E_CANCELLED";
        case WOTBMOD_V3_E_BUFFER_TOO_SMALL: return "WOTBMOD_V3_E_BUFFER_TOO_SMALL";
        case WOTBMOD_V3_E_LIMIT_REACHED: return "WOTBMOD_V3_E_LIMIT_REACHED";
        case WOTBMOD_V3_E_BUSY: return "WOTBMOD_V3_E_BUSY";
        case WOTBMOD_V3_E_IO: return "WOTBMOD_V3_E_IO";
        case WOTBMOD_V3_E_PARSE: return "WOTBMOD_V3_E_PARSE";
        case WOTBMOD_V3_E_HASH_MISMATCH: return "WOTBMOD_V3_E_HASH_MISMATCH";
        case WOTBMOD_V3_E_SIGNATURE_INVALID: return "WOTBMOD_V3_E_SIGNATURE_INVALID";
        case WOTBMOD_V3_E_DEPENDENCY_MISSING: return "WOTBMOD_V3_E_DEPENDENCY_MISSING";
        case WOTBMOD_V3_E_INCOMPATIBLE: return "WOTBMOD_V3_E_INCOMPATIBLE";
        case WOTBMOD_V3_E_CALLBACK_FAULT: return "WOTBMOD_V3_E_CALLBACK_FAULT";
        case WOTBMOD_V3_E_PLATFORM: return "WOTBMOD_V3_E_PLATFORM";
        case WOTBMOD_V3_E_TIMEOUT: return "WOTBMOD_V3_E_TIMEOUT";
        default: return "WOTBMOD_V3_UNKNOWN_RESULT";
    }
}

uint64_t UnixMilliseconds() {
    FILETIME file_time = {};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value = {};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    constexpr uint64_t kWindowsToUnix100ns = 116444736000000000ull;
    if (value.QuadPart < kWindowsToUnix100ns) return 0u;
    return (value.QuadPart - kWindowsToUnix100ns) / 10000ull;
}

uint64_t Qpc100ns() {
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER counter = {};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return 0u;
    }
    return static_cast<uint64_t>(
        (counter.QuadPart * 10000000ll) / frequency.QuadPart);
}

struct CallbackMetricScope {
    uint64_t started = Qpc100ns();
    ~CallbackMetricScope() {
        const uint64_t ended = Qpc100ns();
        const uint64_t elapsed = ended >= started ? ended - started : 0u;
        g_callback_count.fetch_add(1u);
        g_callback_total_100ns.fetch_add(elapsed);
        uint64_t maximum = g_callback_max_100ns.load();
        while (maximum < elapsed &&
               !g_callback_max_100ns.compare_exchange_weak(
                   maximum, elapsed)) {
        }
    }
};

std::string TimestampUtc() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);
    char buffer[48] = {};
    sprintf_s(
        buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds));
    return buffer;
}

std::string JsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 16u);
    for (unsigned char character : value) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20u) {
                    char escaped[8] = {};
                    sprintf_s(escaped, "\\u%04x", character);
                    result += escaped;
                } else {
                    result.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return result;
}

std::string ToLower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string SanitizeComment(std::string value) {
    if (value.size() > 160u) value.resize(160u);
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || character == '\r' || character == '\n') {
            character = ' ';
        }
    }
    const std::string lower = ToLower(value);
    if (lower.find("token") != std::string::npos ||
        lower.find("password") != std::string::npos ||
        lower.find("authorization") != std::string::npos ||
        lower.find("bearer ") != std::string::npos ||
        value.find('@') != std::string::npos) {
        return "[REDACTED_BY_PRIVACY_POLICY]";
    }
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

bool JoinPath(
    const std::string& directory,
    const char* leaf,
    std::string* output) {
    if (!output || directory.empty() || !leaf || !leaf[0]) return false;
    *output = directory;
    if (output->back() != '\\' && output->back() != '/') {
        output->push_back('\\');
    }
    output->append(leaf);
    return output->size() < WOTBMOD_V3_MAX_PATH * 4u;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

const char* LiveSeverityName(LiveSeverity severity) {
    switch (severity) {
        case LiveSeverity::Pass: return "PASS";
        case LiveSeverity::Warn: return "WARN";
        case LiveSeverity::Fail: return "FAIL";
        default: return "INFO";
    }
}

std::string TimestampReadable() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char buffer[48] = {};
    sprintf_s(
        buffer, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds));
    return buffer;
}

const char* CapabilityPanelCategory(size_t index) {
    if (index >= kCapabilityCount) return "Diagnostics";
    const char* id = kCapabilities[index].id;
    const char* section = kCapabilities[index].section;
    if (std::strncmp(id, "projectile.", 11u) == 0 ||
        std::strcmp(id, "impact.confirmed") == 0) return "Projectile";
    if (std::strncmp(id, "tracer.", 7u) == 0) return "Tracer";
    if (std::strcmp(id, "shell.ingress") == 0) return "Shell";
    if (std::strcmp(id, "client.leave_to_hangar") == 0) return "LeaveToHangar";
    if (std::strcmp(section, "Reload") == 0 ||
        std::strcmp(id, "lifecycle.cleanup") == 0) return "Reload / Unload";
    if (std::strcmp(section, "Errors") == 0 ||
        std::strcmp(section, "Events") == 0) return "Diagnostics";
    if (std::strcmp(id, "reload.callback_deferred") == 0 ||
        std::strcmp(id, "resources.async_reload") == 0) return "Async";
    return section;
}

const char* CapabilityHumanName(const char* id) {
    if (!id) return "Неизвестный тест";
    // A hook probe is reviewed by the symbol it names, so the symbol itself is
    // the readable name; a hand-written translation would only hide it.
    if (std::strncmp(id, "hooks.", 6u) == 0) {
        for (const CapabilitySpec& spec : kCapabilities) {
            if (std::strcmp(spec.id, id) == 0) return spec.binding_id;
        }
    }
    struct Name { const char* id; const char* text; };
    static constexpr Name names[] = {
        {"client.fingerprint", "Проверка fingerprint клиента"},
        {"client.device_info", "Информация об устройстве"},
        {"client.leave_to_hangar", "Игрок вернулся в ангар"},
        {"lifecycle.info", "Состояние lifecycle runtime"},
        {"lifecycle.cleanup", "Очистка подписок и handle"},
        {"lifecycle.stress_30", "30 циклов загрузки и выгрузки"},
        {"camera.active", "Активная камера создана"},
        {"camera.mode_change", "Изменён режим камеры"},
        {"events.loader_queue", "Событие доставлено через очередь"},
        {"events.callback_thread", "Проверен поток callback"},
        {"entity.enumerate_visible", "Получены публичные entity"},
        {"vehicle.state", "Снят снимок состояния техники видимых машин в бою"},
        {"entity.lifecycle", "Создана или уничтожена публичная entity"},
        {"rpc.incoming_metadata", "Получены входящие RPC metadata"},
        {"shell.ingress", "Игрок произвёл выстрел"},
        {"projectile.created", "Создан projectile"},
        {"projectile.updated", "Projectile перешёл в IN_FLIGHT"},
        {"projectile.destroyed", "Projectile уничтожен"},
        {"impact.visual", "Создан визуальный эффект попадания"},
        {"impact.confirmed", "Зарегистрировано попадание"},
        {"tracer.requested", "Запрошен tracer"},
        {"tracer.created", "Создан tracer"},
        {"tracer.visible", "Tracer отображается"},
        {"tracer.destroyed", "Tracer уничтожен"},
        {"tracer.style", "Применён стиль tracer"},
        {"ges.observe_all", "Подписка на всю шину GES"},
        {"ges.event_count", "Получены события GES"},
        {"ges.publish_echo", "Публикация в GES с echo"},
        {"session.cluster.enumerate", "Список кластеров региона"},
        {"session.cluster.current", "Текущий кластер"},
        {"session.cluster.refuse_outside_hangar", "Отказ смены кластера вне ангара"},
        {"session.cluster.change", "Смена кластера из ангара"},
        {"render.backend", "Определён render backend"},
        {"render.native_borrowed", "D3D11 device/context доступны"},
        {"render.resize", "Изменился размер swapchain"},
        {"render.device_lifecycle", "D3D11 device lifecycle изменился"},
        {"render.callback", "Render callback выполнен"},
        {"ui.validation_panel", "Панель Live API Test Center отрисована"},
        {"ui.readonly_inspector", "UI-экран проинспектирован"},
        {"ui.live_text", "Прочитан живой текст штатного UI"},
        {"hud.stock_controls", "Штатные контролы HUD найдены и изменены (все слоты)"},
        {"hud.stock_reset", "Штатные контролы HUD восстановлены через reset"},
        {"scene.readonly_inspector", "Scene проинспектирована"},
        {"material.readonly_inspector", "Material проинспектирован"},
        {"audio.custom_file", "Воспроизведён custom audio"},
        {"audio.native_sound_event", "Воспроизведено native sound event"},
        {"resources.portable_text", "Ресурс загружен"},
        {"resources.async_reload", "Асинхронный ресурс загружен"},
        {"loaders.portable_yaml", "Portable YAML обработан"},
        {"loaders.dvpl", "DVPL loader доступен"},
        {"archive.zip", "Архив открыт"},
        {"loaders.native_dava_yaml", "Native DAVA YAML проверен"},
        {"loaders.native_dava_archive", "Native DAVA archive проверен"},
        {"reload.hangar", "Перезагрузка в ангаре запрошена"},
        {"reload.active_subscription", "Активная подписка обработана"},
        {"reload.async_resource", "Async загрузка отменена"},
        {"reload.callback_deferred", "Reload отложен через main queue"},
        {"reload.rollback", "Resource transaction откатилась"},
        {"reload.cleanup", "Мод успешно выгружен"},
        {"errors.crash_recovery", "Crash recovery marker проверен"},
        {"errors.last_error", "Получена последняя ошибка API"}
    };
    for (const Name& name : names) {
        if (std::strcmp(name.id, id) == 0) return name.text;
    }
    return id;
}

const char* CapabilityInstruction(const char* id) {
    if (!id) return "Выполните действие в клиенте";
    if (std::strncmp(id, "hooks.", 6u) == 0) {
        return "F6 проба символа, H проба всех символов";
    }
    if (std::strcmp(id, "shell.ingress") == 0) return "произведите выстрел в бою";
    if (std::strncmp(id, "projectile.", 11u) == 0 ||
        std::strncmp(id, "tracer.", 7u) == 0 ||
        std::strcmp(id, "impact.confirmed") == 0) return "стреляйте в тренировочном бою";
    if (std::strcmp(id, "camera.mode_change") == 0) return "переключите ARCADE и SNIPER";
    if (std::strcmp(id, "entity.lifecycle") == 0) return "создайте или уничтожьте entity в бою";
    if (std::strcmp(id, "render.resize") == 0) return "измените окно или нажмите Alt+Tab";
    if (std::strcmp(id, "client.leave_to_hangar") == 0) return "подтвердите возврат в ангар";
    if (std::strcmp(id, "reload.hangar") == 0 ||
        std::strncmp(id, "reload.", 7u) == 0) return "запустите тест клавишей F6";
    if (std::strncmp(id, "audio.", 6u) == 0) return "запустите активный audio-тест";
    return "выполните действие из сценария Live Test Center";
}

bool CapabilityHighFrequency(size_t index) {
    if (index >= kCapabilityCount) return false;
    const char* id = kCapabilities[index].id;
    return std::strcmp(id, "projectile.updated") == 0 ||
        std::strcmp(id, "entity.lifecycle") == 0 ||
        std::strcmp(id, "events.callback_thread") == 0;
}

bool IsTestEnabled(size_t index) {
    if (index >= kCapabilityCount) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_test_enabled[index] != 0u;
}

void QueueLogLine(const std::string& path, const std::string& line);
void SaveLiveSettings();
bool WriteWaveTone(
    const std::string& path,
    uint32_t frequency = 440u,
    int16_t amplitude = 1800,
    uint32_t duration_ms = 250u);

bool ReadFileBounded(
    const std::string& path,
    uint32_t maximum,
    std::string* output) {
    if (!output) return false;
    output->clear();
    HANDLE file = CreateFileA(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > maximum) {
        CloseHandle(file);
        return false;
    }
    output->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0u;
    const bool ok = output->empty() ||
        (ReadFile(
             file, &(*output)[0], static_cast<DWORD>(output->size()),
             &read, nullptr) != FALSE && read == output->size());
    CloseHandle(file);
    if (!ok) output->clear();
    return ok;
}

bool WriteFileExact(
    const std::string& path,
    const void* data,
    size_t size,
    bool append) {
    HANDLE file = CreateFileA(
        path.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, append ? OPEN_ALWAYS : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0u;
    bool ok = true;
    while (offset < size) {
        const DWORD portion = static_cast<DWORD>(std::min<size_t>(
            size - offset, static_cast<size_t>(0x7fffffffu)));
        DWORD written = 0u;
        if (!WriteFile(file, bytes + offset, portion, &written, nullptr) ||
            written != portion) {
            ok = false;
            break;
        }
        offset += written;
    }
    // Append-only diagnostics may remain in the OS cache. A durable flush for
    // every observed event stalls the game thread and can cause visible FPS loss.
    if (!append) FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

void LogWorkerMain() {
    for (;;) {
        std::deque<QueuedLogLine> batch;
        {
            std::unique_lock<std::mutex> lock(g_log_queue_mutex);
            g_log_queue_cv.wait_for(
                lock, std::chrono::milliseconds(1000), [] {
                    return g_log_worker_stop.load() || !g_log_queue.empty();
                });
            if (g_log_worker_stop.load() && g_log_queue.empty()) break;
            batch.swap(g_log_queue);
        }
        for (const QueuedLogLine& item : batch) {
            if (!item.path.empty() && !item.line.empty()) {
                WriteFileExact(item.path, item.line.data(), item.line.size(), true);
            }
        }
    }
}

void StartLogWorker() {
    if (g_log_worker_running.exchange(true)) return;
    g_log_worker_stop.store(false);
    g_log_worker = std::thread(&LogWorkerMain);
}

void StopLogWorker() {
    if (!g_log_worker_running.exchange(false)) return;
    g_log_worker_stop.store(true);
    g_log_queue_cv.notify_all();
    if (g_log_worker.joinable()) g_log_worker.join();
}

void QueueLogLine(const std::string& path, const std::string& line) {
    if (path.empty() || line.empty()) return;
    if (!g_log_worker_running.load()) {
        WriteFileExact(path, line.data(), line.size(), true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_log_queue_mutex);
        constexpr size_t kMaximumQueuedLines = 4096u;
        if (g_log_queue.size() >= kMaximumQueuedLines) {
            g_log_queue.pop_front();
        }
        g_log_queue.push_back({path, line});
    }
    g_log_queue_cv.notify_one();
}

bool SoundModeAllows(LiveSeverity severity, bool selected) {
    if (g_sound_mode == SoundNotificationMode::Off || g_battle_mute) return false;
    if (severity == LiveSeverity::Fail) return true;
    if (severity == LiveSeverity::Pass &&
        (g_sound_mode == SoundNotificationMode::PassAndFail ||
         (g_sound_mode == SoundNotificationMode::SelectedEvents && selected))) {
        return true;
    }
    return g_sound_mode == SoundNotificationMode::SelectedEvents && selected;
}

void QueueFeedback(LiveSeverity severity, bool selected) {
    if (!SoundModeAllows(severity, selected)) return;
    if (severity == LiveSeverity::Fail) g_pending_fail_sound.fetch_add(1u);
    else if (severity == LiveSeverity::Warn) g_pending_warn_sound.fetch_add(1u);
    else if (severity == LiveSeverity::Pass) g_pending_pass_sound.fetch_add(1u);
}

void QueueLiveLog(
    size_t capability,
    LiveSeverity severity,
    const char* event_name,
    const std::string& payload,
    bool allow_sound = true) {
    if (capability >= kCapabilityCount) return;
    const bool selected = IsTestEnabled(capability);
    if (!selected) return;
    const uint64_t now = UnixMilliseconds();
    if (CapabilityHighFrequency(capability) &&
        now < g_last_live_event_ms.load() + 120u) {
        return;
    }
    g_last_live_event_ms.store(now);
    LiveLogEntry entry;
    entry.timestamp_ms = now;
    entry.capability = capability;
    entry.severity = severity;
    entry.category = CapabilityPanelCategory(capability);
    entry.title = CapabilityHumanName(kCapabilities[capability].id);
    entry.api = kCapabilities[capability].id;
    entry.event_name = event_name ? event_name : "";
    entry.payload = SanitizeComment(payload);
    entry.thread_id = GetCurrentThreadId();
    {
        std::lock_guard<std::mutex> lock(g_live_log_mutex);
        constexpr size_t kLiveLogCapacity = 128u;
        if (g_live_log.size() >= kLiveLogCapacity) g_live_log.pop_front();
        g_live_log.push_back(entry);
    }
    std::string line;
    line += "[" + TimestampReadable() + "]\r\n[";
    line += LiveSeverityName(severity);
    line += "]\r\n[" + entry.category + "]\r\n" + entry.title +
        "\r\nAPI: " + entry.api + "\r\nevent: " + entry.event_name +
        "\r\nthread_id: " + std::to_string(entry.thread_id);
    if (!entry.payload.empty()) line += "\r\npayload: " + entry.payload;
    line += "\r\n\r\n";
    QueueLogLine(g_live_log_path, line);
    if (allow_sound) QueueFeedback(severity, selected);
}

bool WriteTextAtomic(const std::string& path, const std::string& text) {
    const std::string temporary = path + ".tmp";
    if (!WriteFileExact(temporary, text.data(), text.size(), false)) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    if (!MoveFileExA(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    return true;
}

void Log(uint32_t level, const std::string& message) {
    if (g_core && g_core->log) {
        g_core->log(
            g_mod, level, "native-validation", message.c_str());
    }
}

size_t FindCapability(const char* id) {
    if (!id) return kCapabilityCount;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        if (std::strcmp(kCapabilities[index].id, id) == 0) return index;
    }
    return kCapabilityCount;
}

std::vector<size_t> SectionCapabilities(size_t section) {
    std::vector<size_t> result;
    if (section >= kSectionCount) return result;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const char* category = kCapabilities[index].section;
        const char* id = kCapabilities[index].id;
        if (std::strcmp(kSections[section], "Projectile") == 0 &&
            (std::strstr(id, "projectile.") == id ||
             std::strcmp(id, "impact.confirmed") == 0)) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Tracer") == 0 &&
                   std::strncmp(id, "tracer.", 7u) == 0) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Shell") == 0 &&
                   std::strcmp(id, "shell.ingress") == 0) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "LeaveToHangar") == 0 &&
                   std::strcmp(id, "client.leave_to_hangar") == 0) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Reload / Unload") == 0 &&
                   (std::strcmp(category, "Reload") == 0 ||
                    std::strcmp(id, "lifecycle.cleanup") == 0)) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Diagnostics") == 0 &&
                   (std::strcmp(category, "Errors") == 0 ||
                    std::strcmp(category, "Events") == 0)) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Async") == 0 &&
                   (std::strcmp(id, "reload.callback_deferred") == 0 ||
                    std::strcmp(id, "resources.async_reload") == 0)) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Render / D3D11 / DXGI") == 0 &&
                   std::strcmp(category, "Render / D3D11 / DXGI") == 0) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], "Resources") == 0 &&
                   (std::strcmp(category, "Resources") == 0 ||
                    std::strcmp(id, "resources.portable_text") == 0)) {
            result.push_back(index);
        } else if (std::strcmp(kSections[section], category) == 0) {
            result.push_back(index);
        }
    }
    return result;
}

size_t SelectedCapability() {
    const std::vector<size_t> indices = SectionCapabilities(g_selected_section);
    if (indices.empty()) return 0u;
    if (g_selected_in_section >= indices.size()) g_selected_in_section = 0u;
    return indices[g_selected_in_section];
}

void RefreshClientState() {
    if (!g_core || !g_core->get_context) return;
    uint64_t context = 0u;
    if (g_core->get_context(g_mod, &context) != WOTBMOD_V3_OK) return;
    if ((context & WOTBMOD_V3_CONTEXT_BATTLE) != 0u) g_client_state = "BATTLE";
    else if ((context & WOTBMOD_V3_CONTEXT_REPLAY) != 0u) g_client_state = "REPLAY";
    else if ((context & WOTBMOD_V3_CONTEXT_TRAINING) != 0u) g_client_state = "TRAINING";
    else if ((context & WOTBMOD_V3_CONTEXT_RESULTS) != 0u) g_client_state = "RESULTS";
    else if ((context & WOTBMOD_V3_CONTEXT_HANGAR) != 0u) g_client_state = "HANGAR";
    else if ((context & WOTBMOD_V3_CONTEXT_LOADING) != 0u) g_client_state = "LOADING";
    else g_client_state = "UNKNOWN";
}

uint32_t HandleGeneration(uint64_t handle) {
    if (!g_handles || !g_handles->get_info ||
        handle == WOTBMOD_V3_INVALID_HANDLE) return 0u;
    WotbModV3HandleInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    return g_handles->get_info(g_mod, handle, &info) == WOTBMOD_V3_OK
        ? info.generation : 0u;
}

std::string HandleText(uint64_t handle) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE) return "";
    char value[32] = {};
    sprintf_s(value, "h:%016llx", static_cast<unsigned long long>(handle));
    return value;
}

void ObserveEntity(
    const WotbModV3PublicEntitySnapshot& snapshot,
    bool removed,
    bool preexisting) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint64_t now = UnixMilliseconds();
    EntityObservation* observation = nullptr;
    for (EntityObservation& candidate : g_entity_observations) {
        if (candidate.public_id == snapshot.public_id &&
            candidate.handle == snapshot.handle) {
            observation = &candidate;
            break;
        }
    }
    if (!observation) {
        for (EntityObservation& candidate : g_entity_observations) {
            if (candidate.public_id != snapshot.public_id ||
                !candidate.alive) continue;
            uint32_t alive = 1u;
            candidate.old_handle_invalidated =
                g_handles && g_handles->is_alive &&
                        g_handles->is_alive(
                            g_mod, candidate.handle, &alive) == WOTBMOD_V3_OK
                    ? (alive == 0u ? 1 : 0)
                    : -1;
            candidate.alive = false;
            candidate.destroyed_unix_ms = now;
        }
        if (g_entity_observations.size() >= 128u) {
            g_entity_observations.erase(g_entity_observations.begin());
        }
        EntityObservation created;
        created.public_id = snapshot.public_id;
        created.type = snapshot.type;
        created.handle = snapshot.handle;
        created.generation = HandleGeneration(snapshot.handle);
        created.alive = !removed;
        created.preexisting = preexisting;
        created.created_unix_ms = preexisting ? 0u : now;
        created.destroyed_unix_ms = removed ? now : 0u;
        g_entity_observations.push_back(created);
        observation = &g_entity_observations.back();
    } else {
        observation->type = snapshot.type;
        observation->generation = HandleGeneration(snapshot.handle);
        observation->preexisting = observation->preexisting || preexisting;
        observation->alive = !removed;
        if (removed) {
            observation->destroyed_unix_ms = now;
            uint32_t alive = 1u;
            observation->old_handle_invalidated =
                g_handles && g_handles->is_alive &&
                        g_handles->is_alive(
                            g_mod, observation->handle, &alive) == WOTBMOD_V3_OK
                    ? (alive == 0u ? 1 : 0)
                    : -1;
        }
    }
}

uint32_t ActiveEntityCount() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t count = 0u;
    for (const EntityObservation& entity : g_entity_observations) {
        if (entity.alive) ++count;
    }
    return count;
}

void RefreshEntityInvalidations() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_handles || !g_handles->is_alive) return;
    for (EntityObservation& entity : g_entity_observations) {
        if (entity.alive || entity.old_handle_invalidated == 1 ||
            entity.handle == WOTBMOD_V3_INVALID_HANDLE) continue;
        uint32_t alive = 1u;
        if (g_handles->is_alive(g_mod, entity.handle, &alive) == WOTBMOD_V3_OK) {
            entity.old_handle_invalidated = alive == 0u ? 1 : 0;
        }
    }
}

void RecordResult(
    size_t index,
    WotbModV3Result code,
    const std::string& result,
    uint64_t handle = WOTBMOD_V3_INVALID_HANDLE,
    uint32_t active_handles = 0u) {
    if (index >= kCapabilityCount) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    CapabilityState& state = g_states[index];
    ++state.call_count;
    state.last_event_unix_ms = UnixMilliseconds();
    if (state.first_seen_unix_ms == 0u) {
        state.first_seen_unix_ms = state.last_event_unix_ms;
    }
    state.callback_thread = GetCurrentThreadId();
    state.last_code = code;
    state.last_result = result;
    state.handle = handle;
    state.generation = HandleGeneration(handle);
    state.active_handles = active_handles;
    state.last_error = code == WOTBMOD_V3_OK ? "" : ResultName(code);
    state.warning = false;
    if (code == WOTBMOD_V3_E_NOT_SUPPORTED ||
        code == WOTBMOD_V3_E_PLATFORM) {
        if (state.verdict == ManualVerdict::None) {
            state.status = ValidationStatus::NotSupported;
        }
    } else if (code == WOTBMOD_V3_E_CLIENT_MISMATCH) {
        state.status = ValidationStatus::Failed;
        state.warning = true;
    } else if (code == WOTBMOD_V3_OK && kCapabilities[index].native &&
               state.verdict == ManualVerdict::None) {
        state.status = ValidationStatus::LiveTestPending;
    }
}

std::string LastErrorMessage() {
    if (!g_bootstrap || !g_bootstrap->get_last_error) return "";
    WotbModV3ErrorInfo error = {};
    WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
    if (g_bootstrap->get_last_error(g_mod, &error) != WOTBMOD_V3_OK) {
        return "";
    }
    return error.message;
}

std::string JsonStringValue(const std::string& text, const char* key) {
    const std::string marker = std::string("\"") + key + "\": \"";
    const size_t begin = text.find(marker);
    if (begin == std::string::npos) return "";
    size_t cursor = begin + marker.size();
    std::string result;
    bool escaped = false;
    for (; cursor < text.size(); ++cursor) {
        const char character = text[cursor];
        if (escaped) {
            switch (character) {
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: result.push_back(character); break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            break;
        } else {
            result.push_back(character);
        }
    }
    return result;
}

uint64_t JsonUnsignedValue(
    const std::string& text,
    const char* key,
    uint64_t fallback) {
    const std::string marker = std::string("\"") + key + "\": ";
    const size_t begin = text.find(marker);
    if (begin == std::string::npos) return fallback;
    const char* value = text.c_str() + begin + marker.size();
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end == value ? fallback : static_cast<uint64_t>(parsed);
}

ValidationStatus ParseStatus(
    const std::string& value,
    ValidationStatus fallback) {
    for (uint32_t candidate = 0u;
         candidate <= static_cast<uint32_t>(ValidationStatus::NotSupported);
         ++candidate) {
        const ValidationStatus status = static_cast<ValidationStatus>(candidate);
        if (value == StatusName(status)) return status;
    }
    return fallback;
}

ManualVerdict ParseVerdict(const std::string& value) {
    if (value == "PASS") return ManualVerdict::Pass;
    if (value == "FAIL") return ManualVerdict::Fail;
    if (value == "SKIP") return ManualVerdict::Skip;
    return ManualVerdict::None;
}

std::string MatrixJson() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::string json;
    json.reserve(65536u);
    json += "{\n  \"schema\": \"wotbmod.live-capability-matrix/v1\",\n";
    json += "  \"generated_at\": \"" + JsonEscape(TimestampUtc()) + "\",\n";
    json += "  \"fingerprint_key\": \"" + JsonEscape(g_fingerprint_key) + "\",\n";
    json += "  \"native_status_cap\": \"LIVE_TEST_PENDING until exact-fingerprint manual PASS\",\n";
    const size_t camera_index = FindCapability("camera.mode_change");
    const uint32_t camera_thread = camera_index < kCapabilityCount
        ? g_states[camera_index].callback_thread : 0u;
    char camera[768] = {};
    sprintf_s(
        camera,
        "  \"camera_inspector\": {\"active\": %s, \"handle\": \"%s\", \"generation\": %u, \"mode\": \"%s\", \"previous_mode\": \"%s\", \"mode_event_count\": %llu, \"duplicate_count\": %llu, \"thread_id\": %u, \"lifetime_ms\": %llu},\n",
        g_camera_last_handle != WOTBMOD_V3_INVALID_HANDLE ? "true" : "false",
        JsonEscape(HandleText(g_camera_last_handle)).c_str(),
        HandleGeneration(g_camera_last_handle),
        CameraModeName(g_camera_mode),
        CameraModeName(g_camera_previous_mode),
        static_cast<unsigned long long>(g_camera_event_count),
        static_cast<unsigned long long>(g_camera_duplicate_count),
        camera_thread,
        static_cast<unsigned long long>(
            g_camera_birth_ms ? UnixMilliseconds() - g_camera_birth_ms : 0u));
    json += camera;
    char render[768] = {};
    sprintf_s(
        render,
        "  \"render_inspector\": {\"backend\": %u, \"viewport\": {\"width\": %u, \"height\": %u}, \"device\": \"%s\", \"context\": \"%s\", \"swapchain\": \"%s\", \"resize_count\": %llu, \"swapchain_recreation_count\": %llu, \"device_lost_count\": %llu, \"device_restored_count\": %llu, \"render_callback_count\": %llu, \"last_render_thread_id\": %u, \"native_pointer_values_persisted\": false},\n",
        g_render_backend.load(), g_render_width.load(), g_render_height.load(),
        g_native_device_present.load() ? "present" : "absent",
        g_native_context_present.load() ? "present" : "absent",
        g_native_swapchain_present.load() ? "present" : "absent",
        static_cast<unsigned long long>(g_render_resize_count.load()),
        static_cast<unsigned long long>(g_swapchain_recreation_count.load()),
        static_cast<unsigned long long>(g_device_lost_count.load()),
        static_cast<unsigned long long>(g_device_restored_count.load()),
        static_cast<unsigned long long>(g_render_calls.load()),
        g_render_thread.load());
    json += render;
    json += "  \"entity_inspector\": [\n";
    for (size_t index = 0u; index < g_entity_observations.size(); ++index) {
        const EntityObservation& entity = g_entity_observations[index];
        char fields[640] = {};
        sprintf_s(
            fields,
            "    {\"public_id\": %u, \"type\": %u, \"handle\": \"%s\", \"generation\": %u, \"alive\": %s, \"created_before_mod\": %s, \"created_unix_ms\": %llu, \"destroyed_unix_ms\": %llu, \"old_handle_invalidated\": \"%s\"}",
            entity.public_id, entity.type,
            JsonEscape(HandleText(entity.handle)).c_str(), entity.generation,
            entity.alive ? "true" : "false",
            entity.preexisting ? "true" : "false",
            static_cast<unsigned long long>(entity.created_unix_ms),
            static_cast<unsigned long long>(entity.destroyed_unix_ms),
            entity.old_handle_invalidated < 0
                ? "UNKNOWN"
                : (entity.old_handle_invalidated ? "YES" : "NO"));
        json += fields;
        json += index + 1u == g_entity_observations.size() ? "\n" : ",\n";
    }
    json += "  ],\n";
    json += "  \"capabilities\": [\n";
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const CapabilitySpec& spec = kCapabilities[index];
        const CapabilityState& state = g_states[index];
        char numbers[512] = {};
        sprintf_s(
            numbers,
            "\"native\": %s, \"backend_available\": %s, \"call_count\": %llu, \"thread_id\": %u, \"last_event_unix_ms\": %llu, \"active_handles\": %u, \"generation\": %u, \"last_error_code\": %u",
            spec.native ? "true" : "false",
            state.backend_available ? "true" : "false",
            static_cast<unsigned long long>(state.call_count),
            state.callback_thread,
            static_cast<unsigned long long>(state.last_event_unix_ms),
            state.active_handles,
            state.generation,
            static_cast<unsigned>(state.last_code));
        json += "    {\"section\": \"" + JsonEscape(spec.section) +
            "\", \"id\": \"" + JsonEscape(spec.id) +
            "\", \"interface\": \"" + JsonEscape(spec.interface_id) +
            "\", \"binding_id\": \"" + JsonEscape(spec.binding_id) +
            "\", \"status\": \"" + StatusName(state.status) +
            "\", \"verdict\": \"" + VerdictName(state.verdict) +
            "\", " + numbers +
            ", \"handle\": \"" + JsonEscape(HandleText(state.handle)) +
            "\", \"last_result\": \"" + JsonEscape(state.last_result) +
            "\", \"warning\": " + std::string(state.warning ? "true" : "false") +
            ", \"last_error\": \"" + JsonEscape(state.last_error) +
            "\", \"comment\": \"" + JsonEscape(state.comment) + "\"}";
        json += index + 1u == kCapabilityCount ? "\n" : ",\n";
    }
    json += "  ]\n}\n";
    return json;
}

std::string ResultsJson() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::string json;
    json.reserve(32768u);
    json += "{\n  \"schema\": \"wotbmod.live-validation-results/v1\",\n";
    json += "  \"client_version\": \"" + JsonEscape(g_client_version) + "\",\n";
    json += "  \"client_build\": \"" + JsonEscape(g_client_version) + "\",\n";
    json += "  \"executable_sha256\": \"" + JsonEscape(g_client_sha) + "\",\n";
    char loader[96] = {};
    sprintf_s(
        loader, "sdk=%u;bootstrap=%u",
        g_bootstrap ? g_bootstrap->sdk_version : 0u,
        g_bootstrap ? g_bootstrap->bootstrap_version : 0u);
    json += "  \"loader_version\": \"" + std::string(loader) + "\",\n";
    json += "  \"validation_mod_version\": \"" + std::string(kModVersion) + "\",\n";
    json += "  \"validated_at\": \"" + JsonEscape(TimestampUtc()) + "\",\n";
    json += "  \"fingerprint_key\": \"" + JsonEscape(g_fingerprint_key) + "\",\n";
    char count[64] = {};
    sprintf_s(count, "  \"load_cycle_count\": %llu,\n",
              static_cast<unsigned long long>(g_load_cycle_count));
    json += count;
    const uint64_t callback_count = g_callback_count.load();
    char performance[768] = {};
    sprintf_s(
        performance,
        "  \"performance\": {\"callbacks_per_second\": %.3f, "
        "\"trace_per_second\": %.3f, \"bytes_per_second\": %.3f, "
        "\"snapshot_writes_per_minute\": %.3f, "
        "\"draw_calls_per_frame\": %.3f, "
        "\"average_callback_us\": %.3f, \"max_callback_us\": %.3f, "
        "\"total_callbacks\": %llu, \"total_trace_entries\": %llu, "
        "\"total_trace_bytes\": %llu, \"total_snapshot_writes\": %llu},\n",
        g_callbacks_per_second,
        g_trace_per_second,
        g_bytes_per_second,
        g_snapshot_writes_per_minute,
        g_draw_calls_per_frame,
        callback_count
            ? static_cast<double>(g_callback_total_100ns.load()) /
                static_cast<double>(callback_count) / 10.0
            : 0.0,
        static_cast<double>(g_callback_max_100ns.load()) / 10.0,
        static_cast<unsigned long long>(callback_count),
        static_cast<unsigned long long>(g_trace_count.load()),
        static_cast<unsigned long long>(g_trace_bytes.load()),
        static_cast<unsigned long long>(g_snapshot_writes.load()));
    json += performance;
    json += "  \"results\": [\n";
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const CapabilityState& state = g_states[index];
        json += "    {\"test_id\": \"" + JsonEscape(kCapabilities[index].id) +
            "\", \"status\": \"" + StatusName(state.status) +
            "\", \"verdict\": \"" + VerdictName(state.verdict) +
            "\", \"warning\": " + std::string(state.warning ? "true" : "false") +
            ", \"comment\": \"" + JsonEscape(state.comment) +
            "\", \"crash_disabled\": " +
            std::string(state.crash_disabled ? "true" : "false") + "}";
        json += index + 1u == kCapabilityCount ? "\n" : ",\n";
    }
    json += "  ]\n}\n";
    return json;
}

void SaveSnapshots() {
    if (g_snapshot_batch.load() != 0u) {
        g_snapshots_dirty.store(1u);
        return;
    }
    g_snapshot_writes.fetch_add(1u);
    if (!g_matrix_path.empty()) WriteTextAtomic(g_matrix_path, MatrixJson());
    if (!g_results_path.empty()) WriteTextAtomic(g_results_path, ResultsJson());
    SaveLiveSettings();
}

void LoadResults() {
    std::string text;
    if (!ReadFileBounded(g_results_path, 1024u * 1024u, &text)) {
        g_load_cycle_count = 1u;
        return;
    }
    if (JsonStringValue(text, "fingerprint_key") != g_fingerprint_key) {
        g_load_cycle_count = 1u;
        return;
    }
    g_load_cycle_count = JsonUnsignedValue(text, "load_cycle_count", 0u) + 1u;
    size_t cursor = 0u;
    while ((cursor = text.find("{\"test_id\":", cursor)) != std::string::npos) {
        const size_t end = text.find('}', cursor);
        if (end == std::string::npos) break;
        const std::string line = text.substr(cursor, end - cursor + 1u);
        const size_t index = FindCapability(JsonStringValue(line, "test_id").c_str());
        if (index < kCapabilityCount) {
            CapabilityState& state = g_states[index];
            state.verdict = ParseVerdict(JsonStringValue(line, "verdict"));
            state.status = ParseStatus(JsonStringValue(line, "status"), state.status);
            state.comment = SanitizeComment(JsonStringValue(line, "comment"));
            state.warning = line.find("\"warning\": true") != std::string::npos;
            state.crash_disabled = line.find("\"crash_disabled\": true") != std::string::npos;
            if (kCapabilities[index].native &&
                state.verdict == ManualVerdict::None &&
                state.status == ValidationStatus::Supported) {
                state.status = ValidationStatus::LiveTestPending;
            }
        }
        cursor = end + 1u;
    }
}

void WriteReloadPending() {
    if (g_reload_pending_path.empty()) return;
    const std::string body =
        std::to_string(g_reload_pending_from) + "\n" +
        g_reload_pending_row + "\n";
    WriteTextAtomic(g_reload_pending_path, body);
}

void ReadReloadPending() {
    g_reload_pending_from = 0u;
    g_reload_pending_row.clear();
    std::string text;
    if (g_reload_pending_path.empty() ||
        !ReadFileBounded(g_reload_pending_path, 512u, &text)) {
        return;
    }
    const size_t newline = text.find('\n');
    if (newline == std::string::npos) return;
    g_reload_pending_from = std::strtoull(
        text.substr(0u, newline).c_str(), nullptr, 10);
    const size_t start = newline + 1u;
    const size_t next = text.find('\n', start);
    g_reload_pending_row = text.substr(
        start,
        next == std::string::npos ? std::string::npos : next - start);
    while (!g_reload_pending_row.empty() &&
           (g_reload_pending_row.back() == '\r' ||
            g_reload_pending_row.back() == '\n' ||
            g_reload_pending_row.back() == ' ')) {
        g_reload_pending_row.pop_back();
    }
}

void ClearReloadPending() {
    g_reload_pending_from = 0u;
    g_reload_pending_row.clear();
    if (!g_reload_pending_path.empty()) {
        DeleteFileA(g_reload_pending_path.c_str());
    }
}

// Called by a reload test just before request_reload: stamp the current boot
// generation so the reloaded instance can prove the module really cycled.
void MarkReloadPending(size_t index) {
    if (index >= kCapabilityCount) return;
    g_reload_pending_from = g_load_cycle_count;
    g_reload_pending_row = kCapabilities[index].id;
    WriteReloadPending();
}

void MarkReloadRowPassed(const char* id, const std::string& message) {
    const size_t index = FindCapability(id);
    if (index >= kCapabilityCount) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    CapabilityState& state = g_states[index];
    ++state.call_count;
    state.last_code = WOTBMOD_V3_OK;
    state.last_result = message;
    state.last_error.clear();
    state.warning = false;
    state.verdict = ManualVerdict::Pass;
    state.status = ValidationStatus::Supported;
}

// Runs once on boot, after LoadResults() has bumped g_load_cycle_count. If a
// reload was requested in a previous instance and the generation has advanced,
// the module demonstrably unloaded and reloaded: record the real PASS.
void ProcessReloadGeneration() {
    ReadReloadPending();
    if (g_reload_pending_from == 0u ||
        g_load_cycle_count <= g_reload_pending_from) {
        return;
    }
    const std::string message =
        "module reloaded live (load cycle " +
        std::to_string(g_reload_pending_from) + " -> " +
        std::to_string(g_load_cycle_count) + ")";
    if (!g_reload_pending_row.empty()) {
        MarkReloadRowPassed(g_reload_pending_row.c_str(), message);
    }
    // A completed reload is itself proof of a clean unload barrier.
    MarkReloadRowPassed("reload.cleanup", message);
    Log(WOTBMOD_V3_LOG_INFO,
        "hot reload proven: " + message + " (row=" +
            (g_reload_pending_row.empty()
                 ? std::string("reload.cleanup")
                 : g_reload_pending_row) +
            ")");
    ClearReloadPending();
    SaveSnapshots();
}

void InitializeTestSelection() {
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        g_test_enabled[index] =
            (!CapabilityHighFrequency(index) &&
             kCapabilities[index].baseline != ValidationStatus::NotSupported)
            ? 1u : 0u;
    }
}

void SaveLiveSettings() {
    if (g_settings_path.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    std::string selected;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        if (!g_test_enabled[index]) continue;
        if (!selected.empty()) selected += ',';
        selected += kCapabilities[index].id;
    }
    std::string json = "{\n";
    json += "  \"schema\": \"wotbmod.live-test-center-settings/v1\",\n";
    json += "  \"fingerprint_key\": \"" + JsonEscape(g_fingerprint_key) + "\",\n";
    json += "  \"selected\": \"" + JsonEscape(selected) + "\",\n";
    json += "  \"sound_mode\": " + std::to_string(static_cast<uint32_t>(g_sound_mode)) + ",\n";
    char volume[32] = {};
    sprintf_s(volume, "%.3f", static_cast<double>(g_sound_volume));
    json += "  \"sound_volume\": " + std::string(volume) + ",\n";
    json += "  \"battle_mute\": " + std::string(g_battle_mute ? "true" : "false") + ",\n";
    json += "  \"log_filter\": " + std::to_string(g_log_filter) + ",\n";
    json += "  \"log_paused\": " + std::string(g_log_paused ? "true" : "false") + ",\n";
    json += "  \"log_autoscroll\": " + std::string(g_log_autoscroll ? "true" : "false") + ",\n";
    json += "  \"panel_x\": " + std::to_string(g_panel_x) + ",\n";
    json += "  \"panel_y\": " + std::to_string(g_panel_y) + ",\n";
    json += "  \"panel_width\": " + std::to_string(g_panel_width) + ",\n";
    json += "  \"panel_height\": " + std::to_string(g_panel_height) + "\n}\n";
    WriteTextAtomic(g_settings_path, json);
}

void LoadLiveSettings() {
    std::string text;
    if (!ReadFileBounded(g_settings_path, 128u * 1024u, &text) ||
        JsonStringValue(text, "fingerprint_key") != g_fingerprint_key) {
        return;
    }
    const std::string selected = JsonStringValue(text, "selected");
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const std::string marker = std::string(",") + kCapabilities[index].id + ",";
        const std::string padded = "," + selected + ",";
        g_test_enabled[index] = padded.find(marker) != std::string::npos ? 1u : 0u;
    }
    g_sound_mode = static_cast<SoundNotificationMode>(std::min<uint64_t>(
        JsonUnsignedValue(text, "sound_mode", 2u), 3u));
    g_sound_volume = std::clamp(
        static_cast<float>(JsonUnsignedValue(text, "sound_volume_milli", 180u)) / 1000.0f,
        0.0f, 1.0f);
    const size_t volume_begin = text.find("\"sound_volume\":");
    if (volume_begin != std::string::npos) {
        const char* start = text.c_str() + volume_begin + 16u;
        char* end = nullptr;
        const float parsed = static_cast<float>(std::strtod(start, &end));
        if (end != start) g_sound_volume = std::clamp(parsed, 0.0f, 1.0f);
    }
    g_battle_mute = text.find("\"battle_mute\": true") != std::string::npos;
    g_log_filter = static_cast<uint32_t>(std::min<uint64_t>(
        JsonUnsignedValue(text, "log_filter", 0u), 5u));
    g_log_paused = text.find("\"log_paused\": true") != std::string::npos;
    g_log_autoscroll = text.find("\"log_autoscroll\": false") == std::string::npos;
    g_panel_x = static_cast<int32_t>(std::clamp<uint64_t>(
        JsonUnsignedValue(text, "panel_x", 16u), 0u, 4000u));
    g_panel_y = static_cast<int32_t>(std::clamp<uint64_t>(
        JsonUnsignedValue(text, "panel_y", 16u), 0u, 4000u));
    g_panel_width = static_cast<int32_t>(std::clamp<uint64_t>(
        JsonUnsignedValue(text, "panel_width", 1888u), 800u, 4000u));
    g_panel_height = static_cast<int32_t>(std::clamp<uint64_t>(
        JsonUnsignedValue(text, "panel_height", 1048u), 600u, 4000u));
}

void SetAllTests(bool enabled, bool include_unsupported) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        g_test_enabled[index] = enabled &&
            (include_unsupported || !kCapabilities[index].deliberately_unsupported)
            ? 1u : 0u;
    }
    g_snapshots_dirty.store(1u);
}

void ApplyPreset(uint32_t preset) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const CapabilitySpec& spec = kCapabilities[index];
        bool enabled = false;
        const char* category = CapabilityPanelCategory(index);
        switch (preset) {
            case 0u: // Minimal
                enabled = std::strcmp(spec.id, "client.fingerprint") == 0 ||
                    std::strcmp(spec.id, "lifecycle.info") == 0 ||
                    std::strcmp(spec.id, "camera.active") == 0 ||
                    std::strcmp(spec.id, "render.backend") == 0;
                break;
            case 1u: // Hangar
                enabled = std::strcmp(category, "Client") == 0 ||
                    std::strcmp(category, "Camera") == 0 ||
                    std::strcmp(category, "UI") == 0 ||
                    std::strcmp(category, "Audio") == 0 ||
                    std::strcmp(category, "Resources") == 0 ||
                    std::strcmp(category, "LeaveToHangar") == 0 ||
                    std::strcmp(category, "Render / D3D11 / DXGI") == 0;
                break;
            case 2u: // Camera
                enabled = std::strcmp(category, "Camera") == 0;
                break;
            case 3u: // Battle shot
                enabled = std::strcmp(category, "Projectile") == 0 ||
                    std::strcmp(category, "Shell") == 0 ||
                    std::strcmp(category, "Tracer") == 0 ||
                    std::strcmp(category, "BigWorld Entity") == 0 ||
                    std::strcmp(category, "Camera") == 0;
                break;
            case 4u: // UI
                enabled = std::strcmp(category, "UI") == 0;
                break;
            case 5u: // Render
                enabled = std::strcmp(category, "Render / D3D11 / DXGI") == 0;
                break;
            case 6u: // Audio
                enabled = std::strcmp(category, "Audio") == 0;
                break;
            case 7u: // Resources
                enabled = std::strcmp(category, "Resources") == 0 ||
                    std::strcmp(category, "VFS / Overlays") == 0 ||
                    std::strcmp(category, "Async") == 0;
                break;
            case 8u: // Reload stress
                enabled = std::strcmp(category, "Reload / Unload") == 0 ||
                    std::strcmp(category, "Async") == 0;
                break;
            case 9u: // Safe
                enabled = spec.safe_test != SafeTest::None &&
                    !spec.deliberately_unsupported;
                break;
            case 10u: // All available
                enabled = !spec.deliberately_unsupported &&
                    g_states[index].backend_available;
                break;
            default:
                enabled = false;
                break;
        }
        g_test_enabled[index] = enabled ? 1u : 0u;
    }
    g_snapshots_dirty.store(1u);
}

void AppendFailure(
    const char* test_id,
    const std::string& comment,
    const char* reason) {
    if (g_failures_path.empty()) return;
    std::string entry;
    entry += "\n## " + TimestampUtc() + " — `" +
        std::string(test_id ? test_id : "unknown") + "`\n\n";
    entry += "- Fingerprint: `" + g_fingerprint_key + "`\n";
    entry += "- Reason: " + std::string(reason ? reason : "manual FAIL") + "\n";
    entry += "- Comment: " +
        (comment.empty() ? std::string("(empty)") : comment) + "\n";
    WriteFileExact(g_failures_path, entry.data(), entry.size(), true);
}

void WriteCrashMarker(size_t index, const char* action) {
    if (index >= kCapabilityCount || g_marker_path.empty()) return;
    RefreshClientState();
    std::string marker;
    marker += "{\n  \"schema\": \"wotbmod.active-native-test/v1\",\n";
    marker += "  \"active\": true,\n";
    marker += "  \"test_id\": \"" + JsonEscape(kCapabilities[index].id) + "\",\n";
    marker += "  \"capability\": \"" + JsonEscape(kCapabilities[index].section) + "\",\n";
    marker += "  \"binding\": \"" + JsonEscape(kCapabilities[index].binding_id) + "\",\n";
    marker += "  \"client_state\": \"" + JsonEscape(g_client_state) + "\",\n";
    marker += "  \"thread_id\": " + std::to_string(GetCurrentThreadId()) + ",\n";
    marker += "  \"owner\": \"" + std::string(kModId) + "\",\n";
    marker += "  \"last_action\": \"" + JsonEscape(action ? action : "") + "\",\n";
    marker += "  \"fingerprint_key\": \"" + JsonEscape(g_fingerprint_key) + "\",\n";
    marker += "  \"timestamp\": \"" + JsonEscape(TimestampUtc()) + "\"\n}\n";
    WriteTextAtomic(g_marker_path, marker);
    if (g_diagnostics && g_diagnostics->crash_set_last_action) {
        g_diagnostics->crash_set_last_action(g_mod, action ? action : "");
    }
}

void ClearCrashMarker() {
    if (!g_marker_path.empty()) DeleteFileA(g_marker_path.c_str());
    if (g_diagnostics && g_diagnostics->crash_set_last_action) {
        g_diagnostics->crash_set_last_action(g_mod, "idle");
    }
}

void RecoverCrashMarker() {
    std::string marker;
    if (!ReadFileBounded(g_marker_path, 64u * 1024u, &marker)) return;
    if (JsonStringValue(marker, "fingerprint_key") != g_fingerprint_key) {
        DeleteFileA(g_marker_path.c_str());
        return;
    }
    const std::string test_id = JsonStringValue(marker, "test_id");
    const size_t index = FindCapability(test_id.c_str());
    if (index < kCapabilityCount) {
        CapabilityState& state = g_states[index];
        state.status = ValidationStatus::Failed;
        state.crash_disabled = true;
        state.last_code = WOTBMOD_V3_E_CALLBACK_FAULT;
        state.last_result = "previous client session ended with active test marker";
        state.last_error = "CRASH_SUSPECTED; test auto-disabled";
        AppendFailure(
            kCapabilities[index].id,
            JsonStringValue(marker, "last_action"),
            "stale ACTIVE_NATIVE_TEST marker; correlation is suspected, not proven");
    }
    DeleteFileA(g_marker_path.c_str());
}

void RotateTraceIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(
            g_trace_path.c_str(), GetFileExInfoStandard, &data)) return;
    ULARGE_INTEGER size = {};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    if (size.QuadPart < kMaximumTraceBytes) return;
    const std::string previous = g_trace_path + ".1";
    DeleteFileA(previous.c_str());
    MoveFileExA(g_trace_path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Trace(
    const char* capability,
    const char* event_name,
    const char* owner,
    uint64_t handle,
    uint32_t generation,
    const char* result,
    const char* error_code,
    const std::string& payload_summary) {
    if (g_trace_path.empty() || !event_name) return;
    const std::string lower_event = ToLower(event_name);
    if (lower_event.find("chat") != std::string::npos) return;
    RefreshClientState();
    RotateTraceIfNeeded();
    std::string line;
    line.reserve(1024u + payload_summary.size());
    line += "{\"timestamp\":\"" + JsonEscape(TimestampUtc()) +
        "\",\"client_state\":\"" + JsonEscape(g_client_state) +
        "\",\"thread_id\":" + std::to_string(GetCurrentThreadId()) +
        ",\"mod_id\":\"" + std::string(kModId) +
        "\",\"capability\":\"" + JsonEscape(capability ? capability : "") +
        "\",\"event\":\"" + JsonEscape(event_name) +
        "\",\"owner\":\"" + JsonEscape(owner ? owner : "") +
        "\",\"handle\":\"" + JsonEscape(HandleText(handle)) +
        "\",\"generation\":" + std::to_string(generation) +
        ",\"result\":\"" + JsonEscape(result ? result : "") +
        "\",\"error_code\":\"" + JsonEscape(error_code ? error_code : "") +
        "\",\"payload_summary\":" +
        (payload_summary.empty() ? std::string("{}") : payload_summary) + "}\n";
    QueueLogLine(g_trace_path, line);
    g_trace_count.fetch_add(1u);
    g_trace_bytes.fetch_add(line.size());
}

void InitializePaths() {
    char path[WOTBMOD_V3_MAX_PATH * 2u] = {};
    uint32_t size = static_cast<uint32_t>(sizeof(path));
    if (g_core && g_core->get_mod_data_directory &&
        g_core->get_mod_data_directory(g_mod, path, &size) == WOTBMOD_V3_OK) {
        g_data_directory = path;
    } else {
        char module_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        char* separator = std::strrchr(module_path, '\\');
        if (separator) *separator = '\0';
        g_data_directory = module_path;
        g_data_directory += "\\mods\\data\\wotbmod.native_validation";
    }
    EnsureDirectory(g_data_directory);
    std::string game_directory;
    char executable[MAX_PATH] = {};
    const DWORD executable_length = GetModuleFileNameA(
        nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    if (executable_length > 0u && executable_length < sizeof(executable)) {
        char* separator = std::strrchr(executable, '\\');
        if (separator) {
            *separator = '\0';
            game_directory = executable;
        }
    }
    if (!game_directory.empty()) {
        std::string logs_directory;
        if (JoinPath(game_directory, "mods\\logs", &logs_directory)) {
            EnsureDirectory(logs_directory);
            JoinPath(logs_directory, "native_validation_live.log", &g_live_log_path);
            JoinPath(logs_directory, "native_validation_live_report.md", &g_report_path);
        }
    }
    JoinPath(g_data_directory, "settings.live.json", &g_settings_path);
    JoinPath(g_data_directory, "audio", &g_audio_directory);
    EnsureDirectory(g_audio_directory);
    std::string pass_audio;
    std::string warn_audio;
    std::string fail_audio;
    std::string complete_audio;
    if (JoinPath(g_audio_directory, "pass.wav", &pass_audio)) {
        WriteWaveTone(pass_audio, 660u, 1200, 90u);
    }
    if (JoinPath(g_audio_directory, "warn.wav", &warn_audio)) {
        WriteWaveTone(warn_audio, 440u, 1000, 130u);
    }
    if (JoinPath(g_audio_directory, "fail.wav", &fail_audio)) {
        WriteWaveTone(fail_audio, 220u, 1600, 180u);
    }
    if (JoinPath(g_audio_directory, "complete.wav", &complete_audio)) {
        WriteWaveTone(complete_audio, 880u, 1200, 140u);
    }
    JoinPath(g_data_directory, "LIVE_EVENT_TRACE.jsonl", &g_trace_path);
    JoinPath(g_data_directory, "LIVE_UI_TREE.jsonl", &g_ui_tree_path);
    JoinPath(g_data_directory, "LIVE_VEHICLE_STATE.jsonl", &g_vehicle_state_path);
    JoinPath(g_data_directory, "LIVE_VALIDATION_RESULTS.json", &g_results_path);
    JoinPath(g_data_directory, "LIVE_CAPABILITY_MATRIX.json", &g_matrix_path);
    JoinPath(g_data_directory, "LIVE_FAILURES.md", &g_failures_path);
    JoinPath(g_data_directory, "ACTIVE_NATIVE_TEST.json", &g_marker_path);
    JoinPath(g_data_directory, "FAIL_COMMENT.txt", &g_comment_path);
    JoinPath(g_data_directory, "reload_pending.txt", &g_reload_pending_path);
    if (GetFileAttributesA(g_failures_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::string header =
            "# LIVE failures\n\nGenerated by `wotbmod.native_validation`. "
            "A crash marker identifies only the last active test; it does not prove causality.\n";
        WriteTextAtomic(g_failures_path, header);
    }
    if (GetFileAttributesA(g_comment_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::string instruction =
            "Replace this line with a short non-sensitive FAIL comment, save the file, then press F8.\r\n";
        WriteTextAtomic(g_comment_path, instruction);
    }
}

void InitializeFingerprint() {
    WotbModV3ClientInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    if (g_bootstrap && g_bootstrap->get_client_info &&
        g_bootstrap->get_client_info(g_mod, &info) == WOTBMOD_V3_OK) {
        g_client_version = info.client_version[0] ? info.client_version : "unknown";
        g_client_sha = info.executable_sha256[0] ? info.executable_sha256 : "unknown";
        g_binding_pack_version = info.binding_pack_version;
    }
    char key[512] = {};
    sprintf_s(
        key, "%s|build=%s|sha256=%s|binding=%u|sdk=%u|bootstrap=%u|validation=%s",
        g_client_version.c_str(), g_client_version.c_str(), g_client_sha.c_str(),
        g_binding_pack_version,
        g_bootstrap ? g_bootstrap->sdk_version : 0u,
        g_bootstrap ? g_bootstrap->bootstrap_version : 0u,
        kModVersion);
    g_fingerprint_key = key;
}

void InitializeCapabilityStates() {
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        CapabilityState& state = g_states[index];
        state = CapabilityState();
        state.status = kCapabilities[index].baseline;
        WotbModV3InterfaceInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        const WotbModV3Result result =
            g_bootstrap && g_bootstrap->get_interface_info
            ? g_bootstrap->get_interface_info(
                  g_mod, kCapabilities[index].interface_id, &info)
            : WOTBMOD_V3_E_NOT_SUPPORTED;
        state.backend_available = result == WOTBMOD_V3_OK &&
            info.status != WOTBMOD_V3_CAPABILITY_UNAVAILABLE &&
            info.status != WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH;
        if (!state.backend_available &&
            kCapabilities[index].baseline != ValidationStatus::Supported) {
            state.status = ValidationStatus::NotSupported;
            state.last_code = result;
            state.last_error = info.unavailable_reason;
        } else if (kCapabilities[index].deliberately_unsupported) {
            state.status = ValidationStatus::NotSupported;
            state.backend_available = false;
            state.last_error = "intentionally disabled until ownership/lifetime/ABI is proven";
        } else if (kCapabilities[index].native && state.backend_available &&
                   state.status != ValidationStatus::NotSupported) {
            state.status = ValidationStatus::LiveTestPending;
        }
    }
}

void MarkManual(size_t index, ManualVerdict verdict) {
    if (index >= kCapabilityCount) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    CapabilityState& state = g_states[index];
    if (verdict == ManualVerdict::None) {
        state.verdict = verdict;
        state.comment.clear();
        state.status = kCapabilities[index].deliberately_unsupported
            ? ValidationStatus::NotSupported
            : (kCapabilities[index].native && state.backend_available
                ? ValidationStatus::LiveTestPending
                : kCapabilities[index].baseline);
    } else if (verdict == ManualVerdict::Pass) {
        state.verdict = verdict;
        state.status = kCapabilities[index].deliberately_unsupported ||
                !state.backend_available
            ? ValidationStatus::NotSupported
            : ValidationStatus::Supported;
        state.comment.clear();
    } else if (verdict == ManualVerdict::Fail) {
        std::string comment;
        ReadFileBounded(g_comment_path, 4096u, &comment);
        state.comment = SanitizeComment(comment);
        state.verdict = verdict;
        state.status = ValidationStatus::Failed;
        AppendFailure(kCapabilities[index].id, state.comment, "manual FAIL");
    } else {
        state.verdict = verdict;
        state.comment.clear();
        state.status = kCapabilities[index].deliberately_unsupported ||
                !state.backend_available
            ? ValidationStatus::NotSupported
            : (kCapabilities[index].native
                ? ValidationStatus::LiveTestPending
                : kCapabilities[index].baseline);
    }
    Trace(
        kCapabilities[index].id,
        "validation.manual_verdict",
        kModId,
        state.handle,
        state.generation,
        VerdictName(state.verdict),
        state.status == ValidationStatus::Failed ? "MANUAL_FAIL" : "",
        "{\"verdict\":\"" +
            std::string(VerdictName(state.verdict)) +
            "\",\"comment_present\":" +
            std::string(state.comment.empty() ? "false" : "true") + "}");
    QueueLiveLog(
        index,
        verdict == ManualVerdict::Pass
            ? LiveSeverity::Pass
            : (verdict == ManualVerdict::Fail ? LiveSeverity::Fail : LiveSeverity::Info),
        "validation.manual_verdict",
        std::string("verdict=") + VerdictName(verdict) +
            (state.comment.empty() ? "" : " comment=" + state.comment));
    SaveSnapshots();
}

void RecordEventCapability(
    const char* id,
    const char* topic,
    uint64_t handle,
    const std::string& summary) {
    const size_t index = FindCapability(id);
    if (index >= kCapabilityCount) return;
    RecordResult(index, WOTBMOD_V3_OK,
                 std::string(topic ? topic : "event") + " " + summary, handle,
                 handle == WOTBMOD_V3_INVALID_HANDLE ? 0u : 1u);
    if (IsTestEnabled(index)) {
        Trace(kCapabilities[index].id, topic, "loader-owned-queue", handle,
              HandleGeneration(handle), "observed", "", summary);
    }
    QueueLiveLog(index, LiveSeverity::Pass, topic, summary);
}

std::string EventSummary(const WotbModV3Event* event, uint64_t* out_handle) {
    if (out_handle) *out_handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!event || !event->payload) return "{}";
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_RPC_OBSERVED) == 0 &&
        event->payload_size >= sizeof(WotbModV3ObservedRpc)) {
        const WotbModV3ObservedRpc* rpc =
            static_cast<const WotbModV3ObservedRpc*>(event->payload);
        return "{\"public_entity_id\":" + std::to_string(rpc->public_entity_id) +
            ",\"method_key\":\"" + JsonEscape(rpc->method_name) +
            "\",\"payload_size\":0,\"payload_size_known\":false,\"direction\":" +
            std::to_string(rpc->direction) +
            ",\"schema_known\":false,\"queue\":\"loader-owned\"}";
    }
    if ((std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED) == 0) &&
        event->payload_size >= sizeof(WotbModV3PublicEntityLifecycleEvent)) {
        const WotbModV3PublicEntityLifecycleEvent* entity =
            static_cast<const WotbModV3PublicEntityLifecycleEvent*>(event->payload);
        if (out_handle) *out_handle = entity->snapshot.handle;
        ObserveEntity(
            entity->snapshot,
            std::strcmp(
                event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED) == 0,
            false);
        return "{\"public_id\":" + std::to_string(entity->snapshot.public_id) +
            ",\"type\":" + std::to_string(entity->snapshot.type) +
            ",\"visible\":" +
            std::string(entity->snapshot.visible_to_player ? "true" : "false") +
            ",\"reason\":" + std::to_string(entity->reason) + "}";
    }
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED) == 0 &&
        event->payload_size >= sizeof(WotbModV3LocalShellFiredEvent)) {
        const WotbModV3LocalShellFiredEvent* shell =
            static_cast<const WotbModV3LocalShellFiredEvent*>(event->payload);
        if (out_handle) *out_handle = shell->projectile;
        return "{\"shot_id\":\"unresolved\",\"projectile_id\":\"" +
            JsonEscape(HandleText(shell->projectile)) +
            "\",\"tracer_id\":\"unresolved\",\"impact_id\":\"unresolved\",\"shell_public_id\":" +
            std::to_string(shell->shell_public_id) + "}";
    }
    if ((std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_CREATED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_UPDATED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED) == 0) &&
        event->payload_size >= sizeof(WotbModV3ProjectileLifecycleEvent)) {
        const WotbModV3ProjectileLifecycleEvent* lifecycle =
            static_cast<const WotbModV3ProjectileLifecycleEvent*>(event->payload);
        if (out_handle) *out_handle = lifecycle->snapshot.projectile;
        return "{\"projectile_id\":\"" +
            JsonEscape(HandleText(lifecycle->snapshot.projectile)) +
            "\",\"previous_state\":" +
            std::to_string(lifecycle->previous_state) +
            ",\"state\":" +
            std::to_string(lifecycle->snapshot.lifecycle_state) +
            ",\"source\":" + std::to_string(lifecycle->snapshot.source) +
            ",\"valid_fields\":" +
            std::to_string(lifecycle->snapshot.valid_fields) +
            ",\"native_shot_id\":" +
            std::to_string(lifecycle->snapshot.native_shot_id) +
            ",\"reason\":" + std::to_string(lifecycle->reason) + "}";
    }
    if ((std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED) == 0 ||
         std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED) == 0) &&
        event->payload_size >= sizeof(WotbModV3VisibleTracerEvent)) {
        const WotbModV3VisibleTracerEvent* tracer =
            static_cast<const WotbModV3VisibleTracerEvent*>(event->payload);
        if (out_handle) *out_handle = tracer->projectile;
        return "{\"shot_id\":\"unresolved\",\"projectile_id\":\"" +
            JsonEscape(HandleText(tracer->projectile)) +
            "\",\"tracer_id\":\"" + JsonEscape(HandleText(tracer->projectile)) +
            "\",\"impact_id\":\"unresolved\",\"owner_scope\":" +
            std::to_string(tracer->owner_scope) + "}";
    }
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED) == 0 &&
        event->payload_size >= sizeof(WotbModV3ClientEventEnvelope)) {
        const WotbModV3ClientEventEnvelope* envelope =
            static_cast<const WotbModV3ClientEventEnvelope*>(event->payload);
        g_camera_previous_mode = envelope->payload.camera.previous_mode;
        g_camera_mode = envelope->payload.camera.mode;
        ++g_camera_event_count;
        if (g_camera_previous_mode == g_camera_mode) ++g_camera_duplicate_count;
        return "{\"previous_mode\":" + std::to_string(g_camera_previous_mode) +
            ",\"mode\":" + std::to_string(g_camera_mode) +
            ",\"native_mode\":" +
            std::to_string(envelope->payload.camera.native_mode) +
            ",\"duplicates\":" + std::to_string(g_camera_duplicate_count) + "}";
    }
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_VEHICLE_KILLED) == 0 &&
        event->payload_size >= sizeof(WotbModV3ClientEventEnvelope)) {
        const WotbModV3ClientEventEnvelope* envelope =
            static_cast<const WotbModV3ClientEventEnvelope*>(event->payload);
        const WotbModV3VehicleKillEventData& kill = envelope->payload.kill;
        const std::string text =
            "{\"victim_id\":" + std::to_string(kill.victim_id) +
            ",\"killer_id\":" + std::to_string(kill.killer_id) +
            ",\"assist_id\":" + std::to_string(kill.assist_id) +
            ",\"reason\":" + std::to_string(kill.reason) +
            ",\"ammo_bay_exploded\":" + std::to_string(kill.ammo_bay_exploded) + "}";
        Log(WOTBMOD_V3_LOG_INFO, ("vehicle.killed received: " + text).c_str());
        return text;
    }
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SHELL_HIT) == 0 &&
        event->payload_size >= sizeof(WotbModV3ClientEventEnvelope)) {
        const WotbModV3ClientEventEnvelope* envelope =
            static_cast<const WotbModV3ClientEventEnvelope*>(event->payload);
        return "{\"shot_id\":" + std::to_string(envelope->payload.hit.shot_id) +
            ",\"projectile_id\":\"unresolved\",\"tracer_id\":\"unresolved\",\"impact_id\":" +
            std::to_string(envelope->payload.hit.shot_id) +
            ",\"confirmed\":true}";
    }
    if (std::strncmp(event->topic, "wotbmod.render.", 15u) == 0 &&
        event->payload_size >= sizeof(WotbModV3RenderLifecycleEvent)) {
        const WotbModV3RenderLifecycleEvent* render =
            static_cast<const WotbModV3RenderLifecycleEvent*>(event->payload);
        g_render_backend.store(render->backend);
        g_render_width.store(static_cast<uint32_t>(
            std::max(0.0f, render->viewport.width)));
        g_render_height.store(static_cast<uint32_t>(
            std::max(0.0f, render->viewport.height)));
        if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0) {
            g_render_resize_count.fetch_add(1u);
        }
        if ((render->component_changes &
             WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN) != 0u) {
            g_swapchain_recreation_count.fetch_add(1u);
        }
        if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0) {
            g_device_lost_count.fetch_add(1u);
        }
        if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0 ||
            std::strcmp(event->topic, WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED) == 0) {
            g_device_restored_count.fetch_add(1u);
        }
        return "{\"previous_backend\":" +
            std::to_string(render->previous_backend) +
            ",\"backend\":" + std::to_string(render->backend) +
            ",\"component_changes\":" +
            std::to_string(render->component_changes) +
            ",\"previous_device_available\":" +
            std::to_string(render->previous_device_available) +
            ",\"device_available\":" +
            std::to_string(render->device_available) +
            ",\"viewport_width\":" +
            std::to_string(render->viewport.width) +
            ",\"viewport_height\":" +
            std::to_string(render->viewport.height) + "}";
    }
    return "{\"payload_size\":" + std::to_string(event->payload_size) +
        ",\"payload_stored\":false}";
}

void UpdateShotCorrelation(
    const WotbModV3Event* event,
    uint64_t handle,
    const std::string& summary) {
    if (!event || handle == WOTBMOD_V3_INVALID_HANDLE) return;
    const std::string key = HandleText(handle);
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(g_shot_chains_mutex);
    ShotChain& chain = g_shot_chains[key];
    const uint64_t now = UnixMilliseconds();
    if (chain.started_ms == 0u) chain.started_ms = now;
    chain.last_ms = now;
    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_SHOT_FIRED) == 0) {
        chain.shell_fired = true;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_CREATED) == 0) {
        chain.projectile_created = true;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_UPDATED) == 0) {
        chain.in_flight = true;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED) == 0) {
        chain.tracer_created = true;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED) == 0 ||
               std::strcmp(event->topic, WOTBMOD_V3_EVENT_SHELL_HIT) == 0) {
        chain.impacted = true;
        chain.impact_visual = summary.find("impact_position") != std::string::npos;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED) == 0) {
        chain.projectile_destroyed = true;
    } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED) == 0) {
        chain.tracer_destroyed = true;
    }
    if (chain.projectile_destroyed) {
        const bool complete = chain.shell_fired && chain.projectile_created &&
            chain.in_flight && chain.impacted && chain.projectile_destroyed;
        QueueLiveLog(
            FindCapability("projectile.destroyed"),
            complete ? LiveSeverity::Pass : LiveSeverity::Warn,
            "shot.chain.completed",
            std::string("correlation=") + key + " steps=" +
                std::to_string((chain.shell_fired ? 1u : 0u) +
                               (chain.projectile_created ? 1u : 0u) +
                               (chain.in_flight ? 1u : 0u) +
                               (chain.impacted ? 1u : 0u) +
                               (chain.projectile_destroyed ? 1u : 0u)) + "/5");
        g_shot_chains.erase(key);
    }
}

void ExpireShotChains() {
    std::lock_guard<std::mutex> lock(g_shot_chains_mutex);
    const uint64_t now = UnixMilliseconds();
    for (auto iterator = g_shot_chains.begin(); iterator != g_shot_chains.end();) {
        ShotChain& chain = iterator->second;
        if (!chain.warned && chain.started_ms != 0u && now > chain.started_ms + 3000u) {
            chain.warned = true;
            QueueLiveLog(
                FindCapability("projectile.created"), LiveSeverity::Warn,
                "shot.chain.timeout",
                "correlation=" + iterator->first + " impact or destroy not received");
        }
        if (now > chain.started_ms + 15000u) iterator = g_shot_chains.erase(iterator);
        else ++iterator;
    }
}

// wotbmod.ges.* deliveries: count them, remember distinct types (capped),
// keep the last camera mode for the echo probe, and trace the first 32
// payload bytes when the runtime lets them be read. The runtime hands each
// subscriber its own copy of WotbModV3GesEvent and identifies the delivery
// by the payload pointer inside it, so reading through this copy is exactly
// what the read_* slots expect. A line per event would only churn the
// rotating trace on a hot bus, so each distinct type is traced once and the
// first 64 events overall.
/* wotbmod.session.cluster.changed: log every status; CONNECTED/FAILED judge
 * the session.cluster.change row when that row started the change. */
void OnSessionClusterEvent(const WotbModV3Event* event) {
    if (!event->payload || event->payload_size < sizeof(WotbModV3ClusterChangedEvent)) return;
    WotbModV3ClusterChangedEvent value = {};
    std::memcpy(&value, event->payload, sizeof(value));
    static const char* const kStatusNames[] = {
        "?", "QUEUED", "STARTED", "CONNECTED", "FAILED"};
    const char* status = value.status < 5u ? kStatusNames[value.status] : "?";
    const std::string text =
        "session.cluster event: from=" + std::to_string(value.from_cluster_id) +
        " to=" + std::to_string(value.to_cluster_id) + " status=" +
        std::to_string(value.status) + " (" + status + ")";
    Log(WOTBMOD_V3_LOG_INFO, text.c_str());
    const size_t index = FindCapability("session.cluster.change");
    if (index >= kCapabilityCount) return;
    QueueLiveLog(index, LiveSeverity::Info, "session.cluster.changed", text);
    const uint64_t started = g_session_change_started_ms.load();
    if (started == 0u) return;
    if (value.status != WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED &&
        value.status != WOTBMOD_V3_CLUSTER_CHANGE_FAILED) {
        return;
    }
    g_session_change_started_ms.store(0u);
    const uint64_t elapsed = UnixMilliseconds() - started;
    std::string now = "?";
    if (g_session_cluster) {
        WotbModV3ClusterInfo current = {};
        WOTBMOD_V3_INIT_STRUCT(current, WOTBMOD_V3_SESSION_CLUSTER_VERSION);
        if (g_session_cluster->get_current(g_mod, &current) == WOTBMOD_V3_OK) {
            now = std::string(current.name) + " (id " + std::to_string(current.cluster_id) + ")";
        }
    }
    const bool connected = value.status == WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED;
    const std::string verdict =
        std::string(connected ? "CONNECTED" : "FAILED") + " to=" +
        std::to_string(value.to_cluster_id) + " now=" + now + " after " +
        std::to_string(elapsed / 1000u) + " s";
    RecordResult(index, connected ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM, verdict);
    Log(WOTBMOD_V3_LOG_INFO, ("session.cluster.change judged: " + verdict).c_str());
    QueueLiveLog(index, connected ? LiveSeverity::Pass : LiveSeverity::Fail,
                 "session.cluster.judged", verdict);
    SaveSnapshots();
}

void OnGesEvent(const WotbModV3Event* event) {
    if (!event->payload || event->payload_size < sizeof(WotbModV3GesEvent)) return;
    WotbModV3GesEvent ges = {};
    std::memcpy(&ges, event->payload, sizeof(ges));
    if (ges.api_version != WOTBMOD_V3_GES_VERSION) return;
    ges.type_name[WOTBMOD_V3_GES_TYPE_NAME_SIZE - 1u] = '\0';
    const uint32_t ordinal = g_ges_events.fetch_add(1u) + 1u;
    if ((ges.flags & WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED) != 0u) g_ges_echo_seen.store(1u);
    bool first_seen = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (g_ges_types_seen.size() < 1024u) {
            first_seen = g_ges_types_seen.insert(ges.type_name).second;
        }
    }
    std::string bytes;
    if (g_ges && std::strcmp(ges.type_name, "Avatar::CameraModeChanged") == 0) {
        int32_t mode = 0;
        if (g_ges->read_i32(&ges, 0u, &mode) == WOTBMOD_V3_OK) {
            g_ges_last_camera_mode.store(mode);
        }
    }
    // The byte preview is only for the traced deliveries; reading eight
    // words through the runtime on every hangar/battle event was the bulk of
    // this callback's cost (2026-09-05).
    if (g_ges && (first_seen || ordinal <= 64u)) {
        const uint32_t limit =
            ges.payload_size != 0u && ges.payload_size < 32u ? ges.payload_size : 32u;
        for (uint32_t offset = 0u; offset + 4u <= limit; offset += 4u) {
            uint32_t word = 0u;
            if (g_ges->read_u32(&ges, offset, &word) != WOTBMOD_V3_OK) break;
            char text[16] = {};
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%s%08x",
                        bytes.empty() ? "" : " ", word);
            bytes += text;
        }
    }
    if (first_seen || ordinal <= 64u) {
        char rva[16] = {};
        _snprintf_s(rva, sizeof(rva), _TRUNCATE, "0x%08x", ges.publisher_rva);
        const std::string summary =
            "{\"type\":\"" + JsonEscape(ges.type_name) +
            "\",\"publisher_rva\":\"" + std::string(rva) +
            "\",\"size\":" + std::to_string(ges.payload_size) +
            ",\"schema_id\":" + std::to_string(ges.schema_id) +
            ",\"flags\":" + std::to_string(ges.flags) +
            ",\"ordinal\":" + std::to_string(ordinal) +
            ",\"bytes\":\"" + bytes + "\"}";
        Trace("ges.event_count", event->topic, ges.type_name,
              WOTBMOD_V3_INVALID_HANDLE, 0u, "OK", "", summary);
        if (first_seen) {
            RecordEventCapability("ges.event_count", event->topic,
                                  WOTBMOD_V3_INVALID_HANDLE, summary);
        }
    }
}

void WOTBMOD_V3_CALL EventCallback(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) noexcept {
    CallbackMetricScope callback_metric;
    try {
        if (!event || !event->topic[0]) return;
        // Frame events are intentionally excluded from validation. They can be
        // emitted every rendered frame and must never trigger diagnostics I/O.
        if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_FRAME_UPDATE) == 0 ||
            std::strcmp(event->topic, "wotbmod.frame.fixed_update") == 0) {
            return;
        }
        if (std::strncmp(event->topic, WOTBMOD_V3_GES_TOPIC_PREFIX,
                         sizeof(WOTBMOD_V3_GES_TOPIC_PREFIX) - 1u) == 0) {
            OnGesEvent(event);
            return;
        }
        if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED) == 0) {
            OnSessionClusterEvent(event);
            return;
        }
        uint64_t handle = WOTBMOD_V3_INVALID_HANDLE;
        const std::string summary = EventSummary(event, &handle);
        UpdateShotCorrelation(event, handle, summary);
        if (std::strcmp(event->topic, "mod.wotbmod.native_validation.self") == 0) {
            RecordEventCapability("events.loader_queue", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_RPC_OBSERVED) == 0) {
            RecordEventCapability("rpc.incoming_metadata", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED) == 0 ||
                   std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED) == 0 ||
                   std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED) == 0) {
            RecordEventCapability("entity.lifecycle", event->topic, handle, summary);
            const size_t entity_index = FindCapability("entity.lifecycle");
            if (entity_index < kCapabilityCount) {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                g_states[entity_index].active_handles = ActiveEntityCount();
            }
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED) == 0 ||
                   std::strcmp(event->topic, WOTBMOD_V3_EVENT_SHOT_FIRED) == 0) {
            RecordEventCapability("shell.ingress", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_CREATED) == 0) {
            RecordEventCapability("projectile.created", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_UPDATED) == 0) {
            RecordEventCapability("projectile.updated", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED) == 0) {
            RecordEventCapability("projectile.updated", event->topic, handle, summary);
            RecordEventCapability("impact.confirmed", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED) == 0) {
            RecordEventCapability("projectile.destroyed", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED) == 0) {
            RecordEventCapability("tracer.created", event->topic, handle, summary);
            RecordEventCapability("tracer.visible", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED) == 0) {
            RecordEventCapability("tracer.destroyed", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED) == 0) {
            RecordEventCapability("camera.mode_change", event->topic, handle, summary);
        } else if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SHELL_HIT) == 0) {
            RecordEventCapability("impact.confirmed", event->topic, handle, summary);
        } else if (std::strncmp(event->topic, "wotbmod.render.", 15u) == 0) {
            if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0) {
                RecordEventCapability("render.resize", event->topic, handle, summary);
            } else if (std::strcmp(
                           event->topic,
                           WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED) == 0) {
                RecordEventCapability("render.backend", event->topic, handle, summary);
            } else {
                RecordEventCapability("render.device_lifecycle", event->topic, handle, summary);
            }
        } else if (IsTestEnabled(FindCapability("events.loader_queue"))) {
            Trace("events.loader_queue", event->topic, "loader-owned-queue",
                  handle, HandleGeneration(handle), "observed", "", summary);
        }
        const size_t thread_index = FindCapability("events.callback_thread");
        if (thread_index < kCapabilityCount) {
            RecordResult(
                thread_index,
                event->thread_role == WOTBMOD_V3_THREAD_MAIN ||
                        event->thread_role == WOTBMOD_V3_THREAD_RENDER
                    ? WOTBMOD_V3_OK
                    : WOTBMOD_V3_E_WRONG_THREAD,
                "event callback role=" + std::to_string(event->thread_role));
        }
        // Persist at most once per periodic OnFrame maintenance pass instead of
        // rewriting two JSON snapshots synchronously inside every callback.
        g_snapshots_dirty.store(1u);
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "EventCallback contained an exception");
    }
}

WotbModV3Result WOTBMOD_V3_CALL EntityVisitor(
    WotbModV3Handle,
    const WotbModV3PublicEntitySnapshot* entity,
    void* user_data) noexcept {
    try {
        EntityVisitContext* context =
            static_cast<EntityVisitContext*>(user_data);
        if (!entity || !context || !entity->visible_to_player) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++context->count;
        ObserveEntity(*entity, false, context->preexisting);
        Trace("entity.enumerate_visible", "entity.snapshot", "public-registry",
              entity->handle, HandleGeneration(entity->handle), "visible", "",
              "{\"public_id\":" + std::to_string(entity->public_id) +
              ",\"type\":" + std::to_string(entity->type) +
              ",\"local\":" + std::string(entity->local_player ? "true" : "false") + "}");
        return WOTBMOD_V3_OK;
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

bool WriteWaveTone(
    const std::string& path,
    uint32_t frequency,
    int16_t amplitude,
    uint32_t duration_ms) {
#pragma pack(push, 1)
    struct WaveHeader {
        char riff[4];
        uint32_t riff_size;
        char wave[4];
        char fmt[4];
        uint32_t fmt_size;
        uint16_t format;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data[4];
        uint32_t data_size;
    };
#pragma pack(pop)
    constexpr uint32_t sample_rate = 22050u;
    const uint32_t sample_count = std::max<uint32_t>(
        1u, sample_rate * std::max<uint32_t>(duration_ms, 20u) / 1000u);
    const uint32_t data_size = sample_count * sizeof(int16_t);
    const WaveHeader header = {
        {'R','I','F','F'}, 36u + data_size, {'W','A','V','E'},
        {'f','m','t',' '}, 16u, 1u, 1u, sample_rate,
        sample_rate * sizeof(int16_t), sizeof(int16_t), 16u,
        {'d','a','t','a'}, data_size};
    HANDLE file = CreateFileA(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    bool ok = WriteFile(file, &header, sizeof(header), &written, nullptr) != FALSE &&
        written == sizeof(header);
    std::array<int16_t, 512u> samples = {};
    uint32_t produced = 0u;
    while (ok && produced < sample_count) {
        const uint32_t batch = std::min<uint32_t>(
            static_cast<uint32_t>(samples.size()), sample_count - produced);
        for (uint32_t index = 0u; index < batch; ++index) {
            const uint32_t sample = produced + index;
            const uint32_t period = std::max<uint32_t>(
                2u, sample_rate / std::max<uint32_t>(frequency, 20u));
            const uint32_t phase = sample % period;
            samples[index] = phase < period / 2u ? amplitude : -amplitude;
        }
        const DWORD bytes = batch * sizeof(int16_t);
        written = 0u;
        ok = WriteFile(file, samples.data(), bytes, &written, nullptr) != FALSE &&
            written == bytes;
        produced += batch;
    }
    CloseHandle(file);
    return ok;
}

bool EnsureProbeMount() {
    if (g_probe_mount != WOTBMOD_V3_INVALID_HANDLE) return true;
    if (!g_vfs || !g_vfs->mount_package) return false;
    std::string root;
    if (!JoinPath(g_data_directory, "probe_resources", &root) ||
        !EnsureDirectory(root)) return false;
    const WotbModV3Result result = g_vfs->mount_package(
        g_mod, "native-validation-data", root.c_str(), 1000,
        &g_probe_mount);
    return result == WOTBMOD_V3_OK || result == WOTBMOD_V3_E_ALREADY_EXISTS;
}

bool EnsureAudioMount() {
    if (g_audio_mount != WOTBMOD_V3_INVALID_HANDLE) return true;
    if (!g_vfs || !g_vfs->mount_package || g_audio_directory.empty()) return false;
    const WotbModV3Result result = g_vfs->mount_package(
        g_mod, "native-validation-audio", g_data_directory.c_str(), 1001,
        &g_audio_mount);
    return result == WOTBMOD_V3_OK || result == WOTBMOD_V3_E_ALREADY_EXISTS;
}

WotbModV3Result PlayFeedbackTone(LiveSeverity severity) {
    if (!g_audio) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const bool probe_mounted = EnsureProbeMount();
    const bool audio_mounted = !probe_mounted && EnsureAudioMount();
    if (!probe_mounted && !audio_mounted) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const char* file = severity == LiveSeverity::Fail ? "live-feedback-fail.wav" :
        severity == LiveSeverity::Warn ? "live-feedback-warn.wav" :
        "live-feedback-pass.wav";
    std::string physical;
    if (probe_mounted) {
        std::string root;
        if (!JoinPath(g_data_directory, "probe_resources", &root) ||
            !JoinPath(root, file, &physical)) return WOTBMOD_V3_E_IO;
    } else {
        const char* audio_file = severity == LiveSeverity::Fail ? "fail.wav" :
            severity == LiveSeverity::Warn ? "warn.wav" : "pass.wav";
        if (!JoinPath(g_audio_directory, audio_file, &physical)) return WOTBMOD_V3_E_IO;
        file = audio_file;
    }
    if (GetFileAttributesA(physical.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const uint32_t frequency = severity == LiveSeverity::Fail ? 220u :
            severity == LiveSeverity::Warn ? 440u : 660u;
        const int16_t amplitude = severity == LiveSeverity::Fail ? 1600 : 1200;
        const uint32_t duration = severity == LiveSeverity::Fail ? 180u :
            severity == LiveSeverity::Warn ? 130u : 90u;
        if (!WriteWaveTone(physical, frequency, amplitude, duration)) {
            return WOTBMOD_V3_E_IO;
        }
    }
    WotbModV3AudioDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_AUDIO_VERSION);
    std::string uri = "mod://wotbmod.native_validation/";
    if (probe_mounted) uri += file;
    else uri += "audio/" + std::string(file);
    descriptor.uri = uri.c_str();
    descriptor.volume = std::clamp(g_sound_volume, 0.0f, 1.0f);
    descriptor.pitch = 1.0f;
    descriptor.priority = severity == LiveSeverity::Fail ? 100u : 10u;
    descriptor.bus = "master";
    WotbModV3AudioHandle audio = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = g_audio->create(g_mod, &descriptor, &audio);
    if (result != WOTBMOD_V3_OK) return result;
    result = g_audio->preload(g_mod, audio);
    if (result == WOTBMOD_V3_OK) result = g_audio->play(g_mod, audio);
    if (result == WOTBMOD_V3_OK) Sleep(severity == LiveSeverity::Fail ? 60u : 35u);
    const WotbModV3Result stop = g_audio->stop(g_mod, audio, 0.03f);
    const WotbModV3Result destroy = g_audio->destroy(g_mod, audio);
    if (result != WOTBMOD_V3_OK) return result;
    if (stop != WOTBMOD_V3_OK) return stop;
    return destroy;
}

void PumpFeedback() {
    if (g_sound_mode == SoundNotificationMode::Off || g_battle_mute) return;
    const uint64_t now = UnixMilliseconds();
    if (now < g_last_sound_ms.load() + 180u) return;
    LiveSeverity severity = LiveSeverity::Info;
    uint32_t pending = g_pending_fail_sound.load();
    if (pending != 0u && g_pending_fail_sound.compare_exchange_strong(pending, pending - 1u)) {
        severity = LiveSeverity::Fail;
    } else {
        pending = g_pending_warn_sound.load();
        if (pending != 0u && g_pending_warn_sound.compare_exchange_strong(pending, pending - 1u)) {
            severity = LiveSeverity::Warn;
        } else {
            pending = g_pending_pass_sound.load();
            if (pending == 0u || !g_pending_pass_sound.compare_exchange_strong(pending, pending - 1u)) {
                return;
            }
            severity = LiveSeverity::Pass;
        }
    }
    g_last_sound_ms.store(now);
    const WotbModV3Result feedback_result = PlayFeedbackTone(severity);
    if (feedback_result != WOTBMOD_V3_OK) {
        QueueLogLine(g_live_log_path,
            "[" + TimestampReadable() + "]\r\n[WARN]\r\n[AUDIO]\r\nНе удалось воспроизвести feedback tone\r\nresult: " +
            std::to_string(static_cast<unsigned>(feedback_result)) + "\r\n\r\n");
    }
}

// vehicle.state: what the public-entity surface actually delivers for every
// vehicle the local client can see, written to LIVE_VEHICLE_STATE.jsonl. The
// snapshot fields (team/health/max_health) come straight from the native
// ingress; each is also queried through get_public_property so the row records
// OK vs NOT_SUPPORTED per field. Position/direction come from the loader's
// PlayerController pose hook for the local vehicle only (live 2026-09-06);
// other vehicles and display_name have no verified source and refuse. Battle
// only; this is the "vehicle state" evidence.
struct VehicleStateContext {
    uint32_t count = 0u;
    uint32_t allies = 0u;
    uint32_t enemies = 0u;
    uint32_t with_health = 0u;
    uint32_t named = 0u;
    uint32_t local_id = 0u;
    uint32_t local_team = 0u;
    std::string jsonl;
};

std::string VehicleStateEscape(const char* text) {
    std::string out;
    for (const char* p = text; *p; ++p) {
        if (*p == '"' || *p == '\\') out += '\\';
        if (static_cast<unsigned char>(*p) < 0x20u) { out += ' '; continue; }
        out += *p;
    }
    return out;
}

// value of a property as JSON, or null when the runtime refuses it
std::string ProbeValueJson(WotbModV3EntityHandle handle, const char* name) {
    if (!g_entities || !g_entities->get_public_property) return "null";
    WotbModV3PublicValue value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    if (g_entities->get_public_property(g_mod, handle, name, &value) != WOTBMOD_V3_OK) {
        return "null";
    }
    switch (value.type) {
        case WOTBMOD_V3_PUBLIC_VALUE_INT64:
            return std::to_string(value.value.integer);
        case WOTBMOD_V3_PUBLIC_VALUE_STRING:
            return "\"" + VehicleStateEscape(value.value.string_value) + "\"";
        default:
            return "null";
    }
}

std::string ProbeProperty(WotbModV3EntityHandle handle, const char* name) {
    if (!g_entities || !g_entities->get_public_property) return "no-api";
    WotbModV3PublicValue value = {};
    WOTBMOD_V3_INIT_STRUCT(value, WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    const WotbModV3Result result =
        g_entities->get_public_property(g_mod, handle, name, &value);
    return ResultName(result);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleStateVisitor(
    WotbModV3Handle,
    const WotbModV3PublicEntitySnapshot* entity,
    void* user_data) noexcept {
    try {
        VehicleStateContext* context =
            static_cast<VehicleStateContext*>(user_data);
        if (!entity || !context || !entity->visible_to_player) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++context->count;
        if (entity->type == WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE) {
            // teams are the arena's raw indices; "allies" are resolved
            // against the local vehicle's team once enumeration is over
            if (entity->team == 1u) ++context->allies;
            else if (entity->team == 2u) ++context->enemies;
            if (entity->max_health > 0) ++context->with_health;
            if (entity->local_player) {
                context->local_id = entity->public_id;
                context->local_team = entity->team;
            }
        }
        std::string line = "{\"public_id\":" + std::to_string(entity->public_id) +
            ",\"type\":" + std::to_string(entity->type) +
            ",\"local\":" + std::string(entity->local_player ? "true" : "false") +
            ",\"team\":" + std::to_string(entity->team) +
            ",\"health\":" + std::to_string(entity->health) +
            ",\"max_health\":" + std::to_string(entity->max_health) +
            ",\"public_type\":\"" + std::string(entity->public_type) + "\"" +
            ",\"position\":[" + std::to_string(entity->position.x) + "," +
            std::to_string(entity->position.y) + "," +
            std::to_string(entity->position.z) + "]" +
            ",\"direction\":[" + std::to_string(entity->direction.x) + "," +
            std::to_string(entity->direction.y) + "," +
            std::to_string(entity->direction.z) + "]" +
            ",\"display_name\":\"" + VehicleStateEscape(entity->display_name) + "\"" +
            ",\"clan_tag\":" + ProbeValueJson(entity->handle, "clan_tag") +
            ",\"account_id\":" + ProbeValueJson(entity->handle, "account_id") +
            ",\"kills\":" + ProbeValueJson(entity->handle, "kills") +
            ",\"vehicle_name\":" + ProbeValueJson(entity->handle, "vehicle_name") +
            ",\"vehicle_display_name\":" + ProbeValueJson(entity->handle, "vehicle_display_name") +
            ",\"props\":{";
        if (entity->display_name[0]) ++context->named;
        const char* names[] = {
            "team", "health", "max_health", "position", "direction", "display_name",
            "clan_tag", "account_id", "kills", "vehicle_name", "vehicle_display_name"};
        for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (i) line += ",";
            line += "\"" + std::string(names[i]) + "\":\"" +
                    ProbeProperty(entity->handle, names[i]) + "\"";
        }
        line += "}}\n";
        context->jsonl += line;
        return WOTBMOD_V3_OK;
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result RunVehicleStateTest(std::string* out_description) {
    if (!g_entities || !g_entities->enumerate_visible) {
        *out_description = "entity.public interface unavailable";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    RefreshClientState();
    VehicleStateContext context;
    const WotbModV3Result result =
        g_entities->enumerate_visible(g_mod, &VehicleStateVisitor, &context);
    if (result == WOTBMOD_V3_OK && !g_vehicle_state_path.empty()) {
        WriteFileExact(
            g_vehicle_state_path, context.jsonl.data(), context.jsonl.size(), false);
    }
    if (context.local_team == 2u) {
        const uint32_t team1 = context.allies;
        context.allies = context.enemies;
        context.enemies = team1;
    }
    std::string description =
        "state=" + g_client_state +
        " visible=" + std::to_string(context.count) +
        " allies=" + std::to_string(context.allies) +
        " enemies=" + std::to_string(context.enemies) +
        " local_team=" + std::to_string(context.local_team) +
        " with_health=" + std::to_string(context.with_health) +
        " named=" + std::to_string(context.named) +
        " local_id=" + std::to_string(context.local_id) +
        " file=LIVE_VEHICLE_STATE.jsonl";
    if (result != WOTBMOD_V3_OK) {
        description += " enumerate=" + std::string(ResultName(result)) +
            " (" + LastErrorMessage() + ")";
    }
    Log(result == WOTBMOD_V3_OK ? WOTBMOD_V3_LOG_INFO : WOTBMOD_V3_LOG_WARNING,
        ("vehicle.state judged: " + std::string(ResultName(result)) + " " +
         description).c_str());
    *out_description = description;
    return result;
}

// `out_step` names the call that failed and carries the runtime's own
// message: the row read FAIL on every 11.20 sweep with nothing but a code,
// and the step was guessed (docs, 6 September). Empty on success.
WotbModV3Result RunAudioTone(std::string* out_step = nullptr) {
    auto note = [&](const char* step, WotbModV3Result code) {
        if (out_step) {
            *out_step = std::string(step) + " -> " + ResultName(code) +
                        " (" + LastErrorMessage() + ")";
        }
    };
    if (!g_audio || !EnsureProbeMount()) {
        note(g_audio ? "probe mount" : "audio interface", WOTBMOD_V3_E_NOT_SUPPORTED);
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    std::string root;
    std::string wave;
    JoinPath(g_data_directory, "probe_resources", &root);
    JoinPath(root, "validation-tone.wav", &wave);
    if (!WriteWaveTone(wave)) {
        note("WriteWaveTone", WOTBMOD_V3_E_IO);
        return WOTBMOD_V3_E_IO;
    }
    WotbModV3AudioDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_AUDIO_VERSION);
    // `mod://self/`, not the package id: the id is not a VFS authority this
    // mod owns, and ResolveVirtualFilePath refused it as E_PERMISSION_DENIED
    // on every 11.20 sweep (the text fixture learned the same lesson).
    descriptor.uri = "mod://self/validation-tone.wav";
    descriptor.volume = 0.18f;
    descriptor.pitch = 1.0f;
    descriptor.bus = "master";
    WotbModV3AudioHandle audio = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = g_audio->create(g_mod, &descriptor, &audio);
    if (result != WOTBMOD_V3_OK) {
        note("create", result);
        return result;
    }
    const char* step = "preload";
    result = g_audio->preload(g_mod, audio);
    if (result == WOTBMOD_V3_OK) {
        step = "play";
        result = g_audio->play(g_mod, audio);
    }
    if (result != WOTBMOD_V3_OK) note(step, result);
    if (result == WOTBMOD_V3_OK) Sleep(80u);
    // Fade 0: the Windows backend has no timed fades, and the host answers
    // NOT_SUPPORTED to any non-zero fade on stop and to any non-zero
    // duration on fade_to (live 11.20, sweep 3 of 2026-09-08). The row
    // proves the create/preload/play/stop/destroy chain, not a fade.
    const WotbModV3Result stop = g_audio->stop(g_mod, audio, 0.0f);
    if (stop != WOTBMOD_V3_OK && result == WOTBMOD_V3_OK) note("stop", stop);
    const WotbModV3Result destroy = g_audio->destroy(g_mod, audio);
    if (destroy != WOTBMOD_V3_OK && result == WOTBMOD_V3_OK && stop == WOTBMOD_V3_OK) {
        note("destroy", destroy);
    }
    if (result != WOTBMOD_V3_OK) return result;
    if (stop != WOTBMOD_V3_OK) return stop;
    return destroy;
}

WotbModV3Result RunResourceText(bool asynchronous) {
    if (!g_resources || !EnsureProbeMount()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    std::string root;
    std::string path;
    JoinPath(g_data_directory, "probe_resources", &root);
    JoinPath(root, "validation-probe.txt", &path);
    const std::string content = "wotbmod.native_validation\n";
    if (!WriteFileExact(path, content.data(), content.size(), false)) {
        return WOTBMOD_V3_E_IO;
    }
    WotbModV3ResourceLoadDesc descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_RESOURCES_VERSION);
    // `mod://self/`, NOT `mod://wotbmod.native_validation/`.
    //
    // The package id is not a VFS authority this mod owns. The fixture lives
    // under the "native-validation-data" mount EnsureProbeMount created over
    // probe_resources/, and the package id resolves - when it resolves at all -
    // to the package root, where this file does not exist. Live on 11.19.0.834
    // that read as E_PERMISSION_DENIED synchronously and "VFS resource read
    // failed" asynchronously, and the synchronous row went on showing PASS from
    // an older verdict while every fresh call was refused. `self` is what the
    // DVPL probe already uses and what resolves against this mod's own mounts.
    CopyText(descriptor.uri, sizeof(descriptor.uri),
             "mod://self/validation-probe.txt");
    descriptor.expected_type = WOTBMOD_V3_RESOURCE_TEXT;
    descriptor.max_bytes = 4096u;
    WotbModV3ResourceHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = asynchronous
        ? g_resources->load_async_ex(g_mod, &descriptor, &handle)
        : g_resources->load(g_mod, &descriptor, &handle);
    if (result != WOTBMOD_V3_OK) return result;
    if (asynchronous) {
        if (g_async_resource != WOTBMOD_V3_INVALID_HANDLE) {
            g_resources->release(g_mod, g_async_resource);
        }
        g_async_resource = handle;
        return result;
    }
    return g_resources->release(g_mod, handle);
}

// Parses the YAML fixture and hands back the LIVE document handle - the caller
// owns it and must release it. Split out of RunPortableYaml so the owner-cleanup
// test can watch a real runtime-owned object go alive -> dead without inventing
// a second fixture that could drift from this one.
WotbModV3Result RunPortableYamlDocument(WotbModV3Handle* out_document) {
    if (!g_yaml) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (!out_document) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_document = WOTBMOD_V3_INVALID_HANDLE;
    const char yaml_text[] = "validation:\n  enabled: true\n  count: 3\n";
    WotbModV3YamlLimits limits = {};
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_YAML_VERSION);
    limits.max_bytes = sizeof(yaml_text);
    limits.max_nodes = 32u;
    limits.max_depth = 8u;
    WotbModV3ConstBuffer yaml_buffer = {};
    WOTBMOD_V3_INIT_STRUCT(yaml_buffer, WOTBMOD_V3_ABI_VERSION);
    yaml_buffer.data = yaml_text;
    yaml_buffer.size = static_cast<uint32_t>(sizeof(yaml_text) - 1u);
    WotbModV3Handle document = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = g_yaml->parse(
        g_mod, &yaml_buffer, &limits, &document);
    if (result != WOTBMOD_V3_OK) return result;
    *out_document = document;
    return WOTBMOD_V3_OK;
}

WotbModV3Result RunPortableYaml() {
    WotbModV3Handle document = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = RunPortableYamlDocument(&document);
    if (result != WOTBMOD_V3_OK) return result;
    WotbModV3YamlNodeId root = 0u;
    result = g_yaml->get_root(g_mod, document, &root);
    if (g_handles) g_handles->release(g_mod, document);
    return result;
}

// Records the reason the runtime last swept this owner. Nothing in an enabled
// mod can make the runtime call this - disable, unload and shutdown are the
// only reasons - so the owner-cleanup test asserts only that registration and
// unregistration are honoured, and this stays available for the day a live
// unload exists to observe.
std::atomic<uint32_t> g_owner_cleanup_reason{UINT32_MAX};

void WOTBMOD_V3_CALL OwnerCleanupProbeCallback(
    WotbModV3Handle,
    uint32_t reason,
    void*) noexcept {
    g_owner_cleanup_reason.store(reason, std::memory_order_relaxed);
}

WotbModV3Result RunPortableArchive() {
    if (!g_archive) return WOTBMOD_V3_E_NOT_SUPPORTED;
    std::string directory;
    JoinPath(g_data_directory, "archive_probe", &directory);
    if (!EnsureDirectory(directory)) return WOTBMOD_V3_E_IO;
    std::string file;
    JoinPath(directory, "entry.txt", &file);
    const std::string content = "portable archive validation\n";
    if (!WriteFileExact(file, content.data(), content.size(), false)) {
        return WOTBMOD_V3_E_IO;
    }
    WotbModV3ArchiveLimits limits = {};
    WOTBMOD_V3_INIT_STRUCT(limits, WOTBMOD_V3_ARCHIVE_VERSION);
    limits.max_depth = 4u;
    limits.max_files = 16u;
    limits.max_total_unpacked_bytes = 4096u;
    limits.max_single_file_bytes = 4096u;
    WotbModV3ArchiveHandle archive = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = g_archive->open_directory(
        g_mod, directory.c_str(), &limits, &archive);
    if (result != WOTBMOD_V3_OK) return result;
    uint32_t count = 0u;
    result = g_archive->get_entry_count(g_mod, archive, &count);
    if (g_handles) g_handles->release(g_mod, archive);
    return result == WOTBMOD_V3_OK && count == 1u
        ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
}

/*
 * DVPL, WITH A CONTAINER THIS MOD BUILT ITSELF.
 *
 * `loaders.dvpl` shipped as SUPPORTED with SafeTest::None behind it - a status
 * with nothing to run, so F6 did nothing and `calls` stayed 0 for every live
 * session. This is the test that was missing.
 *
 * The fixture is written here rather than taken from the game so the test owns
 * every byte it asserts on: a DVPL is a payload followed by a 20-byte footer of
 * <uncompressed, compressed, crc32, compression, "DVPL">, and type 0 stores the
 * payload verbatim. That is enough to exercise the whole container path -
 * footer parse, size agreement, CRC verification, byte-exact output - without
 * depending on a game file this mod has no permission to read.
 *
 * THE CORRUPTED SECOND HALF IS THE POINT. A happy path alone would pass just as
 * well against a decoder that ignored the checksum entirely, which is precisely
 * the kind of hollow green this whole exercise exists to stop. So one payload
 * byte is flipped and the decoder is required to answer E_HASH_MISMATCH: the
 * test fails both when DVPL stops working and when it stops checking.
 * THE URI USES `mod://self/`, NOT THE PACKAGE ID. `wotb.resources` resolves a
 * `mod://<package-id>/` URI through the VFS mount name and accepts it, but
 * `wotb.loaders` runs the stricter authority check in ParseVfsUri, which
 * compares the authority against ModNamespace(mod) and answers
 * E_PERMISSION_DENIED - "URI authority does not belong to the calling mod" -
 * for anything else. Measured live: the spelling this mod already uses for
 * resources.load was refused here. `self` is mapped to the caller's own
 * namespace by that same check, so it is both correct and id-agnostic.
 */
uint32_t ValidationCrc32(const uint8_t* bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - static_cast<uint32_t>(crc & 1u);
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void AppendLe32(std::string* out, uint32_t value) {
    out->push_back(static_cast<char>(value & 0xFFu));
    out->push_back(static_cast<char>((value >> 8u) & 0xFFu));
    out->push_back(static_cast<char>((value >> 16u) & 0xFFu));
    out->push_back(static_cast<char>((value >> 24u) & 0xFFu));
}

std::string BuildUncompressedDvpl(const std::string& payload) {
    std::string container = payload;
    const uint32_t size = static_cast<uint32_t>(payload.size());
    AppendLe32(&container, size);
    AppendLe32(&container, size);
    AppendLe32(
        &container,
        ValidationCrc32(
            reinterpret_cast<const uint8_t*>(payload.data()),
            payload.size()));
    AppendLe32(&container, 0u);
    container.append("DVPL", 4u);
    return container;
}

/*
 * THE NATIVE SOUND-EVENT BRIDGE, EXERCISED RATHER THAN ASSUMED.
 *
 * `audio.native_sound_event` was declared native with a live backend and had
 * SafeTest::None behind it, so nothing ever called it. The bridge is real -
 * SoundEventCreate marshals through the host as "sound_event_create" and the
 * loader answers it with CreateSoundEvent against the client sound engine.
 *
 * THE NAME IS A REAL ONE, taken from the client's own Data/sounds.yaml:
 * sounds_general.BUTTON_CHECKBOX maps to "GUI/buttons/checkbox". Inventing a
 * name would only have proved that the bridge answers, not that it resolves;
 * a shipped UI event proves both.
 *
 * NOTHING IS PLAYED. Creating a sound event does not trigger it, and this test
 * deliberately never calls sound_event_trigger: a validation pass should not
 * make noise on the player's machine, and playback adds nothing the round trip
 * does not already establish. What is asserted is that the handle comes back,
 * that the engine round-trips the name it was given, and that destroy accepts
 * it - create, read, release, which is the whole lifecycle this capability
 * claims.
 */
WotbModV3Result RunNativeSoundEvent(std::string* out_description) {
    if (!g_audio || !g_audio->sound_event_create ||
        !g_audio->sound_event_get_name ||
        !g_audio->sound_event_destroy) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const char* const event_name = "GUI/buttons/checkbox";
    WotbModV3AudioHandle event = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result =
        g_audio->sound_event_create(g_mod, event_name, &event);
    if (result != WOTBMOD_V3_OK) {
        if (out_description) {
            *out_description =
                std::string("sound_event_create(\"") + event_name +
                "\") -> " + LastErrorMessage();
        }
        return result;
    }
    if (event == WOTBMOD_V3_INVALID_HANDLE) {
        if (out_description) {
            *out_description = "create returned OK with an invalid handle";
        }
        return WOTBMOD_V3_E_PLATFORM;
    }

    char name[128] = {};
    uint32_t name_size = static_cast<uint32_t>(sizeof(name));
    const WotbModV3Result named =
        g_audio->sound_event_get_name(g_mod, event, name, &name_size);

    char bus[64] = {};
    uint32_t bus_size = static_cast<uint32_t>(sizeof(bus));
    const WotbModV3Result bussed = g_audio->sound_event_get_bus
        ? g_audio->sound_event_get_bus(g_mod, event, bus, &bus_size)
        : WOTBMOD_V3_E_NOT_SUPPORTED;

    const WotbModV3Result destroyed =
        g_audio->sound_event_destroy(g_mod, event);

    if (named != WOTBMOD_V3_OK) {
        if (out_description) {
            *out_description = "created, but get_name failed";
        }
        return named;
    }
    if (std::strcmp(name, event_name) != 0) {
        if (out_description) {
            *out_description =
                std::string("name round-tripped as \"") + name +
                "\", expected \"" + event_name + "\"";
        }
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (destroyed != WOTBMOD_V3_OK) {
        if (out_description) *out_description = "destroy failed";
        return destroyed;
    }
    if (out_description) {
        *out_description =
            std::string("created \"") + name + "\" (bus=" +
            (bussed == WOTBMOD_V3_OK ? bus : "NOT_EXPOSED") +
            "), name round-tripped, destroyed; never triggered";
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result RunPortableDvpl(std::string* out_description) {
    if (!g_loaders || !EnsureProbeMount()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    std::string root;
    JoinPath(g_data_directory, "probe_resources", &root);
    const std::string payload =
        "wotbmod.native_validation dvpl container probe\n";

    std::string good_path;
    JoinPath(root, "validation-probe.dvpl", &good_path);
    const std::string good = BuildUncompressedDvpl(payload);
    if (!WriteFileExact(good_path, good.data(), good.size(), false)) {
        return WOTBMOD_V3_E_IO;
    }

    std::vector<char> storage(payload.size() + 64u);
    WotbModV3Buffer buffer = {};
    WOTBMOD_V3_INIT_STRUCT(buffer, WOTBMOD_V3_ABI_VERSION);
    buffer.data = storage.data();
    buffer.capacity = static_cast<uint32_t>(storage.size());
    WotbModV3Result result = g_loaders->unpack_dvpl(
        g_mod,
        "mod://self/validation-probe.dvpl",
        &buffer);
    if (result != WOTBMOD_V3_OK) {
        if (out_description) *out_description = LastErrorMessage();
        return result;
    }
    if (buffer.size != payload.size() ||
        std::memcmp(buffer.data, payload.data(), payload.size()) != 0) {
        if (out_description) {
            *out_description =
                "unpacked " + std::to_string(buffer.size) +
                " bytes, expected " + std::to_string(payload.size());
        }
        return WOTBMOD_V3_E_PLATFORM;
    }

    std::string corrupt = good;
    corrupt[0] = static_cast<char>(corrupt[0] ^ 0x40);
    std::string bad_path;
    JoinPath(root, "validation-probe-corrupt.dvpl", &bad_path);
    if (!WriteFileExact(bad_path, corrupt.data(), corrupt.size(), false)) {
        return WOTBMOD_V3_E_IO;
    }
    WotbModV3Buffer rejected = {};
    WOTBMOD_V3_INIT_STRUCT(rejected, WOTBMOD_V3_ABI_VERSION);
    rejected.data = storage.data();
    rejected.capacity = static_cast<uint32_t>(storage.size());
    const WotbModV3Result refused = g_loaders->unpack_dvpl(
        g_mod,
        "mod://self/validation-probe-corrupt.dvpl",
        &rejected);
    if (refused != WOTBMOD_V3_E_HASH_MISMATCH) {
        if (out_description) {
            *out_description =
                "a corrupted container returned " +
                std::to_string(static_cast<int>(refused)) +
                ", expected E_HASH_MISMATCH";
        }
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (out_description) {
        *out_description =
            "unpacked " + std::to_string(buffer.size) +
            " bytes byte-exact; corrupted container refused with "
            "E_HASH_MISMATCH";
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result RunInspector(
    WotbModV3DevtoolsInspectFn inspector,
    const char* selector,
    std::string* out_text = nullptr) {
    if (!inspector) return WOTBMOD_V3_E_NOT_SUPPORTED;
    uint32_t size = 0u;
    const WotbModV3Result result = inspector(g_mod, selector, nullptr, &size);
    if (result == WOTBMOD_V3_E_BUFFER_TOO_SMALL && size > 0u && size < 65536u) {
        std::vector<char> buffer(size);
        const WotbModV3Result filled =
            inspector(g_mod, selector, buffer.data(), &size);
        if (filled == WOTBMOD_V3_OK && out_text) {
            out_text->assign(buffer.data(), strnlen_s(buffer.data(), size));
        }
        return filled;
    }
    return result;
}

// Pulls one `"key":<number>` out of an inspector's JSON so a verdict line can
// quote what the client actually reported instead of "returned data".  Not a
// parser and not pretending to be one - the inspectors emit these fields
// themselves, and anything unexpected simply leaves the count out.
bool ReadJsonUnsigned(
    const std::string& json,
    const char* key,
    unsigned long long* out_value) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return false;
    size_t cursor = at + needle.size();
    if (cursor >= json.size() || !std::isdigit(
            static_cast<unsigned char>(json[cursor]))) {
        return false;
    }
    unsigned long long value = 0u;
    while (cursor < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[cursor]))) {
        value = value * 10u + static_cast<unsigned long long>(
            json[cursor] - '0');
        ++cursor;
    }
    *out_value = value;
    return true;
}

// THE SCENE INSPECTORS MUST NOT RUN ON THE RENDER THREAD.
//
// Measured on 11.19.0.834: both answered WOTBMOD_V3_E_TIMEOUT after exactly two
// seconds. The reason is not the inspector - it is where the panel calls from.
// PollPanelKeys runs inside the render/present callback, and the DAVA scene
// routes marshal onto the engine's main thread, which at that moment is waiting
// for present to return. The two threads wait on each other and the marshal's
// own timeout is what breaks it. Nothing was wrong with the data; the call site
// was.
//
// So the work is scheduled onto the loader-owned main queue, exactly like
// reload.callback_deferred already does, and the verdict is recorded from
// there. RunSafeTest still records the DISPATCH result first, so a row that
// cannot even be scheduled says so instead of hanging on "pending" forever.
void WOTBMOD_V3_CALL DeferredInspectorCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (index >= kCapabilityCount) return;
        const bool scene =
            kCapabilities[index].safe_test == SafeTest::SceneInspector;
        WotbModV3DevtoolsInspectFn inspector = nullptr;
        if (g_devtools) {
            inspector = scene ? g_devtools->inspect_scene
                              : g_devtools->inspect_material;
        }
        std::string json;
        const WotbModV3Result result = RunInspector(inspector, "*", &json);
        std::string description;
        if (result == WOTBMOD_V3_E_NOT_SUPPORTED) {
            description = scene
                ? "read-only native Scene enumeration is unavailable"
                : "read-only native Material enumeration is unavailable";
        } else if (result != WOTBMOD_V3_OK) {
            description = std::string(scene ? "Scene" : "Material") +
                " inspector refused: " + ResultName(result) + " " +
                LastErrorMessage();
        } else if (scene) {
            unsigned long long count = 0u;
            description = ReadJsonUnsigned(json, "count", &count)
                ? "live scene enumerated: " + std::to_string(count) + " nodes"
                : "scene inspector returned " +
                  std::to_string(json.size()) + " bytes of JSON";
        } else {
            description = "live material probed on scene node " +
                std::to_string(json.size()) + " bytes of JSON";
            const size_t node_at = json.find("\"node\":\"");
            if (node_at != std::string::npos) {
                const size_t begin = node_at + 8u;
                const size_t end = json.find('"', begin);
                if (end != std::string::npos) {
                    description = "live material probed on scene node \"" +
                        json.substr(begin, end - begin) + "\"";
                }
            }
        }
        RecordResult(index, result, description);
        QueueLiveLog(
            index,
            result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
            "validation.safe_test",
            description);
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "DeferredInspectorCallback contained an exception");
    }
}

// Breadth-first over the active screen, asking every control for the
// engine's own text; runs on the DAVA main thread (see the UiLiveText case).
// Each wrapper is released as soon as its children are queued, so the live
// handle count stays at the frontier, not the whole screen.
WotbModV3Result RunUiLiveTextWalk(std::string* out_description) {
    WotbModV3Result result = WOTBMOD_V3_OK;
    std::string description;
    WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
    result = g_ui && g_ui_read && g_ui_read->control_get_live_text
        ? g_ui->get_active_screen(g_mod, &screen)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
    uint32_t visited = 0u;
    uint32_t with_text = 0u;
    std::string samples;
    std::string first_error;
    if (result == WOTBMOD_V3_OK) {
        // The whole screen goes to LIVE_UI_TREE.jsonl, one control per
        // line with its parent's index: this is the inventory the HUD
        // backend is mapped from (control names, rects, live text).
        if (!g_ui_tree_path.empty()) DeleteFileA(g_ui_tree_path.c_str());
        std::vector<WotbModV3UiHandle> queue;
        std::vector<size_t> parents;
        queue.push_back(screen);
        parents.push_back(static_cast<size_t>(-1));
        for (size_t i = 0u; i < queue.size() && visited < 4096u; ++i) {
            const WotbModV3UiHandle node = queue[i];
            ++visited;
            char text[256] = {};
            uint32_t size = sizeof(text);
            const WotbModV3Result read =
                g_ui_read->control_get_live_text(g_mod, node, text, &size);
            if (read == WOTBMOD_V3_OK) {
                ++with_text;
                if (samples.size() < 160u) {
                    samples += (samples.empty() ? "" : " | ") + std::string(text);
                }
            } else if (read != WOTBMOD_V3_E_NOT_FOUND && first_error.empty()) {
                first_error = std::string(ResultName(read)) + ": " + LastErrorMessage();
            }
            WotbModV3UiControlSnapshot snapshot = {};
            WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
            const bool have_snapshot =
                g_ui->control_get_snapshot(g_mod, node, &snapshot) == WOTBMOD_V3_OK;
            if (!g_ui_tree_path.empty()) {
                char rect[96] = {};
                _snprintf_s(rect, sizeof(rect), _TRUNCATE, "[%.1f,%.1f,%.1f,%.1f]",
                            snapshot.geometry.x, snapshot.geometry.y,
                            snapshot.geometry.width, snapshot.geometry.height);
                const std::string line =
                    std::string("{") + "\"" + "index" + "\"" + ":" + std::to_string(i) +
                    "," + "\"" + "parent" + "\"" + ":" + (parents[i] == static_cast<size_t>(-1) ? std::string("-1") : std::to_string(parents[i])) +
                    "," + "\"" + "id" + "\"" + ":" + "\"" + JsonEscape(have_snapshot ? snapshot.id : "") + "\"" +
                    "," + "\"" + "type" + "\"" + ":" + std::to_string(snapshot.type) +
                    "," + "\"" + "flags" + "\"" + ":" + std::to_string(snapshot.flags) +
                    "," + "\"" + "rect" + "\"" + ":" + rect +
                    "," + "\"" + "children" + "\"" + ":" + std::to_string(snapshot.child_count) +
                    "," + "\"" + "text" + "\"" + ":" + (read == WOTBMOD_V3_OK ? "\"" + JsonEscape(text) + "\"" : std::string("null")) +
                    "}" + "\n";
                QueueLogLine(g_ui_tree_path, line);
            }
            uint32_t count = 0u;
            if (g_ui->v2.control_get_child_count(g_mod, node, &count) == WOTBMOD_V3_OK) {
                for (uint32_t c = 0u; c < count && queue.size() < 8192u; ++c) {
                    WotbModV3UiHandle child = WOTBMOD_V3_INVALID_HANDLE;
                    if (g_ui->v2.control_get_child_at(g_mod, node, c, &child) == WOTBMOD_V3_OK) {
                        queue.push_back(child);
                        parents.push_back(i);
                    }
                }
            }
            g_ui->v2.control_destroy(g_mod, node);
            queue[i] = WOTBMOD_V3_INVALID_HANDLE;
        }
        for (WotbModV3UiHandle visited_handle : queue) {
            if (visited_handle != WOTBMOD_V3_INVALID_HANDLE) {
                g_ui->v2.control_destroy(g_mod, visited_handle);
            }
        }
        if (with_text == 0u) result = WOTBMOD_V3_E_NOT_FOUND;
    }
    description = "visited=" + std::to_string(visited) +
        " with_text=" + std::to_string(with_text) +
        " tree=LIVE_UI_TREE.jsonl" +
        (samples.empty() ? std::string() : " samples=" + samples) +
        (first_error.empty() ? std::string() : " first_error=" + first_error) +
        (result == WOTBMOD_V3_E_NOT_SUPPORTED ? " (ui.read live text unavailable)" : "");
    *out_description = description;
    return result;
}

void WOTBMOD_V3_CALL DeferredUiLiveTextCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (index >= kCapabilityCount) return;
        std::string description;
        const WotbModV3Result result = RunUiLiveTextWalk(&description);
        RecordResult(index, result, description);
        QueueLiveLog(
            index,
            result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
            "validation.safe_test",
            description);
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "DeferredUiLiveTextCallback contained an exception");
    }
}

// A solid-colour RGBA PNG (stored deflate blocks, no compression): the
// texture the HUD overlay slots are tested with.
uint32_t Crc32Png(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0u; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

bool WritePngSolid(
    const std::string& path,
    uint32_t width,
    uint32_t height,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (width == 0u || height == 0u || width > 256u || height > 256u) return false;
    std::vector<uint8_t> raw;
    raw.reserve(height * (1u + width * 4u));
    for (uint32_t y = 0u; y < height; ++y) {
        raw.push_back(0u);
        for (uint32_t x = 0u; x < width; ++x) {
            raw.push_back(r); raw.push_back(g); raw.push_back(b); raw.push_back(a);
        }
    }
    // zlib stream: header, one stored block per 65535 bytes, adler32
    std::vector<uint8_t> zlib = {0x78u, 0x01u};
    size_t offset = 0u;
    while (offset < raw.size()) {
        const size_t chunk = std::min<size_t>(65535u, raw.size() - offset);
        const bool last = offset + chunk >= raw.size();
        zlib.push_back(last ? 1u : 0u);
        zlib.push_back(static_cast<uint8_t>(chunk & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((chunk >> 8) & 0xFFu));
        zlib.push_back(static_cast<uint8_t>(~chunk & 0xFFu));
        zlib.push_back(static_cast<uint8_t>((~chunk >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + chunk);
        offset += chunk;
    }
    uint32_t s1 = 1u, s2 = 0u;
    for (const uint8_t value : raw) { s1 = (s1 + value) % 65521u; s2 = (s2 + s1) % 65521u; }
    const uint32_t adler = (s2 << 16) | s1;
    for (int shift = 24; shift >= 0; shift -= 8) zlib.push_back(static_cast<uint8_t>((adler >> shift) & 0xFFu));
    std::vector<uint8_t> png = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    auto chunkOut = [&](const char* type, const std::vector<uint8_t>& body) {
        const uint32_t length = static_cast<uint32_t>(body.size());
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(static_cast<uint8_t>((length >> shift) & 0xFFu));
        std::vector<uint8_t> typed(type, type + 4);
        typed.insert(typed.end(), body.begin(), body.end());
        png.insert(png.end(), typed.begin(), typed.end());
        const uint32_t crc = Crc32Png(typed.data(), typed.size()) ^ 0xFFFFFFFFu;
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(static_cast<uint8_t>((crc >> shift) & 0xFFu));
    };
    std::vector<uint8_t> ihdr;
    for (int shift = 24; shift >= 0; shift -= 8) ihdr.push_back(static_cast<uint8_t>((width >> shift) & 0xFFu));
    for (int shift = 24; shift >= 0; shift -= 8) ihdr.push_back(static_cast<uint8_t>((height >> shift) & 0xFFu));
    ihdr.push_back(8u);  // bit depth
    ihdr.push_back(6u);  // RGBA
    ihdr.push_back(0u); ihdr.push_back(0u); ihdr.push_back(0u);
    chunkOut("IHDR", ihdr);
    chunkOut("IDAT", zlib);
    chunkOut("IEND", {});
    return WriteFileExact(path, png.data(), png.size(), false);
}

// hud.stock_controls: every gameplay.hud slot that drives a stock battle
// control. Runs on the DAVA main thread like the UI walks, because the
// resolve is a name search over the live screen tree, and because the slots
// apply immediately only there (elsewhere they land on the next frame).
// Outside BATTLE/TRAINING the access check refuses and the row records that.
WotbModV3Result RunHudStockTest(std::string* out_description) {
    if (!g_hud) {
        g_hud = QueryApi<WotbModV3GameplayHudApiV1>(
            WOTBMOD_V3_IFACE_GAMEPLAY_HUD, WOTBMOD_V3_GAMEPLAY_HUD_VERSION);
    }
    if (!g_hud) {
        *out_description = "gameplay.hud interface unavailable: " + LastErrorMessage();
        Log(WOTBMOD_V3_LOG_WARNING, ("hud.stock_controls judged: " + *out_description).c_str());
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    struct Step {
        const char* name;
        WotbModV3Result result;
        WotbModV3Result expected;
        std::string error;
    };
    // Each step's error text is captured right after the call: the mod's
    // last error is overwritten by later calls (and by the UI walk that runs
    // on the same main queue), so a single read at the end named the wrong
    // failure (live 2026-09-05: "the control has no text component").
    Step steps[32];
    size_t count = 0u;
    auto run = [&](const char* name, WotbModV3Result value, WotbModV3Result expected) {
        Step& step = steps[count++];
        step.name = name;
        step.result = value;
        step.expected = expected;
        if (value != expected) step.error = LastErrorMessage();
    };
    // Assets for the texture and sound slots live in the probe mount:
    // a solid magenta 24x24 PNG and the validation tone.
    std::string pngUri;
    std::string toneUri;
    if (EnsureProbeMount()) {
        std::string root;
        std::string png;
        std::string wave;
        JoinPath(g_data_directory, "probe_resources", &root);
        JoinPath(root, "hud-overlay.png", &png);
        JoinPath(root, "validation-tone.wav", &wave);
        if (WritePngSolid(png, 24u, 24u, 0xFFu, 0x30u, 0xFFu, 0xFFu)) {
            pngUri = "mod://self/hud-overlay.png";
        }
        if (WriteWaveTone(wave)) {
            toneUri = "mod://self/validation-tone.wav";
        }
    }
    // Geometry and visibility (the original seven), then the new slots. The
    // tweaks stay on screen; hud.stock_reset (or OnDisable) puts them back.
    run("minimap.size", g_hud->minimap_set_size(g_mod, 1.25f), WOTBMOD_V3_OK);
    run("sixth_sense.scale", g_hud->sixth_sense_set_scale(g_mod, 1.5f), WOTBMOD_V3_OK);
    run("reticle.size", g_hud->reticle_set_size(g_mod, 1.5f), WOTBMOD_V3_OK);
    run("damagelog.hide", g_hud->damagelog_set_enabled(g_mod, 0u), WOTBMOD_V3_OK);
    run("damagelog.show", g_hud->damagelog_set_enabled(g_mod, 1u), WOTBMOD_V3_OK);
    run("session_stats.hide", g_hud->session_stats_set_enabled(g_mod, 0u), WOTBMOD_V3_OK);
    run("session_stats.show", g_hud->session_stats_set_enabled(g_mod, 1u), WOTBMOD_V3_OK);
    run("minimap.opacity", g_hud->minimap_set_opacity(g_mod, 0.5f), WOTBMOD_V3_OK);
    run("reticle.color", g_hud->reticle_set_color(g_mod, 0xFF40FFFFu), WOTBMOD_V3_OK);
    run("reticle.dispersion.off", g_hud->reticle_set_dispersion_circle(g_mod, 0u), WOTBMOD_V3_OK);
    run("reticle.reload.off", g_hud->reticle_set_reloading_indicator(g_mod, 0u), WOTBMOD_V3_OK);
    run("hit.style.compact", g_hud->hit_indicator_set_style(g_mod, WOTBMOD_V3_HIT_STYLE_COMPACT), WOTBMOD_V3_OK);
    run("hit.color.hit", g_hud->hit_indicator_set_color_hit(g_mod, 0x00FF00FFu), WOTBMOD_V3_OK);
    run("hit.color.ricochet", g_hud->hit_indicator_set_color_ricochet(g_mod, 0x00A0FFFFu), WOTBMOD_V3_OK);
    run("hit.color.pen", g_hud->hit_indicator_set_color_pen(g_mod, 0xFF0000FFu), WOTBMOD_V3_E_NOT_SUPPORTED);
    run("hit.style.directional", g_hud->hit_indicator_set_style(g_mod, WOTBMOD_V3_HIT_STYLE_DIRECTIONAL), WOTBMOD_V3_E_NOT_SUPPORTED);
    run("damagelog.max_entries", g_hud->damagelog_set_max_entries(g_mod, 2u), WOTBMOD_V3_OK);
    run("damagelog.filter_own", g_hud->damagelog_set_filter_own(g_mod, 1u), WOTBMOD_V3_OK);
    run("damagelog.show_blocked", g_hud->damagelog_set_show_blocked(g_mod, 0u), WOTBMOD_V3_E_NOT_SUPPORTED);
    run("session_stats.fields", g_hud->session_stats_set_fields(g_mod, WOTBMOD_V3_STAT_DAMAGE), WOTBMOD_V3_OK);
    run("session_stats.fields.kills", g_hud->session_stats_set_fields(g_mod, WOTBMOD_V3_STAT_KILLS), WOTBMOD_V3_E_NOT_SUPPORTED);
    run("sixth_sense.delay", g_hud->sixth_sense_set_delay_ms(g_mod, 4000.0f), WOTBMOD_V3_OK);
    run("sixth_sense.sound", toneUri.empty() ? WOTBMOD_V3_E_IO
        : g_hud->sixth_sense_set_sound(g_mod, toneUri.c_str()), WOTBMOD_V3_OK);
    run("sixth_sense.texture", pngUri.empty() ? WOTBMOD_V3_E_IO
        : g_hud->sixth_sense_set_texture(g_mod, pngUri.c_str()), WOTBMOD_V3_OK);
    run("reticle.texture", pngUri.empty() ? WOTBMOD_V3_E_IO
        : g_hud->reticle_set_texture(g_mod, pngUri.c_str()), WOTBMOD_V3_OK);
    run("reticle.sniper_texture", pngUri.empty() ? WOTBMOD_V3_E_IO
        : g_hud->reticle_set_sniper_texture(g_mod, pngUri.c_str()), WOTBMOD_V3_OK);
    run("minimap.last_known", g_hud->minimap_set_show_last_known(g_mod, 1u), WOTBMOD_V3_E_NOT_SUPPORTED);
    WotbModV3Result result = WOTBMOD_V3_OK;
    std::string description;
    for (size_t i = 0u; i < count; ++i) {
        const Step& step = steps[i];
        if (!description.empty()) description += " ";
        description += step.name;
        description += "=";
        description += ResultName(step.result);
        if (step.result == step.expected) {
            if (step.expected != WOTBMOD_V3_OK) description += "(expected)";
            continue;
        }
        description += "(" + step.error + ")";
        if (result == WOTBMOD_V3_OK) {
            result = step.result == WOTBMOD_V3_OK ? WOTBMOD_V3_E_PLATFORM : step.result;
        }
    }
    // The loader keeps a line to ~1400 bytes; 27 steps with error texts run
    // past that, so the verdict goes out in numbered pieces.
    {
        const std::string head = "hud.stock_controls judged: " +
            std::string(ResultName(result)) + " ";
        const size_t pieceSize = 900u;
        const size_t pieces = std::max<size_t>(1u, (description.size() + pieceSize - 1u) / pieceSize);
        for (size_t i = 0u; i < pieces; ++i) {
            const std::string piece = description.substr(i * pieceSize, pieceSize);
            Log(result == WOTBMOD_V3_OK ? WOTBMOD_V3_LOG_INFO : WOTBMOD_V3_LOG_WARNING,
                (head + "[" + std::to_string(i + 1u) + "/" + std::to_string(pieces) + "] " + piece).c_str());
        }
    }
    *out_description = description;
    return result;
}

void WOTBMOD_V3_CALL DeferredHudResetCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (index >= kCapabilityCount) return;
        const WotbModV3Result result = g_hud
            ? g_hud->reset(g_mod) : WOTBMOD_V3_E_NOT_SUPPORTED;
        std::string description = std::string("reset=") + ResultName(result);
        if (result != WOTBMOD_V3_OK) description += " " + LastErrorMessage();
        Log(result == WOTBMOD_V3_OK ? WOTBMOD_V3_LOG_INFO : WOTBMOD_V3_LOG_WARNING,
            ("hud.stock_reset judged: " + description).c_str());
        RecordResult(index, result, description);
        QueueLiveLog(
            index,
            result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
            "validation.safe_test",
            description);
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "DeferredHudResetCallback contained an exception");
    }
}

void WOTBMOD_V3_CALL DeferredHudStockCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (index >= kCapabilityCount) return;
        std::string description;
        const WotbModV3Result result = RunHudStockTest(&description);
        RecordResult(index, result, description);
        QueueLiveLog(
            index,
            result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
            "validation.safe_test",
            description);
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "DeferredHudStockCallback contained an exception");
    }
}

// ges.publish_echo: the loader replays the engine's dispatch loop, which is
// MAIN-thread only (live 11.20.0.887, 2026-09-04: E_WRONG_THREAD from the
// render-thread sweep). Publish from the main queue and judge there.
void WOTBMOD_V3_CALL DeferredGesEchoCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (index >= kCapabilityCount || !g_ges) return;
        // Avatar::CameraModeChanged {mode, flag}: the last mode the bus
        // reported, so the client sees a no-op change.
        struct {
            int32_t mode;
            uint8_t flag;
            uint8_t pad[3];
        } payload = {g_ges_last_camera_mode.load(), 0u, {0u, 0u, 0u}};
        const WotbModV3Result result = g_ges->publish(
            g_mod, "Avatar::CameraModeChanged", &payload, sizeof(payload),
            WOTBMOD_V3_GES_PUBLISH_ECHO);
        size_t distinct = 0u;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            distinct = g_ges_types_seen.size();
        }
        const std::string description =
            "events=" + std::to_string(g_ges_events.load()) +
            " distinct=" + std::to_string(distinct) +
            " publish=" + (result == WOTBMOD_V3_OK ? std::string("OK (main thread)")
                                                  : std::string(ResultName(result)) + " " + LastErrorMessage()) +
            " echo_seen=" + std::to_string(g_ges_echo_seen.load());
        RecordResult(index, result, description);
        QueueLiveLog(
            index,
            result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
            "validation.safe_test",
            description);
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "DeferredGesEchoCallback contained an exception");
    }
}

void WOTBMOD_V3_CALL DeferredReloadCallback(
    WotbModV3Handle,
    void* user_data) noexcept {
    try {
        const size_t index = static_cast<size_t>(
            reinterpret_cast<uintptr_t>(user_data));
        if (g_lifecycle) MarkReloadPending(index);
        const WotbModV3Result result = g_lifecycle
            ? g_lifecycle->request_reload(g_mod, g_mod)
            : WOTBMOD_V3_E_NOT_SUPPORTED;
        RecordResult(
            index, result,
            result == WOTBMOD_V3_OK
                ? "reload queued from main-queue callback; module cycles at "
                  "frame boundary"
                : "deferred request_reload executed");
        ClearCrashMarker();
        SaveSnapshots();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "DeferredReloadCallback contained an exception");
    }
}

// Placeholder detour for the hook liveness probe.  create_symbol rejects a
// null detour outright, so a valid code pointer has to be supplied — but this
// function is never installed and never called: OBSERVE is not one of the two
// modes the native backend implements, so create_symbol classifies the symbol
// and returns before any trampoline is written.  That is deliberate.  The
// signature and calling convention of most published symbols are unknown, and
// a detour with the wrong convention would corrupt the stack and take the
// client down; the probe must stay read-only.
void WOTBMOD_V3_CALL HookProbeUnusedDetour() noexcept {
}

// OBSERVE callbacks for the targets the loader describes (the 22 functions it
// detours itself; see docs/HOOK_MODES_RU.md for the signature table). Each
// no-op has exactly the parameter count of its target so the callee-cleans
// thiscall return leaves the stack where the loader's typed call expects it;
// scalar arguments narrower than a pointer still occupy one 4-byte slot on
// x86, which is why every parameter is spelled as a pointer-sized value. The
// probe attaches one of these, and the loader may genuinely call it while it
// is attached, so they must stay empty.
void __fastcall ProbeNoop0(void*, void*) noexcept {}
void __fastcall ProbeNoop1(void*, void*, const void*) noexcept {}
void __fastcall ProbeNoop2(void*, void*, const void*, const void*) noexcept {}
void __fastcall ProbeNoop3(
    void*, void*, const void*, const void*, const void*) noexcept {}
void __fastcall ProbeNoop7(
    void*, void*, const void*, const void*, const void*, const void*,
    const void*, const void*, const void*) noexcept {}
void __fastcall ProbeNoop9(
    void*, void*, const void*, const void*, const void*, const void*,
    const void*, const void*, const void*, const void*,
    const void*) noexcept {}
void* __cdecl ProbeNoopFileCreate(const void*, uint32_t) noexcept {
    return nullptr;
}

struct ProbeCallback {
    const char* symbol;
    void* callback;
};

const ProbeCallback kProbeCallbacks[] = {
    {"Vehicle::onEnterWorld", reinterpret_cast<void*>(&ProbeNoop0)},
    {"Vehicle::onLeaveWorld", reinterpret_cast<void*>(&ProbeNoop0)},
    {"Client::Initialize", reinterpret_cast<void*>(&ProbeNoop0)},
    {"BWEntity::~BWEntity", reinterpret_cast<void*>(&ProbeNoop0)},
    {"BWEntity::dtor", reinterpret_cast<void*>(&ProbeNoop0)},
    {"DAVA::Scene::Draw", reinterpret_cast<void*>(&ProbeNoop0)},
    {"DAVA::Scene::Activate", reinterpret_cast<void*>(&ProbeNoop0)},
    {"DAVA::Scene::Deactivate", reinterpret_cast<void*>(&ProbeNoop0)},
    {"Vehicle::showShooting", reinterpret_cast<void*>(&ProbeNoop1)},
    {"Vehicle::set_health", reinterpret_cast<void*>(&ProbeNoop1)},
    {"DAVA::UIControl::SystemInput", reinterpret_cast<void*>(&ProbeNoop1)},
    {"GES::Avatar::CameraModeChanged", reinterpret_cast<void*>(&ProbeNoop1)},
    {"CameraModeChanged", reinterpret_cast<void*>(&ProbeNoop1)},
    {"GameCamera::~GameCamera", reinterpret_cast<void*>(&ProbeNoop1)},
    {"GameCamera::dtor", reinterpret_cast<void*>(&ProbeNoop1)},
    {"BWEntity::BWEntity", reinterpret_cast<void*>(&ProbeNoop1)},
    {"BWEntity::ctor", reinterpret_cast<void*>(&ProbeNoop1)},
    {"TracerManager::~TracerManager", reinterpret_cast<void*>(&ProbeNoop1)},
    {"TracerManager::dtor", reinterpret_cast<void*>(&ProbeNoop1)},
    {"Avatar::updateVehicleHealth", reinterpret_cast<void*>(&ProbeNoop2)},
    {"UIShellSelectorControl::OnCurrentAmmoChanged",
     reinterpret_cast<void*>(&ProbeNoop2)},
    {"ReloadTimer::setState", reinterpret_cast<void*>(&ProbeNoop2)},
    {"TracerManager::TracerManager", reinterpret_cast<void*>(&ProbeNoop2)},
    {"TracerManager::ctor", reinterpret_cast<void*>(&ProbeNoop2)},
    {"GameCamera::GameCamera", reinterpret_cast<void*>(&ProbeNoop3)},
    {"GameCamera::ctor", reinterpret_cast<void*>(&ProbeNoop3)},
    {"TracerManager::ShowTracer", reinterpret_cast<void*>(&ProbeNoop7)},
    {"GameSceneController::OnVehicleHitDamage",
     reinterpret_cast<void*>(&ProbeNoop9)},
    {"DAVA::File::Create", reinterpret_cast<void*>(&ProbeNoopFileCreate)},
};

const ProbeCallback* FindProbeCallback(const std::string& symbol) {
    for (const ProbeCallback& entry : kProbeCallbacks) {
        if (symbol == entry.symbol) return &entry;
    }
    return nullptr;
}

// Probes one published hook symbol for liveness and writes a human-readable
// verdict into *out_description.
//
// Returns WOTBMOD_V3_OK when the symbol resolved in this build — that is the
// probe succeeding, even though create_symbol reports it as E_NOT_SUPPORTED
// ("only AROUND and REPLACE"), because reaching the mode check at all means
// the backend already found the target.  Every other outcome keeps the code
// the runtime returned.  The raw code and message are always repeated in the
// description, so nothing is hidden by the mapping.
WotbModV3Result ProbeHookSymbol(
    size_t index,
    const char* symbol,
    std::string* out_description) {
    const std::string name(symbol ? symbol : "");
    if (name.empty()) {
        if (out_description) *out_description = "capability has no bound symbol";
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_hooks || !g_hooks->create_symbol) {
        if (out_description) {
            *out_description = name +
                ": hooks interface unavailable (needs the hooks.symbol grant)";
        }
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    WotbModV3HookCreateInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_HOOKS_VERSION);
    info.mode = WOTBMOD_V3_HOOK_OBSERVE;
    info.priority = 0;
    // Deliberately not WOTBMOD_V3_HOOK_CREATE_ALLOW_PENDING: pending would make
    // the runtime register and keep a hook record instead of answering the
    // question, and the answer is the whole point.
    info.flags = 0u;
    const ProbeCallback* described = FindProbeCallback(name);
    info.detour = described
        ? described->callback
        : reinterpret_cast<void*>(&HookProbeUnusedDetour);
    WotbModV3HookHandle hook = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result code =
        g_hooks->create_symbol(g_mod, name.c_str(), &info, &hook);
    const std::string message = LastErrorMessage();
    WotbModV3Result reported = code;
    std::string verdict;
    if (code == WOTBMOD_V3_OK && described) {
        // The loader attached the no-op as a real observer on its detour;
        // taking it off again proves the whole attach/detach cycle live.
        const WotbModV3Result removed =
            g_hooks->remove ? g_hooks->remove(g_mod, hook)
                            : WOTBMOD_V3_E_NOT_SUPPORTED;
        verdict = "OBSERVE attached and detached (" +
            std::string(ResultName(removed)) + ")";
        reported = removed;
    } else if (code == WOTBMOD_V3_E_NOT_SUPPORTED &&
               message.find("signature of this target is not described") !=
                   std::string::npos) {
        verdict = "symbol RESOLVED; no observer point on this target";
        reported = WOTBMOD_V3_OK;
    } else if (code == WOTBMOD_V3_OK) {
        // Unexpected on this runtime, and a leak if ignored: every probe would
        // add one more hook record for the mod's lifetime.
        const WotbModV3Result removed =
            g_hooks->remove ? g_hooks->remove(g_mod, hook)
                            : WOTBMOD_V3_E_NOT_SUPPORTED;
        verdict = "UNEXPECTED OK for OBSERVE; handle removed (" +
            std::string(ResultName(removed)) + ")";
        reported = WOTBMOD_V3_E_PLATFORM;
        QueueLiveLog(
            index, LiveSeverity::Warn, "validation.hook_probe_unexpected",
            name + ": create_symbol accepted an OBSERVE hook; the handle was "
                   "removed immediately, remove result " +
                ResultName(removed));
        Log(WOTBMOD_V3_LOG_WARNING,
            "hook probe for " + name +
                " returned OK for OBSERVE; handle removed");
    } else if (code == WOTBMOD_V3_E_NOT_SUPPORTED &&
               message.find("only AROUND and REPLACE") != std::string::npos) {
        verdict = "symbol RESOLVED (target present in this build; "
                  "no detour installed)";
        reported = WOTBMOD_V3_OK;
    } else if (code == WOTBMOD_V3_E_NOT_SUPPORTED &&
               message.find("not present in the reviewed binding pack") !=
                   std::string::npos) {
        verdict = "symbol did not resolve";
    } else if (code == WOTBMOD_V3_E_NOT_SUPPORTED &&
               message.find("native hook backend is unavailable") !=
                   std::string::npos) {
        verdict = "native hook backend is off "
                  "(fingerprint mismatch or not installed)";
    } else if (code == WOTBMOD_V3_E_CLIENT_MISMATCH) {
        verdict = "client fingerprint does not match";
    } else if (code == WOTBMOD_V3_E_PERMISSION_DENIED) {
        // The permission check runs before resolution is reported, so this
        // says nothing about whether the target exists — only that this mod
        // may not ask.  Managed targets such as Camera::setFOV land here
        // without the matching gameplay-tweak grant.
        verdict = "refused: a higher permission tier or a named grant is "
                  "required, so liveness was not established";
    } else {
        verdict = "unclassified outcome";
    }
    if (out_description) {
        *out_description = name + ": " + verdict + " [create_symbol OBSERVE -> " +
            std::string(ResultName(code)) +
            (message.empty() ? std::string() : "; " + message) + "]";
    }
    return reported;
}

// A live check of `bootstrap/get_last_error`.
//
// The row carried ValidationStatus::HostTested with SafeTest::None - a claim
// with nothing runnable behind it, the same shape of defect `loaders.dvpl` had.
// Proving the contract needs a failure that is unambiguously ours, so this
// makes one on purpose: `get_client_info` with no output buffer, which every
// implementation must refuse, and then reads the error back.
//
// Three things are checked, and two of them have been wrong in this codebase
// before. The provoked call must actually fail (an implementation that accepts
// a null output has a worse problem than this row). `get_last_error` must
// succeed. And what it reports must MATCH the code the failing call returned -
// an error store that answers OK with an empty message, or with a stale code
// from some earlier call, is exactly what this row exists to catch.
WotbModV3Result RunLastError(std::string* out_description) {
    if (!g_bootstrap || !g_bootstrap->get_last_error ||
        !g_bootstrap->get_client_info) {
        if (out_description) {
            *out_description = "bootstrap does not publish get_last_error";
        }
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const WotbModV3Result provoked =
        g_bootstrap->get_client_info(g_mod, nullptr);
    if (provoked == WOTBMOD_V3_OK) {
        if (out_description) {
            *out_description =
                "get_client_info(nullptr) was accepted, so no error was recorded";
        }
        return WOTBMOD_V3_E_PLATFORM;
    }
    WotbModV3ErrorInfo error = {};
    WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result read = g_bootstrap->get_last_error(g_mod, &error);
    if (read != WOTBMOD_V3_OK) {
        if (out_description) {
            *out_description = std::string("get_last_error refused: ") +
                ResultName(read);
        }
        return read;
    }
    if (error.message[0] == '\0') {
        if (out_description) {
            *out_description = "get_last_error answered OK with an empty message";
        }
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (error.code != static_cast<uint32_t>(provoked)) {
        if (out_description) {
            *out_description = std::string("get_last_error reported code ") +
                std::to_string(error.code) + " for a call that returned " +
                ResultName(provoked);
        }
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (out_description) {
        *out_description = std::string("provoked ") + ResultName(provoked) +
            "; get_last_error round-tripped code and message: \"" +
            error.message + "\"";
    }
    return WOTBMOD_V3_OK;
}

// A live check of `runtime/owner-cleanup`.
//
// This row is named "очистка подписок и handle", and it also stood on
// SafeTest::None. A mod cannot disable itself to watch the runtime sweep its
// owner state, but it does not need to: the observable half of the contract is
// that a released handle stops being alive, that a cancelled subscription stops
// being a valid token, and that a cleanup callback can be registered and taken
// back without leaking. All three are testable from inside an enabled mod, and
// all three are what a caller actually depends on.
//
// Every object created here is released on every path, including the failing
// ones - a test for cleanup that leaks on failure would be its own punchline.
WotbModV3Result RunOwnerCleanup(std::string* out_description) {
    if (!g_lifecycle || !g_lifecycle->register_cleanup ||
        !g_lifecycle->unregister_cleanup || !g_handles ||
        !g_handles->is_alive || !g_handles->release) {
        if (out_description) {
            *out_description =
                "lifecycle cleanup or the handles interface is unavailable";
        }
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    WotbModV3Token cleanup_token = 0u;
    const WotbModV3Result registered = g_lifecycle->register_cleanup(
        g_mod, &OwnerCleanupProbeCallback, nullptr, &cleanup_token);
    if (registered != WOTBMOD_V3_OK) {
        if (out_description) {
            *out_description = std::string("register_cleanup refused: ") +
                ResultName(registered);
        }
        return registered;
    }

    // A real owned object, not a fabricated handle: the whole point is that the
    // runtime's own bookkeeping is what answers.
    WotbModV3Handle document = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result step = RunPortableYamlDocument(&document);
    std::string description;
    if (step == WOTBMOD_V3_OK) {
        uint32_t alive = 0u;
        step = g_handles->is_alive(g_mod, document, &alive);
        if (step == WOTBMOD_V3_OK && alive == 0u) {
            description = "a freshly created handle reported itself dead";
            step = WOTBMOD_V3_E_PLATFORM;
        }
    }
    if (step == WOTBMOD_V3_OK) {
        step = g_handles->release(g_mod, document);
        if (step == WOTBMOD_V3_OK) {
            uint32_t alive = 1u;
            // After release the runtime is entitled to answer either "not
            // alive" or "unknown handle"; both mean the owner no longer holds
            // it. Answering OK with alive=1 does not.
            const WotbModV3Result probe =
                g_handles->is_alive(g_mod, document, &alive);
            if (probe == WOTBMOD_V3_OK && alive != 0u) {
                description = "a released handle was still reported alive";
                step = WOTBMOD_V3_E_PLATFORM;
            } else {
                document = WOTBMOD_V3_INVALID_HANDLE;
            }
        }
    }
    if (document != WOTBMOD_V3_INVALID_HANDLE) {
        g_handles->release(g_mod, document);
    }

    const WotbModV3Result unregistered =
        g_lifecycle->unregister_cleanup(g_mod, cleanup_token);
    if (step == WOTBMOD_V3_OK && unregistered != WOTBMOD_V3_OK) {
        description = std::string("unregister_cleanup refused: ") +
            ResultName(unregistered);
        step = unregistered;
    }
    if (out_description) {
        *out_description = step == WOTBMOD_V3_OK
            ? "cleanup callback registered and taken back; an owned handle went "
              "alive -> dead on release"
            : description;
    }
    return step;
}

WotbModV3Result RequestReloadForRow(size_t index);

/* The four Session rows. enumerate/current read the region; the refusal row
 * expects E_CONFLICT outside the hangar; the change row acts only with
 * WOTBMOD_VALIDATION_CLUSTER_TARGET=<id|auto> and leaves its verdict to the
 * changed event (OnSessionClusterEvent). */
WotbModV3Result RunSessionClusterTest(const char* id, std::string* description) {
    if (!g_session_cluster) {
        *description = "wotbmod.session.cluster unavailable: " + LastErrorMessage();
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    uint32_t count = 0u;
    WotbModV3Result result = g_session_cluster->enumerate(g_mod, nullptr, &count);
    if (result != WOTBMOD_V3_OK) {
        *description = "enumerate(count) -> " + std::string(ResultName(result)) + " " +
                       LastErrorMessage();
        return result;
    }
    std::vector<WotbModV3ClusterInfo> items(count);
    for (WotbModV3ClusterInfo& item : items) {
        WOTBMOD_V3_INIT_STRUCT(item, WOTBMOD_V3_SESSION_CLUSTER_VERSION);
    }
    if (count != 0u) {
        result = g_session_cluster->enumerate(g_mod, items.data(), &count);
        if (result != WOTBMOD_V3_OK) {
            *description = "enumerate(items) -> " + std::string(ResultName(result));
            return result;
        }
    }
    std::string names;
    uint32_t current_count = 0u;
    const WotbModV3ClusterInfo* current = nullptr;
    for (const WotbModV3ClusterInfo& item : items) {
        if (!names.empty()) names += ",";
        names += item.name;
        names += "#" + std::to_string(item.cluster_id);
        if (item.current) names += "*";
        if (!item.alive) names += "(dead)";
        if (!item.allowed) names += "(denied)";
        if (item.current) {
            ++current_count;
            current = &item;
        }
    }
    if (std::strcmp(id, "session.cluster.enumerate") == 0) {
        *description = "count=" + std::to_string(count) + " current=" +
                       std::to_string(current_count) + " names=" + names;
        return count >= 1u && current_count == 1u ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
    }
    WotbModV3ClusterInfo now = {};
    WOTBMOD_V3_INIT_STRUCT(now, WOTBMOD_V3_SESSION_CLUSTER_VERSION);
    result = g_session_cluster->get_current(g_mod, &now);
    if (result != WOTBMOD_V3_OK) {
        *description = "get_current -> " + std::string(ResultName(result)) + " " +
                       LastErrorMessage();
        return result;
    }
    if (std::strcmp(id, "session.cluster.current") == 0) {
        *description = std::string("current=") + now.name + " id=" +
                       std::to_string(now.cluster_id) + " alive=" + std::to_string(now.alive) +
                       " allowed=" + std::to_string(now.allowed) + " ccu=" +
                       std::to_string(now.ccu) +
                       (current && std::strcmp(current->name, now.name) == 0
                            ? " (matches enumerate)" : " (enumerate disagrees)");
        return current && std::strcmp(current->name, now.name) == 0
            ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
    }
    RefreshClientState();
    if (std::strcmp(id, "session.cluster.refuse_outside_hangar") == 0) {
        if (g_client_state == "HANGAR") {
            *description = "in the hangar; run this row from a battle or the results screen";
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        result = g_session_cluster->change(g_mod, now.cluster_id);
        *description = "change(" + std::to_string(now.cluster_id) + ") in " + g_client_state +
                       " -> " + ResultName(result) + " " + LastErrorMessage();
        return result == WOTBMOD_V3_E_CONFLICT ? WOTBMOD_V3_OK : WOTBMOD_V3_E_PLATFORM;
    }
    /* session.cluster.change */
    char target[64] = {};
    const DWORD length = GetEnvironmentVariableA(
        "WOTBMOD_VALIDATION_CLUSTER_TARGET", target, sizeof(target));
    if (length == 0u || length >= sizeof(target)) {
        *description = "set WOTBMOD_VALIDATION_CLUSTER_TARGET=<id|auto> to run the switch";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    int32_t cluster_id = WOTBMOD_V3_SESSION_CLUSTER_AUTO;
    if (std::strcmp(target, "other") == 0) {
        /* The first alive, allowed cluster that is not the current one. */
        cluster_id = now.cluster_id;
        for (const WotbModV3ClusterInfo& item : items) {
            if (!item.current && item.alive && item.allowed) {
                cluster_id = item.cluster_id;
                break;
            }
        }
    } else if (std::strcmp(target, "auto") != 0) {
        cluster_id = std::atoi(target);
        for (const WotbModV3ClusterInfo& item : items) {
            if (std::strcmp(item.name, target) == 0) cluster_id = item.cluster_id;
        }
    }
    if (g_client_state != "HANGAR") {
        *description = "not in the hangar (" + g_client_state + "); the switch needs the hangar";
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    g_session_change_started_ms.store(UnixMilliseconds());
    result = g_session_cluster->change(g_mod, cluster_id);
    if (result != WOTBMOD_V3_OK) {
        g_session_change_started_ms.store(0u);
        *description = "change(" + std::to_string(cluster_id) + ") -> " + ResultName(result) +
                       " " + LastErrorMessage();
        return result;
    }
    *description = "change(" + std::to_string(cluster_id) + ") accepted from " + now.name +
                   "; waiting for the changed event";
    QueueLiveLog(FindCapability(id), LiveSeverity::Info, "session.cluster.change", *description);
    return WOTBMOD_V3_OK;
}

void RunSafeTest(size_t index) {
    if (index >= kCapabilityCount) return;
    CapabilityState& state = g_states[index];
    if (state.crash_disabled) {
        RecordResult(index, WOTBMOD_V3_E_NOT_SUPPORTED,
                     "auto-disabled after suspected crash; press F12 to re-enable");
        SaveSnapshots();
        return;
    }
    const CapabilitySpec& spec = kCapabilities[index];
    if (spec.safe_test == SafeTest::None) return;
    if (spec.deliberately_unsupported) {
        const WotbModV3Result blocked = g_unsafe_confirmed
            ? WOTBMOD_V3_E_NOT_SUPPORTED : WOTBMOD_V3_E_PERMISSION_DENIED;
        const std::string message = g_unsafe_confirmed
            ? "unsafe/native capability remains NOT_SUPPORTED on this client"
            : "unsafe test blocked; press U to confirm, then F6";
        RecordResult(index, blocked, message);
        QueueLiveLog(index, LiveSeverity::Warn, "validation.unsafe_blocked", message);
        SaveSnapshots();
        return;
    }
    if (spec.native) WriteCrashMarker(index, spec.id);
    WotbModV3Result result = WOTBMOD_V3_E_NOT_SUPPORTED;
    std::string description = "not supported";
    uint64_t handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t active_handles = 0u;
    switch (spec.safe_test) {
        case SafeTest::ClientFingerprint: {
            WotbModV3ClientInfo info = {};
            WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
            result = g_bootstrap->get_client_info(g_mod, &info);
            description = "version=" + std::string(info.client_version) +
                " sha256=" + std::string(info.executable_sha256).substr(0u, 12u) + "...";
            break;
        }
        case SafeTest::DeviceInfo: {
            WotbModV3DeviceInfo info = {};
            // THE INTERFACE'S OWN VERSION, NOT THE ABI'S.
            //
            // `DeviceGetInfo` checks `api_version == WOTBMOD_V3_DEVICE_VERSION`,
            // which is 1; `WOTBMOD_V3_ABI_VERSION` is 0x00030000. Every other
            // per-interface struct in this file already passes its own constant
            // - audio, resources, yaml, archive, hooks, render, input, events -
            // and only this one reached for the ABI. Live it answered
            // "device info structure header is invalid" on every call, so
            // `client.device_info` could never be validated at all.
            //
            // The host double does not make this check, which is why the test
            // still reads HOST_TESTED. That gap is the reason a live run is a
            // release gate and not a formality.
            WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_DEVICE_VERSION);
            result = g_device ? g_device->get_info(g_mod, &info)
                              : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "os=" + std::string(info.operating_system) +
                  " cpu=" + std::string(info.processor)
                : LastErrorMessage();
            break;
        }
        case SafeTest::LeaveToHangar:
            result = g_client ? g_client->leave_to_hangar(g_mod)
                              : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = "LeaveToHangar requested by explicit F6 action";
            break;
        case SafeTest::LifecycleInfo: {
            WotbModV3LifecycleInfo info = {};
            // Same mistake as DeviceInfo above, same fix:
            // runtime_services.cpp:232 checks
            // `api_version != WOTBMOD_V3_LIFECYCLE_VERSION` and this passed the
            // ABI's. Live it answered "lifecycle info has an invalid structure
            // header" every time.
            WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_LIFECYCLE_VERSION);
            result = g_lifecycle ? g_lifecycle->get_info(g_mod, &info)
                                 : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "state=" + std::to_string(info.state) +
                  " hot_reload=" + std::to_string(info.hot_reload_supported)
                : LastErrorMessage();
            break;
        }
        case SafeTest::DeferredReload:
        case SafeTest::ReloadWithSubscription:
            result = RequestReloadForRow(index);
            description = result == WOTBMOD_V3_OK
                ? "reload queued; module unloads and reloads at the frame "
                  "boundary (PASS recorded by the reloaded instance)"
                : "request_reload was rejected";
            break;
        case SafeTest::CameraSnapshot: {
            result = g_camera ? g_camera->get_active(g_mod, &handle)
                              : WOTBMOD_V3_E_NOT_SUPPORTED;
            uint32_t mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
            if (result == WOTBMOD_V3_OK) {
                result = g_camera->get_mode(g_mod, handle, &mode);
                if (g_camera_last_handle != handle) {
                    g_camera_last_handle = handle;
                    g_camera_birth_ms = UnixMilliseconds();
                }
                g_camera_previous_mode = g_camera_mode;
                g_camera_mode = mode;
                active_handles = 1u;
            }
            description = "mode=" + std::to_string(mode) +
                " previous=" + std::to_string(g_camera_previous_mode) +
                " lifetime_ms=" + std::to_string(
                    g_camera_birth_ms ? UnixMilliseconds() - g_camera_birth_ms : 0u) +
                " duplicates=" + std::to_string(g_camera_duplicate_count);
            break;
        }
        case SafeTest::EventQueue: {
            const char payload[] = "validation-self-event";
            result = g_events ? g_events->post(
                g_mod, "mod.wotbmod.native_validation.self",
                payload, sizeof(payload) - 1u, WOTBMOD_V3_EVENT_FLAG_NONE)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = "self event posted; delivery must occur via runtime queue";
            break;
        }
        case SafeTest::EntitySnapshot: {
            EntityVisitContext context;
            result = g_entities ? g_entities->enumerate_visible(
                g_mod, &EntityVisitor, &context) : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = "visible_count=" + std::to_string(context.count);
            active_handles = context.count;
            break;
        }
        case SafeTest::VehicleState: {
            result = RunVehicleStateTest(&description);
            break;
        }
        case SafeTest::RpcPolicy: {
            WotbModV3BigWorldRpcPolicy policy = {};
            WOTBMOD_V3_INIT_STRUCT(policy, WOTBMOD_V3_BIGWORLD_RPC_VERSION);
            result = g_rpc ? g_rpc->get_policy(g_mod, &policy)
                           : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "metadata=" + std::to_string(policy.metadata_observation) +
                  " payload=" + std::to_string(policy.payload_access) +
                  " send=" + std::to_string(policy.outgoing_injection) +
                  " modify=" + std::to_string(policy.packet_modification) +
                  " drop=" + std::to_string(policy.packet_drop) +
                  " replay=" + std::to_string(policy.packet_replay)
                : LastErrorMessage();
            break;
        }
        case SafeTest::ProjectileInvalidHandle: {
            uint32_t scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
            result = g_projectile ? g_projectile->projectile_get_owner_scope(
                g_mod, WOTBMOD_V3_INVALID_HANDLE, &scope)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            if (result == WOTBMOD_V3_E_INVALID_HANDLE ||
                result == WOTBMOD_V3_E_NOT_FOUND) result = WOTBMOD_V3_OK;
            if (result == WOTBMOD_V3_OK && g_projectile) {
                WotbModV3ProjectileSnapshot snapshot = {};
                WOTBMOD_V3_INIT_STRUCT(
                    snapshot, WOTBMOD_V3_PROJECTILE_VERSION_2);
                const WotbModV3Result snapshot_result =
                    g_projectile->projectile_get_snapshot(
                        g_mod, WOTBMOD_V3_INVALID_HANDLE, &snapshot);
                if (snapshot_result != WOTBMOD_V3_E_INVALID_HANDLE &&
                    snapshot_result != WOTBMOD_V3_E_NOT_FOUND) {
                    result = snapshot_result;
                }
            }
            description = "V2 invalid-handle owner/snapshot paths rejected safely";
            break;
        }
        case SafeTest::RenderSnapshot: {
            uint32_t backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
            WotbModV3Rect viewport = {};
            result = g_render ? g_render->get_backend(g_mod, &backend)
                              : WOTBMOD_V3_E_NOT_SUPPORTED;
            if (result == WOTBMOD_V3_OK) result = g_render->get_viewport(g_mod, &viewport);
            description = "backend=" + std::to_string(backend) +
                " viewport=" + std::to_string(static_cast<int>(viewport.width)) + "x" +
                std::to_string(static_cast<int>(viewport.height)) +
                " callbacks=" + std::to_string(g_render_calls.load()) +
                " render_thread=" + std::to_string(g_render_thread.load());
            break;
        }
        case SafeTest::RenderNativeSnapshot: {
            void* device = nullptr;
            void* context = nullptr;
            void* swapchain = nullptr;
            result = g_render_native
                ? g_render_native->get_native_device(g_mod, &device)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            if (result == WOTBMOD_V3_OK) result = g_render_native->get_native_context(g_mod, &context);
            if (result == WOTBMOD_V3_OK) result = g_render_native->get_native_swapchain(g_mod, &swapchain);
            g_native_device_present.store(device ? 1u : 0u);
            g_native_context_present.store(context ? 1u : 0u);
            g_native_swapchain_present.store(swapchain ? 1u : 0u);
            description = std::string("borrowed device=") + (device ? "present" : "absent") +
                " context=" + (context ? "present" : "absent") +
                " swapchain=" + (swapchain ? "present" : "absent") +
                "; raw values not persisted";
            break;
        }
        case SafeTest::UiInspector: {
            WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
            result = g_ui ? g_ui->get_active_screen(g_mod, &screen)
                          : WOTBMOD_V3_E_NOT_SUPPORTED;
            WotbModV3UiControlSnapshot snapshot = {};
            WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
            if (result == WOTBMOD_V3_OK) {
                result = g_ui->control_get_snapshot(g_mod, screen, &snapshot);
            }
            description = result == WOTBMOD_V3_OK
                ? "active UI id=" + std::string(snapshot.id) +
                  " children=" + std::to_string(snapshot.child_count) +
                  " flags=" + std::to_string(snapshot.flags)
                : "active native UI tree unavailable: " + LastErrorMessage();
            if (screen != WOTBMOD_V3_INVALID_HANDLE) {
                const WotbModV3Result release_result =
                    g_ui->v2.control_destroy(g_mod, screen);
                if (result == WOTBMOD_V3_OK && release_result != WOTBMOD_V3_OK) {
                    result = release_result;
                }
            }
            break;
        }
        // Breadth-first over the active screen, asking every control for the
        // engine's own text. The handles come from the API and are released
        // here; the walk is bounded so a runaway tree cannot stall the panel.
        // The walk touches every control of the active screen while the DAVA
        // main thread may be rebuilding it, and this poll runs on the render
        // thread: the live run of 2026-09-04 (run 4) lost the client right
        // after the sweep reached this row. The walk is marshalled onto the
        // main thread, like the scene inspector, and the callback judges.
        case SafeTest::UiLiveText: {
            WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
            result = g_async ? g_async->dispatch_to_main_thread(
                g_mod, &DeferredUiLiveTextCallback,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "live text walk scheduled on the DAVA main thread"
                : "live text walk could not be scheduled: " +
                  std::string(ResultName(result));
            handle = task;
            active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
            break;
        }
        case SafeTest::HudStock: {
            WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
            result = g_async ? g_async->dispatch_to_main_thread(
                g_mod, &DeferredHudStockCallback,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "HUD stock-control test scheduled on the DAVA main thread"
                : "HUD stock-control test could not be scheduled: " +
                  std::string(ResultName(result));
            handle = task;
            active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
            break;
        }
        case SafeTest::HudReset: {
            WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
            result = g_async ? g_async->dispatch_to_main_thread(
                g_mod, &DeferredHudResetCallback,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "HUD reset scheduled on the DAVA main thread"
                : "HUD reset could not be scheduled: " +
                  std::string(ResultName(result));
            handle = task;
            active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
            break;
        }
        // Both scene routes marshal onto the DAVA main thread, and this poll
        // runs on the render thread - see DeferredInspectorCallback for the
        // deadlock that produced. Dispatch, and let the callback judge.
        case SafeTest::SceneInspector:
        case SafeTest::MaterialInspector: {
            WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
            result = g_async ? g_async->dispatch_to_main_thread(
                g_mod, &DeferredInspectorCallback,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = result == WOTBMOD_V3_OK
                ? "inspection scheduled on the DAVA main thread"
                : "inspection could not be scheduled: " +
                  std::string(ResultName(result));
            handle = task;
            active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
            break;
        }
        case SafeTest::AudioTone: {
            std::string step;
            result = RunAudioTone(&step);
            description = "generated short WAV create/preload/play/stop/destroy";
            if (!step.empty()) description += ": " + step;
            break;
        }
        case SafeTest::ResourceText:
            result = RunResourceText(false);
            description = "owner VFS text load/release";
            break;
        case SafeTest::PortableYaml:
            result = RunPortableYaml();
            description = "portable YAML parse/root/release";
            break;
        case SafeTest::PortableArchive:
            result = RunPortableArchive();
            description = "portable directory archive open/count/release; ZIP remains host-tested";
            break;
        case SafeTest::NativeSoundEvent: {
            std::string sound_description;
            result = RunNativeSoundEvent(&sound_description);
            description = sound_description.empty()
                ? std::string("native sound event create/read/destroy")
                : sound_description;
            break;
        }
        case SafeTest::PortableDvpl: {
            std::string dvpl_description;
            result = RunPortableDvpl(&dvpl_description);
            description = dvpl_description.empty()
                ? std::string("DVPL container round-trip")
                : dvpl_description;
            break;
        }
        case SafeTest::NativeLoaderAvailability: {
            WotbModV3LoaderBackendInfo info = {};
            WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_LOADERS_VERSION);
            const uint32_t backend = std::strstr(spec.id, "yaml")
                ? WOTBMOD_V3_LOADER_DAVA_YAML : WOTBMOD_V3_LOADER_DAVA_ARCHIVE;
            result = g_loaders ? g_loaders->get_backend_info(g_mod, backend, &info)
                               : WOTBMOD_V3_E_NOT_SUPPORTED;
            if (result == WOTBMOD_V3_OK && info.available == 0u) {
                result = WOTBMOD_V3_E_NOT_SUPPORTED;
            }
            description = info.available ? info.name : info.unavailable_reason;
            break;
        }
        case SafeTest::AsyncResourceLoad:
            result = RunResourceText(true);
            description = result == WOTBMOD_V3_OK
                ? "async load started; the frame poll records the state it "
                  "settles on"
                : "async resource load was refused";
            break;
        case SafeTest::ReloadWithAsyncResource:
            result = RunResourceText(true);
            if (result == WOTBMOD_V3_OK && g_lifecycle) {
                const WotbModV3Result reload = RequestReloadForRow(index);
                if (reload != WOTBMOD_V3_OK) result = reload;
            }
            description = "async resource active; module reloads at the frame "
                          "boundary (PASS recorded by the reloaded instance)";
            break;
        case SafeTest::ReloadFromDeferredCallback: {
            WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
            result = g_async ? g_async->dispatch_to_main_thread(
                g_mod, &DeferredReloadCallback,
                reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                : WOTBMOD_V3_E_NOT_SUPPORTED;
            description = "reload scheduled through loader-owned main queue";
            handle = task;
            active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
            break;
        }
        case SafeTest::CrashRecovery:
            result = WOTBMOD_V3_OK;
            description = "marker write/clear and auto-disable logic armed";
            break;
        case SafeTest::LastError: {
            std::string last_error_description;
            result = RunLastError(&last_error_description);
            description = last_error_description;
            break;
        }
        case SafeTest::OwnerCleanup: {
            std::string cleanup_description;
            result = RunOwnerCleanup(&cleanup_description);
            description = cleanup_description;
            break;
        }
        // One case for all 42 published symbols: the symbol is read back from
        // the selected row's binding_id, so adding a name to kCapabilities is
        // the only edit a new hook target needs.
        case SafeTest::GesProbe: {
            uint32_t type_count = 0u;
            result = g_ges ? g_ges->list_types(g_mod, nullptr, 0u, &type_count)
                           : WOTBMOD_V3_E_NOT_SUPPORTED;
            if (result == WOTBMOD_V3_E_BUFFER_TOO_SMALL) result = WOTBMOD_V3_OK;
            size_t distinct = 0u;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                distinct = g_ges_types_seen.size();
            }
            std::string publish = "skipped";
            if (result == WOTBMOD_V3_OK &&
                std::strcmp(spec.id, "ges.publish_echo") == 0) {
                RefreshClientState();
                if (g_client_state == "BATTLE" || g_client_state == "TRAINING") {
                    WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
                    result = g_async ? g_async->dispatch_to_main_thread(
                        g_mod, &DeferredGesEchoCallback,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task)
                        : WOTBMOD_V3_E_NOT_SUPPORTED;
                    publish = result == WOTBMOD_V3_OK
                        ? "scheduled on the DAVA main thread"
                        : std::string(ResultName(result));
                    handle = task;
                    active_handles = result == WOTBMOD_V3_OK ? 1u : 0u;
                } else {
                    publish = "not in battle";
                    result = WOTBMOD_V3_E_NOT_SUPPORTED;
                }
            }
            description = "types=" + std::to_string(type_count) +
                " events=" + std::to_string(g_ges_events.load()) +
                " distinct=" + std::to_string(distinct) +
                " publish=" + publish +
                " echo_seen=" + std::to_string(g_ges_echo_seen.load());
            break;
        }
        case SafeTest::HookSymbolProbe:
            result = ProbeHookSymbol(index, spec.binding_id, &description);
            break;
        case SafeTest::SessionCluster:
            result = RunSessionClusterTest(spec.id, &description);
            break;
        default:
            break;
    }
    RecordResult(index, result, description, handle, active_handles);
    char error_code[32] = {};
    sprintf_s(error_code, "%u", static_cast<unsigned>(result));
    Trace(
        spec.id, "validation.safe_test", kModId,
        handle, HandleGeneration(handle),
        result == WOTBMOD_V3_OK ? "OK" : "ERROR",
        error_code,
        "{\"description\":\"" + JsonEscape(description) + "\"}");
    QueueLiveLog(
        index,
        result == WOTBMOD_V3_OK ? LiveSeverity::Pass : LiveSeverity::Fail,
        "validation.safe_test",
        description);
    if (spec.native || spec.safe_test == SafeTest::CrashRecovery) ClearCrashMarker();
    SaveSnapshots();
}

// Runs the same probe F6 runs, over every published symbol, in one keystroke.
// The rows are switched on first because QueueLiveLog drops lines for tests
// that are not selected, and a line per symbol is the point of the sweep.
void RunAllHookProbes() {
    std::vector<size_t> indices;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        if (kCapabilities[index].safe_test == SafeTest::HookSymbolProbe) {
            indices.push_back(index);
        }
    }
    if (indices.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (const size_t index : indices) g_test_enabled[index] = 1u;
    }
    g_snapshot_batch.fetch_add(1u);
    for (const size_t index : indices) RunSafeTest(index);
    g_snapshot_batch.fetch_sub(1u);
    SaveSnapshots();
    QueueLiveLog(
        indices.front(), LiveSeverity::Info, "validation.hook_probe_sweep",
        "probed " + std::to_string(indices.size()) +
            " published hook symbols with OBSERVE; no detour was installed",
        false);
}

// Runs every registered safe test in one keystroke.
//
// The reason this exists: a verdict in LIVE_VALIDATION_RESULTS.json outlives the
// loader that produced it. Rows probed months and several loader builds ago still
// read NOT_SUPPORTED even after the backend behind them was implemented, and the
// only way to tell a stale verdict from a real gap was to walk 92 rows pressing
// F6 - which nobody does, so the file drifts. One key re-measures the whole set
// against the client that is actually running.
//
// Three exclusions, each for its own reason:
//   * `None` - there is nothing to run, and RunSafeTest would return immediately.
//   * `HookSymbolProbe` - 42 of the rows, and `H` already sweeps exactly those.
//   * `LeaveToHangar` - it succeeds by leaving the battle, which would abort the
//     sweep and every other test that needs to be in one.
// Rows marked `deliberately_unsupported` are NOT excluded: RunSafeTest already
// answers them honestly (blocked until `U`, then the real call), and their
// answer is precisely what a re-measurement is for.
// Comma-separated id prefixes from an environment variable, so a scripted
// live run can narrow the G sweep without touching the panel: the client
// inherits the runner's environment. WOTBMOD_VALIDATION_ONLY keeps only rows
// whose id starts with one of the prefixes; WOTBMOD_VALIDATION_SKIP drops
// them. Both empty means the whole safe set, as before.
std::vector<std::string> SweepPrefixesFromEnvironment(const char* name) {
    std::vector<std::string> prefixes;
    char buffer[512] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0u || length >= sizeof(buffer)) return prefixes;
    std::string current;
    for (DWORD i = 0u; i <= length; ++i) {
        const char c = i < length ? buffer[i] : ',';
        if (c == ',' || c == ';') {
            if (!current.empty()) prefixes.push_back(current);
            current.clear();
        } else if (c != ' ') {
            current.push_back(c);
        }
    }
    return prefixes;
}

bool IdMatchesAnyPrefix(const char* id, const std::vector<std::string>& prefixes) {
    for (const std::string& prefix : prefixes) {
        if (std::strncmp(id, prefix.c_str(), prefix.size()) == 0) return true;
    }
    return false;
}

// True while RunAllSafeTests is on the stack: reload rows then defer their
// request_reload to the main queue (see the comment on RunAllSafeTests).
bool g_sweep_in_progress = false;

bool IsReloadRow(SafeTest test) {
    return test == SafeTest::DeferredReload ||
           test == SafeTest::ReloadWithSubscription ||
           test == SafeTest::ReloadWithAsyncResource ||
           test == SafeTest::ReloadFromDeferredCallback;
}

WotbModV3Result RequestReloadForRow(size_t index) {
    if (!g_lifecycle) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (g_sweep_in_progress && g_async) {
        WotbModV3TaskHandle task = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result queued = g_async->dispatch_to_main_thread(
            g_mod, &DeferredReloadCallback,
            reinterpret_cast<void*>(static_cast<uintptr_t>(index)), &task);
        if (queued == WOTBMOD_V3_OK) return WOTBMOD_V3_OK;
    }
    MarkReloadPending(index);
    return g_lifecycle->request_reload(g_mod, g_mod);
}

void RunAllSafeTests() {
    const std::vector<std::string> only =
        SweepPrefixesFromEnvironment("WOTBMOD_VALIDATION_ONLY");
    const std::vector<std::string> skip =
        SweepPrefixesFromEnvironment("WOTBMOD_VALIDATION_SKIP");
    std::vector<size_t> indices;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const CapabilitySpec& spec = kCapabilities[index];
        if (spec.safe_test == SafeTest::None) continue;
        if (spec.safe_test == SafeTest::HookSymbolProbe) continue;
        if (spec.safe_test == SafeTest::LeaveToHangar) continue;
        if (!only.empty() && !IdMatchesAnyPrefix(spec.id, only)) continue;
        if (IdMatchesAnyPrefix(spec.id, skip)) continue;
        indices.push_back(index);
    }
    if (!only.empty() || !skip.empty()) {
        Log(WOTBMOD_V3_LOG_INFO,
            ("safe sweep narrowed by environment: only=" +
             std::to_string(only.size()) + " skip=" + std::to_string(skip.size()) +
             " rows=" + std::to_string(indices.size())).c_str());
    }
    if (indices.empty()) return;
    // Rows that end in a hot reload run last. Every deferred main-thread judge
    // queued by the rows before them (UI walk, HUD stock controls, scene and
    // material inspectors) must record and persist its verdict before the
    // module is torn down; queued after the reload request, they ran during
    // teardown and the reloaded instance restored the stale status (live
    // 2026-09-05: hud.stock_controls stayed NOT_SUPPORTED through two sweeps).
    {
        std::vector<size_t> ordered;
        ordered.reserve(indices.size());
        for (const size_t index : indices) {
            if (!IsReloadRow(kCapabilities[index].safe_test)) ordered.push_back(index);
        }
        for (const size_t index : indices) {
            if (IsReloadRow(kCapabilities[index].safe_test)) ordered.push_back(index);
        }
        indices.swap(ordered);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (const size_t index : indices) g_test_enabled[index] = 1u;
    }
    g_snapshot_batch.fetch_add(1u);
    // Reload rows go through the main queue during a sweep: the walk and the
    // inspectors queued there earlier must finish before the module is torn
    // down under them (live 2026-09-04, run 6: black client after the reload).
    g_sweep_in_progress = true;
    for (const size_t index : indices) RunSafeTest(index);
    g_sweep_in_progress = false;
    g_snapshot_batch.fetch_sub(1u);
    SaveSnapshots();
    QueueLiveLog(
        indices.front(), LiveSeverity::Info, "validation.safe_test_sweep",
        "re-ran " + std::to_string(indices.size()) +
            " safe tests against the running client",
        false);
}

WotbModV3Color StatusColor(ValidationStatus status) {
    if (status == ValidationStatus::Supported) return {0.35f, 1.0f, 0.45f, 1.0f};
    if (status == ValidationStatus::Failed) return {1.0f, 0.30f, 0.30f, 1.0f};
    if (status == ValidationStatus::NotSupported) return {0.70f, 0.70f, 0.70f, 1.0f};
    if (status == ValidationStatus::LiveTestPending) return {1.0f, 0.82f, 0.25f, 1.0f};
    return {0.40f, 0.80f, 1.0f, 1.0f};
}

// The managed renderer intentionally uses a compact ASCII atlas.  Keep the
// source strings in Russian for logs/docs, but transliterate them at the
// render boundary so UTF-8 Cyrillic is never shown as a row of question marks.
const std::string& AsciiDisplayText(const char* utf8) {
    static thread_local std::string result;
    result.clear();
    if (!utf8) return result;
    const unsigned char* cursor =
        reinterpret_cast<const unsigned char*>(utf8);
    while (*cursor) {
        const unsigned char first = *cursor++;
        if (first < 0x80u) {
            result.push_back(static_cast<char>(first));
            continue;
        }
        if ((first & 0xE0u) == 0xC0u && cursor[0]) {
            const uint32_t codepoint =
                (static_cast<uint32_t>(first & 0x1Fu) << 6u) |
                static_cast<uint32_t>(*cursor++ & 0x3Fu);
            static const std::map<uint32_t, const char*> translit = {
                {0x0401u, "Yo"}, {0x0410u, "A"}, {0x0411u, "B"},
                {0x0412u, "V"}, {0x0413u, "G"}, {0x0414u, "D"},
                {0x0415u, "E"}, {0x0416u, "Zh"}, {0x0417u, "Z"},
                {0x0418u, "I"}, {0x0419u, "J"}, {0x041Au, "K"},
                {0x041Bu, "L"}, {0x041Cu, "M"}, {0x041Du, "N"},
                {0x041Eu, "O"}, {0x041Fu, "P"}, {0x0420u, "R"},
                {0x0421u, "S"}, {0x0422u, "T"}, {0x0423u, "U"},
                {0x0424u, "F"}, {0x0425u, "Kh"}, {0x0426u, "Ts"},
                {0x0427u, "Ch"}, {0x0428u, "Sh"}, {0x0429u, "Shch"},
                {0x042Au, "`"}, {0x042Bu, "Y"}, {0x042Cu, "'"},
                {0x042Du, "E"}, {0x042Eu, "Yu"}, {0x042Fu, "Ya"},
                {0x0406u, "I"}, {0x0407u, "Yi"}, {0x0404u, "Ye"},
                {0x0408u, "J"},
                {0x0430u, "a"}, {0x0431u, "b"}, {0x0432u, "v"},
                {0x0433u, "g"}, {0x0434u, "d"}, {0x0435u, "e"},
                {0x0436u, "zh"}, {0x0437u, "z"}, {0x0438u, "i"},
                {0x0439u, "j"}, {0x043Au, "k"}, {0x043Bu, "l"},
                {0x043Cu, "m"}, {0x043Du, "n"}, {0x043Eu, "o"},
                {0x043Fu, "p"}, {0x0440u, "r"}, {0x0441u, "s"},
                {0x0442u, "t"}, {0x0443u, "u"}, {0x0444u, "f"},
                {0x0445u, "kh"}, {0x0446u, "ts"}, {0x0447u, "ch"},
                {0x0448u, "sh"}, {0x0449u, "shch"}, {0x044Au, "`"},
                {0x044Bu, "y"}, {0x044Cu, "'"}, {0x044Du, "e"},
                {0x044Eu, "yu"}, {0x044Fu, "ya"}, {0x0451u, "yo"},
                {0x0456u, "i"}, {0x0457u, "yi"}, {0x0454u, "ye"},
                {0x0458u, "j"}
            };
            const auto found = translit.find(codepoint);
            result += found == translit.end() ? "?" : found->second;
            continue;
        }
        // Consume continuation bytes for malformed/unsupported UTF-8 and
        // render one visible replacement rather than leaking raw bytes.
        if ((first & 0xF0u) == 0xE0u && cursor[0] && cursor[1]) {
            cursor += 2;
        } else if ((first & 0xF8u) == 0xF0u &&
                   cursor[0] && cursor[1] && cursor[2]) {
            cursor += 3;
        }
        result.push_back('?');
    }
    return result;
}

std::string ClipDisplayText(const std::string& text, size_t max_chars) {
    const std::string& ascii = AsciiDisplayText(text.c_str());
    if (ascii.size() <= max_chars) return ascii;
    if (max_chars < 4u) return ascii.substr(0u, max_chars);
    return ascii.substr(0u, max_chars - 3u) + "...";
}

bool EnsurePanelBackground() {
    if (!g_render || g_panel_background_texture != WOTBMOD_V3_INVALID_HANDLE) {
        return g_panel_background_texture != WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_panel_background_attempted) return false;
    g_panel_background_attempted = true;
    const uint32_t white_pixel = 0xFFFFFFFFu;
    WotbModV3TextureDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_RENDER_VERSION);
    descriptor.width = 1u;
    descriptor.height = 1u;
    descriptor.format = WOTBMOD_V3_TEXTURE_RGBA8_UNORM;
    descriptor.row_pitch = sizeof(white_pixel);
    descriptor.initial_data = &white_pixel;
    descriptor.initial_data_size = sizeof(white_pixel);
    descriptor.debug_name = "native-validation-panel-background";
    const WotbModV3Result result = g_render->create_texture(
        g_mod, &descriptor, &g_panel_background_texture);
    if (result != WOTBMOD_V3_OK) {
        g_panel_background_texture = WOTBMOD_V3_INVALID_HANDLE;
        return false;
    }
    return true;
}

WotbModV3Result DrawTextLine(
    const char* text,
    float x,
    float y,
    float size,
    WotbModV3Color color) {
    if (!g_render || !text) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const std::string& display_text = AsciiDisplayText(text);
    WotbModV3DrawText line = {};
    WOTBMOD_V3_INIT_STRUCT(line, WOTBMOD_V3_RENDER_VERSION);
    line.text = display_text.c_str();
    line.font_uri = "builtin://ascii";
    line.position = {x, y};
    line.color = color;
    line.font_size = size;
    line.max_width = std::max(320.0f,
        static_cast<float>(g_render_width.load()) - x - 12.0f);
    const WotbModV3Result result = g_render->draw_text(g_mod, &line);
    if (result == WOTBMOD_V3_OK) g_draw_calls.fetch_add(1u);
    return result;
}

const char* SoundModeName(SoundNotificationMode mode) {
    switch (mode) {
        case SoundNotificationMode::Off: return "OFF";
        case SoundNotificationMode::FailOnly: return "FAIL only";
        case SoundNotificationMode::PassAndFail: return "PASS + FAIL";
        case SoundNotificationMode::SelectedEvents: return "selected events";
        default: return "unknown";
    }
}

const char* LogFilterName(uint32_t filter) {
    switch (filter) {
        case 1u: return "PASS";
        case 2u: return "FAIL";
        case 3u: return "WARN";
        case 4u: return "INFO";
        case 5u: return "SELECTED";
        case 6u: return "CATEGORY";
        default: return "ALL";
    }
}

const char* DisplayStatus(const CapabilityState& state) {
    if (state.verdict != ManualVerdict::None) return VerdictName(state.verdict);
    return StatusName(state.status);
}

bool LiveLogMatches(const LiveLogEntry& entry, size_t selected) {
    if (g_log_filter == 1u && entry.severity != LiveSeverity::Pass) return false;
    if (g_log_filter == 2u && entry.severity != LiveSeverity::Fail) return false;
    if (g_log_filter == 3u && entry.severity != LiveSeverity::Warn) return false;
    if (g_log_filter == 4u && entry.severity != LiveSeverity::Info) return false;
    if (g_log_filter == 5u && (entry.capability >= kCapabilityCount ||
        !IsTestEnabled(entry.capability))) return false;
    if (g_log_filter == 6u && entry.category != CapabilityPanelCategory(selected)) return false;
    if (g_log_selected_only && (entry.capability >= kCapabilityCount ||
        entry.capability != selected)) return false;
    return true;
}

void ResetCapability(size_t index) {
    if (index >= kCapabilityCount) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    CapabilityState& state = g_states[index];
    state.verdict = ManualVerdict::None;
    state.warning = false;
    state.call_count = 0u;
    state.last_event_unix_ms = 0u;
    state.last_code = WOTBMOD_V3_E_NOT_SUPPORTED;
    state.last_result = "ожидает события";
    state.last_error.clear();
    state.comment.clear();
    state.status = state.backend_available
        ? (kCapabilities[index].native ? ValidationStatus::LiveTestPending
                                       : kCapabilities[index].baseline)
        : ValidationStatus::NotSupported;
    QueueLiveLog(index, LiveSeverity::Info, "validation.reset", "результат сброшен", false);
    g_snapshots_dirty.store(1u);
}

void ExportReadableReport() {
    if (g_report_path.empty()) return;
    std::lock_guard<std::recursive_mutex> state_lock(g_mutex);
    std::string report;
    report += "# Native Validation Live Test Center\r\n\r\n";
    report += "- fingerprint: `" + g_fingerprint_key + "`\r\n";
    report += "- client: `" + g_client_version + "`\r\n";
    report += "- state: `" + g_client_state + "`\r\n";
    report += "- sound: `" + std::string(SoundModeName(g_sound_mode)) + "`\r\n\r\n";
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        const CapabilityState& state = g_states[index];
        report += "- [" + std::string(DisplayStatus(state)) + "] `" +
            kCapabilities[index].id + "` — " +
            CapabilityHumanName(kCapabilities[index].id) +
            "; calls=" + std::to_string(state.call_count) +
            "; last=" + state.last_result + "\r\n";
    }
    report += "\r\n## Recent live events\r\n\r\n";
    {
        std::lock_guard<std::mutex> lock(g_live_log_mutex);
        const size_t begin = g_live_log.size() > 32u ? g_live_log.size() - 32u : 0u;
        size_t row = 0u;
        for (const LiveLogEntry& entry : g_live_log) {
            if (row++ < begin) continue;
            report += "- [" + std::string(LiveSeverityName(entry.severity)) + "] " +
                entry.title + " — " + entry.payload + "\r\n";
        }
    }
    WriteTextAtomic(g_report_path, report);
    const std::string txt_path = g_report_path + ".txt";
    WriteFileExact(txt_path, report.data(), report.size(), false);
    SaveSnapshots();
    QueueLogLine(g_live_log_path,
        "[" + TimestampReadable() + "]\r\n[INFO]\r\n[Diagnostics]\r\nОтчёт экспортирован\r\npath: " +
        SanitizeComment(g_report_path) + "\r\n\r\n");
}

void CopySelectedLogLine(size_t selected) {
    LiveLogEntry chosen;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_live_log_mutex);
        for (auto iterator = g_live_log.rbegin(); iterator != g_live_log.rend(); ++iterator) {
            if (iterator->capability == selected) {
                chosen = *iterator;
                found = true;
                break;
            }
        }
    }
    if (!found) return;
    std::string text = "[" + std::string(LiveSeverityName(chosen.severity)) + "] " +
        chosen.title + " | API: " + chosen.api + " | " + chosen.payload;
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1u);
    if (memory) {
        void* destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, text.c_str(), text.size() + 1u);
            GlobalUnlock(memory);
            SetClipboardData(CF_TEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) GlobalFree(memory);
    CloseClipboard();
}

void DrawLiveTestCenter(const WotbModV3RenderFrameInfo* frame) {
    if (!frame) return;
    ExpireShotChains();
    PumpFeedback();
    std::lock_guard<std::recursive_mutex> state_lock(g_mutex);
    const uint32_t width = g_render_width.load();
    const uint32_t height = g_render_height.load();
    const float left = static_cast<float>(std::max<int32_t>(0, g_panel_x));
    const float top = static_cast<float>(std::max<int32_t>(0, g_panel_y));
    const float panel_width = std::min<float>(
        static_cast<float>(std::max<int32_t>(800, g_panel_width)),
        std::max<float>(800.0f, static_cast<float>(width) - left - 8.0f));
    const float panel_height = std::min<float>(
        static_cast<float>(std::max<int32_t>(600, g_panel_height)),
        std::max<float>(600.0f, static_cast<float>(height) - top - 8.0f));
    const float category_x = left + 12.0f;
    const float list_x = left + panel_width * 0.245f;
    const float log_x = left + panel_width * 0.665f;
    const float column_top = top + 62.0f;
    const WotbModV3Color header = {1.0f, 0.84f, 0.30f, 1.0f};
    const WotbModV3Color muted = {0.68f, 0.76f, 0.88f, 1.0f};
    const WotbModV3Color white = {0.92f, 0.94f, 1.0f, 1.0f};
    const WotbModV3Color good = {0.38f, 1.0f, 0.50f, 1.0f};
    // The line path is a guaranteed solid quad in the managed renderer and
    // remains available even when a device reset invalidates the 1x1 texture.
    WotbModV3DrawLine backdrop = {};
    WOTBMOD_V3_INIT_STRUCT(backdrop, WOTBMOD_V3_RENDER_VERSION);
    backdrop.from = {left, top + panel_height * 0.5f, -1000.0f};
    backdrop.to = {left + panel_width, top + panel_height * 0.5f, -1000.0f};
    backdrop.color = {0.015f, 0.025f, 0.060f, 0.96f};
    backdrop.width = panel_height;
    if (g_render->draw_line(g_mod, &backdrop) == WOTBMOD_V3_OK) {
        g_draw_calls.fetch_add(1u);
    }
    if (EnsurePanelBackground()) {
        WotbModV3DrawSprite background = {};
        WOTBMOD_V3_INIT_STRUCT(background, WOTBMOD_V3_RENDER_VERSION);
        background.texture = g_panel_background_texture;
        background.material = WOTBMOD_V3_INVALID_HANDLE;
        background.destination = {left, top, panel_width, panel_height};
        background.source_uv = {0.0f, 0.0f, 1.0f, 1.0f};
        background.color = {0.02f, 0.04f, 0.09f, 0.70f};
        background.rotation_radians = 0.0f;
        background.z = -1000.0f;
        if (g_render->draw_sprite(g_mod, &background) == WOTBMOD_V3_OK) {
            g_draw_calls.fetch_add(1u);
        }
    }
    DrawTextLine("LIVE API TEST CENTER  |  F5 hide", left + 12.0f, top + 12.0f, 22.0f, white);
    char meta[640] = {};
    sprintf_s(meta, "Client %s | build %s | SHA %.12s... | binding %u | scene %s | sound %s %.0f%% | log %s%s",
        g_client_version.c_str(), g_client_version.c_str(), g_client_sha.c_str(),
        g_binding_pack_version, g_client_state.c_str(), SoundModeName(g_sound_mode),
        static_cast<double>(g_sound_volume * 100.0f), LogFilterName(g_log_filter),
        g_battle_mute ? " | BATTLE MUTE" : "");
    DrawTextLine(meta, left + 12.0f, top + 38.0f, 12.0f, muted);
    DrawTextLine("CATEGORIES", category_x, column_top, 14.0f, header);
    DrawTextLine("TESTS / EVENTS", list_x, column_top, 14.0f, header);
    DrawTextLine("LIVE LOG", log_x, column_top, 14.0f, header);

    for (size_t section = 0u; section < kSectionCount; ++section) {
        const std::vector<size_t> indices = SectionCapabilities(section);
        uint32_t selected_count = 0u;
        uint32_t pass_count = 0u;
        uint32_t fail_count = 0u;
        uint32_t pending_count = 0u;
        uint32_t unsupported_count = 0u;
        for (const size_t index : indices) {
            const CapabilityState& state = g_states[index];
            if (IsTestEnabled(index)) ++selected_count;
            if (state.verdict == ManualVerdict::Pass) ++pass_count;
            if (state.verdict == ManualVerdict::Fail || state.status == ValidationStatus::Failed) ++fail_count;
            if (state.status == ValidationStatus::LiveTestPending || state.verdict == ManualVerdict::None) ++pending_count;
            if (state.status == ValidationStatus::NotSupported || kCapabilities[index].deliberately_unsupported) ++unsupported_count;
        }
        char line[320] = {};
        sprintf_s(line, "%c %-21s sel=%02u P=%02u F=%02u wait=%02u%s",
            section == g_selected_section ? '>' : ' ', kSections[section], selected_count,
            pass_count, fail_count, pending_count,
            unsupported_count ? " NS" : "");
        DrawTextLine(line, category_x, column_top + 20.0f + section * 18.0f, 11.0f,
            section == g_selected_section ? header : muted);
    }

    const std::vector<size_t> indices = SectionCapabilities(g_selected_section);
    const size_t selected = SelectedCapability();
    const float list_y = column_top + 20.0f;
    const size_t first_row = indices.empty() ? 0u :
        (g_selected_in_section > 12u ? g_selected_in_section - 12u : 0u);
    size_t visible_row = 0u;
    for (size_t row = first_row; row < indices.size() && visible_row < 15u; ++row, ++visible_row) {
        const size_t index = indices[row];
        const CapabilityState& state = g_states[index];
        char line[680] = {};
        sprintf_s(line, "%c%c %-27s %-18s calls=%-4llu t=%u",
            IsTestEnabled(index) ? '[' : ' ', IsTestEnabled(index) ? 'x' : ' ',
            ClipDisplayText(CapabilityHumanName(kCapabilities[index].id), 27u).c_str(),
            ClipDisplayText(DisplayStatus(state), 18u).c_str(),
            static_cast<unsigned long long>(state.call_count), state.callback_thread);
        DrawTextLine(line, list_x, list_y + visible_row * 18.0f, 11.0f,
            index == selected ? header : StatusColor(state.status));
        char api[680] = {};
        sprintf_s(api, "    API: %-38s  action: %s",
            kCapabilities[index].id,
            ClipDisplayText(CapabilityInstruction(kCapabilities[index].id), 52u).c_str());
        DrawTextLine(api, list_x + 8.0f, list_y + visible_row * 18.0f + 12.0f, 9.5f, muted);
        ++visible_row;
    }
    float details_y = list_y + 286.0f;
    char detail[900] = {};
    const CapabilityState& selected_state = g_states[selected];
    sprintf_s(detail, "SELECTED: %s | API=%s | status=%s | verdict=%s | calls=%llu | last=%s",
        ClipDisplayText(CapabilityHumanName(kCapabilities[selected].id), 28u).c_str(),
        kCapabilities[selected].id,
        StatusName(selected_state.status), VerdictName(selected_state.verdict),
        static_cast<unsigned long long>(selected_state.call_count),
        ClipDisplayText(selected_state.last_result, 80u).c_str());
    DrawTextLine(detail, list_x, details_y, 11.0f, white);
    char instruction[900] = {};
    sprintf_s(instruction, "expected action: %s | last payload: %s | last thread: %u | last event: %llu",
        ClipDisplayText(CapabilityInstruction(kCapabilities[selected].id), 52u).c_str(),
        selected_state.last_result.empty()
            ? "none"
            : ClipDisplayText(selected_state.last_result, 80u).c_str(),
        selected_state.callback_thread,
        static_cast<unsigned long long>(selected_state.last_event_unix_ms));
    DrawTextLine(instruction, list_x, details_y + 18.0f, 10.0f, muted);

    std::vector<LiveLogEntry> visible_logs;
    {
        std::lock_guard<std::mutex> lock(g_live_log_mutex);
        for (auto iterator = g_live_log.rbegin(); iterator != g_live_log.rend() && visible_logs.size() < 28u; ++iterator) {
            if (!LiveLogMatches(*iterator, selected)) continue;
            visible_logs.push_back(*iterator);
        }
    }
    if (!g_log_paused) {
        const float log_y = column_top + 20.0f;
        size_t row = 0u;
        for (auto iterator = visible_logs.rbegin(); iterator != visible_logs.rend(); ++iterator, ++row) {
            char line[920] = {};
            sprintf_s(line, "[%s] %-25s %s",
                LiveSeverityName(iterator->severity),
                ClipDisplayText(iterator->title, 25u).c_str(),
                ClipDisplayText(iterator->payload, 88u).c_str());
            DrawTextLine(line, log_x, log_y + row * 22.0f, 10.0f,
                iterator->severity == LiveSeverity::Fail ? StatusColor(ValidationStatus::Failed) :
                iterator->severity == LiveSeverity::Warn ? header :
                iterator->severity == LiveSeverity::Pass ? good : muted);
            char api[920] = {};
            sprintf_s(api, "  %s | %s | tid=%u",
                ClipDisplayText(iterator->api, 30u).c_str(),
                ClipDisplayText(iterator->event_name, 34u).c_str(),
                iterator->thread_id);
            DrawTextLine(api, log_x, log_y + row * 22.0f + 11.0f, 8.5f, muted);
        }
    }
    char summary[640] = {};
    uint32_t chosen = 0u, pass = 0u, fail = 0u, wait = 0u, unsupported = 0u;
    for (size_t index = 0u; index < kCapabilityCount; ++index) {
        if (!IsTestEnabled(index)) continue;
        ++chosen;
        const CapabilityState& state = g_states[index];
        if (state.verdict == ManualVerdict::Pass) ++pass;
        if (state.verdict == ManualVerdict::Fail || state.status == ValidationStatus::Failed) ++fail;
        if (state.status == ValidationStatus::LiveTestPending || state.verdict == ManualVerdict::None) ++wait;
        if (state.status == ValidationStatus::NotSupported) ++unsupported;
    }
    sprintf_s(summary, "Selected %u | PASS %u | FAIL %u | WAIT %u | NOT_SUPPORTED %u | panel %ux%u",
        chosen, pass, fail, wait, unsupported, width, height);
    const float footer = top + panel_height - 82.0f;
    DrawTextLine(summary, left + 12.0f, footer, 12.0f, good);
    DrawTextLine("TAB category  UP/DOWN test  SPACE toggle  F6 active  F7 PASS  F8 FAIL  F9 SKIP  F10 reset", left + 12.0f, footer + 18.0f, 10.0f, header);
    DrawTextLine("1-9 presets, 0 all available  A all safe  N none  L filter  K pause  O autoscroll  M sound  B battle mute", left + 12.0f, footer + 34.0f, 10.0f, header);
    DrawTextLine("E report  Y copy selected log  H probe all hook symbols  G re-run all safe tests  F4 validation bundle  Shift+arrows resize  Ctrl+arrows move", left + 12.0f, footer + 50.0f, 10.0f, muted);
    g_export_button_left.store(static_cast<int32_t>(left + 8.0f));
    g_export_button_top.store(static_cast<int32_t>(footer + 14.0f));
    g_export_button_right.store(static_cast<int32_t>(left + 360.0f));
    g_export_button_bottom.store(static_cast<int32_t>(footer + 32.0f));
}

void PollPanelKeys();

// Scripted runs cannot rely on a key press landing in a rendered frame (the
// panel polls once per frame and the garage drops frames), so a run can ask
// for the sweep and the hook probe by wall-clock instead:
//   WOTBMOD_VALIDATION_AUTOSWEEP_SECONDS=N  show the panel at load, run the
//                                           G sweep N seconds later
//   WOTBMOD_VALIDATION_AUTOPROBE_SECONDS=N  run the H hook probe N seconds
//                                           after load
uint64_t g_autosweep_at_ms = 0u;
uint64_t g_autoprobe_at_ms = 0u;

uint64_t SecondsFromEnvironment(const char* name) {
    char buffer[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0u || length >= sizeof(buffer)) return 0u;
    return std::strtoull(buffer, nullptr, 10);
}

//   WOTBMOD_VALIDATION_AUTOSWEEP_BATTLE_SECONDS=N  run the sweep N seconds after
//                                                  the client first enters BATTLE
//                                                  (the HUD tree dump needs it)
bool RegisterRenderPanel();
uint64_t g_autosweep_battle_delay_ms = 0u;
uint64_t g_autosweep_battle_at_ms = 0u;

void ArmAutoActions() {
    const uint64_t sweep = SecondsFromEnvironment("WOTBMOD_VALIDATION_AUTOSWEEP_SECONDS");
    const uint64_t probe = SecondsFromEnvironment("WOTBMOD_VALIDATION_AUTOPROBE_SECONDS");
    const uint64_t battle = SecondsFromEnvironment("WOTBMOD_VALIDATION_AUTOSWEEP_BATTLE_SECONDS");
    if (battle) {
        char armed[8] = {};
        if (GetEnvironmentVariableA("WOTBMOD_VALIDATION_AUTO_BATTLE_ARMED", armed, sizeof(armed)) == 0u) {
            SetEnvironmentVariableA("WOTBMOD_VALIDATION_AUTO_BATTLE_ARMED", "1");
            g_autosweep_battle_delay_ms = battle * 1000u;
            RegisterRenderPanel();
            Log(WOTBMOD_V3_LOG_INFO,
                "auto battle sweep armed: " + std::to_string(battle) + "s after BATTLE");
        }
    }
    if (sweep == 0u && probe == 0u) return;
    // Once per process: a hot reload re-runs OnEnable in the same process,
    // and re-arming there would sweep (and reload) again without end.
    char armed[8] = {};
    if (GetEnvironmentVariableA("WOTBMOD_VALIDATION_AUTO_ARMED", armed, sizeof(armed)) != 0u) {
        return;
    }
    SetEnvironmentVariableA("WOTBMOD_VALIDATION_AUTO_ARMED", "1");
    const uint64_t now = UnixMilliseconds();
    if (sweep) g_autosweep_at_ms = now + sweep * 1000u;
    if (probe) g_autoprobe_at_ms = now + probe * 1000u;
    Log(WOTBMOD_V3_LOG_INFO,
        "auto actions armed from environment: sweep_in=" + std::to_string(sweep) +
        "s probe_in=" + std::to_string(probe) + "s");
}

void RunAutoActions() {
    const uint64_t now = UnixMilliseconds();
    if (g_autosweep_battle_delay_ms) {
        RefreshClientState();
        if (g_client_state == "BATTLE" || g_client_state == "TRAINING") {
            if (g_autosweep_battle_at_ms == 0u) {
                g_autosweep_battle_at_ms = now + g_autosweep_battle_delay_ms;
            } else if (now >= g_autosweep_battle_at_ms) {
                g_autosweep_battle_delay_ms = 0u;
                Log(WOTBMOD_V3_LOG_INFO, "auto battle sweep: running all safe tests in battle");
                RunAllSafeTests();
            }
        }
    }
    if (g_autosweep_at_ms && now >= g_autosweep_at_ms) {
        g_autosweep_at_ms = 0u;
        Log(WOTBMOD_V3_LOG_INFO, "auto sweep: running all safe tests");
        RunAllSafeTests();
    }
    if (g_autoprobe_at_ms && now >= g_autoprobe_at_ms) {
        g_autoprobe_at_ms = 0u;
        Log(WOTBMOD_V3_LOG_INFO, "auto probe: running all hook probes");
        RunAllHookProbes();
    }
}
void PollExportButton();

void WOTBMOD_V3_CALL RenderCallback(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo* frame,
    void*) noexcept {
    CallbackMetricScope callback_metric;
    try {
        g_render_calls.fetch_add(1u);
        g_render_thread.store(GetCurrentThreadId());
        if (frame) {
            g_render_backend.store(frame->backend);
            g_render_width.store(static_cast<uint32_t>(
                std::max(0.0f, frame->viewport.width)));
            g_render_height.store(static_cast<uint32_t>(
                std::max(0.0f, frame->viewport.height)));
        }
        const size_t callback_index = FindCapability("render.callback");
        if (callback_index < kCapabilityCount) {
            std::lock_guard<std::recursive_mutex> state_lock(g_mutex);
            CapabilityState& state = g_states[callback_index];
            state.call_count = g_render_calls.load();
            state.callback_thread = g_render_thread.load();
            state.last_event_unix_ms = UnixMilliseconds();
            state.last_code = WOTBMOD_V3_OK;
            state.last_result = "PRESENT render callback";
            if (state.verdict == ManualVerdict::None) {
                state.status = ValidationStatus::LiveTestPending;
            }
        }
        if (g_panel_visible.load() == 0u || !frame || !g_render) return;
        PollPanelKeys();
        RunAutoActions();
        const uint64_t draw_start = g_draw_calls.load();
        const uint64_t now_ms = UnixMilliseconds();
        if (g_perf_sample_ms == 0u) g_perf_sample_ms = now_ms;
        if (now_ms > g_perf_sample_ms + 999u) {
            const double seconds =
                static_cast<double>(now_ms - g_perf_sample_ms) / 1000.0;
            const uint64_t callbacks = g_callback_count.load();
            const uint64_t traces = g_trace_count.load();
            const uint64_t bytes = g_trace_bytes.load();
            const uint64_t snapshots = g_snapshot_writes.load();
            g_callbacks_per_second =
                static_cast<double>(callbacks - g_perf_sample_callbacks) /
                seconds;
            g_trace_per_second =
                static_cast<double>(traces - g_perf_sample_trace) / seconds;
            g_bytes_per_second =
                static_cast<double>(bytes - g_perf_sample_bytes) / seconds;
            g_snapshot_writes_per_minute =
                static_cast<double>(snapshots - g_perf_sample_snapshots) /
                seconds * 60.0;
            g_perf_sample_ms = now_ms;
            g_perf_sample_callbacks = callbacks;
            g_perf_sample_trace = traces;
            g_perf_sample_bytes = bytes;
            g_perf_sample_snapshots = snapshots;
            RefreshClientState();
            RefreshEntityInvalidations();
            if (g_async_resource != WOTBMOD_V3_INVALID_HANDLE &&
                g_resources) {
                WotbModV3ResourceInfo info = {};
                WOTBMOD_V3_INIT_STRUCT(
                    info, WOTBMOD_V3_RESOURCES_VERSION);
                const WotbModV3Result result = g_resources->get_info(
                    g_mod, g_async_resource, &info);
                // NAME the state and carry the reason. "async state=3" is
                // WOTBMOD_V3_RESOURCE_FAILED, and it read like a progress
                // report - a failed load was sitting in this row looking like
                // data. The resource carries its own error text; print it.
                RecordResult(
                    FindCapability("resources.async_reload"),
                    result == WOTBMOD_V3_OK &&
                            info.state == WOTBMOD_V3_RESOURCE_FAILED
                        ? WOTBMOD_V3_E_IO
                        : result,
                    result == WOTBMOD_V3_OK
                        ? "async " + std::string(ResourceStateName(info.state)) +
                          (info.error[0] ? std::string(": ") + info.error
                                         : std::string())
                        : LastErrorMessage(),
                    g_async_resource,
                    1u);
                if (result != WOTBMOD_V3_OK ||
                    info.state == WOTBMOD_V3_RESOURCE_READY ||
                    info.state == WOTBMOD_V3_RESOURCE_FAILED) {
                    g_resources->release(g_mod, g_async_resource);
                    g_async_resource = WOTBMOD_V3_INVALID_HANDLE;
                }
            }
            if (g_snapshots_dirty.exchange(0u) != 0u) SaveSnapshots();
        }
        DrawLiveTestCenter(frame);
        return;
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const float x = 18.0f;
        float y = 18.0f;
        const WotbModV3Result title_result = DrawTextLine(
            "WotbMod Native Validation 1.1.0  [F5 hide]", x, y, 18.0f,
            {0.95f, 0.95f, 1.0f, 1.0f});
        if (g_panel_draw_diagnostic.exchange(1u) == 0u) {
            RecordResult(
                FindCapability("ui.validation_panel"),
                title_result,
                title_result == WOTBMOD_V3_OK
                    ? "live D3D11 text draw accepted"
                    : "live panel draw failed: " +
                        std::string(ResultName(title_result)));
            if (title_result != WOTBMOD_V3_OK) {
                Log(WOTBMOD_V3_LOG_ERROR,
                    "validation panel draw failed on first live frame");
            }
            g_snapshots_dirty.store(1u);
        }
        if (title_result != WOTBMOD_V3_OK) return;
        y += 24.0f;
        char fingerprint[256] = {};
        sprintf_s(
            fingerprint, "Client %s | SHA %.12s... | binding %u | state %s | load cycle %llu",
            g_client_version.c_str(), g_client_sha.c_str(), g_binding_pack_version,
            g_client_state.c_str(), static_cast<unsigned long long>(g_load_cycle_count));
        DrawTextLine(fingerprint, x, y, 13.0f, {0.72f, 0.84f, 1.0f, 1.0f});
        y += 22.0f;
        char section[256] = {};
        sprintf_s(section, "[%02u/%02u] %s  (TAB/Shift+TAB section, UP/DOWN test)",
                  static_cast<unsigned>(g_selected_section + 1u),
                  static_cast<unsigned>(kSectionCount),
                  kSections[g_selected_section]);
        DrawTextLine(section, x, y, 16.0f, {1.0f, 0.82f, 0.30f, 1.0f});
        y += 24.0f;
        const std::vector<size_t> indices = SectionCapabilities(g_selected_section);
        for (size_t row = 0u; row < indices.size() && row < 10u; ++row) {
            const size_t index = indices[row];
            const CapabilityState& state = g_states[index];
            char line[512] = {};
            sprintf_s(line, "%c %-31s %-18s backend=%s calls=%llu handles=%u",
                      row == g_selected_in_section ? '>' : ' ',
                      kCapabilities[index].id, StatusName(state.status),
                      state.backend_available ? "yes" : "no",
                      static_cast<unsigned long long>(state.call_count),
                      state.active_handles);
            DrawTextLine(line, x, y, 13.0f, StatusColor(state.status));
            y += 18.0f;
        }
        const size_t selected = SelectedCapability();
        const CapabilitySpec& spec = kCapabilities[selected];
        const CapabilityState& state = g_states[selected];
        y += 6.0f;
        DrawTextLine("Selected capability details", x, y, 15.0f,
                     {0.95f, 0.95f, 1.0f, 1.0f});
        y += 20.0f;
        char details[768] = {};
        _snprintf_s(details, sizeof(details), _TRUNCATE,
                  "binding=%s | result=%s | error_code=%u | error=%s | thread=%u | event_ms=%llu | generation=%u | handles=%u | safe=%s | verdict=%s",
                  spec.binding_id, state.last_result.c_str(),
                  static_cast<unsigned>(state.last_code),
                  state.last_error.empty() ? "none" : state.last_error.c_str(),
                  state.callback_thread,
                  static_cast<unsigned long long>(state.last_event_unix_ms),
                  state.generation, state.active_handles,
                  spec.safe_test == SafeTest::None ? "no" : "F6",
                  VerdictName(state.verdict));
        DrawTextLine(details, x, y, 12.0f, {0.88f, 0.88f, 0.88f, 1.0f});
        y += 18.0f;
        char camera[512] = {};
        sprintf_s(camera,
                  "camera handle=%s gen=%u mode=%s previous=%s events=%llu duplicates=%llu thread=%u lifetime_ms=%llu",
                  HandleText(g_camera_last_handle).c_str(),
                  HandleGeneration(g_camera_last_handle),
                  CameraModeName(g_camera_mode),
                  CameraModeName(g_camera_previous_mode),
                  static_cast<unsigned long long>(g_camera_event_count),
                  static_cast<unsigned long long>(g_camera_duplicate_count),
                  FindCapability("camera.mode_change") < kCapabilityCount
                      ? g_states[FindCapability("camera.mode_change")].callback_thread
                      : 0u,
                  static_cast<unsigned long long>(
                      g_camera_birth_ms ? UnixMilliseconds() - g_camera_birth_ms : 0u));
        DrawTextLine(camera, x, y, 12.0f, {0.75f, 0.90f, 1.0f, 1.0f});
        y += 20.0f;
        if (std::strcmp(kSections[g_selected_section], "BigWorld Entity") == 0) {
            char entity[512] = {};
            if (!g_entity_observations.empty()) {
                const EntityObservation& latest = g_entity_observations.back();
                sprintf_s(
                    entity,
                    "entity public_id=%u type=%u gen=%u alive=%s preexisting=%s old_invalidated=%s active=%u",
                    latest.public_id, latest.type, latest.generation,
                    latest.alive ? "yes" : "no",
                    latest.preexisting ? "yes" : "no",
                    latest.old_handle_invalidated < 0
                        ? "UNKNOWN"
                        : (latest.old_handle_invalidated ? "yes" : "no"),
                    ActiveEntityCount());
            } else {
                strcpy_s(entity, "entity inspector: no visible public entities observed");
            }
            DrawTextLine(entity, x, y, 12.0f, {0.75f, 0.90f, 1.0f, 1.0f});
            y += 18.0f;
        }
        if (std::strcmp(
                kSections[g_selected_section], "Render / D3D11 / DXGI") == 0) {
            char render[640] = {};
            sprintf_s(
                render,
                "render backend=%u viewport=%ux%u resize=%llu swapchain=%llu lost=%llu restored=%llu callback=%llu thread=%u",
                g_render_backend.load(), g_render_width.load(), g_render_height.load(),
                static_cast<unsigned long long>(g_render_resize_count.load()),
                static_cast<unsigned long long>(g_swapchain_recreation_count.load()),
                static_cast<unsigned long long>(g_device_lost_count.load()),
                static_cast<unsigned long long>(g_device_restored_count.load()),
                static_cast<unsigned long long>(g_render_calls.load()),
                g_render_thread.load());
            DrawTextLine(render, x, y, 12.0f, {0.75f, 0.90f, 1.0f, 1.0f});
            y += 18.0f;
        }
        g_export_button_left.store(static_cast<int32_t>(x - 4.0f));
        g_export_button_top.store(static_cast<int32_t>(y - 3.0f));
        g_export_button_right.store(static_cast<int32_t>(x + 238.0f));
        g_export_button_bottom.store(static_cast<int32_t>(y + 17.0f));
        DrawTextLine(
            "[ Export Validation Bundle ] F4 | F6 RUN SAFE | F7 PASS | F8 FAIL | F9 SKIP | F10 RESET",
            x, y, 13.0f, {1.0f, 0.92f, 0.55f, 1.0f});
        PollExportButton();
        y += 18.0f;
        DrawTextLine(
            "F11 Mark camera mode change verified | F12 re-enable crash-disabled selected test",
            x, y, 13.0f, {1.0f, 0.92f, 0.55f, 1.0f});
        y += 18.0f;
        DrawTextLine(
            "Native status never exceeds LIVE_TEST_PENDING until exact-fingerprint PASS.",
            x, y, 12.0f, {1.0f, 0.62f, 0.30f, 1.0f});
        y += 18.0f;
        g_draw_calls_per_frame = static_cast<double>(
            g_draw_calls.load() - draw_start);
        const uint64_t callback_count = g_callback_count.load();
        const double callback_average_us = callback_count
            ? static_cast<double>(g_callback_total_100ns.load()) /
                static_cast<double>(callback_count) / 10.0
            : 0.0;
        char profiler[640] = {};
        sprintf_s(
            profiler,
            "profiler callbacks/s=%.1f trace/s=%.1f bytes/s=%.0f snapshots/min=%.1f draw/frame=%.0f callback_us avg=%.1f max=%.1f",
            g_callbacks_per_second,
            g_trace_per_second,
            g_bytes_per_second,
            g_snapshot_writes_per_minute,
            g_draw_calls_per_frame,
            callback_average_us,
            static_cast<double>(g_callback_max_100ns.load()) / 10.0);
        DrawTextLine(
            profiler, x, y, 12.0f,
            {0.60f, 1.0f, 0.72f, 1.0f});
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "RenderCallback contained an exception");
    }
}

bool RegisterRenderPanel();

void WOTBMOD_V3_CALL PanelInputCallback(
    WotbModV3Handle,
    WotbModV3Handle,
    float,
    uint32_t pressed,
    void*) noexcept {
    CallbackMetricScope callback_metric;
    if (!pressed) return;
    try {
        if (g_panel_visible.load() != 0u) {
            g_panel_visible.store(0u);
            g_export_button_left.store(0);
            g_export_button_top.store(0);
            g_export_button_right.store(0);
            g_export_button_bottom.store(0);
            if (g_render &&
                g_render_token != WOTBMOD_V3_INVALID_HANDLE) {
                g_render->unregister_callback(g_mod, g_render_token);
                g_render_token = WOTBMOD_V3_INVALID_HANDLE;
            }
        } else {
            RegisterRenderPanel();
        }
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "PanelInputCallback contained an exception");
    }
}

bool RegisterPanelInput() {
    if (!g_input) return false;
    WotbModV3InputBinding binding = {};
    WOTBMOD_V3_INIT_STRUCT(binding, WOTBMOD_V3_INPUT_VERSION);
    binding.device = WOTBMOD_V3_INPUT_DEVICE_KEYBOARD;
    binding.code = VK_F5;
    binding.modifiers = WOTBMOD_V3_INPUT_MOD_NONE;
    binding.scale = 1.0f;
    WotbModV3InputActionDesc action = {};
    WOTBMOD_V3_INIT_STRUCT(action, WOTBMOD_V3_INPUT_VERSION);
    action.value_type = WOTBMOD_V3_INPUT_VALUE_BUTTON;
    // The input API validates the context mask against the declared enum
    // bits.  UINT64_MAX contains reserved bits and makes registration fail
    // with WOTBMOD_V3_E_INVALID_ARGUMENT, leaving F5 silently unbound.
    action.contexts = static_cast<uint64_t>(WOTBMOD_V3_CONTEXT_ALL);
    CopyText(action.id, sizeof(action.id), "native_validation.panel");
    CopyText(action.display_name, sizeof(action.display_name),
             "Native validation panel");
    CopyText(action.description, sizeof(action.description),
             "Show or hide the native validation panel");
    action.default_bindings = &binding;
    action.default_binding_count = 1u;
    WotbModV3Result register_result = g_input->register_action(
        g_mod, &action, &g_panel_action);
    WotbModV3Result subscribe_result = WOTBMOD_V3_OK;
    if (register_result == WOTBMOD_V3_OK) {
        subscribe_result = g_input->subscribe(
            g_mod,
            g_panel_action,
            &PanelInputCallback,
            nullptr,
            &g_panel_action_token);
    }
    const WotbModV3Result result =
        register_result != WOTBMOD_V3_OK ? register_result : subscribe_result;
    if (result != WOTBMOD_V3_OK) {
        if (g_panel_action != WOTBMOD_V3_INVALID_HANDLE) {
            g_input->unregister_action(g_mod, g_panel_action);
        }
        g_panel_action = WOTBMOD_V3_INVALID_HANDLE;
        g_panel_action_token = WOTBMOD_V3_INVALID_HANDLE;
        char message[256] = {};
        sprintf_s(
            message, "native input ingress unavailable; F5 panel action not registered (register=%u subscribe=%u)",
            static_cast<unsigned>(register_result),
            static_cast<unsigned>(subscribe_result));
        Log(WOTBMOD_V3_LOG_ERROR, message);
        return false;
    }
    return true;
}

bool KeyPressed(int key) {
    static std::array<uint8_t, 256u> previous = {};
    if (key < 0 || key >= static_cast<int>(previous.size())) return false;
    const SHORT state = GetAsyncKeyState(key);
    const bool down = (state & 0x8000) != 0;
    // Bit 0 is "pressed since the last GetAsyncKeyState for this key": a tap
    // shorter than one frame is otherwise invisible to a per-frame poll,
    // which is how scripted G/H/F4 presses were lost in the garage.
    const bool tapped = (state & 0x0001) != 0;
    const bool pressed =
        (down && previous[static_cast<size_t>(key)] == 0u) ||
        (tapped && !down);
    previous[static_cast<size_t>(key)] = down ? 1u : 0u;
    return pressed;
}

void LaunchValidationBundleExporter() {
    char executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(
        nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    if (length == 0u || length >= sizeof(executable)) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "cannot locate the game directory for bundle export");
        return;
    }
    char* separator = std::strrchr(executable, '\\');
    if (!separator) return;
    *separator = '\0';
    std::string script = executable;
    script += "\\_mod_tools\\mod_api\\tools\\export_live_validation_bundle.ps1";
    if (GetFileAttributesA(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "validation bundle exporter script is missing");
        return;
    }
    SYSTEMTIME now = {};
    GetSystemTime(&now);
    char filename[96] = {};
    sprintf_s(
        filename,
        "ValidationBundle-%04u%02u%02u-%02u%02u%02u.zip",
        static_cast<unsigned>(now.wYear),
        static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute),
        static_cast<unsigned>(now.wSecond));
    std::string output;
    if (!JoinPath(g_data_directory, filename, &output)) return;
    if (script.find('"') != std::string::npos ||
        g_data_directory.find('"') != std::string::npos ||
        output.find('"') != std::string::npos) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "unsafe quote in validation exporter path");
        return;
    }
    std::string command =
        "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        "-File \"" + script + "\" -DataDirectory \"" +
        g_data_directory + "\" -OutputPath \"" + output + "\"";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(
            nullptr,
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            executable,
            &startup,
            &process)) {
        Log(WOTBMOD_V3_LOG_ERROR,
            "validation bundle exporter failed to start");
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Log(WOTBMOD_V3_LOG_INFO,
        "validation bundle export started: " + output);
}

void PollExportButton() {
    if (!KeyPressed(VK_LBUTTON)) return;
    const HWND window = GetForegroundWindow();
    if (!window) return;
    DWORD process_id = 0u;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != GetCurrentProcessId()) return;
    POINT cursor = {};
    if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor)) return;
    if (cursor.x < g_export_button_left.load() ||
        cursor.x > g_export_button_right.load() ||
        cursor.y < g_export_button_top.load() ||
        cursor.y > g_export_button_bottom.load()) {
        return;
    }
    LaunchValidationBundleExporter();
}

void PollPanelKeys() {
    if (g_panel_visible.load() == 0u) return;
    if (KeyPressed(VK_F4)) LaunchValidationBundleExporter();
    bool settings_changed = false;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (KeyPressed(VK_TAB)) {
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
            g_selected_section = g_selected_section == 0u
                ? kSectionCount - 1u : g_selected_section - 1u;
        } else {
            g_selected_section = (g_selected_section + 1u) % kSectionCount;
        }
        g_selected_in_section = 0u;
        settings_changed = true;
    }
    if (!shift && !ctrl && KeyPressed(VK_LEFT)) {
        g_panel_focus = g_panel_focus == 0u ? 2u : g_panel_focus - 1u;
    }
    if (!shift && !ctrl && KeyPressed(VK_RIGHT)) {
        g_panel_focus = (g_panel_focus + 1u) % 3u;
    }
    const std::vector<size_t> indices = SectionCapabilities(g_selected_section);
    if (g_panel_focus == 0u && KeyPressed(VK_UP)) {
        g_selected_section = g_selected_section == 0u ? kSectionCount - 1u : g_selected_section - 1u;
        g_selected_in_section = 0u;
        settings_changed = true;
    }
    if (g_panel_focus == 0u && KeyPressed(VK_DOWN)) {
        g_selected_section = (g_selected_section + 1u) % kSectionCount;
        g_selected_in_section = 0u;
        settings_changed = true;
    }
    if (!indices.empty() && g_panel_focus == 1u) {
        if (KeyPressed(VK_UP)) {
            g_selected_in_section = g_selected_in_section == 0u
                ? indices.size() - 1u : g_selected_in_section - 1u;
            settings_changed = true;
        }
        if (KeyPressed(VK_DOWN)) {
            g_selected_in_section = (g_selected_in_section + 1u) % indices.size();
            settings_changed = true;
        }
        if (KeyPressed(VK_HOME)) g_selected_in_section = 0u;
        if (KeyPressed(VK_END)) g_selected_in_section = indices.size() - 1u;
    }
    const size_t selected = SelectedCapability();
    if (KeyPressed(VK_SPACE) && selected < kCapabilityCount) {
        g_test_enabled[selected] = g_test_enabled[selected] ? 0u : 1u;
        settings_changed = true;
    }
    if (KeyPressed(VK_F6)) RunSafeTest(selected);
    if (KeyPressed('H')) {
        RunAllHookProbes();
        settings_changed = true;
    }
    if (KeyPressed('G')) {
        RunAllSafeTests();
        settings_changed = true;
    }
    if (KeyPressed(VK_F7)) MarkManual(selected, ManualVerdict::Pass);
    if (KeyPressed(VK_F8)) MarkManual(selected, ManualVerdict::Fail);
    if (KeyPressed(VK_F9)) MarkManual(selected, ManualVerdict::Skip);
    if (KeyPressed(VK_F10)) ResetCapability(selected);
    if (KeyPressed(VK_F11)) {
        const size_t camera = FindCapability("camera.mode_change");
        if (camera < kCapabilityCount) MarkManual(camera, ManualVerdict::Pass);
    }
    if (KeyPressed(VK_F12) && selected < kCapabilityCount) {
        g_states[selected].crash_disabled = false;
        if (g_states[selected].verdict == ManualVerdict::Fail) {
            g_states[selected].verdict = ManualVerdict::None;
        }
        g_states[selected].status = kCapabilities[selected].native
            ? ValidationStatus::LiveTestPending
            : kCapabilities[selected].baseline;
        g_states[selected].last_error.clear();
        g_states[selected].last_result = "manually re-enabled after crash marker";
        settings_changed = true;
    }
    if (KeyPressed('U')) {
        g_unsafe_confirmed = !g_unsafe_confirmed;
        QueueLiveLog(selected, LiveSeverity::Warn, "validation.unsafe_confirmation",
            g_unsafe_confirmed ? "unsafe confirmation enabled for this session"
                               : "unsafe confirmation disabled", false);
    }
    if (KeyPressed('A')) {
        ApplyPreset(9u);
        settings_changed = true;
    }
    if (KeyPressed('N')) {
        SetAllTests(false, false);
        settings_changed = true;
    }
    if (KeyPressed('S')) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (size_t index = 0u; index < kCapabilityCount; ++index) {
            g_test_enabled[index] = !kCapabilities[index].deliberately_unsupported &&
                g_states[index].backend_available ? 1u : 0u;
        }
        settings_changed = true;
    }
    if (KeyPressed('C')) {
        std::lock_guard<std::mutex> lock(g_live_log_mutex);
        g_live_log.clear();
        settings_changed = true;
    }
    if (KeyPressed('K')) {
        g_log_paused = !g_log_paused;
        settings_changed = true;
    }
    if (KeyPressed('O')) {
        g_log_autoscroll = !g_log_autoscroll;
        settings_changed = true;
    }
    if (KeyPressed('X')) {
        g_log_selected_only = !g_log_selected_only;
        settings_changed = true;
    }
    if (KeyPressed('L')) {
        g_log_filter = (g_log_filter + 1u) % 7u;
        settings_changed = true;
    }
    if (KeyPressed('M')) {
        g_sound_mode = static_cast<SoundNotificationMode>(
            (static_cast<uint32_t>(g_sound_mode) + 1u) % 4u);
        settings_changed = true;
    }
    if (KeyPressed('B')) {
        g_battle_mute = !g_battle_mute;
        settings_changed = true;
    }
    if (KeyPressed('E')) ExportReadableReport();
    if (KeyPressed('Y')) CopySelectedLogLine(selected);
    if (KeyPressed(VK_OEM_MINUS)) {
        g_sound_volume = std::clamp(g_sound_volume - 0.02f, 0.0f, 1.0f);
        settings_changed = true;
    }
    if (KeyPressed(VK_OEM_PLUS) || KeyPressed(VK_ADD)) {
        g_sound_volume = std::clamp(g_sound_volume + 0.02f, 0.0f, 1.0f);
        settings_changed = true;
    }
    if (KeyPressed('R')) ResetCapability(selected);
    for (int key = '0'; key <= '9'; ++key) {
        if (!KeyPressed(key)) continue;
        const uint32_t preset = key == '0' ? 10u : static_cast<uint32_t>(key - '1');
        ApplyPreset(preset);
        settings_changed = true;
    }
    if (shift && (GetAsyncKeyState(VK_LEFT) & 0x8000)) {
        g_panel_width = std::max<int32_t>(800, g_panel_width - 32);
        settings_changed = true;
    }
    if (shift && (GetAsyncKeyState(VK_RIGHT) & 0x8000)) {
        g_panel_width = std::min<int32_t>(4000, g_panel_width + 32);
        settings_changed = true;
    }
    if (shift && (GetAsyncKeyState(VK_UP) & 0x8000)) {
        g_panel_height = std::max<int32_t>(600, g_panel_height - 32);
        settings_changed = true;
    }
    if (shift && (GetAsyncKeyState(VK_DOWN) & 0x8000)) {
        g_panel_height = std::min<int32_t>(4000, g_panel_height + 32);
        settings_changed = true;
    }
    if (ctrl && (GetAsyncKeyState(VK_LEFT) & 0x8000)) {
        g_panel_x = std::max<int32_t>(0, g_panel_x - 16);
        settings_changed = true;
    }
    if (ctrl && (GetAsyncKeyState(VK_RIGHT) & 0x8000)) {
        g_panel_x = std::min<int32_t>(4000, g_panel_x + 16);
        settings_changed = true;
    }
    if (ctrl && (GetAsyncKeyState(VK_UP) & 0x8000)) {
        g_panel_y = std::max<int32_t>(0, g_panel_y - 16);
        settings_changed = true;
    }
    if (ctrl && (GetAsyncKeyState(VK_DOWN) & 0x8000)) {
        g_panel_y = std::min<int32_t>(4000, g_panel_y + 16);
        settings_changed = true;
    }
    if (settings_changed) {
        g_snapshots_dirty.store(1u);
    }
}

void CleanupRuntime() {
    if (g_handles &&
        g_panel_action_token != WOTBMOD_V3_INVALID_HANDLE) {
        g_handles->release(g_mod, g_panel_action_token);
        g_panel_action_token = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_input && g_panel_action != WOTBMOD_V3_INVALID_HANDLE) {
        g_input->unregister_action(g_mod, g_panel_action);
        g_panel_action = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_events) {
        for (WotbModV3EventToken token : g_system_event_tokens) {
            if (token != WOTBMOD_V3_INVALID_HANDLE) {
                g_events->unsubscribe(g_mod, token);
            }
        }
    }
    g_system_event_tokens.clear();
    if (g_events && g_self_event_token != WOTBMOD_V3_INVALID_HANDLE) {
        g_events->unsubscribe(g_mod, g_self_event_token);
        g_self_event_token = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_render && g_render_token != WOTBMOD_V3_INVALID_HANDLE) {
        g_panel_visible.store(0u);
        g_export_button_left.store(0);
        g_export_button_top.store(0);
        g_export_button_right.store(0);
        g_export_button_bottom.store(0);
        g_render->unregister_callback(g_mod, g_render_token);
        g_render_token = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_render && g_panel_background_texture != WOTBMOD_V3_INVALID_HANDLE) {
        g_render->destroy_texture(g_mod, g_panel_background_texture);
        g_panel_background_texture = WOTBMOD_V3_INVALID_HANDLE;
    }
    g_panel_background_attempted = false;
    if (g_resources && g_async_resource != WOTBMOD_V3_INVALID_HANDLE) {
        g_resources->release(g_mod, g_async_resource);
        g_async_resource = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_vfs && g_probe_mount != WOTBMOD_V3_INVALID_HANDLE) {
        g_vfs->unmount(g_mod, g_probe_mount);
        g_probe_mount = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_vfs && g_audio_mount != WOTBMOD_V3_INVALID_HANDLE) {
        g_vfs->unmount(g_mod, g_audio_mount);
        g_audio_mount = WOTBMOD_V3_INVALID_HANDLE;
    }
}

void SubscribeEvents() {
    if (!g_events) return;
    // Subscribe only to events that have validation cases. The former
    // `wotbmod.*` wildcard also delivered frame.update at the render rate.
    const std::array<const char*, 17u> system_topics = {{
        WOTBMOD_V3_EVENT_VEHICLE_KILLED,
        WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED,
        WOTBMOD_V3_EVENT_RPC_OBSERVED,
        WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED,
        WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED,
        WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED,
        WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED,
        WOTBMOD_V3_EVENT_SHOT_FIRED,
        WOTBMOD_V3_EVENT_PROJECTILE_CREATED,
        WOTBMOD_V3_EVENT_PROJECTILE_UPDATED,
        WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED,
        WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED,
        WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED,
        WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED,
        WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED,
        WOTBMOD_V3_EVENT_SHELL_HIT,
        "wotbmod.render.*"
    }};
    WotbModV3Result system_result = WOTBMOD_V3_E_NOT_SUPPORTED;
    for (const char* topic : system_topics) {
        WotbModV3EventSubscriptionInfo system = {};
        WOTBMOD_V3_INIT_STRUCT(system, WOTBMOD_V3_EVENTS_VERSION);
        system.topic_pattern = topic;
        system.priority = WOTBMOD_V3_EVENT_PRIORITY_LOWEST;
        system.receive_system_events = 1u;
        WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result result = g_events->subscribe(
            g_mod, &system, &EventCallback, nullptr, &token);
        if (result == WOTBMOD_V3_OK) {
            g_system_event_tokens.push_back(token);
            system_result = WOTBMOD_V3_OK;
        } else {
            Log(WOTBMOD_V3_LOG_WARNING,
                ("system topic subscription refused: " + std::string(topic) + " -> " +
                 ResultName(result) + " (" + LastErrorMessage() + ")").c_str());
            if (system_result != WOTBMOD_V3_OK &&
                result != WOTBMOD_V3_E_NOT_SUPPORTED) {
                system_result = result;
            }
        }
    }
    RecordResult(
        FindCapability("events.loader_queue"), system_result,
        "targeted system subscriptions=" +
            std::to_string(g_system_event_tokens.size()));

    // The whole GES bus through one pattern: the runtime observes every type
    // that has a subscriber, so this single subscription is what makes the
    // loader register its listener on all of them.
    g_ges_events.store(0u);
    g_ges_echo_seen.store(0u);
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_ges_types_seen.clear();
    }
    if (g_ges && g_events) {
        WotbModV3EventSubscriptionInfo ges_all = {};
        WOTBMOD_V3_INIT_STRUCT(ges_all, WOTBMOD_V3_EVENTS_VERSION);
        ges_all.topic_pattern = "wotbmod.ges.*";
        ges_all.priority = WOTBMOD_V3_EVENT_PRIORITY_LOWEST;
        ges_all.receive_system_events = 1u;
        WotbModV3EventToken ges_token = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result ges_result = g_events->subscribe(
            g_mod, &ges_all, &EventCallback, nullptr, &ges_token);
        if (ges_result == WOTBMOD_V3_OK) g_system_event_tokens.push_back(ges_token);
        RecordResult(FindCapability("ges.observe_all"), ges_result,
                     ges_result == WOTBMOD_V3_OK ? "subscribed wotbmod.ges.*"
                                                 : LastErrorMessage());
    } else {
        RecordResult(FindCapability("ges.observe_all"), WOTBMOD_V3_E_NOT_SUPPORTED,
                     "wotbmod.ges unavailable: " + LastErrorMessage());
    }

    WotbModV3EventSubscriptionInfo self = {};
    WOTBMOD_V3_INIT_STRUCT(self, WOTBMOD_V3_EVENTS_VERSION);
    self.topic_pattern = "mod.wotbmod.native_validation.self";
    self.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    self.receive_system_events = 0u;
    const WotbModV3Result self_result = g_events->subscribe(
        g_mod, &self, &EventCallback, nullptr, &g_self_event_token);
    if (self_result != WOTBMOD_V3_OK) {
        RecordResult(FindCapability("events.loader_queue"), self_result,
                     "self event queue subscription");
    }
}

void CapturePreexistingEntities() {
    if (!g_entities) return;
    EntityVisitContext context;
    context.preexisting = true;
    const WotbModV3Result result = g_entities->enumerate_visible(
        g_mod, &EntityVisitor, &context);
    RecordResult(
        FindCapability("entity.enumerate_visible"), result,
        result == WOTBMOD_V3_OK
            ? "preexisting visible entities=" + std::to_string(context.count)
            : LastErrorMessage(),
        WOTBMOD_V3_INVALID_HANDLE, context.count);
}

bool RegisterRenderPanel() {
    if (!g_render) return false;
    if (g_render_token != WOTBMOD_V3_INVALID_HANDLE) {
        g_panel_visible.store(1u);
        return true;
    }
    WotbModV3Result result = g_render->register_callback(
        // PRESENT is the only real client ingress.  Use the lowest priority
        // so the test-center backdrop/text is the last overlay and remains
        // readable over catalog/garage panels from other mods.
        g_mod, WOTBMOD_V3_RENDER_PHASE_AFTER_UI, -100000,
        &RenderCallback, nullptr, &g_render_token);
    if (result == WOTBMOD_V3_E_NOT_SUPPORTED) {
        result = g_render->register_callback(
            g_mod, WOTBMOD_V3_RENDER_PHASE_PRESENT, 100000,
            &RenderCallback, nullptr, &g_render_token);
    }
    RecordResult(FindCapability("ui.validation_panel"), result,
                 result == WOTBMOD_V3_OK
                    ? "on-demand render panel registered"
                    : "render panel backend unavailable");
    if (result == WOTBMOD_V3_OK) {
        g_panel_draw_diagnostic.store(0u);
        g_panel_visible.store(1u);
        return true;
    }
    g_panel_visible.store(0u);
    g_render_token = WOTBMOD_V3_INVALID_HANDLE;
    return false;
}

void QueryInterfaces() {
    g_core = QueryApi<WotbModV3CoreApiV1>(WOTBMOD_V3_IFACE_CORE, WOTBMOD_V3_CORE_VERSION);
    g_handles = QueryApi<WotbModV3HandlesApiV1>(WOTBMOD_V3_IFACE_HANDLES, WOTBMOD_V3_HANDLES_VERSION);
    g_lifecycle = QueryApi<WotbModV3LifecycleApiV1>(WOTBMOD_V3_IFACE_LIFECYCLE, WOTBMOD_V3_LIFECYCLE_VERSION);
    g_events = QueryApi<WotbModV3EventsApiV1>(WOTBMOD_V3_IFACE_EVENTS, WOTBMOD_V3_EVENTS_VERSION);
    g_async = QueryApi<WotbModV3AsyncApiV1>(WOTBMOD_V3_IFACE_ASYNC, WOTBMOD_V3_ASYNC_VERSION);
    g_vfs = QueryApi<WotbModV3VfsApiV1>(WOTBMOD_V3_IFACE_VFS, WOTBMOD_V3_VFS_VERSION);
    g_resources = QueryApi<WotbModV3ResourcesApiV1>(WOTBMOD_V3_IFACE_RESOURCES, WOTBMOD_V3_RESOURCES_VERSION);
    g_yaml = QueryApi<WotbModV3YamlApiV1>(WOTBMOD_V3_IFACE_YAML, WOTBMOD_V3_YAML_VERSION);
    g_archive = QueryApi<WotbModV3ArchiveApiV1>(WOTBMOD_V3_IFACE_ARCHIVE, WOTBMOD_V3_ARCHIVE_VERSION);
    g_loaders = QueryApi<WotbModV3LoadersApiV1>(WOTBMOD_V3_IFACE_LOADERS, WOTBMOD_V3_LOADERS_VERSION);
    g_render = QueryApi<WotbModV3RenderApiV1>(WOTBMOD_V3_IFACE_RENDER, WOTBMOD_V3_RENDER_VERSION);
    g_render_native = QueryApi<WotbModV3RenderNativeApiV1>(WOTBMOD_V3_IFACE_RENDER_NATIVE, WOTBMOD_V3_RENDER_NATIVE_VERSION);
    g_camera = QueryApi<WotbModV3CameraApiV1>(WOTBMOD_V3_IFACE_CAMERA, WOTBMOD_V3_CAMERA_VERSION);
    g_audio = QueryApi<WotbModV3AudioApiV2>(WOTBMOD_V3_IFACE_AUDIO, WOTBMOD_V3_AUDIO_VERSION);
    g_entities = QueryApi<WotbModV3EntityPublicApiV1>(WOTBMOD_V3_IFACE_ENTITY_PUBLIC, WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    g_rpc = QueryApi<WotbModV3BigWorldRpcApiV1>(WOTBMOD_V3_IFACE_BIGWORLD_RPC, WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    g_projectile = QueryApi<WotbModV3ProjectileApiV2>(WOTBMOD_V3_IFACE_PROJECTILE, WOTBMOD_V3_PROJECTILE_VERSION_2);
    g_tracer = QueryApi<WotbModV3TracerApiV1>(WOTBMOD_V3_IFACE_TRACER, WOTBMOD_V3_TRACER_VERSION);
    g_ui = QueryApi<WotbModV3UiApiV3>(WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION_3);
    g_ui_read = QueryApi<WotbModV3UiApiV4>(WOTBMOD_V3_IFACE_UI_READ, WOTBMOD_V3_UI_VERSION_4);
    g_hud = QueryApi<WotbModV3GameplayHudApiV1>(WOTBMOD_V3_IFACE_GAMEPLAY_HUD, WOTBMOD_V3_GAMEPLAY_HUD_VERSION);
    if (!g_hud) {
        Log(WOTBMOD_V3_LOG_WARNING,
            ("gameplay.hud interface not granted at enable: " + LastErrorMessage()).c_str());
    }
    g_client = QueryApi<WotbModV3ClientApiV1>(WOTBMOD_V3_IFACE_CLIENT, WOTBMOD_V3_CLIENT_VERSION);
    g_device = QueryApi<WotbModV3DeviceApiV1>(WOTBMOD_V3_IFACE_DEVICE, WOTBMOD_V3_DEVICE_VERSION);
    g_diagnostics = QueryApi<WotbModV3DiagnosticsApiV2>(WOTBMOD_V3_IFACE_DIAGNOSTICS, WOTBMOD_V3_DIAGNOSTICS_VERSION_2);
    g_devtools = QueryApi<WotbModV3DevtoolsApiV3>(WOTBMOD_V3_IFACE_DEVTOOLS, WOTBMOD_V3_DEVTOOLS_VERSION_3);
    g_input = QueryApi<WotbModV3InputApiV1>(WOTBMOD_V3_IFACE_INPUT, WOTBMOD_V3_INPUT_VERSION);
    g_hooks = QueryApi<WotbModV3HooksApiV1>(WOTBMOD_V3_IFACE_HOOKS, WOTBMOD_V3_HOOKS_VERSION);
    g_ges = QueryApi<WotbModV3GesApiV1>(WOTBMOD_V3_IFACE_GES, WOTBMOD_V3_GES_VERSION);
    g_session_cluster = QueryApi<WotbModV3SessionClusterApiV1>(
        WOTBMOD_V3_IFACE_SESSION_CLUSTER, WOTBMOD_V3_SESSION_CLUSTER_VERSION);
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) noexcept {
    try {
        g_entity_observations.clear();
        {
            std::lock_guard<std::mutex> lock(g_shot_chains_mutex);
            g_shot_chains.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_live_log_mutex);
            g_live_log.clear();
        }
        g_pending_pass_sound.store(0u);
        g_pending_warn_sound.store(0u);
        g_pending_fail_sound.store(0u);
        g_last_sound_ms.store(0u);
        g_last_live_event_ms.store(0u);
        g_unsafe_confirmed = false;
        g_system_event_tokens.clear();
        g_panel_visible.store(0u);
        g_snapshots_dirty.store(0u);
        g_panel_draw_diagnostic.store(0u);
        g_export_button_left.store(0);
        g_export_button_top.store(0);
        g_export_button_right.store(0);
        g_export_button_bottom.store(0);
        g_render_calls.store(0u);
        g_render_thread.store(0u);
        g_render_backend.store(WOTBMOD_V3_RENDER_BACKEND_NONE);
        g_render_width.store(0u);
        g_render_height.store(0u);
        g_render_resize_count.store(0u);
        g_swapchain_recreation_count.store(0u);
        g_device_lost_count.store(0u);
        g_device_restored_count.store(0u);
        g_native_device_present.store(0u);
        g_native_context_present.store(0u);
        g_native_swapchain_present.store(0u);
        g_callback_count.store(0u);
        g_callback_total_100ns.store(0u);
        g_callback_max_100ns.store(0u);
        g_trace_count.store(0u);
        g_trace_bytes.store(0u);
        g_snapshot_writes.store(0u);
        g_draw_calls.store(0u);
        g_callbacks_per_second = 0.0;
        g_trace_per_second = 0.0;
        g_bytes_per_second = 0.0;
        g_snapshot_writes_per_minute = 0.0;
        g_draw_calls_per_frame = 0.0;
        g_perf_sample_ms = 0u;
        g_perf_sample_callbacks = 0u;
        g_perf_sample_trace = 0u;
        g_perf_sample_bytes = 0u;
        g_perf_sample_snapshots = 0u;
        g_panel_action = WOTBMOD_V3_INVALID_HANDLE;
        g_panel_action_token = WOTBMOD_V3_INVALID_HANDLE;
        g_camera_birth_ms = 0u;
        g_camera_last_handle = WOTBMOD_V3_INVALID_HANDLE;
        g_camera_previous_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
        g_camera_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
        g_camera_event_count = 0u;
        g_camera_duplicate_count = 0u;
        g_bootstrap = bootstrap;
        g_mod = mod;
        QueryInterfaces();
        InitializePaths();
        InitializeFingerprint();
        InitializeCapabilityStates();
        InitializeTestSelection();
        LoadResults();
        ProcessReloadGeneration();
        LoadLiveSettings();
        StartLogWorker();
        const size_t stress_index = FindCapability("lifecycle.stress_30");
        if (stress_index < kCapabilityCount) {
            CapabilityState& stress = g_states[stress_index];
            stress.call_count = g_load_cycle_count;
            stress.last_code = WOTBMOD_V3_OK;
            stress.last_result = "clean load cycles=" +
                std::to_string(g_load_cycle_count) + "/30";
            if (g_load_cycle_count >= 30u) {
                stress.status = ValidationStatus::StressTested;
            }
        }
        RecoverCrashMarker();
        RegisterPanelInput();
        SubscribeEvents();
        CapturePreexistingEntities();
        RefreshClientState();
        SaveSnapshots();
        Trace("lifecycle.info", "validation.enabled", kModId,
              g_mod, HandleGeneration(g_mod), "enabled", "", "{}");
        Log(WOTBMOD_V3_LOG_INFO,
            "native validation ready; F5 panel, F6 test, F7/F8/F9/F10 verdicts");
        ArmAutoActions();
        if (g_autosweep_at_ms || g_autoprobe_at_ms) RegisterRenderPanel();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_FATAL, "OnEnable contained an exception");
    }
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap*,
    WotbModV3Handle) noexcept {
    try {
        Trace("lifecycle.cleanup", "validation.disabling", kModId,
              g_mod, HandleGeneration(g_mod), "cleanup", "", "{}");
        // The stock HUD tweaks belong to this module instance; a hot reload
        // or unload must not leave the battle screen tinted or trimmed.
        if (g_hud) g_hud->reset(g_mod);
        SaveSnapshots();
        CleanupRuntime();
        StopLogWorker();
        ClearCrashMarker();
    } catch (...) {
        Log(WOTBMOD_V3_LOG_ERROR, "OnDisable contained an exception");
    }
}

void WOTBMOD_V3_CALL OnUnload(
    const WotbModV3Bootstrap*,
    WotbModV3Handle) noexcept {
    try {
        SaveSnapshots();
        CleanupRuntime();
        StopLogWorker();
        ClearCrashMarker();
        g_bootstrap = nullptr;
        g_mod = WOTBMOD_V3_INVALID_HANDLE;
    } catch (...) {
        /* No exception may cross the C ABI during unload. */
    }
}

}  // namespace

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbNativeValidation_GetCapabilityCount() noexcept {
    return static_cast<uint32_t>(kCapabilityCount);
}

WOTBMOD_V3_EXPORT const char* WOTBMOD_V3_CALL
WotbNativeValidation_GetCapabilityId(uint32_t index) noexcept {
    return index < kCapabilityCount ? kCapabilities[index].id : nullptr;
}

WOTBMOD_V3_EXPORT const char* WOTBMOD_V3_CALL
WotbNativeValidation_GetCapabilitySection(uint32_t index) noexcept {
    return index < kCapabilityCount ? kCapabilities[index].section : nullptr;
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbNativeValidation_GetCapabilityNative(uint32_t index) noexcept {
    return index < kCapabilityCount && kCapabilities[index].native ? 1u : 0u;
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbNativeValidation_GetRuntimeContractFlags() noexcept {
    // 1=no hidden render callback, 2=no on_frame callback,
    // 4=dirty/coalesced snapshots, 8=no durable append flush.
    return 0x0fu;
}

WOTBMOD_V3_EXPORT uint64_t WOTBMOD_V3_CALL
WotbNativeValidation_GetPerfCounter(uint32_t index) noexcept {
    switch (index) {
        case 0u: return g_callback_count.load();
        case 1u: return g_trace_count.load();
        case 2u: return g_trace_bytes.load();
        case 3u: return g_snapshot_writes.load();
        case 4u: return g_draw_calls.load();
        case 5u: return g_callback_total_100ns.load();
        case 6u: return g_callback_max_100ns.load();
        default: return 0u;
    }
}

WOTBMOD_V3_EXPORT const char* WOTBMOD_V3_CALL
WotbNativeValidation_StatusAfterVerdict(
    uint32_t native_capability,
    const char* verdict) noexcept {
    if (verdict && _stricmp(verdict, "PASS") == 0) {
        return native_capability ? "LIVE_TEST_PENDING" : "HOST_TESTED";
    }
    if (verdict && _stricmp(verdict, "FAIL") == 0) return "FAILED";
    return native_capability ? "LIVE_TEST_PENDING" : "HOST_TESTED";
}

WOTBMOD_V3_EXPORT const char* WOTBMOD_V3_CALL
WotbNativeValidation_StatusAfterFingerprintVerdict(
    uint32_t native_capability,
    const char* verdict,
    const char* evidence_fingerprint,
    const char* current_fingerprint) noexcept {
    if (verdict && _stricmp(verdict, "FAIL") == 0) return "FAILED";
    if (native_capability && verdict &&
        _stricmp(verdict, "PASS") == 0 &&
        evidence_fingerprint && current_fingerprint &&
        evidence_fingerprint[0] &&
        std::strcmp(evidence_fingerprint, current_fingerprint) == 0) {
        return "SUPPORTED";
    }
    return native_capability ? "LIVE_TEST_PENDING" : "HOST_TESTED";
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbNativeValidation_SanitizeComment(
    const char* input,
    char* output,
    uint32_t capacity) noexcept {
    try {
        if (!output || capacity == 0u) return 0u;
        const std::string sanitized = SanitizeComment(input ? input : "");
        if (sanitized.size() + 1u > capacity) return 0u;
        CopyText(output, capacity, sanitized.c_str());
        return static_cast<uint32_t>(sanitized.size() + 1u);
    } catch (...) {
        return 0u;
    }
}

WOTBMOD_V3_ENTRY {
    try {
        if (!bootstrap || !out_info ||
            bootstrap->struct_size < sizeof(WotbModV3Bootstrap) ||
            bootstrap->api_version != WOTBMOD_V3_ABI_VERSION ||
            !bootstrap->query_interface || !bootstrap->get_interface_info ||
            !bootstrap->get_last_error || !bootstrap->get_client_info ||
            mod == WOTBMOD_V3_INVALID_HANDLE) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        std::memset(out_info, 0, sizeof(*out_info));
        out_info->struct_size = sizeof(*out_info);
        out_info->api_version = WOTBMOD_V3_ABI_VERSION;
        out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_UNSAFE;
        CopyText(out_info->id, sizeof(out_info->id), kModId);
        CopyText(out_info->name, sizeof(out_info->name), "Native Validation");
        CopyText(out_info->version, sizeof(out_info->version), kModVersion);
        CopyText(out_info->author, sizeof(out_info->author), "BlitzForge SDK");
        CopyText(
            out_info->description, sizeof(out_info->description),
            "Internal exact-fingerprint live validation panel and privacy-bounded JSONL evidence recorder.");
        out_info->on_enable = &OnEnable;
        out_info->on_disable = &OnDisable;
        out_info->on_unload = &OnUnload;
        out_info->on_frame = nullptr;
        return WOTBMOD_V3_OK;
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}
