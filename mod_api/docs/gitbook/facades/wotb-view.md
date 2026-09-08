# `wotb.view`

Фасад поверх raw-таблиц: [`wotb.camera`](../reference/camera.md), [`wotb.gameplay_camera`](../reference/gameplay_camera.md), [`wotb.camera_state`](../reference/camera_state.md), [`wotb.events`](../reference/events.md).

## Методы

- `wotb.view.get`
- `wotb.view.set_fov`
- `wotb.view.fov`
- `wotb.view.reset`
- `wotb.view.project`
- `wotb.view.unproject`
- `wotb.view.transition`
- `wotb.view.shake`
- `wotb.view.on_changed`
- `wotb.view.off`

## Как пользоваться

### `wotb.view`: камера

Поверх `wotb.camera`, `wotb.gameplay_camera` и `wotb.camera_state`.

- `get()` — `mode` словом (`arcade`, `sniper`, ...), `transform`, `fov`,
  `near_plane`, `far_plane`, `observed` (только валидные поля); чтение, в
  котором клиент отказал, перечислено в `unavailable` с причиной;
- `set_fov(degrees [, "hangar" | "battle" | "sniper"])`, `fov()`, `reset()`;
- `project(point)` / `unproject(point)` — `world_to_screen` /
  `screen_to_world` через активную камеру;
- `transition({ target = { position, rotation, scale }, duration, easing,
  preserve_game_control })`, `shake({ amplitude, frequency, duration,
  falloff })`;
- `on_changed(fn)` — смена режима и вход/выход из снайперского; `off(handle)`.

Free, postmortem и cinematic режимы не предлагаются и не имитируются: клиент
не публикует способа в них войти.

