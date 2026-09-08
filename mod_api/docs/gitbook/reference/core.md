# `wotb.core`

Raw-таблица интерфейса `WotbModV3CoreApiV1` (`include/wotbmod/core_v1.h`, версия `WOTBMOD_V3_CORE_VERSION`). Функции ниже вызываются как `wotb.core.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.context`](../facades/wotb-context.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.core.LOG_TRACE` | `WOTBMOD_V3_LOG_TRACE` | enum WotbModV3LogLevel |
| `wotb.core.LOG_DEBUG` | `WOTBMOD_V3_LOG_DEBUG` | enum WotbModV3LogLevel |
| `wotb.core.LOG_INFO` | `WOTBMOD_V3_LOG_INFO` | enum WotbModV3LogLevel |
| `wotb.core.LOG_WARNING` | `WOTBMOD_V3_LOG_WARNING` | enum WotbModV3LogLevel |
| `wotb.core.LOG_ERROR` | `WOTBMOD_V3_LOG_ERROR` | enum WotbModV3LogLevel |
| `wotb.core.LOG_FATAL` | `WOTBMOD_V3_LOG_FATAL` | enum WotbModV3LogLevel |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `log` | level: integer, category: string, message: string | true |
| `get_context` | — | context_mask: integer |
| `get_thread_role` | — | thread_role: integer |
| `get_frame_index` | — | frame_index: integer |
| `get_game_directory` | — | string |
| `get_mod_data_directory` | — | string |
| `get_mod_cache_directory` | — | string |
| `get_mod_config_directory` | — | string |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local log_ok, err = wotb.core.log(0, "...", "...")
if not log_ok then wotb.log.warn("core.log: %s", err) end
local get_context, err = wotb.core.get_context()  -- context_mask: integer
if get_context == nil then wotb.log.warn("core.get_context: %s", err) end
local get_thread_role, err = wotb.core.get_thread_role()  -- thread_role: integer
if get_thread_role == nil then wotb.log.warn("core.get_thread_role: %s", err) end
```

