# `wotb.catalog`

Raw-таблица интерфейса `WotbModV3CatalogApiV1` (`include/wotbmod/catalog_v1.h`, версия `WOTBMOD_V3_CATALOG_VERSION`). Функции ниже вызываются как `wotb.catalog.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `content`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.catalog.STATUS_VERIFIED` | `WOTBMOD_V3_CATALOG_VERIFIED` | enum WotbModV3CatalogStatus |
| `wotb.catalog.STATUS_COMMUNITY` | `WOTBMOD_V3_CATALOG_COMMUNITY` | enum WotbModV3CatalogStatus |
| `wotb.catalog.STATUS_UNREVIEWED` | `WOTBMOD_V3_CATALOG_UNREVIEWED` | enum WotbModV3CatalogStatus |
| `wotb.catalog.STATUS_BANNED` | `WOTBMOD_V3_CATALOG_BANNED` | enum WotbModV3CatalogStatus |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `validate_record` | record: table:CatalogRecord | true |
| `evaluate_install` | record: table:CatalogRecord, manifest_id: string, manifest_version: string, package_sha256: string | decision: table:CatalogDecision |
| `status_name` | status: integer | string |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["content"]
local validate_record_ok, err = wotb.catalog.validate_record(record)
if not validate_record_ok then wotb.log.warn("catalog.validate_record: %s", err) end
local evaluate_install, err = wotb.catalog.evaluate_install(record, "...", "...", "...")  -- decision: table:CatalogDecision
if evaluate_install == nil then wotb.log.warn("catalog.evaluate_install: %s", err) end
local status_name, err = wotb.catalog.status_name(0)  -- string
if status_name == nil then wotb.log.warn("catalog.status_name: %s", err) end
```

