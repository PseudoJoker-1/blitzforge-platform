"""Guard the two invariants that keep native hooking fail-closed.

Both were real defects once, and both are the kind that come back silently
during a refactor because nothing crashes on the developer's own machine -
where the fingerprint always matches and shutdown never races.

1. Ordering. Every fixed-RVA backend must be installed *after* the client
   fingerprint is verified. `RC1_BLOCKERS.md` lists "native backend/hook
   enabled on a mismatched fingerprint" as a release blocker, and
   `API_V3_RC1_FREEZE.md` requires binding-pack validation before hook
   installation.

2. Trampoline lifetime. `WotbModV3NativeBindings_Shutdown` must clear every
   g_original_* pointer, otherwise a detour can call a trampoline that
   MH_RemoveHook already returned to MinHook's pool.
"""
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
LOADER = TOOLS / "mod_api" / "loader" / "wotb_mod_loader.cpp"
BINDINGS = TOOLS / "mod_api" / "loader" / "v3_native_bindings.cpp"

loader = LOADER.read_text(encoding="utf-8")
bindings = BINDINGS.read_text(encoding="utf-8")


def position(haystack: str, needle: str, label: str) -> int:
    index = haystack.find(needle)
    assert index >= 0, f"{label}: '{needle}' not found"
    return index


# -- 1. the gate runs before anything that resolves a fixed RVA -------------

gate = position(
    loader,
    "WotbModV3NativeBindings_VerifyClientFingerprint(",
    "loader",
)

GATED_CALLS = (
    "WotbModDavaSound_Create(",
    "WotbModDavaResources_Create(",
    "InstallSceneTrackingHooks(davaResourcesHandle)",
    "WotbModV3NativeBindings_Create(",
)
for call in GATED_CALLS:
    where = position(loader, call, "loader")
    assert where > gate, (
        f"{call} is invoked at offset {where}, before the fingerprint gate at "
        f"{gate}. Every fixed-RVA backend must be gated on the verified "
        "client fingerprint."
    )

# The two hook installers that carry no prologue guard of their own must
# additionally refuse to run on their own, not merely be called from a gated
# site.
for installer in ("InstallSceneTrackingHooks", "InstallDavaFileResolverHook"):
    start = position(loader, f"static bool {installer}(", "loader")
    body = loader[start : start + 1400]
    assert "g_nativeFingerprintVerified" in body, (
        f"{installer} does not check g_nativeFingerprintVerified; a future "
        "caller could install its detours on an unverified build."
    )

# -- 2. shutdown clears every trampoline pointer ----------------------------

ORIGINALS = (
    "g_original_camera_ctor",
    "g_original_camera_dtor",
    "g_original_client_initialize",
    "g_original_entity_ctor",
    "g_original_entity_dtor",
    "g_original_tracer_ctor",
    "g_original_camera_mode_changed",
)

shutdown_start = position(
    bindings, "WotbModV3NativeBindings_Shutdown(void) {", "bindings"
)
shutdown = bindings[shutdown_start : bindings.find("\n}", shutdown_start)]

for name in ORIGINALS:
    assert f"{name} = nullptr;" in shutdown, (
        f"{name} is not cleared in WotbModV3NativeBindings_Shutdown; after "
        "MH_RemoveHook it would point at a freed trampoline."
    )

# Order matters as much as presence: disable, drain, clear, only then remove.
disable_at = shutdown.find("MH_DisableHook")
drain_at = shutdown.find("g_detour_active")
clear_at = shutdown.find("g_original_camera_ctor = nullptr;")
remove_at = shutdown.find("MH_RemoveHook")
assert -1 < disable_at < drain_at < clear_at < remove_at, (
    "shutdown teardown is out of order; required sequence is "
    "MH_DisableHook -> drain g_detour_active -> clear g_original_* -> "
    f"MH_RemoveHook (got {disable_at}, {drain_at}, {clear_at}, {remove_at})"
)

# Every detour that dereferences a trampoline must join the in-flight count,
# or the drain above proves nothing.
for name in ORIGINALS:
    detour_uses = bindings.count(f"{name}(")
    assert detour_uses, f"{name} is never called - dead binding?"
guard_count = bindings.count("DetourGuard guard;")
assert guard_count >= len(ORIGINALS), (
    f"only {guard_count} detours take a DetourGuard, expected at least "
    f"{len(ORIGINALS)}; an unguarded detour is invisible to the shutdown drain."
)

print(
    f"native gate ordering: {len(GATED_CALLS)} fixed-RVA backends gated, "
    f"{len(ORIGINALS)} trampolines cleared, {guard_count} detours guarded"
)
