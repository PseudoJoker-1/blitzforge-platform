# BlitzForge API 0.1.0 Public Preview

Это публичный preview Lua Mod API, а не stable-релиз. Bundle рассчитан
только на Windows x86 клиент World of Tanks Blitz `11.20.0.887` с SHA-256
`4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af`.
Установщик откажется менять файлы любой другой сборки.

Один набор содержит всё, что нужно игроку: `version.dll` и
`wotb_mod_loader.dll` (loader), подписанный `mods\wotbmod.lua_host.wotbmod`
(Lua host), ключи в `mods\trust\keys` (ключ этого набора и, если он есть,
публичный ключ портала) и папку `wotbmod\` с инструментами игрока:
`wotbmod.py` (install/update/rollback/list/info/quarantine/report-crash/verify)
и `wotbmod_launcher.py` (кнопка «Установить в игру» на портале). Хеши всех
файлов перечислены в `release-manifest.json`; `verify.ps1` сверяет их.

Каталог модов и кнопка «Установить в игру»: https://blitz-forge.org.
Публичный ключ портала уже лежит в этом наборе, поэтому релизы, подписанные
порталом, устанавливаются как доверенные.

## Установка

Проще всего: закройте игру и запустите установщик `BlitzForge-Setup-<версия>.exe`
(обычный мастер Windows). Он сам найдёт папку игры через Steam, проверит
сборку клиента, докачает и поставит недостающие зависимости (Visual C++
Runtime x86; по желанию Python для разработчиков), скопирует файлы,
зарегистрирует кнопку «Установить в игру» и добавит команду `wotbmod` в PATH
пользователя. Удаление — через «Приложения» Windows. Python игроку не нужен:
инструменты собраны в `wotbmod\wotbmod.exe`.

Тот же набор в виде архива ставится скриптами из PowerShell (каталог
распакованного bundle):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\verify.ps1
```

Установщик до записи проверяет клиент, хэши payload и detached ECDSA P-256
подпись Lua host. Он не заменяет неизвестный `version.dll`, чужой loader или
другой ключ по тому же trust-store пути. Исходные файлы сохраняются под
`mods\cache\install_backups`, а состояние операции — в
`mods\cache\public-preview-install.json`. Повторный запуск безопасно обновляет
файлы, если установленная версия не менялась посторонней программой.

Для нестандартного каталога игры:

```powershell
.\install.ps1 -GameRoot 'D:\SteamLibrary\steamapps\common\World of Tanks Blitz'
```

Чтобы кнопка «Установить в игру» на портале работала в браузере, нужен
Python 3.11+ и регистрация launcher-а (запись в реестре текущего пользователя,
без прав администратора):

```powershell
.\install.ps1 -RegisterLauncher -AddToPath
rem или позже, вручную:
"<папка игры>\wotbmod\wotbmod.exe" launcher register --game-root "<папка игры>"
```

После `-AddToPath` (или мастера) в любом новом терминале работает команда
`wotbmod`: `wotbmod list`, `wotbmod install <id> --catalog https://blitz-forge.org/api/v1`,
`wotbmod rollback <id>`, `wotbmod uninstall <id>`, `wotbmod info <id>`.
Папка игры определяется сама (Steam); для другой добавьте `--game-root`.

## Lua-моды

Native host устанавливается как подписанный `mods\wotbmod.lua_host.wotbmod`.
Lua-мод является каталогом `mods\lua\<id>` с `manifest.json` и `main.lua`.
Примеры из `examples` намеренно не включаются автоматически: выберите нужный
пример и скопируйте его каталог в `mods\lua`. В `mods\mods.ini` достаточно
включить `wotbmod.lua_host`: host сам обнаруживает Lua-моды по manifest. Имена
каталогов в bundle уже точно совпадают с их `manifest.id` (`example.lua_*`);
переименовывать их не нужно.

В preview доступны runtime UI, типизированные события, snapshots публично
видимых игроков, input, camera/gameplay helpers и остальные reviewed V3
интерфейсы, перечисленные в `LUA_MODS_RU.md` внутри host package. Каждый Lua
callback ограничен бюджетом в 100 000 VM-инструкций.

`example.lua_session_stats` — готовая ангарная панель статистики за текущий
запуск клиента. Она считает подтверждённые полученный урон, локальные выстрелы,
выживание и длительность, а неподтверждённые результат/damage dealt/kills/hits
честно помечает `N/A`.

## Удаление

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

Uninstall сверяет хэши всех установленных файлов. Если файл изменён после
установки, операция останавливается и ничего не удаляет. После успешного
восстановления recovery backup сохраняется для ручной проверки.

## Известные границы preview

- поддерживается только указанный exact client fingerprint;
- Lua sandbox не устраняет системный риск native SEH в основном C++ runtime;
- недоказанные данные скрытых противников, raw pointers и произвольный DAVA
  component injection не предоставляются;
- после обновления игры требуется новый binding pack и повторная live-проверка;
- перед распространением модов автор обязан проверить их в реальном клиенте и
  не запрашивать permissions шире фактически используемых.
