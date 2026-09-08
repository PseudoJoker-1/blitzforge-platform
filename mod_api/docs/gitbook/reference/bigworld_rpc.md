# `wotb.bigworld_rpc`

Raw-таблица интерфейса `WotbModV3BigWorldRpcApiV1` (`include/wotbmod/bigworld_rpc_v1.h`, версия `WOTBMOD_V3_BIGWORLD_RPC_VERSION`). Функции ниже вызываются как `wotb.bigworld_rpc.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `bigworld.rpc.observe`, `bigworld.observe`, `bigworld.rpc.metadata`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.bigworld_rpc.RPC_INCOMING` | `WOTBMOD_V3_RPC_INCOMING` | enum WotbModV3RpcDirection |
| `wotb.bigworld_rpc.RPC_OUTGOING` | `WOTBMOD_V3_RPC_OUTGOING` | enum WotbModV3RpcDirection |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_policy` | — | policy: table:BigWorldRpcPolicy |
| `subscribe_observed` | direction_mask: integer, method_filter: string, callback: function | token: handle |
| `unsubscribe_observed` | token: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["bigworld.rpc.observe", "bigworld.observe", "bigworld.rpc.metadata"]
local get_policy, err = wotb.bigworld_rpc.get_policy()  -- policy: table:BigWorldRpcPolicy
if get_policy == nil then wotb.log.warn("bigworld_rpc.get_policy: %s", err) end
local subscribe_observed, err = wotb.bigworld_rpc.subscribe_observed(0, "...", function(...) end)  -- token: handle
if subscribe_observed == nil then wotb.log.warn("bigworld_rpc.subscribe_observed: %s", err) end
local unsubscribe_observed_ok, err = wotb.bigworld_rpc.unsubscribe_observed(handle)
if not unsubscribe_observed_ok then wotb.log.warn("bigworld_rpc.unsubscribe_observed: %s", err) end
```

