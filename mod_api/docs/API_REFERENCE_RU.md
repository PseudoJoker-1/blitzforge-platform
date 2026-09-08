# Справочник Lua API WotbMod V3 (сгенерирован)

Файл создаёт `tools/generate_api_reference.py` из заголовков `include/wotbmod/*.h` — той же модели, из которой генерируются Lua-биндинги. Не правьте его руками: `--check` в тестах падает, когда он отстаёт от заголовков.

Итого: 622 слотов в 47 интерфейсах, 620 доступны из Lua, 2 намеренно нет.

Как читать сигнатуру: аргументы — как их принимает Lua-функция (скрытый `mod` и `user_data` опущены, буферы приходят строкой, callback — функцией); результат — что она возвращает при успехе. При отказе любая функция отвечает `nil, err`. Права — семейство permission-имён, которое проверяет забор host-а перед вызовом; `capability`-статус и контексты — в [API_V3_RU.md](API_V3_RU.md) и `wotb.mod.capabilities()`.

## Фасады (facade-first)

| Модуль | Поверх raw-таблиц | Методы |
| --- | --- | --- |
| `wotb.context` | `core` | current, contains, should_show, apply_visibility, is_hangar, is_battle, is_training, is_replay, is_text_input, is_mod_screen |
| `wotb.players` | `entity_public` | snapshot, local_player, me, our_team, allies, enemy_team, enemies, unknown_team, visible, find, by_id, each_visible, details |
| `wotb.battle` | `events, core` | start, stop, tracking, reset, snapshot, groups, on, once, off, off_all, events, on_<name> (enter, start, end, leave, shot, hit, reload, ammo, damage, death, vehicle_destroyed, spotted, unspotted, camera_changed, sniper_entered, sniper_exited), is_active, state |
| `wotb.session` | `session_cluster, events` | clusters, cluster, change_cluster, on_cluster_changed, off_cluster_changed, off_all |
| `wotb.ges` | `ges, events` | types, subscribe, unsubscribe, publish, schema, on, off, observe, decode, is_available |
| `wotb.mod` | `lifecycle, permissions, capabilities` | id, permissions, has_permission, capability, capabilities, info, on_disable, off_disable |
| `wotb.hud` | `gameplay_hud` | mode, available, status, reset, rgba, reticle.*, damage_log.*, session_stats.*, minimap.*, sixth_sense.*, hit_indicator.* |
| `wotb.screen` | `ui, ui_read, handles` | root, find, children, text, live_text, rect, visible, game_owned, info, set_text, set_visible, mount, unmount, notify, popup |
| `wotb.vehicle` | `vehicle_visual, entity_public` | local_vehicle, visible, is_local, is_hangar, position, appearance_state, skin.register, skin.apply, skin.rollback, skin.state, skin.release, appearance.reset, set_skin, set_camouflage |
| `wotb.shells` | `projectile, events` | on, on_created, on_updated, on_impact, on_destroyed, on_local_shot, off, off_all, snapshot, visual, impact.show, impact.update, impact.hide, tracer.register, tracer.unregister |
| `wotb.view` | `camera, gameplay_camera, camera_state, events` | get, set_fov, fov, reset, project, unproject, transition, shake, on_changed, off |
| `wotb.sound` | `audio` | play, stop, release, set_volume, is_playing, on_finished, replace, reset, reset_all |
| `wotb.keys` | `input, handles` | bind, unbind, unbind_all, key_code, pressed, down, axis, on_pressed, on_released, off, bindings, conflicts, capture_begin, capture_end |
| `wotb.store` | `storage, json` | get, set, delete, has, keys, clear, flush |
| `wotb.files` | `loaders, vfs, resources, yaml, events` | read_text, read_binary, read_json, read_yaml (doc:get, doc:release), load_texture, load_audio, load_scene, info, release, exists, stat, list, watch, unwatch |
| `wotb.panel` | `ui, context, events, handles` | new, mount, unmount, update, set_row, set_rows, set_row_visible, row, row_count, set_button_text, visible, set_visible, show, hide, layout, set_size, set_anchor, set_position, position, size, viewport, control, errors, label, button, image, scroll, column, on, destroy |
| `wotb.log / wotb.json / wotb.timer / wotb.config / wotb.available` | `core, events, settings, storage` | см. LUA_MODS_RU.md, «Convenience-модули» |

Правило имён: таблица фасада никогда не совпадает с таблицей интерфейса; описание каждого модуля — в [LUA_MODS_RU.md](LUA_MODS_RU.md), раздел «Фасады».

## Raw-таблицы по интерфейсам

### `wotb.core` — `core_v1.h`

- C ABI: `WotbModV3CoreApiV1`, версия `WOTBMOD_V3_CORE_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (6): `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERROR`, `LOG_FATAL`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `log` | level: integer, category: string, message: string | true |
| `get_context` | — | context_mask: integer |
| `get_thread_role` | — | thread_role: integer |
| `get_frame_index` | — | frame_index: integer |
| `get_game_directory` | — | string |
| `get_mod_data_directory` | — | string |
| `get_mod_cache_directory` | — | string |
| `get_mod_config_directory` | — | string |

### `wotb.capabilities` — `capabilities_v1.h`

- C ABI: `WotbModV3CapabilitiesApiV1`, версия `WOTBMOD_V3_CAPABILITIES_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (6): `STATUS_AVAILABLE`, `STATUS_UNAVAILABLE`, `STATUS_CLIENT_MISMATCH`, `STATUS_PERMISSION_DENIED`, `STATUS_CONTEXT_RESTRICTED`, `STATUS_DEGRADED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_count` | — | count: integer |
| `get_at` | index: integer | info: table:CapabilityInfo |
| `query` | capability_name: string | info: table:CapabilityInfo |
| `subscribe` | callback: function | token: handle |
| `unsubscribe` | token: handle | true |

### `wotb.permissions` — `permissions_v1.h`

- C ABI: `WotbModV3PermissionsApiV1`, версия `WOTBMOD_V3_PERMISSIONS_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (8): `TIER_SAFE`, `TIER_GAMEPLAY_TWEAK`, `TIER_REVIEWED`, `TIER_UNSAFE`, `STATE_DENIED`, `STATE_GRANTED`, `STATE_REVIEW_REQUIRED`, `STATE_UNAVAILABLE`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_granted_tier` | — | tier: integer |
| `query` | permission_name: string | info: table:PermissionInfo |
| `get_count` | — | count: integer |
| `get_at` | index: integer | info: table:PermissionInfo |

### `wotb.handles` — `handles_v1.h`

- C ABI: `WotbModV3HandlesApiV1`, версия `WOTBMOD_V3_HANDLES_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы: нет.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `retain` | handle: handle | true |
| `release` | handle: handle | true |
| `get_info` | handle: handle | info: table:HandleInfo |
| `is_alive` | handle: handle | alive: integer |

### `wotb.lifecycle` — `lifecycle_v1.h`

- C ABI: `WotbModV3LifecycleApiV1`, версия `WOTBMOD_V3_LIFECYCLE_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (16): `STATE_UNKNOWN`, `STATE_LOADED`, `STATE_ENABLED`, `STATE_DISABLED`, `STATE_UNLOADING`, `STATE_UNLOADED`, `TRANSITION_PRELOAD`, `TRANSITION_LOADED`, `TRANSITION_ENABLED`, `TRANSITION_DISABLED`, `TRANSITION_UNLOADING`, `TRANSITION_UNLOADED`, `CLEANUP_RELEASED`, `CLEANUP_MOD_DISABLED`, `CLEANUP_MOD_UNLOADING`, `CLEANUP_RUNTIME_SHUTDOWN`.

| Слот | Аргументы | Результат |
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

### `wotb.hooks` — `hooks_v1.h`

- C ABI: `WotbModV3HooksApiV1`, версия `WOTBMOD_V3_HOOKS_VERSION`;
- права: `hooks.symbol`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (15): `MAX_TARGET`, `MODE_BEFORE`, `MODE_AFTER`, `MODE_AROUND`, `MODE_REPLACE`, `MODE_OBSERVE`, `STATUS_PENDING_BACKEND`, `STATUS_DISABLED`, `STATUS_ENABLED`, `STATUS_CONFLICT`, `STATUS_REMOVED`, `STATUS_FAILED`, `CREATE_NONE`, `CREATE_ALLOW_PENDING`, `CREATE_ALLOW_CONFLICT`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `create_symbol` | symbol: string, info: table:HookCreateInfo | hook: handle |
| `create_vtable` | object: void, slot: integer, info: table:HookCreateInfo | hook: handle |
| `enable` | hook: handle | true |
| `disable` | hook: handle | true |
| `remove` | hook: handle | true |
| `set_priority` | hook: handle, priority: integer | true |
| `run_before` | hook: handle, other_hook: handle | true |
| `run_after` | hook: handle, other_hook: handle | true |
| `get_original` | hook: handle | original: void |
| `call_next` | hook: handle, arguments: table:ConstBuffer, result: table:Buffer | true |
| `get_info` | hook: handle | info: table:HookInfo |
| `get_owner` | hook: handle | owner: handle |
| `get_status` | hook: handle | status: integer |
| `get_chain` | target: string | hooks: array |
| `get_conflicts` | hook: handle | conflicts: array |
| `enumerate` | — | hooks: array |

### `wotb.unsafe_native` — `unsafe_native_v1.h`

- C ABI: `WotbModV3UnsafeNativeApiV1`, версия `WOTBMOD_V3_UNSAFE_NATIVE_VERSION`;
- права: `native.hook.address`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы: нет.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `create_address_hook` | — | *не публикуется в Lua: нужен сырой нативный указатель* |

### `wotb.events` — `events_v1.h`

- C ABI: `WotbModV3EventsApiV1`, версия `WOTBMOD_V3_EVENTS_VERSION`;
- права: `events.public`;
- биндинг: ручной, `loader/lua/lua_bind_events.cpp`;
- константы (127): `THREAD_UNKNOWN`, `THREAD_MAIN`, `THREAD_RENDER`, `THREAD_AUDIO`, `THREAD_WORKER`, `THREAD_IO`, `TOPIC_RPC_OBSERVED`, `TOPIC_PUBLIC_ENTITY_ADDED`, `TOPIC_PUBLIC_ENTITY_UPDATED`, `TOPIC_PUBLIC_ENTITY_REMOVED`, `MAX_TOPIC`, `MAX_PAYLOAD`, `PRIORITY_LOWEST`, `PRIORITY_LOW`, `PRIORITY_NORMAL`, `PRIORITY_HIGH`, `PRIORITY_HIGHEST`, `FLAG_NONE`, `FLAG_STOPPABLE`, `FLAG_MUTABLE_PAYLOAD`, `FLAG_SYSTEM`, `FLAG_COALESCIBLE`, `TYPE_UI_SCREEN_CHANGED`, `TYPE_SCENE_ACTIVATED`, `TYPE_SCENE_DEACTIVATED`, `TYPE_UI_INPUT`, `TYPE_BATTLE_ENTERED`, `TYPE_BATTLE_STARTED`, `TYPE_BATTLE_ENDED`, `TYPE_BATTLE_LEFT`, `TYPE_LOCAL_VEHICLE_CHANGED`, `TYPE_VEHICLE_SPAWNED`, `TYPE_VEHICLE_DESPAWNED`, `TYPE_SHOT_FIRED`, `TYPE_SHELL_HIT`, `TYPE_VEHICLE_HEALTH_CHANGED`, `TYPE_VEHICLE_DAMAGED`, `TYPE_VEHICLE_DESTROYED`, `TYPE_RELOAD_STATE_CHANGED`, `TYPE_AMMO_CHANGED`, `TYPE_AIM_TARGET_CHANGED`, `TYPE_VEHICLE_SPOTTED`, `TYPE_VEHICLE_UNSPOTTED`, `TYPE_CAMERA_MODE_CHANGED`, `TYPE_VEHICLE_KILLED`, `TOPIC_CLIENT_READY`, `TOPIC_CLIENT_SHUTTING_DOWN`, `TOPIC_GAME_STATE_CHANGED`, `TOPIC_CAPABILITIES_CHANGED`, `TOPIC_FRAME_UPDATE`, `TOPIC_FIXED_UPDATE`, `TOPIC_UI_ROOT_READY`, `TOPIC_UI_ROOT_DESTROYED`, `TOPIC_UI_SCREEN_CHANGED`, `TOPIC_UI_INPUT`, `TOPIC_UI_SCALE_CHANGED`, `TOPIC_SAFE_AREA_CHANGED`, `TOPIC_HUD_READY`, `TOPIC_HUD_DESTROYED`, `TOPIC_BATTLE_ENTERED`, `TOPIC_BATTLE_COUNTDOWN_STARTED`, `TOPIC_BATTLE_STARTED`, `TOPIC_BATTLE_ENDED`, `TOPIC_BATTLE_LEFT`, `TOPIC_SCENE_ACTIVATED`, `TOPIC_SCENE_DEACTIVATED`, `TOPIC_LOCAL_VEHICLE_CHANGED`, `TOPIC_LOCAL_VEHICLE_CREATED`, `TOPIC_LOCAL_VEHICLE_APPEARANCE_READY`, `TOPIC_LOCAL_VEHICLE_DESTROYED`, `TOPIC_VEHICLE_SPAWNED`, `TOPIC_VEHICLE_DESPAWNED`, `TOPIC_SHOT_FIRED`, `TOPIC_SHELL_HIT`, `TOPIC_VEHICLE_HEALTH_CHANGED`, `TOPIC_VEHICLE_DAMAGED`, `TOPIC_VEHICLE_DESTROYED`, `TOPIC_RELOAD_STATE_CHANGED`, `TOPIC_AMMO_CHANGED`, `TOPIC_AIM_TARGET_CHANGED`, `TOPIC_VEHICLE_SPOTTED`, `TOPIC_VEHICLE_UNSPOTTED`, `TOPIC_VEHICLE_KILLED`, `TOPIC_RENDER_DEVICE_CREATED`, `TOPIC_RENDER_DEVICE_LOST`, `TOPIC_RENDER_DEVICE_RESTORED`, `TOPIC_SWAPCHAIN_RESIZED`, `TOPIC_RENDER_BACKEND_CHANGED`, `TOPIC_AUDIO_DEVICE_CHANGED`, `TOPIC_SOUND_EVENT_CREATED`, `TOPIC_SOUND_EVENT_FINISHED`, `TOPIC_INPUT_DEVICE_CHANGED`, `TOPIC_INPUT_ACTION`, `TOPIC_FOV_CHANGED`, `TOPIC_ZOOM_CHANGED`, `TOPIC_CAMERA_MODE_CHANGED`, `TOPIC_CAMERA_TRANSITION_STARTED`, `TOPIC_CAMERA_TRANSITION_ENDED`, `TOPIC_SNIPER_ENTERED`, `TOPIC_SNIPER_EXITED`, `TOPIC_POSTPROCESSING_TOGGLED`, `TOPIC_REPLAY_SPEED_CHANGED`, `TOPIC_REPLAY_SEEK`, `TOPIC_HANGAR_BACKGROUND_CHANGED`, `TOPIC_RETICLE_CHANGED`, `TOPIC_DAMAGE_DEALT`, `TOPIC_DAMAGE_RECEIVED`, `TOPIC_SIXTH_SENSE_TRIGGERED`, `TOPIC_MINIMAP_MARKER_ADDED`, `TOPIC_SESSION_STATS_UPDATED`, `TOPIC_MOD_PRELOAD`, `TOPIC_MOD_LOADED`, `TOPIC_MOD_ENABLED`, `TOPIC_MOD_DISABLED`, `TOPIC_MOD_UNLOADING`, `TOPIC_MOD_UNLOADED`, `TOPIC_LOCAL_SHELL_FIRED`, `TOPIC_VISIBLE_TRACER_CREATED`, `TOPIC_VISIBLE_TRACER_DESTROYED`, `TOPIC_PROJECTILE_CREATED`, `TOPIC_PROJECTILE_UPDATED`, `TOPIC_PROJECTILE_IMPACTED`, `TOPIC_PROJECTILE_DESTROYED`, `TOPIC_RESOURCE_INVALIDATED`, `TOPIC_RESOURCE_RELOADED`, `TOPIC_SESSION_CLUSTER_CHANGED`, `TOPIC_VFS_INVALIDATED`.

| Слот | Аргументы | Результат |
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

### `wotb.ui` — `ui_v3.h`

- C ABI: `WotbModV3UiApiV3`, версия `WOTBMOD_V3_UI_VERSION_3`;
- права: `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui`;
- биндинг: ручной, `loader/lua/lua_bind_ui.cpp (ядро; остальные слоты — генератор)`;
- константы (57): `CONTROL_CONTAINER`, `CONTROL_TEXT`, `CONTROL_IMAGE`, `CONTROL_BUTTON`, `CONTROL_CHECKBOX`, `CONTROL_SLIDER`, `CONTROL_DROPDOWN`, `CONTROL_TEXT_INPUT`, `CONTROL_SCROLL_VIEW`, `CONTROL_LIST`, `CONTROL_TABS`, `LAYOUT_ABSOLUTE`, `LAYOUT_HORIZONTAL`, `LAYOUT_VERTICAL`, `LAYOUT_GRID`, `LAYOUT_FLEX`, `LAYOUT_OVERLAY`, `DIRECTION_LEFT_TO_RIGHT`, `DIRECTION_RIGHT_TO_LEFT`, `DIRECTION_TOP_TO_BOTTOM`, `DIRECTION_BOTTOM_TO_TOP`, `LAYOUT_ALIGN_START`, `LAYOUT_ALIGN_CENTER`, `LAYOUT_ALIGN_END`, `LAYOUT_ALIGN_STRETCH`, `ALIGN_LEFT`, `ALIGN_CENTER`, `ALIGN_RIGHT`, `ALIGN_JUSTIFY`, `EVENT_CLICK`, `EVENT_DOUBLE_CLICK`, `EVENT_VALUE_CHANGED`, `EVENT_TEXT_CHANGED`, `EVENT_FOCUS_GAINED`, `EVENT_FOCUS_LOST`, `EVENT_POINTER_ENTER`, `EVENT_POINTER_LEAVE`, `EVENT_POINTER_DOWN`, `EVENT_POINTER_UP`, `EVENT_DRAG_START`, `EVENT_DRAG`, `EVENT_DRAG_END`, `EVENT_SCROLL`, `EVENT_SUBMIT`, `EVENT_CANCEL`, `STYLE_COLOR`, `STYLE_BACKGROUND_COLOR`, `STYLE_OPACITY`, `STYLE_FONT`, `STYLE_FONT_SIZE`, `STYLE_TEXTURE`, `STYLE_Z_ORDER`, `SNAPSHOT_VISIBLE`, `SNAPSHOT_ENABLED`, `SNAPSHOT_INTERACTABLE`, `SNAPSHOT_FOCUSED`, `SNAPSHOT_GAME_OWNED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `control_create` | descriptor: table:UiControlDescriptor | control: handle |
| `control_clone` | control: handle | clone: handle |
| `control_destroy` | control: handle | true |
| `control_add_child` | parent: handle, child: handle | true |
| `control_remove_child` | parent: handle, child: handle | true |
| `control_set_parent` | control: handle, parent: handle | true |
| `control_get_parent` | control: handle | parent: handle |
| `control_get_child_count` | control: handle | count: integer |
| `control_get_child_at` | control: handle, index: integer | child: handle |
| `control_set_id` | control: handle, id: string | true |
| `control_get_id` | control: handle | string |
| `control_find_by_id` | root: handle, id: string | control: handle |
| `control_find_by_path` | root: handle, path: string | control: handle |
| `control_get_owner_mod` | control: handle | owner: handle |
| `control_is_alive` | control: handle | alive: integer |
| `slot_find` | slot_id: string | slot: handle |
| `slot_attach` | slot: handle, control: handle, priority: integer | true |
| `slot_detach` | slot: handle, control: handle | true |
| `slot_enumerate` | visitor: function | true |
| `control_set_position` | control: handle, position: table:Vec2 | true |
| `control_get_position` | control: handle | position: table:Vec2 |
| `control_set_size` | control: handle, size: table:Vec2 | true |
| `control_get_size` | control: handle | size: table:Vec2 |
| `control_set_anchor` | control: handle, anchor: table:Vec2 | true |
| `control_set_pivot` | control: handle, pivot: table:Vec2 | true |
| `control_set_margin` | control: handle, margin: table:UiEdges | true |
| `control_set_padding` | control: handle, padding: table:UiEdges | true |
| `control_set_min_size` | control: handle, size: table:Vec2 | true |
| `control_set_max_size` | control: handle, size: table:Vec2 | true |
| `control_set_z_order` | control: handle, z_order: integer | true |
| `layout_set` | control: handle, layout: table:UiLayoutDescriptor | true |
| `layout_set_type` | control: handle, type: integer | true |
| `layout_set_direction` | control: handle, direction: integer | true |
| `layout_set_spacing` | control: handle, spacing: number | true |
| `layout_set_alignment` | control: handle, main_alignment: integer, cross_alignment: integer | true |
| `layout_set_weight` | control: handle, weight: number | true |
| `layout_invalidate` | control: handle | true |
| `get_scale_factor` | — | scale: number |
| `get_safe_area` | — | safe_area: table:Rect |
| `get_viewport_size` | — | size: table:Vec2 |
| `control_set_text` | control: handle, text: string | true |
| `control_set_texture` | control: handle, texture_uri: string | true |
| `control_set_color` | control: handle, color: table:Color | true |
| `control_set_opacity` | control: handle, opacity: number | true |
| `control_set_visible` | control: handle, visible: integer | true |
| `control_set_font` | control: handle, font_uri: string | true |
| `control_set_font_size` | control: handle, size: number | true |
| `control_set_text_alignment` | control: handle, alignment: integer | true |
| `control_set_text_wrap` | control: handle, enabled: integer | true |
| `control_set_rich_text` | control: handle, enabled: integer | true |
| `control_set_localization_key` | control: handle, key: string | true |
| `control_set_tooltip` | control: handle, tooltip: string | true |
| `control_set_accessibility_label` | control: handle, label: string | true |
| `control_set_enabled` | control: handle, enabled: integer | true |
| `control_set_interactable` | control: handle, interactable: integer | true |
| `control_set_focus` | control: handle, focused: integer | true |
| `event_subscribe` | control: handle, event_type: integer, callback: function | token: handle |
| `event_unsubscribe` | token: handle | true |
| `button_create` | descriptor: table:UiControlDescriptor | control: handle |
| `checkbox_create` | descriptor: table:UiControlDescriptor, checked: integer | control: handle |
| `slider_create` | descriptor: table:UiControlDescriptor, minimum: number, maximum: number, value: number | control: handle |
| `dropdown_create` | descriptor: table:UiControlDescriptor, choices: array | control: handle |
| `text_input_create` | descriptor: table:UiControlDescriptor | control: handle |
| `scroll_view_create` | descriptor: table:UiControlDescriptor | control: handle |
| `list_create` | descriptor: table:UiControlDescriptor | control: handle |
| `tabs_create` | descriptor: table:UiControlDescriptor | control: handle |
| `dialog_show` | descriptor: table:UiDialogDescriptor | dialog: handle |
| `confirm_show` | descriptor: table:UiDialogDescriptor | dialog: handle |
| `toast_show` | message: string, duration_seconds: number | true |
| `style_push` | control: handle, patch: table:UiStylePatch | override: handle |
| `style_update` | override_handle: handle, patch: table:UiStylePatch | true |
| `style_pop` | override_handle: handle | true |
| `get_active_screen` | — | screen: handle |
| `control_get_snapshot` | control: handle | snapshot: table:UiControlSnapshot |

### `wotb.settings` — `settings_v1.h`

- C ABI: `WotbModV3SettingsApiV1`, версия `WOTBMOD_V3_SETTINGS_VERSION`;
- права: `settings`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (18): `KEY_MAX`, `TEXT_MAX`, `TYPE_BOOL`, `TYPE_INT`, `TYPE_FLOAT`, `TYPE_STRING`, `TYPE_ENUM`, `TYPE_COLOR`, `TYPE_KEYBIND`, `TYPE_FILE`, `TYPE_FOLDER`, `TYPE_TITLE`, `TYPE_BUTTON`, `TYPE_CUSTOM`, `FLAG_NONE`, `FLAG_REQUIRES_RESTART`, `FLAG_READ_ONLY`, `FLAG_HIDDEN`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `register_schema` | schema_version: integer, definitions: array | true |
| `register_preset` | preset: table:SettingPreset | true |
| `get_schema_version` | — | version: integer |
| `get_bool` | key: string | value: integer |
| `get_int` | key: string | value: integer |
| `get_float` | key: string | value: number |
| `get_string` | key: string | string |
| `get_color` | key: string | value: table:Color |
| `set_bool` | key: string, value: integer | true |
| `set_int` | key: string, value: integer | true |
| `set_float` | key: string, value: number | true |
| `set_string` | key: string, value: string | true |
| `set_color` | key: string, value: table:Color | true |
| `reset` | key: string | true |
| `reset_all` | — | true |
| `apply_preset` | preset_id: string | true |
| `subscribe` | callback: function | token: handle |
| `unsubscribe` | token: handle | true |
| `run_migration` | target_version: integer, callback: function | true |

### `wotb.storage` — `storage_v1.h`

- C ABI: `WotbModV3StorageApiV1`, версия `WOTBMOD_V3_STORAGE_VERSION`;
- права: `storage`;
- биндинг: ручной, `loader/lua/lua_bind_storage.cpp`;
- константы (5): `KEY_MAX`, `PATH_DATA`, `PATH_CONFIG`, `PATH_CACHE`, `PATH_TEMP`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_json` | key: string | string |
| `set_json` | key: string, json_utf8: string | true |
| `get_bytes` | key: string | bytes |
| `set_bytes` | key: string, value: table:ConstBuffer | true |
| `erase` | key: string | true |
| `contains` | key: string | contains: integer |
| `flush` | — | true |
| `begin_transaction` | — | transaction: handle |
| `transaction_set_json` | transaction: handle, key: string, json_utf8: string | true |
| `transaction_set_bytes` | transaction: handle, key: string, value: table:ConstBuffer | true |
| `transaction_erase` | transaction: handle, key: string | true |
| `commit` | transaction: handle | true |
| `rollback` | transaction: handle | true |
| `get_path` | path_kind: integer | string |

### `wotb.input` — `input_v1.h`

- C ABI: `WotbModV3InputApiV1`, версия `WOTBMOD_V3_INPUT_VERSION`;
- права: `input.actions`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (21): `ACTION_ID_MAX`, `BINDINGS_MAX`, `MOUSE_LEFT`, `MOUSE_RIGHT`, `MOUSE_MIDDLE`, `MOUSE_X1`, `MOUSE_X2`, `MOUSE_WHEEL`, `MOUSE_MOVE_X`, `MOUSE_MOVE_Y`, `DEVICE_KEYBOARD`, `DEVICE_MOUSE`, `DEVICE_GAMEPAD`, `DEVICE_TOUCH`, `VALUE_BUTTON`, `VALUE_AXIS`, `MOD_NONE`, `MOD_SHIFT`, `MOD_CONTROL`, `MOD_ALT`, `MOD_META`.

| Слот | Аргументы | Результат |
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

### `wotb.vfs` — `vfs_v2.h`

- C ABI: `WotbModV3VfsApiV2`, версия `WOTBMOD_V3_VFS_VERSION_2`;
- права: `resources.mod`, `resources.overlay.game`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (9): `URI_MAX`, `ENTRY_FILE`, `ENTRY_DIRECTORY`, `MOUNT_PACKAGE`, `MOUNT_OVERLAY`, `CHANGE_CREATED`, `CHANGE_MODIFIED`, `CHANGE_DELETED`, `MAX_WRITE_BYTES`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_namespace` | — | string |
| `normalize_uri` | uri: string | string |
| `mount_package` | provider_id: string, physical_directory: string, priority: integer | mount: handle |
| `mount_overlay` | provider_id: string, target_game_uri: string, source_mod_uri: string, priority: integer | mount: handle |
| `unmount` | mount: handle | true |
| `resolve` | uri: string | string |
| `open` | uri: string | file: handle |
| `read` | file: handle, offset: integer | bytes |
| `list` | uri: string | entries: array |
| `stat` | uri: string | stat: table:VfsStat |
| `watch` | uri: string | token: handle |
| `get_providers` | uri: string | providers: array |
| `set_provider_priority` | mount: handle, priority: integer | true |
| `get_conflicts` | — | conflicts: array |
| `write_file` | uri: string, data: table:ConstBuffer | true |
| `append_file` | uri: string, data: table:ConstBuffer | true |
| `copy_file` | source_uri: string, destination_uri: string | true |
| `remove_file` | uri: string | true |

### `wotb.resources` — `resources_v1.h`

- C ABI: `WotbModV3ResourcesApiV1`, версия `WOTBMOD_V3_RESOURCES_VERSION`;
- права: `resources.mod`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (16): `BINARY`, `TEXT`, `IMAGE`, `AUDIO`, `MODEL`, `YAML`, `JSON`, `LOADING`, `READY`, `FAILED`, `CANCELLED`, `LOAD_DEFAULT`, `LOAD_REQUIRE_NATIVE_CLIENT`, `BACKING_NONE`, `BACKING_RAW_VFS_BYTES`, `BACKING_NATIVE_CLIENT_OBJECT`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `load` | desc: table:ResourceLoadDesc | resource: handle |
| `load_async_ex` | desc: table:ResourceLoadDesc | resource: handle |
| `get_info` | resource: handle | info: table:ResourceInfo |
| `copy_data` | resource: handle | bytes |
| `retain` | resource: handle | true |
| `release` | resource: handle | true |
| `preload_group` | group: string, resources: array | true |
| `unload_group` | group: string | true |
| `reload` | resource: handle | true |
| `watch` | resource: handle | token: handle |
| `get_total_memory_usage` | — | bytes: integer |

### `wotb.async` — `async_v1.h`

- C ABI: `WotbModV3AsyncApiV1`, версия `WOTBMOD_V3_ASYNC_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (6): `MAX_TASK_RESULT`, `TASK_QUEUED`, `TASK_RUNNING`, `TASK_COMPLETED`, `TASK_FAILED`, `TASK_CANCELLED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `task_submit` | info: table:TaskSubmitInfo | task: handle |
| `task_cancel` | task: handle | true |
| `task_get_info` | task: handle | info: table:TaskInfo |
| `task_get_state` | task: handle | state: integer |
| `task_get_progress` | task: handle | progress: number |
| `task_set_progress` | task: handle, progress: number | true |
| `task_set_result` | task: handle, data: void, size: integer | true |
| `task_get_result` | task: handle | result: table:Buffer |
| `task_is_cancellation_requested` | task: handle | requested: integer |
| `dispatch_to_main_thread` | callback: function | task: handle |
| `dispatch_to_render_thread` | callback: function | task: handle |
| `dispatch_to_audio_thread` | callback: function | task: handle |
| `pump_current_thread` | max_callbacks: integer | executed: integer |
| `timer_create` | info: table:TimerCreateInfo | timer: handle |
| `timer_cancel` | timer: handle | true |
| `timer_pause` | timer: handle | true |
| `timer_resume` | timer: handle | true |
| `timer_get_info` | timer: handle | info: table:TimerInfo |
| `thread_get_current_role` | — | role: integer |
| `thread_is_main` | — | is_main: integer |

### `wotb.http` — `http_v1.h`

- C ABI: `WotbModV3HttpApiV1`, версия `WOTBMOD_V3_HTTP_VERSION`;
- права: `network.http`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (8): `DEFAULT_MAX_RESPONSE`, `ABSOLUTE_MAX_RESPONSE`, `CREATED`, `CONFIGURED`, `SENDING`, `COMPLETED`, `FAILED`, `CANCELLED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `request_create` | — | request: handle |
| `request_set_method` | request: handle, method: string | true |
| `request_set_url` | request: handle, url: string | true |
| `request_set_header` | request: handle, name: string, value: string | true |
| `request_set_body` | request: handle, data: void, size: integer | true |
| `request_set_timeout` | request: handle, timeout_ms: integer | true |
| `request_set_max_response_size` | request: handle, max_response_size: integer | true |
| `request_send_async` | request: handle, completion: function | true |
| `request_cancel` | request: handle | true |
| `request_get_info` | request: handle | info: table:HttpRequestInfo |
| `response_get_status` | request: handle | status: integer |
| `response_get_header` | request: handle, name: string | string |
| `response_get_body` | request: handle | body: table:Buffer |

### `wotb.intermod` — `intermod_v1.h`

- C ABI: `WotbModV3IntermodApiV1`, версия `WOTBMOD_V3_INTERMOD_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (3): `MAX_SERVICE_ID`, `MAX_MESSAGE_TOPIC`, `MAX_PAYLOAD`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `mod_find` | mod_id: string | mod: handle |
| `mod_is_loaded` | mod_id: string | loaded: integer |
| `mod_get_version` | target_mod: handle | string |
| `mod_get_dependency` | dependency_id: string | dependency: handle, optional: integer |
| `export_interface` | — | *не публикуется в Lua: нужен сырой нативный указатель* |
| `unexport_interface` | export_token: handle | true |
| `import_interface` | service_id: string, minimum_version: integer | interface: table:ImportedInterface |
| `enumerate_interfaces` | — | interfaces: array |
| `message_publish` | topic: string, payload: void, payload_size: integer | true |
| `message_subscribe` | topic_pattern: string, priority: integer, callback: function | token: handle |
| `message_unsubscribe` | token: handle | true |

### `wotb.render` — `render_v1.h`

- C ABI: `WotbModV3RenderApiV1`, версия `WOTBMOD_V3_RENDER_VERSION`;
- права: `battle.render.overlay`, `render.callbacks`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (28): `BACKEND_NONE`, `BACKEND_D3D11`, `BACKEND_D3D12`, `BACKEND_VULKAN`, `BACKEND_METAL`, `BACKEND_OPENGL`, `PHASE_BEFORE_UI`, `PHASE_AFTER_UI`, `PHASE_PRESENT`, `RESOURCE_TEXTURE`, `RESOURCE_MATERIAL`, `TEXTURE_RGBA8_UNORM`, `TEXTURE_BGRA8_UNORM`, `TEXTURE_R8_UNORM`, `TEXTURE_RGBA16_FLOAT`, `PARAMETER_FLOAT`, `PARAMETER_VEC2`, `PARAMETER_VEC3`, `PARAMETER_VEC4`, `PARAMETER_COLOR`, `PARAMETER_MATRIX4`, `PARAMETER_TEXTURE`, `LIFECYCLE_BACKEND`, `LIFECYCLE_DEVICE`, `LIFECYCLE_CONTEXT`, `LIFECYCLE_SWAPCHAIN`, `LIFECYCLE_VIEWPORT`, `LIFECYCLE_AVAILABILITY`.

| Слот | Аргументы | Результат |
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

### `wotb.render_native` — `render_v1.h`

- C ABI: `WotbModV3RenderNativeApiV1`, версия `WOTBMOD_V3_RENDER_NATIVE_VERSION`;
- права: `render.native`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы: нет.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_native_device` | — | device: void |
| `get_native_context` | — | context: void |
| `get_native_swapchain` | — | swapchain: void |

### `wotb.camera` — `camera_v1.h`

- C ABI: `WotbModV3CameraApiV1`, версия `WOTBMOD_V3_CAMERA_VERSION`;
- права: `camera.battle.read`, `camera.hangar`, `camera.replay`, `gameplay.tweak.camera`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `MODE_UNKNOWN`, `MODE_HANGAR`, `MODE_ARCADE`, `MODE_SNIPER`, `MODE_POSTMORTEM`, `MODE_REPLAY`, `MODE_FREE`, `MODE_CINEMATIC`, `MODIFIER_BEFORE_GAME`, `MODIFIER_AFTER_GAME`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_active` | — | camera: handle |
| `get_mode` | camera: handle | mode: integer |
| `get_transform` | camera: handle | transform: table:Transform |
| `set_transform` | camera: handle, transform: table:Transform | true |
| `get_fov` | camera: handle | degrees: number |
| `set_fov` | camera: handle, degrees: number | true |
| `get_near_plane` | camera: handle | near_plane: number |
| `get_far_plane` | camera: handle | far_plane: number |
| `world_to_screen` | camera: handle, world: table:Vec3 | screen: table:Vec3 |
| `screen_to_world` | camera: handle, screen: table:Vec3 | world: table:Vec3 |
| `add_modifier` | phase: integer, priority: integer, callback: function | token: handle |
| `remove_modifier` | token: handle | true |
| `transition_to` | camera: handle, transition: table:CameraTransition | true |
| `add_shake` | camera: handle, shake: table:CameraShake | true |

### `wotb.scene` — `scene_v1.h`

- C ABI: `WotbModV3SceneApiV1`, версия `WOTBMOD_V3_SCENE_VERSION`;
- права: `hangar.scene`, `resources.mod`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (11): `ATTACHMENT_DETACH_ON_PARENT_DESTROY`, `ATTACHMENT_DESTROY_ON_PARENT_DESTROY`, `ATTACHMENT_KEEP_WORLD_TRANSFORM`, `ATTACHMENT_KEEP_LOCAL_TRANSFORM`, `PARAMETER_FLOAT`, `PARAMETER_VEC2`, `PARAMETER_VEC3`, `PARAMETER_VEC4`, `PARAMETER_COLOR`, `PARAMETER_MATRIX4`, `PARAMETER_TEXTURE_URI`.

| Слот | Аргументы | Результат |
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

### `wotb.audio` — `audio_v2.h`

- C ABI: `WotbModV3AudioApiV2`, версия `WOTBMOD_V3_AUDIO_VERSION`;
- права: `audio.custom`, `audio.events`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `STATE_CREATED`, `STATE_PRELOADED`, `STATE_PLAYING`, `STATE_PAUSED`, `STATE_STOPPED`, `STATE_ERROR`, `CREATE_NONE`, `CREATE_SPATIAL`, `CREATE_LOOP`, `CREATE_STREAM`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `create` | descriptor: table:AudioDescriptor | audio: handle |
| `create_stream` | descriptor: table:AudioDescriptor | audio: handle |
| `preload` | audio: handle | true |
| `play` | audio: handle | true |
| `pause` | audio: handle | true |
| `resume` | audio: handle | true |
| `stop` | audio: handle, fade_seconds: number | true |
| `destroy` | audio: handle | true |
| `is_playing` | audio: handle | playing: integer |
| `get_state` | audio: handle | state: integer |
| `set_volume` | audio: handle, volume: number | true |
| `set_pitch` | audio: handle, pitch: number | true |
| `set_pan` | audio: handle, pan: number | true |
| `set_bus` | audio: handle, bus: string | true |
| `set_position_3d` | audio: handle, position: table:Vec3 | true |
| `set_min_distance` | audio: handle, distance: number | true |
| `set_max_distance` | audio: handle, distance: number | true |
| `fade_to` | audio: handle, target_volume: number, duration_seconds: number | true |
| `seek` | audio: handle, seconds: number | true |
| `get_position` | audio: handle | seconds: number |
| `get_duration` | audio: handle | seconds: number |
| `set_loop` | audio: handle, loop: integer | true |
| `on_started` | audio: handle, callback: function | token: handle |
| `on_finished` | audio: handle, callback: function | token: handle |
| `on_error` | audio: handle, callback: function | token: handle |
| `unsubscribe` | token: handle | true |
| `sound_override_register` | event_name: string, replacement_uri: string, priority: integer | token: handle |
| `sound_override_unregister` | token: handle | true |
| `sound_override_set_priority` | token: handle, priority: integer | true |
| `sound_override_get_conflicts` | event_name: string, visitor: function | true |
| `sound_event_create` | event_name: string | event: handle |
| `sound_event_trigger` | event: handle | true |
| `sound_event_stop` | event: handle, force: integer | true |
| `sound_event_set_paused` | event: handle, paused: integer | true |
| `sound_event_set_volume` | event: handle, volume: number | true |
| `sound_event_set_position` | event: handle, position: table:Vec3 | true |
| `sound_event_set_parameter` | event: handle, name: string, value: number | true |
| `sound_event_get_parameter` | event: handle, name: string | value: number |
| `sound_event_has_parameter` | event: handle, name: string | has_parameter: integer |
| `sound_event_get_name` | event: handle | string |
| `sound_event_get_bus` | event: handle | string |
| `sound_event_set_speed` | event: handle, speed: number | true |
| `sound_event_set_direction` | event: handle, direction: table:Vec3 | true |
| `sound_event_set_velocity` | event: handle, velocity: table:Vec3 | true |
| `sound_event_set_loop_count` | event: handle, loop_count: integer | true |
| `sound_event_set_priority` | event: handle, priority: integer | true |
| `sound_event_destroy` | event: handle | true |
| `set_listener_transform` | transform: table:Transform | true |
| `set_bus_volume` | bus: string, volume: number | true |

### `wotb.vehicle_visual` — `vehicle_visual_v2.h`

- C ABI: `WotbModV3VehicleVisualApiV2`, версия `WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2`;
- права: `gameplay.tweak.vehicle`, `vehicle.local.cosmetic`, `game.entity.public`, `resources.mod`, `resources.overlay.game`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (14): `APPEARANCE_UNKNOWN`, `APPEARANCE_LOADING`, `APPEARANCE_READY`, `APPEARANCE_DESTROYED`, `PART_HULL`, `PART_TURRET`, `PART_GUN`, `PART_TRACKS`, `PART_CHASSIS`, `PART_EFFECTS`, `SKIN_MAX_ASSETS`, `SKIN_MESH`, `SKIN_MATERIAL`, `SKIN_TEXTURE`.

| Слот | Аргументы | Результат |
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

### `wotb.gameplay_camera` — `gameplay_camera_v1.h`

- C ABI: `WotbModV3GameplayCameraApiV1`, версия `WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION`;
- права: `gameplay.tweak.camera`, `gameplay.tweak.freecam`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `TRANSITION_SMOOTH`, `TRANSITION_INSTANT`, `TRANSITION_CUSTOM`, `EASING_LINEAR`, `EASING_IN_QUAD`, `EASING_OUT_QUAD`, `EASING_IN_OUT_QUAD`, `EASING_IN_CUBIC`, `EASING_OUT_CUBIC`, `EASING_IN_OUT_CUBIC`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `set_fov` | degrees: number | true |
| `get_fov` | — | degrees: number |
| `set_fov_hangar` | degrees: number | true |
| `set_fov_battle` | degrees: number | true |
| `set_fov_sniper` | degrees: number | true |
| `reset_fov` | — | true |
| `set_zoom_steps` | steps: number, count: integer | true |
| `get_zoom_steps` | — | steps: number |
| `set_zoom_multiplier` | multiplier: number | true |
| `set_max_zoom` | multiplier: number | true |
| `set_transition_mode` | mode: integer | true |
| `set_transition_duration` | milliseconds: number | true |
| `set_transition_easing` | easing: integer | true |
| `set_shake_enabled` | enabled: integer | true |
| `set_shake_intensity` | multiplier: number | true |
| `set_postprocessing_enabled` | enabled: integer | true |
| `set_bloom_enabled` | enabled: integer | true |
| `set_motion_blur_enabled` | enabled: integer | true |
| `set_vignette_enabled` | enabled: integer | true |
| `set_color_grading` | lut_uri: string | true |
| `set_dof_enabled` | enabled: integer | true |
| `set_dof_params` | focus: number, aperture: number, max_blur: number | true |
| `free_enable` | enabled: integer | true |
| `free_set_speed` | units_per_second: number | true |
| `free_set_position` | position: table:Vec3 | true |
| `free_set_rotation` | pitch_yaw_roll: table:Vec3 | true |
| `free_get_position` | — | position: table:Vec3 |
| `free_get_rotation` | — | pitch_yaw_roll: table:Vec3 |
| `get_config` | — | config: table:GameplayCameraConfig |
| `apply_config` | config: table:GameplayCameraConfig | true |

### `wotb.gameplay_hud` — `gameplay_hud_v1.h`

- C ABI: `WotbModV3GameplayHudApiV1`, версия `WOTBMOD_V3_GAMEPLAY_HUD_VERSION`;
- права: `gameplay.tweak.hud`, `battle.ui`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (19): `ANCHOR_TOP_LEFT`, `ANCHOR_TOP`, `ANCHOR_TOP_RIGHT`, `ANCHOR_LEFT`, `ANCHOR_CENTER`, `ANCHOR_RIGHT`, `ANCHOR_BOTTOM_LEFT`, `ANCHOR_BOTTOM`, `ANCHOR_BOTTOM_RIGHT`, `HIT_STYLE_GAME_DEFAULT`, `HIT_STYLE_COMPACT`, `HIT_STYLE_DIRECTIONAL`, `HIT_STYLE_MINIMAL`, `STAT_DAMAGE`, `STAT_KILLS`, `STAT_WN8`, `STAT_WINRATE`, `STAT_SHOTS`, `STAT_PENETRATION_RATE`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `reticle_set_texture` | texture_uri: string | true |
| `reticle_set_color` | rgba: integer | true |
| `reticle_set_size` | scale: number | true |
| `reticle_set_sniper_texture` | texture_uri: string | true |
| `reticle_set_reloading_indicator` | enabled: integer | true |
| `reticle_set_dispersion_circle` | enabled: integer | true |
| `damagelog_set_enabled` | enabled: integer | true |
| `damagelog_set_position` | anchor: integer | true |
| `damagelog_set_max_entries` | count: integer | true |
| `damagelog_set_show_blocked` | enabled: integer | true |
| `damagelog_set_show_ricochet` | enabled: integer | true |
| `damagelog_set_show_module_damage` | enabled: integer | true |
| `damagelog_set_format` | format: string | true |
| `damagelog_set_filter_own` | enabled: integer | true |
| `session_stats_set_enabled` | enabled: integer | true |
| `session_stats_set_fields` | fields: integer | true |
| `minimap_set_size` | scale: number | true |
| `minimap_set_opacity` | opacity: number | true |
| `minimap_set_show_last_known` | enabled: integer | true |
| `minimap_set_show_artillery_range` | enabled: integer | true |
| `minimap_set_show_drawing` | enabled: integer | true |
| `minimap_add_marker` | world_x: number, world_z: number, label: string, color: integer | marker_id: integer |
| `minimap_remove_marker` | marker_id: integer | true |
| `sixth_sense_set_texture` | texture_uri: string | true |
| `sixth_sense_set_sound` | audio_uri: string | true |
| `sixth_sense_set_position` | position: table:Vec2 | true |
| `sixth_sense_set_scale` | scale: number | true |
| `sixth_sense_set_delay_ms` | milliseconds: number | true |
| `hit_indicator_set_style` | style: integer | true |
| `hit_indicator_set_color_hit` | rgba: integer | true |
| `hit_indicator_set_color_pen` | rgba: integer | true |
| `hit_indicator_set_color_ricochet` | rgba: integer | true |
| `hit_indicator_set_color_crit` | rgba: integer | true |
| `reset` | — | true |

### `wotb.gameplay_hangar` — `gameplay_hangar_v1.h`

- C ABI: `WotbModV3GameplayHangarApiV1`, версия `WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION`;
- права: `gameplay.tweak.hangar`, `hangar.scene`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (9): `LIGHTING_GAME_DEFAULT`, `LIGHTING_DAY`, `LIGHTING_NIGHT`, `LIGHTING_STUDIO`, `LIGHTING_CINEMATIC`, `UI_BANNERS`, `UI_NEWS`, `UI_OFFERS`, `UI_CHAT`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `set_background` | texture_uri: string | true |
| `set_background_video` | video_uri: string | true |
| `set_music` | audio_uri: string | true |
| `set_lighting` | preset: integer | true |
| `set_vehicle_preview_angle` | yaw: number, pitch: number | true |
| `set_vehicle_preview_zoom` | zoom: number | true |
| `set_floor_texture` | texture_uri: string | true |
| `set_skybox` | texture_uri: string | true |
| `hide_ui_elements` | mask: integer | true |
| `set_camera_orbit_speed` | multiplier: number | true |
| `reset` | — | true |

### `wotb.gameplay_replay` — `gameplay_replay_v1.h`

- C ABI: `WotbModV3GameplayReplayApiV1`, версия `WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION`;
- права: `gameplay.tweak.replay`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (5): `CAMERA_FREE`, `CAMERA_FOLLOW_VEHICLE`, `CAMERA_TOP_DOWN`, `CAMERA_CINEMATIC`, `CAMERA_FIRST_PERSON`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `set_speed` | multiplier: number | true |
| `seek` | timestamp_seconds: number | true |
| `get_duration` | — | seconds: number |
| `get_position` | — | seconds: number |
| `add_marker` | timestamp_seconds: number, label: string | marker_id: integer |
| `remove_marker` | marker_id: integer | true |
| `set_camera_mode` | mode: integer | true |
| `set_follow_vehicle` | public_entity_id: integer | true |
| `export_clip` | start_seconds: number, end_seconds: number, output_uri: string | true |

### `wotb.entity_public` — `entity_public_v1.h`

- C ABI: `WotbModV3EntityPublicApiV1`, версия `WOTBMOD_V3_ENTITY_PUBLIC_VERSION`;
- права: `entity.public.visible`, `game.entity.public`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (17): `TYPE_UNKNOWN`, `TYPE_VEHICLE`, `TYPE_PROJECTILE`, `TYPE_EFFECT`, `VALUE_BOOL`, `VALUE_INT64`, `VALUE_DOUBLE`, `VALUE_VEC3`, `VALUE_STRING`, `REASON_UNKNOWN`, `REASON_LOCAL_PLAYER`, `REASON_VISIBLE`, `REASON_UPDATED`, `REASON_HIDDEN`, `REASON_LEFT_WORLD`, `REASON_NATIVE_DESTROYED`, `REASON_SHUTDOWN`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_public_id` | entity: handle | public_id: integer |
| `get_public_type` | entity: handle | string |
| `get_public_property` | entity: handle, property: string | value: table:PublicValue |
| `subscribe_public_property` | entity: handle, property: string, callback: function | token: handle |
| `unsubscribe_public_property` | token: handle | true |
| `is_visible_to_player` | entity: handle | visible: integer |
| `get_snapshot` | entity: handle | snapshot: table:PublicEntitySnapshot |
| `enumerate_visible` | visitor: function | true |

### `wotb.bigworld_rpc` — `bigworld_rpc_v1.h`

- C ABI: `WotbModV3BigWorldRpcApiV1`, версия `WOTBMOD_V3_BIGWORLD_RPC_VERSION`;
- права: `bigworld.rpc.observe`, `bigworld.observe`, `bigworld.rpc.metadata`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (2): `RPC_INCOMING`, `RPC_OUTGOING`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_policy` | — | policy: table:BigWorldRpcPolicy |
| `subscribe_observed` | direction_mask: integer, method_filter: string, callback: function | token: handle |
| `unsubscribe_observed` | token: handle | true |

### `wotb.projectile` — `projectile_v2.h`

- C ABI: `WotbModV3ProjectileApiV2`, версия `WOTBMOD_V3_PROJECTILE_VERSION_2`;
- права: `gameplay.tweak.projectile_visual`, `visible.projectile.events`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (30): `OWNER_UNKNOWN`, `OWNER_LOCAL_PLAYER`, `OWNER_ALLY_VISIBLE`, `OWNER_ENEMY_VISIBLE`, `OWNER_REPLAY`, `STATE_CREATED`, `STATE_IN_FLIGHT`, `STATE_IMPACTED`, `STATE_DESTROYED`, `SOURCE_STOCK_SHOT`, `SOURCE_NATIVE_IMPACT`, `SOURCE_STOCK_TRACER`, `SOURCE_API_MANAGED`, `FIELD_NATIVE_SHOT_ID`, `FIELD_PRIMARY_ENTITY`, `FIELD_SECONDARY_ENTITY`, `FIELD_SHELL_TYPE`, `FIELD_ORIGIN`, `FIELD_DIRECTION`, `FIELD_POSITION`, `FIELD_IMPACT_POSITION`, `FIELD_STOCK_SHOT_CODE`, `FIELDS_ALL`, `DESTROY_NATIVE`, `DESTROY_IMPACT_COMPLETE`, `DESTROY_TIMEOUT`, `DESTROY_ENTITY_REMOVED`, `DESTROY_SHUTDOWN`, `IMPACT_VISUAL_NONE`, `IMPACT_VISUAL_NATIVE_SCENE`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `tracer_style_register` | descriptor: table:TracerStyleDescriptor | style: handle |
| `tracer_style_unregister` | style: handle | true |
| `tracer_set_texture` | style: handle, texture_uri: string | true |
| `tracer_set_color` | style: handle, color: table:Color | true |
| `tracer_set_width` | style: handle, width: number | true |
| `tracer_set_lifetime` | style: handle, seconds: number | true |
| `tracer_set_fade` | style: handle, fade_start: number | true |
| `projectile_get_visual_entity` | projectile: handle | entity: handle |
| `projectile_attach_visual` | projectile: handle, entity: handle, lifetime_policy: integer | true |
| `projectile_get_owner_scope` | projectile: handle | scope: integer |
| `projectile_get_snapshot` | projectile: handle | snapshot: table:ProjectileSnapshot |
| `impact_visual_register` | descriptor: table:ImpactVisualDescriptor | visual: handle |
| `impact_visual_update` | visual: handle, descriptor: table:ImpactVisualDescriptor | true |
| `impact_visual_unregister` | visual: handle | true |

### `wotb.yaml` — `yaml_v1.h`

- C ABI: `WotbModV3YamlApiV1`, версия `WOTBMOD_V3_YAML_VERSION`;
- права: `resources.mod`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (8): `INVALID_NODE`, `NULL`, `MAP`, `SEQUENCE`, `STRING`, `BOOL`, `INT`, `FLOAT`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `parse` | yaml_utf8: table:ConstBuffer, limits: table:YamlLimits | document: handle |
| `parse_uri` | uri: string, limits: table:YamlLimits | document: handle |
| `get_root` | document: handle | node: handle |
| `get_type` | document: handle, node: handle | type: integer |
| `get_size` | document: handle, node: handle | size: integer |
| `map_get` | document: handle, node: handle, key: string | child: handle |
| `sequence_get` | document: handle, node: handle, index: integer | child: handle |
| `get_string` | document: handle, node: handle | string |
| `get_bool` | document: handle, node: handle | value: integer |
| `get_int` | document: handle, node: handle | value: integer |
| `get_float` | document: handle, node: handle | value: number |

### `wotb.archive` — `archive_v1.h`

- C ABI: `WotbModV3ArchiveApiV1`, версия `WOTBMOD_V3_ARCHIVE_VERSION`;
- права: `resources.mod`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы: нет.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `open_directory` | physical_directory: string, limits: table:ArchiveLimits | archive: handle |
| `open_package_file` | physical_file: string, limits: table:ArchiveLimits | archive: handle |
| `get_entry_count` | archive: handle | count: integer |
| `get_entry` | archive: handle, index: integer | entry: table:ArchiveEntry |
| `read_entry` | archive: handle, relative_path: string | bytes |
| `extract_entry` | archive: handle, relative_path: string, destination_uri: string | true |
| `extract_all` | archive: handle, destination_uri: string | true |
| `get_sha256` | archive: handle | sha256: string |
| `verify_sha256` | archive: handle, expected_sha256: string | true |
| `cancel` | archive: handle | true |

### `wotb.loaders` — `loaders_v1.h`

- C ABI: `WotbModV3LoadersApiV1`, версия `WOTBMOD_V3_LOADERS_VERSION`;
- права: `resources.mod`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (6): `UTF8_TEXT`, `BINARY`, `MINIMAL_YAML`, `DAVA_YAML`, `DVPL`, `DAVA_ARCHIVE`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `load_text_utf8` | uri: string, max_bytes: integer | bytes |
| `load_binary` | uri: string, max_bytes: integer | bytes |
| `load_yaml` | uri: string, limits: table:YamlLimits | document: handle |
| `load_dava_yaml` | uri: string | document: handle |
| `unpack_dvpl` | uri: string | bytes |
| `open_dava_archive` | uri: string | archive: handle |
| `get_backend_info` | backend: integer | info: table:LoaderBackendInfo |

### `wotb.client` — `client_v1.h`

- C ABI: `WotbModV3ClientApiV1`, версия `WOTBMOD_V3_CLIENT_VERSION`;
- права: `client.leave_to_hangar`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (5): `COMPATIBILITY_UNKNOWN`, `COMPATIBILITY_SUPPORTED`, `COMPATIBILITY_DEGRADED`, `COMPATIBILITY_BINDINGS_MISSING`, `COMPATIBILITY_HASH_MISMATCH`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `is_supported` | — | supported: integer |
| `get_binding_pack_version` | — | version: integer |
| `get_compatibility_state` | — | state: integer |
| `enumerate_missing_bindings` | visitor: function | true |
| `leave_to_hangar` | — | true |

### `wotb.device` — `device_v1.h`

- C ABI: `WotbModV3DeviceApiV1`, версия `WOTBMOD_V3_DEVICE_VERSION`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (5): `ARCH_UNKNOWN`, `ARCH_X86`, `ARCH_X64`, `ARCH_ARM32`, `ARCH_ARM64`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_info` | — | info: table:DeviceInfo |
| `get_graphics_adapter_count` | — | count: integer |
| `get_graphics_adapter_at` | index: integer | info: table:GraphicsAdapterInfo |

### `wotb.diagnostics` — `diagnostics_v2.h`

- C ABI: `WotbModV3DiagnosticsApiV2`, версия `WOTBMOD_V3_DIAGNOSTICS_VERSION_2`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (14): `MAX_CONTEXT_ENTRIES`, `MAX_BREADCRUMBS`, `MAX_CONTEXT_KEY`, `MAX_CONTEXT_VALUE`, `MAX_ACTION`, `MAX_BREADCRUMB_CATEGORY`, `MAX_BREADCRUMB_MESSAGE`, `MAX_BREADCRUMB_CONTEXT`, `MOD_CREATED`, `MOD_LOADED`, `MOD_ENABLED`, `MOD_DISABLED`, `MOD_UNLOADING`, `MOD_FAULTED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_stats` | — | stats: table:DiagnosticsStats |
| `copy_report_json` | — | string |
| `report_fault` | category: string, message: string, context_json: string | true |
| `crash_add_context` | key: string, value: string | true |
| `crash_set_last_action` | action: string | true |
| `crash_add_breadcrumb` | category: string, message: string, context_json: string | true |
| `export_bundle` | bundle_name: string | string |
| `get_mod_health` | — | health: table:ModHealth |

### `wotb.devtools` — `devtools_v3.h`

- C ABI: `WotbModV3DevtoolsApiV3`, версия `WOTBMOD_V3_DEVTOOLS_VERSION_3`;
- права: `core`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы: нет.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `marker` | category: string, name: string, context_json: string | true |
| `span_begin` | category: string, name: string | span: handle |
| `span_end` | span: handle | info: table:ProfilerSpanInfo |
| `counter_set` | category: string, name: string, value: number | true |
| `inspect_ui` | selector: string | string |
| `inspect_scene` | selector: string | string |
| `inspect_material` | selector: string | string |
| `inspect_resource` | selector: string | string |
| `inspect_hook_chain` | selector: string | string |
| `inspect_events` | selector: string | string |
| `profiler_get_mod_cpu_time` | — | aggregate: table:ProfilerAggregate |
| `profiler_get_mod_memory` | — | memory: table:ProfilerMemory |
| `get_callback_profile` | — | profile: table:CallbackProfile |
| `set_callback_budget` | budget_microseconds: integer, max_callbacks_per_frame: integer | true |
| `reset_callback_profile` | — | true |

### `wotb.manifest` — `manifest_v1.h`

- C ABI: `WotbModV3ManifestApiV1`, версия `WOTBMOD_V3_MANIFEST_VERSION`;
- права: `content`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `PACKAGE_NATIVE`, `PACKAGE_CONTENT_ONLY`, `DEPENDENCY_REQUIRED`, `DEPENDENCY_OPTIONAL`, `DEPENDENCY_INCOMPATIBLE`, `SIGNATURE_UNSIGNED`, `SIGNATURE_DECLARED`, `SIGNATURE_VALID`, `SIGNATURE_INVALID`, `SIGNATURE_UNSUPPORTED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `parse_json` | json_utf8: table:ConstBuffer | manifest: handle |
| `parse_uri` | uri: string | manifest: handle |
| `validate` | manifest: handle | true |
| `get_info` | manifest: handle | info: table:ManifestInfo |
| `get_api_requirement` | manifest: handle, index: integer | requirement: table:ManifestApiRequirement |
| `get_dependency` | manifest: handle, index: integer | dependency: table:ManifestDependency |
| `get_permission` | manifest: handle, index: integer | string |
| `get_resource_pattern` | manifest: handle, index: integer | string |
| `get_locale_pattern` | manifest: handle, index: integer | string |
| `get_entrypoint` | manifest: handle, index: integer | entrypoint: table:ManifestEntrypoint |
| `get_client_build` | manifest: handle, index: integer | string |
| `get_client_executable_hash` | manifest: handle, index: integer | string |
| `resolve_dependencies` | manifest: handle, installed: array | report: table:DependencyReport |
| `verify_content_sha256` | physical_file: string, expected_sha256: string | true |
| `verify_signature` | manifest: handle, physical_package: string | true |

### `wotb.catalog` — `catalog_v1.h`

- C ABI: `WotbModV3CatalogApiV1`, версия `WOTBMOD_V3_CATALOG_VERSION`;
- права: `content`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (4): `STATUS_VERIFIED`, `STATUS_COMMUNITY`, `STATUS_UNREVIEWED`, `STATUS_BANNED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `validate_record` | record: table:CatalogRecord | true |
| `evaluate_install` | record: table:CatalogRecord, manifest_id: string, manifest_version: string, package_sha256: string | decision: table:CatalogDecision |
| `status_name` | status: integer | string |

### `wotb.content` — `content_v1.h`

- C ABI: `WotbModV3ContentApiV1`, версия `WOTBMOD_V3_CONTENT_VERSION`;
- права: `content`, `resources.mod`, `resources.overlay.game`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (6): `OVERRIDE_AUDIO`, `OVERRIDE_TEXTURE`, `OVERRIDE_UI`, `OVERRIDE_HANGAR`, `OVERRIDE_MODEL`, `OVERRIDE_LOCALIZATION`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `parse_json` | json_utf8: table:ConstBuffer | content: handle |
| `parse_uri` | uri: string | content: handle |
| `validate` | content: handle | true |
| `get_info` | content: handle | info: table:ContentInfo |
| `get_override` | content: handle, index: integer | override: table:ContentOverride |
| `apply` | content: handle | true |
| `unapply` | content: handle | true |

### `wotb.ui_read` — `ui_v4.h`

- C ABI: `WotbModV3UiApiV4`, версия `WOTBMOD_V3_UI_VERSION_4`;
- права: `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `GAME_OWNED`, `TEXT_SET`, `TEXTURE_SET`, `FONT_SET`, `COLOR_SET`, `BACKGROUND_COLOR_SET`, `OPACITY_SET`, `FONT_SIZE_SET`, `TEXT_LAYOUT_SET`, `LIVE_TEXT_SET`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `control_get_text` | control: handle | string |
| `control_get_texture` | control: handle | string |
| `control_get_font` | control: handle | string |
| `control_get_style` | control: handle | style: table:UiStyleSnapshot |
| `control_get_live_text` | control: handle | string |

### `wotb.camera_state` — `camera_v2.h`

- C ABI: `WotbModV3CameraApiV2`, версия `WOTBMOD_V3_CAMERA_VERSION_2`;
- права: `camera.battle.read`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (11): `ANIMATION_ORDINARY_A`, `ANIMATION_ORDINARY_B`, `ANIMATION_LOOK_OUT`, `ANIMATION_POST_MORTEM`, `ANIMATION_LOOK_ON_TARGET`, `ANIMATION_NONE`, `ANIMATION_OBSERVER`, `ANIMATION_UNKNOWN`, `VIEW_SNIPER`, `VIEW_ARCADE`, `VIEW_UNKNOWN`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_animation_state` | — | state: integer |
| `get_view_mode` | — | mode: integer |
| `get_observed_state` | — | state: table:CameraObservedState |

### `wotb.audio_intercept` — `audio_v3.h`

- C ABI: `WotbModV3AudioApiV3`, версия `WOTBMOD_V3_AUDIO_VERSION_3`;
- права: `audio.events`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (3): `PASS_THROUGH`, `SUBSTITUTE`, `SUPPRESS`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `intercept_register` | event_name: string, priority: integer, callback: function | token: handle |
| `intercept_unregister` | token: handle | true |
| `intercept_set_priority` | token: handle, priority: integer | true |
| `is_intercept_active` | — | active: integer |

### `wotb.scene_enumerate` — `scene_v2.h`

- C ABI: `WotbModV3SceneApiV2`, версия `WOTBMOD_V3_SCENE_VERSION_2`;
- права: `game.entity.public`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (10): `WALK_MAX_DEPTH`, `WALK_MAX_CHILDREN`, `WALK_MAX_NODES`, `NODE_NAME_SIZE`, `NODE_NO_PARENT`, `NODE_IS_SCENE`, `NODE_HAS_WORLD_MATRIX`, `NODE_NAME_TRUNCATED`, `NODE_CHILDREN_TRUNCATED`, `NODE_DEPTH_LIMITED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_limits` | — | limits: table:SceneWalkLimits |
| `walk_active_scene` | request: table:SceneWalkRequest | nodes: table:SceneNodeRecord |

### `wotb.tracer` — `tracer_v1.h`

- C ABI: `WotbModV3TracerApiV1`, версия `WOTBMOD_V3_TRACER_VERSION`;
- права: `visible.projectile.events`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (13): `SHELL_TYPE_COUNT`, `STYLE_NAME_SIZE`, `STYLE_UNKNOWN`, `STYLE_ARMOR_PIERCING`, `STYLE_ARMOR_PIERCING_CR`, `STYLE_HIGH_EXPLOSIVE`, `STYLE_HOLLOW_CHARGE`, `STYLE_ANTI_TANK_GUIDED_MISSILE`, `STYLE_RAILGUN`, `STYLE_IMPROVED_DETECTION`, `STYLE_STT_TRACER`, `STYLE_NAME_VALID`, `STYLE_COLOR_VALID`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `get_shell_type_count` | — | count: integer |
| `get_style_for_shell_type` | shell_type: integer | style: table:TracerStyle |

### `wotb.ges` — `ges_v1.h`

- C ABI: `WotbModV3GesApiV1`, версия `WOTBMOD_V3_GES_VERSION`;
- права: `ges.observe`;
- биндинг: ручной, `loader/lua/lua_bind_ges.cpp`;
- константы (15): `TYPE_NAME_SIZE`, `TOPIC_PREFIX`, `EVENT_MOD_PUBLISHED`, `EVENT_SIZE_KNOWN`, `EVENT_SCHEMA_KNOWN`, `PUBLISH_ECHO`, `FIELD_I32`, `FIELD_U32`, `FIELD_F32`, `FIELD_BOOL`, `FIELD_PTR`, `FIELD_CSTR`, `FIELD_FASTNAME`, `FIELD_BYTES`, `FIELD_U8`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `list_types` | names: char*, capacity: integer | count: integer |
| `read_i32` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_u32` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_f32` | event: table:GesEvent, offset: integer, out: number | true |
| `read_bool` | event: table:GesEvent, offset: integer, out: integer | true |
| `read_ptr` | event: table:GesEvent, offset: integer, out: void* | true |
| `read_cstring` | event: table:GesEvent, offset: integer, buffer: string, capacity: integer | true |
| `get_schema` | type_name: string | schema_id: integer, payload_size: integer, field_count: integer |
| `schema_field` | schema_id: integer, index: integer | field: table:GesField |
| `publish` | type_name: string, payload: void, payload_size: integer, flags: integer | true |

### `wotb.session_cluster` — `session_cluster_v1.h`

- C ABI: `WotbModV3SessionClusterApiV1`, версия `WOTBMOD_V3_SESSION_CLUSTER_VERSION`;
- права: `session.cluster.read`;
- биндинг: генератор `tools/generate_lua_bindings.py`;
- константы (5): `AUTO`, `CHANGE_QUEUED`, `CHANGE_STARTED`, `CHANGE_CONNECTED`, `CHANGE_FAILED`.

| Слот | Аргументы | Результат |
| --- | --- | --- |
| `enumerate` | — | items: array |
| `get_current` | — | info: table:ClusterInfo |
| `change` | cluster_id: integer | true |

