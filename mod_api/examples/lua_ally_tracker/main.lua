local MAX_ROWS = 7
local PANEL_WIDTH = 620
local PANEL_HEIGHT = 570
local ROW_HEIGHT = 48
local SAFE_LEFT = 128
local SAFE_RIGHT = 28
local SAFE_TOP = 116
local VISIBLE_CONTEXTS = wotb.context.BATTLE +
    wotb.context.TRAINING + wotb.context.REPLAY
local BLOCKED_CONTEXTS = wotb.context.MOD_SCREEN +
    wotb.context.TEXT_INPUT

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.68, g = 0.75, b = 0.82, a = 1.00},
    panel = {r = 0.025, g = 0.045, b = 0.065, a = 0.92},
    row = {r = 0.07, g = 0.11, b = 0.15, a = 0.96},
    accent = {r = 0.24, g = 0.76, b = 0.42, a = 1.00},
    warning = {r = 0.96, g = 0.65, b = 0.12, a = 1.00},
}

local screen = nil
local root = nil
local controls = {}
local control_order = {}
local row_public_ids = {}
local ui_subscriptions = {}
local event_subscriptions = {}
local records = {}
local mounted = false
local screen_rebind_pending = false
local context_dirty = true
local context_allowed = false
local context_mask = wotb.context.NONE
local data_dirty = true
local collapsed = false
local selected_id = nil
local local_team = 0
local elapsed_seconds = 0
local viewport_width = 1920
local viewport_height = 1080

local function log_error(scope, err)
    print("Lua Ally Tracker " .. scope .. ": " .. tostring(err))
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

local function make_text(name, parent, text, x, y, width, height, size, color)
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

local function make_button(name, parent, text, x, y, width, height)
    return create(name, {
        type = wotb.ui.CONTROL_BUTTON,
        text = text,
        x = x,
        y = y,
        width = width,
        height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = 19,
        color = COLORS.white,
        background_color = COLORS.row,
        alignment = wotb.ui.ALIGN_LEFT,
        visible = true,
        enabled = true,
        interactable = true,
    }, parent)
end

local function set_text(control, value)
    local ok, err = control:set_text(value)
    if not ok then log_error("set_text", err) end
    return ok
end

local function set_visible(control, value)
    local ok, err = control:set_visible(value)
    if not ok then log_error("set_visible", err) end
    return ok
end

local function subscribe_ui(control, callback)
    local token, err = control:on(wotb.ui.EVENT_CLICK, callback)
    if token == nil then
        log_error("UI subscription", err)
        return false
    end
    ui_subscriptions[#ui_subscriptions + 1] = token
    return true
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
    local x = math.max(SAFE_LEFT,
        viewport_width - PANEL_WIDTH - SAFE_RIGHT)
    controls.Panel:set_position(x, SAFE_TOP)
end

local function clear_records()
    records = {}
    selected_id = nil
    local_team = 0
    elapsed_seconds = 0
    data_dirty = true
end

local function update_record(value, currently_visible)
    if type(value) ~= "table" or value.is_local == true or
            value.local_player == true or value.local_player == 1 then
        return
    end
    if value.relation ~= "ally" and not value.is_ally then
        return
    end
    local public_id = tonumber(value.public_id)
    if public_id == nil or public_id <= 0 then return end

    local record = records[public_id]
    local changed = record == nil
    if record == nil then record = {public_id = public_id} end

    local display_name = value.display_name_available and
        value.display_name or ("Союзник #" .. tostring(public_id))
    local team = tonumber(value.team) or 0
    local public_type = value.public_type or "vehicle"
    local health = tonumber(value.health) or 0
    local max_health = tonumber(value.max_health) or 0
    local health_percent = value.health_percent
    local alive = value.alive == true or health > 0
    local visible = currently_visible ~= false
    changed = changed or record.display_name ~= display_name or
        record.team ~= team or record.public_type ~= public_type or
        record.health ~= health or record.max_health ~= max_health or
        record.health_percent ~= health_percent or record.alive ~= alive or
        record.currently_visible ~= visible

    record.display_name = display_name
    record.team = team
    record.public_type = public_type
    record.health = health
    record.max_health = max_health
    record.health_percent = health_percent
    record.alive = alive
    record.currently_visible = visible
    record.last_update = elapsed_seconds

    if value.position_available == true and type(value.position) == "table" and
            type(value.position.x) == "number" and
            type(value.position.y) == "number" and
            type(value.position.z) == "number" then
        local next_position = {
            x = value.position.x,
            y = value.position.y,
            z = value.position.z,
        }
        changed = changed or record.position == nil or
            record.position.x ~= next_position.x or
            record.position.y ~= next_position.y or
            record.position.z ~= next_position.z
        record.position = next_position
        record.position_time = elapsed_seconds
    end
    records[public_id] = record
    if changed then data_dirty = true end
end

local function refresh_players()
    local snapshot, err = wotb.players.snapshot()
    if snapshot == nil then
        log_error("players.snapshot", err)
        return false
    end
    if type(snapshot.local_player) == "table" then
        local_team = tonumber(snapshot.local_player.team) or 0
    elseif tonumber(snapshot.local_team) ~= nil then
        local_team = tonumber(snapshot.local_team) or 0
    end
    local seen = {}
    for _, value in ipairs(snapshot.our_team or {}) do
        if not value.is_local then
            update_record(value, true)
            local public_id = tonumber(value.public_id)
            if public_id ~= nil then seen[public_id] = true end
        end
    end
    for public_id, record in pairs(records) do
        if not seen[public_id] and record.currently_visible then
            record.currently_visible = false
            data_dirty = true
        end
    end
    return true
end

local function sorted_records()
    local values = {}
    for _, record in pairs(records) do values[#values + 1] = record end
    table.sort(values, function(left, right)
        if left.alive ~= right.alive then return left.alive end
        return left.public_id < right.public_id
    end)
    return values
end

local function age_text(record)
    local age = math.max(0, elapsed_seconds - (record.last_update or 0))
    if age < 1 then return "сейчас" end
    return string.format("%.0f с назад", age)
end

local function position_text(record)
    if record.position == nil then return "позиция API недоступна" end
    return string.format("x %.1f  y %.1f  z %.1f",
        record.position.x, record.position.y, record.position.z)
end

local function row_text(record)
    local hp = record.max_health > 0 and
        string.format("%d/%d", record.health, record.max_health) or
        tostring(record.health)
    local state = record.alive and "ЖИВ" or "УНИЧТОЖЕН"
    local visibility = record.currently_visible and "ВИДИМ" or "ПОСЛЕДНЕЕ"
    return string.format("%s  ·  HP %s  ·  %s/%s  ·  %s",
        record.display_name, hp, state, visibility, age_text(record))
end

local function detail_text(record)
    if record == nil then
        return "Выберите союзника · координаты выводятся только с подтверждённым источником"
    end
    local percent = record.health_percent and
        string.format("%.0f%%", record.health_percent) or "—"
    return string.format(
        "ID %d · team %d · %s · HP %d/%d (%s)\nПоследняя подтверждённая позиция: %s",
        record.public_id, record.team, record.public_type,
        record.health, record.max_health, percent, position_text(record))
end

local function render_records()
    if not mounted then return end
    local values = sorted_records()
    set_text(controls.Title,
        string.format("СОЮЗНИКИ · %d ПУБЛИЧНЫХ ЗАПИСЕЙ", #values))
    for index = 1, MAX_ROWS do
        local row = controls["Row" .. index]
        local record = values[index]
        if record ~= nil and not collapsed then
            row_public_ids[index] = record.public_id
            set_text(row, row_text(record))
            set_visible(row, true)
        else
            row_public_ids[index] = nil
            set_visible(row, false)
        end
    end
    set_visible(controls.Detail, not collapsed)
    set_visible(controls.Footer, not collapsed)
    set_text(controls.Collapse, collapsed and "+" or "–")
    set_text(controls.Detail, detail_text(records[selected_id]))
    controls.Panel:set_size(PANEL_WIDTH, collapsed and 66 or PANEL_HEIGHT)
    data_dirty = false
end

local function build_ui()
    root = create("AllyTrackerRoot", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0,
        width = viewport_width,
        height = viewport_height,
        visible = true,
        interactable = false,
    }, screen)
    if root == nil then return false end

    local panel = create("Panel", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = SAFE_TOP,
        width = PANEL_WIDTH,
        height = PANEL_HEIGHT,
        background_color = COLORS.panel,
        visible = true,
        interactable = false,
    }, root)
    if panel == nil then return false end

    make_text("Title", panel, "СОЮЗНИКИ", 22, 12, 520, 34, 24, COLORS.accent)
    make_button("Collapse", panel, "–", 556, 10, 42, 40)
    make_text("Subtitle", panel,
        "HP · public attributes · последнее подтверждённое обновление",
        22, 46, 560, 26, 17, COLORS.muted)

    local template = make_button(
        "RowTemplate", panel, "", 20, 82, 580, ROW_HEIGHT)
    if template == nil then return false end
    set_visible(template, false)

    for index = 1, MAX_ROWS do
        local row_index = index
        local row, err = template:clone()
        if row == nil then
            log_error("clone row", err)
            return false
        end
        remember("Row" .. index, row)
        row:set_id("AllyRow" .. index)
        row:set_parent(panel)
        row:set_position(20, 82 + (index - 1) * (ROW_HEIGHT + 4))
        row:set_size(580, ROW_HEIGHT)
        set_visible(row, false)
        subscribe_ui(row, function()
            selected_id = row_public_ids[row_index]
            data_dirty = true
        end)
    end

    make_text("Detail", panel,
        "Выберите союзника", 22, 458, 576, 62, 17, COLORS.white)
    make_text("Footer", panel,
        "Только публичные данные. Скрытые противники не сохраняются.",
        22, 530, 576, 26, 16, COLORS.warning)
    subscribe_ui(controls.Collapse, function()
        collapsed = not collapsed
        data_dirty = true
    end)
    layout()
    render_records()
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
    row_public_ids = {}
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

local function on_context_event(event)
    if type(event) == "table" and type(event.context_mask) == "number" then
        context_mask = event.context_mask
    end
    context_dirty = true
end

local function on_screen_changed(event)
    on_context_event(event)
    screen_rebind_pending = true
end

local function on_entity_event(event)
    local data = type(event) == "table" and event.data or nil
    local snapshot = type(data) == "table" and data.snapshot or nil
    if type(snapshot) ~= "table" then return end

    if local_team > 0 and snapshot.team == local_team then
        snapshot.is_ally = true
        snapshot.relation = "ally"
        update_record(snapshot,
            event.topic ~= wotb.events.TOPIC_PUBLIC_ENTITY_REMOVED)
    end
end

local function subscribe_events()
    subscribe_event(wotb.events.TOPIC_UI_SCREEN_CHANGED, on_screen_changed)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENTERED, on_context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_STARTED, on_context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENDED, on_context_event)
    subscribe_event(wotb.events.TOPIC_BATTLE_LEFT, function(event)
        on_context_event(event)
        clear_records()
    end)
    subscribe_event(wotb.events.TOPIC_PUBLIC_ENTITY_ADDED, on_entity_event)
    subscribe_event(wotb.events.TOPIC_PUBLIC_ENTITY_UPDATED, on_entity_event)
    subscribe_event(wotb.events.TOPIC_PUBLIC_ENTITY_REMOVED, on_entity_event)
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
    clear_records()
    screen_rebind_pending = false
    context_dirty = true
    subscribe_events()
    refresh_context()
    if context_allowed then
        refresh_players()
        mount()
    end
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

    if frame_index % 30 == 0 then refresh_players() end
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
    if data_dirty then render_records() end
end

function on_disable()
    unsubscribe_events()
    unmount()
    clear_records()
    context_mask = wotb.context.NONE
    context_allowed = false
    context_dirty = true
    print("Lua Ally Tracker disabled")
end
