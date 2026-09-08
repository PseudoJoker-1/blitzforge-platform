#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_UI_VERSION 2u

typedef enum WotbModV3UiControlType {
    WOTBMOD_V3_UI_CONTROL_CONTAINER = 0,
    WOTBMOD_V3_UI_CONTROL_TEXT = 1,
    WOTBMOD_V3_UI_CONTROL_IMAGE = 2,
    WOTBMOD_V3_UI_CONTROL_BUTTON = 3,
    WOTBMOD_V3_UI_CONTROL_CHECKBOX = 4,
    WOTBMOD_V3_UI_CONTROL_SLIDER = 5,
    WOTBMOD_V3_UI_CONTROL_DROPDOWN = 6,
    WOTBMOD_V3_UI_CONTROL_TEXT_INPUT = 7,
    WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW = 8,
    WOTBMOD_V3_UI_CONTROL_LIST = 9,
    WOTBMOD_V3_UI_CONTROL_TABS = 10
} WotbModV3UiControlType;

typedef enum WotbModV3UiLayoutType {
    WOTBMOD_V3_UI_LAYOUT_ABSOLUTE = 0,
    WOTBMOD_V3_UI_LAYOUT_HORIZONTAL = 1,
    WOTBMOD_V3_UI_LAYOUT_VERTICAL = 2,
    WOTBMOD_V3_UI_LAYOUT_GRID = 3,
    WOTBMOD_V3_UI_LAYOUT_FLEX = 4,
    WOTBMOD_V3_UI_LAYOUT_OVERLAY = 5
} WotbModV3UiLayoutType;

typedef enum WotbModV3UiDirection {
    WOTBMOD_V3_UI_DIRECTION_LEFT_TO_RIGHT = 0,
    WOTBMOD_V3_UI_DIRECTION_RIGHT_TO_LEFT = 1,
    WOTBMOD_V3_UI_DIRECTION_TOP_TO_BOTTOM = 2,
    WOTBMOD_V3_UI_DIRECTION_BOTTOM_TO_TOP = 3
} WotbModV3UiDirection;

typedef enum WotbModV3UiAlignment {
    WOTBMOD_V3_UI_ALIGN_START = 0,
    WOTBMOD_V3_UI_ALIGN_CENTER = 1,
    WOTBMOD_V3_UI_ALIGN_END = 2,
    WOTBMOD_V3_UI_ALIGN_STRETCH = 3
} WotbModV3UiAlignment;

typedef enum WotbModV3UiTextAlignment {
    WOTBMOD_V3_UI_TEXT_ALIGN_LEFT = 0,
    WOTBMOD_V3_UI_TEXT_ALIGN_CENTER = 1,
    WOTBMOD_V3_UI_TEXT_ALIGN_RIGHT = 2,
    WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY = 3
} WotbModV3UiTextAlignment;

typedef enum WotbModV3UiEventType {
    WOTBMOD_V3_UI_EVENT_CLICK = 1,
    WOTBMOD_V3_UI_EVENT_DOUBLE_CLICK = 2,
    WOTBMOD_V3_UI_EVENT_VALUE_CHANGED = 3,
    WOTBMOD_V3_UI_EVENT_TEXT_CHANGED = 4,
    WOTBMOD_V3_UI_EVENT_FOCUS_GAINED = 5,
    WOTBMOD_V3_UI_EVENT_FOCUS_LOST = 6,
    WOTBMOD_V3_UI_EVENT_POINTER_ENTER = 7,
    WOTBMOD_V3_UI_EVENT_POINTER_LEAVE = 8,
    WOTBMOD_V3_UI_EVENT_POINTER_DOWN = 9,
    WOTBMOD_V3_UI_EVENT_POINTER_UP = 10,
    WOTBMOD_V3_UI_EVENT_DRAG_START = 11,
    WOTBMOD_V3_UI_EVENT_DRAG = 12,
    WOTBMOD_V3_UI_EVENT_DRAG_END = 13,
    WOTBMOD_V3_UI_EVENT_SCROLL = 14,
    WOTBMOD_V3_UI_EVENT_SUBMIT = 15,
    WOTBMOD_V3_UI_EVENT_CANCEL = 16
} WotbModV3UiEventType;

typedef enum WotbModV3UiStyleField {
    WOTBMOD_V3_UI_STYLE_COLOR = 1u << 0,
    WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR = 1u << 1,
    WOTBMOD_V3_UI_STYLE_OPACITY = 1u << 2,
    WOTBMOD_V3_UI_STYLE_FONT = 1u << 3,
    WOTBMOD_V3_UI_STYLE_FONT_SIZE = 1u << 4,
    WOTBMOD_V3_UI_STYLE_TEXTURE = 1u << 5,
    WOTBMOD_V3_UI_STYLE_Z_ORDER = 1u << 6
} WotbModV3UiStyleField;

typedef struct WotbModV3UiEdges {
    float left;
    float top;
    float right;
    float bottom;
} WotbModV3UiEdges;

typedef struct WotbModV3UiControlDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t visible;
    const char* id;
    const char* text;
    const char* texture_uri;
    WotbModV3Rect geometry;
} WotbModV3UiControlDescriptor;

typedef struct WotbModV3UiLayoutDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t direction;
    uint32_t main_alignment;
    uint32_t cross_alignment;
    float spacing;
    float weight;
    uint32_t columns;
    uint32_t reserved;
} WotbModV3UiLayoutDescriptor;

typedef struct WotbModV3UiStylePatch {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t fields;
    int32_t priority;
    WotbModV3Color color;
    WotbModV3Color background_color;
    float opacity;
    float font_size;
    int32_t z_order;
    const char* font_uri;
    const char* texture_uri;
} WotbModV3UiStylePatch;

typedef struct WotbModV3UiEvent {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t modifiers;
    WotbModV3UiHandle control;
    WotbModV3Vec2 pointer;
    WotbModV3Vec2 delta;
    double number_value;
    const char* text_value;
} WotbModV3UiEvent;

typedef struct WotbModV3UiChoiceDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* id;
    const char* label;
    const char* value;
} WotbModV3UiChoiceDescriptor;

typedef struct WotbModV3UiDialogDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* title;
    const char* message;
    const char* accept_label;
    const char* cancel_label;
    uint32_t modal;
    uint32_t reserved;
} WotbModV3UiDialogDescriptor;

typedef void(WOTBMOD_V3_CALL* WotbModV3UiEventCallback)(
    WotbModV3Handle mod,
    const WotbModV3UiEvent* event,
    void* user_data);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3UiSlotVisitor)(
    WotbModV3Handle mod,
    const char* slot_id,
    void* user_data);

typedef struct WotbModV3UiApiV2 {
    uint32_t struct_size;
    uint32_t api_version;

    WotbModV3Result(WOTBMOD_V3_CALL* control_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* control_clone)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiHandle* out_clone);
    WotbModV3Result(WOTBMOD_V3_CALL* control_destroy)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control);
    WotbModV3Result(WOTBMOD_V3_CALL* control_add_child)(
        WotbModV3Handle mod,
        WotbModV3UiHandle parent,
        WotbModV3UiHandle child);
    WotbModV3Result(WOTBMOD_V3_CALL* control_remove_child)(
        WotbModV3Handle mod,
        WotbModV3UiHandle parent,
        WotbModV3UiHandle child);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_parent)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiHandle parent);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_parent)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiHandle* out_parent);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_child_count)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_child_at)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t index,
        WotbModV3UiHandle* out_child);

    WotbModV3Result(WOTBMOD_V3_CALL* control_set_id)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* id);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_id)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_find_by_id)(
        WotbModV3Handle mod,
        WotbModV3UiHandle root,
        const char* id,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* control_find_by_path)(
        WotbModV3Handle mod,
        WotbModV3UiHandle root,
        const char* path,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_owner_mod)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Handle* out_owner);
    WotbModV3Result(WOTBMOD_V3_CALL* control_is_alive)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t* out_alive);

    /*
     * Semantic slots are available only when the native adapter resolves the
     * exact named extension point. Implementations must not substitute a
     * generic screen root. When unavailable, slot_find returns
     * WOTBMOD_V3_E_NOT_SUPPORTED and writes WOTBMOD_V3_INVALID_HANDLE.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* slot_find)(
        WotbModV3Handle mod,
        const char* slot_id,
        WotbModV3UiHandle* out_slot);
    WotbModV3Result(WOTBMOD_V3_CALL* slot_attach)(
        WotbModV3Handle mod,
        WotbModV3UiHandle slot,
        WotbModV3UiHandle control,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* slot_detach)(
        WotbModV3Handle mod,
        WotbModV3UiHandle slot,
        WotbModV3UiHandle control);
    WotbModV3Result(WOTBMOD_V3_CALL* slot_enumerate)(
        WotbModV3Handle mod,
        WotbModV3UiSlotVisitor visitor,
        void* user_data);

    WotbModV3Result(WOTBMOD_V3_CALL* control_set_position)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 position);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_position)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2* out_position);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_size)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_size)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2* out_size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_anchor)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 anchor);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_pivot)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 pivot);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_margin)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiEdges margin);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_padding)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiEdges padding);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_min_size)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_max_size)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Vec2 size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_z_order)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        int32_t z_order);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const WotbModV3UiLayoutDescriptor* layout);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set_type)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t type);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set_direction)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t direction);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set_spacing)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        float spacing);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set_alignment)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t main_alignment,
        uint32_t cross_alignment);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_set_weight)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        float weight);
    WotbModV3Result(WOTBMOD_V3_CALL* layout_invalidate)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control);
    WotbModV3Result(WOTBMOD_V3_CALL* get_scale_factor)(
        WotbModV3Handle mod,
        float* out_scale);
    WotbModV3Result(WOTBMOD_V3_CALL* get_safe_area)(
        WotbModV3Handle mod,
        WotbModV3Rect* out_safe_area);
    WotbModV3Result(WOTBMOD_V3_CALL* get_viewport_size)(
        WotbModV3Handle mod,
        WotbModV3Vec2* out_size);

    WotbModV3Result(WOTBMOD_V3_CALL* control_set_text)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* text);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_texture)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_color)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3Color color);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_opacity)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        float opacity);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_visible)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t visible);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_font)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* font_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_font_size)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        float size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_text_alignment)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t alignment);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_text_wrap)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_rich_text)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_localization_key)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* key);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_tooltip)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* tooltip);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_accessibility_label)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const char* label);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_enabled)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_interactable)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t interactable);
    WotbModV3Result(WOTBMOD_V3_CALL* control_set_focus)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t focused);

    WotbModV3Result(WOTBMOD_V3_CALL* event_subscribe)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        uint32_t event_type,
        WotbModV3UiEventCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* event_unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3Token token);

    WotbModV3Result(WOTBMOD_V3_CALL* button_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* checkbox_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        uint32_t checked,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* slider_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        double minimum,
        double maximum,
        double value,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* dropdown_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        const WotbModV3UiChoiceDescriptor* choices,
        uint32_t choice_count,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* text_input_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* scroll_view_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* list_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* tabs_create)(
        WotbModV3Handle mod,
        const WotbModV3UiControlDescriptor* descriptor,
        WotbModV3UiHandle* out_control);
    WotbModV3Result(WOTBMOD_V3_CALL* dialog_show)(
        WotbModV3Handle mod,
        const WotbModV3UiDialogDescriptor* descriptor,
        WotbModV3UiHandle* out_dialog);
    WotbModV3Result(WOTBMOD_V3_CALL* confirm_show)(
        WotbModV3Handle mod,
        const WotbModV3UiDialogDescriptor* descriptor,
        WotbModV3UiHandle* out_dialog);
    WotbModV3Result(WOTBMOD_V3_CALL* toast_show)(
        WotbModV3Handle mod,
        const char* message,
        float duration_seconds);

    WotbModV3Result(WOTBMOD_V3_CALL* style_push)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        const WotbModV3UiStylePatch* patch,
        WotbModV3Handle* out_override);
    WotbModV3Result(WOTBMOD_V3_CALL* style_update)(
        WotbModV3Handle mod,
        WotbModV3Handle override_handle,
        const WotbModV3UiStylePatch* patch);
    WotbModV3Result(WOTBMOD_V3_CALL* style_pop)(
        WotbModV3Handle mod,
        WotbModV3Handle override_handle);
} WotbModV3UiApiV2;

#ifdef __cplusplus
}
#endif
