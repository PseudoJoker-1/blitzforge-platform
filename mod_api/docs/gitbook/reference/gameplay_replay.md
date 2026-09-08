# `wotb.gameplay_replay`

Raw-таблица интерфейса `WotbModV3GameplayReplayApiV1` (`include/wotbmod/gameplay_replay_v1.h`, версия `WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION`). Функции ниже вызываются как `wotb.gameplay_replay.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.replay`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.gameplay_replay.CAMERA_FREE` | `WOTBMOD_V3_REPLAY_CAMERA_FREE` | enum WotbModV3ReplayCamera |
| `wotb.gameplay_replay.CAMERA_FOLLOW_VEHICLE` | `WOTBMOD_V3_REPLAY_CAMERA_FOLLOW_VEHICLE` | enum WotbModV3ReplayCamera |
| `wotb.gameplay_replay.CAMERA_TOP_DOWN` | `WOTBMOD_V3_REPLAY_CAMERA_TOP_DOWN` | enum WotbModV3ReplayCamera |
| `wotb.gameplay_replay.CAMERA_CINEMATIC` | `WOTBMOD_V3_REPLAY_CAMERA_CINEMATIC` | enum WotbModV3ReplayCamera |
| `wotb.gameplay_replay.CAMERA_FIRST_PERSON` | `WOTBMOD_V3_REPLAY_CAMERA_FIRST_PERSON` | enum WotbModV3ReplayCamera |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `set_speed` | multiplier: number | true |
| `seek` | timestamp_seconds: number | true |
| `get_duration` | — | seconds: number |
| `get_position` | — | seconds: number |
| `add_marker` | timestamp_seconds: number, label: string | marker_id: integer |
| `remove_marker` | marker_id: integer | true |
| `set_camera_mode` | mode: integer | true |
| `set_follow_vehicle` | public_entity_id: integer | true |
| `export_clip` | start_seconds: number, end_seconds: number, output_uri: string | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.replay"]
local set_speed_ok, err = wotb.gameplay_replay.set_speed(0.0)
if not set_speed_ok then wotb.log.warn("gameplay_replay.set_speed: %s", err) end
local seek_ok, err = wotb.gameplay_replay.seek(0.0)
if not seek_ok then wotb.log.warn("gameplay_replay.seek: %s", err) end
local get_duration, err = wotb.gameplay_replay.get_duration()  -- seconds: number
if get_duration == nil then wotb.log.warn("gameplay_replay.get_duration: %s", err) end
```

