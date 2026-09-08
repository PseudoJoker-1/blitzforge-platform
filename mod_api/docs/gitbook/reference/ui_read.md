# `wotb.ui_read`

Raw-таблица интерфейса `WotbModV3UiApiV4` (`include/wotbmod/ui_v4.h`, версия `WOTBMOD_V3_UI_VERSION_4`). Функции ниже вызываются как `wotb.ui_read.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.screen`](../facades/wotb-screen.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.ui_read.GAME_OWNED` | `WOTBMOD_V3_UI_READ_GAME_OWNED` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.TEXT_SET` | `WOTBMOD_V3_UI_READ_TEXT_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.TEXTURE_SET` | `WOTBMOD_V3_UI_READ_TEXTURE_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.FONT_SET` | `WOTBMOD_V3_UI_READ_FONT_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.COLOR_SET` | `WOTBMOD_V3_UI_READ_COLOR_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.BACKGROUND_COLOR_SET` | `WOTBMOD_V3_UI_READ_BACKGROUND_COLOR_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.OPACITY_SET` | `WOTBMOD_V3_UI_READ_OPACITY_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.FONT_SIZE_SET` | `WOTBMOD_V3_UI_READ_FONT_SIZE_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.TEXT_LAYOUT_SET` | `WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET` | enum WotbModV3UiReadFlag |
| `wotb.ui_read.LIVE_TEXT_SET` | `WOTBMOD_V3_UI_READ_LIVE_TEXT_SET` | enum WotbModV3UiReadFlag |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `control_get_text` | control: handle | string |
| `control_get_texture` | control: handle | string |
| `control_get_font` | control: handle | string |
| `control_get_style` | control: handle | style: table:UiStyleSnapshot |
| `control_get_live_text` | control: handle | string |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"]
local control_get_text, err = wotb.ui_read.control_get_text(handle)  -- string
if control_get_text == nil then wotb.log.warn("ui_read.control_get_text: %s", err) end
local control_get_texture, err = wotb.ui_read.control_get_texture(handle)  -- string
if control_get_texture == nil then wotb.log.warn("ui_read.control_get_texture: %s", err) end
local control_get_font, err = wotb.ui_read.control_get_font(handle)  -- string
if control_get_font == nil then wotb.log.warn("ui_read.control_get_font: %s", err) end
```

