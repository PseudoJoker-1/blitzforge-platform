# Текущий статус BlitzForge API (WotbMod V3)

Снимок состояния: 14 августа 2026 года, клиент `11.19.0.834`, SHA-256
`41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`,
binding pack `111900834`.

## Что означает статус

- `HOST_TESTED` / portable — backend проверен автоматическими тестами и не
  зависит от адресов игры. Он должен работать при соблюдении permission,
  context, thread и ownership contract.
- `BOUND` / `LIVE_TEST_PENDING` — адрес и hook подходят точному fingerprint,
  loader их установил, но конкретное поведение ещё должно быть подтверждено в
  запущенном клиенте. Наличие адреса не равно завершённой live-проверке.
- `SUPPORTED` — portable backend прошёл тесты либо native capability вручную
  подтверждена на этом fingerprint.
- `NOT_SUPPORTED` — function slot существует, но реального безопасного backend
  нет. Вызов честно возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`.

## Известные ограничения (11.20.0.887, состояние на 8 сентября 2026)

То, что разработчик мода встретит в первые дни. Это не баги SDK, а честные
ответы клиента; каждая строка проверена вживую и обходной путь указан.

| Ограничение | Как проявляется | Что делать |
|---|---|---|
| Текст штатных контролов не меняется | `ui.control_set_text` на game-owned контроле → `NOT_SUPPORTED`; читать текст можно (`screen.live_text`) | Подписи держать статическими в ресурсном пакете (yaml), состояние показывать `set_enabled`/`set_visible`; свой текст — в своей панели (`wotb.panel`) |
| Клик по штатной кнопке не приходит | `ui.event_subscribe(EVENT_CLICK)` на контроле, найденном от корня экрана, не срабатывает | Ловить `wotbmod.ui.input` (`action == 3`) и сверять с прямоугольником, собранным по `control_get_parent` (пример: `examples/cluster_picker`) |
| Диалогов клиента нет | `ui.confirm_show`/`dialog_show` → `NOT_SUPPORTED` | Подтверждение — своя панель `wotb.panel` с кнопками |
| Плавных затуханий звука нет | `stop(fade > 0)` и `fade_to(duration > 0)` → `NOT_SUPPORTED` | `stop(0)`, громкость менять шагами по таймеру |
| Сцена в бою не перечисляется | `scene.enumerate` в бою → `E_NOT_FOUND` («refused the size request»); в ангаре работает (303 узла) | Открытый дефект native backend; в бою полагаться на `entity.*`/`vehicle.state` |
| Показ/скрытие штатных контролов | работает через настоящий `SetVisibilityFlag`; дети, скрытые в yaml и показанные в рантайме, раскладываются заново | Раскладывать кнопки при сборке страницы (`visible: true` в yaml), переключать видимость контейнера |
| Страница настроек — не смена экрана | `wotbmod.ui.screen_changed` для неё не приходит | Искать свои контролы по отпусканию указателя (`wotbmod.ui.input`) |
| `impact.*` события | нужен снаряд, попавший в технику | В одиночной тренировочной комнате не проверяется |
| Хуки `BEFORE` и 20 целей без описания сигнатуры | `hooks.create_symbol` → `NOT_SUPPORTED` | Только `OBSERVE`/`AFTER` на 22 описанных целях; `wotbmod.hooks` — DEGRADED по замыслу |
| Трассеры от своих выстрелов | `tracer.requested/style` — `NOT_SUPPORTED` по замыслу; события `tracer.*` от одиночного выстрела в пустой комнате не пришли | Ждать боя с трассерами других игроков |
| Сырой `wotb.ui` открывается только всей семьёй прав | без `ui.modify.game`+`ui.create`+`ui.modify.own`+`battle.ui` любой вызов → `permission denied: ui.modify.game` | Просить все четыре, если нужен сырой доступ; фасадам `wotb.screen`/`wotb.panel` хватает своего набора |
| Именной grant `network:https://<host>` не открывает `wotb.http` в Lua | таблица `wotb.http` закрыта правом `network.http`; скрипт только с именным grant получает `permission denied: network.http` | Просить `network.http` (REVIEWED) и держать хосты в коде; именной grant остаётся правилом для native-модов |
| Мастер preview.N ставится только на «свою» сборку клиента | после патча Blitz: `Installed client executable does not match this release` | Ждать набор под новую сборку; порядок — `docs/CLIENT_PATCH_PLAYBOOK_RU.md` |


## Что уже добавлено

### Основа SDK и runtime

- стабильный C ABI V3 и C++17 wrapper;
- interface discovery, capabilities и точные коды ошибок;
- permission tiers, named grants и ограничения по игровому context/thread;
- owner-scoped handles с generation, retain/release и автоматическим cleanup;
- lifecycle, deferred unload/reload barriers, events, async-задачи и timers;
- hot reload native-модуля: `request_reload` только ставит флаг, а разборка
  (`DestroyMod`, снятие owned-ресурсов, `FreeLibrary`) и повторная загрузка
  через тот же package preflight выполняются в начале кадра, когда код модуля
  не на стеке; слот таблицы переиспользуется. Реализация от 23 августа 2026
  закоммичена 4 сентября и покрыта `v3_package_runtime_tests` (`HOST_TESTED`);
  живые строки `reload.*` validation-мода — `LIVE_TEST_PENDING`, прогоны до
  23 августа видели `hot_reload=0`, потому что этот код в их сборку не входил;
- `wotbmod.ui.read`: `control_get_live_text` читает живой текст штатного UI
  (`DAVA::UITextComponent` и `UIDynamicAtlasTextComponent`, 4 сентября 2026);
  живой PASS на 11.20.0.887 в ангаре 5 сентября 2026: 226 из 2479 контролов
  активного экрана отдали текст (имена игроков, счётчики, подписи), дамп в
  `LIVE_UI_TREE.jsonl`;
- `wotbmod.hooks`: режимы `OBSERVE` и `AFTER` реализованы на 22 опубликованных
  именах через наблюдателей внутри детуров лоадера (4 сентября 2026,
  `docs/HOOK_MODES_RU.md`); `HOST_TESTED`, живой статус `LIVE_TEST_PENDING` до
  прогона пробы `H`. Интерфейс остаётся `DEGRADED`: `BEFORE` не реализован,
  у 20 имён нет описанной сигнатуры, `AROUND`/`REPLACE` на целях лоадера дают
  `E_CONFLICT`;- diagnostics V2, crash marker, last error, counters и portable devtools;
- manifest, package preflight, catalog, content plan и mod lifecycle;
- settings, scoped storage, VFS, resources, HTTP и inter-mod API.

### Ресурсы и контент

- bounded text/binary resource loading и async reload с rollback;
- minimal YAML backend;
- DVPL type `0`, `1` и `2` с проверкой размеров и CRC;
- directory archive и ограниченный ZIP/`.wotbmod` reader;
- транзакционные game-resource overlays для `TEXTURE`, `UI` и `MODEL`;
- typed resource API для UI, scene, texture, audio и generic объектов;
- custom loose audio files и native sound-event path через Audio V2;
- vehicle visual/skin descriptors и resource-based model/material paths.

### Клиент, UI и рендер

- UI V3: active-screen inspection, поиск и обход существующего дерева
  `UIControl`, snapshot состояния/геометрии, изменение разрешённых штатных
  controls, owner-aware events и managed layout с transactional rollback;
- native input ingress через WndProc/DAVA queue, action polling, bounded event
  queues, coalescing и capture contracts;
- Scene API и managed D3D11 renderer для texture/material/sprite/text/line, а
  также managed `draw_mesh` через временное DAVA Scene attachment;
- render callback, backend/viewport/frame telemetry, resize и device lifecycle;
- guarded borrowed D3D11 device/context/swapchain для `UNSAFE` модов;
- Camera API: active camera, transform, FOV, clipping, projection, AFTER_GAME
  modifiers, native `ARCADE`/`SNIPER` и context-derived `HANGAR`/`REPLAY`;
- generic Win32/DXGI device info и штатный `LeaveToHangar`.

### Игровые данные и события

- основные client/game/battle/scene/UI/HUD/vehicle/gameplay/input/replay/audio
  event IDs и loader-owned dispatch;
- типизированный Lua `event.data` для 22 client events, public entity,
  projectile, local shell и metadata-only RPC lifecycle с сохранением raw
  `event.payload`;
- подтверждённые производные events: local vehicle created/destroyed,
  local damage received и вход/выход из sniper mode; `damage_dealt` не
  синтезируется без достоверного attacker source;
- Lua `wotb.players` с local/our/enemy/unknown views, причём enemy scope
  ограничен текущими visible entities, а неподтверждённые team/name/position
  помечаются unavailable;
- Lua `wotb.context` с named context masks и fail-closed UI visibility;
- установочные Lua-примеры Runtime UI Lab, Ally Tracker и Battle Telemetry:
  controls и input callbacks существуют только в боевом контексте и полностью
  удаляются при `MOD_SCREEN`/`TEXT_INPUT`;
- public entity registry только для local или уже видимых игроку сущностей;
- metadata-only входящих BigWorld RPC без payload и без injection;
- projectile V2 lifecycle `CREATED -> IN_FLIGHT -> IMPACTED -> DESTROYED` с
  provenance/valid-fields, owner/generation/timeout cleanup и native shot/hit
  ingress; managed DAVA Scene impact visuals с lifetime/max-instance limits;
- transactional vehicle skin packs: exact-path mesh/material/texture overlays,
  отдельные LOD entries, apply/state/rollback/release;
- gameplay camera, HUD, hangar и replay interface tables.
- `wotbmod.ges` — общая шина `GES::GameEventSystem` клиента `11.20.0.887`:
  один захваченный `SubscribeImpl` даёт указатель шины, а собственный
  listener в `ListenerList` наблюдает любой из 601 типов события по запросу;
  топики `wotbmod.ges.<Owner>.<Name>`, флаги `SIZE_KNOWN`/`SCHEMA_KNOWN`
  честно говорят, известны ли размер и схема payload (15 схем: живая
  `Avatar::CameraModeChanged` и 14 снятых с декомпилированных публикаторов
  4 сентября 2026, см. `API_V3_RU.md` §58.6), чтение полей только внутри
  доставки,
  публикация модом с `PUBLISH_ECHO`; Lua `wotb.ges`. Статус
  `LIVE_TEST_PENDING` до ручного PASS раздела «GES» validation-мода на точном
  fingerprint (spec `superpowers/specs/2026-09-03-ges-event-bus-design.md`).
- `wotbmod.session.cluster` (API 1.1, 8 сентября 2026) — кластер входа своего
  региона без перезапуска клиента: `enumerate` (список `EU_C0/C2/C3/C4` с
  флагами alive/allowed и меткой текущего), `get_current`, `change(id | -1)`.
  Backend: один захват `LoginManager*` на `LoginManager::OnHostChosen`
  (каждый вход), от него `services` → `Region`/`ConnectionManager`; сам
  переход делает штатный `LoginManager::ChangeCluster`, флаг «ручной выбор»
  (`+796`) ставится на `id >= 0` и снимается при `FAILED`. Исход приходит
  событием `wotbmod.session.cluster.changed` (`QUEUED/STARTED/CONNECTED/FAILED`,
  судья — возврат контекста `HANGAR`, таймаут 60 с); права
  `session.cluster.read` (SAFE) и `session.cluster.change` (REVIEWED); Lua
  `wotb.session_cluster` + фасад `wotb.session`. **LIVE PASS 8 сентября 2026**
  (11.20.0.887, ангар, скриптовые прогоны validation-мода, раздел «Session»):
  `enumerate` = `EU_C0/EU_C3/EU_C4` (три хоста на этом аккаунте), `current`
  совпал с `OnHostChosen`; `change(3)` из ангара → «Подключение…» → ангар на
  `EU_C3` через 12 с (`CONNECTED`); `change(-1)` через `Disconnect(12)` →
  автовыбор клиента, ангар через 15 с (`CONNECTED`). `ChangeCluster(-1)`
  напрямую из ангара кончается диалогом «Вы отключены от сервера» — так
  клиент не умеет, поэтому AUTO идёт через `HandleDisconnect(12) → TryNextHost`;
  таймаут 60 с честно дал `FAILED` и снял флаг ручного выбора. Строка
  `refuse_outside_hangar` (отказ из боя) — `LIVE_TEST_PENDING`. Spec:
  `superpowers/specs/2026-09-07-cluster-picker-design.md`.
  **Каталог модов в игре LIVE PASS 8 сентября 2026 (`blitzforge.catalog` 1.0.0 +
  `blitzforge.catalog.ui` 1.0.0):** иконка в левой колонке ангара открывает штатный экран
  (ресурсный пакет вклеивает `ModCatalogScreen` в `Hangar.yaml`, генератор
  `tools/build_catalog_ui.py`); вкладка «Каталог» показала три портальных мода с состоянием
  из `wotbmod list --json`, вкладка «Кастомные моды» — восемь своих; из экрана прошли
  `uninstall` и `launcher-open` (`blitzforge.night_mode` снят и поставлен заново с портала),
  `disable`/`enable` (`example.lua_facade_tour`, `mods.ini` переключился) и
  `restart-client` (клиент закрылся за 4 с, маркер сессии заархивирован, перезапуск через
  Steam, ангар через 40–64 с без safe mode; повторный перезапуск из клиента, запущенного
  Steam, тоже прошёл — помощник отсоединяется от job-объекта Steam). Бит `MOD_SCREEN`
  поднимается на время открытого экрана через флаг ввода `ModCatalogStateMarker`
  (`control_set_interactable`). Мост `wotb.packages` (loader-private, право
  `packages.manage`) и хост-тесты — `tests/lua_host_tests.cpp`. Что выяснилось: свойства
  `Anchor` в YAML должны быть на уровень глубже ключа, иначе контрол теряет привязку молча;
  `wotb.timer.now_ms()` — `nil`, пока нет живого таймера (дебаунс кликов — по кадрам);
  именной grant `network:https://…` не открывает `wotb.http` в Lua (см. таблицу выше).
  **Мод `blitzforge.cluster_picker` LIVE PASS 8 сентября 2026 (прогоны 23–25):** строка
  «AUTO | C0 | C3 | C4» в штатных настройках (ресурсный пакет `…cluster_picker.ui` 1.0.1,
  6 кнопок `StyledButton` со статическими подписями), текущий кластер выключен,
  отсутствующие спрятаны; клик → своя панель подтверждения (`wotb.panel`) →
  `change(4)` → `CONNECTED` через 12–13 с, ангар на `EU_C4`, штатный выход без дампов.
  Что при этом выяснилось про UI-слой на 11.20: (1) `ui_control_set_visible` теперь
  зовёт настоящий `UIControl::SetVisibilityFlag` (якорь `DefaultUiControlSetVisibilityFlag`,
  RVA `0x007CEDB0`) — прежняя запись бита `+0x4C` не давала ни каскада OnVisible, ни
  layout-dirty, и показанный так `StyledButton` оставался пустым и на позиции 0,0;
  (2) `ui.control_set_text` для game-owned контрола — `NOT_SUPPORTED` (путь «пересборка
  из YAML» есть только у созданных модом), поэтому подписи в ресурсном пакете статические;
  (3) `ui.event_subscribe(EVENT_CLICK)` на штатной кнопке, найденной `control_find_by_id`
  от корня экрана, не срабатывает: hit-test рантайма складывает позиции только обёрнутых
  им родителей, а обёртка от корня знает лишь корень — мод считает прямоугольник сам по
  `control_get_parent` и ловит `wotbmod.ui.input` (`action == 3`); (4) страница настроек
  строится внутри экрана ангара, `wotbmod.ui.screen_changed` для неё не приходит;
  (5) `ui.confirm_show`/`dialog_show` на 11.20 не опубликованы. Сырой `wotb.ui` открывается
  только всей UI-семьёй (`ui.modify.game` + `ui.create` + `ui.modify.own` + `battle.ui`).
  Судья `CONNECTED` теперь ждёт 500 мс стабильного `HANGAR` (прогон 23: обработчик мода,
  создавший тост в тот же кадр, что и разрушение старой сцены, попал на null-контрол в
  апдейте движка — 0xC0000005 в `UIControl::GetComponent`; прогоны 24–25 без окна и с ним
  чисты), а loader на время переключения раз в ~60 кадров взводит полный UI-probe:
  без этого ангар после `ChangeCluster` классифицировался через 62 с (прогон 22 →
  ложный `FAILED`).

### Lua-слой поверх замороженного ABI

- 540 констант V3-заголовков, опубликованных генератором в 35 таблицах `wotb.*`
  одной функцией `RegisterGeneratedConstants`. Генератор выписывает само C-имя
  и не разбирает ни одного C-выражения, поэтому значение вычисляет компилятор
  host-а и разойтись с заголовком оно не может. Правило имени задаётся отдельно
  для каждого интерфейса; столкновение двух имён на одной таблице и константа,
  затеняющая слот, останавливают генерацию ненулевым кодом, а не побеждают
  молча;
- 77 констант `base.h` не публикуются намеренно: коды `WotbModV3Result` (вся
  Lua-конвенция — `nil, message`, и числовой протокол ошибок конкурировал бы с
  ней), `WotbModV3HandleType` (скрипт не видит сырой handle),
  `WotbModV3GameContext` (уже опубликован как `wotb.context.*`) и `#define`
  пределы маршалинга host-а;
- следствие, которое пришлось задокументировать отдельно: таблица `wotb.<имя>`
  теперь создаётся безусловно у каждого интерфейса с константами, поэтому
  проверка `if wotb.settings then` больше не отвечает на вопрос «опубликован ли
  интерфейс». Корректная проверка — наличие функции в таблице; разница между
  «опубликован», «разрешён» и «поддержан» описана в
  [`LUA_MODS_RU.md`](LUA_MODS_RU.md);
- пять convenience-модулей на чистом Lua (`loader/lua/lua_preludes.cpp`):
  `wotb.log` — уровни поверх `wotb.core.log`; `wotb.json` — encode/decode без
  рекурсии, с различением пустого объекта и пустого массива и с sentinel-ом для
  JSON `null`, поэтому успешный decode никогда не возвращает Lua `nil`;
  `wotb.timer` — `after`/`every` поверх события кадра, с ленивой подпиской,
  которая исчезает вместе с последним живым таймером, и без догоняющих серий
  вызовов после паузы; `wotb.battle` — снимок боя, где ненаблюдавшееся поле
  равно `nil` и
  перечислено в `unavailable` с причиной, а не подменяется нулём; `wotb.config`
  — схема с типами, в которой default никогда не затирает сохранённое значение,
  а нечитаемое хранилище является ошибкой, а не пустым документом. Собственных
  permissions у модулей нет и быть не может: они вызывают только то, что и так
  доступно скрипту, поэтому забор, ownership registry и teardown работают без
  изменений. Статус модулей — `HOST_TESTED`: отдельного подтверждения в
  запущенном клиенте у них пока нет;
- facade-слой 6 сентября 2026 (`loader/lua/lua_preludes.cpp`,
  `lua_bindings.cpp`, `lua_bind_ges.cpp`): `wotb.context.is_*`;
  `wotb.players` — `me`/`allies`/`enemies`/`by_id`/`each_visible`,
  `details` (тег клана, account id, фраги, имя танка по
  `get_public_property`) и флаги позы, выведенные из единичной длины
  направления, а не заданные константой; `wotb.battle.on_*`/`once`/`off`/
  `is_active`/`state`; `wotb.ges.schema`/`on`/`off`/`observe`/`decode`;
  `wotb.mod` — `id`/`permissions`/`has_permission`/`capability`/`info`/
  `on_disable` (диспетчер обработчиков вызывается host-ом из
  `LuaScript::Deactivate` до глобального `on_disable`); `wotb.hud` над 34
  слотами `gameplay_hud`. Правило имён: таблица фасада никогда не совпадает с
  raw-таблицей интерфейса. Host-тесты: 938 проверок
  (`tests/build_lua_host_tests.cmd`), пример `examples/lua_facade_tour`.
  Live 6 сентября 2026, случайный бой 15:15–15:19 на 11.20.0.887: пример на
  фасадах писал уничтожения с никами через `players.by_id`, 47 выстрелов
  через `battle.on_shot`, первый режим камеры через `ges.observe` с decode,
  `hud.reticle.set_color` → OK и честный отказ
  `hud.minimap.set_show_last_known` → `not supported by this client build`;
  `wotb.mod.capabilities()` перечислил capability рантайма. Найдено попутно:
  правка прицела на экране загрузки (до появления HUD) оставила цель оверлея
  прицела уничтоженной, и шаг `reticle.texture` validation-мода ответил
  `E_OBJECT_DESTROYED` — HUD-правки надо делать, когда HUD уже существует
  (пример перенесён на 45 с после `battle.started`), а backend должен
  переоткрывать цель оверлея после пересоздания HUD;
- фасады второго среза, 6 сентября 2026: `wotb.screen` (над `ui`/`ui_read`, с
  проверкой game-owned до вызова), `wotb.vehicle` (над `vehicle_visual`),
  `wotb.shells` (над `projectile` и событиями, `valid_fields` булевыми
  флагами), `wotb.view` (над `camera`/`gameplay_camera`/`camera_state`),
  `wotb.sound` (над `audio`), `wotb.keys` (над `input`, действия с
  namespace скрипта), `wotb.store` (над `storage`+`json`), `wotb.files` (над
  `loaders`/`vfs`/`resources`/`yaml`), динамические контролы `wotb.panel`.
  Mock публикует эти десять интерфейсов только для их тестов; host-тесты
  1032 проверки. Live 6 сентября 2026, случайный бой 16:22–16:26 на
  11.20.0.887 (`examples/lua_facade_tour` под правами своего манифеста):
  `players.each_visible` дал 7 машин с ником, кланом, именем танка и позой,
  `view.get` → `arcade fov=54.0`, `hud.reticle.set_color` → OK, девять
  уничтожений через `wotbmod.vehicle.killed`, `store.set_json` последнего боя
  → OK, ни одного `permission denied`. Найдено: `ui.toast_show` на этом
  клиенте отвечает `not supported`, поэтому `screen.notify` теперь рисует
  собственную строку-оверлей через `wotb.panel` и снимает её таймером
  (второе возвращаемое значение говорит, что именно нарисовано: `"toast"`
  или `"overlay"`; host-тест есть, live-подтверждение оверлея — в следующем
  прогоне); `view.get` в ангаре честно отвечает `incompatible` (боевой
  камеры нет), `hud.minimap.set_show_last_known` → `not supported` как и у
  validation-мода;
- SDK для авторов, 6 сентября 2026: `docs/QUICKSTART_RU.md` (15 минут до
  своего мода), `wotbmod new --type lua --template hello|tour|panel|battle|
  hud|vehicle` (шаблон — это тот же проверенный пример из `examples/`),
  пять facade-first примеров (`lua_facade_tour`, `lua_facade_panel`,
  `lua_facade_battle`, `lua_hud_tweaks`, `lua_skin_switcher`), каждый
  прогоняется host-тестами под правами собственного манифеста,
  `docs/API_REFERENCE_RU.md` генерируется `tools/generate_api_reference.py`
  из заголовков (`--check` в `tests/test_sdk_docs.py`), схемы
  `schemas/wotbmod-lua-manifest-v1.schema.json` и
  `schemas/wotbmod-config-schema-v1.schema.json`, сниппеты
  `sdk/vscode/wotbmod.code-snippets`;
- пакеты, зависимости и релизы (этап 3), 6 сентября 2026, CLI 1.2:
  `tools/wotbmod_packages.py` — `install`/`uninstall`/`update`/`rollback`/
  `list`/`verify`/`keygen`/`release`/`publish`/`policy`; `tools/wotbmod_trust.py`
  — проверка и выпуск подписей ECDSA P-256/SHA-256 на чистом Python (вектор
  CNG-подписанта и RFC 6979 A.2.5 в тестах). `install` показывает SHA-256,
  права по tier и план зависимостей до копирования, ставит required
  dependency раньше зависимого (из папки-каталога или HTTP-каталога),
  отказывает несовместимому клиенту, испорченному архиву (CRC), `invalid`/
  `revoked` подписи, unsigned/untrusted при политике и обеим сторонам
  `incompatibilities`; версия неизменяема в `release`, `publish` и `install`;
  `rollback` возвращает байты и `.sig` из `mods/cache/install_backups/`.
  Loader читает `[policy] require_trusted_signature` из `mods.ini`
  (`src/wotb_mod_runtime.cpp`, оба пути preflight). Тесты:
  `tests/test_wotbmod_packages.py` (19), запускаются из
  `tests/build_wotbmod_cli_tests.cmd`. Описание для новичка — раздел
  «Установка, зависимости и релизы» в `docs/WOTBMOD_PACKAGE_FORMAT_RU.md`;
- портал модов (этап 4), 6 сентября 2026: `portal/wotbmod_portal.py` +
  `portal/portal_templates.py`, только stdlib (`http.server`, `sqlite3`).
  Роли user/developer/verified developer/moderator/administrator; страницы
  каталога (поиск, фильтры, сортировка), мода (SHA-256, права по tier,
  зависимости, совместимость, история версий, оценки/отзывы/жалобы),
  профиля, кабинета (загрузка версий, API-токены), модерации, установки
  (`wotbmod://install/<id>@<версия>?source=…` + ручной путь); API
  `/api/v1/*` (auth с подтверждением почты и сбросом пароля, publish
  multipart с Bearer-токеном, compat/rating/comments/reports, статистика) и
  старая форма `/api/mods` для каталога в ангаре. Загрузка проверяется как в
  loader (`load_package` + подпись), версия неизменяема (409), «проверен»
  ставит только модератор, релизы проверенных подписывает ключ портала.
  `wotbmod install/update --catalog http://…/api/v1` работают без отдельного
  кода. Тест `tests/test_wotbmod_portal.py` проходит весь путь на loopback.
  Описание — `docs/PORTAL_RU.md`;
- мост браузер → клиент (этап 5), 6 сентября 2026: `tools/wotbmod_launcher.py`
  регистрирует `wotbmod://` в HKCU и обрабатывает
  `wotbmod://install/<id>@<версия>?source=<каталог>` и
  `wotbmod://uninstall/<id>`: строгий разбор (только каталожный релиз по
  http(s), никаких путей и DLL), точная версия из `index.json`, хеш
  скачанного файла против записи, затем `wotbmod install` со всеми его
  проверками и подтверждением; повторный клик идемпотентен. Lua-пакеты
  (`.wotbmod` с Lua-manifest) теперь проходят через `release`/`publish`/
  `install`/`update`/`rollback`/`uninstall`: распаковка в `mods/lua/<id>/`,
  архив и `.sig` в `mods/cache/lua_packages/`. `tools/wotbmod.py` отдаёт
  общий reader/writer store-архивов (`_read_store_archive`,
  `_write_store_archive`). Тесты `tests/test_wotbmod_launcher.py`; описание
  — `docs/LAUNCHER_RU.md`;
- стабилизация (этап 6), 6 сентября 2026: live 17:26–17:40 на 11.20.0.887 —
  `screen.notify` через оверлей подтверждён (`F7: shown via overlay`);
  закрытие клиента посреди случайного боя (WM_CLOSE) с loader-ом и без
  него не дало crash dump — падение при выходе на этом build не
  воспроизводится (старые дампы 10:03 — до сегодняшних изменений); политика
  `[policy] require_trusted_signature` в первом прогоне не блокировала
  (второй проход preflight сбрасывал бит) — исправлено (`SignaturePolicyFlags`
  на всех четырёх точках) и подтверждено live 17:47 с пересобранным
  loader-ом: `package preflight discovered=5 ready=1 blocked=4` — подписанный
  доверенным ключом Lua host загрузился, три неподписанных sample-пакета и
  каталог validation-мода заблокированы до `LoadLibrary` с причиной
  `trusted detached package signature is required`; HUD-обёртки
  стоковых контролов теперь проверяются снимком перед использованием и
  переоткрываются после пересоздания HUD (`Resolve` в
  `src/v3/client_services.cpp`); `build.cmd` снова зелёный (заглушка
  `HudMainThreadTick` в standalone-тестах); CLI: `wotbmod quarantine`,
  `wotbmod info` с причиной блокировки из лога loader-а, тесты `AuditTests`.
  Известное расхождение, не из этого дня: `tools/verify_rc1_contract.py`
  сравнивает с `rc1/contract_snapshot.json` от 4 сентября, где
  `wotbmod.gameplay.hud` ещё `UNAVAILABLE` (с 5 сентября — `DEGRADED`), плюс
  дайджесты enum и публичных заголовков — перезаморозка снимка остаётся
  решением владельца контракта;
- сообщество и безопасность (этап 7), 6 сентября 2026: `wotbmod scan`
  (`tools/wotbmod_scan.py`, PE-парсер и Lua-правила, тесты
  `tests/test_wotbmod_scan.py`), гейт в `release`/`install`/портале, метки
  риска tier-ов, `PERMISSION ESCALATION` при обновлении, audit API,
  takedown, `wotbmod report-crash` + `/compat`, документы
  `CONTRIBUTING_RU.md`, `REVIEW_POLICY_RU.md`, `SECURITY_POLICY_RU.md`.

### Production hardening

- общий per-mod callback budget `100..8000 us`, предел callbacks/frame,
  coalescing для помеченных high-frequency событий и profiler counters;
- exact-build validation каждого native binding до установки hook и атомарный
  отчёт `mods/cache/binding_pack_validation.json`; несовпадение fail-closed;
- detached package signatures `ECDSA P-256/SHA-256`, strict sidecar parser и
  filesystem trust store `<key_id>.p256`; untrusted/invalid package не проходит
  политику обязательной подписи;
- отдельно подписанный `WOTBMOD-REVOCATIONS-V1` список отозванных ключей и
  конкретных `package_id@version`; malformed/invalid revocation data fail-closed;
- crash-loop safe mode с attribution последнего mod/callback/native binding,
  portable-only recovery, повторным crash counter и auto-disable виновника;
- privacy-safe validation bundle export: токены, bearer credentials и e-mail
  вычищаются, chat/RPC payload lines не экспортируются.
- public-preview release bundle: exact-client SHA gate, detached-подпись Lua
  host-а, Windows CNG signing key, payload post-copy verification, защита чужих
  proxy/loader/key, update state, автоматический rollback и hash-safe uninstall;
  installer lifecycle проверяется отдельным adversarial PowerShell harness.
- SEH-безопасное резервирование VFS provider slot. Скан `g_mounts` под
  `g_vfs_mutex`, который выполняли обе точки входа mount, вынесен в
  `ReserveVfsProviderSlot` (`../src/v3/data_services.cpp`) и защищён
  `__try`/`__finally` вместо `std::lock_guard`. Всё собирается с `/EHsc`, под
  которым MSVC не выполняет C++ деструкторы при раскрутке к `__except`, а этот
  участок достижим из кода мода, работающего именно под таким обработчиком:
  мод, вызывающий `set_mod_enabled(self, true)` из собственного callback,
  приходит сюда через `MountV3PackageDuringEnable`. Access violation в скане
  оставлял обычный нерекурсивный `std::mutex` захваченным навсегда потоком,
  который продолжал работать, и видимым симптомом было зависание через
  несколько минут на постороннем потоке без единой ссылки на настоящую причину.
  Termination handler при такой раскрутке выполняется и этим закрывает путь,
  который деструктор не закрывает.

  Регрессия проверяется отдельным набором
  `../tests/v3_vfs_mount_fault_tests.cpp`, который вызывается из `build.cmd`.
  Он измеряет саму предпосылку — что `/EHsc` действительно пропускает
  деструктор `lock_guard` при раскрутке к `__except`, — затем
  поднимает настоящий access violation внутри настоящей защищённой секции
  и проверяет, что после него `g_vfs_mutex` по-прежнему захватывается, что
  mount/unmount через публичный путь продолжают работать и что обычный ранний
  выход из секции тоже освобождает lock. Оставшиеся десять критических секций
  `g_vfs_mutex` относятся к тому же классу и не исправлены.

### Воспроизводимый live-прогон и evidence

- checked-in harness вместо обвязки, которая раньше писалась заново под каждый
  прогон и жила только в transcript агента:
  [`../tools/run_live_evidence.ps1`](../tools/run_live_evidence.ps1) выполняет
  preflight и гигиену, снимает baselines до запуска, переносит stale
  `runtime_session.marker` вместо удаления, запускает клиент со staged-копией
  реплея, делает DPI-корректные скриншоты по расписанию моментов, завершает
  клиент только `WM_CLOSE` и никогда force-kill, и режет loader-лог строго по
  байтовому смещению от baseline;
- [`../tools/judge_live_slice.ps1`](../tools/judge_live_slice.ps1) выносит
  машинный вердикт по пяти воротам: нет плохих строк, процесс завершился, marker
  снят, число crash dump не изменилось и найден каждый обязательный позитивный
  шаблон. Пятые ворота существуют потому, что первые четыре проходят и на
  полностью тихом логе, в котором вообще ничего не загрузилось. Exit code `1`
  («прогон грязный») и `2` («судья не отработал») различаются намеренно;
- процедура целиком, честная область действия и построчный список того, что
  сверено с исходниками, а что взято из transcript и не подтверждено, —
  в [`LIVE_EVIDENCE_RU.md`](LIVE_EVIDENCE_RU.md). Чистый вердикт означает ровно
  одно: срез лога чист и заявленные строки найдены. Он не означает, что
  capability работает корректно.

## V3 RC1 freeze

RC1 snapshot перезаморожен 6 сентября 2026 как API 1.0 (см.
[`API_FREEZE_RU.md`](API_FREEZE_RU.md)): 58 публичных файлов, 46 interface IDs,
54 permission name, 51 x86 ABI table size и 3 sample-пакета;
`tools/verify_rc1_contract.py` печатает текущий контракт как
`files=58 interfaces=46 permissions=54 samples=3`, а
`v3_rc1_contract_probe` — как `51 x86 sizes frozen`. Относительно снимка
4 сентября изменились три поля: статус `wotbmod.gameplay.hud`
(UNAVAILABLE → DEGRADED после стоковых HUD-контролов 5 сентября), дайджест
enum-значений и дайджест публичных заголовков; ABI-таблицы и их размеры не
менялись.
Свежий полный `build.cmd` завершился
с exit code `0`: package loader `70/0`, Lua host `759/0`, full API
`1661 assertions`, installer lifecycle `27/0` и общий финальный banner PASS.
Сам clean build не запускает игру; сразу после него те же артефакты были
установлены и проверены отдельным live replay/battle прогоном.

Замороженный контракт: [`../API_V3_RC1_FREEZE.md`](../API_V3_RC1_FREEZE.md).
Package signature/revocation contract:
[`PACKAGE_SIGNATURE_RC1.md`](PACKAGE_SIGNATURE_RC1.md). Ручной live checklist:
[`../MANUAL_LIVE_VALIDATION_RU.md`](../MANUAL_LIVE_VALIDATION_RU.md).

## Что подтверждено текущим клиентом

Последний live-лог подтвердил exact-fingerprint binding pack и установку всех
`12/12` gameplay/native hooks. Также фактически наблюдались D3D11 backend,
managed text draw, device lifecycle и доставка callback через loader queue.

Public-preview Lua host загрузился из доверенно подписанного package как
`0.1.0-preview.1`: `signature-status=2`, `package-loaded=1`, `failed=0`.
Battle Telemetry получала настоящие события, Ally Tracker показал шесть
союзников, а provider подтвердил семь public own-team vehicles вместе с local
player. Панели не исчезли между двумя кадрами через пять секунд и корректно
снялись после выхода из battle. F8 открыл Runtime UI Lab и разблокировал мышь;
лог и Win32 `GetCursorInfo(CURSOR_FLAGS=1)` дали независимое подтверждение.

Audio, часть RPC/projectile/tracer и `LeaveToHangar` нельзя считать полностью
проверенными только по этому replay: соответствующие действия в нём не были
выполнены. Их текущий честный статус — `LIVE_TEST_PENDING`. Public-entity/team,
camera-mode, shell/reload/damage и battle lifecycle в этом запуске фактически
наблюдались.

## Исправление просадки FPS

Причина была не просто в количестве callback. Validation-мод получал
`wotbmod.frame.update` примерно с частотой кадра и внутри каждого события:

1. дописывал JSONL trace;
2. вызывал `FlushFileBuffers`;
3. полностью переписывал два snapshot JSON с write-through;
4. постоянно держал render callback и рисовал каждую строку дважды.

Теперь:

- wildcard `wotbmod.*` заменён 11 точечными системными подписками;
- frame update дополнительно отбрасывается защитной проверкой;
- snapshot помечается dirty и сохраняется максимум один раз за maintenance
  interval;
- append-only trace не делает durable flush на каждую запись;
- validation-панель скрыта по умолчанию, callback создаётся только по `F5` и
  снимается при закрытии;
- текст панели рисуется одним draw call на строку вместо shadow + foreground.

При скрытой панели validation-мод больше не должен создавать render callback
или per-frame disk I/O. Старые `LIVE_EVENT_TRACE.jsonl` могут оставаться
большими, но новая сборка не должна продолжать записывать туда frame events.

## Устранение фризов ангара 5 сентября 2026

Симптомы на клиенте 11.20.0.887: загрузка ангара около 170 с, кадр раз в
2-3 с при повороте танка или клике, Windows фиксировал AppHangTransient.
Причина измерена самим бэкендом: `VirtualQuery` внутри процесса клиента
стоит около 1,1 мс за вызов (строка `address checks` в логе загрузчика при
старте). `IsExecutableAddress` и `ValidateObject` в
`src/wotb_mod_dava_resources.cpp` делали по такому вызову на каждый обёрнутый
или обойдённый UIControl, поэтому полный обход дерева ангара (около 2500
контролов) занимал около 2 с, а загрузчик запускал его каждые 15 кадров из
`PollUiScreenChanged`; получение активного экрана и освобождение обёртки
стоили ещё 3-5 мс на каждый кадр.

Что сделано (коммит `79f73d1`):

- адреса внутри образа игры проверяются по таблице исполняемых секций PE без
  системного вызова; адреса кучи по-прежнему уходят в `VirtualQuery`;
- опрос экрана не взводит полный пересчёт классификации своими же обёртками
  (guard самоактивности), полный пересчёт идёт не чаще раза в 60 кадров на
  одном экране, отсутствие маркера каталога запоминается на 300 кадров;
- строка `frame hitch` в логе загрузчика содержит профиль по хукам за окно
  между отчётами (engine-frame, main pump, scene-draw, `File::Create`,
  ui-input, GES deliveries, present hook с разбивкой ui-poll); пишется не
  чаще раза в секунду при кадре дольше 200 мс и только после 600 кадров.

Результат живого прогона 5 сентября (обычный `mods.ini`, 6 модов): ангар за
25-30 с, ни одной строки `frame hitch` при повороте танка и кликах, хук
загрузчика меньше 0,1 мс на кадр. Оставшиеся затыки около 1 с при смене танка
и открытии экранов принадлежат движку: в их окнах `File::Create` даёт тысячи
вызовов, а доля present hook меньше 10 мс в секунду.

Вылет при повороте танка воспроизвёлся один раз и только во время PROBE
Lua-модуля skin_atelier; без него не воспроизводился. Модуль убран из
`mods/lua`.

## HUD на штатных контролах 5 сентября 2026

`wotbmod.gameplay.hud` перестал быть заглушкой: пять ключей таблицы
`kHudStockControls` (`minimap` -> `Minimap`, `sixth_sense` -> `Lamp`,
`reticle` -> `GunAim`, `damagelog` -> `RibbonsContainer`, `session_stats` ->
`DamageStatistics`) заполнены по дампу дерева тренировочного боя и ведут
настоящие контролы `BattleScreen` через UI host bridge. Живой прогон в
тренировочном бою (строка `hud.stock_controls`, 16:38): `minimap_set_size`,
`sixth_sense_set_scale`, `reticle_set_size`, `damagelog_set_enabled` (скрыть и
показать), `session_stats_set_enabled` (скрыть и показать), `reset` вернули
`OK`. `minimap_set_opacity` остаётся `NOT_SUPPORTED`: прозрачность есть только
у контролов, построенных модом из YAML. Статус интерфейса теперь `DEGRADED`, а
не `UNAVAILABLE`. Повторный прогон 19:20 на финальной сборке дал строке чистый
`WOTBMOD_V3_OK`, ответ по прозрачности помечен как ожидаемый. После горячей
перезагрузки в конце прогона строка снова показывает `LIVE_TEST_PENDING`:
статус нативных строк не переживает перезагрузку модуля, доказательство
остаётся в строке `hud.stock_controls judged` лога загрузчика.

Ночью того же дня (коммит abe68f7) бэкенд получили ещё 25 слотов: цвет
фона игровых контролов через `UIControlBackground` (+0x60), покадровый тик на
главном потоке, оверлеи-картинки внутри штатных контролов, звук и время
свечения лампы, лимит ленты, поля счётчиков, прозрачность миникарты через
альфу поддерева. Строка `hud.stock_controls` проверяет 27 шагов, `hud.stock_reset`
откатывает. Живой прогон 22:34 в тренировочном бою: строка целиком `OK`, 22 шага
`OK` и 5 ожидаемых отказов, `hud.stock_reset` `OK`. Три уточнения по ходу: сегменты
разброса прицела появляются и исчезают динамически (тик переобходит детей), фон у
скрытых вспышек возникает с первым попаданием (цвет ждёт его), а большинство
спрайтов HUD рисует `UIDynamicAtlasImageComponent` (цвет по `+0x3C`), который
бэкенд теперь тоже красит. Подробности в `API_V3_RU.md`, раздел 36.

По дороге закрыты три дефекта, из-за которых строка не могла пройти:
проверка доступа искала незарегистрированную capability `gameplay.tweak.hud`
(теперь уровень доверия, контекст и именное право проверяются раздельно, как у
камеры); validation-мод не запрашивал это право в манифесте; строки с горячей
перезагрузкой шли в прогоне раньше UI-строк, и судьи на главном потоке
выполнялись уже во время выгрузки модуля (теперь перезагрузка последней, а
вердикт HUD дублируется в лог загрузчика).

## Состояние техники и боя 6 сентября 2026

Публичный интерфейс `wotbmod.entity.public` отдаёт по каждой видимой машине
snapshot: public id, тип, флаги visible/local, команда, здоровье и максимум
здоровья, публичный тип. Эти поля нативный ingress заполняет достоверно
(команда проверяется по vtable RVA `0x033056A4`, поле `+0xB0`; entity id `+0x1C`;
здоровье `+0xB8`; максимум `+0x11C`).

**Позиция и направление своей машины закрыты (ночной прогон 6 сентября).**
Источник: покадровое обновление позы в `PlayerController` (RVA `0x011B8C10`,
вызывается из `BattleController::Update` перед публикацией пустого тика
`Avatar::TurretAndCameraPositionsChanged`; `this` берётся из виртуального
слота `+0x174` владельца). Функция переписывает блок по `this+0x5C` под
спинлоком `+224`; по `+108` в нём лежит мировая матрица 4x4 внешности
управляемой машины (`DAVA::Matrix4`, строки 0..2 оси корпуса, строка 3 его
начало). Лоадер ставит AFTER-хук, копирует блок под тем же спинлоком
(ограниченный спин, освобождение при любом сбое) и не чаще 10 Гц, только при
сдвиге от 5 см или повороте от 1°, публикует через штатный ingress
`position` = начало матрицы и `direction` = единичная строка forward. Мир
Y-up (строка 1 матрицы вертикальна, forward = строка 2); выбор строки лоадер
делает по самой матрице один раз на машину. Публикация идёт только пока своя
машина жива; после гибели остаётся последняя поза. Первые три снимка блока на
машину пишутся в лог как трасса (`pose #N`).

Проверка движением 01:19–01:20 в случайном бою: при удержании W начало
матрицы ушло на 30 м вдоль строки 2 (`z` −225 → −195 при неизменном `x`), при
удержании D строка 2 повернулась с (0, 0, 1) на (1, 0, 0.03) без сдвига
начала, второй W сдвинул `x` на 20 м вдоль нового направления. Авто-свип
01:19:33: `vehicle.state` `OK`, у своей машины `position` и `direction`
отвечают `OK`, у союзников по-прежнему `NOT_SUPPORTED` (у них источника нет,
контракт «позиция только видимых» не нарушен). Остальные поля блока: `+60`
и `+88` точки камеры в Z-up-порядке DAVA (x, z, высота), `+172`/`+176` углы
камеры в радианах, `+104` всегда ноль. Отображаемое имя по-прежнему **не**
заполняется, запрос честно возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`.

Состояние боя мод получает событиями GES: `Avatar::PlayerDied`,
`PlayerRespawned`, `PeriodBattleStarted`/`PeriodBattleFinished`,
`BattleStatusUpdated`, `ArenaFreeze`, `DirectShootReady`, `Session::RoundFinished`
(15 схем в `ges_schemas.cpp`), плюс производные `wotbmod.battle.*` и
`wotbmod.vehicle.*` от лоадера. Отдельного query-интерфейса состояния боя нет,
это событийная модель.

Строка валидации `vehicle.state` (BigWorld Entity) в бою перечисляет видимые
машины, пишет `LIVE_VEHICLE_STATE.jsonl` со всеми полями snapshot, включая
числа position/direction, и по каждому полю фиксирует доступность через
`get_public_property` (OK против NOT_SUPPORTED).

## Чужие машины, имена игроков и фраги 6 сентября 2026

**Позиции чужих машин закрыты.** Тот же кадровый хук читает для каждой
публичной машины (союзники и засвеченные враги) мировую матрицу по цепочке
объекта внешности `entity+56 → +8 → +1344` (entity это то, что возвращает
виртуальный слот 1 VehicleGameLogic, он же `LiveVehicleRecord::entity`).
Цепочка доверяется только после самопроверки: для своей машины она обязана
дать то же начало, что и блок позы (совпало с точностью до сантиметра в
02:38 и 02:48), иначе публикация чужих поз не включается. Публикация 10 Гц,
только при сдвиге от 5 см или повороте от 1°, только пока машина жива и
публична; при потере засвета запись удаляется штатным путём.

**Имена, клан, account id, техника и фраги закрыты.** Источник: `ClientArena`
(`Classes/Battle/ClientArena/ClientArena.cpp`), который принимает серверный
protobuf `UpdateArena` и держит по каждой машине 184-байтный
`ArenaVehicleInfo`. Три AFTER-хука (якоря в `anchor_rvas.h`):
`0x01235E10` создаёт/обновляет `ArenaVehicleInfo` из `VehicleInfo` (список
машин на загрузке и vehicle_added), `0x01238420` применяет
`VehicleStatisticsInfo` (kills по vehicle_id), `0x0122DF30` обрабатывает
`VehicleKilled` (victim/killer/assist/reason). Раскладка снята с живых дампов:
`ArenaVehicleInfo` +24 id, +28 модель игрока (ник по +8), +36 дескриптор
техники (по +52 запись типа: тег по +8, ключ локализации
`#france_vehicles:...` по +32), +40 команда, +48 int64 account id, +56
std::string тег клана, +80 int64 id клана, +96 жив, +124 kills (−100 до
первого пакета статистики); `VehicleInfo` +16 id, +20 команда, +24 указатель
на std::string ника, +40 int64 account id, +48 тег клана. Лоадер держит
ростер по vehicle_id и вливает его в публичный snapshot: `display_name` =
ник, а `clan_tag`, `account_id`, `kills`/`frags`, `vehicle_name`
(«нация:тег») отдаются через `get_public_property` по имени (snapshot ABI не
трогали, в рантайме это блок extras рядом со snapshot с теми же битами
валидности). Проверено в 02:48: все 14 игроков боя с верными никами, кланами,
account id и командами (в том числе свой), `vehicle.state` `OK`, `named=7`;
фраги обновлялись по ходу боя (kills=1, 2), события гибели декодированы
(victim/killer/assist). Строка `vehicle.state` пишет значения этих свойств в
`LIVE_VEHICLE_STATE.jsonl`.

**Локализованное название и событие гибели (6 сентября, утро).**
`vehicle_display_name` берётся из таблиц локализации самого клиента:
`DAVA::Singleton<LocalizationSystem>::instance` (RVA `0x0421653C`) держит
список файлов строк по `+80`, у каждого файла `std::map<string,string>` по
`+52` (узел 64 байта, ключ по `+16`, значение по `+40`); лоадер обходит дерево
сам, без вызова игрового кода, по ключу вида `#usa_vehicles:A100_T49` (ключи в
`Data/Strings/ru.yaml` хранятся с `#`). Событие `wotbmod.vehicle.killed`
публикуется из хука `ClientArena::VehicleKilled` через штатную очередь
клиентских событий (`WOTBMOD_EVENT_VEHICLE_KILLED`, payload
`WotbModV3VehicleKillEventData`: жертва, убийца, ассист, причина, взрыв
боеукладки), источник `WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED`, есть в Lua как
`TOPIC_VEHICLE_KILLED`. Проверено в случайном бою 6 сентября 09:58–10:02:
`vehicle_display_name` дал «T49», «ELC AMX-901», «IS-2», «WZ-122 TM» (таблица
`ru`, 30327 строк), мод валидации получил четыре события гибели с жертвой,
убийцей и ассистом. Попутно выяснилось, что скрипт сборки компилирует только
отсутствующие объекты: после смены заголовков нужен чистый прогон, иначе
рантайм остаётся со старой маской источников и новый топик отвечает «нет
издателя».

Пакет `UpdateArena.player_name` (case 13, обработчик `0x0122B9A0`: payload
`+12` указатель на std::string нового ника, `+16` vehicle id) тоже
перехвачен: ростер обновляет ник, а публичная запись переиздаётся, так что
`display_name` и подписки на него видят смену имени по ходу боя. Live
6 сентября 15:05: хук встал (`gameplay hooks installed mask=0x3FEFF (17/18)`),
в проверочном бою пакетов переименования не пришло (строк `arena player name`
нет), остальные строки ростера и `vehicle.state` без изменений.

**Не сделано:** карьерная статистика игроков (по решению 6 сентября: только
данные клиента, без Wargaming API), история боёв (отложена).

## Падение клиента при выходе (замечено 6 сентября 2026)

При закрытии клиента 11.20.0.887 посреди боя в 10:03 (закрыл пользователь)
WER записал APPCRASH `c0000005` по `wotblitz.exe+0x6E02EB`: деструктор
`sub_8FD2E0` снимает себя со списка синглтона `dword_4598D58`
(`sub_AD3BA0()`), который к этому моменту уже обнулён, и `sub_AE02C0`
читает `[this+0x98]` по нулю. Это порядок разрушения статики при выходе, а не
код новых хуков: в дампе `wotblitz.exe(1).29080.dmp` в верхних кадрах стека
главного потока только сам клиент. Тот же почерк (ноль по `+0x9C`, другая
сборка, `+0x6C6DCD`) есть в двух дампах 08:22 и 08:31 из другого окружения со
старым лоадером (`wotblitz.exe` размером `0x44B3000`, дампы Codex). Штатное
закрытие через `WM_CLOSE` из ангара за ночь ни разу не падало. Нужно
проверить, падает ли выход без лоадера вообще, и если нет, искать, что
лоадер меняет в порядке teardown (снятие хуков в DllMain, живые подписки).

## Заморозка API 1.0 и сквозная цепочка 6 сентября 2026

Что здесь проверялось. «Заморозка» — обещание, что перечисленные имена,
структуры и форматы больше не меняются без новой major-версии; «сквозная
цепочка» — один и тот же мод проходит весь путь игрока: публикация на
портале, кнопка «Установить в игру», установка в клиент, реальный бой,
откат. Всё ниже сделано на клиенте `11.20.0.887`, локальном портале
(`127.0.0.1:8080`) и loader-е от 17:47 6 сентября.

### Заморозка

RC1-снимок перезаморожен (`python tools/verify_rc1_contract.py --emit`):
`files=58 interfaces=46 permissions=54 samples=3`, полный `build.cmd`
зелёный, включая контрактный тест и тест установщика (27 проверок).
Что именно заморожено, что нет и как вносить изменения — в
[`API_FREEZE_RU.md`](API_FREEZE_RU.md).

### Сквозная цепочка

1. **Портал.** Разработчик `demo@example.org` выпустил
   `example.lua_facade_tour` версии `1.0.1`: `wotbmod release
   --sign-with-key`, затем `wotbmod publish --to
   http://127.0.0.1:8080/api/v1 --token`. Портал проверил пакет, прогнал
   `wotbmod scan` и переподписал релиз своим ключом
   `blitzforge-portal-2026` (в каталоге релиз помечен `signed_by: portal`).
2. **Кнопка «Установить в игру».** Ссылка
   `wotbmod://install/example.lua_facade_tour@1.0.1?source=http://127.0.0.1:8080/api/v1`
   открылась через зарегистрированный launcher
   (`wotbmod_launcher.py register`); launcher вызвал `wotbmod install`,
   в `%LOCALAPPDATA%\WotbMod\launcher.log` — `result 0`. Папка
   `mods/lua/example.lua_facade_tour` стала `1.0.1`, архив и подпись легли в
   `mods/cache/lua_packages`, запись — в `mods/cache/wotbmod_installs.json`.
   Для автоматизации launcher понимает переменные окружения
   `WOTBMOD_LAUNCHER_YES=1` (не спрашивать подтверждение) и
   `WOTBMOD_LAUNCHER_NO_PAUSE=1` (не ждать нажатия клавиши).
3. **Клиент и бой.** Случайный бой 19:45: тур работал на установленной
   из портала версии (ростер 7 машин, камера arcade, прицел, сохранённый
   «последний бой»); validation sweep дал `hud.stock_controls` — все строки
   `OK`, `hud.stock_reset` — `OK`, `vehicle.state` — `OK`; новых crash-дампов
   в `%LOCALAPPDATA%\CrashDumps` не появилось.
4. **Откат.** После выхода из клиента `wotbmod rollback
   example.lua_facade_tour` вернул `1.0.0`, резервная копия `1.0.1`
   осталась в `mods/cache/install_backups`. В `wotbmod info` восстановленная
   папка показана как `unsigned`: исходная `1.0.0` была скопирована вручную,
   а не установлена из пакета, поэтому подписи у неё никогда не было — это
   честное состояние, а не ошибка отката.

### Пять интерфейсов 16 августа на `11.20.0.887`

Источник — `mods/logs/native_validation_live.log` (строки панели
`wotbmod.native_validation`). Строки, которые панель судит сама, судятся
через 40 с после начала боя; строки «по событию» засчитываются, когда
событие реально пришло.

| Интерфейс | Строка панели | Итог 6 сентября 2026 |
|---|---|---|
| `wotbmod.ui.read` | `ui.readonly_inspector`, `ui.live_text` | `PASS` (19:46) |
| `wotbmod.camera.state` | `camera.mode_change` | `PASS` по событию `wotbmod.gameplay.camera_mode_changed` (19:46:56, sniper → arcade после Shift) |
| `wotbmod.camera.state` | `camera.active` | `FAIL` в этом прогоне: sweep сработал до первого переключения камеры (`mode=0`); тот же тест дал `PASS` 5 сентября 22:09 (`mode=2`), когда переключение было раньше sweep-а |
| `wotbmod.audio.intercept` | `audio.native_sound_event` | `PASS` (19:46) |
| `wotbmod.audio.intercept` | `audio.custom_file` | `FAIL`: автотест «WAV create/preload/play/stop/destroy» не прошёл; код ошибки в живой лог не пишется, поэтому шаг не известен. Единственный `PASS` этой строки (21 августа, `11.19.0.834`) был ручным вердиктом F6 |
| `wotbmod.scene.enumerate` | `scene.readonly_inspector` | нестабильно: `PASS` 16:13 («303 узла»), `FAIL` 19:46 (`E_NOT_FOUND`, «native scene enumeration refused the size request»); такие же чередования с 4 сентября |
| `wotbmod.tracer` | `tracer.created`, `tracer.visible`, `tracer.destroyed` | `PASS` 11:04 по событиям `visible_tracer_created/destroyed` (handle `h:e400000000000021`) |
| `wotbmod.tracer` | `tracer.requested`, `tracer.style` | `NOT_SUPPORTED` по замыслу |

Вывод: `ui.read` и `tracer` подтверждены на текущем клиенте; `camera.state`
подтверждён по событию, а строка `camera.active` зависит от порядка
действий; `audio.custom_file` и `scene.enumerate` — открытые дефекты native
backend на `11.20.0.887` (не SDK и не портала), их статус остаётся
`LIVE_TEST_PENDING`.

### Финальный прогон 8 сентября 2026 (`11.20.0.887`, скриптовые прогоны без человека)

Ангар: четыре auto-sweep (`ONLY=all SKIP=reload`, 46 безопасных строк), отдельный
прогон `ONLY=reload`; бой: тренировочная комната (создание через список комнат,
`Готов`, `В бой`), sweep через 25 с после `BATTLE`, перед ним Shift (снайперский
режим) и один выстрел. Матрицы: `LIVE_CAPABILITY_MATRIX.json` из `mods/data/
native_validation_mod` после каждого прогона (копии в scratchpad сессии).

| Интерфейс | Строка панели | Итог 8 сентября 2026 |
|---|---|---|
| `wotbmod.ui.read` | `ui.live_text` | `PASS` (00:23): visited=2380, with_text=285, без единого `CALLBACK_FAULT`. **Дефект найден и починен:** `IsReadableRange` в DAVA-бэкенде считал «внутри образа» любой адрес выше начала образа (не было `start < g_imageEnd`, размер уходил в переполнение) — обход текста разыменовывал мусор, 32 first-chance AV за sweep, `with_text=1` |
| `wotbmod.camera.state` | `camera.active`, `camera.mode_change` | `PASS` (01:16, тренировочный бой): `mode=3` после Shift перед sweep-ом. В ангаре честно `E_NOT_FOUND` (камеры боя нет); **манифест validation-мода не просил `camera.hangar`** — в ангаре строка отвечала `PERMISSION_DENIED`, право добавлено |
| `wotbmod.audio.intercept` | `audio.custom_file` | `PASS` (00:23). Два дефекта теста: URI `mod://wotbmod.native_validation/…` (id пакета — не VFS-authority мода, `PERMISSION_DENIED`; теперь `mod://self/`) и `stop(0.05)` — хост отвечает `NOT_SUPPORTED` на любой ненулевой fade (плавных затуханий у Windows-бэкенда нет; то же для `fade_to` с длительностью). Шаг отказа теперь пишется в результат строки |
| `wotbmod.scene.enumerate` | `scene.readonly_inspector`, `material.readonly_inspector` | ангар: `PASS` во всех четырёх прогонах (303 узла, `Sky_Sphere.sc2`); **бой: `E_NOT_FOUND`** («refused the size request») в трёх боевых прогонах — активная сцена боя не захватывается трекером (`Scene::Activate first hit … track=0`), открытый дефект native backend |
| `wotbmod.entity.*`, `wotbmod.rpc`, `wotbmod.gameplay.hud` | `vehicle.state`, `entity.enumerate_visible`, `entity.lifecycle`, `rpc.incoming_metadata`, `hud.stock_controls`, `hud.stock_reset` | `PASS` (00:51, 01:16, тренировочный бой): `visible=1 allies=1 named=1 with_health=1`, `metadata=1`, все стоковые HUD-контролы `OK` (ожидаемые `NOT_SUPPORTED` на месте), reset `OK` |
| `wotbmod.ges` | `ges.observe_all`, `ges.event_count`, `ges.publish_echo` | `PASS`: ангар 88 событий/66 типов; бой 974/78, `publish=OK (main thread) echo_seen=1` |
| `wotbmod.session.cluster` | `enumerate`, `current`, `refuse_outside_hangar`, `change` | `PASS`: `change(4) in BATTLE -> E_CONFLICT` (01:16); `change` — отдельные прогоны 02:14/02:29/04:22–04:50 (см. выше) |
| `wotbmod.projectile` | `projectile.created/updated/destroyed`, `shell.ingress` | `PASS` (01:16): выстрел из тренировочной комнаты → `shot ingress: PublishNewProjectile -> 0`, событие `projectile.destroyed` (`h:e400000000000001`, state 4). `impact.*` — нужна цель, венью-ограничение |
| `wotbmod.reload` | `reload.hangar`, `reload.cleanup` | `PASS` (00:25): «module reloaded live (load cycle 147 -> 148)»; `reload.active_subscription`/`rollback` в auto-прогоне не запускаются (нужен ручной сценарий) |
| `wotbmod.tracer` | `tracer.created/visible/destroyed` | в тренировочной комнате событий трассеров не пришло (один выстрел в землю); `PASS` 6 сентября 11:04 остаётся единственным живым свидетельством |
| `wotbmod.tracer` | `tracer.requested`, `tracer.style` | `NOT_SUPPORTED` по замыслу |

Ловушки прогонов, чтобы не платить за них снова: клиент, закрытый в бою,
при следующем запуске переподключается в тот же бой (sweep срабатывает до
любых действий, а по концу боя остаётся экран результатов — его закрывает
клик по стрелке справа (1880,540)); после серии быстрых перезапусков вход
может зависнуть на «Подключение…» на весь прогон — пауза пару минут; список
тренировочных комнат — иконка левой панели (55,890) при 1920×1080,
«Создать комнату» (1755,50) и подтверждение (960,1020), в комнате `Готов`
(1785,1020) и `В бой` (1790,50).

### Повторная проба HUD после починки Resolve

Утром 6 сентября правка HUD на экране загрузки (событие `battle.start`
приходит раньше, чем HUD построен) оставляла обёртку стокового контрола
«мёртвой»: все последующие вызовы отвечали `E_OBJECT_DESTROYED`. Починка —
`Resolve` заново ищет контрол, если снимок устарел. Проверка вечером
(бой 20:07, Lua-мод `test.early_hud` с `wotb.battle.on("start", …)`):
`reticle.set_color` на экране загрузки — `OK`, повтор через 20 с — `OK`,
validation sweep на 40-й секунде — `hud.stock_controls` все строки `OK`
(включая `reticle.*`), `hud.stock_reset` — `OK`, `wotb.hud.reset()` на 60-й
секунде — `OK`; ответов `E_OBJECT_DESTROYED` в логе — ноль. Пробный мод
после проверки удалён из клиента.

### Один набор для игроков

`tools/build_public_preview.ps1 -SkipBuild -Version 0.1.0-preview.15` теперь
собирает всё, что нужно игроку, в один архив
`build/public_preview/WotbMod-API-0.1.0-preview.15-win32.zip` (SHA-256
`9d353283ac46601ec151ddea66d730d0f8433801660aebe61265a4e23db38ac7`): loader
(`version.dll`, `wotb_mod_loader.dll`), подписанный Lua host, ключ набора
`blitzforge-preview-2026` и публичный ключ портала `blitzforge-portal-2026`,
папку `wotbmod\` с CLI и launcher-ом. Отпечаток клиента в скрипте —
`11.20.0.887` / `4813544d…`, binding pack `112000887`; `release-manifest.json`
получил секцию `tools` (пути CLI/launcher, схема `wotbmod`, список ключей
портала). `install.ps1 -RegisterLauncher` после копирования регистрирует
`wotbmod://` для текущего пользователя.

## Портал в сети и пакеты ресурсов (вечер 6 сентября 2026)

Портал развёрнут по адресу https://blitz-forge.org (VPS Ubuntu, systemd-служба
`wotbmod-portal` за Caddy с сертификатом Let's Encrypt, домен в Cloudflare
в режиме «только DNS», письма через Resend с адреса `noreply@blitz-forge.org`).
Ключ подписи портала тот же `blitzforge-portal-2026`, что лежит в наборе
для игроков, поэтому релизы, которые портал переподписал, у игроков
доверенные без дополнительных шагов.

Новый тип пакета `"type": "resource"` (см.
[`WOTBMOD_PACKAGE_FORMAT_RU.md`](WOTBMOD_PACKAGE_FORMAT_RU.md), раздел
«Пакеты ресурсов»): подписанный `.wotbmod`, который заменяет файлы в
`Data/` игры. `wotbmod install` сверяет хеш стокового файла, сохраняет
оригинал, пишет новый DVPL (чистый Python, `tools/wotbmod_dvpl.py`) и
чистит кэш шейдеров; `uninstall` возвращает оригинал, `rollback` меняет
версии. Регрессия — шесть тестов в `tests/test_wotbmod_packages.py`
(26/26 зелёные), статическая проверка портала помечает такие релизы
меткой «файлы игры».

Первый мод в каталоге — «Ночной режим» `blitzforge.night_mode 1.3.0`
(`examples/night_mode`, те же два шейдера, что ставил старый установщик
как `night-mode 1.2.4`). Проверено на этом клиенте: `wotbmod release`
с ключом разработчика `pseud-2026`, `wotbmod publish` на портал, портал
переподписал релиз, кнопка «Установить в игру» через `wotbmod://` поставила
его с результатом 0, файлы в `Data/Materials/Shaders` совпали с патчем
(проверка распаковкой настоящим `lz4`), `uninstall` вернул стоковые файлы.

Набор для игроков `0.1.0-preview.16` (loader, Lua host, CLI c
`wotbmod_dvpl.py`, launcher, ключи набора и портала) лежит на странице
https://blitz-forge.org/download с размером, датой и SHA-256 и инструкцией
по установке; сборка прошла тест установщика (27 проверок).

## Установщик для игроков (ночь 6–7 сентября 2026)

Набор игрока теперь ставится обычным мастером Windows
`BlitzForge-Setup-<версия>.exe` (Inno Setup 6, скрипт
`release/public_preview/setup.iss`), как у любой программы: приветствие,
папка игры, компоненты, готовность, установка, завершение, запись в
«Приложениях» Windows для удаления. Что мастер делает сам:

- находит папку игры через реестр Steam и `libraryfolders.vdf`, а на шаге
  «Папка игры» сверяет SHA-256 `wotblitz.exe` с поддерживаемой сборкой и
  отказывается ставиться на другую; не даёт продолжить, пока игра запущена;
- на шаге «Готово к установке» показывает зависимости и докачивает
  недостающие: Visual C++ Runtime x86 (загрузчик импортирует
  `MSVCP140`/`VCRUNTIME140`), а при выбранном компоненте — Python 3.13 для
  разработчиков (URL и SHA-256 зашиты в скрипт);
- распаковывает набор во временную папку и запускает тот же `install.ps1`
  с ключами `-RegisterLauncher` и `-AddToPath` по выбранным компонентам;
  ошибка скрипта прерывает мастер до записи чего-либо, текст ошибки
  показывается из UTF-8-лога;
- удаление из «Приложений» запускает `uninstall.ps1` (возвращает исходные
  файлы игры, убирает запись из PATH пользователя и регистрацию
  `wotbmod://`).

Инструменты игрока собраны в один `wotbmod\wotbmod.exe` (PyInstaller,
`tools/wotbmod_entry.py`): это и CLI (`wotbmod install/list/uninstall/...`,
папка игры определяется по расположению exe), и обработчик ссылок
`wotbmod://` (`wotbmod.exe launcher open "%1"`). Python игроку не нужен;
`wotbmod.cmd` рядом выбирает exe, а без него — Python-скрипты.

Проверено на этом клиенте тихими прогонами (`/VERYSILENT /CURRENTUSER`):
обновление поверх, удаление (файлы игры возвращены, PATH и схема сняты,
запись в «Приложениях» исчезла), чистая установка заново; в окне мастера
пройдены все страницы (скриншоты в scratchpad). Набор `0.1.0-preview.17`
(установщик 12 МБ и zip 9 МБ) выложен на https://blitz-forge.org/download.

## Открытые дефекты, найденные живыми прогонами 16 августа 2026

Оба найдены запуском настоящего клиента и **не исправлены**. Оба нарушают
правило «Lua-скрипт не может уронить или подвесить игру», то есть это дефекты
хоста, а не модов, которые их проявили.

### Гонка при массовом создании UI-контролов

Семь одновременно работающих Lua-модов уронили клиент: `0xC0000005`, чтение по
`0xF0`, `wotblitz.exe+0x800fb6`. В логе последними идут подряд создаваемые
контролы, падение — сразу после `control-169.yaml`.

Что установлено бисектом (все прогоны через `run_live_evidence.ps1`, факт
входа в бой подтверждён счётчиком строк освобождения ресурсов):

| Набор модов | Результат |
|---|---|
| 5 исходных примеров | CLEAN, создано 859 контролов |
| 5 + `lua_damage_log` | CLEAN |
| **7 вместе** | **падение на 169-м контроле** |

Версия «упёрлись в потолок количества контролов» **опровергнута**: чистый
прогон создал 859 штук, то есть впятеро больше, чем было в момент падения.
Похоже на гонку на пути создания контролов, которая тем вероятнее, чем больше
модов строят панели одновременно.

Что НЕ установлено: точное место гонки. Дамп не разбирался.

### Зависание клиента при выгрузке с модом на `wotb.timer`

Прогон «5 исходных + `lua_reload_timer`» дошёл до конца, но клиент **не вышел**
по `WM_CLOSE` за 120 секунд и перестал отвечать. Харнесс правильно отказался
его убивать и потребовал закрыть вручную.

`lua_reload_timer` — единственный из написанных модов, который держит
подписку на кадровое событие через `wotb.timer` (перерисовка 10 раз в секунду).
Подозрение падает на путь снятия подписок при выгрузке: `ReleaseEventSubscriptions`
ждёт завершения доставок «в полёте», и зависшая доставка ждёт вечно.

Что НЕ установлено: **это один прогон**, причинность не доказана. На гонке один
прогон не доказывает ничего — в этой же сессии я на этом уже обжёгся. Прежде
чем чинить, нужно воспроизвести.

Поэтому `lua_reload_timer` **не установлен** в игру, хотя исходник лежит в
`examples/`. Установлен и подтверждён живым прогоном `lua_damage_log`.

## Что ещё нужно для полного native покрытия

Для этих задач теперь есть loader-private ABI v1
([`DAVA_NATIVE_PRIVATE_ABI_RU.md`](DAVA_NATIVE_PRIVATE_ABI_RU.md)): шесть
all-or-nothing capability-групп, owner/kind-checked host tokens, quiescence при
замене backend и release retry. YAML и ResourceArchive копируются из provider-а
в portable snapshot и не оставляют public handle на native объект. Registry и
snapshot path имеют статус `HOST_TESTED`; это инфраструктура, а не доказательство
конкретных RVA клиента.

Текущий exact-build provider для `11.19.0.834` выставляет loader-private
adapters для native YAML/archive snapshots, `NMaterial`/`Texture` mutation и
apply, mesh load/hot-swap, stock tracers и reviewed `CLASS_FACTORY`.
`CLASS_FACTORY` остаётся safe allowlist, а не raw ObjectFactory: допустимы
только `DAVA::UIControl`, `DAVA::Entity`, `DAVA::NMaterial`, `DAVA::Texture`,
`DAVA::Mesh` и `DAVA::MeshConsumer`. Raw ObjectFactory, произвольные
DAVA-классы, vtable/pointer/FFI и component injection намеренно отсутствуют,
потому что public Lua получает только typed safe handles с owner/kind/lifetime
проверками. На exact client `11.19.0.834` все шесть typed capability-групп
прошли live-проверку: YAML, ResourceArchive, NMaterial, mesh hot-swap, stock
tracer и reviewed class factory имеют `PASS`. Неизвестное имя класса имеет
отдельный честный статус `SAFE_REVIEWED_ONLY`, а не ошибочный общий
`NOT_SUPPORTED` для поддерживаемого allowlist.

16 августа 2026 года объявлены пять интерфейсов: `wotbmod.ui.read`,
`wotbmod.camera.state`, `wotbmod.audio.intercept`, `wotbmod.scene.enumerate` и
`wotbmod.tracer`. Их portable-половина покрыта регрессией
[`../tests/v3_post_rc1_services_tests.cpp`](../tests/v3_post_rc1_services_tests.cpp)
(`286/0`). Native backend у всех пяти установлен тем же днём в
`loader/v3_native_client_services.cpp` (`ui.read` читает зеркало значений,
которые мод сам записал через `ui_v4`, а не живой текст движка); живой лог
лоадера подтверждает `declared client backend installed ui.read=1
camera.state=1 audio.intercept=1 scene.enumerate=1 tracer=1(25)`. Прогон
23 августа 2026 на `11.19.0.834` дал `PASS` строкам `ui.readonly_inspector`,
`scene.readonly_inspector`, `material.readonly_inspector`, `camera.active`,
`camera.mode_change`, `audio.custom_file`, `audio.native_sound_event`;
`tracer.created/visible/destroyed` остаются `LIVE_TEST_PENDING`, а
`tracer.requested`/`tracer.style` — намеренно `NOT_SUPPORTED`. На
`11.20.0.887` те же строки прогнаны заново 6 сентября 2026; итог по
интерфейсам — в разделе «Заморозка API 1.0 и сквозная цепочка 6 сентября
2026»: `ui.read` и `tracer` подтверждены, `camera.state` подтверждён по
событию смены режима, `audio.custom_file` и `scene.enumerate` — открытые
дефекты native backend, их честный статус `LIVE_TEST_PENDING`, а не
`SUPPORTED`.

Следующие границы намеренно остаются `NOT_SUPPORTED`, потому что найденного RVA
недостаточно для безопасного публичного ABI:

- **камера: состояние анимации против режима обзора.** Поле
  `CameraController+0x5C` — это машина состояний ANIMATION-контроллера из семи
  значений, а не enum режимов камеры. Пишется оно только в
  `CameraController::SwitchState` (`0x015A9FA0`) и в конструкторе (начальное
  значение `5`). `POSTMORTEM = 3` доказан по RTTI: состояние владеет объектом
  `PostMortemAnimationController` в `+0x6C`; тем же способом доказаны
  `2 = LookOut` (`+0x68`), `4 = LookOnTarget` (`+0x74`) и `6 = Observer`
  (`+0x70`). Состояния `0` и `1` намеренно оставлены безымянными: статические
  свидетельства о них противоречивы, а одна опубликованная догадка здесь уже
  оказалась неверной. Состояния `FREE` в этой машине нет вообще — `FreeCamera`
  это подкласс `GameCamera`, принадлежащий другому владельцу, и через
  `SwitchState` он не достигается. Класс `CinemaCameraController` в этой сборке
  **существует** (COL `0x03975618`, vtable `0x03692EEC`, живая запись vtable в
  `0x01599018`), поэтому прежнее утверждение «в этой сборке нет
  кинематографической камеры» — оно появилось из поиска слова `cinematic` в
  строках — снимается как вводящее в заблуждение; это просто не тот
  `CameraController`, который владеет описанной машиной. Аркада/снайпер —
  **другое поле на другом объекте**: `GameCamera+0x320`, индекс из двух
  значений, и опубликованное ранее соответствие `0 = ARCADE` было
  **инвертировано**, правильное чтение — `0 = SNIPER`. Обе величины объявлены
  в `camera_v2.h` только на чтение и с раздельными флагами достоверности, так
  что backend, прочитавший одну, не может выдать вторую за известную. Сеттера
  нет и не будет: политика переходов живёт в восьми точках вызова
  `SwitchState`, а не внутри неё, поэтому прямая запись обошла бы единственное,
  что эту политику соблюдает. Native mode source остаётся `NOT_SUPPORTED`;
- **semantic AUDIO — механизм доказан, детур ещё не написан.** Это больше не
  «отсутствует вообще». Ingress — vtable slot `+0x0C` (`CreateSoundEvent`)
  звуковой системы, а не тело функции; `DAVA::FastName` — один dword с
  interned `const char*`, поэтому сравнение имени это `strcmp` по
  разыменованному dword: без аллокации, без конструирования `FastName` и без
  захвата lock-а на горячем пути. Подавление обязано возвращать **собственный**
  пустой объект движка — `SoundEventStub`, который базовый
  `DAVA::SoundSystem::CreateSoundEvent` (`0x00C4F040`) строит через
  `sub_C474F0` (vtable `0x0360C7EC`), — и никогда `nullptr`: минимум шесть из
  восьми реальных call site-ов результат на null не проверяют и пишут его прямо
  в поле, так что `nullptr` это падение, а не подавление. Portable-половина
  (`audio_v3.h`, реестр перехватов, порядок по приоритету, thread-local
  re-entry guard) — `HOST_TESTED`. Самого детура нет, поэтому интерфейс
  остаётся `NOT_SUPPORTED`;
- **semantic HANGAR — якоря есть, воронка не выбрана.** Якорей много
  (`HangarCameraController`, `NewbieHangarCameraController`, hangar-состояния
  UI), но ни одна проверенная точка не пропускает через себя весь
  hangar-контент. Перехват «где придётся» подменил бы часть экрана и разошёлся
  бы с остальной, а это ровно тот вид частичной правды, который контракт
  запрещает. Остаётся `NOT_SUPPORTED`;
- **semantic LOCALIZATION — точки опоры нет.** Строкового pivot для самого
  lookup-а не найдено, то есть перехватывать пока попросту нечего.
  Дополнительно возврат строк через границу заново открывает проблему
  client-CRT ABI (аллокатор и layout `std::string` клиента), которую весь
  остальной ABI обходит копированием в буфер вызывающей стороны. Остаётся
  `NOT_SUPPORTED`;
- **tracer: чтение штатной таблицы стилей против loader-private stock create.**
  Read-only часть доказана: таблица из 25 указателей по `0x03FD6168` отображает
  байтовый код снаряда `0..24` на восемь имён стилей, а сам resolver
  ограничивает индекс (`cmp al,0x19` / `jnb`), поэтому `25` — точная граница
  образа, а не догадка. Она объявлена в `tracer_v1.h`, её portable-половина
  `HOST_TESTED`, и код вне границы всегда `WOTBMOD_V3_E_INVALID_ARGUMENT`, а не
  clamp к последней записи. Штатный цвет по `record+0x34` помечен INFERRED и
  отдаётся только под отдельным флагом `COLOR_VALID`; без подтверждения флаг
  снят, а цвет обнулён. Публичный `wotbmod.tracer` остаётся read-only:
  registration/style mutation и attachment к внутреннему object не публикуются.
  Loader-private DAVA adapter для stock tracer create теперь вызывает
  `TracerManager::ShowTracer` через проверенный bridge и возвращает typed token,
  не отдавая game-owned manager или visual node скрипту. Exact-client результат
  этого typed create route — `PASS`; public style mutation и raw attachment
  по-прежнему намеренно не публикуются;
- **scene: перечисление против мутации.** Read-only обход доказан по
  `Entity::AddNode` (`0x00CD6C10`) и зеркальному `Entity::RemoveNode`
  (`0x00D10C90`): дети — непрерывный массив `Entity*` `[+0x08, +0x0C)`,
  родитель `+0x18`, имя — `FastName` в `+0x1C`, `TransformComponent*` в
  `+0x3C`, мировая `Matrix4` в `TC+0x60`; `Scene` переиспользует тот же
  контейнер. Обход объявлен в `scene_v2.h`, ограничен по глубине, ширине и
  числу узлов, отдаёт только копии — ни указателя, ни курсора, ни handle не
  переживает вызов — и разрешён **исключительно на main-потоке**:
  `AddNode`/`RemoveNode` двигают вектор детей `memmove`-ом без блокировки, а
  `RemoveNode` вызывает `Release` сразу после уплотнения, поэтому читатель с
  другого потока может держать указатель, который освобождается следующей
  инструкцией; ни lock-а, ни счётчика версий, которые это обнаружили бы, не
  существует, и отказ по потоку — единственная защита. Typed API-owned
  `NMaterial` mutation/apply имеет live `PASS`; неограниченная мутация
  произвольного game-owned `NMaterial` и запись в произвольную game-owned Scene
  остаются `NOT_SUPPORTED`;
- `YamlParser::ParseFile` и `ResourceArchive` подключены через private typed
  provider route. Loader копирует native YAML/archive в bounded owner-owned
  snapshot и сразу освобождает native token; public handle не держит DAVA
  object. Exact-client parse/open/enumerate/read/release получили `PASS`.
  Portable bounded
  YAML, ZIP/archive и DVPL при этом полностью доступны;
- UI V3 создаёт API-owned native text/image/button/input/scroll controls,
  меняет text/texture/font/color/opacity/background, выполняет managed layout,
  managed style stack и независимый clone. Обратное чтение того, что этот API
  сам записал, объявлено отдельным `wotbmod.ui.read` (`ui_v4.h`) и читает
  зеркало loader-а, а не живой `UIStaticText`: доказанного пути прочитать
  собственный текст game-owned control-а нет, поэтому такого slot-а в таблице
  тоже нет. Произвольный raw DAVA class/component injection,
  localization/tooltip/accessibility/focus и native animation/effect components
  ещё не предоставляются;
- private ABI содержит typed `NMATERIAL` и `MESH_HOT_SWAP` groups и проверяет
  owner/kind/lifetime; exact provider реализует create/mutation/apply для
  material/texture и mesh load/hot-swap через typed mesh consumer. Это не
  превращает vehicle skin V2 в автоматический rollback уже показанной модели:
  skin pack всё ещё атомарно подменяет exact resource paths и LOD, а
  exact-client material mutation/apply и mesh hot-swap получили `PASS`;
- binding pack автоматически **проверяется**, но новые RVA автоматически не
  генерируются: после обновления игры несовпавшие bindings отключаются и должны
  быть подтверждены новым exact-build pack;
- outgoing RPC остаётся metadata-only; send/modify/drop/replay намеренно не
  предоставляются публичному mod API.

Функциональный exact-client статус шести typed DAVA-групп подтверждён live.
Перед широким публичным релизом остаётся отдельный release-hardening этап:
unload/reload, device loss, переходы hangar/battle/replay и 30+ циклов на точном
fingerprint. Он не меняет текущий `PASS` проверенных операций, но проверяет их
долговременную устойчивость.

Полное описание таблиц, методов и контрактов находится в `API_V3_RU.md`.
Автоматические доказательства перечислены в `../tests/API_TEST_REPORT.md`, а
порядок live-проверки — в `../MANUAL_LIVE_VALIDATION_RU.md`.
