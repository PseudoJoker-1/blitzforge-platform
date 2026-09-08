# `wotb.gameplay_camera`

Raw-таблица интерфейса `WotbModV3GameplayCameraApiV1` (`include/wotbmod/gameplay_camera_v1.h`, версия `WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION`). Функции ниже вызываются как `wotb.gameplay_camera.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.view`](../facades/wotb-view.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.camera`, `gameplay.tweak.freecam`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.gameplay_camera.TRANSITION_SMOOTH` | `WOTBMOD_V3_CAMERA_TRANSITION_SMOOTH` | enum WotbModV3CameraTransitionMode |
| `wotb.gameplay_camera.TRANSITION_INSTANT` | `WOTBMOD_V3_CAMERA_TRANSITION_INSTANT` | enum WotbModV3CameraTransitionMode |
| `wotb.gameplay_camera.TRANSITION_CUSTOM` | `WOTBMOD_V3_CAMERA_TRANSITION_CUSTOM` | enum WotbModV3CameraTransitionMode |
| `wotb.gameplay_camera.EASING_LINEAR` | `WOTBMOD_V3_EASING_LINEAR` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_IN_QUAD` | `WOTBMOD_V3_EASING_IN_QUAD` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_OUT_QUAD` | `WOTBMOD_V3_EASING_OUT_QUAD` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_IN_OUT_QUAD` | `WOTBMOD_V3_EASING_IN_OUT_QUAD` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_IN_CUBIC` | `WOTBMOD_V3_EASING_IN_CUBIC` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_OUT_CUBIC` | `WOTBMOD_V3_EASING_OUT_CUBIC` | enum WotbModV3Easing |
| `wotb.gameplay_camera.EASING_IN_OUT_CUBIC` | `WOTBMOD_V3_EASING_IN_OUT_CUBIC` | enum WotbModV3Easing |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `set_fov` | degrees: number | true |
| `get_fov` | — | degrees: number |
| `set_fov_hangar` | degrees: number | true |
| `set_fov_battle` | degrees: number | true |
| `set_fov_sniper` | degrees: number | true |
| `reset_fov` | — | true |
| `set_zoom_steps` | steps: number, count: integer | true |
| `get_zoom_steps` | — | steps: number |
| `set_zoom_multiplier` | multiplier: number | true |
| `set_max_zoom` | multiplier: number | true |
| `set_transition_mode` | mode: integer | true |
| `set_transition_duration` | milliseconds: number | true |
| `set_transition_easing` | easing: integer | true |
| `set_shake_enabled` | enabled: integer | true |
| `set_shake_intensity` | multiplier: number | true |
| `set_postprocessing_enabled` | enabled: integer | true |
| `set_bloom_enabled` | enabled: integer | true |
| `set_motion_blur_enabled` | enabled: integer | true |
| `set_vignette_enabled` | enabled: integer | true |
| `set_color_grading` | lut_uri: string | true |
| `set_dof_enabled` | enabled: integer | true |
| `set_dof_params` | focus: number, aperture: number, max_blur: number | true |
| `free_enable` | enabled: integer | true |
| `free_set_speed` | units_per_second: number | true |
| `free_set_position` | position: table:Vec3 | true |
| `free_set_rotation` | pitch_yaw_roll: table:Vec3 | true |
| `free_get_position` | — | position: table:Vec3 |
| `free_get_rotation` | — | pitch_yaw_roll: table:Vec3 |
| `get_config` | — | config: table:GameplayCameraConfig |
| `apply_config` | config: table:GameplayCameraConfig | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.camera", "gameplay.tweak.freecam"]
local set_fov_ok, err = wotb.gameplay_camera.set_fov(0.0)
if not set_fov_ok then wotb.log.warn("gameplay_camera.set_fov: %s", err) end
local get_fov, err = wotb.gameplay_camera.get_fov()  -- degrees: number
if get_fov == nil then wotb.log.warn("gameplay_camera.get_fov: %s", err) end
local set_fov_hangar_ok, err = wotb.gameplay_camera.set_fov_hangar(0.0)
if not set_fov_hangar_ok then wotb.log.warn("gameplay_camera.set_fov_hangar: %s", err) end
```

