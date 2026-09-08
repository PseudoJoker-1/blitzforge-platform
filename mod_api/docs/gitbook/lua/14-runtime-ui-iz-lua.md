# Runtime UI из Lua

Ниже описан сам интерфейс `wotb.ui`. Если нужна обычная панель из строк и
кнопок, живущая в бою, начните с `wotb.panel` — она собрана ровно из этих
вызовов и снимает с автора монтирование, привязку к экрану, контекстный gate и
разбор при выгрузке.

Основной runtime-конструктор принимает одну Lua-таблицу:

```lua
local label, err = wotb.ui.create({
    type = wotb.ui.CONTROL_TEXT,
    id = "VolumeLabel",
    text = "Громкость: 75%",
    x = 40, y = 60, width = 320, height = 48,
    font = "~res:/Fonts/WarHeliosCondCBold.ttf",
    font_size = 24,
    color = {r=1, g=1, b=1, a=1},
    background_color = {r=0.05, g=0.08, b=0.12, a=0.95},
    alignment = wotb.ui.ALIGN_LEFT,
    opacity = 1.0,
    visible = true,
})
if not label then
    print(err)
    return
end

label:set_text("Громкость: 80%")
label:set_color({r=1, g=0.65, b=0.1, a=1})
```

Изменения, которым нужна перестройка native DAVA control (`text`, `texture`,
`font`, `color`, `opacity`, background, alignment и wrap), объединяются до
следующего кадра. Поэтому setters безопасно вызывать прямо из click/drag
callback: за кадр применяется только последнее значение, а control не
освобождается посреди обработки input event. Geometry, visibility, enabled и
interactable применяются через прямые native setters.

Поддерживаются все frozen `CONTROL_*`: container, text, image, button,
checkbox, slider, dropdown, text input, scroll view, list и tabs. Object handle
даёт методы `clone`, `destroy`, `add_child`, `set_parent`, `set_text`,
`set_texture`, `set_font`, `set_font_size`, `set_color`, `set_opacity`,
geometry/layout, `on` и `push_style`. Ошибки сохраняют общий контракт
`nil, message`.

Динамический список строится обычным Lua-циклом:

```lua
local list = wotb.ui.create({
    type = wotb.ui.CONTROL_LIST,
    id = "Players", x = 40, y = 140, width = 360, height = 400,
})
local row = wotb.ui.create({
    type = wotb.ui.CONTROL_BUTTON,
    id = "RowTemplate", text = "", width = 330, height = 38,
    parent = list, visible = false,
})

for index, player in ipairs(players) do
    local copy = row:clone()
    copy:set_parent(list)
    copy:set_position(0, (index - 1) * 44)
    copy:set_text(player.name)
    copy:set_visible(true)
    copy:on(wotb.ui.EVENT_CLICK, function()
        label:set_text("Выбран: " .. player.name)
    end)
end
```

Для совместимости остаётся template-backed форма `control_create(type, id,
uri)`, но runtime UI и пример от неё не зависят. Произвольные raw DAVA class
names/component payloads и FFI намеренно недоступны; выбираются безопасные
типы/свойства API. Native animation/effect component injection пока не
предоставляется — анимация выполняется из `on_frame` через property setters.
