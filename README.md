# BlitzForge

Платформа модов для World of Tanks Blitz (Windows, клиент `11.20.0.887`):
загрузчик с замороженным C ABI, Lua-хост, инструменты разработчика,
портал каталога и мастер установки для игроков.

- Сайт и каталог: https://blitz-forge.org
- Документация: https://pd0-2.gitbook.io/blitzforge
- Набор для игрока: https://blitz-forge.org/download

## Состав репозитория

| Папка | Что это |
| --- | --- |
| `mod_api/include` | Публичный C ABI (`WotbModV3*`), заморожен как API 1.1 |
| `mod_api/src`, `mod_api/loader` | Runtime и загрузчик (MinHook-детуры, fingerprint клиента, DAVA-бэкенд, Lua-хост) |
| `mod_api/tools` | Команда `wotbmod` (пакеты, подписи, каталог, `sync`), launcher `wotbmod://`, генераторы Lua-биндингов и документации |
| `mod_api/examples` | Примеры: `lua_facade_tour`, `cluster_picker` (+ `cluster_picker_ui`), `night_mode`, validation-мод |
| `mod_api/docs` | Документация на русском: статус API, известные ограничения, первый мод за 15 минут, правила модерации, playbook патча клиента |
| `mod_api/tests` | Тесты runtime, CLI, launcher, портала (`build.cmd` собирает и прогоняет всё) |
| `wotbmod-portal` | Портал каталога: backend на `http.server` + SQLite, шаблоны, деплой |
| `reanchor` | Переанкоровка фиксированных адресов после патча клиента (рецепты `recipes.json`) |
| `mods/native-validation` | Пакет validation-мода, который судит строки API вживую |
| `proxy_dll` | Proxy `version.dll`, поднимающий загрузчик в процессе клиента |

## Сборка

Нужны Visual Studio 2022 (x86 toolset) и Python 3.13.

```
cd mod_api
build.cmd                 # runtime, тесты, пакеты примеров (~10 минут)
loader\build_live.cmd     # DLL загрузчика и validation-мод для живого клиента
tools\build_public_preview.ps1 -SkipBuild -Version 0.1.0-preview.N   # набор для игрока (нужен Inno Setup 6)
```

Портал: `wotbmod-portal/run.cmd` (локально), `deploy/` (VPS). Перед
деплоем `python tools/sync_sdk.py` подтягивает vendored SDK.

## Что не лежит в репозитории

Дампы, базы IDA и копии клиента (`analysis/`), ключи подписи, `config.json`
портала и его данные, сборочные артефакты. Публичные ключи доверия
(`mods/trust/keys/*.p256`) — часть цепочки доверия и лежат.

## Статус

`0.1.0-alpha.1`. Все интерфейсы API проверены на живом клиенте, кроме
перечисленных в `mod_api/docs/KNOWN_LIMITATIONS_RU.md`.
