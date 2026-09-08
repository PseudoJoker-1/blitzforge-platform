# `wotb.devtools`

Raw-таблица интерфейса `WotbModV3DevtoolsApiV3` (`include/wotbmod/devtools_v3.h`, версия `WOTBMOD_V3_DEVTOOLS_VERSION_3`). Функции ниже вызываются как `wotb.devtools.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `marker` | category: string, name: string, context_json: string | true |
| `span_begin` | category: string, name: string | span: handle |
| `span_end` | span: handle | info: table:ProfilerSpanInfo |
| `counter_set` | category: string, name: string, value: number | true |
| `inspect_ui` | selector: string | string |
| `inspect_scene` | selector: string | string |
| `inspect_material` | selector: string | string |
| `inspect_resource` | selector: string | string |
| `inspect_hook_chain` | selector: string | string |
| `inspect_events` | selector: string | string |
| `profiler_get_mod_cpu_time` | — | aggregate: table:ProfilerAggregate |
| `profiler_get_mod_memory` | — | memory: table:ProfilerMemory |
| `get_callback_profile` | — | profile: table:CallbackProfile |
| `set_callback_budget` | budget_microseconds: integer, max_callbacks_per_frame: integer | true |
| `reset_callback_profile` | — | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local marker_ok, err = wotb.devtools.marker("...", "...", "...")
if not marker_ok then wotb.log.warn("devtools.marker: %s", err) end
local span_begin, err = wotb.devtools.span_begin("...", "...")  -- span: handle
if span_begin == nil then wotb.log.warn("devtools.span_begin: %s", err) end
local span_end, err = wotb.devtools.span_end(handle)  -- info: table:ProfilerSpanInfo
if span_end == nil then wotb.log.warn("devtools.span_end: %s", err) end
```

