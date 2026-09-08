# `wotb.events`

Raw-таблица интерфейса `WotbModV3EventsApiV1` (`include/wotbmod/events_v1.h`, версия `WOTBMOD_V3_EVENTS_VERSION`). Функции ниже вызываются как `wotb.events.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.battle`](../facades/wotb-battle.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `events.public`. Имена объявляются в `permissions` манифеста.

Биндинг ручной: `loader/lua/lua_bind_events.cpp`.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.events.THREAD_UNKNOWN` | `WOTBMOD_V3_THREAD_UNKNOWN` | enum WotbModV3ThreadRole |
| `wotb.events.THREAD_MAIN` | `WOTBMOD_V3_THREAD_MAIN` | enum WotbModV3ThreadRole |
| `wotb.events.THREAD_RENDER` | `WOTBMOD_V3_THREAD_RENDER` | enum WotbModV3ThreadRole |
| `wotb.events.THREAD_AUDIO` | `WOTBMOD_V3_THREAD_AUDIO` | enum WotbModV3ThreadRole |
| `wotb.events.THREAD_WORKER` | `WOTBMOD_V3_THREAD_WORKER` | enum WotbModV3ThreadRole |
| `wotb.events.THREAD_IO` | `WOTBMOD_V3_THREAD_IO` | enum WotbModV3ThreadRole |
| `wotb.events.TOPIC_RPC_OBSERVED` | `WOTBMOD_V3_EVENT_RPC_OBSERVED` | строка |
| `wotb.events.TOPIC_PUBLIC_ENTITY_ADDED` | `WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED` | строка |
| `wotb.events.TOPIC_PUBLIC_ENTITY_UPDATED` | `WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED` | строка |
| `wotb.events.TOPIC_PUBLIC_ENTITY_REMOVED` | `WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED` | строка |
| `wotb.events.MAX_TOPIC` | `WOTBMOD_V3_MAX_EVENT_TOPIC` | define |
| `wotb.events.MAX_PAYLOAD` | `WOTBMOD_V3_MAX_EVENT_PAYLOAD` | define |
| `wotb.events.PRIORITY_LOWEST` | `WOTBMOD_V3_EVENT_PRIORITY_LOWEST` | enum WotbModV3EventPriority |
| `wotb.events.PRIORITY_LOW` | `WOTBMOD_V3_EVENT_PRIORITY_LOW` | enum WotbModV3EventPriority |
| `wotb.events.PRIORITY_NORMAL` | `WOTBMOD_V3_EVENT_PRIORITY_NORMAL` | enum WotbModV3EventPriority |
| `wotb.events.PRIORITY_HIGH` | `WOTBMOD_V3_EVENT_PRIORITY_HIGH` | enum WotbModV3EventPriority |
| `wotb.events.PRIORITY_HIGHEST` | `WOTBMOD_V3_EVENT_PRIORITY_HIGHEST` | enum WotbModV3EventPriority |
| `wotb.events.FLAG_NONE` | `WOTBMOD_V3_EVENT_FLAG_NONE` | enum WotbModV3EventFlags |
| `wotb.events.FLAG_STOPPABLE` | `WOTBMOD_V3_EVENT_FLAG_STOPPABLE` | enum WotbModV3EventFlags |
| `wotb.events.FLAG_MUTABLE_PAYLOAD` | `WOTBMOD_V3_EVENT_FLAG_MUTABLE_PAYLOAD` | enum WotbModV3EventFlags |
| `wotb.events.FLAG_SYSTEM` | `WOTBMOD_V3_EVENT_FLAG_SYSTEM` | enum WotbModV3EventFlags |
| `wotb.events.FLAG_COALESCIBLE` | `WOTBMOD_V3_EVENT_FLAG_COALESCIBLE` | enum WotbModV3EventFlags |
| `wotb.events.TYPE_UI_SCREEN_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_UI_SCREEN_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_SCENE_ACTIVATED` | `WOTBMOD_V3_CLIENT_EVENT_SCENE_ACTIVATED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_SCENE_DEACTIVATED` | `WOTBMOD_V3_CLIENT_EVENT_SCENE_DEACTIVATED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_UI_INPUT` | `WOTBMOD_V3_CLIENT_EVENT_UI_INPUT` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_BATTLE_ENTERED` | `WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_BATTLE_STARTED` | `WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_BATTLE_ENDED` | `WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_BATTLE_LEFT` | `WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_LOCAL_VEHICLE_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_SPAWNED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_DESPAWNED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_SHOT_FIRED` | `WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_SHELL_HIT` | `WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_HEALTH_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_DAMAGED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_DESTROYED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_RELOAD_STATE_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_AMMO_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_AIM_TARGET_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_SPOTTED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_UNSPOTTED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_CAMERA_MODE_CHANGED` | `WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED` | enum WotbModV3ClientEventType |
| `wotb.events.TYPE_VEHICLE_KILLED` | `WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED` | enum WotbModV3ClientEventType |
| `wotb.events.TOPIC_CLIENT_READY` | `WOTBMOD_V3_EVENT_CLIENT_READY` | строка |
| `wotb.events.TOPIC_CLIENT_SHUTTING_DOWN` | `WOTBMOD_V3_EVENT_CLIENT_SHUTTING_DOWN` | строка |
| `wotb.events.TOPIC_GAME_STATE_CHANGED` | `WOTBMOD_V3_EVENT_GAME_STATE_CHANGED` | строка |
| `wotb.events.TOPIC_CAPABILITIES_CHANGED` | `WOTBMOD_V3_EVENT_CAPABILITIES_CHANGED` | строка |
| `wotb.events.TOPIC_FRAME_UPDATE` | `WOTBMOD_V3_EVENT_FRAME_UPDATE` | строка |
| `wotb.events.TOPIC_FIXED_UPDATE` | `WOTBMOD_V3_EVENT_FIXED_UPDATE` | строка |
| `wotb.events.TOPIC_UI_ROOT_READY` | `WOTBMOD_V3_EVENT_UI_ROOT_READY` | строка |
| `wotb.events.TOPIC_UI_ROOT_DESTROYED` | `WOTBMOD_V3_EVENT_UI_ROOT_DESTROYED` | строка |
| `wotb.events.TOPIC_UI_SCREEN_CHANGED` | `WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED` | строка |
| `wotb.events.TOPIC_UI_INPUT` | `WOTBMOD_V3_EVENT_UI_INPUT` | строка |
| `wotb.events.TOPIC_UI_SCALE_CHANGED` | `WOTBMOD_V3_EVENT_UI_SCALE_CHANGED` | строка |
| `wotb.events.TOPIC_SAFE_AREA_CHANGED` | `WOTBMOD_V3_EVENT_SAFE_AREA_CHANGED` | строка |
| `wotb.events.TOPIC_HUD_READY` | `WOTBMOD_V3_EVENT_HUD_READY` | строка |
| `wotb.events.TOPIC_HUD_DESTROYED` | `WOTBMOD_V3_EVENT_HUD_DESTROYED` | строка |
| `wotb.events.TOPIC_BATTLE_ENTERED` | `WOTBMOD_V3_EVENT_BATTLE_ENTERED` | строка |
| `wotb.events.TOPIC_BATTLE_COUNTDOWN_STARTED` | `WOTBMOD_V3_EVENT_BATTLE_COUNTDOWN_STARTED` | строка |
| `wotb.events.TOPIC_BATTLE_STARTED` | `WOTBMOD_V3_EVENT_BATTLE_STARTED` | строка |
| `wotb.events.TOPIC_BATTLE_ENDED` | `WOTBMOD_V3_EVENT_BATTLE_ENDED` | строка |
| `wotb.events.TOPIC_BATTLE_LEFT` | `WOTBMOD_V3_EVENT_BATTLE_LEFT` | строка |
| `wotb.events.TOPIC_SCENE_ACTIVATED` | `WOTBMOD_V3_EVENT_SCENE_ACTIVATED` | строка |
| `wotb.events.TOPIC_SCENE_DEACTIVATED` | `WOTBMOD_V3_EVENT_SCENE_DEACTIVATED` | строка |
| `wotb.events.TOPIC_LOCAL_VEHICLE_CHANGED` | `WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED` | строка |
| `wotb.events.TOPIC_LOCAL_VEHICLE_CREATED` | `WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED` | строка |
| `wotb.events.TOPIC_LOCAL_VEHICLE_APPEARANCE_READY` | `WOTBMOD_V3_EVENT_LOCAL_VEHICLE_APPEARANCE_READY` | строка |
| `wotb.events.TOPIC_LOCAL_VEHICLE_DESTROYED` | `WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED` | строка |
| `wotb.events.TOPIC_VEHICLE_SPAWNED` | `WOTBMOD_V3_EVENT_VEHICLE_SPAWNED` | строка |
| `wotb.events.TOPIC_VEHICLE_DESPAWNED` | `WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED` | строка |
| `wotb.events.TOPIC_SHOT_FIRED` | `WOTBMOD_V3_EVENT_SHOT_FIRED` | строка |
| `wotb.events.TOPIC_SHELL_HIT` | `WOTBMOD_V3_EVENT_SHELL_HIT` | строка |
| `wotb.events.TOPIC_VEHICLE_HEALTH_CHANGED` | `WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED` | строка |
| `wotb.events.TOPIC_VEHICLE_DAMAGED` | `WOTBMOD_V3_EVENT_VEHICLE_DAMAGED` | строка |
| `wotb.events.TOPIC_VEHICLE_DESTROYED` | `WOTBMOD_V3_EVENT_VEHICLE_DESTROYED` | строка |
| `wotb.events.TOPIC_RELOAD_STATE_CHANGED` | `WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED` | строка |
| `wotb.events.TOPIC_AMMO_CHANGED` | `WOTBMOD_V3_EVENT_AMMO_CHANGED` | строка |
| `wotb.events.TOPIC_AIM_TARGET_CHANGED` | `WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED` | строка |
| `wotb.events.TOPIC_VEHICLE_SPOTTED` | `WOTBMOD_V3_EVENT_VEHICLE_SPOTTED` | строка |
| `wotb.events.TOPIC_VEHICLE_UNSPOTTED` | `WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED` | строка |
| `wotb.events.TOPIC_VEHICLE_KILLED` | `WOTBMOD_V3_EVENT_VEHICLE_KILLED` | строка |
| `wotb.events.TOPIC_RENDER_DEVICE_CREATED` | `WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED` | строка |
| `wotb.events.TOPIC_RENDER_DEVICE_LOST` | `WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST` | строка |
| `wotb.events.TOPIC_RENDER_DEVICE_RESTORED` | `WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED` | строка |
| `wotb.events.TOPIC_SWAPCHAIN_RESIZED` | `WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED` | строка |
| `wotb.events.TOPIC_RENDER_BACKEND_CHANGED` | `WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED` | строка |
| `wotb.events.TOPIC_AUDIO_DEVICE_CHANGED` | `WOTBMOD_V3_EVENT_AUDIO_DEVICE_CHANGED` | строка |
| `wotb.events.TOPIC_SOUND_EVENT_CREATED` | `WOTBMOD_V3_EVENT_SOUND_EVENT_CREATED` | строка |
| `wotb.events.TOPIC_SOUND_EVENT_FINISHED` | `WOTBMOD_V3_EVENT_SOUND_EVENT_FINISHED` | строка |
| `wotb.events.TOPIC_INPUT_DEVICE_CHANGED` | `WOTBMOD_V3_EVENT_INPUT_DEVICE_CHANGED` | строка |
| `wotb.events.TOPIC_INPUT_ACTION` | `WOTBMOD_V3_EVENT_INPUT_ACTION` | строка |
| `wotb.events.TOPIC_FOV_CHANGED` | `WOTBMOD_V3_EVENT_FOV_CHANGED` | строка |
| `wotb.events.TOPIC_ZOOM_CHANGED` | `WOTBMOD_V3_EVENT_ZOOM_CHANGED` | строка |
| `wotb.events.TOPIC_CAMERA_MODE_CHANGED` | `WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED` | строка |
| `wotb.events.TOPIC_CAMERA_TRANSITION_STARTED` | `WOTBMOD_V3_EVENT_CAMERA_TRANSITION_STARTED` | строка |
| `wotb.events.TOPIC_CAMERA_TRANSITION_ENDED` | `WOTBMOD_V3_EVENT_CAMERA_TRANSITION_ENDED` | строка |
| `wotb.events.TOPIC_SNIPER_ENTERED` | `WOTBMOD_V3_EVENT_SNIPER_ENTERED` | строка |
| `wotb.events.TOPIC_SNIPER_EXITED` | `WOTBMOD_V3_EVENT_SNIPER_EXITED` | строка |
| `wotb.events.TOPIC_POSTPROCESSING_TOGGLED` | `WOTBMOD_V3_EVENT_POSTPROCESSING_TOGGLED` | строка |
| `wotb.events.TOPIC_REPLAY_SPEED_CHANGED` | `WOTBMOD_V3_EVENT_REPLAY_SPEED_CHANGED` | строка |
| `wotb.events.TOPIC_REPLAY_SEEK` | `WOTBMOD_V3_EVENT_REPLAY_SEEK` | строка |
| `wotb.events.TOPIC_HANGAR_BACKGROUND_CHANGED` | `WOTBMOD_V3_EVENT_HANGAR_BACKGROUND_CHANGED` | строка |
| `wotb.events.TOPIC_RETICLE_CHANGED` | `WOTBMOD_V3_EVENT_RETICLE_CHANGED` | строка |
| `wotb.events.TOPIC_DAMAGE_DEALT` | `WOTBMOD_V3_EVENT_DAMAGE_DEALT` | строка |
| `wotb.events.TOPIC_DAMAGE_RECEIVED` | `WOTBMOD_V3_EVENT_DAMAGE_RECEIVED` | строка |
| `wotb.events.TOPIC_SIXTH_SENSE_TRIGGERED` | `WOTBMOD_V3_EVENT_SIXTH_SENSE_TRIGGERED` | строка |
| `wotb.events.TOPIC_MINIMAP_MARKER_ADDED` | `WOTBMOD_V3_EVENT_MINIMAP_MARKER_ADDED` | строка |
| `wotb.events.TOPIC_SESSION_STATS_UPDATED` | `WOTBMOD_V3_EVENT_SESSION_STATS_UPDATED` | строка |
| `wotb.events.TOPIC_MOD_PRELOAD` | `WOTBMOD_V3_EVENT_MOD_PRELOAD` | строка |
| `wotb.events.TOPIC_MOD_LOADED` | `WOTBMOD_V3_EVENT_MOD_LOADED` | строка |
| `wotb.events.TOPIC_MOD_ENABLED` | `WOTBMOD_V3_EVENT_MOD_ENABLED` | строка |
| `wotb.events.TOPIC_MOD_DISABLED` | `WOTBMOD_V3_EVENT_MOD_DISABLED` | строка |
| `wotb.events.TOPIC_MOD_UNLOADING` | `WOTBMOD_V3_EVENT_MOD_UNLOADING` | строка |
| `wotb.events.TOPIC_MOD_UNLOADED` | `WOTBMOD_V3_EVENT_MOD_UNLOADED` | строка |
| `wotb.events.TOPIC_LOCAL_SHELL_FIRED` | `WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED` | строка |
| `wotb.events.TOPIC_VISIBLE_TRACER_CREATED` | `WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED` | строка |
| `wotb.events.TOPIC_VISIBLE_TRACER_DESTROYED` | `WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED` | строка |
| `wotb.events.TOPIC_PROJECTILE_CREATED` | `WOTBMOD_V3_EVENT_PROJECTILE_CREATED` | строка |
| `wotb.events.TOPIC_PROJECTILE_UPDATED` | `WOTBMOD_V3_EVENT_PROJECTILE_UPDATED` | строка |
| `wotb.events.TOPIC_PROJECTILE_IMPACTED` | `WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED` | строка |
| `wotb.events.TOPIC_PROJECTILE_DESTROYED` | `WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED` | строка |
| `wotb.events.TOPIC_RESOURCE_INVALIDATED` | `WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED` | строка |
| `wotb.events.TOPIC_RESOURCE_RELOADED` | `WOTBMOD_V3_EVENT_RESOURCE_RELOADED` | строка |
| `wotb.events.TOPIC_SESSION_CLUSTER_CHANGED` | `WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED` | строка |
| `wotb.events.TOPIC_VFS_INVALIDATED` | `WOTBMOD_V3_EVENT_VFS_INVALIDATED` | строка |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `subscribe` | info: table:EventSubscriptionInfo, callback: function | token: handle |
| `unsubscribe` | token: handle | true |
| `set_priority` | token: handle, priority: integer | true |
| `post` | topic: string, payload: void, payload_size: integer, flags: integer | true |
| `stop_propagation` | dispatch_token: handle | true |
| `get_dispatch_info` | dispatch_token: handle | info: table:EventDispatchInfo |
| `get_thread` | dispatch_token: handle | thread_role: integer |
| `get_timestamp` | dispatch_token: handle | timestamp_ns: integer |
| `get_context` | dispatch_token: handle | context_mask: integer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["events.public"]
local subscribe, err = wotb.events.subscribe(info, function(...) end)  -- token: handle
if subscribe == nil then wotb.log.warn("events.subscribe: %s", err) end
local unsubscribe_ok, err = wotb.events.unsubscribe(handle)
if not unsubscribe_ok then wotb.log.warn("events.unsubscribe: %s", err) end
local set_priority_ok, err = wotb.events.set_priority(handle, 0)
if not set_priority_ok then wotb.log.warn("events.set_priority: %s", err) end
```

