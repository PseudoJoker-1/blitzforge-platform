# Lua Facade Battle

Телеметрия боя на фасадах: `wotb.battle.on_shot/on_hit/on_damage/
on_vehicle_destroyed/on_end`, имена через `wotb.players.by_id`, детали
(тег клана, фраги) через `wotb.players.details`, список машин через 30 с после
старта (`wotb.timer.after`), накопительный счётчик боёв через `wotb.store`.
`on_frame` не определён — цена на кадр ноль.

Скопируйте папку как `<game>\mods\lua\example.lua_facade_battle`. Шаблон:
`wotbmod new ... --type lua --template battle`.
