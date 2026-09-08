# `wotb.audio_intercept`

Raw-таблица интерфейса `WotbModV3AudioApiV3` (`include/wotbmod/audio_v3.h`, версия `WOTBMOD_V3_AUDIO_VERSION_3`). Функции ниже вызываются как `wotb.audio_intercept.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `audio.events`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.audio_intercept.PASS_THROUGH` | `WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH` | enum WotbModV3SoundInterceptDecision |
| `wotb.audio_intercept.SUBSTITUTE` | `WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE` | enum WotbModV3SoundInterceptDecision |
| `wotb.audio_intercept.SUPPRESS` | `WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS` | enum WotbModV3SoundInterceptDecision |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `intercept_register` | event_name: string, priority: integer, callback: function | token: handle |
| `intercept_unregister` | token: handle | true |
| `intercept_set_priority` | token: handle, priority: integer | true |
| `is_intercept_active` | — | active: integer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["audio.events"]
local intercept_register, err = wotb.audio_intercept.intercept_register("...", 0, function(...) end)  -- token: handle
if intercept_register == nil then wotb.log.warn("audio_intercept.intercept_register: %s", err) end
local intercept_unregister_ok, err = wotb.audio_intercept.intercept_unregister(handle)
if not intercept_unregister_ok then wotb.log.warn("audio_intercept.intercept_unregister: %s", err) end
local intercept_set_priority_ok, err = wotb.audio_intercept.intercept_set_priority(handle, 0)
if not intercept_set_priority_ok then wotb.log.warn("audio_intercept.intercept_set_priority: %s", err) end
```

