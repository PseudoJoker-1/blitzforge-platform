# Известные ограничения

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
