# `wotb.client`

Raw-таблица интерфейса `WotbModV3ClientApiV1` (`include/wotbmod/client_v1.h`, версия `WOTBMOD_V3_CLIENT_VERSION`). Функции ниже вызываются как `wotb.client.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `client.leave_to_hangar`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.client.COMPATIBILITY_UNKNOWN` | `WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN` | enum WotbModV3ClientCompatibility |
| `wotb.client.COMPATIBILITY_SUPPORTED` | `WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED` | enum WotbModV3ClientCompatibility |
| `wotb.client.COMPATIBILITY_DEGRADED` | `WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED` | enum WotbModV3ClientCompatibility |
| `wotb.client.COMPATIBILITY_BINDINGS_MISSING` | `WOTBMOD_V3_CLIENT_COMPATIBILITY_BINDINGS_MISSING` | enum WotbModV3ClientCompatibility |
| `wotb.client.COMPATIBILITY_HASH_MISMATCH` | `WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH` | enum WotbModV3ClientCompatibility |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `is_supported` | — | supported: integer |
| `get_binding_pack_version` | — | version: integer |
| `get_compatibility_state` | — | state: integer |
| `enumerate_missing_bindings` | visitor: function | true |
| `leave_to_hangar` | — | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["client.leave_to_hangar"]
local is_supported, err = wotb.client.is_supported()  -- supported: integer
if is_supported == nil then wotb.log.warn("client.is_supported: %s", err) end
local get_binding_pack_version, err = wotb.client.get_binding_pack_version()  -- version: integer
if get_binding_pack_version == nil then wotb.log.warn("client.get_binding_pack_version: %s", err) end
local get_compatibility_state, err = wotb.client.get_compatibility_state()  -- state: integer
if get_compatibility_state == nil then wotb.log.warn("client.get_compatibility_state: %s", err) end
```

