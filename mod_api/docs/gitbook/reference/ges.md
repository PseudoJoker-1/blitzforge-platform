# `wotb.ges`

Raw-таблица интерфейса `WotbModV3GesApiV1` (`include/wotbmod/ges_v1.h`, версия `WOTBMOD_V3_GES_VERSION`). Функции ниже вызываются как `wotb.ges.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.ges`](../facades/wotb-ges.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `ges.observe`. Имена объявляются в `permissions` манифеста.

Биндинг ручной: `loader/lua/lua_bind_ges.cpp`.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.ges.TYPE_NAME_SIZE` | `WOTBMOD_V3_GES_TYPE_NAME_SIZE` | define |
| `wotb.ges.TOPIC_PREFIX` | `WOTBMOD_V3_GES_TOPIC_PREFIX` | строка |
| `wotb.ges.EVENT_MOD_PUBLISHED` | `WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED` | define |
| `wotb.ges.EVENT_SIZE_KNOWN` | `WOTBMOD_V3_GES_EVENT_SIZE_KNOWN` | define |
| `wotb.ges.EVENT_SCHEMA_KNOWN` | `WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN` | define |
| `wotb.ges.PUBLISH_ECHO` | `WOTBMOD_V3_GES_PUBLISH_ECHO` | define |
| `wotb.ges.FIELD_I32` | `WOTBMOD_V3_GES_FIELD_I32` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_U32` | `WOTBMOD_V3_GES_FIELD_U32` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_F32` | `WOTBMOD_V3_GES_FIELD_F32` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_BOOL` | `WOTBMOD_V3_GES_FIELD_BOOL` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_PTR` | `WOTBMOD_V3_GES_FIELD_PTR` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_CSTR` | `WOTBMOD_V3_GES_FIELD_CSTR` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_FASTNAME` | `WOTBMOD_V3_GES_FIELD_FASTNAME` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_BYTES` | `WOTBMOD_V3_GES_FIELD_BYTES` | enum WotbModV3GesFieldKind |
| `wotb.ges.FIELD_U8` | `WOTBMOD_V3_GES_FIELD_U8` | enum WotbModV3GesFieldKind |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `list_types` | names: char*, capacity: integer | count: integer |
| `read_i32` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_u32` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_f32` | event: table:GesEvent, offset: integer, out: number | true |
| `read_bool` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_ptr` | event: table:GesEvent, offset: integer, out: void* | true |
| `read_cstring` | event: table:GesEvent, offset: integer, buffer: string, capacity: integer | true |
| `get_schema` | type_name: string | schema_id: integer, payload_size: integer, field_count: integer |
| `schema_field` | schema_id: integer, index: integer | field: table:GesField |
| `publish` | type_name: string, payload: void, payload_size: integer, flags: integer | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["ges.observe"]
local list_types, err = wotb.ges.list_types(names, 0)  -- count: integer
if list_types == nil then wotb.log.warn("ges.list_types: %s", err) end
local read_i32_ok, err = wotb.ges.read_i32(event, 0, 0)
if not read_i32_ok then wotb.log.warn("ges.read_i32: %s", err) end
local read_u32_ok, err = wotb.ges.read_u32(event, 0, 0)
if not read_u32_ok then wotb.log.warn("ges.read_u32: %s", err) end
```

