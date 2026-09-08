# WoT Blitz Mod API

Clean, UI-independent SDK and runtime core for a Geode-style native mod
loader. This project does not depend on or integrate with the old mod under
`proxy_dll`.

Полная русскоязычная документация: [`docs/API_RU.md`](docs/API_RU.md).

V3 RC1 заморожен для стабилизации: публичные ABI, permission names и schema
зафиксированы в [`API_V3_RC1_FREEZE.md`](API_V3_RC1_FREEZE.md). Ручной
in-client прогон описан в
[`MANUAL_LIVE_VALIDATION_RU.md`](MANUAL_LIVE_VALIDATION_RU.md), а оставшиеся
native-блокеры — в [`RC1_BLOCKERS.md`](RC1_BLOCKERS.md). До завершения этого
прогона native capability сохраняют статус `LIVE_TEST_PENDING`.

Lua host документирован отдельно: [`docs/LUA_MODS_RU.md`](docs/LUA_MODS_RU.md).
Он поставляется обычным native `.wotbmod`, запускает manifest-based Lua-моды из
`mods\lua` и поддерживает отдельную `lua-dev` папку с hot reload для разработки.
Готовый интерактивный пример UI находится в
[`examples/lua_ui_framework`](examples/lua_ui_framework). Ангарный пример
полноценной статистики за сессию — в
[`examples/lua_session_stats`](examples/lua_session_stats); он намеренно
показывает `N/A` для метрик, которые текущий native bridge не подтверждает.

Первый распространяемый результат имеет статус `0.1.0-public-preview`, а не
stable. `tools\build_public_preview.ps1` собирает exact-client win32 bundle,
подписывает Lua host неэкспортируемым Windows CNG ECDSA P-256 ключом и добавляет
hash-verifying install/update/verify/uninstall scripts с защитой чужих DLL и
восстановимым state. Bundle поддерживает только WoT Blitz `11.19.0.834` x86 с
опубликованным SHA-256; другая сборка отклоняется до изменения файлов.

## What is included

- A versioned, C-compatible ABI in `include/wotb_mod_api.h`.
- The modular V3 C ABI in `include/wotb_mod_api_v3.h` and an optional
  exception-free, header-only C++17 view in `include/wotbmod/wotbmod.hpp`
  with typed interface queries and ownership-aware `OwnedHandle` RAII.
- A static runtime core that discovers and loads `mods\*.dll`.
- Lifecycle callbacks: load, enable, frame, disable, and unload.
- Per-mod logging, data directories, and isolated INI configuration.
- Optional host-owned hook operations through an architecture-supplied adapter.
- RVA, loaded-export, and byte-pattern lookup.
- Mod enumeration and enable/disable controls for a separate UI layer.
- Priority-based loose/DVPL resource mounts scoped to each enabled mod.
- Typed UI package/control, YAML, scene, texture, audio clip, and generic loads
  through a loader-owned DAVA bridge.
- Mod-owned audio playback with loop, 2D pan, 3D position/distance, volume,
  pitch, pause/resume/stop, state query, and backend fault isolation.
- A ready Windows custom-audio backend based on Media Foundation and XAudio2
  for loose WAV, MP3, AAC/M4A, and WMA files.
- ABI 2.4 native `DAVA::SoundSystem` events: create, trigger, stop,
  pause/resume, volume, 3D position, RTPC parameters, state, name, and
  ownership-safe release.
- ABI 2.5 main-thread FIFO dispatch plus typed `UIControl` geometry,
  visibility and hierarchy operations, and Scene local transform/hierarchy
  operations.
- ABI 2.6 creation of empty native `UIControl`/`Entity` objects and owned
  access to the active UI screen and active rendered 3D Scene.
- ABI 2.7 subscriptions for active UI-screen and Scene lifecycle events,
  with callback-scoped borrowed handles and explicit `resource_clone`.
- ABI 2.8 traversal and mutation of the existing UI tree, typed vehicle
  snapshots, and battle/vehicle/shot/health/reload event payloads.
- ABI 2.9 stock vehicle skin registration: exact-path replacement of tank
  `.sc2`, `.scg`, material/FX YAML and `.tex` resources through mod-owned
  mounts or existing stock `~res:/` resources.
- A version-specific `WotbModDavaSound` bridge for the verified x86 client,
  including the live `DAVA::SoundSystemProxy` forwarding layer.
- Automatic release of audio playbacks and loaded resources, plus removal of
  mounts, when a mod is disabled, faults, or unloads.
- Callback crash isolation: an SEH fault disables only the failing mod and
  removes hooks owned by it.
- Persistent crash-loop recovery: an unclean session marker starts the next
  launch in safe mode before any third-party package or loose DLL is loaded.
- A minimal example mod and a standalone smoke host.
- Standalone publishable API/audio self-test mods and an integration host that
  runs them against the ready Windows audio backend.

The shipping SDK does not expose an exhaustive raw-address table. Native entry
points are selected from the exact-build binding pack and published to mods
only through the reviewed symbolic hook registry; unknown symbols fail closed.
The current registry contains only the targets for which the public operation,
permission and native signature are documented.

Legacy process/RVA/export/pattern lookup and raw hooks remain in isolated
contract tests and developer-unsafe builds, but the shipping loader does not
expose them. Hidden-enemy state, aim/autofire, server-packet mutation and
anti-cheat bypass do not exist in the stable API.

## Build and verify

Run:

```bat
build.cmd
```

The command builds the x86 static runtime and example DLL, checks both public
headers with a C compiler, then runs the original smoke test and the adversarial
full-contract suite. The latter exercises every host/runtime function, invalid
arguments, limits, ownership, backend failures, malformed mod DLLs, lifecycle
cleanup, resource/audio limits, resource concurrency, and runtime
reinitialization. Outputs:

- `build\wotb_mod_runtime.lib`
- `build\hello_mod.dll`
- `build\smoke_host.exe`
- `build\api_full_host.exe`
- `build\published_selftest_host.exe`
- `build\new_api_mods_host.exe`
- `build\new_gameplay_events_test_mod.dll`
- `build\vehicle_skin_test_mod.dll`
- `build\object260_t3485_model_mod.dll`
- `build\rc1_packages\sample.ui_transaction.wotbmod`
- `build\rc1_packages\sample.vehicle_cosmetic.wotbmod`
- `build\rc1_packages\sample.camera_render.wotbmod`
- `build\public_preview\WotbMod-API-0.1.0-preview.1-win32.zip`

RC1 также включает signed revocation list для package trust store, crash-loop
safe mode, privacy-safe validation bundle export и три standalone sample-мода.
Sample-пакеты являются unsigned developer examples: при включённой mandatory
signature policy их необходимо подписать доверенным ключом перед установкой.
Lua host внутри public-preview bundle подписан; исходные Lua-примеры включены
отдельно и намеренно не устанавливаются автоматически. Имена example-каталогов
в bundle совпадают с `manifest.id`, поэтому их можно копировать в `mods\lua`
без переименования. Финальный exact-client replay подтвердил trusted signature,
`package-loaded=1`, стабильные battle panels и F8 cursor unlock.

For the in-client integration run:

```bat
loader\build_live.cmd
build\live_test_launcher.exe
```

`live_test_launcher.exe` uses the isolated `build\live_env`. To launch the
real client with packages installed into `<game>\mods`, use:

```bat
build\wotb_mod_launcher.exe
```

The production launcher enables early mod registration before the first
hangar model opens. The loader validates the current `DAVA::File::Create`
RVA and falls back to its guarded function signature when the client moves
the function without changing its implementation.

The live harness loads six isolated test mods, exercises all 76 host
functions, verifies the real hook lifecycle, creates a native DAVA/Wwise
event, runs the ready custom-file audio backend, and calls the native
UIControl/Scene object operations inside the client. Two focused ABI 2.9 mods
separately validate the six new gameplay/input payloads and a visible
T-34-85 mesh/texture skin replacement. The latest verified run
on WoT Blitz 11.19.0.834 completed with
`142 PASS / 0 SKIP / 0 FAIL`. After the initial UI-only
startup state, `scene_get_active` acquired the live 3D Scene. A loose custom
`.sc2` and a newly constructed `Entity` were transformed, attached to that
Scene for 180 consecutive rendered frames, detached, and released. Native
`UIControl` creation, active-screen lookup, reload guards, resource loads,
sound and the earlier hierarchy operations also passed.
The event layer delivered seven UI-screen changes, two Scene activations and
one Scene deactivation on the dispatch thread; resource cloning and borrowed
handle invalidation were verified against the real DAVA objects.
The ABI 2.9 loader installed all gameplay/input detours with
`mask=0xFFF (12/12)`, enabled the native stock-resource redirect and completed
the focused vehicle-skin registry/resolver test with `15/0`. Battle-only event
callbacks were not actively stimulated during this hangar run, and the
visible stock-tank replacement remains a separate field check.

The standalone focused harness already reports
`NEW EVENTS MOD: passes=14 failures=0` and
`VEHICLE SKIN MOD: passes=15 failures=0`; it injects all seven focused event
payloads, including camera mode,
checks borrowed vehicle ownership, and resolves mesh/material/texture aliases
including the `.dvpl` stock-path form.

The maintained scenario and coverage table is
[`tests/API_TEST_MATRIX.md`](tests/API_TEST_MATRIX.md); the latest evidence and
known integration limits are in
[`tests/API_TEST_REPORT.md`](tests/API_TEST_REPORT.md).
Publishable test packages and their backend IDs are documented in
[`publish/README.md`](publish/README.md).

## Embed the runtime in your loader

Link `wotb_mod_runtime.lib`, include `wotb_mod_runtime.h`, and initialize it
from a normal worker thread after the game module is mapped:

```cpp
WotbModWindowsAudioOptions audioOptions = {};
audioOptions.struct_size = sizeof(audioOptions);

WotbModWindowsAudioHandle windowsAudio = nullptr;
WotbModRuntimeAudioBackend audio = {};
if (WotbModWindowsAudio_Create(
        &audioOptions, &windowsAudio, &audio) != WOTBMOD_OK) {
    return false;
}

WotbModDavaSoundOptions soundOptions = {};
soundOptions.struct_size = sizeof(soundOptions);
soundOptions.game_module = GetModuleHandleA(nullptr);

WotbModDavaSoundHandle davaSound = nullptr;
WotbModRuntimeSoundBackend sound = {};
if (WotbModDavaSound_Create(
        &soundOptions, &davaSound, &sound) != WOTBMOD_OK) {
    WotbModWindowsAudio_Destroy(windowsAudio);
    return false;
}

WotbModRuntimeOptions options = {};
options.struct_size = sizeof(options);
options.game_directory = gameDirectory;
options.mods_directory = modsDirectory; // optional; defaults to <game>\mods
options.game_module = GetModuleHandleA(nullptr);
options.log_sink = MyLogSink;
options.hook_backend = &myHookAdapter;   // optional
options.resource_backend = &myDavaResources; // optional typed UI/scene bridge
options.audio_backend = &audio;
options.sound_backend = &sound;

if (WotbModRuntime_Initialize(&options) != WOTBMOD_OK) {
    WotbModDavaSound_Destroy(davaSound);
    WotbModWindowsAudio_Destroy(windowsAudio);
    return false;
}
WotbModRuntime_LoadAll(); // individual invalid mods remain isolated
```

Include `wotb_mod_windows_audio.h` when using this ready backend. The build
already links its implementation into `wotb_mod_runtime.lib`; the final loader
must link `xaudio2.lib`, `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`, and
`ole32.lib`. Include `wotb_mod_dava_sound.h` for the native event bridge.

Call `WotbModRuntime_DispatchFrame` from your render/frame integration point.
The API accepts opaque D3D pointers and does not draw or initialize any UI:

```cpp
WotbModRuntime_DispatchFrame(
    swapChain,
    device,
    immediateContext,
    backBufferWidth,
    backBufferHeight,
    deltaSeconds);
```

Call `WotbModRuntime_Shutdown` from a safe worker/control thread before your
loader unloads, then call `WotbModDavaSound_Destroy(davaSound)` and
`WotbModWindowsAudio_Destroy(windowsAudio)`. Do not
call shutdown from `DllMain`; Windows loader-lock rules make plugin callbacks
and `FreeLibrary` unsafe there.

The runtime claims `mods\cache\runtime_session.marker` during initialize and
removes its own marker on clean shutdown or orderly CRT process teardown. If
the previous process crashed, the stale marker makes `LoadAll` a successful
diagnostic no-op and blocks explicit mod enable requests. The log includes the
last recorded phase/mod. Exit the game and delete the marker after inspection,
or set `WOTBMOD_SAFE_MODE_OVERRIDE=1` for one explicit recovery startup. The
override deletes the stale marker and immediately claims a fresh one, so
another crash re-enables safe mode.

## Hook adapter boundary

The runtime intentionally does not choose MinHook, PolyHook, or another hook
engine. Your architecture supplies four operations:

```cpp
WotbModRuntimeHookBackend hooks = {};
hooks.struct_size = sizeof(hooks);
hooks.user_data = myHookEngine;
hooks.create = HookCreate;
hooks.enable = HookEnable;
hooks.disable = HookDisable;
hooks.remove = HookRemove;
```

The runtime tracks hook ownership per mod. A mod cannot enable, disable, or
remove another mod's hook through the API. Disabling, faulting, or unloading a
mod removes all hooks still owned by that mod.

## Client resources, UI packages, and custom models

The runtime contains the cross-mod overlay registry and the public stable ABI.
For WoT Blitz 11.19.0.834 x86 the SDK includes the DAVA-specific object
bridge:

```cpp
WotbModDavaResourcesOptions dava = {};
dava.struct_size = sizeof(dava);
dava.game_module = GetModuleHandleA(nullptr);

WotbModDavaResourcesHandle davaHandle = nullptr;
WotbModRuntimeResourceBackend resources = {};
WotbModResult result = WotbModDavaResources_Create(
    &dava, &davaHandle, &resources);

options.resource_backend =
    result == WOTBMOD_OK ? &resources : nullptr;
```

Include `wotb_mod_dava_resources.h`. Zero RVA fields select the verified
11.19 defaults; a newer client loader can override every anchor after
re-anchoring. The bridge refuses non-x86/wrong-image layouts instead of
calling unchecked addresses.

The verified loader integration points are documented in
[`../re_anchors.md`](../re_anchors.md) under **Client resources**. The normal
file hook calls `WotbModRuntime_ResolveResourcePath(requestedPath, ...)`.
`WOTBMOD_ERROR_NOT_FOUND` means continue through the original DAVA resolver.
This covers loose files and `.dvpl` fallbacks without exposing DAVA C++ types
or raw game addresses to a mod DLL.

A mod mounts only a directory below its own data directory:

```cpp
WotbModResourceMountInfo mount = {};
mount.struct_size = sizeof(mount);
mount.virtual_root = "~res:/Mods/author.example/";
mount.source_directory = "resources";
mount.priority = 100;
mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;

WotbModResourceMountId mountId = 0;
host->resource_mount(mod, &mount, &mountId);
```

With this layout:

```text
mods/data/<module-name>/resources/
  UI/MyScreen.yaml
  3d/MyModel.sc2
  3d/MyModel/texture.tex
```

the game paths are:

```text
~res:/Mods/author.example/UI/MyScreen.yaml
~res:/Mods/author.example/3d/MyModel.sc2
```

The resolver handles loose/DVPL lookup and priority. Production Scene object
loading is live-tested with a loose `.sc2`; referenced materials and textures
must be reachable by DAVA. Explicit engine objects use `resource_load`; set the request type to
`WOTBMOD_RESOURCE_UI_PACKAGE`, `WOTBMOD_RESOURCE_UI_CONTROL`, or
`WOTBMOD_RESOURCE_SCENE`. `object_name` is used for a named UI control.
Returned handles are opaque and ownership-checked by the runtime.

The supplied live loader constructs real refcounted `UIPackage`,
`UIControl`, and `Scene` objects. Reload builds the replacement before
releasing the old object; release calls DAVA refcounting. Mod loading is
deferred to a warmed-up client `Present` thread so UI/Scene engine services
are initialized and the callbacks run on the client thread. The live test
completed all three object lifecycles and observed the real UI and Scene
entry hooks.

ABI 2.5 adds native operations without exposing DAVA pointers:

```cpp
WotbModUiControlGeometry rect = {};
rect.struct_size = sizeof(rect);
rect.x = 24.0f;
rect.y = 32.0f;
rect.width = 320.0f;
rect.height = 180.0f;
host->ui_control_set_geometry(mod, control, &rect);
host->ui_control_set_visible(mod, control, 1);
host->ui_control_add_child(mod, parentControl, control);
host->ui_control_remove_child(mod, parentControl, control);

WotbModSceneTransform transform = {};
transform.struct_size = sizeof(transform);
transform.rotation_w = 1.0f;
transform.scale_x = 1.0f;
transform.scale_y = 1.0f;
transform.scale_z = 1.0f;
host->scene_set_transform(mod, scene, &transform);
host->scene_add_child(mod, parentScene, scene);
host->scene_remove_child(mod, parentScene, scene);
```

Calls made from the `DispatchFrame` thread are synchronous. Calls from other
threads are validated and placed in a fixed main-thread queue; `OK` then means
accepted, and late backend failures are logged. `main_thread_enqueue` always
defers a callback to the beginning of a later `DispatchFrame`. Pending work is
discarded before mod disable/fault/unload, so no callback can outlive its DLL.

ABI 2.6 adds object factories and active-root getters. These four calls return
a new mod-owned resource handle and are intentionally synchronous, so invoke
them from `on_frame` or a `main_thread_enqueue` callback:

```cpp
static void WOTBMOD_CALL BuildObjects(
    const WotbModHostApi* host,
    WotbModHandle mod,
    void*) {
    WotbModUiControlGeometry rect = {};
    rect.struct_size = sizeof(rect);
    rect.x = 20.0f;
    rect.y = 20.0f;
    rect.width = 240.0f;
    rect.height = 80.0f;

    WotbModResourceHandle screen = nullptr;
    WotbModResourceHandle control = nullptr;
    if (host->ui_get_active_screen(mod, &screen) == WOTBMOD_OK &&
        host->ui_control_create(mod, &rect, &control) == WOTBMOD_OK) {
        host->ui_control_add_child(mod, screen, control);
        // Keep handles while the object is in use. Detach before release.
        host->ui_control_remove_child(mod, screen, control);
    }
    if (control) host->resource_release(mod, control);
    if (screen) host->resource_release(mod, screen);

    WotbModResourceHandle scene = nullptr;
    WotbModResourceHandle entity = nullptr;
    if (host->scene_get_active(mod, &scene) == WOTBMOD_OK &&
        host->scene_entity_create(mod, &entity) == WOTBMOD_OK) {
        host->scene_add_child(mod, scene, entity);
        host->scene_remove_child(mod, scene, entity);
    }
    if (entity) host->resource_release(mod, entity);
    if (scene) host->resource_release(mod, scene);
}

host->main_thread_enqueue(mod, &BuildObjects, nullptr);
```

`ui_control_create` accepts a null geometry and then uses the engine default
rectangle. `ui_get_active_screen` and `scene_get_active` retain the borrowed
engine object before publishing the handle. `scene_get_active` returns
`WOTBMOD_ERROR_NOT_FOUND` when the client is on a UI-only screen with no
activated/rendered 3D Scene. Calling any factory/getter from a non-dispatch
thread returns `WOTBMOD_ERROR_WRONG_THREAD`. Factory/getter handles are not
file-backed, so `resource_reload` returns `WOTBMOD_ERROR_PLATFORM`; release
them with `resource_release`.

ABI 2.7 lets mods receive active-root changes without their own per-frame
polling:

```cpp
static WotbModResourceHandle g_activeScreen = nullptr;

static void WOTBMOD_CALL OnClientEvent(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void*) {
    if (event &&
        event->type == WOTBMOD_EVENT_UI_SCREEN_CHANGED &&
        event->resource) {
        WotbModResourceHandle clone = nullptr;
        if (host->resource_clone(
                mod, event->resource, &clone) == WOTBMOD_OK) {
            if (g_activeScreen) {
                host->resource_release(mod, g_activeScreen);
            }
            g_activeScreen = clone;
        }
    }
}

WotbModEventSubscriptionId subscription = 0;
host->event_subscribe(
    mod,
    WOTBMOD_EVENT_UI_SCREEN_CHANGED |
        WOTBMOD_EVENT_SCENE_ACTIVATED,
    &OnClientEvent,
    nullptr,
    &subscription);
```

Event resource handles are borrowed and valid only during the callback.
`resource_release` and `resource_reload` reject them; use `resource_clone`
inside the callback when the object must survive. Subscriptions and cloned
resources are automatically removed during mod cleanup.

ABI 2.8 can traverse and modify UI controls already owned by the client:

```cpp
WotbModResourceHandle screen = nullptr;
WotbModResourceHandle panel = nullptr;
if (host->ui_get_active_screen(mod, &screen) == WOTBMOD_OK &&
    host->ui_control_find_by_name(
        mod, screen, "AmmoPanel", 1, &panel) == WOTBMOD_OK) {
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    host->ui_control_get_state(mod, panel, &state);
    host->ui_control_set_visible(mod, panel, 1);
    host->ui_control_set_input_enabled(mod, panel, 1, 0);
    host->ui_control_set_disabled(mod, panel, 0, 0);
}
if (panel) host->resource_release(mod, panel);
if (screen) host->resource_release(mod, screen);
```

The same API exposes parent/child traversal through
`ui_control_get_parent`, `ui_control_get_child_count`, and
`ui_control_get_child_at`. All query calls are synchronous and restricted to
the `DispatchFrame` thread. Returned controls are owned handles and must be
released.

Vehicle access is snapshot-based and never exposes native game pointers:

```cpp
uint32_t vehicleCount = 0;
host->vehicle_get_count(mod, &vehicleCount);

WotbModVehicleHandle local = nullptr;
if (host->vehicle_get_local(mod, &local) == WOTBMOD_OK) {
    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    host->vehicle_get_info(mod, local, &info);
    host->vehicle_release(mod, local);
}
```

Mods can subscribe to all 22 event masks: UI/Scene lifecycle and UI input,
battle lifecycle, vehicle spawn/despawn/local changes, shots and shell hits,
health/damage/destruction, reload, ammo, aim-target, spotting and camera-mode
changes.
`event->vehicle` and `event->other_vehicle` are borrowed only for the
callback; call `vehicle_clone` if a snapshot must survive, then release that
clone with `vehicle_release`. `BATTLE_STARTED`/`BATTLE_ENDED` are inferred
from the first vehicle entering and the last vehicle leaving the world.

## Vehicle skins and stock asset replacement

ABI 2.9 redirects exact stock DAVA paths to files from the registering mod's
active resource mounts. One skin can replace the tank mesh, its material/FX
YAML files and any referenced `.tex` files:

```cpp
static WotbModResourceMountId g_mount = 0;
static WotbModVehicleSkinHandle g_skin = nullptr;

WotbModResourceMountInfo mount = {};
mount.struct_size = sizeof(mount);
mount.virtual_root = "~res:/Mods/author.skin/";
mount.source_directory = "skin";
mount.priority = 100;
mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
host->resource_mount(mod, &mount, &g_mount);

WotbModVehicleSkinAsset assets[] = {
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_MESH,
     "~res:/3d/Tanks/USA/M4A3E8/hull.sc2",
     "~res:/Mods/author.skin/hull.sc2"},
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_MATERIAL,
     "~res:/3d/Tanks/USA/M4A3E8/materials.yaml",
     "~res:/Mods/author.skin/materials.yaml"},
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_TEXTURE,
     "~res:/3d/Tanks/USA/M4A3E8/hull.tex",
     "~res:/Mods/author.skin/hull.tex"},
};

WotbModVehicleSkinDescriptor skin = {};
skin.struct_size = sizeof(skin);
skin.skin_id = "author.m4a3e8-style";
skin.vehicle_name = "M4A3E8";
skin.assets = assets;
skin.asset_count = sizeof(assets) / sizeof(assets[0]);
skin.priority = 100;
skin.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;
host->vehicle_skin_register(mod, &skin, &g_skin);
```

`stock_virtual_path` is an exact, case-insensitive normalized match; a
physical `.dvpl` request is matched to its logical path without `.dvpl`.
`replacement_virtual_path` is resolved through mounts owned by the same
enabled mod or to an existing stock `~res:/` resource under the local game
`Data` directory. This permits resource-free stock-to-stock model swaps;
arbitrary absolute paths are not accepted. Higher skin priority wins, and the
newest registration wins a tie. Use
`vehicle_skin_set_enabled`, `vehicle_skin_get_info` and
`vehicle_skin_release` for lifecycle control. Runtime cleanup removes every
skin owned by a disabled, faulted or unloaded mod.

The loader hooks `DAVA::File::Create`, so the redirect applies to normal
client model loading rather than exposing raw `DAVA::Entity` or material
pointers. It affects new file opens immediately. Objects already resident in
DAVA caches are rebuilt on the client's normal garage/battle/model lifecycle;
the API does not forcibly mutate an already rendered entity.

## Audio clips and playback

Audio files use the same mounted virtual paths as UI and scene assets. ABI 2.3
adds `audio_clip_load`, which loads an arbitrary loose audio file from a mod
mount, then lets the mod create one or more independent playbacks:

```cpp
WotbModResourceHandle clip = nullptr;
host->audio_clip_load(
    mod,
    "~res:/Mods/author.example/Audio/alert.wav",
    &clip);

WotbModAudioPlayInfo play = {};
play.struct_size = sizeof(play);
play.flags = WOTBMOD_AUDIO_PLAY_LOOP;
play.volume = 0.8f;
play.pitch = 1.0f;
play.pan = -0.25f;

WotbModAudioPlaybackHandle playback = nullptr;
host->audio_play(mod, clip, &play, &playback);
```

The older typed-resource form remains equivalent: call `resource_load` with
`request.type = WOTBMOD_RESOURCE_AUDIO_CLIP`. The convenience function and the
typed form both resolve only an enabled mod's mounted loose file; they do not
accept an arbitrary absolute path.

The supplied Windows backend decodes through installed Media Foundation
codecs—normally WAV, MP3, AAC/M4A, and WMA—and plays through XAudio2. It
supports loop, pause/resume/stop, volume, pitch, and 2D pan. It deliberately
returns `WOTBMOD_ERROR_PLATFORM` for `WOTBMOD_AUDIO_PLAY_SPATIAL`; use a client
sound-engine adapter when engine-native 3D spatial audio is required. OGG is
not bundled with the ready backend.

A custom/client backend connects both file loading and playback:

```cpp
WotbModRuntimeAudioBackend audio = {};
audio.struct_size = sizeof(audio);
audio.user_data = myAudioEngine;
audio.load_clip = AudioLoadFile;
audio.reload_clip = AudioReloadFile;       // optional
audio.release_clip = AudioReleaseClip;
audio.play = AudioPlay;
audio.pause = AudioPause;                 // optional
audio.resume = AudioResume;               // optional
audio.stop = AudioStop;                   // optional
audio.set_parameters = AudioSetParameters; // optional
audio.get_state = AudioGetState;          // optional
audio.release = AudioRelease;
options.audio_backend = &audio;
```

`play` and `release` are required for playback. `load_clip` and `release_clip`
are required for the custom-file route; when they are absent, audio clips fall
back to the generic resource backend for ABI 2.2 compatibility. A clip cannot
be reloaded or released while one of its playback handles is active. Release
playback first; disable/fault/shutdown does this automatically in the correct
order.

## Native client sound events

ABI 2.4 exposes sound events already registered in the client's DAVA/Wwise
banks. Mods receive only an opaque handle:

```cpp
WotbModSoundEventHandle event = nullptr;
if (host->sound_event_create(
        mod, "guns/tracers/tracer_hard", &event) == WOTBMOD_OK) {
    host->sound_event_set_volume(mod, event, 0.25f);
    host->sound_event_set_position(mod, event, 0.0f, 0.0f, 0.0f);
    host->sound_event_trigger(mod, event);
    host->sound_event_stop(mod, event);
    host->sound_event_release(mod, event);
}
```

Available operations are `sound_event_create`, `trigger`, `stop`,
`set_paused`, `set_volume`, `set_position`, `get_state`, `set_parameter`,
`get_parameter`, `has_parameter`, `get_name`, and `release`. State reports
the last successful lifecycle operation because DAVA `IsActive()` describes
the lifetime of an event instance, not its playing/paused command state.
RTPC values are engine-owned: a successful `set_parameter` does not guarantee
that `get_parameter` echoes the requested value byte-for-byte; Wwise may
normalize, clamp, or defer it.

The ready bridge is pinned to the anchors documented in
[`../re_anchors.md`](../re_anchors.md) and must be re-anchored after a client
update. This route does not load arbitrary audio files; use `audio_clip_load`
for WAV/MP3/AAC/WMA files shipped by a mod.

## Writing a mod

Include `wotb_mod_api.h` and export exactly one entry point:

```cpp
WOTBMOD_ENTRY {
    if (!host || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }

    *out_info = {};
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "author.mod-id";
    out_info->name = "My Mod";
    out_info->version = "1.0.0";
    out_info->author = "Author";
    out_info->on_enable = OnEnable;
    out_info->on_disable = OnDisable;
    out_info->on_unload = OnUnload;
    out_info->on_frame = OnFrame;
    return WOTBMOD_OK;
}
```

See `examples\hello_mod\hello_mod.cpp` for logging, config, frame usage, a
resource mount, and a typed UI-package load.

## ABI rules

- Major ABI mismatch means incompatible.
- Public structs begin with `struct_size` and only grow by appending fields.
- Public data uses fixed-width integers, pointers, callbacks, and fixed buffers.
- Do not pass STL objects, C++ exceptions, or allocations across the ABI.
- `WotbModLoad` and initial `on_enable` run on the loader's worker thread.
- `on_frame` runs on the thread that calls `DispatchFrame`, normally render.
- ABI 2.2 added typed audio clips and mod-owned playback. ABI 2.3 adds
  `audio_clip_load`, custom-file backend callbacks, and the ready Windows
  decoder/player. ABI 2.4 adds native client sound events and the loader-owned
  DAVA sound backend. ABI 2.5 adds the main-thread queue and typed
  UIControl/Scene operations. ABI 2.6 adds native UIControl/Entity factories
  and active UI/Scene getters. ABI 2.7 adds client-event subscriptions and
  cloning for callback-scoped resources. ABI 2.8 adds existing-UI traversal,
  vehicle snapshots and typed gameplay events. ABI 2.9 adds mod-owned stock
  vehicle mesh/material/texture path replacement. Resource-level reload remains
  available for file-backed objects when the selected backend supports it.
  DLL hot reload is not provided; replacing a DLL still requires a controlled
  runtime shutdown and reload.
- Custom `.sc2` entities can be loaded and attached to the active Scene.
  ABI 2.9 additionally replaces exact stock tank asset paths through the
  loader-owned DAVA file bridge.
