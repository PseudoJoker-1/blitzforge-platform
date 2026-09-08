# `wotb.ges`

Фасад поверх raw-таблиц: [`wotb.ges`](../reference/ges.md), [`wotb.events`](../reference/events.md).

## Методы

- `wotb.ges.types`
- `wotb.ges.subscribe`
- `wotb.ges.unsubscribe`
- `wotb.ges.publish`
- `wotb.ges.schema`
- `wotb.ges.on`
- `wotb.ges.off`
- `wotb.ges.observe`
- `wotb.ges.decode`
- `wotb.ges.is_available`

## Как пользоваться

### `wotb.ges`: `schema`, `on`/`off`, `observe`, `decode`

- `schema(type_name)` — `{ id, size, field_count, fields = { {name, offset,
  kind, size}, ... } }` до всякой подписки (слоты ABI `get_schema` и
  `schema_field`, ручной биндинг); `nil, err` для типа без схемы. Имя — в
  написании `types()`: `Avatar::CameraModeChanged`;
- `on(pattern, fn)` / `off(handle)` — те же `subscribe`/`unsubscribe`;
- `decode(ev)` — все поля схемы по именам одной таблицей, через `ev:field`;
  второй результат — таблица полей, которые runtime не может отдать в Lua
  (FastName, байты), с причинами; без схемы — `nil, err`;
- `observe(pattern, options, fn)` — `options.once` снимает подписку после
  первой доставки, `options.decode` вызывает `fn(fields, ev, unreadable)`
  вместо `fn(ev)`; `is_available()`.

Расширение ставится только если клиент опубликовал GES: на клиенте без него
таблицы `wotb.ges` нет вовсе, и `wotb.available('ges')` отвечает `false`.

```lua
wotb.ges.observe("Avatar::CameraModeChanged", { decode = true },
    function(fields) print("режим камеры", fields.mode) end)
```

