# `wotb.capabilities`

Raw-таблица интерфейса `WotbModV3CapabilitiesApiV1` (`include/wotbmod/capabilities_v1.h`, версия `WOTBMOD_V3_CAPABILITIES_VERSION`). Функции ниже вызываются как `wotb.capabilities.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.mod`](../facades/wotb-mod.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.capabilities.STATUS_AVAILABLE` | `WOTBMOD_V3_CAPABILITY_AVAILABLE` | enum WotbModV3CapabilityStatus |
| `wotb.capabilities.STATUS_UNAVAILABLE` | `WOTBMOD_V3_CAPABILITY_UNAVAILABLE` | enum WotbModV3CapabilityStatus |
| `wotb.capabilities.STATUS_CLIENT_MISMATCH` | `WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH` | enum WotbModV3CapabilityStatus |
| `wotb.capabilities.STATUS_PERMISSION_DENIED` | `WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED` | enum WotbModV3CapabilityStatus |
| `wotb.capabilities.STATUS_CONTEXT_RESTRICTED` | `WOTBMOD_V3_CAPABILITY_CONTEXT_RESTRICTED` | enum WotbModV3CapabilityStatus |
| `wotb.capabilities.STATUS_DEGRADED` | `WOTBMOD_V3_CAPABILITY_DEGRADED` | enum WotbModV3CapabilityStatus |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_count` | — | count: integer |
| `get_at` | index: integer | info: table:CapabilityInfo |
| `query` | capability_name: string | info: table:CapabilityInfo |
| `subscribe` | callback: function | token: handle |
| `unsubscribe` | token: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local get_count, err = wotb.capabilities.get_count()  -- count: integer
if get_count == nil then wotb.log.warn("capabilities.get_count: %s", err) end
local get_at, err = wotb.capabilities.get_at(0)  -- info: table:CapabilityInfo
if get_at == nil then wotb.log.warn("capabilities.get_at: %s", err) end
local query, err = wotb.capabilities.query("...")  -- info: table:CapabilityInfo
if query == nil then wotb.log.warn("capabilities.query: %s", err) end
```

