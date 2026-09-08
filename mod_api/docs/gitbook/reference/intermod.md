# `wotb.intermod`

Raw-таблица интерфейса `WotbModV3IntermodApiV1` (`include/wotbmod/intermod_v1.h`, версия `WOTBMOD_V3_INTERMOD_VERSION`). Функции ниже вызываются как `wotb.intermod.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.intermod.MAX_SERVICE_ID` | `WOTBMOD_V3_MAX_SERVICE_ID` | define |
| `wotb.intermod.MAX_MESSAGE_TOPIC` | `WOTBMOD_V3_MAX_MESSAGE_TOPIC` | define |
| `wotb.intermod.MAX_PAYLOAD` | `WOTBMOD_V3_MAX_INTERMOD_PAYLOAD` | define |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `mod_find` | mod_id: string | mod: handle |
| `mod_is_loaded` | mod_id: string | loaded: integer |
| `mod_get_version` | target_mod: handle | string |
| `mod_get_dependency` | dependency_id: string | dependency: handle, optional: integer |
| `export_interface` | — | *не публикуется в Lua: нужен сырой нативный указатель* |
| `unexport_interface` | export_token: handle | true |
| `import_interface` | service_id: string, minimum_version: integer | interface: table:ImportedInterface |
| `enumerate_interfaces` | — | interfaces: array |
| `message_publish` | topic: string, payload: void, payload_size: integer | true |
| `message_subscribe` | topic_pattern: string, priority: integer, callback: function | token: handle |
| `message_unsubscribe` | token: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local mod_find, err = wotb.intermod.mod_find("...")  -- mod: handle
if mod_find == nil then wotb.log.warn("intermod.mod_find: %s", err) end
local mod_is_loaded, err = wotb.intermod.mod_is_loaded("...")  -- loaded: integer
if mod_is_loaded == nil then wotb.log.warn("intermod.mod_is_loaded: %s", err) end
local mod_get_version, err = wotb.intermod.mod_get_version(handle)  -- string
if mod_get_version == nil then wotb.log.warn("intermod.mod_get_version: %s", err) end
```

