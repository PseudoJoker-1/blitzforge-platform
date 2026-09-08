# `wotb.resources`

Raw-таблица интерфейса `WotbModV3ResourcesApiV1` (`include/wotbmod/resources_v1.h`, версия `WOTBMOD_V3_RESOURCES_VERSION`). Функции ниже вызываются как `wotb.resources.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.files`](../facades/wotb-files.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `resources.mod`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.resources.BINARY` | `WOTBMOD_V3_RESOURCE_BINARY` | enum WotbModV3ResourceType |
| `wotb.resources.TEXT` | `WOTBMOD_V3_RESOURCE_TEXT` | enum WotbModV3ResourceType |
| `wotb.resources.IMAGE` | `WOTBMOD_V3_RESOURCE_IMAGE` | enum WotbModV3ResourceType |
| `wotb.resources.AUDIO` | `WOTBMOD_V3_RESOURCE_AUDIO` | enum WotbModV3ResourceType |
| `wotb.resources.MODEL` | `WOTBMOD_V3_RESOURCE_MODEL` | enum WotbModV3ResourceType |
| `wotb.resources.YAML` | `WOTBMOD_V3_RESOURCE_YAML` | enum WotbModV3ResourceType |
| `wotb.resources.JSON` | `WOTBMOD_V3_RESOURCE_JSON` | enum WotbModV3ResourceType |
| `wotb.resources.LOADING` | `WOTBMOD_V3_RESOURCE_LOADING` | enum WotbModV3ResourceState |
| `wotb.resources.READY` | `WOTBMOD_V3_RESOURCE_READY` | enum WotbModV3ResourceState |
| `wotb.resources.FAILED` | `WOTBMOD_V3_RESOURCE_FAILED` | enum WotbModV3ResourceState |
| `wotb.resources.CANCELLED` | `WOTBMOD_V3_RESOURCE_CANCELLED` | enum WotbModV3ResourceState |
| `wotb.resources.LOAD_DEFAULT` | `WOTBMOD_V3_RESOURCE_LOAD_DEFAULT` | enum WotbModV3ResourceLoadFlags |
| `wotb.resources.LOAD_REQUIRE_NATIVE_CLIENT` | `WOTBMOD_V3_RESOURCE_LOAD_REQUIRE_NATIVE_CLIENT` | enum WotbModV3ResourceLoadFlags |
| `wotb.resources.BACKING_NONE` | `WOTBMOD_V3_RESOURCE_BACKING_NONE` | enum WotbModV3ResourceBacking |
| `wotb.resources.BACKING_RAW_VFS_BYTES` | `WOTBMOD_V3_RESOURCE_BACKING_RAW_VFS_BYTES` | enum WotbModV3ResourceBacking |
| `wotb.resources.BACKING_NATIVE_CLIENT_OBJECT` | `WOTBMOD_V3_RESOURCE_BACKING_NATIVE_CLIENT_OBJECT` | enum WotbModV3ResourceBacking |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `load` | desc: table:ResourceLoadDesc | resource: handle |
| `load_async_ex` | desc: table:ResourceLoadDesc | resource: handle |
| `get_info` | resource: handle | info: table:ResourceInfo |
| `copy_data` | resource: handle | bytes |
| `retain` | resource: handle | true |
| `release` | resource: handle | true |
| `preload_group` | group: string, resources: array | true |
| `unload_group` | group: string | true |
| `reload` | resource: handle | true |
| `watch` | resource: handle | token: handle |
| `get_total_memory_usage` | — | bytes: integer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["resources.mod"]
local load, err = wotb.resources.load(desc)  -- resource: handle
if load == nil then wotb.log.warn("resources.load: %s", err) end
local load_async_ex, err = wotb.resources.load_async_ex(desc)  -- resource: handle
if load_async_ex == nil then wotb.log.warn("resources.load_async_ex: %s", err) end
local get_info, err = wotb.resources.get_info(handle)  -- info: table:ResourceInfo
if get_info == nil then wotb.log.warn("resources.get_info: %s", err) end
```

