# sample.vehicle_cosmetic

RC1 sample for a transactional, exact-path vehicle cosmetic pack. It loads its
JSON descriptor, registers explicit mesh/material/texture entries with LOD
metadata, applies them to the selected local `R110_Object_260`, reports mount
state and `requires_model_reload`, reacts to local-vehicle changes, and rolls
back every overlay before releasing the pack.

The reproducible staging script copies valid T-34-85 DVPL mesh/geometry and
textures from the installed client into the local sample package. Those files
are not stored in the SDK source tree. The standalone material YAML entry only
demonstrates the frozen exact-path transaction; live DAVA NMaterial mutation is
experimental and is not claimed by RC1.

## Expected result

- Descriptor load and pack registration succeed.
- `WotbSampleVehicle_GetState()` reports registration/application/state.
- `WotbSampleVehicle_RequiresReload()` is `1` after mounting mesh/texture paths.
- Selecting another tank rolls back the previous mount and reapplies only when
  the current vehicle matches the pack policy.
- A cached hangar model may require leaving/re-entering the tank preview.

## Cleanup verification

Disable/unload must unsubscribe the vehicle event, rollback all exact-path
mounts, release the pack, vehicle handle and descriptor. No replacement path
may remain mounted. The host test verifies this inverse sequence.

## Manual checklist

1. Use the package produced by `tools\build_rc1_sample_packages.ps1` for the
   exact client installation from which it was built.
2. Select Object 260, enable the mod, then leave and re-enter its preview if the
   cached model does not reload.
3. Switch to another tank and back; inspect state/reapply counters.
4. Disable the mod, reload the preview, and confirm the stock model is restored.

Build and host test: `tests\build_rc1_samples_tests.cmd`.
