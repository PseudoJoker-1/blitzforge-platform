# Контекст и видимость UI

Lua convenience API `wotb.context` работает поверх frozen
`wotb.core.get_context()` и не меняет публичный C ABI. Он публикует маски
`NONE`, `LOADING`, `HANGAR`, `BATTLE`, `REPLAY`, `TRAINING`, `RESULTS`,
`MOD_SCREEN`, `TEXT_INPUT`, `ALL` и функции:

- `current()` — текущая native context mask;
- `contains(value, flag)` — проверка отдельного флага;
- `should_show(allowed, blocked [, current])` — fail-closed решение о
  видимости;
- `apply_visibility(control, allowed, blocked [, current])` — применяет это
  решение к owned control и возвращает `true, visible`.

Battle-only root без каталога и текстового ввода:

```lua
local play = wotb.context.BATTLE + wotb.context.TRAINING +
    wotb.context.REPLAY
local blocked = wotb.context.MOD_SCREEN + wotb.context.TEXT_INPUT

local visible, err = wotb.context.should_show(play, blocked)
if visible == nil then
    print(err)
elseif visible then
    mount_ui()
else
    unmount_ui()
end
```

Для полного отсутствия перекрытий рекомендуемый lifecycle именно
`mount/unmount`, а не только `root:set_visible(false)`: вместе с controls
удаляются input subscriptions и active-screen handle.
