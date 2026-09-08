# `wotb.async`

Raw-таблица интерфейса `WotbModV3AsyncApiV1` (`include/wotbmod/async_v1.h`, версия `WOTBMOD_V3_ASYNC_VERSION`). Функции ниже вызываются как `wotb.async.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.async.MAX_TASK_RESULT` | `WOTBMOD_V3_MAX_TASK_RESULT` | define |
| `wotb.async.TASK_QUEUED` | `WOTBMOD_V3_TASK_QUEUED` | enum WotbModV3TaskState |
| `wotb.async.TASK_RUNNING` | `WOTBMOD_V3_TASK_RUNNING` | enum WotbModV3TaskState |
| `wotb.async.TASK_COMPLETED` | `WOTBMOD_V3_TASK_COMPLETED` | enum WotbModV3TaskState |
| `wotb.async.TASK_FAILED` | `WOTBMOD_V3_TASK_FAILED` | enum WotbModV3TaskState |
| `wotb.async.TASK_CANCELLED` | `WOTBMOD_V3_TASK_CANCELLED` | enum WotbModV3TaskState |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `task_submit` | info: table:TaskSubmitInfo | task: handle |
| `task_cancel` | task: handle | true |
| `task_get_info` | task: handle | info: table:TaskInfo |
| `task_get_state` | task: handle | state: integer |
| `task_get_progress` | task: handle | progress: number |
| `task_set_progress` | task: handle, progress: number | true |
| `task_set_result` | task: handle, data: void, size: integer | true |
| `task_get_result` | task: handle | result: table:Buffer |
| `task_is_cancellation_requested` | task: handle | requested: integer |
| `dispatch_to_main_thread` | callback: function | task: handle |
| `dispatch_to_render_thread` | callback: function | task: handle |
| `dispatch_to_audio_thread` | callback: function | task: handle |
| `pump_current_thread` | max_callbacks: integer | executed: integer |
| `timer_create` | info: table:TimerCreateInfo | timer: handle |
| `timer_cancel` | timer: handle | true |
| `timer_pause` | timer: handle | true |
| `timer_resume` | timer: handle | true |
| `timer_get_info` | timer: handle | info: table:TimerInfo |
| `thread_get_current_role` | — | role: integer |
| `thread_is_main` | — | is_main: integer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local task_submit, err = wotb.async.task_submit(info)  -- task: handle
if task_submit == nil then wotb.log.warn("async.task_submit: %s", err) end
local task_cancel_ok, err = wotb.async.task_cancel(handle)
if not task_cancel_ok then wotb.log.warn("async.task_cancel: %s", err) end
local task_get_info, err = wotb.async.task_get_info(handle)  -- info: table:TaskInfo
if task_get_info == nil then wotb.log.warn("async.task_get_info: %s", err) end
```

