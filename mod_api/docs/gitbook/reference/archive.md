# `wotb.archive`

Raw-таблица интерфейса `WotbModV3ArchiveApiV1` (`include/wotbmod/archive_v1.h`, версия `WOTBMOD_V3_ARCHIVE_VERSION`). Функции ниже вызываются как `wotb.archive.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `resources.mod`. Имена объявляются в `permissions` манифеста.

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `open_directory` | physical_directory: string, limits: table:ArchiveLimits | archive: handle |
| `open_package_file` | physical_file: string, limits: table:ArchiveLimits | archive: handle |
| `get_entry_count` | archive: handle | count: integer |
| `get_entry` | archive: handle, index: integer | entry: table:ArchiveEntry |
| `read_entry` | archive: handle, relative_path: string | bytes |
| `extract_entry` | archive: handle, relative_path: string, destination_uri: string | true |
| `extract_all` | archive: handle, destination_uri: string | true |
| `get_sha256` | archive: handle | sha256: string |
| `verify_sha256` | archive: handle, expected_sha256: string | true |
| `cancel` | archive: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["resources.mod"]
local open_directory, err = wotb.archive.open_directory("...", limits)  -- archive: handle
if open_directory == nil then wotb.log.warn("archive.open_directory: %s", err) end
local open_package_file, err = wotb.archive.open_package_file("...", limits)  -- archive: handle
if open_package_file == nil then wotb.log.warn("archive.open_package_file: %s", err) end
local get_entry_count, err = wotb.archive.get_entry_count(handle)  -- count: integer
if get_entry_count == nil then wotb.log.warn("archive.get_entry_count: %s", err) end
```

