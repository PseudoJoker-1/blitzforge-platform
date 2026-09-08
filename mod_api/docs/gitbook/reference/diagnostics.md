# `wotb.diagnostics`

Raw-таблица интерфейса `WotbModV3DiagnosticsApiV2` (`include/wotbmod/diagnostics_v2.h`, версия `WOTBMOD_V3_DIAGNOSTICS_VERSION_2`). Функции ниже вызываются как `wotb.diagnostics.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.diagnostics.MAX_CONTEXT_ENTRIES` | `WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES` | define |
| `wotb.diagnostics.MAX_BREADCRUMBS` | `WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS` | define |
| `wotb.diagnostics.MAX_CONTEXT_KEY` | `WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_KEY` | define |
| `wotb.diagnostics.MAX_CONTEXT_VALUE` | `WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_VALUE` | define |
| `wotb.diagnostics.MAX_ACTION` | `WOTBMOD_V3_DIAGNOSTICS_MAX_ACTION` | define |
| `wotb.diagnostics.MAX_BREADCRUMB_CATEGORY` | `WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CATEGORY` | define |
| `wotb.diagnostics.MAX_BREADCRUMB_MESSAGE` | `WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_MESSAGE` | define |
| `wotb.diagnostics.MAX_BREADCRUMB_CONTEXT` | `WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CONTEXT` | define |
| `wotb.diagnostics.MOD_CREATED` | `WOTBMOD_V3_DIAGNOSTIC_MOD_CREATED` | enum WotbModV3DiagnosticModState |
| `wotb.diagnostics.MOD_LOADED` | `WOTBMOD_V3_DIAGNOSTIC_MOD_LOADED` | enum WotbModV3DiagnosticModState |
| `wotb.diagnostics.MOD_ENABLED` | `WOTBMOD_V3_DIAGNOSTIC_MOD_ENABLED` | enum WotbModV3DiagnosticModState |
| `wotb.diagnostics.MOD_DISABLED` | `WOTBMOD_V3_DIAGNOSTIC_MOD_DISABLED` | enum WotbModV3DiagnosticModState |
| `wotb.diagnostics.MOD_UNLOADING` | `WOTBMOD_V3_DIAGNOSTIC_MOD_UNLOADING` | enum WotbModV3DiagnosticModState |
| `wotb.diagnostics.MOD_FAULTED` | `WOTBMOD_V3_DIAGNOSTIC_MOD_FAULTED` | enum WotbModV3DiagnosticModState |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_stats` | — | stats: table:DiagnosticsStats |
| `copy_report_json` | — | string |
| `report_fault` | category: string, message: string, context_json: string | true |
| `crash_add_context` | key: string, value: string | true |
| `crash_set_last_action` | action: string | true |
| `crash_add_breadcrumb` | category: string, message: string, context_json: string | true |
| `export_bundle` | bundle_name: string | string |
| `get_mod_health` | — | health: table:ModHealth |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local get_stats, err = wotb.diagnostics.get_stats()  -- stats: table:DiagnosticsStats
if get_stats == nil then wotb.log.warn("diagnostics.get_stats: %s", err) end
local copy_report_json, err = wotb.diagnostics.copy_report_json()  -- string
if copy_report_json == nil then wotb.log.warn("diagnostics.copy_report_json: %s", err) end
local report_fault_ok, err = wotb.diagnostics.report_fault("...", "...", "...")
if not report_fault_ok then wotb.log.warn("diagnostics.report_fault: %s", err) end
```

