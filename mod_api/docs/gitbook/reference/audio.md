# `wotb.audio`

Raw-таблица интерфейса `WotbModV3AudioApiV2` (`include/wotbmod/audio_v2.h`, версия `WOTBMOD_V3_AUDIO_VERSION`). Функции ниже вызываются как `wotb.audio.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.sound`](../facades/wotb-sound.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `audio.custom`, `audio.events`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.audio.STATE_CREATED` | `WOTBMOD_V3_AUDIO_STATE_CREATED` | enum WotbModV3AudioState |
| `wotb.audio.STATE_PRELOADED` | `WOTBMOD_V3_AUDIO_STATE_PRELOADED` | enum WotbModV3AudioState |
| `wotb.audio.STATE_PLAYING` | `WOTBMOD_V3_AUDIO_STATE_PLAYING` | enum WotbModV3AudioState |
| `wotb.audio.STATE_PAUSED` | `WOTBMOD_V3_AUDIO_STATE_PAUSED` | enum WotbModV3AudioState |
| `wotb.audio.STATE_STOPPED` | `WOTBMOD_V3_AUDIO_STATE_STOPPED` | enum WotbModV3AudioState |
| `wotb.audio.STATE_ERROR` | `WOTBMOD_V3_AUDIO_STATE_ERROR` | enum WotbModV3AudioState |
| `wotb.audio.CREATE_NONE` | `WOTBMOD_V3_AUDIO_CREATE_NONE` | enum WotbModV3AudioCreateFlag |
| `wotb.audio.CREATE_SPATIAL` | `WOTBMOD_V3_AUDIO_CREATE_SPATIAL` | enum WotbModV3AudioCreateFlag |
| `wotb.audio.CREATE_LOOP` | `WOTBMOD_V3_AUDIO_CREATE_LOOP` | enum WotbModV3AudioCreateFlag |
| `wotb.audio.CREATE_STREAM` | `WOTBMOD_V3_AUDIO_CREATE_STREAM` | enum WotbModV3AudioCreateFlag |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `create` | descriptor: table:AudioDescriptor | audio: handle |
| `create_stream` | descriptor: table:AudioDescriptor | audio: handle |
| `preload` | audio: handle | true |
| `play` | audio: handle | true |
| `pause` | audio: handle | true |
| `resume` | audio: handle | true |
| `stop` | audio: handle, fade_seconds: number | true |
| `destroy` | audio: handle | true |
| `is_playing` | audio: handle | playing: integer |
| `get_state` | audio: handle | state: integer |
| `set_volume` | audio: handle, volume: number | true |
| `set_pitch` | audio: handle, pitch: number | true |
| `set_pan` | audio: handle, pan: number | true |
| `set_bus` | audio: handle, bus: string | true |
| `set_position_3d` | audio: handle, position: table:Vec3 | true |
| `set_min_distance` | audio: handle, distance: number | true |
| `set_max_distance` | audio: handle, distance: number | true |
| `fade_to` | audio: handle, target_volume: number, duration_seconds: number | true |
| `seek` | audio: handle, seconds: number | true |
| `get_position` | audio: handle | seconds: number |
| `get_duration` | audio: handle | seconds: number |
| `set_loop` | audio: handle, loop: integer | true |
| `on_started` | audio: handle, callback: function | token: handle |
| `on_finished` | audio: handle, callback: function | token: handle |
| `on_error` | audio: handle, callback: function | token: handle |
| `unsubscribe` | token: handle | true |
| `sound_override_register` | event_name: string, replacement_uri: string, priority: integer | token: handle |
| `sound_override_unregister` | token: handle | true |
| `sound_override_set_priority` | token: handle, priority: integer | true |
| `sound_override_get_conflicts` | event_name: string, visitor: function | true |
| `sound_event_create` | event_name: string | event: handle |
| `sound_event_trigger` | event: handle | true |
| `sound_event_stop` | event: handle, force: integer | true |
| `sound_event_set_paused` | event: handle, paused: integer | true |
| `sound_event_set_volume` | event: handle, volume: number | true |
| `sound_event_set_position` | event: handle, position: table:Vec3 | true |
| `sound_event_set_parameter` | event: handle, name: string, value: number | true |
| `sound_event_get_parameter` | event: handle, name: string | value: number |
| `sound_event_has_parameter` | event: handle, name: string | has_parameter: integer |
| `sound_event_get_name` | event: handle | string |
| `sound_event_get_bus` | event: handle | string |
| `sound_event_set_speed` | event: handle, speed: number | true |
| `sound_event_set_direction` | event: handle, direction: table:Vec3 | true |
| `sound_event_set_velocity` | event: handle, velocity: table:Vec3 | true |
| `sound_event_set_loop_count` | event: handle, loop_count: integer | true |
| `sound_event_set_priority` | event: handle, priority: integer | true |
| `sound_event_destroy` | event: handle | true |
| `set_listener_transform` | transform: table:Transform | true |
| `set_bus_volume` | bus: string, volume: number | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["audio.custom", "audio.events"]
local create, err = wotb.audio.create(descriptor)  -- audio: handle
if create == nil then wotb.log.warn("audio.create: %s", err) end
local create_stream, err = wotb.audio.create_stream(descriptor)  -- audio: handle
if create_stream == nil then wotb.log.warn("audio.create_stream: %s", err) end
local preload_ok, err = wotb.audio.preload(handle)
if not preload_ok then wotb.log.warn("audio.preload: %s", err) end
```

