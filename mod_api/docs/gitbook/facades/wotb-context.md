# `wotb.context`

Фасад поверх raw-таблиц: [`wotb.core`](../reference/core.md).

## Методы

- `wotb.context.current`
- `wotb.context.contains`
- `wotb.context.should_show`
- `wotb.context.apply_visibility`
- `wotb.context.is_hangar`
- `wotb.context.is_battle`
- `wotb.context.is_training`
- `wotb.context.is_replay`
- `wotb.context.is_text_input`
- `wotb.context.is_mod_screen`

## Как пользоваться

### `wotb.context`: `is_*`

`is_hangar()`, `is_battle()`, `is_training()`, `is_replay()`,
`is_text_input()`, `is_mod_screen()` — по одному `core.get_context` на вызов;
ответ `true`/`false` или `nil, err`, если маску прочитать нельзя. Спрашивайте
не чаще раза в кадр.

```lua
if wotb.context.is_battle() then panel:show() end
```

