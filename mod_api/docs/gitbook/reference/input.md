# `wotb.input`

Raw-таблица интерфейса `WotbModV3InputApiV1` (`include/wotbmod/input_v1.h`, версия `WOTBMOD_V3_INPUT_VERSION`). Функции ниже вызываются как `wotb.input.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.keys`](../facades/wotb-keys.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `input.actions`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.input.ACTION_ID_MAX` | `WOTBMOD_V3_INPUT_ACTION_ID_MAX` | define |
| `wotb.input.BINDINGS_MAX` | `WOTBMOD_V3_INPUT_BINDINGS_MAX` | define |
| `wotb.input.MOUSE_LEFT` | `WOTBMOD_V3_INPUT_MOUSE_LEFT` | define |
| `wotb.input.MOUSE_RIGHT` | `WOTBMOD_V3_INPUT_MOUSE_RIGHT` | define |
| `wotb.input.MOUSE_MIDDLE` | `WOTBMOD_V3_INPUT_MOUSE_MIDDLE` | define |
| `wotb.input.MOUSE_X1` | `WOTBMOD_V3_INPUT_MOUSE_X1` | define |
| `wotb.input.MOUSE_X2` | `WOTBMOD_V3_INPUT_MOUSE_X2` | define |
| `wotb.input.MOUSE_WHEEL` | `WOTBMOD_V3_INPUT_MOUSE_WHEEL` | define |
| `wotb.input.MOUSE_MOVE_X` | `WOTBMOD_V3_INPUT_MOUSE_MOVE_X` | define |
| `wotb.input.MOUSE_MOVE_Y` | `WOTBMOD_V3_INPUT_MOUSE_MOVE_Y` | define |
| `wotb.input.DEVICE_KEYBOARD` | `WOTBMOD_V3_INPUT_DEVICE_KEYBOARD` | enum WotbModV3InputDevice |
| `wotb.input.DEVICE_MOUSE` | `WOTBMOD_V3_INPUT_DEVICE_MOUSE` | enum WotbModV3InputDevice |
| `wotb.input.DEVICE_GAMEPAD` | `WOTBMOD_V3_INPUT_DEVICE_GAMEPAD` | enum WotbModV3InputDevice |
| `wotb.input.DEVICE_TOUCH` | `WOTBMOD_V3_INPUT_DEVICE_TOUCH` | enum WotbModV3InputDevice |
| `wotb.input.VALUE_BUTTON` | `WOTBMOD_V3_INPUT_VALUE_BUTTON` | enum WotbModV3InputValueType |
| `wotb.input.VALUE_AXIS` | `WOTBMOD_V3_INPUT_VALUE_AXIS` | enum WotbModV3InputValueType |
| `wotb.input.MOD_NONE` | `WOTBMOD_V3_INPUT_MOD_NONE` | enum WotbModV3InputModifiers |
| `wotb.input.MOD_SHIFT` | `WOTBMOD_V3_INPUT_MOD_SHIFT` | enum WotbModV3InputModifiers |
| `wotb.input.MOD_CONTROL` | `WOTBMOD_V3_INPUT_MOD_CONTROL` | enum WotbModV3InputModifiers |
| `wotb.input.MOD_ALT` | `WOTBMOD_V3_INPUT_MOD_ALT` | enum WotbModV3InputModifiers |
| `wotb.input.MOD_META` | `WOTBMOD_V3_INPUT_MOD_META` | enum WotbModV3InputModifiers |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `register_action` | desc: table:InputActionDesc | action: handle |
| `unregister_action` | action: handle | true |
| `set_contexts` | action: handle, contexts: integer | true |
| `get_bindings` | action: handle | bindings: array |
| `set_bindings` | action: handle, bindings: array | true |
| `subscribe` | action: handle, callback: function | token: handle |
| `is_action_down` | action: handle | down: integer |
| `is_action_pressed` | action: handle | pressed: integer |
| `get_axis` | action: handle | value: number |
| `capture_begin` | contexts: integer | true |
| `capture_end` | — | binding: table:InputBinding |
| `find_conflicts` | action: handle | conflicts: array |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["input.actions"]
local register_action, err = wotb.input.register_action(desc)  -- action: handle
if register_action == nil then wotb.log.warn("input.register_action: %s", err) end
local unregister_action_ok, err = wotb.input.unregister_action(handle)
if not unregister_action_ok then wotb.log.warn("input.unregister_action: %s", err) end
local set_contexts_ok, err = wotb.input.set_contexts(handle, 0)
if not set_contexts_ok then wotb.log.warn("input.set_contexts: %s", err) end
```

