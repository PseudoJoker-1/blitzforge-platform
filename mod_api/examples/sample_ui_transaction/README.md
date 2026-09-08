# sample.ui_transaction

RC1 sample for inspecting and transactionally changing an existing game-owned
`UIControl`. It reads the active control name, type, derived parent path and
geometry; moves it by one pixel; attaches a managed vertical-layout container;
subscribes to its click event; then restores and releases everything on disable
or unload.

Text inspection is deliberately reported as `NOT_EXPOSED_RC1`: the frozen UI
V3 snapshot has no text getter. This sample covers only transactional mutation
of a game-owned tree; runtime native Text/Image/Button controls are demonstrated
separately by `example.lua_ui_framework`.

## Expected result

- Log contains `name=... type=... path=... text=NOT_EXPOSED_RC1 geometry=...`.
- Export `WotbSampleUi_GetState()` has bits 0-3 set after enable.
- Clicking the managed container sets bit 6.
- Disable/unload sets rollback and cleanup bits 4-5.

## Cleanup verification

After disabling the mod, the stock screen position must equal its original
position, the managed child must be absent, and no UI event callback may fire.
The host test verifies create/attach/layout/event and exact inverse cleanup.

## Manual checklist

1. Enable the package in hangar and inspect the loader log.
2. Confirm no game text/image/button was replaced.
3. Disable it and confirm the stock screen returns to the original position.
4. Change screen, re-enable, and repeat once to detect stale handles.

Build and host test: `tests\build_rc1_samples_tests.cmd`.
