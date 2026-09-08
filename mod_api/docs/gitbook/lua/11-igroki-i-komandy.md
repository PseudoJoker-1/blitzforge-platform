# Игроки и команды

Lua convenience API `wotb.players` строится поверх
`wotb.entity_public.enumerate_visible`:

- `snapshot()` — полный снимок с `local_player`, `our_team`, `enemy_team`,
  `unknown_team`, `visible_players` и флагами полноты;
- `local_player()`, `our_team()`, `enemy_team()`, `unknown_team()`, `visible()`;
- `find(public_id)` — поиск только в текущем публичном снимке.

Каждая запись содержит handle, `public_id`, type, visible/local flags, health,
max health, alive/health_percent, public vehicle type и optional team/name.
Также добавляются `relation`, `is_local`, `is_ally`, `is_enemy` и явные
`*_available` флаги. На 11.20.0.887 подтверждены live ID, visibility/local,
health/max health, public type, team, display name (ник из ростера арены) и
поза всех публичных машин. `position_available`/`direction_available`
выводятся из данных, а не задаются константой: у машины с подтверждённой позой
направление — единичный вектор, у машины без источника — нулевой, и нулевой
вектор никогда не выдаётся за позу. Короткие имена (`me`, `allies`, `enemies`,
`by_id`, `each_visible`) и поля ростера по запросу (`details`: тег клана,
account id, фраги, имя танка) описаны в разделе «Фасады».

`enemy_team` принципиально содержит только уже видимых клиенту противников.
После unspot нелокальная entity удаляется из public registry. Если team не
подтверждён, машина попадает в `unknown_team`, а `relation_available` и
`team_data_complete` показывают, что деление на команды неполно. Скрытых enemy
HP/position/name и native pointers в Lua нет.
