# Типизированные игровые события

`wotb.events.subscribe` по-прежнему передаёт исходный binary payload в
`event.payload`, но для известных native topics дополнительно создаёт
`event.data`. Декодирование выполняется только при точном совпадении topic,
версии и полного размера структуры; повреждённый или укороченный payload даёт
`data = nil`, а raw bytes сохраняются для совместимости.

Аргументы `subscribe` позиционные:
`subscribe(topic, callback [, priority [, receive_system_events]])`. Клиентские
topics публикует сам клиент, поэтому четвёртый аргумент для них обязан быть
`true`.

```lua
local token, err = wotb.events.subscribe(
    wotb.events.TOPIC_DAMAGE_RECEIVED,
    function(event)
        local data = event.data
        if data then
            print("Получен урон: " .. tostring(data.damage))
            print("HP: " .. tostring(data.previous_health) .. " -> " ..
                  tostring(data.health))
        end
    end,
    wotb.events.PRIORITY_NORMAL,
    true)
if token == nil then print(err) end
```

Полного HP в этом payload нет: `WotbModV3DamageEventData` несёт `damage`,
`previous_health`, `health`, `reason_code` и `source_entity_id`, и ничего
больше. Максимальное HP берётся из `wotb.players.local_player().max_health` —
это другой источник, и учитывать его нужно как другой.

**Два последних поля на текущей сборке клиента всегда нули, и это не «пока
никто не бил».** Единственный производитель `wotbmod.vehicle.damaged` в дереве
(`loader/wotb_mod_loader.cpp`) обнуляет структуру и заполняет только `damage`,
`previous_health` и `health`; `reason_code` и `source_entity_id` не пишутся
никогда. Автор, прочитавший этот список без оговорки, выводит «источник 0» и
считает, что это идентификатор сущности номер ноль. Не считайте: ноль здесь
означает «поле не заполнено». Словаря значений для `reason_code` тоже нигде
нет — среди 586 сгенерированных констант нет ни одной `..._DAMAGE_REASON_*`,
так что код непрозрачен и печатать его как число бессмысленно.

**И отдельно про `max_health` у `wotb.players`.** Когда проверенное чтение
поля не удаётся, лоадер подставляет максимальное HP, которое он когда-либо
видел у этой машины. В Lua это приходит обычным числом, и отличить измеренное
значение от выведенного из максимума **невозможно**: у `wotb.players` есть
флаги `team_available`, `display_name_available` и `position_available`, но
флага для `max_health` нет. Поэтому доля HP, построенная на этом значении, —
правдоподобное число, а не наблюдение. Если показываете её игроку, пишите
рядом, откуда она взялась.

`wotb.events.TOPIC_*` содержит подтверждённые battle, vehicle, camera,
public-entity, projectile, local-shell и observed-RPC topics. `TYPE_*` содержит
22 значения client-event envelope. Для `vehicle.local.created`,
`vehicle.local.destroyed`, `gameplay.damage_received`, `sniper_entered` и
`sniper_exited` используется исходный envelope события, из которого topic был
безопасно выведен. Несуществующие источники вроде `damage_dealt` не имитируются.
