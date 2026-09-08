# `wotb.vehicle`

Фасад поверх raw-таблиц: [`wotb.vehicle_visual`](../reference/vehicle_visual.md), [`wotb.entity_public`](../reference/entity_public.md).

## Методы

- `wotb.vehicle.local_vehicle`
- `wotb.vehicle.visible`
- `wotb.vehicle.is_local`
- `wotb.vehicle.is_hangar`
- `wotb.vehicle.position`
- `wotb.vehicle.appearance_state`
- `wotb.vehicle.skin.register`
- `wotb.vehicle.skin.apply`
- `wotb.vehicle.skin.rollback`
- `wotb.vehicle.skin.state`
- `wotb.vehicle.skin.release`
- `wotb.vehicle.appearance.reset`
- `wotb.vehicle.set_skin`
- `wotb.vehicle.set_camouflage`

## Как пользоваться

### `wotb.vehicle`: своя машина и скины

Поверх `wotb.vehicle_visual`; всё client-only, как и сам интерфейс.

- `local_vehicle()` — handle своей машины (отдать через `wotb.handles.release`);
  `visible()` — записи `wotb.players.visible()`;
- `is_local(v)`, `is_hangar(v)`, `position(v)`, `appearance_state(v)` →
  `"unknown" | "loading" | "ready" | "destroyed"`, код вторым результатом;
- `skin.register(desc)` → pack (`asset_count` заполняется из `assets`),
  `skin.apply(pack, v)`, `skin.rollback(pack)`, `skin.state(pack)` →
  `{ applied, asset_count, mounted_asset_count, requires_model_reload,
  vehicle }` с булевыми флагами (`requires_model_reload == true` — «применено
  к ресурсам, но не к модели на экране»), `skin.release(pack)`;
- `appearance.reset(v)` — `restore_appearance`; `set_skin(v, uri)`,
  `set_camouflage(v, uri)`.

