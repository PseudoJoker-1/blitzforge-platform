# `wotb.hooks`

Raw-таблица интерфейса `WotbModV3HooksApiV1` (`include/wotbmod/hooks_v1.h`, версия `WOTBMOD_V3_HOOKS_VERSION`). Функции ниже вызываются как `wotb.hooks.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `hooks.symbol`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.hooks.MAX_TARGET` | `WOTBMOD_V3_MAX_HOOK_TARGET` | define |
| `wotb.hooks.MODE_BEFORE` | `WOTBMOD_V3_HOOK_BEFORE` | enum WotbModV3HookMode |
| `wotb.hooks.MODE_AFTER` | `WOTBMOD_V3_HOOK_AFTER` | enum WotbModV3HookMode |
| `wotb.hooks.MODE_AROUND` | `WOTBMOD_V3_HOOK_AROUND` | enum WotbModV3HookMode |
| `wotb.hooks.MODE_REPLACE` | `WOTBMOD_V3_HOOK_REPLACE` | enum WotbModV3HookMode |
| `wotb.hooks.MODE_OBSERVE` | `WOTBMOD_V3_HOOK_OBSERVE` | enum WotbModV3HookMode |
| `wotb.hooks.STATUS_PENDING_BACKEND` | `WOTBMOD_V3_HOOK_STATUS_PENDING_BACKEND` | enum WotbModV3HookStatus |
| `wotb.hooks.STATUS_DISABLED` | `WOTBMOD_V3_HOOK_STATUS_DISABLED` | enum WotbModV3HookStatus |
| `wotb.hooks.STATUS_ENABLED` | `WOTBMOD_V3_HOOK_STATUS_ENABLED` | enum WotbModV3HookStatus |
| `wotb.hooks.STATUS_CONFLICT` | `WOTBMOD_V3_HOOK_STATUS_CONFLICT` | enum WotbModV3HookStatus |
| `wotb.hooks.STATUS_REMOVED` | `WOTBMOD_V3_HOOK_STATUS_REMOVED` | enum WotbModV3HookStatus |
| `wotb.hooks.STATUS_FAILED` | `WOTBMOD_V3_HOOK_STATUS_FAILED` | enum WotbModV3HookStatus |
| `wotb.hooks.CREATE_NONE` | `WOTBMOD_V3_HOOK_CREATE_NONE` | enum WotbModV3HookCreateFlags |
| `wotb.hooks.CREATE_ALLOW_PENDING` | `WOTBMOD_V3_HOOK_CREATE_ALLOW_PENDING` | enum WotbModV3HookCreateFlags |
| `wotb.hooks.CREATE_ALLOW_CONFLICT` | `WOTBMOD_V3_HOOK_CREATE_ALLOW_CONFLICT` | enum WotbModV3HookCreateFlags |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `create_symbol` | symbol: string, info: table:HookCreateInfo | hook: handle |
| `create_vtable` | object: void, slot: integer, info: table:HookCreateInfo | hook: handle |
| `enable` | hook: handle | true |
| `disable` | hook: handle | true |
| `remove` | hook: handle | true |
| `set_priority` | hook: handle, priority: integer | true |
| `run_before` | hook: handle, other_hook: handle | true |
| `run_after` | hook: handle, other_hook: handle | true |
| `get_original` | hook: handle | original: void |
| `call_next` | hook: handle, arguments: table:ConstBuffer, result: table:Buffer | true |
| `get_info` | hook: handle | info: table:HookInfo |
| `get_owner` | hook: handle | owner: handle |
| `get_status` | hook: handle | status: integer |
| `get_chain` | target: string | hooks: array |
| `get_conflicts` | hook: handle | conflicts: array |
| `enumerate` | — | hooks: array |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["hooks.symbol"]
local create_symbol, err = wotb.hooks.create_symbol("...", info)  -- hook: handle
if create_symbol == nil then wotb.log.warn("hooks.create_symbol: %s", err) end
local create_vtable, err = wotb.hooks.create_vtable(object, 0, info)  -- hook: handle
if create_vtable == nil then wotb.log.warn("hooks.create_vtable: %s", err) end
local enable_ok, err = wotb.hooks.enable(handle)
if not enable_ok then wotb.log.warn("hooks.enable: %s", err) end
```

