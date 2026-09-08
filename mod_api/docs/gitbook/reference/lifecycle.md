# `wotb.lifecycle`

Raw-таблица интерфейса `WotbModV3LifecycleApiV1` (`include/wotbmod/lifecycle_v1.h`, версия `WOTBMOD_V3_LIFECYCLE_VERSION`). Функции ниже вызываются как `wotb.lifecycle.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.mod`](../facades/wotb-mod.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `core`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.lifecycle.STATE_UNKNOWN` | `WOTBMOD_V3_MOD_STATE_UNKNOWN` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.STATE_LOADED` | `WOTBMOD_V3_MOD_STATE_LOADED` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.STATE_ENABLED` | `WOTBMOD_V3_MOD_STATE_ENABLED` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.STATE_DISABLED` | `WOTBMOD_V3_MOD_STATE_DISABLED` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.STATE_UNLOADING` | `WOTBMOD_V3_MOD_STATE_UNLOADING` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.STATE_UNLOADED` | `WOTBMOD_V3_MOD_STATE_UNLOADED` | enum WotbModV3LifecycleState |
| `wotb.lifecycle.TRANSITION_PRELOAD` | `WOTBMOD_V3_MOD_TRANSITION_PRELOAD` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.TRANSITION_LOADED` | `WOTBMOD_V3_MOD_TRANSITION_LOADED` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.TRANSITION_ENABLED` | `WOTBMOD_V3_MOD_TRANSITION_ENABLED` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.TRANSITION_DISABLED` | `WOTBMOD_V3_MOD_TRANSITION_DISABLED` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.TRANSITION_UNLOADING` | `WOTBMOD_V3_MOD_TRANSITION_UNLOADING` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.TRANSITION_UNLOADED` | `WOTBMOD_V3_MOD_TRANSITION_UNLOADED` | enum WotbModV3LifecycleTransition |
| `wotb.lifecycle.CLEANUP_RELEASED` | `WOTBMOD_V3_CLEANUP_RELEASED` | enum WotbModV3CleanupReason |
| `wotb.lifecycle.CLEANUP_MOD_DISABLED` | `WOTBMOD_V3_CLEANUP_MOD_DISABLED` | enum WotbModV3CleanupReason |
| `wotb.lifecycle.CLEANUP_MOD_UNLOADING` | `WOTBMOD_V3_CLEANUP_MOD_UNLOADING` | enum WotbModV3CleanupReason |
| `wotb.lifecycle.CLEANUP_RUNTIME_SHUTDOWN` | `WOTBMOD_V3_CLEANUP_RUNTIME_SHUTDOWN` | enum WotbModV3CleanupReason |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_current` | — | current_mod: handle |
| `get_info` | — | info: table:LifecycleInfo |
| `get_install_path` | — | string |
| `get_resource_path` | — | string |
| `get_data_path` | — | string |
| `get_cache_path` | — | string |
| `get_config_path` | — | string |
| `register_cleanup` | callback: function | token: handle |
| `unregister_cleanup` | token: handle | true |
| `request_enable` | target_mod: handle | true |
| `request_disable` | target_mod: handle | true |
| `request_reload` | target_mod: handle | true |
| `can_hot_reload` | target_mod: handle | can_hot_reload: integer, string |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["core"]
local get_current, err = wotb.lifecycle.get_current()  -- current_mod: handle
if get_current == nil then wotb.log.warn("lifecycle.get_current: %s", err) end
local get_info, err = wotb.lifecycle.get_info()  -- info: table:LifecycleInfo
if get_info == nil then wotb.log.warn("lifecycle.get_info: %s", err) end
local get_install_path, err = wotb.lifecycle.get_install_path()  -- string
if get_install_path == nil then wotb.log.warn("lifecycle.get_install_path: %s", err) end
```

