#pragma once

#include <cstdint>

/*
 * Every fixed RVA the binding pack depends on, in one place.
 *
 * Valid for exactly one client build (11.20.0.887, SHA-256 4813544d...).
 * The loader verifies that fingerprint before any of these is dereferenced;
 * on a mismatch the native backends stay off rather than jumping into
 * whatever now lives at these offsets.
 *
 * `const` at namespace scope has internal linkage, so each translation unit
 * gets its own copy - no ODR issue, no linker symbol.
 *
 * Generated once from the previously scattered declarations. Keep the
 * `const uint32_t kNameRva = 0x...u;` spelling: reanchor/anchors.py parses
 * this file with a regex that matches exactly that form.
 */

const uint32_t kAimTargetClearRva = 0x012900D0u;
const uint32_t kAimTargetSetRva = 0x0128F920u;
/*
 * The camouflage visibility mask dword of the native customization editor.
 *
 * CamouflagesManagerImpl::GetCamouflages enumerates every entry of
 * `camouflages.yaml` (the composed overlay document included) and passes each
 * candidate through CamouflagesAccountModelImpl's state filter with THIS dword
 * as the allowed-state bitmask (`push dword [VA 0x374F0E4]` at VA 0x1A059D0,
 * the only reference in the image; 11.19 had it at 0x36CFF98 / 0x19A7832). The shipped value is 3 - bits for lock
 * states 0 (unlocked) and 1 (locked-but-visible); a name the server never sent
 * answers state 2 and is dropped before a card is built. Widening bit 2 lets
 * locally-authored entries reach MakeCamouflageItem, which draws them from the
 * yaml fields this mod already composes. Data, not code: one dword, one xref,
 * restored by whoever widened it.
 */
const uint32_t kCamouflagesVisibilityMaskRva = 0x0334F0E4u;
/*
 * CamouflagesAccountModelImpl's per-name state query and the masked list
 * filter, both reached virtually from CamouflagesManagerImpl::GetCamouflages.
 * The probe detours both to log their traffic and to dump the filter's input
 * elements - the measurement that settles WHERE a locally authored name is
 * dropped: before the filter (never enumerated) or after it (card-building).
 */
const uint32_t kCamouflageLockedStateRva = 0x01605290u;
const uint32_t kCamouflagesStateFilterRva = 0x01606290u;
const uint32_t kAmmoChangedRva = 0x01226C90u;
const uint32_t kBwEntityCtorRva = 0x0235A470u;
const uint32_t kBwEntityDtorRva = 0x0235A640u;
/*
 * CameraController::SwitchState. __thiscall(int newState), retn 4.
 * The state field at CameraController+0x5C is written in exactly two places:
 * the constructor (0x015CE410 writes the initial 5; 0x01598D90 on 11.19) and the tail of this
 * function (mov [esi+5Ch], edi ; call StartNewState). A detour here therefore
 * observes every transition and, more importantly, yields a live, self-
 * validating this-pointer in ecx on the thread that mutates it - which is the
 * only way to reach a CameraController at all on this build, since the owner
 * class of the AvatarContext +0x2C slot that publishes it is UNKNOWN.
 */
const uint32_t kCameraControllerSwitchStateRva = 0x011E0750u;
const uint32_t kCameraModeChangedRva = 0x011A86E0u;
const uint32_t kClientInitializeRva = 0x0118FB90u;
const uint32_t kDavaFileCreateRva = 0x006C7A90u;
const uint32_t kDefaultEntityCtorRva = 0x0084CC90u;
const uint32_t kDefaultEntityVtableRva = 0x032848F8u;
const uint32_t kDefaultFastNameCtorRva = 0x005107A0u;
const uint32_t kDefaultYamlParseFileWrapperRva = 0x005B9030u;
const uint32_t kDefaultResourceArchiveCtorRva = 0x0059E810u;
const uint32_t kDefaultResourceArchiveDtorRva = 0x004C52D0u;
const uint32_t kDefaultPackArchiveVtableRva = 0x032607A0u;
const uint32_t kDefaultZipArchiveVtableRva = 0x03261B84u;
const uint32_t kDefaultNMaterialCtorRva = 0x008E3C70u;
const uint32_t kDefaultNMaterialVtableRva = 0x0328CA34u;
const uint32_t kDefaultNMaterialSetFxRva = 0x009283A0u;
const uint32_t kDefaultNMaterialSetQualityRva = 0x00929B80u;
const uint32_t kDefaultNMaterialHasPropertyRva = 0x00907640u;
const uint32_t kDefaultNMaterialAddPropertyRva = 0x008EE6F0u;
const uint32_t kDefaultNMaterialSetPropertyRva = 0x009299F0u;
const uint32_t kDefaultNMaterialRemovePropertyRva = 0x009221C0u;
const uint32_t kDefaultNMaterialHasFlagRva = 0x00907600u;
const uint32_t kDefaultNMaterialSetFlagRva = 0x00929440u;
const uint32_t kDefaultNMaterialRemoveFlagRva = 0x009220F0u;
const uint32_t kDefaultNMaterialHasTextureRva = 0x009076C0u;
const uint32_t kDefaultNMaterialSetTextureRva = 0x009295A0u;
const uint32_t kDefaultNMaterialRemoveTextureRva = 0x00922610u;
const uint32_t kDefaultTextureCreateFromFileRva = 0x007424F0u;
/*
 * DAVA::GetRenderObject(Entity const*). The helper reads the exact-build
 * cached RenderComponent at Entity+0x40 and returns RenderComponent+0x10.
 */
const uint32_t kDefaultEntityGetRenderObjectRva = 0x00ABC160u;
/*
 * RenderObject::GetRenderBatch(uint32_t). The exact 11.20 RenderBatchWithOptions
 * array begins at RenderObject+0x3C and uses 0x10-byte entries.
 */
const uint32_t kDefaultRenderObjectGetRenderBatchRva = 0x002B0BA0u;
const uint32_t kDefaultRenderBatchVtableRva = 0x0329C818u;
/* RenderBatch::SetMaterial. Retains the replacement NMaterial, swaps the
 * pointer at +0x44 and releases the previous material. */
const uint32_t kDefaultRenderBatchSetMaterialRva = 0x00A7A6F0u;
/*
 * RenderBatch::SetPolygonGroup. This releases the previous PolygonGroup,
 * retains the replacement and refreshes the batch AABB from the new source.
 */
const uint32_t kDefaultRenderBatchSetPolygonGroupRva = 0x00A7B040u;
const uint32_t kDefaultGetEngineContextRva = 0x006CE3A0u;
const uint32_t kDefaultHybridEventVtableRva = 0x0349A17Cu;
const uint32_t kDefaultOperatorDeleteRva = 0x02128F61u;
const uint32_t kDefaultOperatorNewRva = 0x02128CDEu;
const uint32_t kDefaultRefCountedReleaseRva = 0x0051D1B0u;
const uint32_t kDefaultRefCountedRetainRva = 0x0051D6C0u;
const uint32_t kDefaultSceneActivateRva = 0x008534A0u;
const uint32_t kDefaultSceneCtorRva = 0x0084CF50u;
const uint32_t kDefaultSceneDeactivateRva = 0x00867AB0u;
const uint32_t kDefaultSceneDrawRva = 0x0086B0B0u;
const uint32_t kDefaultSceneLoadEntityRva = 0x008799D0u;
const uint32_t kDefaultSceneLoadFromFileRva = 0x00879BC0u;
const uint32_t kDefaultSceneVtableRva = 0x03284AC4u;
const uint32_t kDefaultSoundGroupRva = 0x0416A4D4u;
const uint32_t kDefaultSoundSystemProxyVtableRva = 0x03275300u;
const uint32_t kDefaultSoundSystemSingletonRva = 0x0416CDE4u;
const uint32_t kDefaultSoundSystemVtableRva = 0x034994C0u;
const uint32_t kDefaultTransformSetLocalTransformRva = 0x00885660u;
const uint32_t kDefaultUiControlCtorRva = 0x00784BD0u;
/* DAVA::UIControl::SetVisibilityFlag (IDA wotb1120 VA 0x00BCEDB0, the setter the
 * UIControl reflection block registers for "visible" next to the getter
 * 0x00BAB7D0): __thiscall(bool), retn 4, prologue 55 8B EC 8A 55 08. Flips bit
 * 0x02 of the flags byte at +0x4C, then on show cascades the parent's view
 * state (+0xAC) through 0x00BAEFB0 (OnVisible - what StyledButton needs to
 * draw) and on hide runs 0x00BD0260 (OnInvisible); both paths end in
 * 0x00BC8E60(3) = SetLayoutDirty. A raw flag write skips all of that, which
 * is why a StyledButton shown that way stayed invisible (live 2026-09-08). */
const uint32_t kDefaultUiControlSetVisibilityFlagRva = 0x007CEDB0u;
const uint32_t kDefaultUiControlSystemGetScreenRva = 0x007A8C80u;
const uint32_t kDefaultUiControlVtableRva = 0x0327A878u;
/* DAVA::UITextComponent vftable (IDA wotb1120 VA 0x0369AAA8, RTTI col 0x03987F90);
 * text std::string at +0x44, object 0xE0. The first anchor said 0x0029AAA8 - a
 * dropped digit - and no hangar label matched until the live RTTI dump of
 * 2026-09-04 showed UIStaticText holding this class at control+0x140. */
const uint32_t kDefaultUiTextComponentVtableRva = 0x0329AAA8u;
/* DAVA::UIDynamicAtlasTextComponent vftable (RTTI col 0x039AB710 -> vtable
 * 0x036B6D58 in IDA wotb1120): ctor sub_10E7CD0 lays out std::string text at
 * +0x60, std::string font name at +0x78, FilePath at +0x90; object 0xE8.
 * Every hangar/HUD label of 11.20.0.887 carries this component, not the
 * plain UITextComponent. */
const uint32_t kDefaultUiDynamicAtlasTextComponentVtableRva = 0x032B6D58u;
const uint32_t kDefaultUiExtractControlRva = 0x007A38F0u;
const uint32_t kDefaultUiLoadPackageRva = 0x007B25E0u;
const uint32_t kDefaultUiPackageBuilderCtorRva = 0x008A7220u;
const uint32_t kDefaultUiPackageBuilderDtorRva = 0x008ADB40u;
const uint32_t kDefaultUiPackageLoaderCtorRva = 0x00787FF0u;
const uint32_t kDefaultUiPackageLoaderDtorRva = 0x0078C670u;
const uint32_t kDefaultUiPackageLoaderVtableRva = 0x0327B32Cu;
const uint32_t kDefaultUiPackageVtableRva = 0x0327B234u;
const uint32_t kDefaultWwiseEventVtableRva = 0x03499FD4u;
/*
 * DAVA::Private::EngineBackend::UpdateAndDrawWindows.
 * __thiscall(EngineBackend*, float frameDelta, bool skipUpdate), retn 8.
 *
 * This is the main-thread frame ingress, and the thread identity is PROVEN
 * rather than assumed: its only caller is 0x00ADE1AD inside 0x00ADDE80 (0x00ABF653 / 0x00ABF320 on 11.19), which
 * is the Win32 game loop (PeekMessageW / TranslateMessage / DispatchMessageW).
 * A thread that pumps a window message queue is that window's thread. It runs
 * once per frame unconditionally, including on UI-only screens where no 3D
 * scene exists - which Scene::Draw does not, so Scene::Draw is not a
 * substitute as a general main-thread pump.
 */
const uint32_t kEngineUpdateAndDrawWindowsRva = 0x006E06B0u;
const uint32_t kGameCameraCtorRva = 0x011D0670u;
const uint32_t kGameCameraDtorRva = 0x011DD0F0u;
const uint32_t kGameCameraSetFovRva = 0x01103060u;
const uint32_t kGameCameraVtableRva = 0x0330AAFCu;
const uint32_t kLeaveToHangarRva = 0x011AA910u;
const uint32_t kObservedPayloadGetterRva = 0x01BF1E40u;
const uint32_t kObservedStatusRva = 0x0122D820u;
const uint32_t kOnEnterWorldRva = 0x012A1480u;
const uint32_t kOnLeaveWorldRva = 0x012A2850u;
const uint32_t kReloadSetStateRva = 0x01201290u;
const uint32_t kResolveAvatarVehicleRva = 0x00309800u;
const uint32_t kSetHealthRva = 0x012A73C0u;
const uint32_t kShowShootingRva = 0x012AA790u;
/*
 * The engine's OWN null sound event.
 *
 * sub_AFBE50 (sub_C474F0 on 11.19) takes no arguments, does not touch `this`, allocates 0x24 bytes
 * with operator new, runs the RefCounted constructor (refcount 1) and installs
 * the SoundEventStub vtable; on allocation failure it returns 0. It is what
 * base DAVA::SoundSystem::CreateSoundEvent (on 11.19: 0x00C4F040 = call
 * sub_C474F0 ; retn 8) hands back, so it is the object the engine itself considers a
 * legitimate "no sound" result.
 *
 * Suppression MUST return this and never nullptr: of the eight real
 * CreateSoundEvent call sites at least six do not null-check (11.19 audit:
 * 0x00786381, 0x00787216, 0x015F84A4, 0x015F8577 and 0x01572348 store the
 * result straight into a field; the 11.20 sites follow the same
 * `mov ecx,[singleton] ; push offset group` pattern). Returning null is a crash, not a suppression.
 */
const uint32_t kSoundEventStubFactoryRva = 0x006FBE50u;
const uint32_t kSoundEventStubVtableRva = 0x03275258u;
/*
 * Expected occupants of vtable slot +0x0C (CreateSoundEvent) on the two
 * systems the live singleton can be. The detour replaces the SLOT, and refuses
 * to do so unless the slot still contains exactly these addresses.
 */
const uint32_t kSoundSystemHybridCreateEventRva = 0x021321D0u;
const uint32_t kSoundSystemProxyCreateEventRva = 0x0070EEC0u;
const uint32_t kTracerManagerCtorRva = 0x011D3470u;
const uint32_t kTracerManagerDtorRva = 0x011DD8E0u;
const uint32_t kTracerManagerVtableRva = 0x0330AA8Cu;
const uint32_t kTracerManagerShowTracerRva = 0x012030C0u;
/*
 * 25 const char* entries mapping a byte shell-type code 0..24 onto eight
 * distinct style names. The bound is the image's own: the resolver at
 * 0x015E3A90 (0x0155A040 on 11.19) gates on `cmp al, 19h / jnb error` before
 * `mov edx, off_4038F80[eax*4]`. Immutable .rdata, so it is readable from any
 * thread and needs no hook.
 */
const uint32_t kTracerShellStyleTableRva = 0x03C38F80u;
const uint32_t kUiControlSystemInputRva = 0x007CF9A0u;
const uint32_t kUpdateVehicleHealthRva = 0x012AC1E0u;
const uint32_t kVehicleHitDamageRva = 0x01326660u;
/*
 * PlayerController pose update (0x015B8C10 on 11.20.0.887), called every
 * frame from BattleController::Update (0x015B8330) just before the one-byte
 * Avatar::TurretAndCameraPositionsChanged tick is published:
 * `mov ecx,[edi-8]; call [eax+174h]; mov ecx,eax; push delta; call`, so its
 * `this` is the controller returned by that virtual slot, not the updater's.
 * It rewrites the pose block at this+0x5C under the spinlock at block+224:
 * +60 Vec3 (controller state +944), +72 Vec3, +84 float, +88 Vec3, +104 int,
 * +108 4x4 world matrix of the controlled vehicle's appearance (+1344 of the
 * object at appearance+8), +172/+176 camera angles, +192 byte; +0..+59 is the
 * RayTest result under a second spinlock at +228. The ReplayRecorder reads
 * the same block. Prologue 55 8B EC 6A FF.
 */
const uint32_t kPlayerControllerPoseUpdateRva = 0x011B8C10u;
/*
 * ClientArena (C:/ba/tc/work/t/client/Classes/Battle/ClientArena/ClientArena.cpp)
 * receives the server's UpdateArena protobuf (oneof case at +20, payload
 * pointer at +12) and dispatches through the handler table set up in its
 * constructor 0x01614930. Three of them carry the roster and the per-player
 * counters:
 *  - 0x01635E10 __thiscall(ClientArena*, VehicleInfo*) -> ArenaVehicleInfo*:
 *    creates or updates the 184-byte ArenaVehicleInfo in the map at
 *    ClientArena+272 (vehicle_list and vehicle_added both go through it).
 *    ArenaVehicleInfo (live dumps 2026-09-06): +24 vehicle id, +28 player
 *    model (name std::string at +8), +36 vehicle descriptor (type record at
 *    +52: tag +8, user string key +32), +40 team, +48 int64 account id, +56
 *    std::string clan tag, +80 int64 clan id, +96 is_alive, +97 avatar
 *    ready, +124 kills (-100 until the first statistics packet).
 *  - 0x01638420 __thiscall(ClientArena*, VehicleStatisticsInfo*): writes
 *    message +16 (kills) into the ArenaVehicleInfo found by message +12
 *    (vehicle id); reached from both UpdateArena.statistics (repeated) and
 *    UpdateArena.vehicle_statistics.
 *  - 0x0122DF30 __thiscall(ClientArena*, UpdateArena*): vehicle_killed (case
 *    6): payload +12 victim, +16 killer, +20 assist, +24 reason enum, +28
 *    ammo bay exploded; marks the victim dead and notifies listeners.
 * All three start with 55 8B EC 6A FF.
 */
const uint32_t kArenaAddVehicleInfoRva = 0x01235E10u;
const uint32_t kArenaApplyVehicleStatisticsRva = 0x01238420u;
const uint32_t kArenaOnVehicleKilledRva = 0x0122DF30u;
/*
 * ClientArena player_name handler (UpdateArena case 13, 0x0162B9A0,
 * __thiscall(ClientArena*, UpdateArena*)): payload +12 std::string* new
 * name, +16 vehicle id; renames the player model of that vehicle. Prologue
 * 55 8B EC 6A FF.
 */
const uint32_t kArenaOnPlayerNameRva = 0x0122B9A0u;
/*
 * DAVA::Singleton<LocalizationSystem>::instance (dword_461653C): the
 * application's startup at 0x012CE157 loads it, then calls SetDirectory
 * (0x009D3E60, "~res:/Strings/"), SetCurrentLocale (0x009D2E90) and the
 * file loader 0x009BEE90 which appends a StringFile to the std::list at
 * LocalizationSystem+80 (size at +84). Each StringFile keeps its strings in
 * a std::map<std::string, std::string> whose head node pointer sits at
 * StringFile+52: 64-byte nodes with left/parent/right at +0/+4/+8, isnil
 * at +13, the key std::string at +16 and the value at +40. Read-only after
 * the locale is loaded, so the loader walks it without calling the game.
 */
const uint32_t kLocalizationInstanceRva = 0x0421653Cu;
/*
 * GES::GameEventSystem core (docs/superpowers/specs/2026-09-03-ges-event-bus-design.md §2).
 * GetListeners(type_index) 0x006DBC30 thiscall ret 4; GetOrCreateList
 * 0x006DBF80 thiscall ret 8; the ListenerList factory 0x0112F0B0 (cdecl,
 * T-independent); ListenerList::Add 0x011260E0 thiscall ret 0xC;
 * ListenerList::Erase 0x007B0380 thiscall ret 8; SubscribeImpl 0x006DD830
 * thiscall ret 0x30, hooked only to capture the bus pointer.
 */
const uint32_t kGesGetListenersRva = 0x002DBC30u;
const uint32_t kGesGetOrCreateListRva = 0x002DBF80u;
const uint32_t kGesListenerListFactoryRva = 0x00D2F0B0u;
const uint32_t kGesListenerListAddRva = 0x00D260E0u;
const uint32_t kGesListenerListEraseRva = 0x003B0380u;
const uint32_t kGesSubscribeImplRva = 0x002DD830u;
/*
 * Login cluster switch (docs/superpowers/specs/2026-09-07-cluster-picker-design.md,
 * IDA wotb1120 2026-09-08). LoginManager::OnHostChosen 0x1D08FF0 is
 * __thiscall(LoginManager*, int, int, ClusterHost*), prologue 55 8B EC 6A FF,
 * called on every login with the host the client picked; its `this` is the
 * only global-free path to `services` (LoginManager+32). ChangeCluster
 * 0x1CF1C40 __thiscall(int clusterId), same prologue: writes clusterToLogin
 * (+488), shows "Connecting...", ConnectionManager->Disconnect(5) ->
 * HandleDisconnect(5) -> HandleDisconnectOnChangingCluster ->
 * Region::GetClusterById -> re-login with the saved credentials. TryNextHost
 * honours the byte at +796 ("manually selected cluster, won't retry").
 * Region = vector<ClusterHost> (stride 208) reached as
 * *(*(services->vtbl[79]() + 40) + 1592); ClusterHost {+24/+28 vector of
 * login addresses (stride 208, float ping at +48; "alive" = some address
 * answered, the test Region::HasAliveCluster 0x14CC140 makes), +184 int state
 * (1 = the client marked the host dead, ClusterHost::IsDead 0x1866960),
 * +188 -> descriptor {+0 std::string name "EU_C4", +24 std::string url
 * "login4.wotblitz.eu:20016", +120 int id (what GetClusterById compares),
 * +188 byte check-alive, +189 byte forced}}; ConnectionManager =
 * services->vtbl[25](), current ClusterHost* at +12; LoginManager =
 * services->vtbl[26](). Live 2026-09-08: the region vector holds 3 hosts on
 * this account (yaml lists 4), url/id read back as expected.
 */
const uint32_t kLoginManagerOnHostChosenRva = 0x01908FF0u;
const uint32_t kLoginManagerChangeClusterRva = 0x018F1C40u;
/*
 * ConnectionManager::Disconnect 0x141F140, __thiscall(int reason, float delay,
 * int arg), retn 0Ch, prologue 55 8B EC 6A FF; `this` = services->vtbl[25]().
 * Reason 12 is the one LoginManager::HandleDisconnect answers with
 * TryNextHost -> Region::DetermineBestCluster (auto pick), which is how AUTO
 * is implemented: ChangeCluster(-1) from the hangar ends in the client's
 * "disconnected from server" dialog (live 2026-09-08 02:20), because
 * HandleDisconnectOnChangingCluster cannot find cluster -1.
 */
const uint32_t kConnectionManagerDisconnectRva = 0x0101F140u;
const uint32_t kLoginManagerServicesOffset = 0x20u;
const uint32_t kLoginManagerClusterToLoginOffset = 0x1E8u;
const uint32_t kLoginManagerManualClusterOffset = 0x31Cu;
const uint32_t kServicesAppCtxSlotOffset = 0x13Cu;
const uint32_t kServicesConnectionManagerSlotOffset = 0x64u;
const uint32_t kServicesLoginManagerSlotOffset = 0x68u;
const uint32_t kAppCtxOwnerOffset = 0x28u;
const uint32_t kAppCtxRegionOffset = 0x638u;
const uint32_t kConnectionManagerCurrentHostOffset = 0xCu;
const uint32_t kClusterHostStrideOffset = 0xD0u;
const uint32_t kClusterHostDescriptorOffset = 0xBCu;
const uint32_t kClusterHostAddressesBeginOffset = 0x18u;
const uint32_t kClusterHostAddressesEndOffset = 0x1Cu;
const uint32_t kClusterHostStateOffset = 0xB8u;
const uint32_t kClusterAddressStrideOffset = 0xD0u;
const uint32_t kClusterAddressPingOffset = 0x30u;
const uint32_t kClusterDescriptorNameOffset = 0x0u;
const uint32_t kClusterDescriptorUrlOffset = 0x18u;
const uint32_t kClusterDescriptorIdOffset = 0x78u;
