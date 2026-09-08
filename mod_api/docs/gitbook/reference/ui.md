# `wotb.ui`

Raw-таблица интерфейса `WotbModV3UiApiV3` (`include/wotbmod/ui_v3.h`, версия `WOTBMOD_V3_UI_VERSION_3`). Функции ниже вызываются как `wotb.ui.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.screen`](../facades/wotb-screen.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui`. Имена объявляются в `permissions` манифеста.

Биндинг ручной: `loader/lua/lua_bind_ui.cpp (ядро; остальные слоты — генератор)`.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.ui.CONTROL_CONTAINER` | `WOTBMOD_V3_UI_CONTROL_CONTAINER` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_TEXT` | `WOTBMOD_V3_UI_CONTROL_TEXT` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_IMAGE` | `WOTBMOD_V3_UI_CONTROL_IMAGE` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_BUTTON` | `WOTBMOD_V3_UI_CONTROL_BUTTON` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_CHECKBOX` | `WOTBMOD_V3_UI_CONTROL_CHECKBOX` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_SLIDER` | `WOTBMOD_V3_UI_CONTROL_SLIDER` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_DROPDOWN` | `WOTBMOD_V3_UI_CONTROL_DROPDOWN` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_TEXT_INPUT` | `WOTBMOD_V3_UI_CONTROL_TEXT_INPUT` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_SCROLL_VIEW` | `WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_LIST` | `WOTBMOD_V3_UI_CONTROL_LIST` | enum WotbModV3UiControlType |
| `wotb.ui.CONTROL_TABS` | `WOTBMOD_V3_UI_CONTROL_TABS` | enum WotbModV3UiControlType |
| `wotb.ui.LAYOUT_ABSOLUTE` | `WOTBMOD_V3_UI_LAYOUT_ABSOLUTE` | enum WotbModV3UiLayoutType |
| `wotb.ui.LAYOUT_HORIZONTAL` | `WOTBMOD_V3_UI_LAYOUT_HORIZONTAL` | enum WotbModV3UiLayoutType |
| `wotb.ui.LAYOUT_VERTICAL` | `WOTBMOD_V3_UI_LAYOUT_VERTICAL` | enum WotbModV3UiLayoutType |
| `wotb.ui.LAYOUT_GRID` | `WOTBMOD_V3_UI_LAYOUT_GRID` | enum WotbModV3UiLayoutType |
| `wotb.ui.LAYOUT_FLEX` | `WOTBMOD_V3_UI_LAYOUT_FLEX` | enum WotbModV3UiLayoutType |
| `wotb.ui.LAYOUT_OVERLAY` | `WOTBMOD_V3_UI_LAYOUT_OVERLAY` | enum WotbModV3UiLayoutType |
| `wotb.ui.DIRECTION_LEFT_TO_RIGHT` | `WOTBMOD_V3_UI_DIRECTION_LEFT_TO_RIGHT` | enum WotbModV3UiDirection |
| `wotb.ui.DIRECTION_RIGHT_TO_LEFT` | `WOTBMOD_V3_UI_DIRECTION_RIGHT_TO_LEFT` | enum WotbModV3UiDirection |
| `wotb.ui.DIRECTION_TOP_TO_BOTTOM` | `WOTBMOD_V3_UI_DIRECTION_TOP_TO_BOTTOM` | enum WotbModV3UiDirection |
| `wotb.ui.DIRECTION_BOTTOM_TO_TOP` | `WOTBMOD_V3_UI_DIRECTION_BOTTOM_TO_TOP` | enum WotbModV3UiDirection |
| `wotb.ui.LAYOUT_ALIGN_START` | `WOTBMOD_V3_UI_ALIGN_START` | enum WotbModV3UiAlignment |
| `wotb.ui.LAYOUT_ALIGN_CENTER` | `WOTBMOD_V3_UI_ALIGN_CENTER` | enum WotbModV3UiAlignment |
| `wotb.ui.LAYOUT_ALIGN_END` | `WOTBMOD_V3_UI_ALIGN_END` | enum WotbModV3UiAlignment |
| `wotb.ui.LAYOUT_ALIGN_STRETCH` | `WOTBMOD_V3_UI_ALIGN_STRETCH` | enum WotbModV3UiAlignment |
| `wotb.ui.ALIGN_LEFT` | `WOTBMOD_V3_UI_TEXT_ALIGN_LEFT` | enum WotbModV3UiTextAlignment |
| `wotb.ui.ALIGN_CENTER` | `WOTBMOD_V3_UI_TEXT_ALIGN_CENTER` | enum WotbModV3UiTextAlignment |
| `wotb.ui.ALIGN_RIGHT` | `WOTBMOD_V3_UI_TEXT_ALIGN_RIGHT` | enum WotbModV3UiTextAlignment |
| `wotb.ui.ALIGN_JUSTIFY` | `WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY` | enum WotbModV3UiTextAlignment |
| `wotb.ui.EVENT_CLICK` | `WOTBMOD_V3_UI_EVENT_CLICK` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_DOUBLE_CLICK` | `WOTBMOD_V3_UI_EVENT_DOUBLE_CLICK` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_VALUE_CHANGED` | `WOTBMOD_V3_UI_EVENT_VALUE_CHANGED` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_TEXT_CHANGED` | `WOTBMOD_V3_UI_EVENT_TEXT_CHANGED` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_FOCUS_GAINED` | `WOTBMOD_V3_UI_EVENT_FOCUS_GAINED` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_FOCUS_LOST` | `WOTBMOD_V3_UI_EVENT_FOCUS_LOST` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_POINTER_ENTER` | `WOTBMOD_V3_UI_EVENT_POINTER_ENTER` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_POINTER_LEAVE` | `WOTBMOD_V3_UI_EVENT_POINTER_LEAVE` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_POINTER_DOWN` | `WOTBMOD_V3_UI_EVENT_POINTER_DOWN` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_POINTER_UP` | `WOTBMOD_V3_UI_EVENT_POINTER_UP` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_DRAG_START` | `WOTBMOD_V3_UI_EVENT_DRAG_START` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_DRAG` | `WOTBMOD_V3_UI_EVENT_DRAG` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_DRAG_END` | `WOTBMOD_V3_UI_EVENT_DRAG_END` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_SCROLL` | `WOTBMOD_V3_UI_EVENT_SCROLL` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_SUBMIT` | `WOTBMOD_V3_UI_EVENT_SUBMIT` | enum WotbModV3UiEventType |
| `wotb.ui.EVENT_CANCEL` | `WOTBMOD_V3_UI_EVENT_CANCEL` | enum WotbModV3UiEventType |
| `wotb.ui.STYLE_COLOR` | `WOTBMOD_V3_UI_STYLE_COLOR` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_BACKGROUND_COLOR` | `WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_OPACITY` | `WOTBMOD_V3_UI_STYLE_OPACITY` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_FONT` | `WOTBMOD_V3_UI_STYLE_FONT` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_FONT_SIZE` | `WOTBMOD_V3_UI_STYLE_FONT_SIZE` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_TEXTURE` | `WOTBMOD_V3_UI_STYLE_TEXTURE` | enum WotbModV3UiStyleField |
| `wotb.ui.STYLE_Z_ORDER` | `WOTBMOD_V3_UI_STYLE_Z_ORDER` | enum WotbModV3UiStyleField |
| `wotb.ui.SNAPSHOT_VISIBLE` | `WOTBMOD_V3_UI_SNAPSHOT_VISIBLE` | enum WotbModV3UiControlSnapshotFlag |
| `wotb.ui.SNAPSHOT_ENABLED` | `WOTBMOD_V3_UI_SNAPSHOT_ENABLED` | enum WotbModV3UiControlSnapshotFlag |
| `wotb.ui.SNAPSHOT_INTERACTABLE` | `WOTBMOD_V3_UI_SNAPSHOT_INTERACTABLE` | enum WotbModV3UiControlSnapshotFlag |
| `wotb.ui.SNAPSHOT_FOCUSED` | `WOTBMOD_V3_UI_SNAPSHOT_FOCUSED` | enum WotbModV3UiControlSnapshotFlag |
| `wotb.ui.SNAPSHOT_GAME_OWNED` | `WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED` | enum WotbModV3UiControlSnapshotFlag |

## Функции

| Функция | Аргументы | Результат |
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

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"]
local control_create, err = wotb.ui.control_create(descriptor)  -- control: handle
if control_create == nil then wotb.log.warn("ui.control_create: %s", err) end
local control_clone, err = wotb.ui.control_clone(handle)  -- clone: handle
if control_clone == nil then wotb.log.warn("ui.control_clone: %s", err) end
local control_destroy_ok, err = wotb.ui.control_destroy(handle)
if not control_destroy_ok then wotb.log.warn("ui.control_destroy: %s", err) end
```

