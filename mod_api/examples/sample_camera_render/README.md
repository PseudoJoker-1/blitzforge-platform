# sample.camera_render

RC1 portable render/camera sample. It registers a managed `AFTER_UI` callback,
draws one telemetry line, reads backend/viewport/frame/delta time, reports the
camera mode (including `UNKNOWN`), installs a low-priority `AFTER_GAME` modifier
that only repairs an invalid FOV, and observes managed render lifecycle events.
It never requests `wotbmod.render.native` or retains COM pointers.

## Expected result

- One overlay line shows backend, viewport, frame, delta time and camera mode.
- Resize/backend or device lifecycle events increment the lifecycle counter;
  the same managed callback continues drawing after restoration.
- Invalid FOV input is repaired to 60 degrees; valid game camera output is not
  changed.

## Cleanup verification

Disable/unload must unsubscribe lifecycle events, unregister the render
callback, remove the camera modifier and release the active camera handle. The
host test exercises resize/backend change and verifies every cleanup call.

## Manual checklist

1. Enable in hangar and enter battle/training; observe `UNKNOWN`, `ARCADE` or
   `SNIPER` without requiring a particular unsupported mode.
2. Resize the window and switch fullscreen/windowed mode.
3. Trigger a safe device recreation if available and confirm one overlay line,
   not duplicate callbacks.
4. Disable the mod and confirm the overlay disappears immediately.

Build and host test: `tests\build_rc1_samples_tests.cmd`.
