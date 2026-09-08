# `wotb.render`

Raw-таблица интерфейса `WotbModV3RenderApiV1` (`include/wotbmod/render_v1.h`, версия `WOTBMOD_V3_RENDER_VERSION`). Функции ниже вызываются как `wotb.render.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `battle.render.overlay`, `render.callbacks`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.render.BACKEND_NONE` | `WOTBMOD_V3_RENDER_BACKEND_NONE` | enum WotbModV3RenderBackend |
| `wotb.render.BACKEND_D3D11` | `WOTBMOD_V3_RENDER_BACKEND_D3D11` | enum WotbModV3RenderBackend |
| `wotb.render.BACKEND_D3D12` | `WOTBMOD_V3_RENDER_BACKEND_D3D12` | enum WotbModV3RenderBackend |
| `wotb.render.BACKEND_VULKAN` | `WOTBMOD_V3_RENDER_BACKEND_VULKAN` | enum WotbModV3RenderBackend |
| `wotb.render.BACKEND_METAL` | `WOTBMOD_V3_RENDER_BACKEND_METAL` | enum WotbModV3RenderBackend |
| `wotb.render.BACKEND_OPENGL` | `WOTBMOD_V3_RENDER_BACKEND_OPENGL` | enum WotbModV3RenderBackend |
| `wotb.render.PHASE_BEFORE_UI` | `WOTBMOD_V3_RENDER_PHASE_BEFORE_UI` | enum WotbModV3RenderPhase |
| `wotb.render.PHASE_AFTER_UI` | `WOTBMOD_V3_RENDER_PHASE_AFTER_UI` | enum WotbModV3RenderPhase |
| `wotb.render.PHASE_PRESENT` | `WOTBMOD_V3_RENDER_PHASE_PRESENT` | enum WotbModV3RenderPhase |
| `wotb.render.RESOURCE_TEXTURE` | `WOTBMOD_V3_RENDER_RESOURCE_TEXTURE` | enum WotbModV3RenderResourceType |
| `wotb.render.RESOURCE_MATERIAL` | `WOTBMOD_V3_RENDER_RESOURCE_MATERIAL` | enum WotbModV3RenderResourceType |
| `wotb.render.TEXTURE_RGBA8_UNORM` | `WOTBMOD_V3_TEXTURE_RGBA8_UNORM` | enum WotbModV3TextureFormat |
| `wotb.render.TEXTURE_BGRA8_UNORM` | `WOTBMOD_V3_TEXTURE_BGRA8_UNORM` | enum WotbModV3TextureFormat |
| `wotb.render.TEXTURE_R8_UNORM` | `WOTBMOD_V3_TEXTURE_R8_UNORM` | enum WotbModV3TextureFormat |
| `wotb.render.TEXTURE_RGBA16_FLOAT` | `WOTBMOD_V3_TEXTURE_RGBA16_FLOAT` | enum WotbModV3TextureFormat |
| `wotb.render.PARAMETER_FLOAT` | `WOTBMOD_V3_RENDER_PARAMETER_FLOAT` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_VEC2` | `WOTBMOD_V3_RENDER_PARAMETER_VEC2` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_VEC3` | `WOTBMOD_V3_RENDER_PARAMETER_VEC3` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_VEC4` | `WOTBMOD_V3_RENDER_PARAMETER_VEC4` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_COLOR` | `WOTBMOD_V3_RENDER_PARAMETER_COLOR` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_MATRIX4` | `WOTBMOD_V3_RENDER_PARAMETER_MATRIX4` | enum WotbModV3RenderParameterType |
| `wotb.render.PARAMETER_TEXTURE` | `WOTBMOD_V3_RENDER_PARAMETER_TEXTURE` | enum WotbModV3RenderParameterType |
| `wotb.render.LIFECYCLE_BACKEND` | `WOTBMOD_V3_RENDER_LIFECYCLE_BACKEND` | enum WotbModV3RenderLifecycleChange |
| `wotb.render.LIFECYCLE_DEVICE` | `WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE` | enum WotbModV3RenderLifecycleChange |
| `wotb.render.LIFECYCLE_CONTEXT` | `WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT` | enum WotbModV3RenderLifecycleChange |
| `wotb.render.LIFECYCLE_SWAPCHAIN` | `WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN` | enum WotbModV3RenderLifecycleChange |
| `wotb.render.LIFECYCLE_VIEWPORT` | `WOTBMOD_V3_RENDER_LIFECYCLE_VIEWPORT` | enum WotbModV3RenderLifecycleChange |
| `wotb.render.LIFECYCLE_AVAILABILITY` | `WOTBMOD_V3_RENDER_LIFECYCLE_AVAILABILITY` | enum WotbModV3RenderLifecycleChange |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `register_callback` | phase: integer, priority: integer, callback: function | token: handle |
| `unregister_callback` | token: handle | true |
| `set_callback_priority` | token: handle, priority: integer | true |
| `get_backend` | — | backend: integer |
| `get_viewport` | — | viewport: table:Rect |
| `get_frame_index` | — | frame_index: integer |
| `get_delta_time` | — | delta_seconds: number |
| `create_texture` | descriptor: table:TextureDescriptor | texture: handle |
| `update_texture` | texture: handle, region: table:Rect, data: void, data_size: integer, row_pitch: integer | true |
| `destroy_texture` | texture: handle | true |
| `create_material` | descriptor: table:MaterialDescriptor | material: handle |
| `set_material_parameter` | material: handle, parameter: table:RenderParameter | true |
| `destroy_material` | material: handle | true |
| `draw_sprite` | draw: table:DrawSprite | true |
| `draw_text` | draw: table:DrawText | true |
| `draw_line` | draw: table:DrawLine | true |
| `draw_mesh` | draw: table:DrawMesh | true |
| `push_state` | — | true |
| `pop_state` | — | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["battle.render.overlay", "render.callbacks"]
local register_callback, err = wotb.render.register_callback(0, 0, function(...) end)  -- token: handle
if register_callback == nil then wotb.log.warn("render.register_callback: %s", err) end
local unregister_callback_ok, err = wotb.render.unregister_callback(handle)
if not unregister_callback_ok then wotb.log.warn("render.unregister_callback: %s", err) end
local set_callback_priority_ok, err = wotb.render.set_callback_priority(handle, 0)
if not set_callback_priority_ok then wotb.log.warn("render.set_callback_priority: %s", err) end
```

