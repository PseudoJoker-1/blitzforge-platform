# `wotb.scene_enumerate`

Raw-таблица интерфейса `WotbModV3SceneApiV2` (`include/wotbmod/scene_v2.h`, версия `WOTBMOD_V3_SCENE_VERSION_2`). Функции ниже вызываются как `wotb.scene_enumerate.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `game.entity.public`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.scene_enumerate.WALK_MAX_DEPTH` | `WOTBMOD_V3_SCENE_WALK_MAX_DEPTH` | define |
| `wotb.scene_enumerate.WALK_MAX_CHILDREN` | `WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN` | define |
| `wotb.scene_enumerate.WALK_MAX_NODES` | `WOTBMOD_V3_SCENE_WALK_MAX_NODES` | define |
| `wotb.scene_enumerate.NODE_NAME_SIZE` | `WOTBMOD_V3_SCENE_NODE_NAME_SIZE` | define |
| `wotb.scene_enumerate.NODE_NO_PARENT` | `WOTBMOD_V3_SCENE_NODE_NO_PARENT` | define |
| `wotb.scene_enumerate.NODE_IS_SCENE` | `WOTBMOD_V3_SCENE_NODE_IS_SCENE` | enum WotbModV3SceneNodeFlag |
| `wotb.scene_enumerate.NODE_HAS_WORLD_MATRIX` | `WOTBMOD_V3_SCENE_NODE_HAS_WORLD_MATRIX` | enum WotbModV3SceneNodeFlag |
| `wotb.scene_enumerate.NODE_NAME_TRUNCATED` | `WOTBMOD_V3_SCENE_NODE_NAME_TRUNCATED` | enum WotbModV3SceneNodeFlag |
| `wotb.scene_enumerate.NODE_CHILDREN_TRUNCATED` | `WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED` | enum WotbModV3SceneNodeFlag |
| `wotb.scene_enumerate.NODE_DEPTH_LIMITED` | `WOTBMOD_V3_SCENE_NODE_DEPTH_LIMITED` | enum WotbModV3SceneNodeFlag |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_limits` | — | limits: table:SceneWalkLimits |
| `walk_active_scene` | request: table:SceneWalkRequest | nodes: table:SceneNodeRecord |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["game.entity.public"]
local get_limits, err = wotb.scene_enumerate.get_limits()  -- limits: table:SceneWalkLimits
if get_limits == nil then wotb.log.warn("scene_enumerate.get_limits: %s", err) end
local walk_active_scene, err = wotb.scene_enumerate.walk_active_scene(request)  -- nodes: table:SceneNodeRecord
if walk_active_scene == nil then wotb.log.warn("scene_enumerate.walk_active_scene: %s", err) end
```

