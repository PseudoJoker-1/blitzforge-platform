# Разрешения и двухуровневый забор

Native runtime видит один `wotbmod.lua_host` handle. Поэтому действует два
уровня:

1. внешний runtime grant host-пакета — абсолютный потолок;
2. внутренний grant скрипта — `manifest.permissions ∩ потолок host-а`.

Если host не может измерить свой grant через `wotbmod.permissions`, он не
запускает ни одного скрипта. Запрещённый вызов возвращает
`nil, "permission denied: <name>"` до разбора аргументов и до вызова ABI.

Host знает все 51 permission names V3. Release-пакет запрашивает 45 разрешений
до tier `REVIEWED` и намеренно не запрашивает шесть `UNSAFE`:

```text
native.memory
native.memory_patch
native.hook.address
native.hooks
render.native
bigworld.rpc.modify
```

Полный vocabulary по tier:

| Tier | Permission names |
| --- | --- |
| `SAFE` | `core`, `ui`, `ui.create`, `ui.modify.own`, `localization`, `audio`, `audio.custom`, `audio.events`, `resources`, `resources.mod`, `filesystem.mod_data`, `input`, `input.actions`, `settings`, `storage`, `events.public`, `entity.public.visible`, `hangar.scene`, `vehicle.local.cosmetic`, `camera.hangar`, `camera.replay`, `network.http.allowlisted`, `content` |
| `GAMEPLAY_TWEAK` | `gameplay.tweak.camera`, `gameplay.tweak.hud`, `gameplay.tweak.hangar`, `gameplay.tweak.replay`, `gameplay.tweak.cosmetic`, `gameplay.tweak.vehicle`, `gameplay.tweak.projectile_visual`, `gameplay.tweak.freecam` |
| `REVIEWED` | `battle.ui`, `battle.render.overlay`, `camera.battle.read`, `visible.projectile.events`, `game.entity.public`, `ui.modify.game`, `resources.overlay.game`, `hooks.symbol`, `render.callbacks`, `bigworld.observe`, `bigworld.rpc.observe`, `bigworld.rpc.metadata`, `client.leave_to_hangar`, `network.http` |
| `UNSAFE` | `native.memory`, `native.memory_patch`, `native.hook.address`, `native.hooks`, `render.native`, `bigworld.rpc.modify` |

Чтобы один script permission не мог «одолжить» другой grant общего native
handle, интерфейс с независимыми операционными правами закрыт консервативно:
скрипт должен запросить весь набор своей таблицы.

| Lua table(s) | Требуемый набор script permissions |
| --- | --- |
| `core`, `capabilities`, `permissions`, `handles`, `lifecycle`, `async`, `intermod`, `device`, `diagnostics`, `devtools` | `core` |
| `hooks` | `hooks.symbol` |
| `unsafe_native` | таблица отсутствует: единственная функция требует raw pointer |
| `events` | `events.public` |
| `ui` | `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui` |
| `settings` | `settings` |
| `storage` | `storage` |
| `input` | `input.actions` |
| `vfs` | `resources.mod`, `resources.overlay.game` |
| `resources`, `yaml`, `archive`, `loaders` | `resources.mod` |
| `dava` | `gameplay.tweak.cosmetic`; `create_texture`, `create_mesh` и `create_mesh_consumer` также требуют `resources.mod` |
| `http` | `network.http` |
| `render` | `battle.render.overlay`, `render.callbacks` |
| `render_native` | `render.native` (не входит в release ceiling) |
| `camera` | `camera.battle.read`, `camera.hangar`, `camera.replay`, `gameplay.tweak.camera` |
| `scene` | `hangar.scene`, `resources.mod` |
| `audio` | `audio.custom`, `audio.events` |
| `vehicle_visual` | `gameplay.tweak.vehicle`, `vehicle.local.cosmetic`, `game.entity.public`, `resources.mod`, `resources.overlay.game` |
| `gameplay_camera` | `gameplay.tweak.camera`, `gameplay.tweak.freecam` |
| `gameplay_hud` | `gameplay.tweak.hud`, `battle.ui` |
| `gameplay_hangar` | `gameplay.tweak.hangar`, `hangar.scene` |
| `gameplay_replay` | `gameplay.tweak.replay` |
| `entity_public` | `entity.public.visible`, `game.entity.public` |
| `bigworld_rpc` | `bigworld.rpc.observe`, `bigworld.observe`, `bigworld.rpc.metadata` |
| `projectile` | `gameplay.tweak.projectile_visual`, `visible.projectile.events` |
| `client` | `client.leave_to_hangar` |
| `manifest`, `catalog` | `content` |
| `content` | `content`, `resources.mod`, `resources.overlay.game` |

Это сознательно может дать более узкий доступ, чем отдельная native-операция.
Без per-call identity в замороженном ABI более широкий inner gate был бы
эскалацией до совокупных прав host-а.
