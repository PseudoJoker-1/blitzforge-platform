# `wotb.settings`

Raw-таблица интерфейса `WotbModV3SettingsApiV1` (`include/wotbmod/settings_v1.h`, версия `WOTBMOD_V3_SETTINGS_VERSION`). Функции ниже вызываются как `wotb.settings.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `settings`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.settings.KEY_MAX` | `WOTBMOD_V3_SETTING_KEY_MAX` | define |
| `wotb.settings.TEXT_MAX` | `WOTBMOD_V3_SETTING_TEXT_MAX` | define |
| `wotb.settings.TYPE_BOOL` | `WOTBMOD_V3_SETTING_BOOL` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_INT` | `WOTBMOD_V3_SETTING_INT` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_FLOAT` | `WOTBMOD_V3_SETTING_FLOAT` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_STRING` | `WOTBMOD_V3_SETTING_STRING` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_ENUM` | `WOTBMOD_V3_SETTING_ENUM` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_COLOR` | `WOTBMOD_V3_SETTING_COLOR` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_KEYBIND` | `WOTBMOD_V3_SETTING_KEYBIND` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_FILE` | `WOTBMOD_V3_SETTING_FILE` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_FOLDER` | `WOTBMOD_V3_SETTING_FOLDER` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_TITLE` | `WOTBMOD_V3_SETTING_TITLE` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_BUTTON` | `WOTBMOD_V3_SETTING_BUTTON` | enum WotbModV3SettingType |
| `wotb.settings.TYPE_CUSTOM` | `WOTBMOD_V3_SETTING_CUSTOM` | enum WotbModV3SettingType |
| `wotb.settings.FLAG_NONE` | `WOTBMOD_V3_SETTING_FLAG_NONE` | enum WotbModV3SettingFlags |
| `wotb.settings.FLAG_REQUIRES_RESTART` | `WOTBMOD_V3_SETTING_FLAG_REQUIRES_RESTART` | enum WotbModV3SettingFlags |
| `wotb.settings.FLAG_READ_ONLY` | `WOTBMOD_V3_SETTING_FLAG_READ_ONLY` | enum WotbModV3SettingFlags |
| `wotb.settings.FLAG_HIDDEN` | `WOTBMOD_V3_SETTING_FLAG_HIDDEN` | enum WotbModV3SettingFlags |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `register_schema` | schema_version: integer, definitions: array | true |
| `register_preset` | preset: table:SettingPreset | true |
| `get_schema_version` | — | version: integer |
| `get_bool` | key: string | value: integer |
| `get_int` | key: string | value: integer |
| `get_float` | key: string | value: number |
| `get_string` | key: string | string |
| `get_color` | key: string | value: table:Color |
| `set_bool` | key: string, value: integer | true |
| `set_int` | key: string, value: integer | true |
| `set_float` | key: string, value: number | true |
| `set_string` | key: string, value: string | true |
| `set_color` | key: string, value: table:Color | true |
| `reset` | key: string | true |
| `reset_all` | — | true |
| `apply_preset` | preset_id: string | true |
| `subscribe` | callback: function | token: handle |
| `unsubscribe` | token: handle | true |
| `run_migration` | target_version: integer, callback: function | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["settings"]
local register_schema_ok, err = wotb.settings.register_schema(0, {})
if not register_schema_ok then wotb.log.warn("settings.register_schema: %s", err) end
local register_preset_ok, err = wotb.settings.register_preset(preset)
if not register_preset_ok then wotb.log.warn("settings.register_preset: %s", err) end
local get_schema_version, err = wotb.settings.get_schema_version()  -- version: integer
if get_schema_version == nil then wotb.log.warn("settings.get_schema_version: %s", err) end
```

