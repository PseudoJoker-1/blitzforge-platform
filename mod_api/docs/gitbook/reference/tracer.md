# `wotb.tracer`

Raw-таблица интерфейса `WotbModV3TracerApiV1` (`include/wotbmod/tracer_v1.h`, версия `WOTBMOD_V3_TRACER_VERSION`). Функции ниже вызываются как `wotb.tracer.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `visible.projectile.events`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.tracer.SHELL_TYPE_COUNT` | `WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT` | define |
| `wotb.tracer.STYLE_NAME_SIZE` | `WOTBMOD_V3_TRACER_STYLE_NAME_SIZE` | define |
| `wotb.tracer.STYLE_UNKNOWN` | `WOTBMOD_V3_TRACER_STYLE_UNKNOWN` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_ARMOR_PIERCING` | `WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_ARMOR_PIERCING_CR` | `WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING_CR` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_HIGH_EXPLOSIVE` | `WOTBMOD_V3_TRACER_STYLE_HIGH_EXPLOSIVE` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_HOLLOW_CHARGE` | `WOTBMOD_V3_TRACER_STYLE_HOLLOW_CHARGE` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_ANTI_TANK_GUIDED_MISSILE` | `WOTBMOD_V3_TRACER_STYLE_ANTI_TANK_GUIDED_MISSILE` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_RAILGUN` | `WOTBMOD_V3_TRACER_STYLE_RAILGUN` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_IMPROVED_DETECTION` | `WOTBMOD_V3_TRACER_STYLE_IMPROVED_DETECTION` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_STT_TRACER` | `WOTBMOD_V3_TRACER_STYLE_STT_TRACER` | enum WotbModV3TracerStyleId |
| `wotb.tracer.STYLE_NAME_VALID` | `WOTBMOD_V3_TRACER_STYLE_NAME_VALID` | enum WotbModV3TracerStyleFlag |
| `wotb.tracer.STYLE_COLOR_VALID` | `WOTBMOD_V3_TRACER_STYLE_COLOR_VALID` | enum WotbModV3TracerStyleFlag |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_shell_type_count` | — | count: integer |
| `get_style_for_shell_type` | shell_type: integer | style: table:TracerStyle |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["visible.projectile.events"]
local get_shell_type_count, err = wotb.tracer.get_shell_type_count()  -- count: integer
if get_shell_type_count == nil then wotb.log.warn("tracer.get_shell_type_count: %s", err) end
local get_style_for_shell_type, err = wotb.tracer.get_style_for_shell_type(0)  -- style: table:TracerStyle
if get_style_for_shell_type == nil then wotb.log.warn("tracer.get_style_for_shell_type: %s", err) end
```

