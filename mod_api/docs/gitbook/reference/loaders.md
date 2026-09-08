# `wotb.loaders`

Raw-таблица интерфейса `WotbModV3LoadersApiV1` (`include/wotbmod/loaders_v1.h`, версия `WOTBMOD_V3_LOADERS_VERSION`). Функции ниже вызываются как `wotb.loaders.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.files`](../facades/wotb-files.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `resources.mod`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.loaders.UTF8_TEXT` | `WOTBMOD_V3_LOADER_UTF8_TEXT` | enum WotbModV3LoaderBackend |
| `wotb.loaders.BINARY` | `WOTBMOD_V3_LOADER_BINARY` | enum WotbModV3LoaderBackend |
| `wotb.loaders.MINIMAL_YAML` | `WOTBMOD_V3_LOADER_MINIMAL_YAML` | enum WotbModV3LoaderBackend |
| `wotb.loaders.DAVA_YAML` | `WOTBMOD_V3_LOADER_DAVA_YAML` | enum WotbModV3LoaderBackend |
| `wotb.loaders.DVPL` | `WOTBMOD_V3_LOADER_DVPL` | enum WotbModV3LoaderBackend |
| `wotb.loaders.DAVA_ARCHIVE` | `WOTBMOD_V3_LOADER_DAVA_ARCHIVE` | enum WotbModV3LoaderBackend |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `load_text_utf8` | uri: string, max_bytes: integer | bytes |
| `load_binary` | uri: string, max_bytes: integer | bytes |
| `load_yaml` | uri: string, limits: table:YamlLimits | document: handle |
| `load_dava_yaml` | uri: string | document: handle |
| `unpack_dvpl` | uri: string | bytes |
| `open_dava_archive` | uri: string | archive: handle |
| `get_backend_info` | backend: integer | info: table:LoaderBackendInfo |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["resources.mod"]
local load_text_utf8, err = wotb.loaders.load_text_utf8("...", 0)  -- bytes
if load_text_utf8 == nil then wotb.log.warn("loaders.load_text_utf8: %s", err) end
local load_binary, err = wotb.loaders.load_binary("...", 0)  -- bytes
if load_binary == nil then wotb.log.warn("loaders.load_binary: %s", err) end
local load_yaml, err = wotb.loaders.load_yaml("...", limits)  -- document: handle
if load_yaml == nil then wotb.log.warn("loaders.load_yaml: %s", err) end
```

