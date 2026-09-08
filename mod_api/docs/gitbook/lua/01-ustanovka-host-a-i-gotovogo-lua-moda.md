# Установка host-а и готового Lua-мода

Игроку и автору мода собирать ничего не нужно: установщик
`BlitzForge-Setup-<версия>.exe` с [blitz-forge.org/download](https://blitz-forge.org/download)
кладёт loader и подписанный Lua host в игру сам, а готовые моды ставятся
кнопкой «Установить в игру» или командой `wotbmod install <id> --catalog
https://blitz-forge.org/api/v1`. Ниже — сборка тех же артефактов из исходников
SDK, она нужна только тем, кто правит host.

Собрать и проверить native host package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lua_host_package.ps1
```

Собрать подписанный public-preview bundle с безопасными install/verify/
uninstall scripts:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_public_preview.ps1 -SkipBuild
```

Перед `-SkipBuild` должны успешно завершиться `build.cmd`,
`proxy_dll\build.cmd` и `loader\build_live.cmd`. Без `-SkipBuild` release script
выполнит их самостоятельно. Приватный ECDSA P-256 ключ создаётся как
неэкспортируемый user key в Windows CNG; в bundle попадают только detached
signature и публичный X||Y trust key.

Команда сначала прогоняет Lua vendor/host tests, проверяет таблицу экспортов
release DLL, затем создаёт и валидирует:

```text
build\lua_host_packages\wotbmod.lua_host.wotbmod
build\lua_host_distribution\
├── wotbmod.lua_host.wotbmod
└── mods\lua\
    ├── example.lua_hello\...
    ├── example.lua_ui_framework\...
    ├── example.lua_ally_tracker\...
    ├── example.lua_battle_telemetry\...
    └── example.lua_session_stats\...
```

Положите `wotbmod.lua_host.wotbmod` в обычный каталог пакетов BlitzForge
`<game>\mods`. Установленный Lua-мод — это распакованная папка:

```text
<game>\mods\lua\example.lua_hello\
├── manifest.json
└── main.lua
```

Имя папки обязано совпадать с `id` без учёта регистра. Reparse-point каталоги
не сканируются. Установленный скрипт читается один раз на цикл enable/disable и
не перезагружается при изменении файла.

Готовый `example.lua_ui_framework` добавляет активную кнопку и полностью
создаёт окно во время работы без авторского YAML. Внутри есть native text,
image, buttons, `UITextField`, два drag-слайдера, toggle, scroll/list с восемью
клонированными строками и Lua-анимация. Мод меняет text/font/color/opacity/
texture/geometry/style и полностью освобождает callbacks, style overrides и
UI handles при `on_disable` или смене active screen.

Два других установленных примера являются не диагностическими матрицами, а
обычными игровыми модами:

- `example.lua_ally_tracker` показывает public ID/team/name/HP союзников,
  время последнего публичного обновления и последнюю позицию только при
  `position_available = true`;
- `example.lua_battle_telemetry` ведёт управляемый журнал типизированных
  damage/reload/ammo/camera/shot/hit событий;
- `example.lua_session_stats` создаёт в ангаре Blitz-панель статистики за
  текущий запуск клиента: история восьми боёв, полученный урон, локальные
  выстрелы, выживание, длительность и результат только при подтверждённом
  `winner_team`. Недоступные damage dealt/kills/hits показываются как `N/A`.
  Native loader публикует `HANGAR` по наличию уникального DAVA-маркера и
  `MOD_SCREEN` по видимости каталога,
  поэтому Lua-моду не нужен собственный доступ к дереву игровых контролов для
  определения видимости. Сам session-мод сохраняет уже подтверждённый корень
  ангара при кратковременном контексте `NONE`, но сбрасывает его на
  `LOADING`/`BATTLE`/`REPLAY`/`TRAINING`/`RESULTS` или при уничтожении root.

Боевые UI-моды удаляют controls вне боя; session statistics, наоборот,
существует только в ангаре. Все четыре всегда блокируются при
`MOD_SCREEN`/`TEXT_INPUT`, поэтому не перекрывают каталог модов.
