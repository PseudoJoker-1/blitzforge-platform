# Фасады `wotb.*`

Короткий API для авторов: каждый модуль стоит поверх одной или нескольких raw-таблиц и никогда не совпадает с ними по имени. Начинайте с фасада; raw-таблица нужна, когда фасаду не хватает слота.

| Модуль | Поверх raw-таблиц |
| --- | --- |
| [`wotb.context`](wotb-context.md) | `core` |
| [`wotb.players`](wotb-players.md) | `entity_public` |
| [`wotb.battle`](wotb-battle.md) | `events, core` |
| [`wotb.session`](wotb-session.md) | `session_cluster, events` |
| [`wotb.ges`](wotb-ges.md) | `ges, events` |
| [`wotb.mod`](wotb-mod.md) | `lifecycle, permissions, capabilities` |
| [`wotb.hud`](wotb-hud.md) | `gameplay_hud` |
| [`wotb.screen`](wotb-screen.md) | `ui, ui_read, handles` |
| [`wotb.vehicle`](wotb-vehicle.md) | `vehicle_visual, entity_public` |
| [`wotb.shells`](wotb-shells.md) | `projectile, events` |
| [`wotb.view`](wotb-view.md) | `camera, gameplay_camera, camera_state, events` |
| [`wotb.sound`](wotb-sound.md) | `audio` |
| [`wotb.keys`](wotb-keys.md) | `input, handles` |
| [`wotb.store`](wotb-store.md) | `storage, json` |
| [`wotb.files`](wotb-files.md) | `loaders, vfs, resources, yaml, events` |
| [`wotb.panel`](wotb-panel.md) | `ui, context, events, handles` |
| [`wotb.log`](wotb-log.md) | `core, events, settings, storage` |
| [`wotb.json`](wotb-json.md) | `core, events, settings, storage` |
| [`wotb.timer`](wotb-timer.md) | `core, events, settings, storage` |
| [`wotb.config`](wotb-config.md) | `core, events, settings, storage` |
| [`wotb.available`](wotb-available.md) | `core, events, settings, storage` |
