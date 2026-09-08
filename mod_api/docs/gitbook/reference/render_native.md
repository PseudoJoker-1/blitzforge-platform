# `wotb.render_native`

Raw-таблица интерфейса `WotbModV3RenderNativeApiV1` (`include/wotbmod/render_v1.h`, версия `WOTBMOD_V3_RENDER_NATIVE_VERSION`). Функции ниже вызываются как `wotb.render_native.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `render.native`. Имена объявляются в `permissions` манифеста.

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_native_device` | — | device: void |
| `get_native_context` | — | context: void |
| `get_native_swapchain` | — | swapchain: void |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["render.native"]
local get_native_device, err = wotb.render_native.get_native_device()  -- device: void
if get_native_device == nil then wotb.log.warn("render_native.get_native_device: %s", err) end
local get_native_context, err = wotb.render_native.get_native_context()  -- context: void
if get_native_context == nil then wotb.log.warn("render_native.get_native_context: %s", err) end
local get_native_swapchain, err = wotb.render_native.get_native_swapchain()  -- swapchain: void
if get_native_swapchain == nil then wotb.log.warn("render_native.get_native_swapchain: %s", err) end
```

