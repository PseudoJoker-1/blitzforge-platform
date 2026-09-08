# `wotb.permissions`

Raw-таблица интерфейса `WotbModV3PermissionsApiV1` (`include/wotbmod/permissions_v1.h`, версия `WOTBMOD_V3_PERMISSIONS_VERSION`). Функции ниже вызываются как `wotb.permissions.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.mod`](../facades/wotb-mod.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.permissions.TIER_SAFE` | `WOTBMOD_V3_PERMISSION_SAFE` | enum WotbModV3PermissionTier |
| `wotb.permissions.TIER_GAMEPLAY_TWEAK` | `WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK` | enum WotbModV3PermissionTier |
| `wotb.permissions.TIER_REVIEWED` | `WOTBMOD_V3_PERMISSION_REVIEWED` | enum WotbModV3PermissionTier |
| `wotb.permissions.TIER_UNSAFE` | `WOTBMOD_V3_PERMISSION_UNSAFE` | enum WotbModV3PermissionTier |
| `wotb.permissions.STATE_DENIED` | `WOTBMOD_V3_PERMISSION_STATE_DENIED` | enum WotbModV3PermissionState |
| `wotb.permissions.STATE_GRANTED` | `WOTBMOD_V3_PERMISSION_STATE_GRANTED` | enum WotbModV3PermissionState |
| `wotb.permissions.STATE_REVIEW_REQUIRED` | `WOTBMOD_V3_PERMISSION_STATE_REVIEW_REQUIRED` | enum WotbModV3PermissionState |
| `wotb.permissions.STATE_UNAVAILABLE` | `WOTBMOD_V3_PERMISSION_STATE_UNAVAILABLE` | enum WotbModV3PermissionState |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_granted_tier` | — | tier: integer |
| `query` | permission_name: string | info: table:PermissionInfo |
| `get_count` | — | count: integer |
| `get_at` | index: integer | info: table:PermissionInfo |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local get_granted_tier, err = wotb.permissions.get_granted_tier()  -- tier: integer
if get_granted_tier == nil then wotb.log.warn("permissions.get_granted_tier: %s", err) end
local query, err = wotb.permissions.query("...")  -- info: table:PermissionInfo
if query == nil then wotb.log.warn("permissions.query: %s", err) end
local get_count, err = wotb.permissions.get_count()  -- count: integer
if get_count == nil then wotb.log.warn("permissions.get_count: %s", err) end
```

