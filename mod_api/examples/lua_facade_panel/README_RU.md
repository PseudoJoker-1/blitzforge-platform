# Lua Facade Panel

Панель из динамических контролов `wotb.panel` (`label`, `row` с кнопками,
`on("error")`), горячая клавиша F6 через `wotb.keys`, память о скрытии через
`wotb.store`, счётчик выстрелов через `wotb.battle.on_shot`.

Скопируйте папку как `<game>\mods\lua\example.lua_facade_panel`
(имя папки равно `id` в `manifest.json`). Шаблон для своего мода:
`tools\wotbmod.cmd new C:\mods\my-panel --type lua --template panel
--id author.my_panel --name "My Panel" --developer "Author"`.

Цена на кадр — один `panel:update(frame)`; строка счётчика переписывается
только при выстреле. `on_disable` отдаёт подписки, действия и контролы.
