# `wotb.players`

Фасад поверх raw-таблиц: [`wotb.entity_public`](../reference/entity_public.md).

## Методы

- `wotb.players.snapshot`
- `wotb.players.local_player`
- `wotb.players.me`
- `wotb.players.our_team`
- `wotb.players.allies`
- `wotb.players.enemy_team`
- `wotb.players.enemies`
- `wotb.players.unknown_team`
- `wotb.players.visible`
- `wotb.players.find`
- `wotb.players.by_id`
- `wotb.players.each_visible`
- `wotb.players.details`

## Как пользоваться

### `wotb.players`: короткие имена, честная поза, `details`

- `me()` — то же, что `local_player()` (`local` — зарезервированное слово Lua,
  поэтому не `local()`);
- `allies()` — свои без локального игрока; `our_team()` по-прежнему включает
  его;
- `enemies()` — то же, что `enemy_team()`: только уже видимые клиенту;
- `by_id(public_id)` — то же, что `find`;
- `each_visible(fn)` — `fn(record)` для каждой видимой машины в порядке
  `public_id`; `return false` из `fn` останавливает обход; ответ — число
  посещённых записей.

Флаги `position_available`/`direction_available` выводятся из данных, а не
задаются константой `false`: loader публикует позу только машине, у которой
проверил цепочку внешности, и опубликованное направление — единичный вектор;
у машины без источника в снимке остаётся нулевой вектор, а `(0, 0, 0)` — не
направление. Единичная длина направления и есть признак настоящей позы;
позиция следует за ним, потому что обе величины берутся из одной матрицы. То
же правило применяет `event.data.snapshot` у `wotbmod.entity.public.*`.

`details(record_or_id)` отдаёт пять полей ростера, которые loader публикует
только по имени (snapshot ABI заморожен): `clan_tag` (пустая строка без
клана), `account_id`, `kills` (и `frags` — второе имя того же числа),
`vehicle_name` (`нация:тег`, например `usa:A100_T49`) и
`vehicle_display_name` (локализованное, «T49»). Это пять вызовов
`get_public_property` на машину, поэтому они делаются по запросу, а не внутри
`snapshot()`. Поле, которое клиент отказался отдать, отсутствует и названо в
`unavailable` с причиной клиента — ноль вместо него не подставляется.

```lua
local ally = wotb.players.allies()[1]
if ally then
    local d, err = wotb.players.details(ally)
    if d then
        print(ally.display_name, d.clan_tag, d.vehicle_display_name, d.kills)
    else
        print(err)      -- "players.details: entity_public is unavailable"
    end
end
```

