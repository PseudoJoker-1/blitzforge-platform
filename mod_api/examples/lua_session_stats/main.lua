local WINDOW_WIDTH = 1060
local WINDOW_HEIGHT = 650
local SAFE_LEFT = 128
local SAFE_RIGHT = 24
local SAFE_TOP = 48
local SAFE_BOTTOM = 32
local MAX_HISTORY = 8
local BATTLE_RESULT_GRACE_SECONDS = 2
local VISIBLE_CONTEXTS = wotb.context.HANGAR
local BLOCKED_CONTEXTS = wotb.context.MOD_SCREEN +
    wotb.context.TEXT_INPUT + wotb.context.RESULTS

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.66, g = 0.72, b = 0.78, a = 1.00},
    faint = {r = 0.43, g = 0.50, b = 0.57, a = 1.00},
    panel = {r = 0.025, g = 0.041, b = 0.060, a = 0.97},
    panel_alt = {r = 0.055, g = 0.078, b = 0.105, a = 0.98},
    row = {r = 0.075, g = 0.105, b = 0.135, a = 0.96},
    gold = {r = 0.96, g = 0.65, b = 0.10, a = 1.00},
    green = {r = 0.25, g = 0.78, b = 0.32, a = 1.00},
    red = {r = 0.87, g = 0.25, b = 0.20, a = 1.00},
    blue = {r = 0.16, g = 0.43, b = 0.68, a = 1.00},
}

local screen = nil
local root = nil
local controls = {}
local control_order = {}
local ui_subscriptions = {}
local event_subscriptions = {}
local mounted = false
local panel_open = true
local context_dirty = true
local context_allowed = false
local context_mask = wotb.context.NONE
local viewport_width = 1920
local viewport_height = 1080
local session_seconds = 0
local data_dirty = true
local reset_armed_seconds = 0
local history = {}
local current_battle = nil
local pending_battle_finish_seconds = 0
local totals = {}
local shot_source = "none"
local last_logged_visibility = ""

local function log_error(scope, err)
    print("Blitz Session Statistics " .. scope .. ": " .. tostring(err))
end

local function reset_totals()
    totals = {
        battles = 0,
        wins = 0,
        losses = 0,
        outcomes_known = 0,
        survival_known = 0,
        survived = 0,
        damage_received = 0,
        max_damage_received = 0,
        shots = 0,
        battle_seconds = 0,
    }
    history = {}
    session_seconds = 0
    reset_armed_seconds = 0
    pending_battle_finish_seconds = 0
    data_dirty = true
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

local function make_text(
        name, parent, text, x, y, width, height, size, color, alignment, wrap)
    return create(name, {
        type = wotb.ui.CONTROL_TEXT,
        text = text,
        x = x, y = y, width = width, height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = size,
        color = color or COLORS.white,
        alignment = alignment or wotb.ui.ALIGN_LEFT,
        text_wrap = wrap == true,
        visible = true,
        interactable = false,
    }, parent)
end

local function make_button(name, parent, text, x, y, width, height, background)
    return create(name, {
        type = wotb.ui.CONTROL_BUTTON,
        text = text,
        x = x, y = y, width = width, height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = 18,
        color = COLORS.white,
        background_color = background,
        alignment = wotb.ui.ALIGN_CENTER,
        visible = true,
        enabled = true,
        interactable = true,
    }, parent)
end

local function set_text(control, value)
    if control == nil then return false end
    local ok, err = control:set_text(value)
    if not ok then log_error("set_text", err) end
    return ok
end

local function set_visible(control, value)
    if control == nil then return false end
    local ok, err = control:set_visible(value)
    if not ok then log_error("set_visible", err) end
    return ok
end

local function set_color(control, value)
    if control == nil then return false end
    local ok, err = control:set_color(value)
    if not ok then log_error("set_color", err) end
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

local function format_integer(value)
    local source = tostring(math.max(0, math.floor((value or 0) + 0.5)))
    local result = source
    while true do
        local replaced, count = string.gsub(result, "^(%d+)(%d%d%d)", "%1 %2")
        result = replaced
        if count == 0 then break end
    end
    return result
end

local function format_duration(value)
    local seconds = math.max(0, math.floor((value or 0) + 0.5))
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor((seconds % 3600) / 60)
    local remainder = seconds % 60
    if hours > 0 then
        return string.format("%d:%02d:%02d", hours, minutes, remainder)
    end
    return string.format("%02d:%02d", minutes, remainder)
end

local function format_percent(numerator, denominator)
    if denominator == nil or denominator <= 0 then return "—" end
    return string.format("%d%%", math.floor(numerator * 100 / denominator + 0.5))
end

local function context_contains(mask, flag)
    local value = wotb.context.contains(mask or 0, flag)
    return value == true
end

local function battle_is_supported(mask)
    return not context_contains(mask, wotb.context.REPLAY) and
        not context_contains(mask, wotb.context.TRAINING)
end

local finish_battle

local function begin_battle(event)
    if current_battle ~= nil and not current_battle.finalized then
        if pending_battle_finish_seconds <= 0 then return end
        finish_battle(nil)
    end
    local event_mask = type(event) == "table" and event.context_mask or context_mask
    local data = type(event) == "table" and event.data or nil
    current_battle = {
        battle_id = type(data) == "table" and (data.battle_id or 0) or 0,
        arena_id = type(data) == "table" and (data.arena_id or 0) or 0,
        duration = 0,
        damage_received = 0,
        shots = 0,
        destroyed = false,
        local_health = nil,
        local_team = 0,
        local_entity_id = 0,
        winner_team = 0,
        reason = 0,
        eligible = battle_is_supported(event_mask),
        finalized = false,
    }
end

local function capture_local_player()
    if current_battle == nil or current_battle.finalized then return end
    local view, err = wotb.players.snapshot()
    if view == nil then
        if err ~= nil then log_error("players snapshot", err) end
        return
    end
    if type(view.local_team) == "number" and view.local_team > 0 then
        current_battle.local_team = view.local_team
    end
    local player = view.local_player
    if type(player) == "table" and type(player.health) == "number" then
        current_battle.local_health = player.health
        if type(player.team) == "number" and player.team > 0 then
            current_battle.local_team = player.team
        end
        if type(player.public_id) == "number" and player.public_id > 0 then
            current_battle.local_entity_id = player.public_id
        end
    end
end

finish_battle = function(event)
    if current_battle == nil or current_battle.finalized then return end
    pending_battle_finish_seconds = 0
    local data = type(event) == "table" and event.data or nil
    if type(data) == "table" then
        current_battle.winner_team = tonumber(data.winner_team) or 0
        current_battle.reason = tonumber(data.reason) or 0
        if current_battle.battle_id == 0 then
            current_battle.battle_id = tonumber(data.battle_id) or 0
        end
        if current_battle.arena_id == 0 then
            current_battle.arena_id = tonumber(data.arena_id) or 0
        end
    end
    current_battle.finalized = true
    if not current_battle.eligible then
        current_battle = nil
        return
    end

    local result = "unknown"
    if current_battle.winner_team > 0 and current_battle.local_team > 0 then
        totals.outcomes_known = totals.outcomes_known + 1
        if current_battle.winner_team == current_battle.local_team then
            result = "win"
            totals.wins = totals.wins + 1
        else
            result = "loss"
            totals.losses = totals.losses + 1
        end
    end

    local survived = nil
    if current_battle.destroyed then
        survived = false
    elseif type(current_battle.local_health) == "number" then
        survived = current_battle.local_health > 0
    end
    if survived ~= nil then
        totals.survival_known = totals.survival_known + 1
        if survived then totals.survived = totals.survived + 1 end
    end

    totals.battles = totals.battles + 1
    totals.damage_received = totals.damage_received + current_battle.damage_received
    totals.max_damage_received = math.max(
        totals.max_damage_received, current_battle.damage_received)
    totals.shots = totals.shots + current_battle.shots
    totals.battle_seconds = totals.battle_seconds + current_battle.duration

    table.insert(history, 1, {
        index = totals.battles,
        result = result,
        duration = current_battle.duration,
        damage_received = current_battle.damage_received,
        shots = current_battle.shots,
        survived = survived,
    })
    while #history > MAX_HISTORY do table.remove(history) end
    current_battle = nil
    data_dirty = true
end

local function result_label(value)
    if value == "win" then return "ПОБЕДА", COLORS.green end
    if value == "loss" then return "ПОРАЖЕНИЕ", COLORS.red end
    return "N/A", COLORS.muted
end

local function survival_label(value)
    if value == true then return "ВЫЖИЛ" end
    if value == false then return "УНИЧТОЖЕН" end
    return "ВЫЖИВАНИЕ N/A"
end

local function render()
    if not mounted then return end
    local average_damage = totals.battles > 0 and
        totals.damage_received / totals.battles or 0
    set_text(controls.LauncherTitle,
        string.format("СЕССИЯ  ·  %d БОЁВ", totals.battles))
    set_text(controls.LauncherSummary,
        string.format("урон %s  ·  выстрелы %s",
            format_integer(totals.damage_received),
            shot_source ~= "none" and format_integer(totals.shots) or "N/A"))
    set_text(controls.SessionSubtitle,
        "ТЕКУЩИЙ ЗАПУСК КЛИЕНТА  ·  " .. format_duration(session_seconds))
    set_text(controls.SessionBattlesValue, tostring(totals.battles))
    set_text(controls.SessionWinrateValue,
        format_percent(totals.wins, totals.outcomes_known))
    set_text(controls.SessionSurvivalValue,
        format_percent(totals.survived, totals.survival_known))
    set_text(controls.SessionDamageValue, format_integer(average_damage))
    set_text(controls.SessionShotsValue,
        shot_source ~= "none" and format_integer(totals.shots) or "—")
    set_text(controls.SessionShotsDetail,
        shot_source ~= "none" and "локальные" or "API N/A")

    set_text(controls.ResultCoverage, string.format(
        "РЕЗУЛЬТАТЫ  %d/%d подтверждено",
        totals.outcomes_known, totals.battles))
    set_text(controls.OutcomeTotals, string.format(
        "Победы  %d\nПоражения  %d\nРезультат N/A  %d",
        totals.wins, totals.losses, totals.battles - totals.outcomes_known))
    set_text(controls.CombatTotals, string.format(
        "Получено урона  %s\nМаксимум за бой  %s\nСреднее за бой  %s\nБоевого времени  %s",
        format_integer(totals.damage_received),
        format_integer(totals.max_damage_received),
        format_integer(average_damage),
        format_duration(totals.battle_seconds)))
    set_text(controls.DataQuality, string.format(
        "LIVE: полученный урон, время, выживание.%s\nN/A: нанесённый урон, убийства, попадания.\nПобеда считается только при подтверждённом winner_team.",
        shot_source ~= "none" and "\nLIVE: локальные выстрелы." or
            "\nN/A: локальные выстрелы на этом client build."))
    set_text(controls.ResetButton,
        reset_armed_seconds > 0 and "НАЖМИТЕ ЕЩЁ РАЗ" or "СБРОСИТЬ СЕССИЮ")

    set_visible(controls.EmptyHistory, #history == 0)
    for index = 1, MAX_HISTORY do
        local row = controls["HistoryRow" .. index]
        local value = history[index]
        if value == nil then
            set_visible(row, false)
        else
            local label, color = result_label(value.result)
            set_text(row, string.format(
                "#%02d   %-10s   %s   УРОН %s   %s ВЫСТР.   %s",
                value.index, label, format_duration(value.duration),
                format_integer(value.damage_received),
                shot_source ~= "none" and tostring(value.shots) or "N/A",
                survival_label(value.survived)))
            set_color(row, color)
            set_visible(row, true)
        end
    end
    data_dirty = false
end

local function layout()
    if root == nil then return end
    root:set_size(viewport_width, viewport_height)
    local centered_x = (viewport_width - WINDOW_WIDTH) * 0.5
    local centered_y = (viewport_height - WINDOW_HEIGHT) * 0.5
    local max_x = math.max(
        SAFE_LEFT, viewport_width - WINDOW_WIDTH - SAFE_RIGHT)
    local max_y = math.max(
        SAFE_TOP, viewport_height - WINDOW_HEIGHT - SAFE_BOTTOM)
    controls.SessionWindow:set_position(
        math.max(SAFE_LEFT, math.min(centered_x, max_x)),
        math.max(SAFE_TOP, math.min(centered_y, max_y)))
    controls.SessionLauncher:set_position(
        math.max(SAFE_LEFT, viewport_width - 342),
        math.max(260, math.min(viewport_height - 120, viewport_height * 0.36)))
end

local function set_panel_open(value)
    panel_open = value
    set_visible(controls.SessionWindow, value)
    set_visible(controls.SessionLauncher, not value)
end

local function create_card(parent, prefix, x, title, detail, value_color)
    local card = create(prefix .. "Card", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = x, y = 88, width = 196, height = 116,
        background_color = COLORS.panel_alt,
        visible = true, interactable = false,
    }, parent)
    if card == nil then return false end
    make_text(prefix .. "Label", card, title,
        14, 10, 168, 22, 16, COLORS.muted)
    make_text(prefix .. "Value", card, "0",
        14, 34, 168, 43, 34, value_color or COLORS.white)
    make_text(prefix .. "Detail", card, detail,
        14, 83, 168, 21, 15, COLORS.faint)
    return true
end

local function build_ui()
    root = create("SessionStatsRoot", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = viewport_width, height = viewport_height,
        visible = true, interactable = false,
    }, screen)
    if root == nil then return false end

    local launcher = create("SessionLauncher", {
        type = wotb.ui.CONTROL_BUTTON,
        x = viewport_width - 342, y = 360, width = 318, height = 78,
        background_color = COLORS.panel,
        visible = false, enabled = true, interactable = true,
    }, root)
    if launcher == nil then return false end
    create("LauncherAccent", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = 7, height = 78,
        background_color = COLORS.gold,
        visible = true, interactable = false,
    }, launcher)
    make_text("LauncherTitle", launcher, "СЕССИЯ  ·  0 БОЁВ",
        20, 10, 282, 28, 21, COLORS.gold)
    make_text("LauncherSummary", launcher, "урон 0  ·  выстрелы 0",
        20, 42, 282, 24, 16, COLORS.muted)

    local window = create("SessionWindow", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = WINDOW_WIDTH, height = WINDOW_HEIGHT,
        background_color = COLORS.panel,
        visible = true, interactable = true,
    }, root)
    if window == nil then return false end
    create("HeaderAccent", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = 8, height = 78,
        background_color = COLORS.gold,
        visible = true, interactable = false,
    }, window)
    make_text("SessionTitle", window, "БОЕВАЯ СЕССИЯ",
        24, 10, 500, 36, 29, COLORS.white)
    make_text("SessionSubtitle", window, "ТЕКУЩИЙ ЗАПУСК КЛИЕНТА",
        24, 47, 520, 24, 16, COLORS.muted)
    make_button("CloseButton", window, "ЗАКРЫТЬ",
        914, 17, 124, 42, COLORS.blue)

    if not create_card(window, "SessionBattles", 20,
            "БОИ", "завершено", COLORS.gold) then return false end
    if not create_card(window, "SessionWinrate", 226,
            "ПОБЕДЫ", "из известных", COLORS.green) then return false end
    if not create_card(window, "SessionSurvival", 432,
            "ВЫЖИВАЕМОСТЬ", "из известных", COLORS.white) then return false end
    if not create_card(window, "SessionDamage", 638,
            "СРЕДНИЙ УРОН", "полученный", COLORS.red) then return false end
    if not create_card(window, "SessionShots", 844,
            "ВЫСТРЕЛЫ", "локальные", COLORS.gold) then return false end

    local history_panel = create("HistoryPanel", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 20, y = 218, width = 620, height = 410,
        background_color = COLORS.panel_alt,
        visible = true, interactable = false,
    }, window)
    if history_panel == nil then return false end
    make_text("HistoryTitle", history_panel, "ПОСЛЕДНИЕ БОИ",
        16, 10, 300, 30, 21, COLORS.gold)
    make_text("HistoryLegend", history_panel,
        "Результат · время · полученный урон · выстрелы · выживание",
        16, 37, 586, 22, 15, COLORS.muted)
    make_text("EmptyHistory", history_panel,
        "Завершите бой — подтверждённые показатели появятся здесь.",
        16, 82, 586, 52, 18, COLORS.white, wotb.ui.ALIGN_LEFT, true)
    local row_template = make_text("HistoryRowTemplate", history_panel, "",
        16, 66, 588, 36, 16, COLORS.white)
    if row_template == nil then return false end
    set_visible(row_template, false)
    for index = 1, MAX_HISTORY do
        local row, err = row_template:clone()
        if row == nil then
            log_error("clone history row", err)
            return false
        end
        remember("HistoryRow" .. index, row)
        row:set_id("SessionHistoryRow" .. index)
        row:set_parent(history_panel)
        row:set_position(16, 66 + (index - 1) * 40)
        row:set_size(588, 36)
        set_visible(row, false)
    end

    local details = create("DetailsPanel", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 650, y = 218, width = 390, height = 410,
        background_color = COLORS.panel_alt,
        visible = true, interactable = false,
    }, window)
    if details == nil then return false end
    make_text("DetailsTitle", details, "ИТОГИ СЕССИИ",
        16, 10, 350, 30, 21, COLORS.gold)
    make_text("ResultCoverage", details, "РЕЗУЛЬТАТЫ  0/0 подтверждено",
        16, 47, 350, 26, 16, COLORS.white)
    make_text("OutcomeTotals", details,
        "Победы  0\nПоражения  0\nРезультат N/A  0",
        16, 78, 350, 76, 17, COLORS.muted, wotb.ui.ALIGN_LEFT, true)
    make_text("CombatTotals", details,
        "Получено урона  0\nМаксимум за бой  0\nСреднее за бой  0\nБоевого времени  00:00",
        16, 158, 350, 100, 17, COLORS.white, wotb.ui.ALIGN_LEFT, true)
    make_text("DataQualityTitle", details, "ДАННЫЕ API",
        16, 266, 350, 24, 17, COLORS.gold)
    make_text("DataQuality", details,
        "LIVE: полученный урон, выстрелы, время, выживание.\nN/A: нанесённый урон, убийства, попадания.\nПобеда считается только при подтверждённом winner_team.",
        16, 292, 350, 70, 15, COLORS.muted, wotb.ui.ALIGN_LEFT, true)
    make_button("ResetButton", details, "СБРОСИТЬ СЕССИЮ",
        16, 365, 358, 34, COLORS.blue)

    subscribe_ui(window, function() end)
    subscribe_ui(launcher, function() set_panel_open(true) end)
    subscribe_ui(controls.CloseButton, function() set_panel_open(false) end)
    subscribe_ui(controls.ResetButton, function()
        if reset_armed_seconds > 0 then
            reset_totals()
        else
            reset_armed_seconds = 4
            data_dirty = true
        end
    end)
    layout()
    set_panel_open(panel_open)
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
    local was_mounted = mounted
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
    if was_mounted then print("Blitz Session Statistics UI unmounted") end
end

local function release_screen(handle)
    if handle == nil then return end
    local ok, err = wotb.handles.release(handle)
    if not ok then log_error("screen release", err) end
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
    print(string.format(
        "Blitz Session Statistics UI mounted viewport=%dx%d",
        viewport_width, viewport_height))
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
    local visibility = string.format(
        "%d:%s",
        context_mask,
        tostring(context_allowed))
    if visibility ~= last_logged_visibility then
        print(string.format(
            "Blitz Session Statistics context=%d allowed=%s",
            context_mask,
            tostring(context_allowed)))
        last_logged_visibility = visibility
    end
    if current_battle ~= nil and not battle_is_supported(context_mask) then
        current_battle.eligible = false
    end
end

local function subscribe_event(topic, callback)
    local token, err = wotb.events.subscribe(
        topic, callback, wotb.events.PRIORITY_NORMAL, true)
    if token == nil then
        log_error("event subscription " .. tostring(topic), err)
        return false
    end
    event_subscriptions[#event_subscriptions + 1] = token
    return true
end

local function on_context_event(event)
    if type(event) == "table" and type(event.context_mask) == "number" then
        context_mask = event.context_mask
    end
    context_dirty = true
end

local function on_screen_changed(event)
    on_context_event(event)
end

local function on_battle_entered(event)
    on_context_event(event)
    begin_battle(event)
end

local function on_battle_ended(event)
    on_context_event(event)
    finish_battle(event)
end

local function on_battle_left(event)
    on_context_event(event)
    if current_battle == nil or current_battle.finalized then return end
    local data = type(event) == "table" and event.data or nil
    if type(data) == "table" then
        current_battle.winner_team = tonumber(data.winner_team) or 0
        current_battle.reason = tonumber(data.reason) or 0
        if current_battle.battle_id == 0 then
            current_battle.battle_id = tonumber(data.battle_id) or 0
        end
        if current_battle.arena_id == 0 then
            current_battle.arena_id = tonumber(data.arena_id) or 0
        end
    end
    pending_battle_finish_seconds = BATTLE_RESULT_GRACE_SECONDS
end

local function on_damage_received(event)
    if current_battle == nil or current_battle.finalized then return end
    local data = type(event) == "table" and event.data or nil
    local damage = type(data) == "table" and tonumber(data.damage) or nil
    if damage ~= nil and damage > 0 then
        current_battle.damage_received = current_battle.damage_received + damage
    end
end

local function on_local_shell_fired()
    if current_battle ~= nil and not current_battle.finalized then
        current_battle.shots = current_battle.shots + 1
    end
end

local function on_public_shot_fired(event)
    if current_battle == nil or current_battle.finalized or
            current_battle.local_entity_id <= 0 then return end
    local data = type(event) == "table" and event.data or nil
    local entity_id = type(data) == "table" and
        tonumber(data.primary_entity_id) or nil
    if entity_id == current_battle.local_entity_id then
        current_battle.shots = current_battle.shots + 1
    end
end

local function on_local_vehicle_destroyed()
    if current_battle ~= nil and not current_battle.finalized then
        current_battle.destroyed = true
        current_battle.local_health = 0
    end
end

local function subscribe_events()
    subscribe_event(wotb.events.TOPIC_UI_SCREEN_CHANGED, on_screen_changed)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENTERED, on_battle_entered)
    subscribe_event(wotb.events.TOPIC_BATTLE_STARTED, on_battle_entered)
    subscribe_event(wotb.events.TOPIC_BATTLE_ENDED, on_battle_ended)
    subscribe_event(wotb.events.TOPIC_BATTLE_LEFT, on_battle_left)
    subscribe_event(wotb.events.TOPIC_DAMAGE_RECEIVED, on_damage_received)
    if subscribe_event(
            wotb.events.TOPIC_LOCAL_SHELL_FIRED,
            on_local_shell_fired) then
        shot_source = "local_shell"
    elseif subscribe_event(
            wotb.events.TOPIC_SHOT_FIRED,
            on_public_shot_fired) then
        shot_source = "public_shot"
    else
        shot_source = "none"
    end
    subscribe_event(
        wotb.events.TOPIC_LOCAL_VEHICLE_DESTROYED,
        on_local_vehicle_destroyed)
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
    reset_totals()
    current_battle = nil
    pending_battle_finish_seconds = 0
    panel_open = true
    shot_source = "none"
    last_logged_visibility = ""
    context_dirty = true
    subscribe_events()
    refresh_context()
    if context_allowed then mount() end
end

function on_frame(frame_index, delta_seconds)
    local delta = math.max(0, delta_seconds or 0)
    session_seconds = session_seconds + delta
    if current_battle ~= nil and not current_battle.finalized then
        current_battle.duration = current_battle.duration + delta
        if frame_index % 30 == 0 then capture_local_player() end
    end
    if pending_battle_finish_seconds > 0 then
        pending_battle_finish_seconds = math.max(
            0, pending_battle_finish_seconds - delta)
        if pending_battle_finish_seconds == 0 then finish_battle(nil) end
    end
    if reset_armed_seconds > 0 then
        local previous = reset_armed_seconds
        reset_armed_seconds = math.max(0, reset_armed_seconds - delta)
        if previous > 0 and reset_armed_seconds == 0 then data_dirty = true end
    end

    if context_dirty or frame_index % 15 == 0 then refresh_context() end
    if not context_allowed then
        if mounted then unmount() end
        return
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
            return
        end
    end
    -- Text/style mutations currently replace the backing DAVA YAML resource.
    -- Keep the dashboard event-driven: an idle hangar must not rebuild a
    -- control every few frames just to advance the session clock.
    if data_dirty then render() end
end

function on_disable()
    unsubscribe_events()
    unmount()
    current_battle = nil
    pending_battle_finish_seconds = 0
    history = {}
    context_mask = wotb.context.NONE
    context_allowed = false
    context_dirty = true
    print("Blitz Session Statistics disabled")
end
