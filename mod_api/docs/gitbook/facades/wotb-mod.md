# `wotb.mod`

Фасад поверх raw-таблиц: [`wotb.lifecycle`](../reference/lifecycle.md), [`wotb.permissions`](../reference/permissions.md), [`wotb.capabilities`](../reference/capabilities.md).

## Методы

- `wotb.mod.id`
- `wotb.mod.permissions`
- `wotb.mod.has_permission`
- `wotb.mod.capability`
- `wotb.mod.capabilities`
- `wotb.mod.info`
- `wotb.mod.on_disable`
- `wotb.mod.off_disable`

## Как пользоваться

### `wotb.mod`: кто я и что мне можно

- `id()` — id скрипта (из `manifest.json`; в dev-папке — имя файла), C-часть;
- `permissions()` — имена прав, которыми скрипт реально владеет: запрос
  манифеста, пересечённый с измеренным потолком host-а;
- `has_permission(name)` — тот же ответ по одному имени; совпадает с тем, что
  скажет забор при вызове;
- `capabilities()` — все capability, которые зарегистрировал runtime, по
  именам (на 11.20.0.887 6 сентября 2026: `catalog`, `content`,
  `filesystem.mod_data`, `input.actions`, `manifest`, `resources`,
  `settings`), отсортированный массив записей со статусом словом и причиной;
- `capability(name)` — статус одной capability словом (`available`,
  `degraded`, `unavailable`, `client_mismatch`, `permission_denied`,
  `context_restricted`) плюс таблица с `reason` клиента. Имя — capability
  runtime-а, а не идентификатор интерфейса: `wotbmod.gameplay.hud` — это
  интерфейс, capability-записи у него нет, и ответ на него —
  `nil, "... capability is not registered"` (измерено на 11.20.0.887);
- `info()` — всё вместе, плюс `host` (id, состояние и tier Lua host-а из
  `lifecycle.get_info`);
- `on_disable(fn)` / `off_disable(handle)` — несколько обработчиков рядом с
  глобальным `on_disable`; выполняются новейший первым, до глобального, каждый
  под `pcall`; host вызывает диспетчер по имени (`__run_disable_handlers`),
  потому что глобалы он читает через rawget.

Не входит: `version()` (production scanner манифеста читает только `id`,
`entrypoint` и `permissions`), `on_reload` (host не отличает reload от
disable — reload начинается с disable), зависимости и настройки
(`wotb.config`).

```lua
print(wotb.mod.id(), table.concat(wotb.mod.permissions(), ", "))
if not wotb.mod.has_permission("gameplay.tweak.hud") then
    print("HUD недоступен: попросите gameplay.tweak.hud в манифесте")
end
wotb.mod.on_disable(function() wotb.storage.set_json("last", "{}") end)
```

