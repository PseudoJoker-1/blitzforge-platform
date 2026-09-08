# `wotb.projectile`

Raw-таблица интерфейса `WotbModV3ProjectileApiV2` (`include/wotbmod/projectile_v2.h`, версия `WOTBMOD_V3_PROJECTILE_VERSION_2`). Функции ниже вызываются как `wotb.projectile.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.shells`](../facades/wotb-shells.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.projectile_visual`, `visible.projectile.events`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.projectile.OWNER_UNKNOWN` | `WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN` | enum WotbModV3ProjectileOwnerScope |
| `wotb.projectile.OWNER_LOCAL_PLAYER` | `WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER` | enum WotbModV3ProjectileOwnerScope |
| `wotb.projectile.OWNER_ALLY_VISIBLE` | `WOTBMOD_V3_PROJECTILE_OWNER_ALLY_VISIBLE` | enum WotbModV3ProjectileOwnerScope |
| `wotb.projectile.OWNER_ENEMY_VISIBLE` | `WOTBMOD_V3_PROJECTILE_OWNER_ENEMY_VISIBLE` | enum WotbModV3ProjectileOwnerScope |
| `wotb.projectile.OWNER_REPLAY` | `WOTBMOD_V3_PROJECTILE_OWNER_REPLAY` | enum WotbModV3ProjectileOwnerScope |
| `wotb.projectile.STATE_CREATED` | `WOTBMOD_V3_PROJECTILE_STATE_CREATED` | enum WotbModV3ProjectileLifecycleState |
| `wotb.projectile.STATE_IN_FLIGHT` | `WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT` | enum WotbModV3ProjectileLifecycleState |
| `wotb.projectile.STATE_IMPACTED` | `WOTBMOD_V3_PROJECTILE_STATE_IMPACTED` | enum WotbModV3ProjectileLifecycleState |
| `wotb.projectile.STATE_DESTROYED` | `WOTBMOD_V3_PROJECTILE_STATE_DESTROYED` | enum WotbModV3ProjectileLifecycleState |
| `wotb.projectile.SOURCE_STOCK_SHOT` | `WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT` | enum WotbModV3ProjectileSource |
| `wotb.projectile.SOURCE_NATIVE_IMPACT` | `WOTBMOD_V3_PROJECTILE_SOURCE_NATIVE_IMPACT` | enum WotbModV3ProjectileSource |
| `wotb.projectile.SOURCE_STOCK_TRACER` | `WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_TRACER` | enum WotbModV3ProjectileSource |
| `wotb.projectile.SOURCE_API_MANAGED` | `WOTBMOD_V3_PROJECTILE_SOURCE_API_MANAGED` | enum WotbModV3ProjectileSource |
| `wotb.projectile.FIELD_NATIVE_SHOT_ID` | `WOTBMOD_V3_PROJECTILE_FIELD_NATIVE_SHOT_ID` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_PRIMARY_ENTITY` | `WOTBMOD_V3_PROJECTILE_FIELD_PRIMARY_ENTITY` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_SECONDARY_ENTITY` | `WOTBMOD_V3_PROJECTILE_FIELD_SECONDARY_ENTITY` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_SHELL_TYPE` | `WOTBMOD_V3_PROJECTILE_FIELD_SHELL_TYPE` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_ORIGIN` | `WOTBMOD_V3_PROJECTILE_FIELD_ORIGIN` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_DIRECTION` | `WOTBMOD_V3_PROJECTILE_FIELD_DIRECTION` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_POSITION` | `WOTBMOD_V3_PROJECTILE_FIELD_POSITION` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_IMPACT_POSITION` | `WOTBMOD_V3_PROJECTILE_FIELD_IMPACT_POSITION` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELD_STOCK_SHOT_CODE` | `WOTBMOD_V3_PROJECTILE_FIELD_STOCK_SHOT_CODE` | enum WotbModV3ProjectileField |
| `wotb.projectile.FIELDS_ALL` | `WOTBMOD_V3_PROJECTILE_FIELDS_ALL` | enum WotbModV3ProjectileField |
| `wotb.projectile.DESTROY_NATIVE` | `WOTBMOD_V3_PROJECTILE_DESTROY_NATIVE` | enum WotbModV3ProjectileDestroyReason |
| `wotb.projectile.DESTROY_IMPACT_COMPLETE` | `WOTBMOD_V3_PROJECTILE_DESTROY_IMPACT_COMPLETE` | enum WotbModV3ProjectileDestroyReason |
| `wotb.projectile.DESTROY_TIMEOUT` | `WOTBMOD_V3_PROJECTILE_DESTROY_TIMEOUT` | enum WotbModV3ProjectileDestroyReason |
| `wotb.projectile.DESTROY_ENTITY_REMOVED` | `WOTBMOD_V3_PROJECTILE_DESTROY_ENTITY_REMOVED` | enum WotbModV3ProjectileDestroyReason |
| `wotb.projectile.DESTROY_SHUTDOWN` | `WOTBMOD_V3_PROJECTILE_DESTROY_SHUTDOWN` | enum WotbModV3ProjectileDestroyReason |
| `wotb.projectile.IMPACT_VISUAL_NONE` | `WOTBMOD_V3_IMPACT_VISUAL_NONE` | enum WotbModV3ImpactVisualFlag |
| `wotb.projectile.IMPACT_VISUAL_NATIVE_SCENE` | `WOTBMOD_V3_IMPACT_VISUAL_NATIVE_SCENE` | enum WotbModV3ImpactVisualFlag |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `tracer_style_register` | descriptor: table:TracerStyleDescriptor | style: handle |
| `tracer_style_unregister` | style: handle | true |
| `tracer_set_texture` | style: handle, texture_uri: string | true |
| `tracer_set_color` | style: handle, color: table:Color | true |
| `tracer_set_width` | style: handle, width: number | true |
| `tracer_set_lifetime` | style: handle, seconds: number | true |
| `tracer_set_fade` | style: handle, fade_start: number | true |
| `projectile_get_visual_entity` | projectile: handle | entity: handle |
| `projectile_attach_visual` | projectile: handle, entity: handle, lifetime_policy: integer | true |
| `projectile_get_owner_scope` | projectile: handle | scope: integer |
| `projectile_get_snapshot` | projectile: handle | snapshot: table:ProjectileSnapshot |
| `impact_visual_register` | descriptor: table:ImpactVisualDescriptor | visual: handle |
| `impact_visual_update` | visual: handle, descriptor: table:ImpactVisualDescriptor | true |
| `impact_visual_unregister` | visual: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.projectile_visual", "visible.projectile.events"]
local tracer_style_register, err = wotb.projectile.tracer_style_register(descriptor)  -- style: handle
if tracer_style_register == nil then wotb.log.warn("projectile.tracer_style_register: %s", err) end
local tracer_style_unregister_ok, err = wotb.projectile.tracer_style_unregister(handle)
if not tracer_style_unregister_ok then wotb.log.warn("projectile.tracer_style_unregister: %s", err) end
local tracer_set_texture_ok, err = wotb.projectile.tracer_set_texture(handle, "...")
if not tracer_set_texture_ok then wotb.log.warn("projectile.tracer_set_texture: %s", err) end
```

