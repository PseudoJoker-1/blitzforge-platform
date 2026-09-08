# Первый мод за 15 минут

От пустой папки до релиза в каталоге BlitzForge. Всё делается из терминала
Windows; нужен клиент World of Tanks Blitz 11.20.0.887 и набор BlitzForge с
https://blitz-forge.org/download (мастер ставит загрузчик, Lua-хост, команду
`wotbmod` и кнопку «Установить в игру»).

## 1. Проверьте, что набор стоит (1 минута)

```
wotbmod list
```

В списке должен быть `wotbmod.lua_host` с подписью `blitzforge-preview-2026`.
Если команды нет — перезапустите терминал (мастер добавляет папку
`<игра>\wotbmod` в PATH пользователя).

## 2. Заготовка мода (2 минуты)

Мод — папка с `manifest.json` и `main.lua`:

```
mkdir my.hello && cd my.hello
```

`manifest.json`:

```json
{
  "id": "my.hello",
  "name": "Привет из ангара",
  "version": "1.0.0",
  "developer": "ваш ник",
  "description": "Панель с приветствием в ангаре.",
  "entrypoint": "main.lua",
  "permissions": ["core", "ui", "ui.create", "ui.modify.own", "battle.ui", "ui.modify.game"]
}
```

`main.lua`:

```lua
local log = wotb.log
log.set_category("hello")

local panel = wotb.panel.new({
  id = "hello", width = 320, height = 90, anchor = "top-right", margin = 24,
  contexts = false,            -- виден и в ангаре, и в бою
  rows = { { text = "Привет, " .. (wotb.players and wotb.players.me and wotb.players.me().nick or "танкист") } },
})

function on_enable()
  local ok, why = panel:mount()
  if not ok then log.warn("панель не встала: %s", tostring(why)) end
  log.info("hello armed")
end

function on_disable()
  panel:destroy()
end
```

Права: `core` даёт логи и таймеры, `ui` + `ui.create` + `ui.modify.own` +
`battle.ui` + `ui.modify.game` — вся UI-семья (сырой `wotb.ui` открывается
только ей целиком; `wotb.panel` рисует свои контролы через него).

## 3. Проверьте в игре, не собирая пакет (3 минуты)

Скопируйте папку в `<игра>\mods\lua\my.hello` и запустите клиент. Lua-хост
подхватывает папки из `mods\lua` как установленные моды; свои сообщения он
пишет в `<игра>\wotb_mod_loader.log`:

```
[v3:hello] hello armed
```

Правки `main.lua` в этой папке применяются горячей перезагрузкой, клиент
перезапускать не нужно. Ошибки скрипта видны там же, с номером строки.

## 4. Ключ разработчика и релиз (3 минуты)

```
wotbmod keygen --key-id my-2026 --out %USERPROFILE%\.wotbmod\developer.key
wotbmod release my.hello -o release --sign-with-key %USERPROFILE%\.wotbmod\developer.key --key-id my-2026
```

`release` прогоняет статическую проверку (запрошенные права против того, что
скрипт реально вызывает), собирает `release\my.hello-1.0.0.wotbmod`, подпись
`.sig` и запись `my.hello-1.0.0.release.json`. Версия неизменяема: другие
байты — только под новым номером.

Поставьте свой пакет так, как его поставит игрок:

```
wotbmod install release\my.hello-1.0.0.wotbmod --yes
```

## 5. Публикация (5 минут)

1. Зарегистрируйтесь на https://blitz-forge.org, подтвердите почту.
2. В кабинете подайте заявку разработчика — модератор одобряет её, письмо
   придёт на почту.
3. В кабинете создайте API-токен и опубликуйте релиз:

```
wotbmod publish release\my.hello-1.0.0.release.json --to https://blitz-forge.org/api/v1 --token <токен>
```

Мод появится в каталоге со статусом «не проверен». Модератор ставит отметку
«проверен» после просмотра прав и кода; после этого релизы подписывает портал
и игроку хватает одного доверенного ключа.

## Что дальше

- Ваш мод виден игрокам в каталоге прямо в ангаре: иконка в левой колонке
  открывает штатный экран `blitzforge.catalog` (ставится мастером), вкладка
  «Каталог» предлагает установку одной кнопкой, вкладка «Кастомные моды»
  показывает и вашу папку из `mods\lua`, пока вы её отлаживаете.
- Справочник фасадов и событий: `docs/LUA_MODS_RU.md` и
  https://pd0-2.gitbook.io/blitzforge.
- Примеры: `examples/lua_facade_tour` (все фасады), `examples/cluster_picker`
  (штатные настройки и смена кластера), `examples/night_mode` (ресурсный пакет).
- Известные ограничения текущего клиента: `docs/API_STATUS_RU.md`, раздел
  «Известные ограничения».
- Правила модерации: `docs/MODERATION_RULES_RU.md`.
