# `wotb.scene`

Raw-таблица интерфейса `WotbModV3SceneApiV1` (`include/wotbmod/scene_v1.h`, версия `WOTBMOD_V3_SCENE_VERSION`). Функции ниже вызываются как `wotb.scene.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `hangar.scene`, `resources.mod`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.scene.ATTACHMENT_DETACH_ON_PARENT_DESTROY` | `WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY` | enum WotbModV3SceneAttachmentPolicy |
| `wotb.scene.ATTACHMENT_DESTROY_ON_PARENT_DESTROY` | `WOTBMOD_V3_ATTACHMENT_DESTROY_ON_PARENT_DESTROY` | enum WotbModV3SceneAttachmentPolicy |
| `wotb.scene.ATTACHMENT_KEEP_WORLD_TRANSFORM` | `WOTBMOD_V3_ATTACHMENT_KEEP_WORLD_TRANSFORM` | enum WotbModV3SceneAttachmentPolicy |
| `wotb.scene.ATTACHMENT_KEEP_LOCAL_TRANSFORM` | `WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM` | enum WotbModV3SceneAttachmentPolicy |
| `wotb.scene.PARAMETER_FLOAT` | `WOTBMOD_V3_SCENE_PARAMETER_FLOAT` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_VEC2` | `WOTBMOD_V3_SCENE_PARAMETER_VEC2` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_VEC3` | `WOTBMOD_V3_SCENE_PARAMETER_VEC3` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_VEC4` | `WOTBMOD_V3_SCENE_PARAMETER_VEC4` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_COLOR` | `WOTBMOD_V3_SCENE_PARAMETER_COLOR` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_MATRIX4` | `WOTBMOD_V3_SCENE_PARAMETER_MATRIX4` | enum WotbModV3SceneParameterType |
| `wotb.scene.PARAMETER_TEXTURE_URI` | `WOTBMOD_V3_SCENE_PARAMETER_TEXTURE_URI` | enum WotbModV3SceneParameterType |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `entity_create` | descriptor: table:SceneEntityDescriptor | entity: handle |
| `entity_load` | resource_uri: string | entity: handle |
| `entity_clone` | entity: handle | clone: handle |
| `entity_destroy` | entity: handle | true |
| `get_active_scene` | — | scene: handle |
| `entity_get_parent` | entity: handle | parent: handle |
| `entity_set_parent` | entity: handle, parent: handle | true |
| `entity_add_child` | entity: handle, child: handle | true |
| `entity_remove_child` | entity: handle, child: handle | true |
| `entity_attach_ex` | entity: handle, parent: handle, node: string, policy: integer | true |
| `entity_set_transform` | entity: handle, transform: table:Transform | true |
| `entity_get_transform` | entity: handle | transform: table:Transform |
| `entity_get_world_transform` | entity: handle | transform: table:Transform |
| `entity_set_world_transform` | entity: handle, transform: table:Transform | true |
| `entity_get_bounds` | entity: handle | bounds: table:SceneBounds |
| `entity_set_render_layer` | entity: handle, layer: integer | true |
| `entity_set_render_order` | entity: handle, order: integer | true |
| `entity_set_lod_bias` | entity: handle, bias: number | true |
| `entity_list_nodes` | entity: handle, visitor: function | true |
| `entity_find_node_by_path` | entity: handle, path: string | node: handle |
| `entity_list_animations` | entity: handle, visitor: function | true |
| `entity_get_animation_duration` | entity: handle, animation: string | seconds: number |
| `entity_set_animation_speed` | entity: handle, animation: string, speed: number | true |
| `entity_set_animation_loop` | entity: handle, animation: string, loop: integer | true |
| `entity_blend_animation` | entity: handle, from_animation: string, to_animation: string, duration_seconds: number | true |
| `entity_set_material_parameter` | entity: handle, material_path: string, parameter: table:SceneParameter | true |
| `entity_clear_material_parameter` | entity: handle, material_path: string, parameter_name: string | true |
| `entity_set_shader_parameter` | entity: handle, parameter: table:SceneParameter | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["hangar.scene", "resources.mod"]
local entity_create, err = wotb.scene.entity_create(descriptor)  -- entity: handle
if entity_create == nil then wotb.log.warn("scene.entity_create: %s", err) end
local entity_load, err = wotb.scene.entity_load("...")  -- entity: handle
if entity_load == nil then wotb.log.warn("scene.entity_load: %s", err) end
local entity_clone, err = wotb.scene.entity_clone(handle)  -- clone: handle
if entity_clone == nil then wotb.log.warn("scene.entity_clone: %s", err) end
```

