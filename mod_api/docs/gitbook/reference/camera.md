# `wotb.camera`

Raw-таблица интерфейса `WotbModV3CameraApiV1` (`include/wotbmod/camera_v1.h`, версия `WOTBMOD_V3_CAMERA_VERSION`). Функции ниже вызываются как `wotb.camera.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.view`](../facades/wotb-view.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `camera.battle.read`, `camera.hangar`, `camera.replay`, `gameplay.tweak.camera`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.camera.MODE_UNKNOWN` | `WOTBMOD_V3_CAMERA_MODE_UNKNOWN` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_HANGAR` | `WOTBMOD_V3_CAMERA_MODE_HANGAR` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_ARCADE` | `WOTBMOD_V3_CAMERA_MODE_ARCADE` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_SNIPER` | `WOTBMOD_V3_CAMERA_MODE_SNIPER` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_POSTMORTEM` | `WOTBMOD_V3_CAMERA_MODE_POSTMORTEM` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_REPLAY` | `WOTBMOD_V3_CAMERA_MODE_REPLAY` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_FREE` | `WOTBMOD_V3_CAMERA_MODE_FREE` | enum WotbModV3CameraMode |
| `wotb.camera.MODE_CINEMATIC` | `WOTBMOD_V3_CAMERA_MODE_CINEMATIC` | enum WotbModV3CameraMode |
| `wotb.camera.MODIFIER_BEFORE_GAME` | `WOTBMOD_V3_CAMERA_MODIFIER_BEFORE_GAME` | enum WotbModV3CameraModifierPhase |
| `wotb.camera.MODIFIER_AFTER_GAME` | `WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME` | enum WotbModV3CameraModifierPhase |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_active` | — | camera: handle |
| `get_mode` | camera: handle | mode: integer |
| `get_transform` | camera: handle | transform: table:Transform |
| `set_transform` | camera: handle, transform: table:Transform | true |
| `get_fov` | camera: handle | degrees: number |
| `set_fov` | camera: handle, degrees: number | true |
| `get_near_plane` | camera: handle | near_plane: number |
| `get_far_plane` | camera: handle | far_plane: number |
| `world_to_screen` | camera: handle, world: table:Vec3 | screen: table:Vec3 |
| `screen_to_world` | camera: handle, screen: table:Vec3 | world: table:Vec3 |
| `add_modifier` | phase: integer, priority: integer, callback: function | token: handle |
| `remove_modifier` | token: handle | true |
| `transition_to` | camera: handle, transition: table:CameraTransition | true |
| `add_shake` | camera: handle, shake: table:CameraShake | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["camera.battle.read", "camera.hangar", "camera.replay", "gameplay.tweak.camera"]
local get_active, err = wotb.camera.get_active()  -- camera: handle
if get_active == nil then wotb.log.warn("camera.get_active: %s", err) end
local get_mode, err = wotb.camera.get_mode(handle)  -- mode: integer
if get_mode == nil then wotb.log.warn("camera.get_mode: %s", err) end
local get_transform, err = wotb.camera.get_transform(handle)  -- transform: table:Transform
if get_transform == nil then wotb.log.warn("camera.get_transform: %s", err) end
```

