# `wotb.session_cluster`

Raw-таблица интерфейса `WotbModV3SessionClusterApiV1` (`include/wotbmod/session_cluster_v1.h`, версия `WOTBMOD_V3_SESSION_CLUSTER_VERSION`). Функции ниже вызываются как `wotb.session_cluster.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.session`](../facades/wotb-session.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `session.cluster.read`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.session_cluster.AUTO` | `WOTBMOD_V3_SESSION_CLUSTER_AUTO` | define |
| `wotb.session_cluster.CHANGE_QUEUED` | `WOTBMOD_V3_CLUSTER_CHANGE_QUEUED` | enum WotbModV3ClusterChangeStatus |
| `wotb.session_cluster.CHANGE_STARTED` | `WOTBMOD_V3_CLUSTER_CHANGE_STARTED` | enum WotbModV3ClusterChangeStatus |
| `wotb.session_cluster.CHANGE_CONNECTED` | `WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED` | enum WotbModV3ClusterChangeStatus |
| `wotb.session_cluster.CHANGE_FAILED` | `WOTBMOD_V3_CLUSTER_CHANGE_FAILED` | enum WotbModV3ClusterChangeStatus |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `enumerate` | — | items: array |
| `get_current` | — | info: table:ClusterInfo |
| `change` | cluster_id: integer | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["session.cluster.read"]
local enumerate, err = wotb.session_cluster.enumerate()  -- items: array
if enumerate == nil then wotb.log.warn("session_cluster.enumerate: %s", err) end
local get_current, err = wotb.session_cluster.get_current()  -- info: table:ClusterInfo
if get_current == nil then wotb.log.warn("session_cluster.get_current: %s", err) end
local change_ok, err = wotb.session_cluster.change(0)
if not change_ok then wotb.log.warn("session_cluster.change: %s", err) end
```

