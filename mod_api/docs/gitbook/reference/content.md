# `wotb.content`

Raw-таблица интерфейса `WotbModV3ContentApiV1` (`include/wotbmod/content_v1.h`, версия `WOTBMOD_V3_CONTENT_VERSION`). Функции ниже вызываются как `wotb.content.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `content`, `resources.mod`, `resources.overlay.game`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.content.OVERRIDE_AUDIO` | `WOTBMOD_V3_CONTENT_AUDIO` | enum WotbModV3ContentOverrideKind |
| `wotb.content.OVERRIDE_TEXTURE` | `WOTBMOD_V3_CONTENT_TEXTURE` | enum WotbModV3ContentOverrideKind |
| `wotb.content.OVERRIDE_UI` | `WOTBMOD_V3_CONTENT_UI` | enum WotbModV3ContentOverrideKind |
| `wotb.content.OVERRIDE_HANGAR` | `WOTBMOD_V3_CONTENT_HANGAR` | enum WotbModV3ContentOverrideKind |
| `wotb.content.OVERRIDE_MODEL` | `WOTBMOD_V3_CONTENT_MODEL` | enum WotbModV3ContentOverrideKind |
| `wotb.content.OVERRIDE_LOCALIZATION` | `WOTBMOD_V3_CONTENT_LOCALIZATION` | enum WotbModV3ContentOverrideKind |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `parse_json` | json_utf8: table:ConstBuffer | content: handle |
| `parse_uri` | uri: string | content: handle |
| `validate` | content: handle | true |
| `get_info` | content: handle | info: table:ContentInfo |
| `get_override` | content: handle, index: integer | override: table:ContentOverride |
| `apply` | content: handle | true |
| `unapply` | content: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["content", "resources.mod", "resources.overlay.game"]
local parse_json, err = wotb.content.parse_json(json_utf8)  -- content: handle
if parse_json == nil then wotb.log.warn("content.parse_json: %s", err) end
local parse_uri, err = wotb.content.parse_uri("...")  -- content: handle
if parse_uri == nil then wotb.log.warn("content.parse_uri: %s", err) end
local validate_ok, err = wotb.content.validate(handle)
if not validate_ok then wotb.log.warn("content.validate: %s", err) end
```

