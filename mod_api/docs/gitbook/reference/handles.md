# `wotb.handles`

Raw-таблица интерфейса `WotbModV3HandlesApiV1` (`include/wotbmod/handles_v1.h`, версия `WOTBMOD_V3_HANDLES_VERSION`). Функции ниже вызываются как `wotb.handles.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.screen`](../facades/wotb-screen.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `retain` | handle: handle | true |
| `release` | handle: handle | true |
| `get_info` | handle: handle | info: table:HandleInfo |
| `is_alive` | handle: handle | alive: integer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local retain_ok, err = wotb.handles.retain(handle)
if not retain_ok then wotb.log.warn("handles.retain: %s", err) end
local release_ok, err = wotb.handles.release(handle)
if not release_ok then wotb.log.warn("handles.release: %s", err) end
local get_info, err = wotb.handles.get_info(handle)  -- info: table:HandleInfo
if get_info == nil then wotb.log.warn("handles.get_info: %s", err) end
```

