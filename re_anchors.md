# WoT Blitz Steam — RE Anchors (May–July 2026)

> **Re-anchoring is now automated — see `reanchor/README_RU.md`.**
>
> The string/RTTI pivot procedure documented further down was the manual way
> to recover these addresses after a patch. `reanchor/` now derives a locator
> recipe for each of the 59 anchors and replays them against a new build with
> no IDA and no dependencies:
>
> ```bash
> python reanchor/cli.py resolve "<new wotblitz.exe>"
> ```
>
> `python test_reanchor.py` proves all 59 recipes still resolve exactly on the
> reference build. The notes below remain the reference for *why* each anchor
> is what it is, and are still needed for the 18 struct field offsets, which
> scanning cannot recover.

## 2026-09-03 — re-anchored to 11.20.0.887

WoT Blitz `11.20.0.887` x86 (Steam build 24835747), `wotblitz.exe` SHA-256
`4813544D3D6B9F45A357E87F5A14D065BD108C4ACD324AC1506CB6D1AD6FF0AF`, 72 718 336
bytes. Fresh IDA database: `analysis/fresh_11_20_0_887_4813544d/*.i64`
(GUI auto-analysis, ~2 h; a copy for headless `re-mcp-ida` lives in
`analysis/mcp_11_20_0_887/`). The 11.19 database in `analysis/` turned out to
hold only 2 functions when opened headless, so the old side was analysed with
pefile + capstone instead (`analysis/pivot_tools_20260903/`).

**All 97 fixed RVAs in `loader/anchor_rvas.h` + `kVehiclePrimaryVtableRva` were
re-derived and the final table is `analysis/reanchor_11.20.0.887_final_rvas.json`.**
`reanchor/cli.py generate` + `verify` on the new image: 97/97 exact.

What the signature recipes alone did: 31/67 (46 %), `REJECTED`, with two
false positives (`ClientInitialize` → 0x017F9B70, `EngineUpdateAndDrawWindows`
→ 0x00C7CF60). What actually pinned each anchor, in order of trust:

| Method | Anchors | Notes |
| --- | ---: | --- |
| RTTI TypeDescriptor → COL → vtable | 14 | every `*Vtable` incl. `VehiclePrimaryVtable` 0x033056A4 |
| vtable slot (same class, same offset, same slot) | 18 | all `VehicleGameLogic`/`GameCamera`/`Scene`/`UIControl`/`Camouflages*` virtuals |
| vtable write in ctor/dtor (i-th code xref) | 6 | `GameCameraCtor`, `TracerManagerCtor`, `DefaultEntityCtor`, `DefaultNMaterialCtor`, `UiPackageLoaderCtor/Dtor` |
| string-literal xrefs + snap to call target | 8 | `DavaFileCreate`, `TextureCreateFromFile`, `SceneLoadEntity`, `TracerManagerShowTracer`, ... |
| call-list alignment from a mapped caller / callee | 6 | `FastNameCtor`, `RefCountedRetain`, `GetEngineContext`, `YamlParseFileWrapper`, `GetRenderBatch` |
| masked-prefix lookup over all function starts, verified by instruction-sequence similarity | 17 | `BwEntityCtor`, `RenderBatch::Set*`, `Entity::GetRenderObject`, most `NMaterial::*`, `ResourceArchiveDtor`, `SceneLoadFromFile` |
| exact byte pattern (tiny function) | 1 | `NMaterialSetQuality` = `mov eax,[arg]; mov eax,[eax]; mov [ecx+28h],eax` at 0x00929B80 |
| data: same-ordinal instruction inside a mapped function | 4 | camo mask `push [0x374F0E4]` at 0x1A059D0; sound singleton written by the hybrid sound-system ctor; sound group = the `push offset` right after `mov ecx,[singleton]`; tracer table = the 25-entry pointer run over the same style strings at 0x4038F80 |
| Hex-Rays structure match (IDA MCP) | 23 | the ones the byte methods only rated 0.4–0.8 |

### GES::GameEventSystem anchors (added 2026-09-03 for `wotbmod.ges`)

Decompiled on the fresh 11.20 database; see
`docs/superpowers/specs/2026-09-03-ges-event-bus-design.md` §2 for layouts.

| Symbol | VA | RVA | Notes |
| --- | --- | --- | --- |
| `GameEventSystem::GetListeners(type_info)` | `0x006DBC30` | `0x002DBC30` | thiscall, `ret 4`; the argument is the `std::type_info*` **itself** (the name at `+4` is hashed: `_std_type_info_hash(a2 + 4)`), `this+4` map lookup, returns `ListenerList*` or 0. Live 2026-09-04: passing a pointer *to* that pointer keyed an orphan list and no event was ever delivered |
| `GameEventSystem::GetOrCreateList(type_info, factory)` | `0x006DBF80` | `0x002DBF80` | thiscall, `ret 8`; same key rule; inserts via `factory` when absent |
| `ListenerList` factory | `0x0112F0B0` | `0x00D2F0B0` | cdecl, T-independent: `new(16)` + sentinel `new(0x50)` |
| `ListenerList::Add(out_handle, std::function*, pos)` | `0x011260E0` | `0x00D260E0` | thiscall, `ret 0xC`; 13 byte-identical instantiations, recipe pins the copy-helper `call` |
| `ListenerList::Erase(out_iter, iter)` | `0x007B0380` | `0x003B0380` | thiscall, `ret 8`; unlink, `_Delete_this`, `operator delete` |
| `GameEventSystem::SubscribeImpl(owner, type_index, std::function)` | `0x006DD830` | `0x002DD830` | thiscall, `ret 0x30`; hooked only to capture `this`. Live: one bus instance in hangar and battle (11.20.0.887) |
| engine-side `ListenerList<CameraModeChanged>::Add` | `0x011259A0` | `0x00D259A0` | reached from the subscribe delegate's slot 2 (`sub_1173630`): `GetOrCreateList(type_info, factory sub_112EDF0)` then `Add(handle, &function, 1)`. Same node shape as the 0xD260E0 instantiation the loader calls: 80-byte node, `std::function` impl copy at `+0x44` (a 48-byte engine wrapper whose `_Do_call` forwards to ours), flags at `+0x48`; list: vtable, sentinel `+4`, size `+8`, insert position `+0xC` |

**Live 2026-09-04 (training battle, 11.20.0.887):** after the key fix
`ges.observe_all` reports `types=601 events=10508 distinct=84`, `publish` from the
DAVA main thread answers OK and `echo_seen=1` (loader answers `E_WRONG_THREAD`
from any other thread).

The event type key is the struct's `std::type_info` in `.data`
(`.?AUCameraModeChanged@Avatar@GES@@` and 600 more); the loader indexes them
at start-up by scanning `.data` for `.?AU…@GES@@`.

11.20 is built with a toolchain that mangles lambda functors with their
enclosing function: `_lambda_1_@?1??OnUpdateObservedStatus@ClientArena@@...`.
That named `ObservedStatus` (= `ClientArena::OnUpdateObservedStatus`, 0x0122D820,
whose first call is `ObservedPayloadGetter` 0x01BF1E40) and confirmed
`ReloadSetState` 0x01201290 (its inlined lambdas are `VehicleGun::OnAmmoSpent`
and `VehicleGun::OnReloadingUpdate`). Use this: `list_names "_lambda_.*@?1??"`
in the new database is a free symbol table for GES code.

Struct offsets: all 29 (+4 new) re-checked and **unchanged**. Evidence: object
sizes at the allocation sites are identical (Entity 0x50, NMaterial 0xA8,
UIControl 0x140, GameCamera 0x5BC); `CameraController+0x5C/+0x28`,
`GameCamera+0x320`, `Vehicle+0xB8/+0x11C`, `Scene+0x100`, `UIControlSystem+0x3C`
read from decompiled 11.20 functions; every other offset is used by the same
vtable slots of `UIControl`/`Entity`/`TransformComponent`/`UIPackage`/
`RenderBatch`/`RenderObject` in both builds (displacement set per slot equal).
`VehicleObject+0xB0` (team) has no static witness and stays runtime-guarded.

Notable moves: engine (DAVA) code shifted by very different deltas per module
(`NMaterial` +0x314AE0, `Texture` +0x199A0, UI −0x54000), GES code by about
+0x36000..+0x3A000. `UIControlSystem::Input`, `UIPackageLoader::ctor/dtor` and
`GameSceneController::OnVehicleHitDamage` changed body noticeably (sequence
similarity 0.2–0.5) but keep their slots, offsets and callers.

ImageBase: `0x00400000`. All addresses below are full VAs (IDA notation).
The active Mod API runtime set was re-anchored on 2026-07-31 for
WoT Blitz `11.19.0.834` x86, `wotblitz.exe` SHA-256
`41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD`.
The fresh image spent two hours in IDA auto-analysis. Packing the large IDB
timed out, so the runtime-bound functions below were recreated and checked
again in the reopened fresh image by string/RTTI pivot, prologue, function
body and x86 return cleanup. Every fixed address currently consumed by the
Mod API was rechecked. Historical research VAs that are not consumed by the
runtime were not promoted to current bindings and remain explicitly
non-callable notes.

Sections not explicitly marked as the current Mod API runtime set are research
notes from the older 11.18 baseline and **must** be revalidated before being
called or dereferenced. This is not hypothetical. Two concrete falsifications
were found on 2026-08-15 against 11.19.0.834:

- The three COL addresses previously listed under "Other camera RTTI" —
  `BaseCameraController 0x0396B05C`, `CameraController 0x0396B308`,
  `GameCameraSingletonComponent 0x03933D30` — do not appear anywhere in this
  image. A `search_bytes` for their little-endian encodings returns **zero**
  matches each, while the same search for the verified `GameCamera` COL
  `0x03973700` returns exactly one. Corrected values are in the table below.
- The `CameraModeChanged` note was not merely unproven but **inverted**: the
  correct reading is `0 = SNIPER`, not `0 = ARCADE`. See the corrected entry.

Treat every unmarked address as hostile until it is re-derived from a type
descriptor string or a `__FUNCTION__` literal.

## Current Mod API fixed-address audit

The audit covers all 61 fixed-RVA declarations used by the current runtime.
Duplicate shared anchors such as `FastName`, `Retain` and `Release` are counted
once per consuming source file.

| Runtime source | Fixed declarations | Fresh 11.19 result |
| --- | ---: | --- |
| `loader/v3_native_bindings.cpp` | 11 | `11/11` function/vtable/ABI targets verified; obsolete local-shell hook removed, camera-mode hook added |
| `loader/wotb_mod_loader.cpp` | 15 | `15/15` gameplay/input/skin targets recreated or decoded at exact function entry and semantically checked |
| `src/wotb_mod_dava_sound.cpp` | 8 | `8/8` singleton/FastName/refcount/vtable targets verified through constructor xrefs and live vtable slots |
| `src/wotb_mod_dava_resources.cpp` | 27 | `27/27` allocator/refcount/UI/Scene function and vtable targets verified through prologues, constructor writes and slot tables |

`build.cmd`, the `10/10` full-contract stress run and
`loader/build_live.cmd` pass after this cross-check. This proves the local
binding code compiles and the guarded contracts are internally consistent; a
real client run is still required for battle-only stimuli, camera transitions
and visible material/mesh replacement.

## Top-level anchors — current status
| Symbol | VA | RVA / status |
| --- | --- | --- |
| `LeaveToHangarCallback` | `0x01572B50` | `0x01172B50` |
| `RegisterLeaveActions` / client initialize owner | `0x01554A70` | `0x01154A70` |
| Action key `.leaveToHangar` | `0x03688F3C` | current string; old `0x04486D80` is invalid |
| `RBB` (Render Battle Begin?) | — | old RVA `0x015FD420` is not a current function entry; no API binding |
| native `DeviceInfoAccessor` | — | old RVA `0x00EC5020` lands on an epilogue in 11.19; rejected |
| native `DeviceInfoGlobal` | — | old VA `0x0407A850` now contains MSVC type-info text; rejected |

The public device API intentionally uses Win32/DXGI metadata and does not
dereference either rejected native `DeviceInfo` anchor.

## Camera (verified via IDA RTTI + ctor)
| Item | VA |
| --- | --- |
| `GameCamera` RTTI Complete Object Locator | `0x03973700` |
| `GameCamera::vftable` (primary) | `0x03690080` (9 methods) |
| `GameCamera::GameCamera(byte, dword, dword)` | `0x0159B2F0` / RVA `0x0119B2F0`; `retn 0x0C` |
| `GameCamera::~GameCamera` (deleting dtor) | `0x015A6FB0` / RVA `0x011A6FB0`; `retn 4` |
| `DAVA::Camera::SetFovY`-like | `0x01490B20` / RVA `0x01090B20`; clamps `1..140`, updates `+0x268` |
| `CameraModeChanged@Avatar@GES` callback | `0x0156ED10` / RVA `0x0116ED10`; `retn 4`. **See the correction below — the old `0=ARCADE, 1=SNIPER` note was inverted and is withdrawn.** |

### Correction, 2026-08-15: what `0x0156ED10` does and does not prove

Its complete body is sixteen bytes of code:

```
push ebp / mov ebp,esp / mov eax,[ebp+8] / cmp dword ptr [eax],0
setz al / mov [ecx+60h],al / pop ebp / retn 4
```

That is `this->+0x60 = (event.field0 == 0)` and nothing else. It is a boolean
consumer: it cannot distinguish 1 from 2 from 7, so it **cannot** establish any
multi-valued enum, and the previous `1 = SNIPER` half was never supported by it.

The mode semantics come from two other, independently proven sites:

- `CameraController::OnCameraModeChanged` `0x015B5200` / RVA `0x011B5200`.
  It is provably a `CameraModeChanged@Avatar@GES` handler: it is bound into a
  `std::function` at `0x015B9A1C` whose `_Func_impl` vtable `0x03693770` has COL
  `0x0397689C` naming `..._Func_impl_no_alloc@V<lambda...>@@XABUCameraModeChanged@Avatar@GES@@...`.
  It maps `field0 == 0` to the engine UI state `"STATE_view_play_mode_sniper"`
  (`0x0369022C`) and `field0 != 0` to `"STATE_view_play_mode_arcade"`
  (`0x03690208`), and raises hint `0x66 SNIPER_ON` / `0x67 SNIPER_OFF` accordingly.
- `GameCamera+0x320`, a **2-valued mode index** — it indexes 16-byte per-mode
  parameter blocks at `+0x360`/`+0x370` and `+0x364`/`+0x374`. At the branch
  `0x015C460B`, value `0` selects `GameCamera::GetPivotForSniperMode`
  (`0x015B1870`, identified by its own `__FUNCTION__` literal `0x03693028`).

**Correct mapping: `0 = SNIPER`, non-zero (`1`) = `ARCADE`.** The boolean cached
at `+0x60` is therefore an *isSniper* flag, not *isArcade*. No mod backend may
ship the old mapping.

GameCamera object layout (current runtime offsets rechecked from
`0x0159B2F0`; auxiliary helper addresses below remain historical research
unless listed in the current table above):
- `+0x000` primary vtable (overridden by GameCamera→`0x03690080`)
- `+0x008/+0x00C` flag dwords
- `+0x020..+0x030` znear/far/aspect/fov/fov2
  (init 1.0/3500.0/1.0/70.0/70.0). The constructor's validation
  clamps `+0x20` as the near plane and enforces `+0x24 > +0x20`;
  these are the offsets used by the 11.19 Mod API binding.
- `+0x038/+0x044/+0x050` position / target / up (`Vector3`; current
  transform backend reads and writes these exact offsets)
- **`+0x080..+0x23F` 7 × Matrix4 (each 64 B), set to identity by `sub_699ED0`=`Matrix4::Identity()`**. Slots map to DAVA::Camera fields (canonical DAVA framework layout):
  - `+0x080` `cameraTransform`
  - `+0x0C0` **`viewMatrix`** ← real View
  - `+0x100` `projMatrix`
  - `+0x140` `viewProjMatrix` ← forum's "ViewMatrix" (combined VP, NOT pure view)
  - `+0x180` `invViewMatrix` (used to feed shader uniform `mainCameraInvViewMatrix`)
  - `+0x1C0` `invProjMatrix`
  - `+0x200` `invViewProjMatrix`
- `+0x24C` dirty-flags word (verified: `mov word [esi+24Ch], 1` in `SetupPerspective` `0x00A822B0`)
- `+0x250` shared resource ptr (RefCounted, Release=`sub_91AD20`)
- `+0x254` secondary vtable ptr (multiple inheritance)
- `+0x268` cached `tan(fov/2)` (vfunc[6] `GetFOV` returns `this[154]`)
- `+0x26C..+0x27C` sub-object vtables (re-resolve before direct use)
- `+0x280, +0x2AC, +0x2D8` three DAVA::Vector/Signal (ctor at `0x0121AC00`)
- `+0x311 / +0x314 / +0x318` ctor args `(byte a2, dword a3, dword a4)`

DAVA::Camera helpers (verified):
- `DAVA::Camera::Camera()` (root ctor) = `0x00A2F020`
- `DAVA::Camera::SetTarget`/`SetPosition` = `0x00A7EA30`
- `DAVA::Camera::SetupPerspective` = `0x00A822B0`
- `DAVA::Camera::ValidateAspect` = `0x014624E0`
- `DAVA::Camera::SetFovY`-like = `0x01490B20` (current runtime binding)
- `Matrix4::Identity()` = `0x00699ED0`
- `DAVA::Camera::Clone()` (vfunc[8]) = `0x01465DA0`
- `DAVA::Camera::SerializeToYaml`-like (vfunc[2]) = `0x00A7A410` (writes `cam.fov/znear/zfar/position/target/up/roll/left/isOrtho/orthoWidth`)
- `DAVA::Camera::GetFOV` (vfunc[6]) = `0x0146FFB0` (returns float at `+0x268`)
- `GameCamera::~GameCamera` (deleting dtor) = `0x015A6FB0`

Other camera RTTI — **the first four rows below were falsified on 2026-08-15**
(`search_bytes` for each claimed COL returns zero matches in 11.19.0.834) and
are replaced by the re-derived table that follows. The remaining rows are
still unrevalidated 11.18 notes.

~~`BaseCameraController` COL `0x0396B05C`~~ — not present, see table
~~`CameraController` COL `0x0396B308`~~ — not present, see table
~~`GameCameraComponent` COL `0x03933CC4`~~ — not present, see table
~~`GameCameraSingletonComponent` COL `0x03933D30`~~ — not present, see table

Re-derived camera RTTI for 11.19.0.834. Every COL and vtable here was
recovered from the class's type-descriptor string by the documented method,
and the method was controlled against the known-good `GameCamera` COL.

| class | type descriptor (name at TD+8) | COL | primary vtable |
|---|---|---|---|
| `BaseCameraController` | `0x0429EAC4` (`0x0429EACC`) | `0x03973944` | `0x03692A14` |
| `CameraController` | `0x0429EB60` (`0x0429EB68`) | `0x03973B48` | `0x03692C30` |
| `GameCameraComponent` | `0x0404D510` (`0x0404D518`) | `0x0393C7C8` | `0x03652B10` |
| `GameCameraSingletonComponent` | `0x0425C1BC` (`0x0425C1C4`) | `0x0393C834` | `0x03652B38` |
| `GameCamera` | `0x0429EA60` (`0x0429EA68`) | `0x03973700` | `0x03690080` |
| `FreeCamera` | `0x042B16A8` (`0x042B16B0`) | `0x03987E3C` | `0x036A4E04` |
| `CinemaCameraController` | `0x0429F570` (`0x0429F578`) | `0x03975618` | `0x03692EEC` |
| `SpectatorCameraController` | `0x042A281C` (`0x042A2824`) | `0x03978530` | `0x036946BC` |
| `ReplayCameraController` | `0x042A27D0` (`0x042A27D8`) | `0x03978404` | `0x036945F4` |

`CameraController`'s Class Hierarchy Descriptor is `0x03973B5C`;
`BaseCameraController`'s is `0x03973958`.

Note for anyone who reads an older summary claiming this build has no
cinematic camera: that came from searching the strings for `cinematic`. The
class is spelled **`CinemaCameraController`** and it does exist, with a live
vtable written at `0x01599018`. `ReplayCameraController`,
`SpectatorCameraController`, `BaseSpectatorCameraController` (`0x0429F3F0`)
and `ReplayLookOutAnimationController` (`0x042A27A8`) are present too. None of
them is the `CameraController` that owns the state machine documented below.
- `FlexFOVCamera` COL `0x0395B130`
- `SharedCameraState` COL `0x0393807C`
- `NewbieHangarCameraController` COL `0x039380E4`
- `HangarCameraController` (inherited) `0x03938164`
- `ObserverCameraStateListener` COL `0x0396A978`

Shader uniform FastName init block (constructs all renderer FastNames including camera/light/fog):
- function body around `0x00A5F560..0x00A5F900`
- `FastName::FastName(this,const char*)` ctor → `0x0090D6E0`
- pushes ptrs to .rdata strings: `mainCameraInvViewMatrix` `0x035EC4DC`, `cameraPosition` `0x035EC50C`, `cameraDirection` `0x035EC51C`, `cameraUp` `0x035EC52C`, …

## Audio / DAVA Wwise sound engine (verified)

The active audio-engine singleton is the `DAVA::SoundSystem*` stored in
`dword_4420A64`. This is not a guessed Wwise SDK global: both the base
`DAVA::SoundSystem` constructor and `DAVA::WwiseHybridSoundSystem` constructor
install `this` into this cell when it is null, and gameplay code reads the same
cell before dispatching sound-event creation through its vtable.

| Item | VA | RVA | Evidence |
| --- | --- | --- | --- |
| `DAVA::SoundSystem*` singleton storage (`sound_engine`) | `0x04420A64` | `0x04020A64` | Written by both base/proxy and hybrid constructors; read by normal gameplay callers |
| `DAVA::SoundSystemProxy::vftable` | `0x0360C894` | `0x0320C894` | Exact RTTI-derived table; this is the live singleton's vtable after client startup |
| `SoundSystemProxy::CreateSoundEvent` forwarding thunk | `0x00C4F050` | `0x0084F050` | Loads inner system from `this+0x20`, then jumps through inner vtable slot `+0x0C` |
| `DAVA::WwiseHybridSoundSystem::WwiseHybridSoundSystem` | `0x024F3620` | `0x020F3620` | Installs singleton and final vtable |
| `DAVA::WwiseHybridSoundSystem::vftable` | `0x0381A070` | `0x0341A070` | Exact RTTI-derived table |
| `DAVA::WwiseSoundEvent::vftable` | `0x0381AB84` | `0x0341AB84` | Exact RTTI-derived table |
| `DAVA::WwiseHybridSoundEvent::vftable` | `0x0381AD2C` | `0x0341AD2C` | Exact RTTI-derived table |
| `SoundSystem::CreateSoundEvent` implementation | `0x024F3B70` | `0x020F3B70` | System vtable slot `+0x0C`; returns a hybrid event |
| `FastName::FastName(const char*)` | `0x0090DE50` | `0x0050DE50` | Builds the event and RTPC keys |
| Default sound-group value (`int32_t(-1)`) | `0x0441E264` | `0x0401E264` | Passed by address by normal gameplay callers |
| Ref-counted object `Release` | `0x0091B490` | `0x0051B490` | Used by game callers when replacing/destroying sound events |

Verified `DAVA::SoundEvent` virtual interface slots:

| Vtable offset | Operation | Concrete `WwiseSoundEvent` VA |
| --- | --- | --- |
| `+0x10` | `IsActive()` | `0x0067E300` |
| `+0x14` | `Trigger()` | `0x025006E0` |
| `+0x18` | `Stop(bool force)` | `0x025006D0` |
| `+0x1C` | `SetPaused(bool)` | `0x02500440` |
| `+0x20` | `SetVolume(float)` | `0x025006A0` |
| `+0x24` | `SetSpeed(float)` | `0x0068AE60` |
| `+0x28` | `SetDirection(const Vector3&)` | `0x02500690` |
| `+0x2C` | `SetPosition(const Vector3&)` | `0x025002F0` |
| `+0x30` | `SetVelocity(const Vector3&)` | `0x025003F0` |
| `+0x34` | `SetLoopCount(int32_t)` | `0x00685170` |
| `+0x38` | `SetPriority(int32_t)` | `0x0068AE60` |
| `+0x3C` | `SetParameter(FastName, float)` | `0x02500400` |
| `+0x40` | `GetParameter(FastName)` | `0x024FFF60` |
| `+0x44` | `HasParameter(FastName)` | `0x024FFFB0` |
| `+0x4C` | `GetEventName()` | `0x024FFEC0` |

Creation ABI, confirmed from current gameplay caller `0x01572348`:

```cpp
// x86 thiscall; all addresses are image-base-relative at runtime.
using FastNameCtor = void* (__thiscall*)(void* outFastName, const char* name);
using CreateSoundEvent = void* (__thiscall*)(
    void* soundSystem,
    const void* fastName,
    const int32_t* soundGroup);

auto fastNameCtor =
    reinterpret_cast<FastNameCtor>(imageBase + 0x0050DE50);
void* soundSystem =
    *reinterpret_cast<void**>(imageBase + 0x04020A64);
void** soundSystemVtable =
    *reinterpret_cast<void***>(soundSystem);
auto createSoundEvent =
    reinterpret_cast<CreateSoundEvent>(soundSystemVtable[0x0C / sizeof(void*)]);

alignas(void*) unsigned char fastName[sizeof(void*)]{};
void* event = createSoundEvent(
    soundSystem,
    fastNameCtor(fastName, eventName),
    reinterpret_cast<const int32_t*>(imageBase + 0x0401E264));
```

The production bridge should resolve the singleton and dispatch through the
interface vtables. Direct calls to the concrete Wwise methods are useful as
verification anchors, but are less patch-resistant. These anchors create and
control events already registered in the client's Wwise banks; arbitrary
external WAV/MP3 streaming is a separate path and is not implied by this
event-factory ABI.

Live process validation showed that the singleton is normally a
`DAVA::SoundSystemProxy`, not the hybrid object directly. The proxy stores the
real `WwiseHybridSoundSystem*` at `+0x20`; its `CreateSoundEvent` slot is the
small forwarding thunk above. `WotbModDavaSound` therefore accepts both the
proxy and direct hybrid vtables and always dispatches through slot `+0x0C`.

The event bridge was exercised inside the current client with
`guns/tracers/tracer_hard`: create, name query, volume, position, trigger,
pause/resume, forced stop, RTPC presence/set/get, state tracking, and release
completed without SEH or client termination. `IsActive()` is not used as the
public playing/paused state because it reports the lifetime/activity of the
DAVA event instance and remains true before trigger and immediately after
stop; the API tracks successful lifecycle commands instead.

## Entity list (BigWorld – verified)
Class chain: `BW::BWConnection` → embeds `BW::BWEntities` at `+0x04` → owns
the entity collection.

### Vtables
| Item | VA |
| --- | --- |
| `BW::BWEntity::vftable` (primary, current) | `0x03866920` |
| `BW::BWEntity::vftable` (secondary, EntityExtension MI) | historical row; re-resolve before direct use |
| `BW::BWConnection::vftable` | `0x0385E714` (19 vfuncs) |
| `BW::EntityExtension::vftable` | `0x0385E768` (right after BWConnection vtable) |
| `BW::BWEntityFactory::vftable` | `0x036B0A28` |
| `BW::EntityFactory::vftable` (derived) | `0x036B0A34` |
| `BWPersonalityEx` (BWConnection-derived) vftable | `0x0385DFC0` |

### Constructors / key functions
| Symbol | VA | Notes |
| --- | --- | --- |
| `BWEntity::BWEntity(int id)` | `0x0271B520` / RVA `0x0231B520` | current binding; `retn 4`; construction alone is not public visibility |
| `BWEntity::~BWEntity` | `0x0271B6F0` / RVA `0x0231B6F0` | current non-deleting dtor; plain `retn` |
| `BWConnection::BWConnection` | `0x02717A70` | embeds BWEntities @+4, allocates 36-byte sub @+0x50 |
| `BWEntities::BWEntities(BWConnection*, ...)` | `0x02715ED0` | initializes 3 sub-containers |
| `BWPersonalityEx::ctor` | `0x02713930` | overrides vtable to `0x0385DFC0`, adds tick/render at +0x58/+0x5C |
| `BWEntityFactory::host_ctor` (allocates BWConnection) | `0x01845E80` | builds whole stack: factory + entityList + handlers + BWConnection |
| `processPendingEntities` callback | `0x0186D340` | walks linked list of entity nodes, fires listener vtable |

### BWEntity object layout (offsets in dwords/bytes)
- `+0x00` primary vtable
- `+0x04` secondary vtable (EntityExtension MI)
- `+0x0C..+0x10` 8-byte sub-object via `sub_987CD0`
- `+0x14` link/head ptr, `+0x18` 0
- `+0x14` (this[5]) ← 12-byte heap node `{self,self,*}`
- `+0x20` `WORD = -1` (flags)
- `+0x30` (this[12]) = **entityID** (ctor arg `a2`)
- `+0x34` byte flag
- `+0x38` 0
- `+0x3C` sub_2734470 (3 dwords)
- `+0x54` 0, `+0x58` byte
- `+0x60` `OWORD 0` (transform/quat?)
- `+0x70` byte

### BWEntities collection layout (inside BWConnection at +0x04)
- `+0x04` head/count
- `+0x08` parent BWConnection ptr
- `+0x0C` aux ptr
- `+0x10..+0x24` map/list `sub_2718760` (Entity-by-ID dictionary)
- `+0x28..+0x3C` `sub_27196D0` (pending entity list)
- `+0x40..+0x4C` `sub_271A9E0` (listeners)
- `+0x50` 0

### Other entity-related RTTI
| Item | VA |
| --- | --- |
| `BW::EntityExtension` COL | `0x03A269DC` |
| `BW::EntityEntryBlockingConditionHandler` COL | `0x03A26BB8` |
| `BW::PendingEntitiesBlockingConditionHandler` COL | `0x03A26B04` |
| `BW::BWEntities::EntitiesListener` COL | `0x039E39A4` |
| `BW::BWEntitiesListener` (separate iface) | type desc `0x04354EAC` |
| `OfflineEntity` COL | `0x03966174` |
| `OfflineEntityExtension` COL | `0x03974D58` |
| `OfflineEntityGameLogic` COL | `0x03974DA4` |
| `EntityGameLogicFactory` COL | `0x03974760` |
| `KineticObjectEntityComponent` COL | `0x03974CD8` |
| `CustomEntityQualitySystem` COL | `0x0397FC90` |
| `BW::ClientMessageHandler<resetEntitiesArgs>` COL | `0x03A26F18` |
| `BW::ClientMessageHandler<selectEntityArgs>` COL | `0x03A27178` |
| `BW::ClientMessageHandler<selectAliasedEntityArgs>` COL | `0x03A2712C` |
| `BW::ClientMessageHandler<controlEntityArgs>` COL | `0x03A27A14` |

### BigWorld method registry (for createEntity / cellEntityMethod RPC)
- Registry singleton (cellEntity methods): `0x0449ED90`, ID slot `0x0449EDD4`
- Registry singleton (entity methods): `0x0449EDEC`, IDs `0x0449EE1C`
  (`createEntity`), `0x0449EE20` (`createEntityDetailed`)
- Registrar function (`add(name, idx, ?, ?)`) `0x0273DF50`
- Per-method-name init thunks at `0x005636A0`, `0x00563750`, `0x00563770`,
  `0x00563790` (call them once on startup; results land in
  `dword_449EDD4..dword_449EE20`).

## Vehicle (BWEntity-derived tank — verified)

`Vehicle` is the concrete BWEntity subclass for an in-world tank. In the
11.19.0.834 client it is found via RTTI string `.?AVVehicle@@` at
`0x0429A144` (type descriptor at `0x0429A13C`).

| Item | VA |
| --- | --- |
| `Vehicle` RTTI Type Descriptor | `0x0429A13C` |
| `Vehicle::RTTI Complete Object Locator` (`??_R4Vehicle@@6B@`) | `0x0396EAC0` |
| `Vehicle::vftable` (primary) | `0x0368A2D4` / RVA `0x0328A2D4` |
| `Vehicle::vftable` (secondary, MI for EntityExtension) | `0x0368A33C` |
| `Vehicle::Vehicle(this, entityId)` (ctor) | `0x0154EC10` |
| `Vehicle::~Vehicle()` | `0x01551820` |

Inheritance remains `Vehicle` → `BWEntity` → `MovementFilterTarget@BW` →
`SafeReferenceCount@BW`; re-derive the current CHD/BCA addresses when those
anchors are needed rather than reusing the stale July values.

### Vehicle layout (from ctor `0x0154EC10` body)
- `+0x000` primary vtable (`0x0368A2D4`)
- `+0x004` secondary vtable (`0x0368A33C`, EntityExtension MI)
- `+0x000..+0x77` inherited entity storage; the live `pEntity()` result used by
  the API exposes entity ID at `+0x1C` on this fingerprint
- `+0x078` 12-byte sub-object built by `sub_7D8360` (likely `BW::EntityID` or filter)
- `+0x080` byte player flag, `+0x07E..+0x080` shorts
- `+0x080 (128)` zero, `+0x094 (148) = 15` (capacity-style, std::string SBO?)
- `+0x098 (152)` second std::string SBO header (`+0x0AC=15`)
- `+0x0BC (188)` ptr to 2-element u16 buffer (`v4[0]=0`) — likely component bitmap
  - `+0x0BC` begin = `sub_24E60DB(2)` allocation
  - `+0x0C0` end (begin+1)
  - `+0x0C4` capacity (begin+2)
- `+0x0C8 (200) .. +0x140 (320)` long zero-init region: more components / cached state
- `+0x110, +0x128, +0x134` sub-object init points (called from second ctor variant `0x0154FFBC`):
  - `+0x110` `sub_67CD60` (DAVA::Vector?)
  - `+0x128` `sub_1583FC0`
  - `+0x134` `loc_67CD60`-style container

### Mod API 2.8/2.9 gameplay event and skin anchors

These are the reviewed x86 loader hooks used by
`_mod_tools/mod_api/loader/wotb_mod_loader.cpp`:

| Event source | VA | RVA | ABI observation |
| --- | --- | --- | --- |
| `VehicleGameLogic::onEnterWorld` | `0x01666F60` | `0x01266F60` | register vehicle; first active vehicle infers battle entered/started |
| `VehicleGameLogic::onLeaveWorld` | `0x01668360` | `0x01268360` | emit despawn; last active vehicle infers battle ended/left |
| `VehicleGameLogic::showShooting` | `0x01670370` | `0x01270370` | second argument points to the shot-code byte |
| `VehicleGameLogic::set_health` | `0x0166CEB0` | `0x0126CEB0` | argument points to previous health; read current health after original |
| `AvatarGameLogic::updateVehicleHealth` | `0x01671E00` | `0x01271E00` | resolves and marks the local vehicle |
| `ReloadTimer::setState` | `0x015C64D0` | `0x011C64D0` | reload state/progress/duration/paused payload |
| Avatar vehicle resolver helper | `0x00737540` | `0x00337540` | maps Avatar vehicle wrapper to battle entity |
| `DAVA::UIControl::SystemInput` | `0x00C358A0` | `0x008358A0` | `UIEvent*`; action `+0x38`, screen x/y `+0x10/+0x14`, `retn 4` |
| `DAVA::UITextComponent::vftable` | `0x0369AAA8` (11.20.0.887) | `0x0329AAA8` | RTTI col `0x03987F90`; object `0xE0`, `std::string text` at `+0x44` (getter `sub_E00BC0` copies `this+68`), `UIComponent::control` at `+0x08`; ctor `sub_DEE460`. First written as `0x0029AAA8` (dropped digit) - corrected 2026-09-04 from a live RTTI dump. `UIStaticText` (vtable RVA `0x0327AA7C`, ctor `sub_B8A3A0`) keeps this component at `+0x140`; every UIControl keeps its components at `+0xE4` as a vector of per-type vectors (12-byte inner vectors, dtor `sub_B8BDC0` -> `sub_BC4660`) |
| `DAVA::UIDynamicAtlasTextComponent::vftable` | `0x036B6D58` | `0x032B6D58` | RTTI col `0x039AB710`; ctor `sub_10E7CD0`: `std::string text` at `+0x60`, font name at `+0x78`, `FilePath` at `+0x90`; object `0xE8`. Second text class accepted by `control_get_live_text` |
| `UIShellSelectorControl::OnCurrentAmmoChanged` | `0x015ECAD0` | `0x011ECAD0` | detour reads `[this+0x20]` before/after original; original commits accepted arg1 at `0x015ECDE3`; emit only on committed change; `retn 8` |
| Aim target set handler | `0x01654980` | `0x01254980` | target entity-id pointer arg1, flags byte arg2, current target `[this+0x5C8]` |
| Aim target clear handler | `0x01654F50` | `0x01254F50` | flags byte arg1, current target `[this+0x5C8]`, `retn 4` |
| Arena observed-status handler | `0x015F2DE0` | `0x011F2DE0` | compact payload `[message+0x0C]`; entity id `+0x0C`, observed bool `+0x10` |
| Observed payload getter | `0x02046730` | `0x01C46730` | payload fallback for non-compact message wrapper |
| `GameSceneController::OnVehicleHitDamage` | `0x016EF7D0` | `0x012EF7D0` | two entity ids, hit position, shell kind, flags and shot id; `retn 0x20` |
| `DAVA::File::Create` | `0x00AA0F50` | `0x006A0F50` | cdecl `(FilePath*, uint32_t)`; verified on current 2026-07-30 client; native mesh/material/texture overlay entry |

The `VehicleGameLogic` primary vtable slot 1 returns the battle entity used by
these hooks. In that returned object, entity id is `uint32_t +0x1C` and health
is `int16_t +0xB8`. These offsets describe the getter result used by the API
bridge; they are distinct from the RTTI-derived `Vehicle/BWEntity` layout
above.

For the fingerprinted 11.19.0.834 client, live replay diagnostics proved that
the getter result itself has the exact primary `Vehicle` vtable RVA
`0x0328A2D4`. Before reading team, the loader requires that vtable, requires
the entity id at `+0x1C` to match the hook snapshot, and accepts only native
team `1` or `2` in the low byte at `Vehicle +0xB0`. Live replay diagnostics
showed all seven allied vehicles as team `2`; `0` remains unknown. Any failed
check omits team instead of exposing guessed data.

The original gameplay targets and most new targets begin with guarded x86
bytes `55 8B EC 6A FF`. Aim-set starts `55 8B EC 53 8B 5D 08`; aim-clear
starts `55 8B EC 57 8B F9`. The loader validates the matching prologue before
installing each detour. The runtime publishes only copied snapshots and POD
event payloads; raw game pointers never cross `WotbModHostApi`.

Native sources are installed for all 21 public masks, including UI input,
shell hit, ammo, aim-target and spotted/unspotted. Every detour queues copied
data through `WotbModRuntime_NotifyClientEvent`.
`BATTLE_STARTED`/`BATTLE_ENDED` remain inferred boundaries, not separate
arena-state hooks.

ABI 2.9 also hooks `DAVA::File::Create`. Exact stock paths registered through
`vehicle_skin_register` are resolved only through the owner mod's active
mounts, then passed to the original DAVA loader. This covers `.sc2`,
material/FX YAML and `.tex` without exposing native object pointers.
The 11.19 live loader installed the complete native source set with
`mask=0xFFF (12/12)`, and the API/hook lifecycle completed without failures.
Battle-only payloads were not actively stimulated in a real battle during
that run, so delivery of those individual callbacks remains a field-test item.

### How to identify a Vehicle pointer at runtime (Pointer Probe heuristic)
1. `*(void**)(p + 0)` matches `module + 0x0328A2D4` (`0x0368A2D4` at preferred base) → primary vtable check (most reliable, but vtable VA shifts every patch).
2. `*(uint8_t*)(p + 0x80) ∈ ASCII printable` and is followed by `\0` → playerName.
3. `*(uint8_t*)(p + 0xB0) ∈ {1,2}` → team (`0` is unknown).
4. `*(int32_t*)(p + 0x11C) ∈ [100..6000]` → MaxHP.
5. Three-way confidence is already implemented in `overlay.cpp` Layout=Vehicle.

## Render hook points (verified)

WoT Blitz uses **D3D11** (we already hook `IDXGISwapChain::Present` slot 8).
The DAVA renderer abstraction is `rhi::*` which dispatches to a D3D11 backend.
There are two practical hook layers:

### Layer A — DXGI / D3D11 API (deepest, every draw)
Hook at the same level as Present. We have `g_context: ID3D11DeviceContext*`
(`overlay.cpp`) so we already read its vtable.

| Function | DeviceContext vtable slot | Notes |
| --- | --- | --- |
| `IDXGISwapChain::Present` | slot 8 (already hooked) | one call per frame |
| `IDXGISwapChain::ResizeBuffers` | slot 13 (already hooked) | window resize |
| **`ID3D11DeviceContext::DrawIndexed`** | **slot 12** | classic D3D9 `DrawIndexedPrimitive` equivalent — every triangle batch |
| `ID3D11DeviceContext::Draw` | slot 13 | non-indexed primitives (rare in WoT Blitz) |
| `ID3D11DeviceContext::DrawIndexedInstanced` | slot 20 | tank instancing / particles |
| `ID3D11DeviceContext::DrawInstanced` | slot 21 | rare |
| `ID3D11DeviceContext::OMSetRenderTargets` | slot 33 | catches render-target switches (per-pass boundary) |
| `ID3D11DeviceContext::ClearRenderTargetView` | slot 50 | per-pass start |

Hook recipe (same MinHook pattern we use for Present):
```cpp
void** ctxVtbl = *(void***)g_context;
typedef void (STDMETHODCALLTYPE *DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
DrawIndexedFn original = nullptr;
MH_CreateHook(ctxVtbl[12], &MyDrawIndexed, (void**)&original);
MH_EnableHook(ctxVtbl[12]);
```
Use this layer for: per-draw timing, primitive count, wireframe override (set
PS to white), tank highlight (rebind PS constants right before the call).

### Layer B — DAVA::Renderer / rhi (engine-level)
The historical code range was `0x00B24000..0x00B55000`. No direct
`__FUNCTION__` xref exists for the
inner draw loop (`Engine::BeginFrame`/`EndFrame`/`Scene::Draw`/`RenderPass::DrawLayers`
strings have **no direct xrefs** — DAVA's `LOG_FRAME_DEPTH` macro emits them
via XOR-encoded loads). Practical entry points:

| Function | VA | Use |
| --- | --- | --- |
| `DAVA::Renderer::Initialize` | `0x00B3F3F0` | current one-shot initialization entry |
| `DAVA::Renderer::SetFlow` | `0x00B4CBD0` | current flow selection entry |
| `rhi::RenderLoop::InitializeRenderLoop` | `0x010D8CF0` | current engine-boot entry |

There is **no stable engine-level "per draw" anchor** (every patch reorders
those internal functions and the strings have no xrefs). For per-draw work,
**always use Layer A** (D3D11 DeviceContext slot 12).

### Recipe to re-find DrawIndexed hook target after a game patch
The D3D11 DeviceContext vtable layout is fixed by Microsoft — slot indices
**do not change between game patches**. The only thing that changes is the
runtime address of the vtable, which we already discover dynamically via
`*(void***)g_context` after `D3D11CreateDeviceAndSwapChain` succeeds in
`OverlayThread`. So this hook is **patch-immune**.

### How to dump live entities at runtime
1. Hook `BWEntity::BWEntity` at `0x0271B520` (`__thiscall(_DWORD* this,int id)`).
2. Read `this[12]` for the entity ID and `this[0]` for the concrete subclass
   vtable — that picks Vehicle/Projectile/etc.
3. Walk the BWEntities map by reading the active `BWConnection*` pointer
   (currently transient; locate via `find_immediate(0x0385E714)` write into
   a static `.data` cell once the master loop runs — singleton ptr lives in
   the master container created by `0x01845E80`).
4. Or hook `processPendingEntities` (`0x0186D340`) — its `this` arg points to
   the active list head, so `*this` = first node, walk via `*node` until 0.

## Projectile / Shell (verified)

WoT Blitz has **two distinct projectile representations**:
1. **`dmg::ShellInfo`** — UI/network data model (loadout, ammo type, damage stats).
2. **`Bullet*` / `Tracer*`** — actual in-world physical projectiles (raycast + visual tracer + Bullet physics world).

### `dmg::ShellInfo` (data model — 24 bytes)
| Item | VA | Notes |
| --- | --- | --- |
| RTTI Complete Object Locator | `0x0392B124` | `??_R4ShellInfo@dmg@@6B@` |
| `dmg::ShellInfo::vftable` | `0x03640910` | hit at `0x0364090C+4`, only 1 vfunc (destructor `0x0114D5D0`) |
| `ShellInfoVector::copy_range` | `0x0113D1C0` | 218 B — full layout proven by this loop |
| Default ctor (zero-fill) | `0x013E559B` | writes vtbl, then zeroes [+0x04..+0x14] |
| Copy ctor (deep copy + AddRef) | `0x013E54F2` | copies all 5 dwords; if src[+0x14]≠0 → AddRef(*p+8) |
| Destructor (Release) | `0x028587DB` | `_InterlockedExchangeAdd(p+8,-1)` then virtual delete via `(*p)[1]` |
| Vector field RTTI | `??_R4?$ValueWrapperClass@VOwnedTank@dmg@@V?$Vector@VShellInfo@dmg@@@React@DAVA@@@DAVA@@6B@` `0x03A33BE8` | reactive prop on `dmg::OwnedTank` |

**Verified layout (24 B = 6 dwords)** — proven by `ShellInfoVector::copy_range` pseudocode. Each iteration writes:
```c
dst[0]   = 0x03640910;     // vtable (immediate constant)
dst[1]   = src[1];         // +0x04 plain int
dst[2]   = src[2];         // +0x08 plain int
dst[3]   = src[3];         // +0x0C plain int
dst[4]   = 0;              // +0x10 weak ptr / handle (only valid if dst[5]≠0)
dst[5]   = 0;              // +0x14 intrusive_ptr to descriptor
if (src[5]) {
    dst[4] = src[4];
    dst[5] = src[5];
    _InterlockedIncrement(src[5] + 8);   // AddRef on shared descriptor
}
```

| Off | Size | Type | Meaning |
| --- | --- | --- | --- |
| `+0x00` | 4 | vtbl | `0x03640910` (constant) |
| `+0x04` | 4 | int | plain (likely `shellTypeId` / `compactDescr`) |
| `+0x08` | 4 | int | plain (likely `count` / `damage` — TBD by readers) |
| `+0x0C` | 4 | int | plain (likely `piercingPower` / `flags`) |
| `+0x10` | 4 | ptr | **weak/raw** — only meaningful when +0x14≠0 |
| `+0x14` | 4 | `IntrusivePtr<X>` | refcounted descriptor; refcnt at `*p + 8` (atomic int32) |

**Important:** `dmg::ShellInfo` is in the `dmg::` (damage) namespace — it is the **per-shell loadout/event record**, NOT the per-gun reload-time container. Reload time lives on a separate `ReloadTimer` object, see § "Reload timer (verified)" below. None of the three plain ints at `+0x04/+0x08/+0x0C` are reload time. To finalize the semantic mapping of those three ints, hook `ShellInfoVector::copy_range` (or any ShellInfo ctor) and dump the values, correlating with the visible loadout in the ammo bay UI.

### Reload timer (verified)

**The "reload time" float is on a `ReloadTimer` sub-object hanging off `AvatarGameLogic` — NOT inside `dmg::ShellInfo`.**

| Item | VA | Notes |
| --- | --- | --- |
| `AvatarGameLogic::updateVehicleGunReloadTime(this, byte* state, float* time)` | `0x0166FC90` | size 185, `__thiscall + 3 stack args`, `__FUNCTION__` literal at `0x03691D24` |
| `ReloadTimer::setState(state, durationSec)` | `0x015C4C60` | size 510; the actual setter that stores the reload-time float |
| Cached state byte mirror in AvatarGameLogic | `+0x5C4` | byte (mirror of new state) |
| `ReloadTimer*` slot on AvatarGameLogic | `+0x204` | pointer to ReloadTimer (passed as `ecx` to setter) |
| Aux 8-byte cache zeroed on each update | `+0x208` | likely `(prevDuration, prevProgress)` for tween |
| Listener / observer pointer (vtable slot 3 notify) | `+0x248` | `(*vtbl)[3](listener, durationFloat)` invoked when reload changes |
| Early-exit guard (function returns if non-zero) | `+0x614` | likely "spectator mode" / "vehicle disabled" flag |

**`ReloadTimer` instance layout** (proven by decompile of `0x015C4C60`):

| Off | Size | Type | Meaning |
| --- | --- | --- | --- |
| `+0x0C` | — | sub-object | event-bus base; passed to `sub_116A860` with listener vtables `0x0368B348` (started) / `0x0368B380` (resumed) |
| `+0x3C` | 4 | int | **state code** (enum, set from `state` arg) — see table below |
| `+0x40` | 4 | float | **progress** ∈ [0..1] — set to `1.0f` on state==2; reset to `0` on state==5 |
| `+0x44` | 4 | float | **reload-time duration in seconds** ← **THE VALUE TO READ/WRITE** |
| `+0x48` | 4 | ptr | tick/animator handle (passed to `sub_91BE60(p,-1)` to cancel; rebuilt via `sub_121A0F0` on start) |
| `+0x4C` | 1 | byte | **paused flag** (1 in case 6, 0 elsewhere) |

State enum (from switch in `0x015C4C60`):

| Code | Action |
| --- | --- |
| 0 | idle / clear (paused=0; calls `sub_15BD040`) |
| 1, 8 | no-op stash (paused=0; just store args) |
| 2 | **STARTED** — progress=1.0, schedule animator, fire `0x0368B348` listener |
| 3, 7 | finalize / stop (paused=0; calls `sub_15BD0E0`) |
| 4 | **PAUSE/RESUME** — recompute remaining: `+0x44 = duration / (1 - progress)`; rebuild animator via `sub_121A0F0`; fire `0x0368B380` listener |
| 5 | RESET (progress=0; paused=0) |
| 6 | MANUAL/HOLD (paused=1; cancel+restart animator) |

**Runtime read recipe** (overlay):
```cpp
// once per frame, given AvatarGameLogic* avatar (= localPlayerEntity / mind controller):
auto* rt = *(uint8_t**)((uint8_t*)avatar + 0x204);
if (rt) {
    int    state    = *(int*)  (rt + 0x3C);
    float  progress = *(float*)(rt + 0x40);   // 0 = full, ~1 = ready
    float  duration = *(float*)(rt + 0x44);   // reload time in seconds
    bool   paused   = *(rt + 0x4C);
    float  remainingSec = duration * (1.0f - progress);   // approximate
}
```

**Hook strategy** to verify offsets after a patch:
1. Hook `ShellInfoVector::copy_range` (`0x0113D1C0`) — dump 24-byte ShellInfo array on every clone; correlate the 3 plain ints with current ammo bay UI to label each int.
2. Hook `AvatarGameLogic::updateVehicleGunReloadTime` (`0x0166FC90`) — log `(this, *state, *time)` on every server update; the 3rd arg's float-pointed value IS the reload time.
3. Hook `ReloadTimer::setState` (`0x015C4C60`) directly — log `(this, state, duration)`. `this` will be the same pointer found at `*(avatar+0x204)`, confirming the slot.

YAML config keys (read at tank load, fed into Vehicle later — useful as static anchors after a patch):
- `gunShellReloadTime`        @ `0x036D5ED8`
- `gunClipReloadTime`         @ `0x036D5EEC`
- `gunClipShellReloadTime`    @ `0x036D5F00`
- `gunClipBurstReloadTime`    @ `0x036D5F58`
- `gunReloadTimeFactor`       @ `0x036D3F68`
- `pumpGunReloadTimes` / `pumpReloadTimes` @ `0x035C33B8` / `0x036D4168`
- UI binding path: `**/rotateControl/reloadAndFuelContainer/reloadTimeContainer/reloadTime` @ `0x036FF090`
- Network proto messages: `ReloadTimeUpdate` `0x037217D2`, `ReloadTimeList` `0x03721E57`

### `ShellMovementStrategy` (kinematic flight strategy)
| Item | VA |
| --- | --- |
| Base vftable (current) | `0x036A6954` |
| Derived vftable (current) | `0x036A69D8` |
| Current constructor candidate | `0x016D92A0` / RVA `0x012D92A0`; two stack arguments, `retn 8` |

The previous binding treated this constructor as
`ctor(this, direction, origin, tank)` and expected `retn 0x0C`. The fresh
function accepts only two stack arguments and does not provide a proven tank
owner argument. That old layout/ownership interpretation is invalid for this
build. The native local-shell hook was removed from the loader instead of
moving the RVA with an ABI mismatch. `LOCAL_SHELL_FIRED` therefore has no
native publisher until a separate local-owner-safe source is proved.

### `BulletWorld` (physics world — Bullet physics middleware)
| Item | VA |
| --- | --- |
| `std::_Ref_count_obj2<BulletWorld>` COL | `0x039BEAA4` (singleton via shared_ptr) |

### `BulletManagementSystem` (DAVA scene system)
| Item | VA | Notes |
| --- | --- | --- |
| `BulletManagementSystem::AddEntityToBulletWorld` | `0x006973B0` (size 649) | iterates entity components, calls `BulletWorld::Add` (sub_7839A0) |
| Component-class ID global | `dword_441487C` | passed to `EntityManager::CollectComponents` |
| Layout: `this[15]` = BulletWorld* | `+0x3C` | |
| Layout: `this[16]` = staticObjectsCounter | `+0x40` | tripped at >= 3000 ("increase BulletWorld::MaxBulletObjectsCount") |

### `TracerSystem` / `TracerManager` (visual tracers)
| Item | VA | Notes |
| --- | --- | --- |
| `TracerSystem` COL | `0x0392634C` (also dup `0x0392639C`) | system has 2 vtables (MI) |
| `TracerSystem::vftable` | `0x03639D74` | 3 ctor sites: `0x010F4431`, `0x010F9141`, `0x01104781` |
| `TracerMovementStrategy` COL | `0x039263B0` | |
| `TracerManager` COL | `0x0396FF08` | current RTTI pivot |
| `TracerManager::vftable` | `0x0368BE68` | current primary table |
| `TracerManager::ctor` | `0x0154D8C0` / RVA `0x0114D8C0` | current manager tracking hook |
| `TracerManager::ShowTracer` | `0x0157D670` / RVA `0x0117D670` | current; `retn 0x1C`, seven internal arguments; consumes ref-counted/keyed shell and scene objects |
| configure legacy tracer parameters | `0x015599F0` / RVA `0x011599F0` | reads per-shell `Width`/effect records; not a lifecycle handle creator |
| explosion-path candidate | `0x01570150` / RVA `0x01170150` | manipulates scene/controller state; not proven to be tracer destruction |
| `TracerExplosionListener` (multi-base) | COL `0x03966B08` | inherited at offsets 8, 12, 368 |
| `BulletRaycastManager::RayTestCamera` (string) | `0x035BB8BC` | aim line raycast |
| `Min/Max tracer scale`, `Tracer ally/enemy color` | `0x35AF444 …` | tunable globals (likely in `optionsXxx.yaml`) |

The 11.19 constructor builds ref-counted/keyed per-shell records containing
`Color` and `Width` for `ARMOR_PIERCING`, `ARMOR_PIERCING_CR`,
`HIGH_EXPLOSIVE`, and other shell categories. This proves that stock style
data exists, but not a stable ownership/mutation ABI. Until the keyed-record
layout, manager lifetime, and create/destroy ingress are validated in a live
client, the public tracer-style API must return `E_NOT_SUPPORTED` instead of
writing these objects by guessed offsets.
The same rule applies to `ShowTracer` and the explosion-path candidate: both addresses
are useful research anchors, but neither is installed as a hook or callable
backend. A future adapter must first prove every argument layout, lifetime,
thread/context restriction and local-owner filter; merely knowing the RVA is
not sufficient.

### Bullet entity component strings (DAVA components)
| String | VA |
| --- | --- |
| `BulletBoxComponent` | `0x035B85E8` |
| `BulletChildrenComponent` | `0x035B85FC` |
| `BulletComponent` | `0x035B8614` |
| `BulletExistsComponent` | `0x035B8624` |
| `BulletManagerComponent` | `0x035B863C` |
| `BulletMovableComponent` | `0x035B8654` |
| `BulletPointComponent` | `0x035B866C` |
| `BulletSelectionComponent` | `0x035B8684` |

### Auxiliary RTTI (UI shell views — already on file)
| Item | VA |
| --- | --- |
| `TrayShellListener` COL | `0x0396A9C0` |
| `TrayShellHandler` COL | `0x03970FDC` |
| `ClientEffectsGenerated::TrayShellEffect` COL | `0x03A12304` |
| `UIShellSelectorControl` COL | `0x03970468` |
| `UIShellItemControl` COL | `0x039705B0` |
| `GunStatusViewImpl::ShellView` COL | `0x0396C504` |

## Render (verified via __FUNCTION__ string anchors)

`DAVA::Renderer` is namespace-only (no RTTI). Located via embedded
`__FUNCTION__` literals — every Renderer:: function carries a
`{line, "DAVA::Renderer::Func", "C:/.../Renderer.cpp"}` triple
referenced from the function body (DAVA logger macro).

The historical namespace range was `0x00B24000`–`0x00B55000`. For the fresh
build, only the runtime-relevant entry points below were re-created and
verified from their embedded `__FUNCTION__` strings.

### Functions
| Symbol | VA | Size | Notes |
| --- | --- | --- | --- |
| `DAVA::Renderer::Initialize` | `0x00B3F3F0` / RVA `0x0073F3F0` | 1040 | current `DAVA::Renderer::Initialize` string owner |
| `DAVA::Renderer::ConstructPackLoadingOrder` | `0x00B24C50` | 540  | Returns vector copied into `dword_445456C..0x4454574` |
| `DAVA::Renderer::SetFlow` | `0x00B4CBD0` / RVA `0x0074CBD0` | 569 | current; validates flow and writes `dword_3FCE3D4` |
| `DAVA::Renderer::DumpCompiledShadersStats` | `0x00B2DBA0` | 1700 | Diagnostic dump |
| Renderer initialized predicate (called at SetFlow entry) | `0x00B31260` | 198  | Reads `dword_4456108`/`byte_4453C3A` |
| `Renderer::Shutdown`-like (writes `byte_4453C3A=0`) | `0x00B52AE7` | inside owner | Cleanup branch |
| 3D RenderObject group (vegetation/water/flora/particle) | strings at `0x35ED…0x3628…` | — | Per-class dispatch via `RenderObject::vftable[idx]` |
| `rhi::RenderLoop::InitializeRenderLoop` | `0x010D8CF0` / RVA `0x00CD8CF0` | 1110 | current `rhi::RenderLoop::InitializeRenderLoop` string owner |

### Key globals (historical research; do not call without a fresh xref)
| Symbol | VA | Meaning |
| --- | --- | --- |
| current flow id | `0x03FCE3D4` | current `SetFlow` write target (LOD/quality preset) |
| `dword_4456108` | `0x04456108` | Allowed-flows vector — begin |
| `dword_445610C` | `0x0445610C` | Allowed-flows vector — end |
| `byte_4453C3A` | `0x04453C3A` | Renderer initialized flag |
| `byte_4453C3B` | `0x04453C3B` | Flow-override skip flag |
| `byte_4456335` | `0x04456335` | Shader cache loaded flag |
| `dword_4453C3C` | `0x04453C3C` | rhi device/context handle (CmdQueue ptr) |
| `dword_445456C` | `0x0445456C` | Pack loading order vector — begin |
| `dword_4454570` | `0x04454570` | Pack loading order vector — end |
| `dword_4454574` | `0x04454574` | Pack loading order vector — capacity |
| `xmmword_3FC5C58` | `0x03FC5C58` | Render config block (16 bytes) |
| `qword_3FC5C68` | `0x03FC5C68` | Render flags (bit fields, set in Initialize) |

### RTTI-bearing render classes (high-level only)
| COL | VA |
| --- | --- |
| `DAVA::UIRenderSystem` | `0x0390BDF8` |

### String table (function-name pivots)
| String | VA |
| --- | --- |
| `Engine::BeginFrame` | `0x035DA3EC` (no direct xrefs; called via vtable) |
| `Engine::EndFrame` | `0x035DA400` |
| `rhi::RenderLoop` | `0x035DA9C0` |
| `rhi::RenderLoop::InitializeRenderLoop` | `0x0363873C` (xref `0x010D5AAA`) |
| `rhi::RenderLoop::SuspendRender` | `0x03638784` |
| `rhi::RenderLoop::WaitingWithImmediateCmdCheckingInterval` | `0x03638658` |
| `rhi::RenderLoop::RenderLoopStateSwitcher::SwitchToResume` | `0x035E79E0` |
| `rhi::RenderLoop::RenderLoopStateSwitcher::SwitchToSuspend` | `0x035E7A64` |
| `Scene::Draw` | `0x035DA700` (no direct xrefs — likely inlined) |
| `RenderPass::PrepareArrays` | `0x035DA8B0` |
| `RenderPass::DrawLayers` | `0x035DA8CC` |
| `Helpers::RenderAPIToString` | `0x03675554` |
| `DAVA::RenderOptions::SetOption` | `0x035F71C0` |
| `DAVA::RenderLayer::DrawWireframe` | `0x03626818` |
| `DAVA::TextBlockSoftwareRender::RenderToSprite` | `0x035EC058` |

Note: `Engine::BeginFrame/EndFrame/Scene::Draw` literals have **no
direct data-offset xrefs** because the DAVA logger macro that emits
them uses inline asm in IDA's view (XOR-encoded loads). To locate
those entry points, search via the FastName cache or hook the rhi
command-queue flush directly. Do not treat the historical namespace range as
a current exhaustive function map.

## Client resources (verified via targeted IDA analysis)

These are the reviewed integration points for the mod API resource bridge.
They are intentionally **not** exposed as raw pointers to third-party mods:
the loader owns DAVA C++ objects and translates the public C descriptors into
these calls. The loader/object tables below are the current 11.19 runtime set.
Additional file-system, parser and SceneFile discovery rows retained for
context are 11.18 research anchors unless explicitly identified as current.

### File system and DVPL
| Symbol | VA | Size | ABI / use |
| --- | --- | --- | --- |
| `DAVA::ResourceArchive::ResourceArchive` | `0x009893F0` / RVA `0x005893F0` | 758 | current 11.19; `__thiscall`; opens DVPK/DVPM/ZIP-backed archive from a DAVA `FilePath` and may throw C++ exceptions |
| `DAVA::FileSystem::AddResourceFolder` | `0x00A9BF50` | 1921 | `__thiscall`, `retn 0x20`; adds a prioritized resource root and invalidates the folder cache |
| `DAVA::File::CheckFsDelegateThenTagsOnAbsolutePath` | `0x00A9EC90` | 877 | central file-resolution point; preferred loose-resource overlay hook |
| `DAVA::File::OpenDvplFile` | `0x00AB8780` | 3807 | opens/decompresses `.dvpl` payloads after path resolution |

`AddResourceFolder` stores 32-byte folder records in the vector at
`FileSystem +0xAC/+0xB0/+0xB4`. Its stack arguments contain DAVA-owned
`FilePath`/folder metadata, so a plain C mod must not call it directly.
`WotbModRuntime_ResolveResourcePath` is the stable boundary. The older
`0x00A9EC90` resolver was a discovery candidate; the current production
loader hooks guarded `DAVA::File::Create` at VA `0x00AA0F50` and otherwise
calls the original loader unchanged.

### YAML and UI packages
| Symbol | VA | Size | ABI / use |
| --- | --- | --- | --- |
| `DAVA::YamlParser::ParseFile` | `0x009B2FD0` / RVA `0x005B2FD0` | 901 | current 11.19; opens through native DAVA File and returns parser-owned/ref-counted objects |
| `DAVA::YamlParser::ParseString` | `0x009B3E10` / RVA `0x005B3E10` | 478 | current 11.19; accepts a DAVA string and returns parser-owned objects |
| `DAVA::UIPackagesCache::GetStylesPackage` | `0x00C05850` | 490 | `__thiscall`, `retn 4`; cached style package |
| `DAVA::UIPackageLoader::UIPackageLoader(bool)` | `0x00BDC510` / RVA `0x007DC510` | object `0x88` | current 11.19 bridge; `__thiscall`, `retn 4`; passes `true` |
| `DAVA::UIPackageLoader::~UIPackageLoader` | `0x00BE1C90` / RVA `0x007E1C90` | — | current 11.19 non-deleting destructor |
| `DAVA::UIPackageLoader::LoadControl` | `0x00C118A0` | 2957 | `__thiscall`, `retn 0x10`; constructs a control from package data |
| `DAVA::UIPackageLoader::LoadControlByName` | `0x00C12430` | 498 | `__thiscall`; named control wrapper around `LoadControl` |
| `DAVA::UIPackageLoader::LoadPackage` | `0x00C13990` / RVA `0x00813990` | — | current 11.19 package entry point; `__thiscall`, `retn 8` |
| `DAVA::UIPackageLoader::LoadStyleSheets` | `0x00C145C0` | 4359 | `__thiscall`; loads package style sheets |
| `DAVA::DefaultUIPackageBuilder::DefaultUIPackageBuilder` | `0x00D45A50` / RVA `0x00945A50` | object `0xA0` | current 11.19 bridge; `__thiscall`, `retn 4` |
| `DAVA::DefaultUIPackageBuilder::~DefaultUIPackageBuilder` | `0x00D4EB90` / RVA `0x0094EB90` | — | current 11.19 non-deleting destructor; function starts at `EB90`, not `EBA0` |
| `DAVA::DefaultUIPackageBuilder::GetPackage` | `0x009E9FF0` | — | returns package pointer at builder `+0x5C` |
| `DAVA::UIPackage::ExtractControl(String)` | `0x00BFEDE0` / RVA `0x007FEDE0` | — | current 11.19; `__thiscall`, hidden `RefPtr<UIControl>` return buffer, `retn 8` |

`ResourceArchive` and both YAML parser entries are confirmed research anchors,
not public callable backends. Their contracts expose DAVA-owned C++ values,
ref-counted parser/archive objects and exception behavior across the module
boundary. The stable API therefore uses bounded portable YAML/archive readers
and returns `WOTBMOD_V3_E_NOT_SUPPORTED` for native DAVA object access until an
exact-build provider proves construction, destruction and cross-CRT exception
handling. A loader-private typed provider ABI now exists and converts YAML and
archive objects into bounded portable snapshots, but the 11.19 provider does
not advertise either capability because those object contracts remain
unproven.

Object/vtable anchors used for runtime validation:

| Object | VA | RVA |
| --- | --- | --- |
| `UIPackageLoader::vftable` | `0x036070A8` | `0x032070A8` |
| `UIPackage::vftable` | `0x03606F6C` | `0x03206F6C` |
| base `UIControl::vftable` | `0x03605F60` | `0x03205F60` |
| refcount `Retain` | `0x0091B9F0` | `0x0051B9F0` |
| refcount `Release` | `0x0091B490` | `0x0051B490` |

Verified base `UIControl` vtable operations used by ABI 2.5+:

| Vtable slot | Symbol | VA | ABI / use |
| ---: | --- | --- | --- |
| 4 | `DAVA::UIControl::SetPosition` | `0x00C2D5B0` | writes `Vector2` at `UIControl +0x6C` and dirties layout |
| 5 | `DAVA::UIControl::SetSize` | `0x00C2E680` | writes `Vector2` at `UIControl +0x74` and dirties layout |
| 6 | `DAVA::UIControl::SetInputEnabled` | `0x00C2C6A0` | `__thiscall(UIControl*, bool enabled, bool hierarchical)` |
| 7 | `DAVA::UIControl::SetDisabled` | `0x00C2BAD0` | `__thiscall(UIControl*, bool disabled, bool hierarchical)` |
| 11 | `DAVA::UIControl::AddControl` | `0x00BE58F0` | retains child, detaches prior parent, then installs new parent |
| 12 | `DAVA::UIControl::RemoveControl` | `0x00C279F0` | detaches and balances hierarchy ownership |
| 13 | `DAVA::UIControl::BringChildToFront` | `0x00BE7680` | retains an attached child, unlinks its intrusive-list node, reinserts it at the tail, dirties hierarchy, then releases the temporary retain |

Slot 10 (`0x00C358A0`) is `SystemInput`, **not** visibility. Do not call it
with a boolean. ABI 2.8 visibility changes the verified visible bit at
`UIControl +0x4C` and sets hierarchy dirty bit `0x08` at `+0x4D`.

The bridge resolves these through the loaded object's own vtable after first
checking that the object derives from the verified base `UIControl` table.
Third-party mods receive only `WotbModResourceHandle`; the slot addresses do
not cross the public ABI.

`WOTBMOD_RESOURCE_UI_PACKAGE` and `WOTBMOD_RESOURCE_UI_CONTROL` route through
`WotbModDavaResources_Create`. The bridge builds a real `UIPackage`, retains
it for `UI_PACKAGE`, or extracts an owning `RefPtr<UIControl>` by
`object_name` for `UI_CONTROL`. Reload creates the replacement first and only
then releases the old object.

ABI 2.6 adds direct creation and active-screen acquisition:

| Item | VA | RVA / offset | ABI / use |
| --- | --- | --- | --- |
| client `operator new` | `0x024ECE2B` | `0x020ECE2B` | `__cdecl(size)`; all native DAVA objects must use the client heap |
| client `operator delete` | `0x024ED0AC` | `0x020ED0AC` | `__cdecl(ptr, size)`; paired with the client allocator |
| `DAVA::UIControl::UIControl(Rect)` | `0x00BD9080` | `0x007D9080` | object size `0x140`; empty/default rect is accepted |
| `DAVA::EngineContext::GetInstance` | `0x00AAB150` | `0x006AB150` | returns the live engine context |
| `EngineContext::uiControlSystem` | — | `EngineContext +0x3C` | live `UIControlSystem*` |
| `DAVA::UIControlSystem::GetScreen` | `0x00C05790` | `0x00805790` | returns current screen at `UIControlSystem +0xDC` |

`ui_control_create` allocates `0x140` bytes with the client allocator and
calls the real constructor. The returned object owns its constructor
reference. `ui_get_active_screen` obtains the current screen, validates its
base vtable, and takes a new `Retain` before publishing the opaque handle.
Both handles are released only through DAVA `Release`; direct client heap
deallocation is never exposed to mods.

ABI 2.8 existing-UI layout and traversal anchors:

| Field / helper | VA / offset | Evidence / use |
| --- | --- | --- |
| `FastName::FastName(const char*)` | `0x0090DE50` / RVA `0x0050DE50` | builds lookup key for `ui_control_find_by_name` |
| control `FastName` | `UIControl +0x30` | compared by FastName id |
| parent pointer | `UIControl +0x38` | retained before returning public handle |
| child-list sentinel | `UIControl +0x44` | intrusive list; node child pointer at `node +0x08` |
| flags / hierarchy dirty | `+0x4C` / `+0x4D` | visible bit `0x02`; dirty bit `0x08` |
| position / size | `+0x6C` / `+0x74` | two `Vector2` values |
| input processor count | `+0x94` | nonzero means input-enabled |
| control state | `+0xA0` | disabled bit `1 << 3` |

Find/parent/child results take an independent `Retain` and are released
through the existing refcount `Release` anchor.

### Scenes and custom 3D models
| Symbol | VA | Size | ABI / use |
| --- | --- | --- | --- |
| `DAVA::SceneFileV2::LoadArchive` | `0x00D00CB0` | 2836 | `__thiscall`, `retn 0x0C` |
| `DAVA::SceneFileV2::LoadArchiveToScene` | `0x00D017D0` | 7046 | `__thiscall`, `retn 0x0C` |
| `DAVA::Entity::LoadComponent` | `0x00D04B40` | 1001 | `__thiscall`, `retn 8` |
| `DAVA::Scene::Scene(32, 0)` | `0x00CCF190` / RVA `0x008CF190` | object `0x1C0` | current 11.19; `__thiscall`, `retn 8`; vtable becomes `0x03612764` |
| `DAVA::EntityCache::LoadEntityUnsafe` | `0x00D06A00` / RVA `0x00906A00` | — | current 11.19; hidden `RefPtr<Scene>` return + `FilePath` |
| Blitz async scene wrapper | `0x00D06C10` / RVA `0x00906C10` | — | current 11.19 wrapper used by the bridge |
| `DAVA::SceneFileV2::LoadHierarchyFromArchive` | `0x00D05F40` | 1516 | `__thiscall`, `retn 0x14` |
| `DAVA::SceneFileV2::LoadSceneArchive` | `0x00D07880` | 3597 | archive-to-scene orchestration |
| `DAVA::SceneIoFlowManager::LoadSceneFromFile` | `0x00D09BC0` / RVA `0x00909BC0` | — | current 11.19 high-level scene-file hook |

Verified Scene/Entity operations used by ABI 2.5+:

| Item | VA / offset | RVA / slot | Evidence / use |
| --- | --- | --- | --- |
| `DAVA::Scene` vtable | `0x03612764` | `0x03212764` | object validation |
| `DAVA::Entity::AddNode` | `0x00CD6C10` | Scene vtable slot 5 | retains child, detaches old parent, propagates scene/parent |
| `DAVA::Entity::RemoveNode` | `0x00D10C90` | Scene vtable slot 6 | detaches child and balances ownership |
| `Entity::transformComponent` | `Entity +0x3C` | — | read directly in `AddNode` and transform call paths |
| `DAVA::TransformComponent::SetLocalTransform` | `0x00D16070` | `0x00916070` | copies DAVA `Transform`, updates world transform or marks local dirty |

ABI 2.6 creation and active-scene anchors:

| Item | VA | RVA / slot | Evidence / use |
| --- | --- | --- | --- |
| `DAVA::Entity::Entity` | `0x00CCE410` | `0x008CE410` | client-heap object size `0x50`; final vtable `0x036125E8` / RVA `0x032125E8` |
| `DAVA::Scene::Draw` | `0x00CEEF40` | `0x008EEF40`, Scene slot 21 | virtual frame hook; refreshes the active-scene candidate |
| `DAVA::Scene::Activate` | `0x00CD5A20` | `0x008D5A20`, Scene slot 22 | stores active state byte `Scene +0x100 = 1` |
| `DAVA::Scene::Deactivate` | `0x00CEBC60` | `0x008EBC60`, Scene slot 23 | stores active state byte `Scene +0x100 = 0` |

`scene_entity_create` uses the client allocator and the real `Entity`
constructor, then publishes the constructor-owned reference.
`scene_get_active` uses the Activate/Deactivate/Draw tracker, validates the
Scene vtable and `+0x100` active byte, and takes a new `Retain`. If no 3D
scene has been activated yet (for example at the early login/loading UI), the
public result is deliberately `WOTBMOD_ERROR_NOT_FOUND`, not a fabricated
Scene.

The x86 DAVA `Transform` passed to `SetLocalTransform` is 40 bytes:
translation `Vector3` (12), scale `Vector3` (12), then rotation quaternion
`x,y,z,w` (16). The public `WotbModSceneTransform` is reordered explicitly
inside the loader bridge; it is not cast to the C++ engine type.

The production bridge calls `LoadEntityUnsafe` at RVA `0x00906A00`, validates
the resulting Scene vtable at RVA `0x03212764`, and releases through DAVA
refcounting. Custom models use the same overlay path as UI: mount a mod
directory at a `~res:/...` root and request a loose `.sc2` path. Referenced
textures/materials must also be reachable by the client.

Live evidence on 11.19.0.834 x86: package/control/scene each completed
load/reload/release; UI rect, visibility and add/remove child returned `OK`;
Scene local transform and add/remove child returned `OK`; direct
`UIControl`/`Entity` construction, mutation, reload rejection and release
also returned `OK`. The early UI-only window correctly returned `NOT_FOUND`;
after the hangar Scene activated, `scene_get_active` succeeded. A loose
custom `.sc2` and a constructed `Entity` were transformed, attached to the
active Scene for 180 consecutive frames, detached and released.
`UIPackageLoader::LoadPackage` hook hit 206 times and
`SceneIoFlowManager::LoadSceneFromFile` hook hit 38 times; the client remained
responsive (`142 PASS / 0 SKIP / 0 FAIL`).

ABI 2.7 client events reuse these verified anchors and add no raw addresses
to the public surface. The loader compares native identity from
`UIControlSystem::GetScreen` for `UI_SCREEN_CHANGED`; the existing
`Scene::Activate`/`Deactivate`/`Draw` tracker emits
`SCENE_ACTIVATED`/`SCENE_DEACTIVATED`. Runtime clones the retained DAVA
wrapper at ingress, publishes callback-scoped borrowed handles, and exposes
only `resource_clone` for retaining them past callback return.

Live ABI 2.7 evidence: 7 UI-screen changes, 2 Scene activations and 1 Scene
deactivation were delivered on the dispatch thread with monotonic sequence.
Borrowed release/reload were denied, the stale handle was rejected after the
callback, and clones of both a `UIControl` and `Scene` remained valid until
explicit release.

### Native material framework

| Item | VA | RVA / evidence |
| --- | --- | --- |
| `DAVA::NMaterial::NMaterial` | `0x009CF190` | RVA `0x005CF190`; one native material-name/FastName argument |
| `NMaterial` primary vtable | `0x035EC458` | written by the current constructor |
| `NMaterial` secondary vtable | `0x035EC474` | written at subobject `+0x20` |

The constructor and dirty-state initialization are confirmed, but a safe
loader-owned property binding/removal, parent-material ownership and
destruction ABI are not. The portable managed render material API remains
available; native Scene `NMaterial` mutation and shader replacement continue
to return `WOTBMOD_V3_E_NOT_SUPPORTED`.

### Loader-private DAVA provider boundary (2026-08-16)

`WotbModDavaNativeBackend` v1 is a typed, loader/runtime-only boundary for six
capability groups: YAML, ResourceArchive, NMaterial mutation, loaded-mesh
hot-swap, stock tracer creation and a reviewed class registry. It deliberately
does not expose provider tokens, engine pointers, arbitrary calls, vtables or
ObjectFactory to mods/Lua. The registry wraps provider tokens in owner/kind
checked host tokens and provides invocation quiescence during backend removal.

The current exact-build resource provider advertises only the class registry
for `DAVA::UIControl` and `DAVA::Entity`, reusing the already guarded resource
constructors/destructors. It does not advertise YAML, archive, NMaterial, mesh
or tracer groups. These five groups are host-tested with provider doubles but
remain `NOT_SUPPORTED` for the client until their complete ABI and live
behavior are proven.

## Input
| Item | VA |
| --- | --- |
| `DAVA::UIInputSystemActionData` value wrapper COL | `0x0391A6A8` |

`InputSystem` itself has no RTTI; pivot via `UIControl::OnInput` vtable slots.

## UI state (legacy 11.18 research baseline; not used by the current API bridge)
| Item | VA |
| --- | --- |
| `DAVA::UIControlSystem::SetScreen` | `0x00C2D210` (renamed in IDB; 1236 B) |
| `DAVA::UIScreenManager::SetScreen` | `0x00C2D700` (renamed in IDB; 632 B) |
| `UIControlSystem::ScreenChanged` (callback) | `0x00C208A0` |
| FastName / refcount Retain | `0x0091B280` |
| FastName / refcount Release | `0x0091AD20` |
| `UIScreenEx` COL / vtable | `0x03967458` / `0x03683704` |
| `UIScreenDelegate` COL / vtable | `0x0396FE34` / `0x0368CD7C` |
| `DLCServerEmuUIController` COL | `0x039AF4E0` |
| `UIShellSelectorControl` COL | `0x03970468` |
| `UIShellItemControl` COL | `0x039705B0` |
| `DAVA::SharedUIControlTextureSingleComponent` COL | `0x038F42A8` |
| `UIInputSystem` RTTI Class Hierarchy Descriptor | `0x0391879C` |
| `UIInputSystem` RTTI Type Descriptor | `0x04001B6C` |
| `UIControlSystem_popupContainer` FastName literal | `0x036074F4` (current) |
| `EngineContext` (DAVA) string | `0x0361846C` |

`UIScreenManager` layout (from `SetScreen` decompile):
- `+0x00` `UIControlSystem*` — owns/reaches the live screen state
- `+0x04` registered-screen container (multiset/vector by id, accessed via `sub_68F500`)
- `+0x08` cached "current registered screen" pointer
- `+0x24` active screen pointer (`this[9]`)

`UIControlSystem` layout (from `SetScreen` decompile):
- `+0xDC (220)` currentScreen pointer
- `+0xE0 (224)` nextScreen pointer (the actual UI-state slot toggled per frame)
- `+0x10C (268)` `byte switchPending` (set when nextScreen becomes null → `ScreenChanged` callback fires)
- Screen object: `+0x30` `FastName screenId` (the human-readable id used in error logs)

Behaviour summary:
- `UIScreenManager::SetScreen(this, screenId)` looks up screen by id in container `+0x04`, falls back to cached `this[2]`, stores active screen at `this[9]`, then calls `UIControlSystem::SetScreen(*this, &screen)`.
- `UIControlSystem::SetScreen` only stores `nextScreen`; the swap from `next → current` happens on the next frame in `ScreenChanged` (`sub_C208A0`).
- Static "null" FastName cache: `dword_4457AC8` (id) / `unk_4457AC4` / `unk_4457C53` (used for empty-screen names in logs).
- `UIControlSystem` singleton is held by `DAVA::EngineContext`; the current
  `UIControlSystem_popupContainer` string owner starts at `0x00C09D80`.

`UIInputSystem` has RTTI (type descriptor + class hierarchy) but no Complete Object Locator — it is a **non-polymorphic** helper held inside `EngineContext`. Real per-control input dispatch goes through `UIControl::OnInput` vtable slots (game-side hooks include `InputBindingTabController::OnInputTrapped` at string `0x03646548` and `DAVA::Automation::EngineApiHelper::OnInput` at `0x0386C550`).

## Method to locate vtable from COL
1. Compute little-endian bytes of COL VA.
2. `search_bytes(those bytes)` — single hit in `.rdata` ⇒ that hit + 4 = vtable start.
3. `find_immediate(vtable_VA)` ⇒ all places where vtable is written into objects (ctors).

## Re-verifying offsets after a game update

When Wargaming ships a new `wotblitz.exe`, every absolute VA in this file becomes invalid (PE rebases code, MSVC reorders functions, layout may shift if a field was added). The **anchor strings and RTTI metadata are stable** — we use them to re-derive everything mechanically.

### 0. Quick triage — has anything actually changed?
```
# from wotb root
fc /b version.dll.bak version.dll          # only checks our DLL
Get-FileHash wotblitz.exe -Algorithm SHA256 # store hash per-build in a side file
```
If SHA differs, do the full re-anchor pass below.

### 1. Re-open the new IDB
1. Move/delete old `wotblitz.exe.{id0,id1,id2,nam,til}` (or rename to `*.bak.<oldver>`).
2. `open_database(file_path="...\wotblitz.exe")` → `wait_for_analysis`.
3. Confirm new `ImageBase` via `get_database_info` (still 0x00400000 historically, but verify).

### 2. Re-derive each section by anchor type

Use the **anchor type** — string literal, RTTI name, or unique constant — listed below. Each entry tells the agent how to rebuild the section without prior knowledge.

| Section | Anchor (search this) | Pivot |
| --- | --- | --- |
| **Camera** | RTTI string `".?AVGameCamera@@"` → COL via xref | COL → vtable (search little-endian COL VA in `.rdata`, +4 is vtable) → `find_immediate(vtable_VA)` finds **ctor write sites**. Walk ctor for 7 calls to `Matrix4::Identity` — slot offsets confirm matrix block. |
| **DAVA::Camera helpers** | `__FUNCTION__` literals: `"DAVA::Camera::SetupPerspective"`, `"DAVA::Camera::SetTarget"` etc. | `get_strings` filter on `DAVA::Camera::` → for each, `get_xrefs_to(string_VA)` lands inside the function body (DAVA logger pattern: `{line, __FUNCTION__, __FILE__}` triple). The enclosing function = the helper. |
| **Renderer** | `__FUNCTION__` literals: `"DAVA::Renderer::Initialize"`, `"DAVA::Renderer::SetFlow"` etc. | Same pivot. The full namespace range becomes `[min(funcStart) .. max(funcEnd)]`. |
| **Entity list / BWEntity** | RTTI `".?AVBWEntity@BW@@"`, `".?AVBWConnection@BW@@"`, `".?AVBWEntities@BW@@"` | COL → vtable → `find_immediate` to ctor. BWEntities is embedded at `+0x04` of BWConnection (find by `mov [esi+4], offset BWEntities_vtbl` pattern). |
| **Vehicle (tank)** | RTTI `".?AVVehicle@@"` (string at `0x04290CD4`, type desc 8 bytes earlier) | `??_R4Vehicle@@6B@` is the COL — list_demangled_names with filter `"Vehicle"` finds it directly. Search bytes for LE-encoded COL → +4 = vtable. The ctor is the **only function** that writes `mov [reg], offset vtable; call BWEntity_ctor` — find via xrefs to vtable VA, then look at the function that calls BWEntity ctor. |
| **Render hook (D3D11)** | None needed — `g_context` vtable is discovered at runtime in `OverlayThread`. Slot indices are fixed by Microsoft (DrawIndexed=12, OMSetRenderTargets=33, ClearRTV=50). | Patch-immune. |
| **BigWorld method registry** | strings `"createEntity"`, `"createEntityDetailed"`, `"cellEntityMethod"` | xref from registrar (`add(name,idx,...)`). Registrar function = whoever takes 4 args and writes to `dword_*` in `.data`. |
| **Projectile / ShellInfo** | RTTI `".?AUShellInfo@dmg@@"`, `".?AVShellMovementStrategy@@"`, `".?AVTracerSystem@@"`, `".?AVTracerManager@@"`, `".?AVBulletWorld@@"` | COL → vtable → ctor. Layout offsets re-derived from ctor body. |
| **UI state** | `"UIControlSystem_popupContainer"`, `"UIScreenManager.cpp"` (DAVA file path), RTTI `".?AVUIControlSystem@DAVA@@"`, `".?AVUIScreenManager@DAVA@@"`, `".?AVUIInputSystem@DAVA@@"` | RTTI for type/class-hierarchy descriptor exists even when COL doesn't. Cross-reference type descriptor → class hierarchy → CHD parent traversal gives the inheritance graph. SetScreen functions: search FastName literals like `"_DAVA_DEFAULT_SCREEN"` or any string the function logs; failing that, the ctor of `UIScreenManager` references the screen-id container. |
| **UIControl object API** | RTTI `".?AVUIControl@DAVA@@"`, upstream `UIControl.cpp` order | COL → base vtable; re-check slots 4/5/6/7/11/12 and the `+0x30/+0x38/+0x44/+0x4C/+0x6C/+0x74/+0x94/+0xA0` fields. Never reuse slot 10 as visibility without new proof. |
| **Scene object API** | RTTI `".?AVScene@DAVA@@"`, `"TransformComponent.cpp"`, upstream `Entity.cpp` | Scene COL → vtable; re-check AddNode/RemoveNode slots 5/6. Pivot from `TransformComponent` setters to re-derive Entity component offset and `SetLocalTransform` RVA/layout. |
| **Input** | RTTI `".?AVUIInputSystem@DAVA@@"`, string `"UIInputSystem.cpp"`, vfunc names via PDB-style mangling on UIControl `".?AVUIControl@DAVA@@"` vtable. | UIControl COL → vtable → slot[OnInput] index (in DAVA always slot ~14, verify by argument types: `(this, UIEvent*)`). |

### 3. Verify Camera matrix layout at runtime (definitive check)

The overlay already has the **GameCamera Pointer Probe** (`Insert` → Pointer Probe → Layout=GameCamera). It now:
- shows all 7 matrix slots `+0x080..+0x200` with labels;
- computes `view * proj` and compares to `+0x140`. If `max|delta| < 0.01` ⇒ **layout is unchanged**, all matrix offsets in this file are still correct.
- if mismatch ⇒ a field was added/removed before the matrix block. Step the dump in 64 B increments around `+0x080..+0x300`, find the new identity-init pattern in the new ctor, and shift `slots[]` in `overlay.cpp` by the delta.

Getting a live `GameCamera*`:
1. Hook ctor (re-find via the new RTTI as above) — log `this` first time it's called.
2. Or scan engine globals: `find_immediate(<new_GameCamera_vtable_VA>)` → in ctor it's stored to `[ecx]`; the singleton holding the camera is reachable from `GameScene` (still `+0x128`, but verify by probing layout=GameScene).

### 4. Verify entity ID layout

Pointer Probe → Vehicle layout already validates `playerName/team/maxHP` — three independent sanity bits. If any drift: dump first 0x300 bytes of a known vehicle (use `:teleport` UI or shoot a tank) and re-locate the ASCII player name and the `0..1` team byte by hand.

### 5. Update workflow checklist

1. ✅ Backup `version.dll`, `re_anchors.md`, `_mod_tools/proxy_dll/overlay.cpp` (git-commit, don't overwrite).
2. ✅ Open new IDB, run section-by-section anchor pivots from the table above.
3. ✅ For each section: regenerate the VA table in `re_anchors.md`. **Keep the old table in a `## Legacy (vX.Y.Z)` block** at the bottom — useful diff reference.
4. ✅ Update `_mod_tools/proxy_dll/overlay.cpp` constants:
   - `Vehicle` offsets (`0x80/0xB0/0x11C`)
   - `VehicleGameLogic` (`+0x04, +0x1B8`)
   - `GameScene::camera*` (`+0x128`)
   - `GameCamera::slots[]` (matrix block)
   - `TankVisual` (`+0x04, +0x14, +0x28, +0x2C, +0x60`)
5. ✅ Update direct-hook bindings (for example
   `LeaveToHangarCallback 0x01572B50`). These move every patch.
6. ✅ Rebuild: `cd _mod_tools\proxy_dll && build.cmd`.
7. ✅ Launch game, press `Insert`, run Pointer Probe on every layout — all green = success.
8. ✅ Update `/memories/repo/wotb_mod_reverse_notes.md` with new build hash + date.

### 6. Anchors that DON'T move

These are stable across patches and should stay in this file as constants:
- All RTTI mangled names (`.?AV...@@`) — Wargaming uses MSVC and doesn't obfuscate.
- `__FUNCTION__` strings (`DAVA::*::*`).
- `.cpp` source file paths in DAVA logger triples.
- DAVA::Camera canonical layout (matrices at +0x80..+0x200) — frozen since DAVA 2017+.
- BigWorld method names (`createEntity`, `cellEntityMethod`, etc.).

What moves:
- All function VAs (every patch).
- vtable VAs (most patches — `.rdata` shifts when shaders/strings are added).
- COL VAs (every patch).
- Field offsets — usually stable, occasionally shift by 4–8 bytes when a feature lands.

## Native validation binding registry (11.19.0.834)

`wotbmod.native_validation` не добавляет новые public anchors и не раскрывает
модам абсолютные writable pointers. Для ручного доказательства он использует
стабильные binding ID из уже проверенного pack и показывает максимум
`LIVE_TEST_PENDING` до PASS пользователя на точном fingerprint.

| Binding ID | RVA | Loader validation | До live PASS |
| --- | ---: | --- | --- |
| `GameCamera::ctor` | `0x0119B2F0` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `GameCamera::dtor` | `0x011A6FB0` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `GameCamera::vtable` | `0x03290080` | readable identity + runtime object check | `LIVE_TEST_PENDING` |
| `DAVA::Camera::SetFovY` | `0x01090B20` | exact prologue before direct call | `LIVE_TEST_PENDING` |
| `Client::Initialize` | `0x01154A70` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `Client::LeaveToHangar` | `0x01172B50` | exact prologue + captured owner | `LIVE_TEST_PENDING` |
| `BWEntity::ctor` | `0x0231B520` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `BWEntity::dtor` | `0x0231B6F0` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `TracerManager::ctor` | `0x0114D8C0` | prologue + MinHook install | `LIVE_TEST_PENDING` |
| `TracerManager::vtable` | `0x0328BE68` | readable identity + runtime object check | `LIVE_TEST_PENDING` |
| `CameraModeChanged` | `0x0116ED10` | prologue + MinHook install | `LIVE_TEST_PENDING` |

Loader пишет по каждой записи строку вида
`V3 native binding id=... kind=... rva=... state=BOUND|FAILED`. Это private
diagnostics: V3-моду передаётся binding ID и capability status, а не адрес
для чтения/записи. Hash mismatch отключает весь fixed-RVA pack до установки
hooks. Полный пользовательский сценарий находится в
`mod_api/MANUAL_LIVE_VALIDATION_RU.md`.

### MAIN async ingress — ЗАКРЫТО 2026-08-15

Раньше здесь стоял запрос: нужна функция, вызываемая **каждый кадр в главном
потоке** (не по вводу). `DAVA::UIControl::SystemInput` не подходил — он
срабатывает только при действии игрока. Кандидаты `UIControlSystem::Update`,
`Core::SystemProcessFrame`, `Engine::Update` в заметках отсутствовали.

Функция найдена через литералы profiler-маркеров DAVA:

**`DAVA::Private::EngineBackend::UpdateAndDrawWindows`**
`0x00AC7090` / RVA `0x006C7090`, `__thiscall(EngineBackend*, float frameDelta,
bool skipUpdate)`, `retn 8` на `0x00AC7251`.
Пролог `55 8B EC 6A FF 68 2D 8C 03 03 64 A1 00 00 00 00`.

Доказательства:

- открывает profiler-scope `"EngineBackend::UpdateAndDrawWindows"`
  (`0x035E24B4`) на `0x00AC70C5`;
- идёт по вектору окон `[this+0x58]..[this+0x5C)` и на каждое открывает
  `"Engine::BeginFrame"` (`0x00AC70FD`), `"Window::Update"` (`0x00AC7139`),
  `"Window::Draw"` (`0x00AC7198`), `"Engine::EndFrame"` (`0x00AC71EB`).

**Поток доказан, а не предположен.** `get_xrefs_to(0x00AC7090)` возвращает
ровно одного вызывающего — `0x00ABF653` внутри функции `0x00ABF320`, и это
Win32-цикл сообщений: `PeekMessageW` на `0x00ABF4F0` и `0x00ABF535`,
`TranslateMessage` на `0x00ABF516`, `DispatchMessageW` на `0x00ABF520`, сразу
за ними scope `"Engine::OnFrame"` (`0x00ABF54F`) и `"Engine::Update"`
(`0x00ABF5E3`). Поток, который качает очередь сообщений окна, и есть поток,
которому окно принадлежит. Значит вызов идёт в главном потоке, раз в кадр,
безусловно — включая экраны без 3D-сцены.

Что делать: связать `kEngineUpdateAndDrawWindowsRva = 0x006C7090`, проверять
пролог `55 8B EC 6A FF 68`, вызывать `PumpMainThread(64)` и
`SetMainIngressOnline(true)` из детура под тем же гейтом отпечатка, что и
остальные gameplay-хуки, и возвращаться через оригинал с нетронутыми 8
байтами аргументов.

**Почему не `Scene::Draw`.** `Scene::Draw` (`0x00CEEF40`, slot 21 vtable
`Scene`, RVA `0x008EEF40`) подтверждён независимо — он открывает scope через
`off_3FACDA0` → литерал `"Scene::Draw"` `0x035E2790`. Но как общий ingress он
не годится по трём причинам: он вообще не тикает во время логина, загрузки и
на чисто-UI экранах — ровно в том окне, где очередь молча встала бы; это
виртуальный слот, вызываемый раз на отрисованный `UI3DView`, то есть его
частота не «раз в кадр» и не гарантированно ненулевая; и он исполняется
внутри draw-прохода, худшего места для mod-callback. Для трекера активной
сцены и для снапшота дерева сцены он по-прежнему правильный выбор.

### Camera state machine `CameraController+0x5C` — 2026-08-15

Это **не** enum режимов камеры. Это машина состояний *контроллеров анимации*.
Аркада/снайпер живут в другом поле — см. коррекцию `0x0156ED10` выше.

- `CameraController::StartNewState` `0x015C99B0` / RVA `0x011C99B0`, `retn 0`.
  Switch по `[this+0x5C]`, 7 case, таблица переходов `0x015C9C60`,
  default `0x015C9AF3` (логирует `"Unknown state:"`).
- `CameraController::SwitchState` `0x015A9FA0` / RVA `0x011A9FA0`,
  `__thiscall(int newState)`, `retn 4`. Хвост: `mov [esi+5Ch], edi ; call
  sub_15C99B0`. Поле пишется **только** отсюда и из конструктора
  (`0x01598D90`, пишет начальное значение `5` на `0x01598EEE`), поэтому детур
  на `SwitchState` видит все переходы.

Имена состояний восстановлены по RTTI объектов, которые конструктор создаёт в
слотах контроллера (`[vtable-4]` → COL → type descriptor):

| case | слот | размер | vtable | класс |
|---|---|---:|---|---|
| 2 | `+0x68` | `0x10` | `0x03692D1C` | `LookOutAnimationController` |
| 3 | `+0x6C` | `0x58` | `0x03692D94` | **`PostMortemAnimationController`** |
| 4 | `+0x74` | `0x10` | `0x03692CF0` | `LookOnTargetAnimationController` |
| 6 | `+0x70` | `0x20` | `0x03692D68` | `ObserverAnimationController` |
| — | `+0x64` | `0x18` | `0x03692D48` | `ManualZoomController` (не case) |

`5` — начальное/пустое состояние (нет объекта, `Stop()` не вызывается).
`0` и `1` — два обычных состояния без контроллера анимации; **их именовать
нельзя**, статические данные противоречивы, и это не нужно: бит аркада/снайпер
живёт в `GameCamera+0x320`.

Независимая перекрёстная проверка: собственный switch `SwitchState`
(`0x015A9FE7`) индексирует `byte_15AA184 = 00 00 01 01 01 00 01` для состояний
0..6 — то есть `Stop()` зовётся для 2,3,4,6 и не зовётся для 0,1,5. Ровно те
же case, у которых есть объект состояния. Две независимые таблицы сходятся.

`FreeCamera` в этой машине состояний **отсутствует**: это подкласс
`GameCamera`, который принадлежит другому объекту (слот `+0x94`, фабрика
`0x016E43D0`) и никогда не выбирается через `SwitchState`.

Политику переходов обеспечивает сам движок, но **на восьми местах вызова**, а
не внутри `SwitchState`: `if (state==3||state==4||state==5||state==6) return;`.
Сам `SwitchState` примет любое значение. Поэтому запись — не публичная
capability: прямой вызов обходит этот отказ и может, например, выдернуть
камеру из `PostMortemAnimationController`, пока анимацией смерти владеет он.

### SoundEventStub — штатный null-object для подавления звука

`DAVA::SoundSystem::CreateSoundEvent` (база) `0x00C4F040` = `call sub_C474F0;
retn 8`. Сам `sub_C474F0` — `0x00C474F0` / RVA `0x008474F0` — не принимает
аргументов и не использует `this`: `operator new(0x24)`, ctor `RefCounted`
(refcount 1), затем vtable `0x0360C7EC`, COL `0x038F5BB4`,
`.?AVSoundEventStub@DAVA@@`. Его 22-слотовая vtable совпадает по раскладке с
`WwiseHybridSoundEvent` (`0x0381AD2C`): `Retain`/`Release` те же
(`0x00912620` / `0x0091BAE0`), `IsActive` = `0x0067E300`, все сеттеры —
nullsub `0x0068AE60`.

Это важно, потому что **вернуть `nullptr` из детура `CreateSoundEvent` нельзя**:
из восьми реальных мест вызова шесть не проверяют результат
(`0x00786381`, `0x00787216`, `0x015F84A4`, `0x015F8577`, `0x01572348` пишут
его напрямую в поле). Подавление звука делается возвратом настоящего
`SoundEventStub`.

`DAVA::FastName` — один dword с интернированным `const char*` (ctor
`0x0090DE50` кладёт результат интернирования в `[this]`). Поэтому детур
сравнивает имена через `strcmp(*(const char**)arg1, ...)` — без аллокаций,
без блокировок, без конструирования `FastName`.

Путь создания события защищён мьютексом `WwiseHybridSoundSystem+0x40`
(`EnterCriticalSection` на `0x24F3A74`), то есть движок сам допускает
конкурентные вызовы — детур обязан быть написан как исполняемый на
произвольном потоке. Мьютекс берётся **внутри** `sub_24F39C0`, а не вокруг
всего `CreateSoundEvent`, поэтому детур может звать оригинал без самоблокировки.
Цепочка систем трёхуровневая: `SoundSystemProxy` → (`+0x20`)
`WwiseHybridSoundSystem` → (`+0x5C`) `WwiseSoundSystem`, и гибрид сам зовёт
`CreateSoundEvent` внутренней системы на `0x024F3BA2` — поэтому вызов
публичного пути из детура обязан быть защищён thread-local флагом реентранса.

### Entity / Scene layout — read-only enumeration

`Entity::RemoveNode` `0x00D10C90` / RVA `0x00910C90`, `retn 4` — точное зеркало
`Entity::AddNode` `0x00CD6C10`: линейный поиск, `memmove` для уплотнения,
`[this+0x0C] -= 4`, `SetScene(nullptr)`, обнуление родительских связей в
`TransformComponent`, затем `Release(child)`. Вектор детей держит ровно одну
сильную ссылку на каждого ребёнка.

`Scene` переиспользует тот же контейнер — доказано дважды: `Scene::Scene`
(`0x00CCF190`) зовёт настоящий ctor `Entity(const FastName&)` (`0x00CCE250`) на
`0x00CCF1C8` до подмены vtable, и слоты 5/6 обеих vtable указывают на те же
`0x00CD6C10` / `0x00D10C90`.

| off | размер | смысл |
|---|---:|---|
| `+0x00` | 4 | vtable (`0x036125E8` Entity / `0x03612764` Scene) |
| `+0x04` | 4 | refcount (`Retain 0x0091B9F0` = `lock inc [+4]`) |
| `+0x08` | 4 | children begin |
| `+0x0C` | 4 | children end |
| `+0x10` | 4 | children capacity end |
| `+0x14` | 4 | `Scene*` |
| `+0x18` | 4 | `Entity*` parent |
| `+0x1C` | 4 | `FastName name` — интернированный `const char*`, **не** refcounted |
| `+0x3C` | 4 | `TransformComponent*` |
| `+0x100` | 1 | (Scene) active byte |

`TransformComponent`: `+0x10` localTransform (40 байт), `+0x38` worldTransform
(40 байт), **`+0x60` worldMatrix (`Matrix4`, 64 байта)** — это и надо читать,
`+0xA0` указатель на родительский world `Transform`, `+0xA4` `Entity*` родителя.

Обход обязан идти **только в главном потоке**: `AddNode`/`RemoveNode` двигают
вектор через `memmove` вообще без блокировок, и `RemoveNode` делает `Release`
сразу после уплотнения — конкурентный читатель может взять указатель, который
освобождается следующей инструкцией. Ни лока, ни счётчика версий для
детектирования этого не существует, поэтому единственная защита —
дисциплина потока.
