# `wotb.shells`

Фасад поверх raw-таблиц: [`wotb.projectile`](../reference/projectile.md), [`wotb.events`](../reference/events.md).

## Методы

- `wotb.shells.on`
- `wotb.shells.on_created`
- `wotb.shells.on_updated`
- `wotb.shells.on_impact`
- `wotb.shells.on_destroyed`
- `wotb.shells.on_local_shot`
- `wotb.shells.off`
- `wotb.shells.off_all`
- `wotb.shells.snapshot`
- `wotb.shells.visual`
- `wotb.shells.impact.show`
- `wotb.shells.impact.update`
- `wotb.shells.impact.hide`
- `wotb.shells.tracer.register`
- `wotb.shells.tracer.unregister`

## Как пользоваться

### `wotb.shells`: снаряды, попадания, трассеры

Наблюдение — через `wotb.events` (`created`, `updated`, `impact`,
`destroyed`, `local_shot`), визуал — через `wotb.projectile`. Траекторию,
физику снаряда и серверный результат фасад не трогает: таких слотов нет.

- `on(name, fn)`, `on_created/on_updated/on_impact/on_destroyed(fn)` —
  `fn(snapshot, data, event)`; `snapshot.fields` — булевы флаги
  `valid_fields` по именам полей (`origin`, `visible_direction`, ...), чтобы
  не читать поле, которое клиент не заполнил; `on_local_shot(fn)` —
  `fn(data, event)`; `off(handle)`, `off_all()`;
- `snapshot(projectile)`, `visual(projectile)`;
- `impact.show(desc)` → handle, `impact.update(handle, desc)`,
  `impact.hide(handle)`; `tracer.register(desc)` → handle,
  `tracer.unregister(handle)`.

