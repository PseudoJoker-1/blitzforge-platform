# `wotb.gameplay_hud`

Raw-таблица интерфейса `WotbModV3GameplayHudApiV1` (`include/wotbmod/gameplay_hud_v1.h`, версия `WOTBMOD_V3_GAMEPLAY_HUD_VERSION`). Функции ниже вызываются как `wotb.gameplay_hud.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.hud`](../facades/wotb-hud.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.hud`, `battle.ui`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.gameplay_hud.ANCHOR_TOP_LEFT` | `WOTBMOD_V3_HUD_ANCHOR_TOP_LEFT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_TOP` | `WOTBMOD_V3_HUD_ANCHOR_TOP` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_TOP_RIGHT` | `WOTBMOD_V3_HUD_ANCHOR_TOP_RIGHT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_LEFT` | `WOTBMOD_V3_HUD_ANCHOR_LEFT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_CENTER` | `WOTBMOD_V3_HUD_ANCHOR_CENTER` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_RIGHT` | `WOTBMOD_V3_HUD_ANCHOR_RIGHT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_BOTTOM_LEFT` | `WOTBMOD_V3_HUD_ANCHOR_BOTTOM_LEFT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_BOTTOM` | `WOTBMOD_V3_HUD_ANCHOR_BOTTOM` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.ANCHOR_BOTTOM_RIGHT` | `WOTBMOD_V3_HUD_ANCHOR_BOTTOM_RIGHT` | enum WotbModV3HudAnchor |
| `wotb.gameplay_hud.HIT_STYLE_GAME_DEFAULT` | `WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT` | enum WotbModV3HitIndicatorStyle |
| `wotb.gameplay_hud.HIT_STYLE_COMPACT` | `WOTBMOD_V3_HIT_STYLE_COMPACT` | enum WotbModV3HitIndicatorStyle |
| `wotb.gameplay_hud.HIT_STYLE_DIRECTIONAL` | `WOTBMOD_V3_HIT_STYLE_DIRECTIONAL` | enum WotbModV3HitIndicatorStyle |
| `wotb.gameplay_hud.HIT_STYLE_MINIMAL` | `WOTBMOD_V3_HIT_STYLE_MINIMAL` | enum WotbModV3HitIndicatorStyle |
| `wotb.gameplay_hud.STAT_DAMAGE` | `WOTBMOD_V3_STAT_DAMAGE` | enum WotbModV3SessionStat |
| `wotb.gameplay_hud.STAT_KILLS` | `WOTBMOD_V3_STAT_KILLS` | enum WotbModV3SessionStat |
| `wotb.gameplay_hud.STAT_WN8` | `WOTBMOD_V3_STAT_WN8` | enum WotbModV3SessionStat |
| `wotb.gameplay_hud.STAT_WINRATE` | `WOTBMOD_V3_STAT_WINRATE` | enum WotbModV3SessionStat |
| `wotb.gameplay_hud.STAT_SHOTS` | `WOTBMOD_V3_STAT_SHOTS` | enum WotbModV3SessionStat |
| `wotb.gameplay_hud.STAT_PENETRATION_RATE` | `WOTBMOD_V3_STAT_PENETRATION_RATE` | enum WotbModV3SessionStat |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `reticle_set_texture` | texture_uri: string | true |
| `reticle_set_color` | rgba: integer | true |
| `reticle_set_size` | scale: number | true |
| `reticle_set_sniper_texture` | texture_uri: string | true |
| `reticle_set_reloading_indicator` | enabled: integer | true |
| `reticle_set_dispersion_circle` | enabled: integer | true |
| `damagelog_set_enabled` | enabled: integer | true |
| `damagelog_set_position` | anchor: integer | true |
| `damagelog_set_max_entries` | count: integer | true |
| `damagelog_set_show_blocked` | enabled: integer | true |
| `damagelog_set_show_ricochet` | enabled: integer | true |
| `damagelog_set_show_module_damage` | enabled: integer | true |
| `damagelog_set_format` | format: string | true |
| `damagelog_set_filter_own` | enabled: integer | true |
| `session_stats_set_enabled` | enabled: integer | true |
| `session_stats_set_fields` | fields: integer | true |
| `minimap_set_size` | scale: number | true |
| `minimap_set_opacity` | opacity: number | true |
| `minimap_set_show_last_known` | enabled: integer | true |
| `minimap_set_show_artillery_range` | enabled: integer | true |
| `minimap_set_show_drawing` | enabled: integer | true |
| `minimap_add_marker` | world_x: number, world_z: number, label: string, color: integer | marker_id: integer |
| `minimap_remove_marker` | marker_id: integer | true |
| `sixth_sense_set_texture` | texture_uri: string | true |
| `sixth_sense_set_sound` | audio_uri: string | true |
| `sixth_sense_set_position` | position: table:Vec2 | true |
| `sixth_sense_set_scale` | scale: number | true |
| `sixth_sense_set_delay_ms` | milliseconds: number | true |
| `hit_indicator_set_style` | style: integer | true |
| `hit_indicator_set_color_hit` | rgba: integer | true |
| `hit_indicator_set_color_pen` | rgba: integer | true |
| `hit_indicator_set_color_ricochet` | rgba: integer | true |
| `hit_indicator_set_color_crit` | rgba: integer | true |
| `reset` | — | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.hud", "battle.ui"]
local reticle_set_texture_ok, err = wotb.gameplay_hud.reticle_set_texture("...")
if not reticle_set_texture_ok then wotb.log.warn("gameplay_hud.reticle_set_texture: %s", err) end
local reticle_set_color_ok, err = wotb.gameplay_hud.reticle_set_color(0)
if not reticle_set_color_ok then wotb.log.warn("gameplay_hud.reticle_set_color: %s", err) end
local reticle_set_size_ok, err = wotb.gameplay_hud.reticle_set_size(0.0)
if not reticle_set_size_ok then wotb.log.warn("gameplay_hud.reticle_set_size: %s", err) end
```

