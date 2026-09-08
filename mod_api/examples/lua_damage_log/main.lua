-- example.lua_damage_log - "ЖУРНАЛ УРОНА"
--
-- What hit you, for how much, and what it did to your HP. Every value on the
-- screen was observed; nothing is filled in.
--
-- PROVENANCE. There are exactly two sources and they are labelled apart
-- everywhere they meet:
--
--   wotbmod.gameplay.damage_received -> event.data
--       damage, previous_health, health, reason_code, source_entity_id.
--       That is the whole payload: WotbModV3DamageEventData (events_v1.h:147)
--       has five fields and no sixth. The runtime derives this topic only when
--       the damaged vehicle is the current local one, so the HP in it is ours.
--   wotb.players.local_player().max_health -> entity_public.enumerate_visible
--       maximum HP, which no event carries at all. A different source, and it
--       is named on the panel every time a number derived from it is shown.
--
--   damage_dealt has no source in this API and is not synthesised here.
--
-- WHAT IS SHOWN WHEN A FIELD IS NOT THERE. Never 0, never a guess:
--   damage/HP missing from the payload      -> "—"
--   reason_code missing                     -> "причина: нет в payload"
--   source_entity_id == 0                   -> "источник: не указан"
--   source id set but not a visible entity  -> "#id, нет в публичном снимке"
--   source id set, entity visible, no name  -> "#id, имя не публикуется"
--   whole payload not typed                 -> the line still appears, saying so
--   max HP never observed                   -> "макс. HP: нет данных", no share
--   the damage subscription was refused     -> said on the legend row, so an
--                                              empty log can never be mistaken
--                                              for a quiet battle
--
-- PER-FRAME COST. on_frame does: one math.type test, two panel:update calls
-- (each one integer modulo plus two field reads on a frame with nothing to do),
-- and two boolean tests. Nothing is allocated and no string is built on an idle
-- frame, and no ABI call is made.
--   * the battle rows are rebuilt only when log_dirty is set - a damage event,
--     a resolved source name, a pause toggle, a remount. At most 11 set_row
--     calls, each of which compares before it crosses the ABI.
--   * entity_public.enumerate_visible runs only while a source id is still
--     unresolved, and once every 60 frames while max HP is still unknown. Once
--     both have settled it never runs again in that battle.
--   * wotb.timer is deliberately not used, so this mod holds no
--     wotbmod.frame.update subscription of its own.
--   * storage is read when the hangar summary is dirty and written once per
--     battle, never per frame.
--
-- Event callbacks only append to Lua tables. Every control, subscription and
-- handle is released in on_disable.

local MAX_EVENTS = 8
local ROW_TITLE = 1
local ROW_STATE = 2
local ROW_FIRST_EVENT = 3
local ROW_LEGEND = ROW_FIRST_EVENT + MAX_EVENTS
local BATTLE_ROWS = ROW_LEGEND

local SESSION_ROW_TITLE = 1
local SESSION_ROW_FIRST = 2
local SESSION_ROW_LAST = 6
local SESSION_ROW_FOOTER = 7

local MAX_HEALTH_PROBE_FRAMES = 60
local MAX_HEALTH_PROBE_ATTEMPTS = 12
local STORE_KEY = "totals"
local NO_DATA = "нет данных"
local DASH = "—"
local LEGEND =
    "урон, HP, причина и источник — только из события; «" .. DASH ..
    "» = клиент не сообщил"

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.68, g = 0.75, b = 0.82, a = 1.00},
    faint = {r = 0.46, g = 0.53, b = 0.60, a = 1.00},
    panel = {r = 0.025, g = 0.040, b = 0.058, a = 0.93},
    amber = {r = 0.96, g = 0.66, b = 0.14, a = 1.00},
    red = {r = 0.88, g = 0.34, b = 0.28, a = 1.00},
    blue = {r = 0.16, g = 0.52, b = 0.84, a = 1.00},
}

-- Every field is an integer counter with a default of 0, and wotb.config never
-- writes a default over a stored value: new() writes nothing, get() writes
-- nothing, and save() writes the document it just read with only the keys this
-- mod explicitly set laid over it.
local SCHEMA = {
    battles = {type = "integer", default = 0, min = 0},
    hits = {type = "integer", default = 0, min = 0},
    damage = {type = "integer", default = 0, min = 0},
    worst_hit = {type = "integer", default = 0, min = 0},
    hits_without_source = {type = "integer", default = 0, min = 0},
    last_battle_hits = {type = "integer", default = 0, min = 0},
    last_battle_damage = {type = "integer", default = 0, min = 0},
}

local battle_panel = nil
local session_panel = nil
local subscriptions = {}
local store = nil
local store_error = nil

local events = {}
local frozen_events = {}
local pending_sources = 0
local paused = false
local log_dirty = true
local session_dirty = true
local session_showing = false

local battle_hits = 0
local battle_damage = 0
local battle_damage_unknown = 0
local battle_worst = 0
local battle_without_source = 0
local battle_committed = false

local max_health = nil
local max_health_note = "снимок ещё не запрашивался"
local max_health_attempts = 0

local damage_subscribe_error = nil
local lifecycle_subscribe_error = nil
local persist_error = nil

local function log_error(scope, err)
    local ok = wotb.log.error("%s: %s", scope, tostring(err))
    if not ok then print("Lua Damage Log " .. scope .. ": " .. tostring(err)) end
end

local function integer(value)
    if math.type(value) == "integer" then return value end
    return nil
end

local function amount(value)
    local number = integer(value)
    if number == nil then return DASH end
    return string.format("%d", number)
end

local function reset_battle()
    events = {}
    frozen_events = {}
    pending_sources = 0
    battle_hits = 0
    battle_damage = 0
    battle_damage_unknown = 0
    battle_worst = 0
    battle_without_source = 0
    battle_committed = false
    max_health = nil
    max_health_note = "снимок ещё не запрашивался"
    max_health_attempts = 0
    log_dirty = true
end

-- ---------------------------------------------------------------------------
-- persisted totals
-- ---------------------------------------------------------------------------

local function open_store()
    store = nil
    store_error = nil
    local opened, err = wotb.config.new({
        backend = "storage",
        key = STORE_KEY,
        autosave = false,
        schema = SCHEMA,
    })
    if opened == nil then
        store_error = tostring(err)
        return
    end
    store = opened
end

-- Read, add, write - never a blind write. A read that fails stops the commit
-- entirely, because a total computed on top of a failed read is a total that
-- would overwrite whatever is really stored with a smaller number.
--
-- battle_committed is set only after save() returns, and save() is the only
-- call here that writes anything: a commit that fails halfway wrote nothing, so
-- the second lifecycle event (battle.left after battle.ended) may safely try
-- again, and a commit that succeeded is never repeated.
local function commit_battle()
    if battle_committed or battle_hits == 0 then return end
    if store == nil then
        persist_error = store_error or "wotb.config недоступен"
        return
    end
    local reloaded, reload_error = store:reload()
    if not reloaded then
        persist_error = tostring(reload_error)
        return
    end

    local additions = {
        battles = 1,
        hits = battle_hits,
        damage = battle_damage,
        hits_without_source = battle_without_source,
    }
    for name, delta in next, additions do
        local current, origin = store:get(name)
        if current == nil then
            persist_error = tostring(origin)
            return
        end
        local written, write_error = store:set(name, current + delta)
        if not written then
            persist_error = tostring(write_error)
            return
        end
    end

    local worst, worst_origin = store:get("worst_hit")
    if worst == nil then
        persist_error = tostring(worst_origin)
        return
    end
    if battle_worst > worst then
        local written, write_error = store:set("worst_hit", battle_worst)
        if not written then
            persist_error = tostring(write_error)
            return
        end
    end

    local last = {
        last_battle_hits = battle_hits,
        last_battle_damage = battle_damage,
    }
    for name, value in next, last do
        local written, write_error = store:set(name, value)
        if not written then
            persist_error = tostring(write_error)
            return
        end
    end

    local saved, save_error = store:save()
    if not saved then
        persist_error = tostring(save_error)
        return
    end
    battle_committed = true
    persist_error = nil
    session_dirty = true
end

-- ---------------------------------------------------------------------------
-- damage events
-- ---------------------------------------------------------------------------

local function push_record(record)
    local target = paused and frozen_events or events
    table.insert(target, 1, record)
    while #target > MAX_EVENTS do table.remove(target) end
    log_dirty = true
end

local function on_damage(event)
    if type(event) ~= "table" then return end
    battle_hits = battle_hits + 1

    local data = event.data
    if type(data) ~= "table" then
        battle_damage_unknown = battle_damage_unknown + 1
        battle_without_source = battle_without_source + 1
        push_record({index = battle_hits, typed = false})
        return
    end

    local damage = integer(data.damage)
    if damage == nil then
        battle_damage_unknown = battle_damage_unknown + 1
    else
        battle_damage = battle_damage + damage
        if damage > battle_worst then battle_worst = damage end
    end

    -- reason_raw and source_raw are kept as the payload gave them, so the line
    -- can tell "the field was not there" apart from "the field was there and
    -- was not a number". Both are unavailability; they are not the same fact.
    local record = {
        index = battle_hits,
        typed = true,
        damage = damage,
        previous_health = integer(data.previous_health),
        health = integer(data.health),
        reason_raw = data.reason_code,
        source_raw = data.source_entity_id,
        source_id = integer(data.source_entity_id),
    }
    if data.source_entity_id == nil then
        record.source_state = "absent"
        battle_without_source = battle_without_source + 1
    elseif record.source_id == nil then
        record.source_state = "unreadable"
        battle_without_source = battle_without_source + 1
    elseif record.source_id == 0 then
        record.source_state = "unset"
        battle_without_source = battle_without_source + 1
    elseif record.source_id < 0 then
        record.source_state = "out_of_range"
        battle_without_source = battle_without_source + 1
    else
        record.source_state = "pending"
        pending_sources = pending_sources + 1
    end
    push_record(record)
end

-- ---------------------------------------------------------------------------
-- the second source: entity_public, through wotb.players
-- ---------------------------------------------------------------------------

local function find_entity(snapshot, public_id)
    local visible = snapshot.visible_players
    if type(visible) ~= "table" then return nil end
    for index = 1, #visible do
        local value = visible[index]
        if type(value) == "table" and value.public_id == public_id then
            return value
        end
    end
    return nil
end

-- One attempt per record, made on the frame after the event arrived. Repeating
-- it every frame would be a per-frame native call for a name that is not
-- coming, and leaving the record "pending" forever would be a line that never
-- says what it knows.
local function apply_snapshot(snapshot, list)
    for index = 1, #list do
        local record = list[index]
        if record.source_state == "pending" then
            local entity = find_entity(snapshot, record.source_id)
            if entity == nil then
                record.source_state = "unseen"
            elseif entity.display_name_available == true and
                    type(entity.display_name) == "string" then
                record.source_state = "named"
                record.source_name = entity.display_name
            else
                record.source_state = "anonymous"
            end
            pending_sources = pending_sources - 1
            log_dirty = true
        end
    end
end

local function fail_pending(list, state)
    for index = 1, #list do
        local record = list[index]
        if record.source_state == "pending" then
            record.source_state = state
            pending_sources = pending_sources - 1
            log_dirty = true
        end
    end
end

-- Bounded on both sides. Source resolution is one attempt per record, and the
-- max-HP probe gives up after MAX_HEALTH_PROBE_ATTEMPTS tries, so a client that
-- never answers costs twelve calls in a battle rather than one per second
-- forever. Nothing here runs before the first damage event of a battle.
local function refresh_public_data(frame_index)
    if battle_hits == 0 then return end
    local want_sources = pending_sources > 0
    local want_max_health = max_health == nil and
        max_health_attempts < MAX_HEALTH_PROBE_ATTEMPTS and
        frame_index % MAX_HEALTH_PROBE_FRAMES == 0
    if not want_sources and not want_max_health then return end
    if want_max_health then max_health_attempts = max_health_attempts + 1 end

    if not wotb.available("entity_public") then
        max_health_note = "entity_public не опубликован"
        if want_sources then
            fail_pending(events, "no_api")
            fail_pending(frozen_events, "no_api")
        end
        return
    end

    local snapshot, snapshot_error = wotb.players.snapshot()
    if snapshot == nil then
        max_health_note = "снимок не прочитан"
        if want_sources then
            fail_pending(events, "failed")
            fail_pending(frozen_events, "failed")
        end
        return
    end

    if want_sources then
        apply_snapshot(snapshot, events)
        apply_snapshot(snapshot, frozen_events)
    end

    if max_health == nil then
        local local_player = snapshot.local_player
        if type(local_player) ~= "table" then
            max_health_note = "локальной машины нет в публичном снимке"
        else
            local value = tonumber(local_player.max_health)
            if value ~= nil and value > 0 then
                max_health = math.floor(value)
                log_dirty = true
            else
                max_health_note = "entity_public не сообщил макс. HP"
            end
        end
        if max_health == nil and
                max_health_attempts >= MAX_HEALTH_PROBE_ATTEMPTS then
            max_health_note = max_health_note .. ", опрос прекращён"
        end
    end
end

-- ---------------------------------------------------------------------------
-- text
-- ---------------------------------------------------------------------------

local function damage_text(record)
    if record.damage == nil then return "урон: " .. DASH end
    if record.damage > 0 then return string.format("-%d HP", record.damage) end
    return string.format("%d HP", record.damage)
end

local function reason_text(record)
    if record.reason_raw == nil then return "причина: нет в payload" end
    if math.type(record.reason_raw) ~= "integer" then
        return "причина: значение нечитаемо"
    end
    -- Printed as a raw code and never as a name: this ABI publishes no
    -- dictionary of damage reasons, so any word here would be invented.
    return string.format("причина: код %d", record.reason_raw)
end

local function source_text(record)
    local state = record.source_state
    if state == "absent" then return "источник: нет в payload" end
    if state == "unreadable" then return "источник: значение нечитаемо" end
    if state == "out_of_range" then return "источник: значение вне диапазона" end
    if state == "unset" then return "источник: не указан" end
    if state == "named" then
        return string.format("источник: %s (#%d)",
            record.source_name, record.source_id)
    end
    if state == "anonymous" then
        return string.format("источник: #%d, имя не публикуется",
            record.source_id)
    end
    if state == "unseen" then
        return string.format("источник: #%d, нет в публичном снимке",
            record.source_id)
    end
    if state == "no_api" then
        return string.format("источник: #%d, entity_public недоступен",
            record.source_id)
    end
    if state == "failed" then
        return string.format("источник: #%d, снимок не прочитан",
            record.source_id)
    end
    return string.format("источник: #%d, поиск не завершён", record.source_id)
end

local function event_text(record)
    if not record.typed then
        return string.format("#%d  ·  событие получено, payload не типизирован",
            record.index)
    end
    return string.format("#%d  ·  %s  ·  HP %s → %s  ·  %s  ·  %s",
        record.index, damage_text(record), amount(record.previous_health),
        amount(record.health), reason_text(record), source_text(record))
end

local function title_text()
    local head = paused and "ЖУРНАЛ УРОНА  ·  ПАУЗА" or "ЖУРНАЛ УРОНА"
    if battle_hits == 0 then
        return head .. "  ·  попаданий не зафиксировано"
    end
    local text = string.format("%s  ·  попаданий: %d  ·  получено: %d HP",
        head, battle_hits, battle_damage)
    if battle_damage_unknown > 0 then
        text = text .. string.format("  ·  без значения урона: %d",
            battle_damage_unknown)
    end
    return text
end

local function state_text()
    -- The newest record overall: while paused, new events land in
    -- frozen_events, and this row stays live even though the list is frozen.
    local newest = frozen_events[1] or events[1]
    local health = newest and newest.health or nil
    local left
    if health == nil then
        left = "HP после последнего попадания: " .. DASH
    else
        left = string.format("HP после последнего попадания: %d", health)
    end
    if max_health == nil then
        return left .. "  ·  макс. HP: " .. NO_DATA ..
            " (" .. max_health_note .. ")"
    end
    if health ~= nil then
        left = left .. string.format("  ·  осталось %.0f%% от макс.",
            health * 100.0 / max_health)
    end
    return left .. string.format("  ·  макс. HP: %d (entity_public)", max_health)
end

local function legend_text()
    if damage_subscribe_error ~= nil then
        return "ПОДПИСКА НА damage_received НЕ УДАЛАСЬ: " ..
            damage_subscribe_error
    end
    if lifecycle_subscribe_error ~= nil then
        return "ПОДПИСКА НА СОБЫТИЯ БОЯ НЕ УДАЛАСЬ: " ..
            lifecycle_subscribe_error .. " · счётчики могут не сбрасываться"
    end
    local count = battle_panel:errors()
    if type(count) == "number" and count > 0 then
        return string.format("%s  ·  UI-ошибок: %d", LEGEND, count)
    end
    return LEGEND
end

-- ---------------------------------------------------------------------------
-- rendering
-- ---------------------------------------------------------------------------

local function set_row(panel, index, text)
    local ok, err = panel:set_row(index, text)
    if not ok then log_error("set_row " .. index, err) end
end

local function set_row_visible(panel, index, visible)
    local ok, err = panel:set_row_visible(index, visible)
    if not ok then log_error("set_row_visible " .. index, err) end
end

local function render_battle()
    set_row(battle_panel, ROW_TITLE, title_text())
    set_row(battle_panel, ROW_STATE, state_text())
    if not paused then
        for offset = 1, MAX_EVENTS do
            local row = ROW_FIRST_EVENT + offset - 1
            local record = events[offset]
            if record == nil then
                set_row_visible(battle_panel, row, false)
            else
                set_row(battle_panel, row, event_text(record))
                set_row_visible(battle_panel, row, true)
            end
        end
    end
    set_row(battle_panel, ROW_LEGEND, legend_text())
    log_dirty = false
end

local function render_session_unavailable(reason)
    set_row(session_panel, SESSION_ROW_FIRST, "боёв записано: " .. NO_DATA)
    set_row(session_panel, SESSION_ROW_FIRST + 1,
        "попаданий и урона: " .. NO_DATA)
    set_row(session_panel, SESSION_ROW_FIRST + 2,
        "худшее попадание: " .. NO_DATA)
    set_row(session_panel, SESSION_ROW_FIRST + 3,
        "без источника в событии: " .. NO_DATA)
    set_row(session_panel, SESSION_ROW_LAST, "последний бой: " .. NO_DATA)
    set_row(session_panel, SESSION_ROW_FOOTER, reason)
end

local function render_session()
    session_dirty = false
    if store == nil then
        render_session_unavailable("Хранилище не открыто: " ..
            tostring(store_error))
        return
    end
    local values, origin, notes = store:all()
    if values == nil then
        render_session_unavailable("Хранилище не прочитано: " ..
            tostring(origin))
        return
    end

    set_row(session_panel, SESSION_ROW_FIRST,
        string.format("боёв записано: %d", values.battles))
    set_row(session_panel, SESSION_ROW_FIRST + 1,
        string.format("попаданий: %d  ·  получено: %d HP",
            values.hits, values.damage))
    set_row(session_panel, SESSION_ROW_FIRST + 2,
        string.format("худшее попадание: %d HP", values.worst_hit))
    set_row(session_panel, SESSION_ROW_FIRST + 3,
        string.format("без источника в событии: %d из %d попаданий",
            values.hits_without_source, values.hits))
    set_row(session_panel, SESSION_ROW_LAST,
        string.format("последний бой: %d попаданий  ·  %d HP",
            values.last_battle_hits, values.last_battle_damage))

    local footer
    if persist_error ~= nil then
        footer = "Последняя запись не удалась: " .. persist_error
    else
        local stored = false
        for _, source in next, origin do
            if source == "stored" then stored = true end
        end
        local noted = false
        for _ in next, notes do noted = true end
        if not stored then
            footer = "Сохранённых итогов ещё нет: показаны нули по умолчанию."
        elseif noted then
            footer = "Часть значений в хранилище отвергнута по типу; " ..
                "для них показан ноль по умолчанию."
        else
            footer = "Итоги в wotb.storage, обновляются в конце каждого боя."
        end
    end
    set_row(session_panel, SESSION_ROW_FOOTER, footer)
end

-- ---------------------------------------------------------------------------
-- panels
-- ---------------------------------------------------------------------------

-- Deliberately does not log. A panel error can recur on every probe cadence,
-- and a mod that writes a log line at a frame cadence is the shipped FPS bug in
-- a new hat. The count is surfaced on the legend row instead, which is why
-- log_dirty is set here.
local function panel_error(scope, message)
    log_dirty = true
end

local function build_battle_panel()
    local rows = {}
    rows[ROW_TITLE] = {text = "ЖУРНАЛ УРОНА", height = 28, size = 21,
                       width = 600, color = COLORS.amber}
    rows[ROW_STATE] = {text = "", height = 24, size = 17, color = COLORS.muted}
    for offset = 1, MAX_EVENTS do
        rows[ROW_FIRST_EVENT + offset - 1] =
            {text = "", height = 24, size = 16, color = COLORS.white,
             visible = false}
    end
    rows[ROW_LEGEND] = {text = LEGEND, height = 22, size = 15,
                        color = COLORS.faint}

    local created, err = wotb.panel.new({
        id = "damage_log",
        width = 780, height = 336,
        anchor = "top-left",
        margin = {left = 128, top = 118},
        padding = 18,
        rows_top = 12,
        row_gap = 4,
        rows = rows,
        background = COLORS.panel,
        buttons = {
            {id = "pause", text = "ПАУЗА", x = 652, y = 10,
             width = 110, height = 28, size = 16,
             color = COLORS.white, background = COLORS.blue,
             on_click = function(self)
                 paused = not paused
                 if not paused then
                     for index = #frozen_events, 1, -1 do
                         table.insert(events, 1, frozen_events[index])
                         frozen_events[index] = nil
                     end
                     while #events > MAX_EVENTS do table.remove(events) end
                 end
                 local ok, button_error = self:set_button_text("pause",
                     paused and "ПРОДОЛЖИТЬ" or "ПАУЗА")
                 if not ok then log_error("set_button_text", button_error) end
                 log_dirty = true
             end},
        },
        on_error = panel_error,
    })
    if created == nil then
        log_error("panel.new battle", err)
        return false
    end
    battle_panel = created
    return true
end

local function build_session_panel()
    local rows = {}
    rows[SESSION_ROW_TITLE] = {text = "ЖУРНАЛ УРОНА  ·  ИТОГИ", height = 28,
                               size = 21, color = COLORS.amber}
    for index = SESSION_ROW_FIRST, SESSION_ROW_LAST do
        rows[index] = {text = "", height = 24, size = 17, color = COLORS.white}
    end
    rows[SESSION_ROW_FOOTER] = {text = "", height = 22, size = 15,
                                color = COLORS.faint}

    local created, err = wotb.panel.new({
        id = "damage_sum",
        width = 560, height = 234,
        anchor = "top-right",
        margin = {right = 28, top = 110},
        padding = 18,
        rows_top = 12,
        row_gap = 4,
        rows = rows,
        background = COLORS.panel,
        contexts = {
            visible = wotb.context.HANGAR,
            blocked = wotb.context.MOD_SCREEN + wotb.context.TEXT_INPUT +
                wotb.context.RESULTS,
        },
        on_error = panel_error,
    })
    if created == nil then
        log_error("panel.new session", err)
        return false
    end
    session_panel = created
    return true
end

-- ---------------------------------------------------------------------------
-- subscriptions
-- ---------------------------------------------------------------------------

local function subscribe(topic, callback)
    if type(topic) ~= "string" then
        return nil, "этот host не публикует такую константу topic"
    end
    local token, err = wotb.events.subscribe(
        topic, callback, wotb.events.PRIORITY_NORMAL, true)
    if token == nil then return nil, tostring(err) end
    subscriptions[#subscriptions + 1] = token
    return true
end

local function subscribe_all()
    local ok, err = subscribe(wotb.events.TOPIC_DAMAGE_RECEIVED, on_damage)
    if not ok then
        damage_subscribe_error = err
        log_error("subscribe damage_received", err)
    end

    local lifecycle = {
        {wotb.events.TOPIC_BATTLE_ENTERED, function() reset_battle() end},
        {wotb.events.TOPIC_BATTLE_ENDED, function() commit_battle() end},
        {wotb.events.TOPIC_BATTLE_LEFT, function() commit_battle() end},
    }
    for index = 1, #lifecycle do
        local subscribed, lifecycle_error =
            subscribe(lifecycle[index][1], lifecycle[index][2])
        if not subscribed and lifecycle_subscribe_error == nil then
            lifecycle_subscribe_error = lifecycle_error
            log_error("subscribe battle lifecycle", lifecycle_error)
        end
    end
end

local function unsubscribe_all()
    for index = #subscriptions, 1, -1 do
        local ok, err = wotb.events.unsubscribe(subscriptions[index])
        if not ok then log_error("unsubscribe", err) end
        subscriptions[index] = nil
    end
end

-- ---------------------------------------------------------------------------
-- lifecycle
-- ---------------------------------------------------------------------------

function on_enable()
    wotb.log.set_category("example.lua_damage_log")
    unsubscribe_all()
    reset_battle()
    paused = false
    session_dirty = true
    session_showing = false
    persist_error = nil
    damage_subscribe_error = nil
    lifecycle_subscribe_error = nil

    if not wotb.available("events") then
        damage_subscribe_error = "wotb.events не опубликован этим клиентом"
        log_error("events", damage_subscribe_error)
    else
        subscribe_all()
    end

    open_store()
    if store == nil then log_error("config.new", store_error) end

    if not build_battle_panel() then return end
    if not build_session_panel() then return end
    wotb.log.info("включён; журнал урона ведётся только по damage_received")
end

function on_frame(frame_index, delta_seconds)
    if math.type(frame_index) ~= "integer" then return end

    -- Outside the panel's visibility gate on purpose: naming a source is a
    -- data question, and an entity that is visible now may be gone by the time
    -- the player closes the mod catalog.
    refresh_public_data(frame_index)

    if battle_panel ~= nil and battle_panel:update(frame_index) then
        if log_dirty then render_battle() end
    end

    if session_panel ~= nil then
        local showing = session_panel:update(frame_index)
        if showing and not session_showing then session_dirty = true end
        session_showing = showing == true
        if showing and session_dirty then render_session() end
    end
end

function on_disable()
    unsubscribe_all()
    if battle_panel ~= nil then
        battle_panel:unmount()
        battle_panel = nil
    end
    if session_panel ~= nil then
        session_panel:unmount()
        session_panel = nil
    end
    events = {}
    frozen_events = {}
    pending_sources = 0
    store = nil
    session_showing = false
    wotb.log.info("выключен")
end
