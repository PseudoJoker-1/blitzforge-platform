# Быстрый старт: Lua-мод за 15 минут

Этот путь ведёт от чистого клиента до своего мода, который что-то показывает
в бою. Всё на фасадном Lua API (`wotb.*`), без компилятора и без reverse
engineering. Термины по ходу: **loader** — `wotb_mod_loader.dll` в папке игры,
он загружает моды; **Lua host** — пакет `wotbmod.lua_host.wotbmod`, который
исполняет Lua-моды; **фасад** — короткий Lua-модуль поверх «сырых» таблиц ABI
(подробно в [LUA_MODS_RU.md](../lua/README.md), раздел «Фасады»).

## 1. Установить loader и Lua host (5 минут)

1. Закройте игру, скачайте `BlitzForge-Setup-<версия>.exe` со страницы
   [blitz-forge.org/download](https://blitz-forge.org/download) и запустите.
   Мастер сам найдёт папку игры через Steam, проверит сборку клиента,
   докачает недостающие зависимости и положит `version.dll`,
   `wotb_mod_loader.dll`, подписанный `mods\wotbmod.lua_host.wotbmod`, ключи
   доверия в `mods\trust\keys`, включит host в `mods\mods.ini`, зарегистрирует
   кнопку «Установить в игру» и добавит команду `wotbmod` в PATH. Ничего
   запускать в PowerShell вручную не нужно; Python тоже не нужен.
2. Откройте новое окно терминала и проверьте, что клиент совпадает с SDK:

   ```text
   wotbmod doctor
   ```

   Строка `client fingerprint ... OK` означает, что версия `wotblitz.exe` и
   её SHA-256 совпадают с теми, под которые собран loader. На другой сборке
   клиента loader не запустится — это защита, а не ошибка. Папку игры команда
   определяет сама (по своему расположению); для второй копии игры добавьте
   `--game-root "D:\путь\World of Tanks Blitz"`.

Собираете SDK из исходников? Тогда набор делает
`tools\build_public_preview.ps1`, а установить его можно и без мастера —
`install.ps1 -RegisterLauncher -AddToPath` из распакованного архива. Внутри
чекаута те же команды доступны как `tools\wotbmod.cmd <команда>`.

## 2. Создать мод из шаблона (2 минуты)

```text
wotbmod new C:\mods\my-first --type lua --template battle ^
  --id yourname.my_first --name "My First Mod" --developer "Your Name"
```

Шаблоны — это те же примеры из `examples/`, проверенные тестами host-а:

| Шаблон | Что делает | Фасады |
| --- | --- | --- |
| `hello` | печатает включение, кадр и отключение | `wotb.storage` |
| `tour` | всё понемногу: ростер, убийства, HUD, GES, F7 | `mod`, `players`, `battle`, `hud`, `ges`, `keys`, `screen`, `view`, `store` |
| `panel` | панель с кнопками, F6 показать/скрыть | `panel`, `keys`, `store`, `battle` |
| `battle` | телеметрия боя и накопительный счётчик | `battle`, `players`, `timer`, `store` |
| `hud` | цвета и размеры штатного HUD, FOV | `hud`, `view`, `timer` |
| `vehicle` | переключение скинов по F9 | `vehicle`, `files`, `keys`, `store` |

В папке появятся `manifest.json` (id, права, точка входа), `main.lua` и
`README_RU.md`. Права в манифесте — это **просьба**: host выдаёт скрипту
только те, что запрошены, и не больше своего потолка.

## 3. Установить и запустить (3 минуты)

```text
wotbmod install C:\mods\my-first --yes
```

Команда покажет права, хеш и зависимости и распакует мод в
`<game>\mods\lua\yourname.my_first` (имя папки всегда равно `id`). Запустите
клиент: в `<game>\wotb_mod_loader.log` появится строка
`[v3:lua.host] yourname.my_first: installed Lua mod loaded`, а всё, что пишет
`wotb.log`, идёт туда же с категорией мода. Убрать мод — `wotbmod uninstall
yourname.my_first`, посмотреть установленное — `wotbmod list`.

Для разработки удобнее dev-папка `<game>\mods\lua-dev`: любой `main.lua` там
перезагружается при сохранении (hot reload), а права — полный потолок host-а.

## 4. Изменить и понять (5 минут)

Откройте `main.lua`. Три правила, которые объясняют весь API:

1. **Ошибка — это значение.** Каждый вызов отвечает `значение` при успехе и
   `nil, "почему"` при отказе (`false, "почему"` там, где `false` — законный
   ответ). Ничего не бросает исключений в ваш код:

   ```lua
   local ok, err = wotb.hud.reticle.set_color({ r = 1, g = 0.5, b = 0 })
   if not ok then wotb.log.warn("прицел: %s", err) end
   ```

2. **Чего клиент не сказал, того нет.** Поле без источника отсутствует и
   названо в `unavailable` с причиной; никто не подставит ноль:

   ```lua
   local d = wotb.players.details(record)
   print(d.clan_tag, d.unavailable.kills)   -- "ABC", nil (или причина)
   ```

3. **Кадр дорог.** Подписывайтесь на события (`wotb.battle.on_shot`) вместо
   опроса в `on_frame`; отдавайте всё в `on_disable` (`off_all`,
   `unbind_all`, `panel:destroy`).

## 5. Выпустить и отдать другим (5 минут)

Мод, который работает у вас, другие ставят из **релиза**: это тот же пакет
плюс подпись плюс запись с его хешем. Три команды:

```bat
rem один раз: ключ разработчика (приватный файл храните как пароль)
wotbmod keygen --out C:\keys\me.key --key-id me-2026

rem релиз: детерминированный .wotbmod, подпись, <id>-<версия>.release.json
wotbmod release C:\mods\my-first --sign-with-key C:\keys\me.key --key-id me-2026

rem публикация на портал (токен берётся в кабинете разработчика)
wotbmod publish C:\mods\releases\yourname.my_first-1.0.0.release.json ^
  --to https://blitz-forge.org/api/v1 --token <токен>

rem или в локальную папку-каталог, без портала
wotbmod publish C:\mods\releases\yourname.my_first-1.0.0.release.json --to C:\catalog
```

Другой игрок ставит его так и видит хеш, права и зависимости до того, как
что-то скопируется в `mods/`:

```bat
rem с портала: кнопкой «Установить в игру» на странице мода или командой
wotbmod install yourname.my_first --catalog https://blitz-forge.org/api/v1
wotbmod list
wotbmod rollback yourname.my_first
```

Правило одно: выпущенная версия не меняется. Поправили код — поднимите
`version` в `manifest.json`. Чтобы подпись считалась доверенной на чужом
клиенте, ваш публичный ключ `me-2026.p256` должен лежать у него в
`mods/trust/keys/`. Подробности — раздел «Установка, зависимости и релизы» в
[WOTBMOD_PACKAGE_FORMAT_RU.md](../packages/package-format.md).

Что дальше: [LUA_MODS_RU.md](../lua/README.md) — весь Lua API с фасадами;
[API_REFERENCE_RU.md](../reference/README.md) — сгенерированный справочник по
каждому слоту с правами и контекстами; [API_STATUS_RU.md](../status/api-status.md) —
что подтверждено на живом клиенте. Нативный мод (C++) начинается с
`wotbmod new --type native` и [WOTBMOD_PACKAGE_FORMAT_RU.md](../packages/package-format.md).
