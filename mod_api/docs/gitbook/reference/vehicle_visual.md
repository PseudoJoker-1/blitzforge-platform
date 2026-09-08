# `wotb.vehicle_visual`

Raw-таблица интерфейса `WotbModV3VehicleVisualApiV2` (`include/wotbmod/vehicle_visual_v2.h`, версия `WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2`). Функции ниже вызываются как `wotb.vehicle_visual.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.vehicle`](../facades/wotb-vehicle.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.vehicle`, `vehicle.local.cosmetic`, `game.entity.public`, `resources.mod`, `resources.overlay.game`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.vehicle_visual.APPEARANCE_UNKNOWN` | `WOTBMOD_V3_VEHICLE_APPEARANCE_UNKNOWN` | enum WotbModV3VehicleAppearanceState |
| `wotb.vehicle_visual.APPEARANCE_LOADING` | `WOTBMOD_V3_VEHICLE_APPEARANCE_LOADING` | enum WotbModV3VehicleAppearanceState |
| `wotb.vehicle_visual.APPEARANCE_READY` | `WOTBMOD_V3_VEHICLE_APPEARANCE_READY` | enum WotbModV3VehicleAppearanceState |
| `wotb.vehicle_visual.APPEARANCE_DESTROYED` | `WOTBMOD_V3_VEHICLE_APPEARANCE_DESTROYED` | enum WotbModV3VehicleAppearanceState |
| `wotb.vehicle_visual.PART_HULL` | `WOTBMOD_V3_VEHICLE_PART_HULL` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.PART_TURRET` | `WOTBMOD_V3_VEHICLE_PART_TURRET` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.PART_GUN` | `WOTBMOD_V3_VEHICLE_PART_GUN` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.PART_TRACKS` | `WOTBMOD_V3_VEHICLE_PART_TRACKS` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.PART_CHASSIS` | `WOTBMOD_V3_VEHICLE_PART_CHASSIS` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.PART_EFFECTS` | `WOTBMOD_V3_VEHICLE_PART_EFFECTS` | enum WotbModV3VehiclePart |
| `wotb.vehicle_visual.SKIN_MAX_ASSETS` | `WOTBMOD_V3_VEHICLE_SKIN_MAX_ASSETS` | define |
| `wotb.vehicle_visual.SKIN_MESH` | `WOTBMOD_V3_VEHICLE_SKIN_MESH` | enum WotbModV3VehicleSkinAssetKind |
| `wotb.vehicle_visual.SKIN_MATERIAL` | `WOTBMOD_V3_VEHICLE_SKIN_MATERIAL` | enum WotbModV3VehicleSkinAssetKind |
| `wotb.vehicle_visual.SKIN_TEXTURE` | `WOTBMOD_V3_VEHICLE_SKIN_TEXTURE` | enum WotbModV3VehicleSkinAssetKind |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_local_player_vehicle` | — | vehicle: handle |
| `is_local_player` | vehicle: handle | is_local: integer |
| `is_hangar_vehicle` | vehicle: handle | is_hangar: integer |
| `get_appearance_state` | vehicle: handle | state: integer |
| `get_enemy` | vehicle: handle | is_enemy: integer |
| `get_enemy_name` | vehicle: handle | string |
| `get_position` | vehicle: handle | position: table:Vec3 |
| `get_part_entity` | vehicle: handle, part: integer | entity: handle |
| `find_attachment_point` | vehicle: handle, point_name: string | world_transform: table:Transform |
| `attach_entity_ex` | attachment: table:VehicleAttachment | true |
| `set_decal_override` | vehicle: handle, slot: string, texture_uri: string | true |
| `set_texture_override` | vehicle: handle, material_path: string, texture_slot: string, texture_uri: string | true |
| `set_color_override` | vehicle: handle, material_path: string, color: table:Color | true |
| `get_available_animation` | vehicle: handle, index: integer | string |
| `play_animation` | vehicle: handle, animation: string, blend_seconds: number | true |
| `restore_appearance` | vehicle: handle | true |
| `set_part_visible` | vehicle: handle, part: integer, visible: integer | true |
| `set_material_override` | vehicle: handle, material_path: string, replacement_material_uri: string | true |
| `skin_apply_to_entity` | vehicle: handle, skin_uri: string | true |
| `set_custom_skin` | vehicle: handle, texture_pack_uri: string | true |
| `set_custom_camouflage` | vehicle: handle, camouflage_uri: string | true |
| `set_emblem` | vehicle: handle, slot: integer, texture_uri: string | true |
| `set_inscription` | vehicle: handle, slot: integer, text: string, font_uri: string | true |
| `set_engine_sound` | vehicle: handle, audio_uri: string | true |
| `set_gun_sound` | vehicle: handle, audio_uri: string | true |
| `play_hangar_animation` | vehicle: handle, animation: string | true |
| `set_hangar_idle_animation` | vehicle: handle, animation: string | true |
| `profile_register` | profile: table:VehicleVisualProfile | profile: handle |
| `profile_apply` | profile: handle, vehicle: handle | true |
| `profile_release` | profile: handle | true |
| `skin_pack_register` | pack: table:VehicleSkinPack | pack: handle |
| `skin_pack_apply` | pack: handle, vehicle: handle | true |
| `skin_pack_rollback` | pack: handle | true |
| `skin_pack_get_state` | pack: handle | state: table:VehicleSkinState |
| `skin_pack_release` | pack: handle | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.vehicle", "vehicle.local.cosmetic", "game.entity.public", "resources.mod", "resources.overlay.game"]
local get_local_player_vehicle, err = wotb.vehicle_visual.get_local_player_vehicle()  -- vehicle: handle
if get_local_player_vehicle == nil then wotb.log.warn("vehicle_visual.get_local_player_vehicle: %s", err) end
local is_local_player, err = wotb.vehicle_visual.is_local_player(handle)  -- is_local: integer
if is_local_player == nil then wotb.log.warn("vehicle_visual.is_local_player: %s", err) end
local is_hangar_vehicle, err = wotb.vehicle_visual.is_hangar_vehicle(handle)  -- is_hangar: integer
if is_hangar_vehicle == nil then wotb.log.warn("vehicle_visual.is_hangar_vehicle: %s", err) end
```

