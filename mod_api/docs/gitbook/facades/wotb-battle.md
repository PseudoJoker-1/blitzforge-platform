# `wotb.battle`

Фасад поверх raw-таблиц: [`wotb.events`](../reference/events.md), [`wotb.core`](../reference/core.md).

## Методы

- `wotb.battle.start`
- `wotb.battle.stop`
- `wotb.battle.tracking`
- `wotb.battle.reset`
- `wotb.battle.snapshot`
- `wotb.battle.groups`
- `wotb.battle.on`
- `wotb.battle.once`
- `wotb.battle.off`
- `wotb.battle.off_all`
- `wotb.battle.events`
- `wotb.battle.on_<name> (enter`
- `wotb.battle.start`
- `wotb.battle.end`
- `wotb.battle.leave`
- `wotb.battle.shot`
- `wotb.battle.hit`
- `wotb.battle.reload`
- `wotb.battle.ammo`
- `wotb.battle.damage`
- `wotb.battle.death`
- `wotb.battle.vehicle_destroyed`
- `wotb.battle.spotted`
- `wotb.battle.unspotted`
- `wotb.battle.camera_changed`
- `wotb.battle.sniper_entered`
- `wotb.battle.sniper_exited)`
- `wotb.battle.is_active`
- `wotb.battle.state`

## Как пользоваться

### `wotb.battle`

Один снимок вместо одиннадцати ручных подписок. Модуль подписывается только на
topics, которые host действительно публикует, и на каждое поле помнит, откуда и
когда оно пришло.

| Функция | Возвращает |
| --- | --- |
| `battle.snapshot()` | таблицу снимка либо `nil, message` |
| `battle.start([groups])` | `true` либо `nil, message` |
| `battle.stop()` | `true` |
| `battle.reset()` | `true` |
| `battle.tracking()` | `true`/`false` |
| `battle.groups()` | отсортированный список имён групп |

Группы: `lifecycle`, `local_vehicle`, `health`, `ammo`, `reload`, `camera`,
`damage`. Подписка ленивая: до первого `start()` или `snapshot()` не создаётся
ни одной. Запрос группы `health` автоматически включает `local_vehicle` — без
entity id локальной машины HP приписать некому.

Снимок содержит:

| Ключ | Что это |
| --- | --- |
| `values` | только реально наблюдавшиеся поля |
| `source` | topic, из которого пришло каждое поле |
| `updated_ns` | `timestamp_ns` события, из которого пришло поле |
| `unavailable` | имя поля → причина, по которой поля нет |
| `subscribe_errors` | topic → почему подписка не удалась |
| `ignored` | `health_without_local_entity_id` — сколько health-событий отброшено |
| `tracking`, `groups` | состояние самого модуля |

Поля из `values` продублированы прямо на снимке, поэтому пишется `snap.health`.

**Поле, которого никто не наблюдал, равно `nil` и одновременно перечислено в
`snapshot.unavailable` с причиной. Оно никогда не подменяется нулём.** Ноль,
выданный за реальные данные, неотличим от факта. Наблюдённый `false` — например,
`snap.sniper` после выхода из снайперского режима — это, наоборот, ответ, и он в
`unavailable` не попадает.

```lua
local snap, err = wotb.battle.snapshot()
if snap == nil then
    wotb.log.warn("battle: %s", err)
    return
end
if snap.health == nil then
    wotb.log.debug("HP пока нет: %s", snap.unavailable.health)
else
    wotb.log.info("HP %d (из %s)", snap.health, snap.source.health)
end
```

Три вещи, которые дешевле прочитать здесь, чем выяснить на практике.

- `max_health` недоступен **навсегда**: ни один публикуемый payload его не
  несёт, в `WotbModV3VehicleEventData` есть только `previous_health` и
  `health`. Строка причины в `unavailable.max_health` прямо называет замену —
  `wotb.players.local_player().max_health`, который приходит из
  `entity_public.enumerate_visible`. Это другой источник, и обращаться с ним
  нужно как с другим.
- `damage_dealt` не существует и не появится: у health-ingress нет доказанного
  источника атакующего, поэтому приписать урон локальному игроку можно было бы
  только угадав. Runtime по той же причине не публикует
  `wotbmod.gameplay.damage_dealt`. Полученный урон, наоборот, есть:
  `snap.damage_received` с полями `damage`, `previous_health`, `health`,
  `reason_code` и `source_entity_id`.
- вход в бой и выход из боя **очищают все per-battle поля**. HP прошлого боя,
  выданный за текущий, — ровно тот отказ, ради предотвращения которого правило и
  введено. `lifecycle` при этом сохраняется: «мы только что вышли из боя» —
  верное утверждение и после выхода.

Пока не пришло `wotbmod.vehicle.local.changed`, health-события приписать некому.
Они не записываются, а считаются, и счётчик виден в
`snapshot.ignored.health_without_local_entity_id`. Отдельно различаются «поле не
обновляется» и «этот клиент такой topic не публикует»: второе видно в
`snapshot.subscribe_errors`.

### `wotb.battle`: обработчики по коротким именам

`on(name, fn)`, `once(name, fn)`, `off(handle)`, `off_all()`, `events()` и по
одной функции `on_<name>(fn)` на имя. Имена и topic-и — одна таблица в
`lua_preludes.cpp`, она же документация:

| Имя | Topic |
| --- | --- |
| `enter` / `start` / `end` / `leave` | `wotbmod.battle.entered` / `.started` / `.ended` / `.left` |
| `shot` | `wotbmod.gameplay.shot_fired` |
| `hit` | `wotbmod.gameplay.shell_hit` |
| `reload` | `wotbmod.gameplay.reload_state_changed` |
| `ammo` | `wotbmod.gameplay.ammo_changed` |
| `damage` | `wotbmod.gameplay.damage_received` |
| `death` | `wotbmod.vehicle.local.destroyed` |
| `vehicle_destroyed` | `wotbmod.vehicle.killed` (victim/killer/assist из ростера арены) |
| `spotted` / `unspotted` | `wotbmod.vehicle.spotted` / `.unspotted` |
| `camera_changed` | `wotbmod.gameplay.camera_mode_changed` |
| `sniper_entered` / `sniper_exited` | `wotbmod.gameplay.sniper_entered` / `_exited` |

Обработчик получает `fn(data, event)`: сначала типизированный payload
(`event.data`; `nil`, если эта сборка его не публикует — фасад ничего не
синтезирует), затем само событие. Каждый вызов идёт под `pcall`: сломанный
обработчик попадает в `wotb.log` как `battle.on_<name> handler raised`, а
следующий обработчик всё равно выполняется. Пока `on()` не вызван, подписок
нет; `off()` возвращает подписку клиенту; `once` снимает себя перед первым
вызовом.

```lua
wotb.battle.on_vehicle_destroyed(function(kill)
    local victim = wotb.players.by_id(kill.victim_id)
    local killer = wotb.players.by_id(kill.killer_id)
    print((type(killer) == "table" and killer.display_name or "?") ..
          " уничтожил " ..
          (type(victim) == "table" and victim.display_name or "?"))
end)
local h, err = wotb.battle.on("teleport", print)
-- h == nil, err == "battle.on: 'teleport' is not a battle event; battle.events() lists them"
```

`is_active()` — в бою ли игрок сейчас (`BATTLE` или `TRAINING` в маске
контекста, без подписок); `state()` — таблица с `active`, `context`,
`tracking` и, пока включён `start()`, последним lifecycle-событием
(`lifecycle`) или причиной его отсутствия (`lifecycle_unavailable`).

