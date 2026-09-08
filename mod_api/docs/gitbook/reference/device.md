# `wotb.device`

Raw-таблица интерфейса `WotbModV3DeviceApiV1` (`include/wotbmod/device_v1.h`, версия `WOTBMOD_V3_DEVICE_VERSION`). Функции ниже вызываются как `wotb.device.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.device.ARCH_UNKNOWN` | `WOTBMOD_V3_ARCH_UNKNOWN` | enum WotbModV3ProcessorArchitecture |
| `wotb.device.ARCH_X86` | `WOTBMOD_V3_ARCH_X86` | enum WotbModV3ProcessorArchitecture |
| `wotb.device.ARCH_X64` | `WOTBMOD_V3_ARCH_X64` | enum WotbModV3ProcessorArchitecture |
| `wotb.device.ARCH_ARM32` | `WOTBMOD_V3_ARCH_ARM32` | enum WotbModV3ProcessorArchitecture |
| `wotb.device.ARCH_ARM64` | `WOTBMOD_V3_ARCH_ARM64` | enum WotbModV3ProcessorArchitecture |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_info` | — | info: table:DeviceInfo |
| `get_graphics_adapter_count` | — | count: integer |
| `get_graphics_adapter_at` | index: integer | info: table:GraphicsAdapterInfo |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local get_info, err = wotb.device.get_info()  -- info: table:DeviceInfo
if get_info == nil then wotb.log.warn("device.get_info: %s", err) end
local get_graphics_adapter_count, err = wotb.device.get_graphics_adapter_count()  -- count: integer
if get_graphics_adapter_count == nil then wotb.log.warn("device.get_graphics_adapter_count: %s", err) end
local get_graphics_adapter_at, err = wotb.device.get_graphics_adapter_at(0)  -- info: table:GraphicsAdapterInfo
if get_graphics_adapter_at == nil then wotb.log.warn("device.get_graphics_adapter_at: %s", err) end
```

