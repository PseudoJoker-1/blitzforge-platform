# Фасады: короткий API поверх raw-таблиц

Raw-таблицы (`wotb.gameplay_hud`, `wotb.entity_public`, ...) повторяют C ABI
один в один: упакованные целые вместо цветов, `0/1` вместо `true/false`,
handle-ы и выходные параметры. Это правильная форма для биндинга и неудобная
для автора. Фасад — модуль на чистом Lua, который прячет эту форму за
короткими именами и отвечает по одной конвенции: значение при успехе,
`nil, err` при отказе (или `false, err` там, где `false` — законный ответ).
Фасад ничего не добавляет к возможностям клиента и не имеет собственных
permissions: каждый его вызов — это вызов raw-слота, и отказ клиента
(`E_NOT_SUPPORTED`, `E_PERMISSION_DENIED`, `E_WRONG_THREAD`) приходит автору
теми же словами с префиксом фасада.

**Правило имён.** Таблица фасада никогда не совпадает по имени с таблицей
интерфейса. Причина та же, что у `wotb.panel` и `wotb.config`: модуль-обёртка
обязан переживать отсутствие интерфейса и отвечать фразой «wotb.gameplay_hud
недоступен», а изнутри `wotb.gameplay_hud` это невозможно; к тому же
Lua-функция на таблице интерфейса сделала бы `wotb.available('gameplay_hud')`
истинным на клиенте, который интерфейс не публиковал. Поэтому `wotb.hud` живёт
рядом с `wotb.gameplay_hud`, а не внутри него. Единственное исключение —
расширение таблицы, которую host построил сам и только при публикации
(`wotb.ges`, прецедент `wotb.dava.run_on_main`): такое расширение никогда не
создаёт таблицу.

Все фасады разрешают raw-таблицу в момент вызова, а не при загрузке, и при
загрузке не подписываются ни на что. Проверены host-тестами
(`tests/lua_host_tests.cpp`, блок «the facade layer»); отдельного
live-подтверждения у них, как и у остальных convenience-модулей, пока нет.
Пример целиком на фасадах — `examples/lua_facade_tour`.

### `wotb.context`: `is_*`

`is_hangar()`, `is_battle()`, `is_training()`, `is_replay()`,
`is_text_input()`, `is_mod_screen()` — по одному `core.get_context` на вызов;
ответ `true`/`false` или `nil, err`, если маску прочитать нельзя. Спрашивайте
не чаще раза в кадр.

```lua
if wotb.context.is_battle() then panel:show() end
```

### `wotb.players`: короткие имена, честная поза, `details`

- `me()` — то же, что `local_player()` (`local` — зарезервированное слово Lua,
  поэтому не `local()`);
- `allies()` — свои без локального игрока; `our_team()` по-прежнему включает
  его;
- `enemies()` — то же, что `enemy_team()`: только уже видимые клиенту;
- `by_id(public_id)` — то же, что `find`;
- `each_visible(fn)` — `fn(record)` для каждой видимой машины в порядке
  `public_id`; `return false` из `fn` останавливает обход; ответ — число
  посещённых записей.

Флаги `position_available`/`direction_available` выводятся из данных, а не
задаются константой `false`: loader публикует позу только машине, у которой
проверил цепочку внешности, и опубликованное направление — единичный вектор;
у машины без источника в снимке остаётся нулевой вектор, а `(0, 0, 0)` — не
направление. Единичная длина направления и есть признак настоящей позы;
позиция следует за ним, потому что обе величины берутся из одной матрицы. То
же правило применяет `event.data.snapshot` у `wotbmod.entity.public.*`.

`details(record_or_id)` отдаёт пять полей ростера, которые loader публикует
только по имени (snapshot ABI заморожен): `clan_tag` (пустая строка без
клана), `account_id`, `kills` (и `frags` — второе имя того же числа),
`vehicle_name` (`нация:тег`, например `usa:A100_T49`) и
`vehicle_display_name` (локализованное, «T49»). Это пять вызовов
`get_public_property` на машину, поэтому они делаются по запросу, а не внутри
`snapshot()`. Поле, которое клиент отказался отдать, отсутствует и названо в
`unavailable` с причиной клиента — ноль вместо него не подставляется.

```lua
local ally = wotb.players.allies()[1]
if ally then
    local d, err = wotb.players.details(ally)
    if d then
        print(ally.display_name, d.clan_tag, d.vehicle_display_name, d.kills)
    else
        print(err)      -- "players.details: entity_public is unavailable"
    end
end
```

### `wotb.battle`: обработчики по коротким именам

`on(name, fn)`, `once(name, fn)`, `off(handle)`, `off_all()`, `events()` и по
одной функции `on_<name>(fn)` на имя. Имена и topic-и — одна таблица в
`lua_preludes.cpp`, она же документация:

| Имя | Topic |
| --- | --- |
| `enter` / `start` / `end` / `leave` | `wotbmod.battle.entered` / `.started` / `.ended` / `.left` |
| `shot` | `wotbmod.gameplay.shot_fired` |
| `hit` | `wotbmod.gameplay.shell_hit` |
| `reload` | `wotbmod.gameplay.reload_state_changed` |
| `ammo` | `wotbmod.gameplay.ammo_changed` |
| `damage` | `wotbmod.gameplay.damage_received` |
| `death` | `wotbmod.vehicle.local.destroyed` |
| `vehicle_destroyed` | `wotbmod.vehicle.killed` (victim/killer/assist из ростера арены) |
| `spotted` / `unspotted` | `wotbmod.vehicle.spotted` / `.unspotted` |
| `camera_changed` | `wotbmod.gameplay.camera_mode_changed` |
| `sniper_entered` / `sniper_exited` | `wotbmod.gameplay.sniper_entered` / `_exited` |

Обработчик получает `fn(data, event)`: сначала типизированный payload
(`event.data`; `nil`, если эта сборка его не публикует — фасад ничего не
синтезирует), затем само событие. Каждый вызов идёт под `pcall`: сломанный
обработчик попадает в `wotb.log` как `battle.on_<name> handler raised`, а
следующий обработчик всё равно выполняется. Пока `on()` не вызван, подписок
нет; `off()` возвращает подписку клиенту; `once` снимает себя перед первым
вызовом.

```lua
wotb.battle.on_vehicle_destroyed(function(kill)
    local victim = wotb.players.by_id(kill.victim_id)
    local killer = wotb.players.by_id(kill.killer_id)
    print((type(killer) == "table" and killer.display_name or "?") ..
          " уничтожил " ..
          (type(victim) == "table" and victim.display_name or "?"))
end)
local h, err = wotb.battle.on("teleport", print)
-- h == nil, err == "battle.on: 'teleport' is not a battle event; battle.events() lists them"
```

`is_active()` — в бою ли игрок сейчас (`BATTLE` или `TRAINING` в маске
контекста, без подписок); `state()` — таблица с `active`, `context`,
`tracking` и, пока включён `start()`, последним lifecycle-событием
(`lifecycle`) или причиной его отсутствия (`lifecycle_unavailable`).

### `wotb.ges`: `schema`, `on`/`off`, `observe`, `decode`

- `schema(type_name)` — `{ id, size, field_count, fields = { {name, offset,
  kind, size}, ... } }` до всякой подписки (слоты ABI `get_schema` и
  `schema_field`, ручной биндинг); `nil, err` для типа без схемы. Имя — в
  написании `types()`: `Avatar::CameraModeChanged`;
- `on(pattern, fn)` / `off(handle)` — те же `subscribe`/`unsubscribe`;
- `decode(ev)` — все поля схемы по именам одной таблицей, через `ev:field`;
  второй результат — таблица полей, которые runtime не может отдать в Lua
  (FastName, байты), с причинами; без схемы — `nil, err`;
- `observe(pattern, options, fn)` — `options.once` снимает подписку после
  первой доставки, `options.decode` вызывает `fn(fields, ev, unreadable)`
  вместо `fn(ev)`; `is_available()`.

Расширение ставится только если клиент опубликовал GES: на клиенте без него
таблицы `wotb.ges` нет вовсе, и `wotb.available('ges')` отвечает `false`.

```lua
wotb.ges.observe("Avatar::CameraModeChanged", { decode = true },
    function(fields) print("режим камеры", fields.mode) end)
```

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

### `wotb.hud`: штатный HUD по группам

Фасад над 34 слотами `wotb.gameplay_hud`, сгруппированными как в заголовке
`gameplay_hud_v1.h`; строится при первом обращении, как `wotb.panel`.

- `mode()` — `native`, когда интерфейс `wotb.gameplay_hud` опубликован
  (runtime публикует его только с backend-ом), иначе
  `unavailable, reason`; правду о каждом слоте говорит ответ самого вызова;
  `available()`; `status()` — какие из 34 слотов опубликованы;
- `reticle.set_texture/set_sniper_texture/set_color/set_size/
  set_reloading_indicator/set_dispersion_circle`;
- `damage_log.show/hide/set_enabled/set_position/set_max_entries/
  set_show_blocked/set_show_ricochet/set_show_module_damage/set_format/
  set_filter_own`;
- `session_stats.show/hide/set_enabled/set_fields`;
- `minimap.set_size/set_opacity/set_show_last_known/
  set_show_artillery_range/set_show_drawing/add_marker/remove_marker`;
- `sixth_sense.set_texture/set_sound/set_position/set_scale/set_delay`;
- `hit_indicator.set_style/set_color_hit/set_color_pen/set_color_ricochet/
  set_color_crit`;
- `reset()` — единственный сброс в ABI, общий для всех групп; `rgba(color)`.

Аргументы: цвет — `{r, g, b[, a]}` в `0..1` или целое `0xRRGGBBAA`; флаги —
boolean; якорь, стиль и поля счётчиков — имя (`"top-right"`, `"compact"`,
`{"damage", "shots"}`) или число из констант `wotb.gameplay_hud.*`. Неверный
аргумент отклоняется до вызова ABI; отказ клиента приходит с префиксом фасада.
На 11.20.0.887 клиент отказывает `minimap.set_show_last_known`,
`damage_log.set_show_blocked`, `hit_indicator.set_color_pen`, стилю
`directional` и полю `kills` — фасад этого не скрывает.

```lua
if wotb.hud.mode() == "native" then
    wotb.hud.reticle.set_color({ r = 1, g = 0.25, b = 1 })
    wotb.hud.damage_log.set_position("top-right")
    local ok, err = wotb.hud.minimap.set_show_last_known(true)
    -- ok == nil, err == "hud.minimap.set_show_last_known: ... not supported"
end
```

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

### `wotb.session`: кластер входа без перезапуска

Поверх `wotb.session_cluster` (API 1.1, 8 сентября 2026) и `wotb.events`.
Клиент сам умеет переключать кластер внутри региона
(`LoginManager::ChangeCluster`); фасад только называет это по-человечески.
Право `session.cluster.read` — на чтение и событие, `session.cluster.change`
(REVIEWED) — на переключение.

- `clusters()` — `{ {id, name, current, alive, allowed, ccu}, ... }` своего
  региона, отсортировано по `id` (`ccu == -1`, когда клиент не знает онлайн);
- `cluster()` — запись кластера, к которому клиент подключён сейчас;
- `change_cluster(4 | "EU_C4" | "auto")` → `true` или `nil, why`. Только из
  ангара; занято, пока предыдущее переключение в полёте и ещё 10 с после него;
  неизвестный id и мёртвый кластер отклоняются словами клиента. `"auto"` —
  штатный автовыбор клиента (ABI `-1`);
- `on_cluster_changed(fn)` → handle, `fn({ from, to, status })` со `status` из
  `queued | started | connected | failed`; `off_cluster_changed(handle)`,
  `off_all()`. `failed` означает и то, что флаг «ручной выбор» снят: следующий
  вход клиент сделает автовыбором.

Переключение действует на текущий сеанс; после перезапуска клиент снова
выбирает кластер сам.

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

### `wotb.sound`: проиграть файл, подменить звук

Поверх `wotb.audio`.

- `play(uri, { loop, spatial, volume, pitch, bus, priority, on_finished })` →
  handle (create + play); `stop(handle [, fade_seconds])`, `release(handle)`,
  (`fade_seconds` только 0: Windows-бэкенд 11.20 не умеет плавных затуханий — ненулевой fade у `stop` и ненулевая длительность у `fade_to` честно отвечают `NOT_SUPPORTED`, live 8 сентября 2026),
  `set_volume(handle, 0..1)`, `is_playing(handle)`, `on_finished(handle, fn)`;
- `replace(event_name, uri [, priority])` → token; `reset(event_name | token)`,
  `reset_all()`. Что клиент не умеет подменять, он говорит сам.

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

### `wotb.packages`: мост к `wotbmod.exe` (loader-private)

Не часть замороженного C ABI: библиотека Lua-хоста, которая запускает
`<игра>\wotbmod\wotbmod.exe` скрытым процессом и отдаёт результат. Так
каталог в игре (`examples/catalog`) ставит, удаляет и выключает моды тем же
кодом, что и команда `wotbmod`: подписи, леджер, отзыв релизов и `sync`
остаются в одном месте. Право `packages.manage` (REVIEWED); без него любой
вызов отвечает `nil, "permission denied: packages.manage"`.

| Имя | Значение |
| --- | --- |
| `packages.run(verb [, args])` | номер задачи либо `nil, message` |
| `packages.poll(job [, wait_ms])` | `{ running = true }`, либо `{ exit_code, stdout, stderr }` (один раз, после ответа задача забыта), либо `nil, message` |
| `packages.cancel(job)` | `true`/`false`; живой процесс завершается |
| `packages.executable()` | путь к `wotbmod.exe` либо `nil, message` |
| `packages.MAX_OUTPUT`, `packages.MAX_POLL_WAIT_MS` | 256 КБ на поток вывода, 5000 мс ожидания в `poll` |

`verb` — только из списка, и у каждого своё число аргументов:

| verb | args | что запускается |
| --- | --- | --- |
| `"list"` | — | `wotbmod list --json --game-root <игра>` |
| `"info"` | `{id}` | `wotbmod info <id> --json …` |
| `"uninstall"` | `{id}` | `wotbmod uninstall <id> --yes …` |
| `"enable"` / `"disable"` | `{id}` | `wotbmod enable|disable <id> …` (`[mods] <id>=1|0`) |
| `"launcher-open"` | `{"wotbmod://install/<id>@<v>?source=<url>"}` | `wotbmod launcher open <link>` — установка с каталога |
| `"restart-client"` | — | `wotbmod restart-client …`, отсоединённый процесс; задача сразу завершена |
| `"sync"` | — | `wotbmod sync --yes …` |

Аргумент — строка из `[A-Za-z0-9._@:/%?=-]` не длиннее 512 байт; всё другое —
`nil, "argument refused"`. Командную строку собирает мост, а не скрипт, и
одновременно у мода живёт одна задача (`nil, "busy"`). В окружение потомка
добавляются `WOTBMOD_LAUNCHER_YES=1` и `WOTBMOD_LAUNCHER_NO_PAUSE=1`, чтобы
launcher не ждал клавиши. `poll` без `wait_ms` не блокирует: опрашивайте из
`on_frame` или таймера. Задачи, не опрошенные к `on_disable`, завершаются
вместе со скриптом.

```lua
local job = assert(wotb.packages.run("list", {}))
wotb.timer.every(250, function(id)
  local state = wotb.packages.poll(job)
  if state and not state.running then
    wotb.timer.cancel(id)
    local rows = wotb.json.decode(state.stdout).packages
    print(#rows .. " packages installed, exit " .. state.exit_code)
  end
end)
```

Установленные Lua-папки хост читает один раз при включении, ресурсные
пакеты меняют файлы, поэтому после успешной команды нужен перезапуск
клиента — `restart-client` делает это сам (закрывает клиент, убирает маркер
сессии, чтобы не попасть в safe mode, запускает через Steam).

### `wotb.store`: типизированные значения

Поверх `wotb.storage` и `wotb.json`: `set(key, value)` кодирует таблицу,
`get(key [, default])` возвращает её обратно и второй результат `"stored"`
или `"default"`; `delete`, `has`, `flush`; `keys()` и `clear()` работают по
индексу, который модуль держит под зарезервированным ключом
`wotb.store.index` — ABI хранилище не перечисляет.

### `wotb.files`: файлы и ресурсы

Поверх `wotb.loaders`, `wotb.vfs`, `wotb.resources`, `wotb.yaml`.

- `read_text(uri [, max_bytes])`, `read_binary(uri [, max_bytes])`,
  `read_json(uri)`; лимит по умолчанию 4 MiB;
- `read_yaml(uri [, limits])` → документ с `doc:get("a.b[2].c")` (скаляр,
  либо `{ kind = "map" | "sequence", size = n }`) и `doc:release()`. Целиком
  в таблицу YAML не превращается: ABI не перечисляет ключи map;
- `load_texture(uri)`, `load_audio(uri)`, `load_scene(uri)` → resource
  (`resources.load` с ожидаемым типом), `info(resource)`, `release(resource)`;
- `exists(uri)` → `true` или `false, reason` (ответ `vfs.stat`), `stat(uri)`,
  `list(uri)`;
- `watch(uri, fn)` — `vfs.watch` плюс подписка на `wotbmod.vfs.invalidated`;
  `fn(event)` на каждую инвалидацию; `unwatch(watch)`.

### `wotb.panel`: контролы после `new()`

`label(opts)`, `button(opts)`, `image(opts)`, `scroll(opts)` добавляют контрол
в любой момент: смонтированная панель получает его сразу, несмонтированная —
при следующем `mount()`; ответ — имя для `panel:control(name)` (`3`, `"go"`,
`"image1"`, `"scroll1"`). `row({ x, y, gap, items = {...} })` и
`column({...})` раскладывают элементы (`kind = "label" | "button" | "image"`)
по оси; `row(index)` по-прежнему читает текст строки. `on("mounted" |
"unmounted" | "visibility" | "error", fn)` — слушатели панели; `destroy()`
размонтирует и забывает spec.
