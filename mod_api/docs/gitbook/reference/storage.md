# `wotb.storage`

Raw-таблица интерфейса `WotbModV3StorageApiV1` (`include/wotbmod/storage_v1.h`, версия `WOTBMOD_V3_STORAGE_VERSION`). Функции ниже вызываются как `wotb.storage.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.store`](../facades/wotb-store.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `storage`. Имена объявляются в `permissions` манифеста.

Биндинг ручной: `loader/lua/lua_bind_storage.cpp`.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.storage.KEY_MAX` | `WOTBMOD_V3_STORAGE_KEY_MAX` | define |
| `wotb.storage.PATH_DATA` | `WOTBMOD_V3_STORAGE_PATH_DATA` | enum WotbModV3StoragePathKind |
| `wotb.storage.PATH_CONFIG` | `WOTBMOD_V3_STORAGE_PATH_CONFIG` | enum WotbModV3StoragePathKind |
| `wotb.storage.PATH_CACHE` | `WOTBMOD_V3_STORAGE_PATH_CACHE` | enum WotbModV3StoragePathKind |
| `wotb.storage.PATH_TEMP` | `WOTBMOD_V3_STORAGE_PATH_TEMP` | enum WotbModV3StoragePathKind |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_json` | key: string | string |
| `set_json` | key: string, json_utf8: string | true |
| `get_bytes` | key: string | bytes |
| `set_bytes` | key: string, value: table:ConstBuffer | true |
| `erase` | key: string | true |
| `contains` | key: string | contains: integer |
| `flush` | — | true |
| `begin_transaction` | — | transaction: handle |
| `transaction_set_json` | transaction: handle, key: string, json_utf8: string | true |
| `transaction_set_bytes` | transaction: handle, key: string, value: table:ConstBuffer | true |
| `transaction_erase` | transaction: handle, key: string | true |
| `commit` | transaction: handle | true |
| `rollback` | transaction: handle | true |
| `get_path` | path_kind: integer | string |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["storage"]
local get_json, err = wotb.storage.get_json("...")  -- string
if get_json == nil then wotb.log.warn("storage.get_json: %s", err) end
local set_json_ok, err = wotb.storage.set_json("...", "...")
if not set_json_ok then wotb.log.warn("storage.set_json: %s", err) end
local get_bytes, err = wotb.storage.get_bytes("...")  -- bytes
if get_bytes == nil then wotb.log.warn("storage.get_bytes: %s", err) end
```

