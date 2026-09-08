local ROOT_WIDTH = 1920
local ROOT_HEIGHT = 1080
local WINDOW_WIDTH = 1000
local WINDOW_HEIGHT = 760
local SLIDER_WIDTH = 420
local KNOB_WIDTH = 26
local SAFE_LEFT = 128
local SAFE_RIGHT = 24
local SAFE_TOP = 52
local SAFE_BOTTOM = 72
local VISIBLE_CONTEXTS = wotb.context.BATTLE +
    wotb.context.TRAINING + wotb.context.REPLAY
local BLOCKED_CONTEXTS = wotb.context.MOD_SCREEN +
    wotb.context.TEXT_INPUT

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.70, g = 0.76, b = 0.82, a = 1.00},
    panel = {r = 0.035, g = 0.055, b = 0.075, a = 0.96},
    panel_alt = {r = 0.075, g = 0.105, b = 0.135, a = 0.98},
    track = {r = 0.12, g = 0.16, b = 0.20, a = 1.00},
    accent = {r = 0.95, g = 0.64, b = 0.08, a = 1.00},
    green = {r = 0.18, g = 0.70, b = 0.36, a = 1.00},
    red = {r = 0.76, g = 0.19, b = 0.16, a = 1.00},
    blue = {r = 0.12, g = 0.42, b = 0.70, a = 1.00},
}

local PLAYER_NAMES = {
    "Lumedas_", "SteelFox", "SteppeWind", "DavaPilot",
    "LuaBuilder", "BlitzForge", "NightScout", "RuntimeUI",
}

local screen = nil
local root = nil
local controls = {}
local control_order = {}
local subscriptions = {}
local style_overrides = {}
local system_subscriptions = {}
local mounted = false
local screen_rebind_pending = false
local retry_at_frame = 0
local context_mask = wotb.context.NONE
local context_dirty = true
local context_allowed = false
local window_open = false
local toggle_enabled = true
local volume = 0.35
local scale = 0.70
local viewport_width = ROOT_WIDTH
local viewport_height = ROOT_HEIGHT
local window_x = 0
local window_y = 0

local function log_error(context, err)
    print("Lua UI " .. context .. " failed: " .. tostring(err))
end

local function set_cursor_unlocked(unlocked)
    if type(wotb.input) ~= "table" or
            type(wotb.input.set_cursor_unlocked) ~= "function" then
        return false
    end
    local ok, err = wotb.input.set_cursor_unlocked(unlocked)
    if not ok and (type(err) ~= "string" or
            not string.find(err, "permission denied", 1, true)) then
        log_error("cursor state", err)
    end
    return ok == true
end

local function remember(name, control)
    controls[name] = control
    control_order[#control_order + 1] = control
    return control
end

local function create(name, spec, parent)
    spec.id = name
    if parent ~= nil then
        spec.parent = parent
    end
    local control, err = wotb.ui.create(spec)
    if control == nil then
        log_error("create " .. name, err)
        return nil
    end
    return remember(name, control)
end

local function set_text(control, text)
    local ok, err = control:set_text(text)
    if not ok then
        log_error("set_text", err)
    end
    return ok
end

local function set_visible(control, visible)
    local ok, err = control:set_visible(visible)
    if not ok then
        log_error("set_visible", err)
    end
    return ok
end

local function subscribe(control, event_type, callback)
    local token, err = control:on(event_type, callback)
    if token == nil then
        log_error("event subscription", err)
        return false
    end
    subscriptions[#subscriptions + 1] = token
    return true
end

local function push_style(control, patch)
    local style, err = control:push_style(patch)
    if style == nil then
        log_error("style push", err)
        return nil
    end
    style_overrides[#style_overrides + 1] = style
    return style
end

local function read_viewport_size()
    local size = wotb.ui.get_viewport_size()
    if type(size) == "table" and type(size.x) == "number" and
            type(size.y) == "number" and size.x > 0 and size.y > 0 then
        return size.x, size.y
    end
    return ROOT_WIDTH, ROOT_HEIGHT
end

local function make_label(name, parent, text, x, y, width, height, size, color)
    return create(name, {
        type = wotb.ui.CONTROL_TEXT,
        text = text,
        x = x,
        y = y,
        width = width,
        height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = size,
        color = color or COLORS.white,
        alignment = wotb.ui.ALIGN_LEFT,
        visible = true,
        interactable = false,
    }, parent)
end

local function make_button(name, parent, text, x, y, width, height, background)
    return create(name, {
        type = wotb.ui.CONTROL_BUTTON,
        text = text,
        x = x,
        y = y,
        width = width,
        height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = 22,
        color = COLORS.white,
        background_color = background,
        alignment = wotb.ui.ALIGN_CENTER,
        visible = true,
        enabled = true,
        interactable = true,
    }, parent)
end

local function layout_root()
    if root == nil then
        return
    end
    root:set_size(viewport_width, viewport_height)
    local centered_x = (viewport_width - WINDOW_WIDTH) * 0.5
    local centered_y = (viewport_height - WINDOW_HEIGHT) * 0.5
    local rightmost_x = math.max(
        SAFE_LEFT, viewport_width - WINDOW_WIDTH - SAFE_RIGHT)
    local bottommost_y = math.max(
        SAFE_TOP, viewport_height - WINDOW_HEIGHT - SAFE_BOTTOM)
    window_x = math.max(SAFE_LEFT, math.min(centered_x, rightmost_x))
    window_y = math.max(SAFE_TOP, math.min(centered_y, bottommost_y))
    controls.Window:set_position(window_x, window_y)
    controls.OpenButton:set_position(
        math.max(20, viewport_width - 330),
        math.max(100, math.min(viewport_height - 90, viewport_height * 0.42)))
end

local function update_slider(prefix, value)
    local fill = controls[prefix .. "Fill"]
    local knob = controls[prefix .. "Knob"]
    local label = controls[prefix .. "Label"]
    local y = prefix == "Volume" and 188 or 288
    fill:set_size(math.max(1, SLIDER_WIDTH * value), 16)
    knob:set_position(60 + SLIDER_WIDTH * value - KNOB_WIDTH * 0.5, y - 5)
    local caption = prefix == "Volume" and "Громкость" or "Масштаб интерфейса"
    set_text(label, string.format("%s: %d%%", caption, math.floor(value * 100 + 0.5)))
end

local function set_slider_from_pointer(prefix, event)
    if type(event) ~= "table" or type(event.pointer) ~= "table" or
            type(event.pointer.x) ~= "number" then
        return
    end
    local value = math.max(0, math.min(1,
        (event.pointer.x - window_x - 60) / SLIDER_WIDTH))
    if prefix == "Volume" then
        volume = value
    else
        scale = value
    end
    update_slider(prefix, value)
end

local function update_toggle()
    local text = toggle_enabled and "ВКЛЮЧЕНО" or "ВЫКЛЮЧЕНО"
    set_text(controls.ToggleButton, text)
    local style = controls.ToggleStyle
    if style ~= nil then
        local ok, err = style:update({
            fields = wotb.ui.STYLE_BACKGROUND_COLOR,
            background_color = toggle_enabled and COLORS.green or COLORS.red,
        })
        if not ok then
            log_error("toggle style update", err)
        end
    end
end

local function set_window_visible(visible)
    window_open = visible
    set_visible(controls.Window, visible)
    set_visible(controls.OpenButton, not visible)
    set_cursor_unlocked(context_allowed and visible)
end

local function read_context()
    local value, err = wotb.context.current()
    if value == nil then
        log_error("context lookup", err)
        return nil
    end
    return value
end

local function refresh_context(force_read)
    if force_read then
        local value = read_context()
        if value ~= nil then
            context_mask = value
        end
    end
    local visible, err = wotb.context.should_show(
        VISIBLE_CONTEXTS, BLOCKED_CONTEXTS, context_mask)
    if visible == nil then
        log_error("context visibility", err)
        visible = false
    end
    context_allowed = visible
    context_dirty = false
    return visible
end

local function populate_player_list(list)
    local template = make_button(
        "PlayerRowTemplate", list, "", 0, 0, 300, 38, COLORS.panel_alt)
    if template == nil then
        return false
    end
    set_visible(template, false)

    for index, player_name in ipairs(PLAYER_NAMES) do
        local row, err = template:clone()
        if row == nil then
            log_error("clone player row", err)
            return false
        end
        remember("PlayerRow" .. index, row)
        local ok
        ok, err = row:set_id("PlayerRow" .. index)
        if not ok then
            log_error("set cloned row id", err)
            return false
        end
        row:set_parent(list)
        row:set_position(0, (index - 1) * 44)
        row:set_size(300, 38)
        set_text(row, string.format("%02d  %s", index, player_name))
        set_visible(row, true)
        subscribe(row, wotb.ui.EVENT_CLICK, function()
            set_text(controls.SelectedPlayer, "Выбран игрок: " .. player_name)
        end)
    end
    return true
end

local function create_slider(prefix, parent, label_y, track_y, initial_value)
    if make_label(prefix .. "Label", parent, "", 60, label_y,
            460, 38, 24, COLORS.white) == nil then
        return false
    end
    if create(prefix .. "Track", {
            type = wotb.ui.CONTROL_BUTTON,
            text = "",
            x = 60,
            y = track_y,
            width = SLIDER_WIDTH,
            height = 16,
            background_color = COLORS.track,
            visible = true,
            interactable = true,
        }, parent) == nil then
        return false
    end
    if create(prefix .. "Fill", {
            type = wotb.ui.CONTROL_IMAGE,
            x = 60,
            y = track_y,
            width = SLIDER_WIDTH * initial_value,
            height = 16,
            background_color = COLORS.accent,
            visible = true,
            interactable = false,
        }, parent) == nil then
        return false
    end
    if create(prefix .. "Knob", {
            type = wotb.ui.CONTROL_IMAGE,
            x = 60,
            y = track_y - 5,
            width = KNOB_WIDTH,
            height = KNOB_WIDTH,
            texture = "~res:/Gfx/UI/CommonUIElements/Selection_back_0.psd",
            color = COLORS.white,
            visible = true,
            interactable = false,
        }, parent) == nil then
        return false
    end

    local function on_pointer(event)
        set_slider_from_pointer(prefix, event)
    end
    subscribe(controls[prefix .. "Track"], wotb.ui.EVENT_POINTER_DOWN, on_pointer)
    subscribe(controls[prefix .. "Track"], wotb.ui.EVENT_DRAG, on_pointer)
    update_slider(prefix, initial_value)
    return true
end

local function build_interface()
    root = create("RuntimeRoot", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0,
        y = 0,
        width = viewport_width,
        height = viewport_height,
        visible = true,
        interactable = false,
    }, screen)
    if root == nil then
        return false
    end

    if make_button("OpenButton", root, "F8 · LUA UI LAB", 0, 0,
            290, 62, COLORS.blue) == nil then
        return false
    end
    local window = create("Window", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0,
        y = 0,
        width = WINDOW_WIDTH,
        height = WINDOW_HEIGHT,
        background_color = COLORS.panel,
        visible = false,
        interactable = false,
    }, root)
    if window == nil then
        return false
    end

    make_label("Title", window, "LUA RUNTIME UI LAB", 42, 24,
        760, 50, 34, COLORS.accent)
    make_label("Description", window,
        "Lua создаёт DAVA controls, меняет свойства и клонирует строки без YAML.",
        42, 74, 850, 42, 20, COLORS.muted)
    make_button("CloseButton", window, "X", 920, 24, 48, 48, COLORS.red)

    create_slider("Volume", window, 132, 188, volume)
    create_slider("Scale", window, 232, 288, scale)

    make_label("WidgetsTitle", window, "Кнопки, ввод и runtime-стили",
        42, 342, 500, 36, 24, COLORS.accent)
    make_button("ToggleButton", window, "", 42, 388, 210, 54, COLORS.green)
    controls.ToggleStyle = push_style(controls.ToggleButton, {
        fields = wotb.ui.STYLE_BACKGROUND_COLOR,
        background_color = COLORS.green,
    })
    make_button("ActionButton", window, "ИЗМЕНИТЬ ТЕКСТ", 270, 388,
        250, 54, COLORS.blue)
    controls.ActionStyle = push_style(controls.ActionButton, {
        fields = wotb.ui.STYLE_BACKGROUND_COLOR + wotb.ui.STYLE_OPACITY,
        background_color = COLORS.blue,
        opacity = 1.0,
    })
    create("TextInput", {
        type = wotb.ui.CONTROL_TEXT_INPUT,
        text = "Введите текст...",
        x = 42,
        y = 462,
        width = 478,
        height = 52,
        font = "~res:/Fonts/WarHeliosCondC.ttf",
        font_size = 22,
        color = COLORS.white,
        background_color = COLORS.panel_alt,
        visible = true,
        interactable = true,
    }, window)
    create("DemoImage", {
        type = wotb.ui.CONTROL_IMAGE,
        texture = "~res:/Gfx/Lobby/icons/icon_tick_l",
        x = 42,
        y = 548,
        width = 74,
        height = 74,
        color = COLORS.green,
        opacity = 1.0,
        visible = true,
        interactable = false,
    }, window)
    make_label("RuntimeStatus", window, "Текст ещё не изменён",
        132, 554, 388, 40, 23, COLORS.white)
    make_label("AnimationStatus", window, "Анимация: opacity + position",
        132, 598, 388, 34, 18, COLORS.muted)

    make_label("ListTitle", window, "Динамический список игроков",
        570, 132, 360, 38, 24, COLORS.accent)
    local list = create("PlayerList", {
        type = wotb.ui.CONTROL_LIST,
        x = 570,
        y = 180,
        width = 330,
        height = 360,
        background_color = COLORS.track,
        visible = true,
        interactable = true,
    }, window)
    if list == nil or not populate_player_list(list) then
        return false
    end
    make_label("SelectedPlayer", window, "Выберите строку списка",
        570, 560, 360, 42, 21, COLORS.white)
    make_button("ResetButton", window, "СБРОСИТЬ ЗНАЧЕНИЯ",
        570, 620, 330, 48, COLORS.panel_alt)
    make_label("Footer", window,
        "F8: окно + мышь · UIStaticText · UIButton · list · clone · events",
        42, 690, 900, 34, 18, COLORS.muted)

    subscribe(controls.OpenButton, wotb.ui.EVENT_CLICK, function()
        set_window_visible(true)
    end)
    subscribe(controls.CloseButton, wotb.ui.EVENT_CLICK, function()
        set_window_visible(false)
    end)
    subscribe(controls.ToggleButton, wotb.ui.EVENT_CLICK, function()
        toggle_enabled = not toggle_enabled
        update_toggle()
    end)
    subscribe(controls.ActionButton, wotb.ui.EVENT_CLICK, function()
        set_text(controls.RuntimeStatus,
            string.format("Громкость: %d%% · Масштаб: %d%%",
                math.floor(volume * 100 + 0.5),
                math.floor(scale * 100 + 0.5)))
        controls.RuntimeStatus:set_color(COLORS.accent)
        controls.ActionStyle:update({
            fields = wotb.ui.STYLE_BACKGROUND_COLOR + wotb.ui.STYLE_OPACITY,
            background_color = COLORS.accent,
            opacity = 0.92,
        })
    end)
    subscribe(controls.ResetButton, wotb.ui.EVENT_CLICK, function()
        volume = 0.35
        scale = 0.70
        toggle_enabled = true
        update_slider("Volume", volume)
        update_slider("Scale", scale)
        update_toggle()
        set_text(controls.RuntimeStatus, "Значения сброшены из Lua")
        controls.RuntimeStatus:set_color(COLORS.white)
        controls.ActionStyle:update({
            fields = wotb.ui.STYLE_BACKGROUND_COLOR + wotb.ui.STYLE_OPACITY,
            background_color = COLORS.blue,
            opacity = 1.0,
        })
    end)

    update_toggle()
    layout_root()
    set_window_visible(false)
    return true
end

local function unsubscribe_all()
    for index = #subscriptions, 1, -1 do
        local ok, err = wotb.ui.event_unsubscribe(subscriptions[index])
        if not ok then
            log_error("event cleanup", err)
        end
    end
    subscriptions = {}
end

local function pop_styles()
    for index = #style_overrides, 1, -1 do
        local ok, err = style_overrides[index]:pop()
        if not ok then
            log_error("style cleanup", err)
        end
    end
    style_overrides = {}
end

local function destroy_controls()
    for index = #control_order, 1, -1 do
        local ok, err = control_order[index]:destroy()
        if not ok then
            log_error("control cleanup", err)
        end
    end
    controls = {}
    control_order = {}
    root = nil
end

local function unmount()
    set_cursor_unlocked(false)
    unsubscribe_all()
    pop_styles()
    destroy_controls()
    if screen ~= nil and type(wotb.handles) == "table" and
            type(wotb.handles.release) == "function" then
        local ok, err = wotb.handles.release(screen)
        if not ok then
            log_error("screen release", err)
        end
    end
    screen = nil
    mounted = false
    window_open = false
end

local function release_screen(handle)
    if handle == nil or type(wotb.handles) ~= "table" or
            type(wotb.handles.release) ~= "function" then
        return
    end
    local ok, err = wotb.handles.release(handle)
    if not ok then log_error("screen release", err) end
end

local function rebind_active_screen()
    if not mounted or root == nil then
        screen_rebind_pending = false
        return false
    end
    local next_screen, lookup_error = wotb.ui.get_active_screen()
    if next_screen == nil then
        log_error("replacement screen lookup", lookup_error)
        return false
    end
    local ok, parent_error = root:set_parent(next_screen)
    if not ok then
        release_screen(next_screen)
        log_error("replacement screen attach", parent_error)
        return false
    end
    local previous_screen = screen
    screen = next_screen
    screen_rebind_pending = false
    release_screen(previous_screen)
    return true
end

local function mount()
    if mounted then
        return true
    end
    local err
    screen, err = wotb.ui.get_active_screen()
    if screen == nil then
        log_error("active screen lookup", err)
        return false
    end
    viewport_width, viewport_height = read_viewport_size()
    if not build_interface() then
        unmount()
        return false
    end
    mounted = true
    print("Lua Runtime UI Lab mounted without an author YAML template")
    return true
end

local function subscribe_screen_changes()
    local topics = {
        wotb.events.TOPIC_UI_SCREEN_CHANGED,
        wotb.events.TOPIC_BATTLE_ENTERED,
        wotb.events.TOPIC_BATTLE_STARTED,
        wotb.events.TOPIC_BATTLE_ENDED,
        wotb.events.TOPIC_BATTLE_LEFT,
    }
    for _, topic in ipairs(topics) do
        local subscribed_topic = topic
        local token, err = wotb.events.subscribe(
            subscribed_topic,
            function(event)
                if type(event) == "table" and
                        type(event.context_mask) == "number" then
                    context_mask = event.context_mask
                end
                context_dirty = true
                if subscribed_topic == wotb.events.TOPIC_UI_SCREEN_CHANGED then
                    screen_rebind_pending = true
                end
            end,
            wotb.events.PRIORITY_NORMAL,
            true)
        if token == nil then
            log_error(
                "system event subscription " .. tostring(subscribed_topic),
                err)
        else
            system_subscriptions[#system_subscriptions + 1] = token
        end
    end
end

local function unsubscribe_screen_changes()
    for index = #system_subscriptions, 1, -1 do
        local ok, err = wotb.events.unsubscribe(system_subscriptions[index])
        if not ok then
            log_error("system event cleanup", err)
        end
    end
    system_subscriptions = {}
end

local function ensure_mount(frame_index)
    if not context_allowed then
        if mounted then
            unmount()
        end
        return false
    end
    if mounted and (screen_rebind_pending or frame_index % 30 == 0) then
        rebind_active_screen()
    end
    if not mounted and frame_index >= retry_at_frame then
        retry_at_frame = frame_index + 60
        mount()
    end
    return mounted
end

-- Kept as a named helper in the example so other mods can copy the policy:
-- battle/training/replay only, and never while the catalog or text input owns
-- the screen.
local function refresh_visibility(frame_index, force_read)
    refresh_context(force_read)
    return ensure_mount(frame_index)
end

function on_enable()
    retry_at_frame = 0
    screen_rebind_pending = false
    unsubscribe_screen_changes()
    unmount()
    subscribe_screen_changes()
    refresh_visibility(0, true)
end

function on_frame(frame_index, delta_seconds)
    if context_dirty or frame_index % 15 == 0 then
        if not refresh_visibility(frame_index, true) then
            return
        end
    elseif not ensure_mount(frame_index) then
        return
    end

    if type(wotb.input) == "table" and
            type(wotb.input.hotkey_pressed) == "function" then
        local pressed, hotkey_error = wotb.input.hotkey_pressed(
            wotb.input.KEY_F8 or 0x77)
        if pressed == true then
            set_window_visible(not window_open)
            local unlocked = nil
            if type(wotb.input.cursor_unlocked) == "function" then
                unlocked = wotb.input.cursor_unlocked()
            end
            print("Lua Runtime UI Lab F8 window=" ..
                tostring(window_open) .. " cursor_unlocked=" ..
                tostring(unlocked))
        elseif pressed == nil and (type(hotkey_error) ~= "string" or
                not string.find(hotkey_error, "permission denied", 1, true)) then
            log_error("F8 hotkey", hotkey_error)
        end
    end
    if window_open then set_cursor_unlocked(true) end

    if frame_index % 60 == 0 then
        local width, height = read_viewport_size()
        if width ~= viewport_width or height ~= viewport_height then
            viewport_width, viewport_height = width, height
            layout_root()
        end
        if wotb.ui.control_get_snapshot(root) == nil then
            unmount()
            retry_at_frame = frame_index + 1
            return
        end
    end

    if window_open and frame_index % 12 == 0 then
        local phase = frame_index * math.max(delta_seconds, 1 / 60) * 0.8
        controls.DemoImage:set_position(42, 548 + math.sin(phase) * 8)
        if frame_index % 60 == 0 then
            local opacity = 0.66 + 0.34 * (0.5 + 0.5 * math.sin(phase))
            controls.AnimationStatus:set_opacity(opacity)
        end
    end
end

function on_disable()
    screen_rebind_pending = false
    context_allowed = false
    context_dirty = true
    context_mask = wotb.context.NONE
    unsubscribe_screen_changes()
    unmount()
    print("Lua Runtime UI Lab disabled")
end
