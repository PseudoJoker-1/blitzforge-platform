# `wotb.hud`

Фасад поверх raw-таблиц: [`wotb.gameplay_hud`](../reference/gameplay_hud.md).

## Методы

- `wotb.hud.mode`
- `wotb.hud.available`
- `wotb.hud.status`
- `wotb.hud.reset`
- `wotb.hud.rgba`
- `wotb.hud.reticle.*`
- `wotb.hud.damage_log.*`
- `wotb.hud.session_stats.*`
- `wotb.hud.minimap.*`
- `wotb.hud.sixth_sense.*`
- `wotb.hud.hit_indicator.*`

## Как пользоваться

### `wotb.hud`: штатный HUD по группам

Фасад над 34 слотами `wotb.gameplay_hud`, сгруппированными как в заголовке
`gameplay_hud_v1.h`; строится при первом обращении, как `wotb.panel`.

- `mode()` — `native`, когда интерфейс `wotb.gameplay_hud` опубликован
  (runtime публикует его только с backend-ом), иначе
  `unavailable, reason`; правду о каждом слоте говорит ответ самого вызова;
  `available()`; `status()` — какие из 34 слотов опубликованы;
- `reticle.set_texture/set_sniper_texture/set_color/set_size/
  set_reloading_indicator/set_dispersion_circle`;
- `damage_log.show/hide/set_enabled/set_position/set_max_entries/
  set_show_blocked/set_show_ricochet/set_show_module_damage/set_format/
  set_filter_own`;
- `session_stats.show/hide/set_enabled/set_fields`;
- `minimap.set_size/set_opacity/set_show_last_known/
  set_show_artillery_range/set_show_drawing/add_marker/remove_marker`;
- `sixth_sense.set_texture/set_sound/set_position/set_scale/set_delay`;
- `hit_indicator.set_style/set_color_hit/set_color_pen/set_color_ricochet/
  set_color_crit`;
- `reset()` — единственный сброс в ABI, общий для всех групп; `rgba(color)`.

Аргументы: цвет — `{r, g, b[, a]}` в `0..1` или целое `0xRRGGBBAA`; флаги —
boolean; якорь, стиль и поля счётчиков — имя (`"top-right"`, `"compact"`,
`{"damage", "shots"}`) или число из констант `wotb.gameplay_hud.*`. Неверный
аргумент отклоняется до вызова ABI; отказ клиента приходит с префиксом фасада.
На 11.20.0.887 клиент отказывает `minimap.set_show_last_known`,
`damage_log.set_show_blocked`, `hit_indicator.set_color_pen`, стилю
`directional` и полю `kills` — фасад этого не скрывает.

```lua
if wotb.hud.mode() == "native" then
    wotb.hud.reticle.set_color({ r = 1, g = 0.25, b = 1 })
    wotb.hud.damage_log.set_position("top-right")
    local ok, err = wotb.hud.minimap.set_show_last_known(true)
    -- ok == nil, err == "hud.minimap.set_show_last_known: ... not supported"
end
```

