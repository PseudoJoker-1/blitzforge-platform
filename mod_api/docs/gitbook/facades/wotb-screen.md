# `wotb.screen`

Фасад поверх raw-таблиц: [`wotb.ui`](../reference/ui.md), [`wotb.ui_read`](../reference/ui_read.md), [`wotb.handles`](../reference/handles.md).

## Методы

- `wotb.screen.root`
- `wotb.screen.find`
- `wotb.screen.children`
- `wotb.screen.text`
- `wotb.screen.live_text`
- `wotb.screen.rect`
- `wotb.screen.visible`
- `wotb.screen.game_owned`
- `wotb.screen.info`
- `wotb.screen.set_text`
- `wotb.screen.set_visible`
- `wotb.screen.mount`
- `wotb.screen.unmount`
- `wotb.screen.notify`
- `wotb.screen.popup`

## Как пользоваться

### `wotb.screen`: дерево UI по коротким именам

Читающая половина UI поверх `wotb.ui` и `wotb.ui_read`; строящая половина —
`wotb.panel`, к которому делегирует `mount`.

- `root()` — handle активного экрана; он принадлежит скрипту, верните его через
  `wotb.handles.release`. Все остальные функции берут и отдают экран сами;
- `find(id_or_path)` — контрол под активным экраном по id или по пути с `/`
  (`control_find_by_id` / `control_find_by_path`); ответ клиента на
  отсутствующее имя — `nil, err` его словами;
- `children(control)`, `rect(control)`, `visible(control)`,
  `game_owned(control)`, `info(control)` — из `control_get_snapshot`, флаги —
  булевыми полями;
- `text(control)` — зеркало текста, который записал мод; `live_text(control)`
  — текст, который движок рисует сейчас (`ui_read.control_get_live_text`);
  для штатного контрола честен только второй;
- `set_text(control, text)`, `set_visible(control, on)` — перед вызовом
  проверяют, не штатный ли контрол: без `ui.modify.game` ответ
  `nil, "...game-owned; ui.modify.game is required..."` ещё до ABI;
- `mount(spec)` — `wotb.panel.new(spec)` плюс `mount()`, `unmount(panel)`;
- `notify(text [, seconds])` — штатный toast клиента; `popup({ title, message,
  accept, cancel, modal })` — штатный диалог (`cancel` делает его confirm).

```lua
local timer = wotb.screen.find("BattleScreen/TimerLabel")
if timer then print(wotb.screen.live_text(timer)) end
wotb.screen.notify("мод загружен", 2)
```

