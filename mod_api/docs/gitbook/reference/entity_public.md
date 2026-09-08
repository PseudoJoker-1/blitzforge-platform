# `wotb.entity_public`

Raw-таблица интерфейса `WotbModV3EntityPublicApiV1` (`include/wotbmod/entity_public_v1.h`, версия `WOTBMOD_V3_ENTITY_PUBLIC_VERSION`). Функции ниже вызываются как `wotb.entity_public.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.players`](../facades/wotb-players.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `entity.public.visible`, `game.entity.public`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.entity_public.TYPE_UNKNOWN` | `WOTBMOD_V3_PUBLIC_ENTITY_UNKNOWN` | enum WotbModV3PublicEntityType |
| `wotb.entity_public.TYPE_VEHICLE` | `WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE` | enum WotbModV3PublicEntityType |
| `wotb.entity_public.TYPE_PROJECTILE` | `WOTBMOD_V3_PUBLIC_ENTITY_PROJECTILE` | enum WotbModV3PublicEntityType |
| `wotb.entity_public.TYPE_EFFECT` | `WOTBMOD_V3_PUBLIC_ENTITY_EFFECT` | enum WotbModV3PublicEntityType |
| `wotb.entity_public.VALUE_BOOL` | `WOTBMOD_V3_PUBLIC_VALUE_BOOL` | enum WotbModV3PublicValueType |
| `wotb.entity_public.VALUE_INT64` | `WOTBMOD_V3_PUBLIC_VALUE_INT64` | enum WotbModV3PublicValueType |
| `wotb.entity_public.VALUE_DOUBLE` | `WOTBMOD_V3_PUBLIC_VALUE_DOUBLE` | enum WotbModV3PublicValueType |
| `wotb.entity_public.VALUE_VEC3` | `WOTBMOD_V3_PUBLIC_VALUE_VEC3` | enum WotbModV3PublicValueType |
| `wotb.entity_public.VALUE_STRING` | `WOTBMOD_V3_PUBLIC_VALUE_STRING` | enum WotbModV3PublicValueType |
| `wotb.entity_public.REASON_UNKNOWN` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_UNKNOWN` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_LOCAL_PLAYER` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_VISIBLE` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_UPDATED` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_HIDDEN` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_LEFT_WORLD` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_LEFT_WORLD` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_NATIVE_DESTROYED` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_NATIVE_DESTROYED` | enum WotbModV3PublicEntityChangeReason |
| `wotb.entity_public.REASON_SHUTDOWN` | `WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN` | enum WotbModV3PublicEntityChangeReason |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_public_id` | entity: handle | public_id: integer |
| `get_public_type` | entity: handle | string |
| `get_public_property` | entity: handle, property: string | value: table:PublicValue |
| `subscribe_public_property` | entity: handle, property: string, callback: function | token: handle |
| `unsubscribe_public_property` | token: handle | true |
| `is_visible_to_player` | entity: handle | visible: integer |
| `get_snapshot` | entity: handle | snapshot: table:PublicEntitySnapshot |
| `enumerate_visible` | visitor: function | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["entity.public.visible", "game.entity.public"]
local get_public_id, err = wotb.entity_public.get_public_id(handle)  -- public_id: integer
if get_public_id == nil then wotb.log.warn("entity_public.get_public_id: %s", err) end
local get_public_type, err = wotb.entity_public.get_public_type(handle)  -- string
if get_public_type == nil then wotb.log.warn("entity_public.get_public_type: %s", err) end
local get_public_property, err = wotb.entity_public.get_public_property(handle, "...")  -- value: table:PublicValue
if get_public_property == nil then wotb.log.warn("entity_public.get_public_property: %s", err) end
```

