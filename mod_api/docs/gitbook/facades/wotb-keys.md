# `wotb.keys`

Фасад поверх raw-таблиц: [`wotb.input`](../reference/input.md), [`wotb.handles`](../reference/handles.md).

## Методы

- `wotb.keys.bind`
- `wotb.keys.unbind`
- `wotb.keys.unbind_all`
- `wotb.keys.key_code`
- `wotb.keys.pressed`
- `wotb.keys.down`
- `wotb.keys.axis`
- `wotb.keys.on_pressed`
- `wotb.keys.on_released`
- `wotb.keys.off`
- `wotb.keys.bindings`
- `wotb.keys.conflicts`
- `wotb.keys.capture_begin`
- `wotb.keys.capture_end`

## Как пользоваться

### `wotb.keys`: горячие клавиши

Поверх `wotb.input`. Id действия в ABI — `"<id скрипта>.<id>"`, поэтому два
мода с `bind("toggle", ...)` не мешают друг другу, и никто не перехватит
чужое действие по имени.

- `bind(id, { key = "F7" | code = 0x76, modifiers = { ctrl = true }, contexts,
  display_name, description, axis })` → action; `unbind(id)`, `unbind_all()`;
  `key_code("F7")`;
- `pressed(id)` (нажата в этом кадре), `down(id)` (удерживается), `axis(id)`;
- `on_pressed(id, fn)`, `on_released(id, fn)` — `fn(id, value)`; `off(token)`;
- `bindings(id)`, `conflicts(id)`, `capture_begin([contexts])` /
  `capture_end()` → `{ device, code, modifiers, scale }`.

```lua
wotb.keys.bind("notify", { key = "F7" })
wotb.keys.on_pressed("notify", function() wotb.screen.notify("F7") end)
```

