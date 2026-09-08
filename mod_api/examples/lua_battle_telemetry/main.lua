local MAX_LINES = 8
local PANEL_WIDTH = 590
local PANEL_HEIGHT = 390
local SAFE_LEFT = 128
local SAFE_TOP = 118
local VISIBLE_CONTEXTS = wotb.context.BATTLE +
    wotb.context.TRAINING + wotb.context.REPLAY
local BLOCKED_CONTEXTS = wotb.context.MOD_SCREEN +
    wotb.context.TEXT_INPUT

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.68, g = 0.75, b = 0.82, a = 1.00},
    panel = {r = 0.025, g = 0.040, b = 0.058, a = 0.91},
    row = {r = 0.065, g = 0.095, b = 0.125, a = 0.95},
    blue = {r = 0.16, g = 0.52, b = 0.84, a = 1.00},
    red = {r = 0.82, g = 0.22, b = 0.18, a = 1.00},
    amber = {r = 0.96, g = 0.66, b = 0.14, a = 1.00},
}

local screen = nil
local root = nil
local controls = {}
local control_order = {}
local ui_subscriptions = {}
local event_subscriptions = {}
local lines = {}
local mounted = false
local screen_rebind_pending = false
local context_dirty = true
local context_allowed = false
local context_mask = wotb.context.NONE
local data_dirty = true
local paused = false
local total_events = 0
local elapsed_seconds = 0
local viewport_width = 1920
local viewport_height = 1080

local function log_error(scope, err)
    print("Lua Battle Telemetry " .. scope .. ": " .. tostring(err))
end

local function remember(name, control)
    controls[name] = control
    control_order[#control_order + 1] = control
    return control
end

local function create(name, spec, parent)
    spec.id = name
    if parent ~= nil then spec.parent = parent end
    local control, err = wotb.ui.create(spec)
    if control == nil then
        log_error("create " .. name, err)
        return nil
    end
    return remember(name, control)
end

local function text_control(name, parent, text, x, y, width, height, size, color)
    return create(name, {
        type = wotb.ui.CONTROL_TEXT,
        text = text,
        x = x, y = y, width = width, height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = size,
        color = color or COLORS.white,
        alignment = wotb.ui.ALIGN_LEFT,
        visible = true,
        interactable = false,
    }, parent)
end

local function button(name, parent, text, x, y, width, color)
    return create(name, {
        type = wotb.ui.CONTROL_BUTTON,
        text = text,
        x = x, y = y, width = width, height = 38,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = 17,
        color = COLORS.white,
        background_color = color,
        alignment = wotb.ui.ALIGN_CENTER,
        visible = true,
        enabled = true,
        interactable = true,
    }, parent)
end

local function set_text(control, value)
    local ok, err = control:set_text(value)
    if not ok then log_error("set_text", err) end
end

local function subscribe_ui(control, callback)
    local token, err = control:on(wotb.ui.EVENT_CLICK, callback)
    if token == nil then
        log_error("UI subscription", err)
        return
    end
    ui_subscriptions[#ui_subscriptions + 1] = token
end

local function read_viewport()
    local size = wotb.ui.get_viewport_size()
    if type(size) == "table" and type(size.x) == "number" and
            type(size.y) == "number" and size.x > 0 and size.y > 0 then
        return size.x, size.y
    end
    return 1920, 1080
end

local function layout()
    if root == nil then return end
    root:set_size(viewport_width, viewport_height)
    controls.Panel:set_position(SAFE_LEFT, SAFE_TOP)
end

local function push_line(message)
    if paused then return end
    total_events = total_events + 1
    table.insert(lines, 1,
        string.format("[%05.1f]  %s", elapsed_seconds, message))
    while #lines > MAX_LINES do table.remove(lines) end
    data_dirty = true
end

local function number(value, fallback)
    if type(value) == "number" then return value end
    return fallback or 0
end

local function format_event(event)
    local topic = event.topic or "event"
    local data = event.data
    if type(data) ~= "table" then
        return topic .. " · payload не типизирован"
    end

    if topic == wotb.events.TOPIC_DAMAGE_RECEIVED or
            topic == wotb.events.TOPIC_VEHICLE_DAMAGED then
        return string.format("УРОН %d · HP %d · источник %d",
            number(data.damage), number(data.health),
            number(data.source_entity_id))
    elseif topic == wotb.events.TOPIC_RELOAD_STATE_CHANGED then
        return string.format("ПЕРЕЗАРЯДКА · %.1f с · %.0f%%%s",
            number(data.remaining_seconds), number(data.progress) * 100,
            data.paused and " · пауза" or "")
    elseif topic == wotb.events.TOPIC_AMMO_CHANGED then
        return string.format("СНАРЯД %d · осталось %d",
            number(data.shell_id), number(data.count))
    elseif topic == wotb.events.TOPIC_CAMERA_MODE_CHANGED or
            topic == wotb.events.TOPIC_SNIPER_ENTERED or
            topic == wotb.events.TOPIC_SNIPER_EXITED then
        return string.format("КАМЕРА %d → %d",
            number(data.previous_mode), number(data.mode))
    elseif topic == wotb.events.TOPIC_SHOT_FIRED then
        return string.format("ВЫСТРЕЛ · shell %d · code %d",
            number(data.shell_id), number(data.shot_code))
    elseif topic == wotb.events.TOPIC_SHELL_HIT then
        return string.format("ПОПАДАНИЕ · shot %d · shell %d",
            number(data.shot_id), number(data.shell_id))
    elseif topic == wotb.events.TOPIC_LOCAL_SHELL_FIRED then
        return string.format("LOCAL SHELL · public %d · type %d",
            number(data.shell_public_id), number(data.shell_type))
    elseif topic == wotb.events.TOPIC_BATTLE_ENTERED or
            topic == wotb.events.TOPIC_BATTLE_STARTED then
        return string.format("БОЙ · arena %d · state %d",
            number(data.arena_id), number(data.state))
    elseif topic == wotb.events.TOPIC_BATTLE_ENDED or
            topic == wotb.events.TOPIC_BATTLE_LEFT then
        return string.format("БОЙ ЗАВЕРШЁН · winner %d · reason %d",
            number(data.winner_team), number(data.reason))
    end
    return topic
end

local function render()
    if not mounted then return end
    set_text(controls.Status, string.format(
        "%s · получено событий: %d",
        paused and "ПАУЗА" or "LIVE", total_events))
    set_text(controls.Pause, paused and "ПРОДОЛЖИТЬ" or "ПАУЗА")
    for index = 1, MAX_LINES do
        local row = controls["Line" .. index]
        local value = lines[index]
        if value ~= nil then
            set_text(row, value)
            row:set_visible(true)
        else
            row:set_visible(false)
        end
    end
    data_dirty = false
end

local function build_ui()
    root = create("BattleTelemetryRoot", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = viewport_width, height = viewport_height,
        visible = true, interactable = false,
    }, screen)
    if root == nil then return false end
    local panel = create("Panel", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = SAFE_LEFT, y = SAFE_TOP,
        width = PANEL_WIDTH, height = PANEL_HEIGHT,
        background_color = COLORS.panel,
        visible = true, interactable = false,
    }, root)
    if panel == nil then return false end

    text_control("Title", panel, "БОЕВАЯ ТЕЛЕМЕТРИЯ · LUA EVENTS",
        20, 12, 360, 34, 23, COLORS.amber)
    button("Pause", panel, "ПАУЗА", 386, 10, 104, COLORS.blue)
    button("Clear", panel, "ОЧИСТИТЬ", 496, 10, 76, COLORS.red)
    text_control("Status", panel, "LIVE", 20, 49, 552, 26, 16, COLORS.muted)

    local template = text_control("LineTemplate", panel, "",
        20, 82, 552, 32, 17, COLORS.white)
    if template == nil then return false end
    template:set_visible(false)
    for index = 1, MAX_LINES do
        local row, err = template:clone()
        if row == nil then
            log_error("clone line", err)
            return false
        end
        remember("Line" .. index, row)
        row:set_id("TelemetryLine" .. index)
        row:set_parent(panel)
        row:set_position(20, 82 + (index - 1) * 36)
        row:set_size(552, 32)
        row:set_visible(false)
    end
    subscribe_ui(controls.Pause, function()
        paused = not paused
        data_dirty = true
    end)
    subscribe_ui(controls.Clear, function()
        lines = {}
        total_events = 0
        data_dirty = true
    end)
    layout()
    render()
    return true
end

local function unsubscribe_ui()
    for index = #ui_subscriptions, 1, -1 do
        local ok, err = wotb.ui.event_unsubscribe(ui_subscriptions[index])
        if not ok then log_error("UI cleanup", err) end
    end
    ui_subscriptions = {}
end

local function unmount()
    unsubscribe_ui()
    for index = #control_order, 1, -1 do
        local ok, err = control_order[index]:destroy()
        if not ok then log_error("control cleanup", err) end
    end
    controls = {}
    control_order = {}
    root = nil
    if screen ~= nil then
        local ok, err = wotb.handles.release(screen)
        if not ok then log_error("screen release", err) end
    end
    screen = nil
    mounted = false
end

local function release_screen(handle)
    if handle == nil then return end
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
        log_error("replacement screen", lookup_error)
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
    if mounted then return true end
    local err
    screen, err = wotb.ui.get_active_screen()
    if screen == nil then
        log_error("active screen", err)
        return false
    end
    viewport_width, viewport_height = read_viewport()
    if not build_ui() then
        unmount()
        return false
    end
    mounted = true
    data_dirty = true
    return true
end

local function refresh_context()
    local current, err = wotb.context.current()
    if current ~= nil then context_mask = current else log_error("context", err) end
    local allowed, visibility_error = wotb.context.should_show(
        VISIBLE_CONTEXTS, BLOCKED_CONTEXTS, context_mask)
    if allowed == nil then
        log_error("visibility", visibility_error)
        allowed = false
    end
    context_allowed = allowed
    context_dirty = false
end

local function subscribe_event(topic, callback)
    local token, err = wotb.events.subscribe(
        topic, callback, wotb.events.PRIORITY_NORMAL, true)
    if token == nil then
        log_error("event subscription " .. tostring(topic), err)
        return
    end
    event_subscriptions[#event_subscriptions + 1] = token
end

local function context_event(event)
    if type(event) == "table" and type(event.context_mask) == "number" then
        context_mask = event.context_mask
    end
    context_dirty = true
    if event.topic == wotb.events.TOPIC_BATTLE_ENTERED then
        lines = {}
        total_events = 0
        elapsed_seconds = 0
    end
    push_line(format_event(event))
end

local function screen_event(event)
    context_event(event)
    screen_rebind_pending = true
end

local function battle_event(event)
    push_line(format_event(event))
end

local function subscribe_events()
    subscribe_event(wotb.events.TOPIC_UI_SCREEN_CHANGED, screen_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENTERED, context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_STARTED, context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENDED, context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_LEFT, context_event)
    subscribe_event(wotb.events.TOPIC_DAMAGE_RECEIVED, battle_event)
    subscribe_event(wotb.events.TOPIC_VEHICLE_DAMAGED, battle_event)
    subscribe_event(wotb.events.TOPIC_RELOAD_STATE_CHANGED, battle_event)
    subscribe_event(wotb.events.TOPIC_AMMO_CHANGED, battle_event)
    subscribe_event(wotb.events.TOPIC_CAMERA_MODE_CHANGED, battle_event)
    subscribe_event(wotb.events.TOPIC_SNIPER_ENTERED, battle_event)
    subscribe_event(wotb.events.TOPIC_SNIPER_EXITED, battle_event)
    subscribe_event(wotb.events.TOPIC_SHOT_FIRED, battle_event)
    subscribe_event(wotb.events.TOPIC_SHELL_HIT, battle_event)
    subscribe_event(wotb.events.TOPIC_LOCAL_SHELL_FIRED, battle_event)
end

local function unsubscribe_events()
    for index = #event_subscriptions, 1, -1 do
        local ok, err = wotb.events.unsubscribe(event_subscriptions[index])
        if not ok then log_error("event cleanup", err) end
    end
    event_subscriptions = {}
end

function on_enable()
    unsubscribe_events()
    unmount()
    lines = {}
    total_events = 0
    elapsed_seconds = 0
    paused = false
    screen_rebind_pending = false
    context_dirty = true
    subscribe_events()
    refresh_context()
    if context_allowed then mount() end
end

function on_frame(frame_index, delta_seconds)
    if context_dirty or frame_index % 15 == 0 then refresh_context() end
    if not context_allowed then
        if mounted then unmount() end
        return
    end
    elapsed_seconds = elapsed_seconds + math.max(0, delta_seconds or 0)
    if mounted and (screen_rebind_pending or frame_index % 30 == 0) then
        rebind_active_screen()
    end
    if not mounted and not mount() then return end
    if frame_index % 60 == 0 then
        local width, height = read_viewport()
        if width ~= viewport_width or height ~= viewport_height then
            viewport_width, viewport_height = width, height
            layout()
        end
        if wotb.ui.control_get_snapshot(root) == nil then
            unmount()
            screen_rebind_pending = true
            return
        end
    end
    if data_dirty then render() end
end

function on_disable()
    unsubscribe_events()
    unmount()
    lines = {}
    context_mask = wotb.context.NONE
    context_allowed = false
    context_dirty = true
    print("Lua Battle Telemetry disabled")
end
