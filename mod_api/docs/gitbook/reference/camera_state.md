# `wotb.camera_state`

Raw-таблица интерфейса `WotbModV3CameraApiV2` (`include/wotbmod/camera_v2.h`, версия `WOTBMOD_V3_CAMERA_VERSION_2`). Функции ниже вызываются как `wotb.camera_state.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.view`](../facades/wotb-view.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `camera.battle.read`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.camera_state.ANIMATION_ORDINARY_A` | `WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_A` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_ORDINARY_B` | `WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_B` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_LOOK_OUT` | `WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_POST_MORTEM` | `WOTBMOD_V3_CAMERA_ANIMATION_POST_MORTEM` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_LOOK_ON_TARGET` | `WOTBMOD_V3_CAMERA_ANIMATION_LOOK_ON_TARGET` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_NONE` | `WOTBMOD_V3_CAMERA_ANIMATION_NONE` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_OBSERVER` | `WOTBMOD_V3_CAMERA_ANIMATION_OBSERVER` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.ANIMATION_UNKNOWN` | `WOTBMOD_V3_CAMERA_ANIMATION_UNKNOWN` | enum WotbModV3CameraAnimationState |
| `wotb.camera_state.VIEW_SNIPER` | `WOTBMOD_V3_CAMERA_VIEW_SNIPER` | enum WotbModV3CameraViewMode |
| `wotb.camera_state.VIEW_ARCADE` | `WOTBMOD_V3_CAMERA_VIEW_ARCADE` | enum WotbModV3CameraViewMode |
| `wotb.camera_state.VIEW_UNKNOWN` | `WOTBMOD_V3_CAMERA_VIEW_UNKNOWN` | enum WotbModV3CameraViewMode |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_animation_state` | — | state: integer |
| `get_view_mode` | — | mode: integer |
| `get_observed_state` | — | state: table:CameraObservedState |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["camera.battle.read"]
local get_animation_state, err = wotb.camera_state.get_animation_state()  -- state: integer
if get_animation_state == nil then wotb.log.warn("camera_state.get_animation_state: %s", err) end
local get_view_mode, err = wotb.camera_state.get_view_mode()  -- mode: integer
if get_view_mode == nil then wotb.log.warn("camera_state.get_view_mode: %s", err) end
local get_observed_state, err = wotb.camera_state.get_observed_state()  -- state: table:CameraObservedState
if get_observed_state == nil then wotb.log.warn("camera_state.get_observed_state: %s", err) end
```

