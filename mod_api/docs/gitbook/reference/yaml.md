# `wotb.yaml`

Raw-таблица интерфейса `WotbModV3YamlApiV1` (`include/wotbmod/yaml_v1.h`, версия `WOTBMOD_V3_YAML_VERSION`). Функции ниже вызываются как `wotb.yaml.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.files`](../facades/wotb-files.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `resources.mod`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.yaml.INVALID_NODE` | `WOTBMOD_V3_YAML_INVALID_NODE` | define |
| `wotb.yaml.NULL` | `WOTBMOD_V3_YAML_NULL` | enum WotbModV3YamlNodeType |
| `wotb.yaml.MAP` | `WOTBMOD_V3_YAML_MAP` | enum WotbModV3YamlNodeType |
| `wotb.yaml.SEQUENCE` | `WOTBMOD_V3_YAML_SEQUENCE` | enum WotbModV3YamlNodeType |
| `wotb.yaml.STRING` | `WOTBMOD_V3_YAML_STRING` | enum WotbModV3YamlNodeType |
| `wotb.yaml.BOOL` | `WOTBMOD_V3_YAML_BOOL` | enum WotbModV3YamlNodeType |
| `wotb.yaml.INT` | `WOTBMOD_V3_YAML_INT` | enum WotbModV3YamlNodeType |
| `wotb.yaml.FLOAT` | `WOTBMOD_V3_YAML_FLOAT` | enum WotbModV3YamlNodeType |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `parse` | yaml_utf8: table:ConstBuffer, limits: table:YamlLimits | document: handle |
| `parse_uri` | uri: string, limits: table:YamlLimits | document: handle |
| `get_root` | document: handle | node: handle |
| `get_type` | document: handle, node: handle | type: integer |
| `get_size` | document: handle, node: handle | size: integer |
| `map_get` | document: handle, node: handle, key: string | child: handle |
| `sequence_get` | document: handle, node: handle, index: integer | child: handle |
| `get_string` | document: handle, node: handle | string |
| `get_bool` | document: handle, node: handle | value: integer |
| `get_int` | document: handle, node: handle | value: integer |
| `get_float` | document: handle, node: handle | value: number |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["resources.mod"]
local parse, err = wotb.yaml.parse(yaml_utf8, limits)  -- document: handle
if parse == nil then wotb.log.warn("yaml.parse: %s", err) end
local parse_uri, err = wotb.yaml.parse_uri("...", limits)  -- document: handle
if parse_uri == nil then wotb.log.warn("yaml.parse_uri: %s", err) end
local get_root, err = wotb.yaml.get_root(handle)  -- node: handle
if get_root == nil then wotb.log.warn("yaml.get_root: %s", err) end
```

