# blitzforge.cluster_picker — выбор кластера в штатных настройках

Пара пакетов:

- `blitzforge.cluster_picker.ui` (ресурсный, `examples/cluster_picker_ui`):
  в `UI/Screens/Common/Settings/PrimaryTab/PrimaryOptionsPage.yaml` между
  блоком «Сервер / Игроки» и кнопкой «Отключиться» вставлена одна скрытая строка
  `bf_cluster_row` — шесть кнопок на штатном прототипе `StyledButton/StyledButton`
  (`bf_cluster_auto` «AUTO», `bf_cluster_0..4` «C0».. «C4»), раскладываются при
  сборке страницы; в рантайме переключается только видимость строки (и кнопок
  отсутствующих кластеров) через настоящий `UIControl::SetVisibilityFlag`.
  Без Lua-пакета строка невидима, страница выглядит стоково. Хеш стокового
  файла проверяется при установке: после обновления клиента пакет честно
  откажет.
- `blitzforge.cluster_picker` (Lua, эта папка): после каждого отпускания
  указателя (`wotbmod.ui.input`, `action == 3`) и по `wotbmod.ui.screen_changed`
  ищет строку (страница настроек строится внутри экрана ангара, событие смены
  экрана для неё не приходит), сверяет кнопки `bf_cluster_N` с
  `wotb.session.clusters()` по id: кластеров, которых у аккаунта нет, — прячет,
  текущий — выключает (`wotb.ui.control_set_enabled`), показывает строку.
  Подписи кнопок статические (AUTO, C0..C4 — объединение каталогов EU/NA/SG):
  на 11.20 `ui.control_set_text` для штатных контролов не поддерживается.
  Клик ловится по прямоугольнику кнопки на экране (сумма позиций по цепочке
  родителей через `wotb.ui.control_get_parent`): `wotb.ui.event_subscribe(EVENT_CLICK)`
  на штатной кнопке, найденной от корня экрана, не срабатывает — рантайм знает
  позиции только обёрнутых им родителей. Подтверждение — своя панель `wotb.panel`
  (диалог клиента `ui.confirm_show` на 11.20 не опубликован) → 
  `wotb.session.change_cluster(id)`; исход приходит через
  `wotb.session.on_cluster_changed` (`started/connected/failed`) и показывается
  тостом `wotb.screen.notify`.

Права: `core`, `events.public`, `storage`, `ui`, `ui.create`, `ui.modify.own`,
`battle.ui`, `ui.modify.game` (сырая таблица `wotb.ui` открывается только всей
UI-семьёй разом),
`session.cluster.read`, `session.cluster.change` (REVIEWED — мод меняет цель
сетевого подключения).

Переключение действует на текущую сессию: после перезапуска клиент снова
выбирает кластер сам (`preferredHost: best_by_ping_and_load`). Смена региона
(EU → NA) не поддерживается — это другой аккаунт.

Установка: оба пакета с портала (`wotbmod://install/blitzforge.cluster_picker.ui@1.0.0`,
затем `wotbmod://install/blitzforge.cluster_picker@1.0.1`) или
`wotbmod install <файл>.wotbmod`.
