-- ---------------------------------------------------------------------------
-- Lua Reload Timer - reload countdown and ammunition panel
-- ---------------------------------------------------------------------------
--
-- WHERE EVERY NUMBER ON SCREEN COMES FROM
--
--   * The countdown is INTERPOLATED between events, never polled. The client
--     publishes wotbmod.gameplay.reload_state_changed from ReloadTimer::setState
--     - on transitions, not once per frame - and the payload carries
--     duration_seconds, progress and remaining_seconds. wotb.battle stores the
--     publishing event's own timestamp_ns next to each field, so what is drawn
--     is `remaining at the event - (now - that event's timestamp)`. The client
--     is asked nothing at all between events.
--   * `now` is the timestamp of the most recent wotbmod.frame.update, read
--     through wotb.timer.now_ms(). Both stamps are produced by the same steady
--     clock (src/v3/runtime_services.cpp, NowNs()), so the subtraction means
--     something. on_frame's delta_seconds is deliberately unused: it is a
--     duration, not a clock, and cannot be differenced against an event stamp.
--   * Shell counts are not invented. On this client build the ammo payload
--     carries count = -1, a sentinel (loader/wotb_mod_loader.cpp,
--     AmmoChangedDetour), so no count has ever been observed and every count
--     reads as a dash with the reason spelled out on the panel. The day a build
--     publishes a real count, these same rows fill in with no code change.
--   * Reload state that has never been observed - before the first shot of a
--     battle, or outside battle - reads "НЕТ ДАННЫХ". Never 0.0, never 100 %.
--
-- PER-FRAME COST
--
-- Measured, not estimated: the figures below come from driving this file
-- against the real convenience preludes with a mock client, 600 frames per
-- case, with the collector stopped so "bytes" means allocated rather than
-- not-yet-freed, and with the same pump under an empty on_frame differenced
-- out. See the harness note in README_RU.md.
--
--   on_frame, panel showing, nothing due:
--     panel:update(frame_index)   one integer modulo and two field reads
--     one boolean test            `if due then`
--     -> nothing allocated, no string built, no ABI crossing, no log line.
--   on_frame, panel not showing (hangar, mod catalog, text input):
--     0.4 KB per 1000 frames, which is measurement noise around zero. The
--     redraw timer is cancelled, and the last timer going away takes
--     wotb.timer's wotbmod.frame.update subscription with it, so outside
--     battle the client's event dispatcher does not look at this mod at all.
--   wotb.timer while the panel is showing:
--     the module's own idle cost - one Lua call, one integer read, one compare
--     per frame - plus this mod's callback, which assigns one boolean, ten
--     times a second.
--   every 100 ms while showing:
--     one wotb.battle.snapshot(), the interpolation arithmetic, and a
--     string.format only for a row whose displayed value actually changed.
--     Measured at about 2.5 KB of garbage per tick, of which roughly 2.4 KB is
--     wotb.battle.snapshot() rebuilding its values/source/updated_ns/
--     unavailable tables; this mod's own formatting is the remaining ~0.1 KB.
--     That is ~25 KB/s while a panel is on screen in battle, and zero the rest
--     of the time.
--   row writes reaching the client:
--     0 per second while READY, unknown or paused - the rows do not change,
--     and set_row compares before it crosses the ABI.
--     Up to 20 per second while actively counting down: the countdown row at
--     the 10 Hz tick rate, plus the share row whenever the integer percentage
--     changes. Measured at 13.6/s for a 25 s cycle, which is the 10 Hz
--     countdown plus 3.6 Hz of percentage steps.
--
--   The 100 ms cadence is deliberate. A tenth of a second is the finest step
--   this panel displays, so redrawing more often than that cannot change a
--   single glyph; and a text mutation is not free here - the loader marks the
--   control rebuild_pending (loader/v3_native_client_services.cpp,
--   QueueUpdatedUiRebuild) and the backing DAVA resource is rebuilt after it.
--   Twenty changed rows a second is a budget. Sixty is the shipped FPS
--   collapse this codebase has already paid for once.

-- ---- tuning ---------------------------------------------------------------
--
-- Everything a player may reasonably want to move lives here. There is no
-- persisted configuration on purpose: a settings store would cost this mod a
-- `storage` or `settings` permission it has no other use for.

local LOG_CATEGORY = "example.lua_reload_timer"

-- Redraw cadence, milliseconds. See the note above before lowering it.
local REDRAW_MS = 100

-- How long a refused wotb.timer.every waits before being asked again, in
-- frames. A client that is refusing must not be asked sixty times a second.
local TIMER_RETRY_FRAMES = 120

-- Geometry. The panel sits against the right edge, a third of the way down:
-- clear of the reticle at the centre of the screen, clear of the team lists at
-- the top and clear of the bottom strip. Move it by editing these four values.
local PANEL_WIDTH = 312
local PANEL_HEIGHT = 172
local PANEL_ANCHOR = "top-right"
local PANEL_MARGIN = {left = 0, right = 28, top = 300, bottom = 0}

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.68, g = 0.75, b = 0.82, a = 1.00},
    faint = {r = 0.45, g = 0.52, b = 0.59, a = 1.00},
    panel = {r = 0.025, g = 0.045, b = 0.065, a = 0.92},
    ready = {r = 0.25, g = 0.80, b = 0.36, a = 1.00},
    paused = {r = 0.96, g = 0.66, b = 0.14, a = 1.00},
}

-- ---- rows -----------------------------------------------------------------

-- Row 1 is the static "ПЕРЕЗАРЯДКА" header and is never written again, so it
-- has no constant here; every row below it is.
local ROW_VALUE = 2
local ROW_SHARE = 3
local ROW_SHELL = 4
local ROW_OTHER = 5
local ROW_SOURCE = 6

-- wotb.panel names its text controls "row1".."rowN". Spelled out rather than
-- concatenated so the per-tick path never builds this string.
local ROW_VALUE_CONTROL = "row2"

-- The one dash this mod uses for "not observed", and the long form for the big
-- row where there is space to say it in words.
local DASH = "—"
local NO_DATA = "НЕТ ДАННЫХ"

-- Provenance labels. Interned constants, so the change test below is a pointer
-- compare rather than a string build.
local MODE_INTERPOLATED = "интерполяция"
local MODE_PAUSED = "пауза"
local MODE_READY_CALC = "готовность рассчитана"
local MODE_OBSERVED = "значение из события"
local MODE_NO_CLOCK = "нет часов кадра"
local MODE_UNKNOWN = "перезарядка не наблюдалась"
local MODE_UNUSABLE = "клиент прислал непригодное значение"

local STATE_COLORS = {
    ready = COLORS.ready,
    paused = COLORS.paused,
    reloading = COLORS.white,
    unknown = COLORS.muted,
}

-- Progress bar. Both strings are pure ASCII and exactly BAR_CELLS bytes long,
-- so string.sub cuts them by cell and never through a multi-byte character.
local BAR_CELLS = 8
local BAR_FULL = "########"
local BAR_VOID = "........"

-- ---- state ----------------------------------------------------------------

local panel = nil
local timer_id = nil
local timer_retry_frame = 0
local showing = false
local due = true

local error_count = 0
local last_error_scope = nil
local last_error_message = nil

-- Ammunition, as observed. shell_counts is only ever written from a count the
-- client actually published; a shell type with no entry reads as a dash.
local shell_counts = {}
local shell_seen = {}
local shell_order = {}
local counts_ever_observed = false
local ammo_stamp = nil
local ammo_shell = nil
local ammo_dirty = true

-- Displayed-value keys. A row is rebuilt only when its own key changes.
local shown_class = nil
local shown_tenths = nil
local shown_percent = nil
local shown_duration = nil
local shown_mode = nil
local shown_age = nil
local applied_color_class = nil

-- ---- error handling -------------------------------------------------------
--
-- Every API call in this file reads its second return. Failures that have a
-- caller are returned; failures this mod causes on its own initiative land
-- here. They are counted always and logged only when the message changes, so a
-- client that refuses the same call every tick costs one log line, not ten a
-- second.

-- tostring can itself raise through a __tostring metamethod, and reporting an
-- error must never be the thing that kills a frame.
local function describe(detail)
    local ok, text = pcall(tostring, detail)
    if ok then return text end
    return "<an error whose __tostring raised>"
end

local function report(scope, message)
    error_count = error_count + 1
    if scope == last_error_scope and message == last_error_message then
        return
    end
    last_error_scope = scope
    last_error_message = message
    local logged = wotb.log.warn(scope .. ": " .. describe(message))
    if not logged then
        -- There is nowhere left to report to: wotb.log is what a failure of
        -- wotb.log would have to be reported through. The counter above still
        -- moved, and forgetting the de-duplication key means a client that
        -- starts accepting log lines later still gets this one.
        last_error_scope = nil
        last_error_message = nil
    end
end

local function put(index, text)
    local ok, why = panel:set_row(index, text)
    if not ok then report("panel.set_row", why) end
end

local function put_visible(index, on)
    local ok, why = panel:set_row_visible(index, on)
    if not ok then report("panel.set_row_visible", why) end
end

-- ---- formatting -----------------------------------------------------------

local function bar(share)
    local filled = math.floor(share * BAR_CELLS + 0.5)
    if filled < 0 then filled = 0 end
    if filled > BAR_CELLS then filled = BAR_CELLS end
    return string.sub(BAR_FULL, 1, filled) ..
        string.sub(BAR_VOID, 1, BAR_CELLS - filled)
end

local function source_text(mode, age_whole)
    if age_whole == nil then
        return mode
    end
    if age_whole <= 0 then
        return mode .. "  ·  событие только что"
    end
    return string.format("%s  ·  событие %d с назад", mode, age_whole)
end

-- ---- ammunition -----------------------------------------------------------

local function remember_shell(id)
    if math.type(id) ~= "integer" or shell_seen[id] then return end
    shell_seen[id] = true
    shell_order[#shell_order + 1] = id
end

local function count_text(id)
    local value = shell_counts[id]
    if value == nil then return DASH end
    return tostring(value)
end

-- Reads the ammo group out of the snapshot. The comparison is against the
-- publishing event's timestamp AND the shell id, because an event delivered
-- without a usable timestamp still carries a real observation.
local function refresh_ammo(snap)
    local stamp = snap.updated_ns.shell_id
    local shell = snap.shell_id
    if stamp == ammo_stamp and shell == ammo_shell then return end
    ammo_stamp = stamp
    ammo_shell = shell
    ammo_dirty = true

    -- The battle module clears every per-battle observation on battle.entered
    -- and battle.left, and this table is derived from those observations. A
    -- count remembered against shell 2 of the previous tank is not a fact about
    -- this one, so the derived memory goes when the observation it came from
    -- does.
    if shell == nil and #shell_order > 0 then
        shell_counts = {}
        shell_seen = {}
        shell_order = {}
        counts_ever_observed = false
    end

    remember_shell(snap.previous_shell_id)
    remember_shell(shell)

    -- The only place a count is ever written, and it demands a non-negative
    -- integer. WotbModV3AmmoEventData.count is int32 and this build always
    -- sends -1, which is a sentinel for "unknown" and not a quantity.
    local count = snap.ammo_count
    if math.type(shell) == "integer" and math.type(count) == "integer" and
            count >= 0 then
        shell_counts[shell] = count
        counts_ever_observed = true
    end
end

local function render_ammo(snap)
    if not ammo_dirty then return end
    ammo_dirty = false

    local current = snap.shell_id
    if math.type(current) ~= "integer" then
        put(ROW_SHELL, "СНАРЯД " .. DASH .. "  ·  запас " .. DASH)
    else
        put(ROW_SHELL, string.format("СНАРЯД №%d  ·  запас %s",
            current, count_text(current)))
    end

    if not counts_ever_observed then
        -- Answers the question the dash above raises, in place, instead of
        -- listing every other shell type against the same dash.
        put(ROW_OTHER, "запас снарядов клиент не публикует")
        put_visible(ROW_OTHER, true)
        return
    end

    local others = nil
    for index = 1, #shell_order do
        local id = shell_order[index]
        if id ~= current then
            local piece = string.format("№%d %s", id, count_text(id))
            others = others and (others .. "  ·  " .. piece) or piece
        end
    end
    if others == nil then
        put_visible(ROW_OTHER, false)
    else
        put(ROW_OTHER, "прочие:  " .. others)
        put_visible(ROW_OTHER, true)
    end
end

-- ---- reload ---------------------------------------------------------------

local function apply_color(class)
    if class == applied_color_class then return end
    local control, control_error = panel:control(ROW_VALUE_CONTROL)
    if control == nil then
        report("panel.control", control_error)
        return
    end
    -- Resolved at call time and by name. wotb.ui exists on every client because
    -- its constants are installed unconditionally, but a slot is a function
    -- only where the client published one, and calling a nil is a raise rather
    -- than a value.
    local api = wotb.ui
    local set_color = type(api) == "table" and api.control_set_color or nil
    if type(set_color) ~= "function" then
        -- This client published no colour slot. The row keeps the colour it was
        -- created with, which is legible; that is a smaller loss than a log
        -- line on every state change, so it is recorded and not reported.
        applied_color_class = class
        return
    end
    local ok, color_error = set_color(control, STATE_COLORS[class])
    if not ok then
        report("ui.control_set_color", color_error)
        return
    end
    applied_color_class = class
end

local function render_unknown(reason)
    if shown_class ~= "unknown" or shown_mode ~= reason then
        shown_class = "unknown"
        shown_tenths = nil
        shown_percent = nil
        shown_duration = nil
        shown_mode = reason
        shown_age = nil
        put(ROW_VALUE, NO_DATA)
        put(ROW_SHARE, "доля " .. DASH)
        put(ROW_SOURCE, reason)
    end
    apply_color("unknown")
end

-- A float from a client payload is not automatically a number this panel can
-- print: a nan compares false against everything, and string.format("%.1f")
-- would happily write "-nan" into a row. A value outside a plausible reload is
-- refused for the same reason - saying so is better than showing it.
local function usable_seconds(value)
    if type(value) ~= "number" then return nil end
    if value ~= value then return nil end
    if value < 0 or value > 3600 then return nil end
    return value
end

local function render_reload(snap)
    local raw_remaining = snap.reload_remaining_seconds
    if raw_remaining == nil then
        -- Not zero and not "100 % ready": nothing has been observed. The reason
        -- string the battle module attaches is English and belongs in the log,
        -- not on a player's screen, so the panel says it in one Russian line.
        render_unknown(MODE_UNKNOWN)
        return
    end
    local remaining_at_event = usable_seconds(raw_remaining)
    if remaining_at_event == nil then
        render_unknown(MODE_UNUSABLE)
        return
    end

    local anchor_ns = snap.updated_ns.reload_remaining_seconds
    local paused = snap.reload_paused == true
    local duration = usable_seconds(snap.reload_duration_seconds)
    if duration ~= nil and duration <= 0 then duration = nil end

    -- The interpolation, and the only arithmetic on this path. now_ms is the
    -- last frame event's timestamp; anchor_ns is the reload event's. A negative
    -- age means the reload event was published after the last frame was
    -- stamped, which is ordinary and means zero elapsed, not a negative one.
    local age = nil
    local now_ms = wotb.timer.now_ms()
    if math.type(anchor_ns) == "integer" and type(now_ms) == "number" then
        age = now_ms * 0.001 - anchor_ns * 1e-9
        if age < 0 then age = 0 end
    end

    local elapsed = 0.0
    if age ~= nil and not paused then elapsed = age end
    local remaining = remaining_at_event - elapsed
    if remaining < 0 then remaining = 0 end

    local class
    local mode
    if paused then
        class = "paused"
        mode = MODE_PAUSED
    elseif age == nil then
        -- No clock to interpolate against, so the last observed value is shown
        -- exactly as it was observed and the panel says so rather than letting
        -- a frozen number look live.
        class = remaining_at_event <= 0 and "ready" or "reloading"
        mode = MODE_NO_CLOCK
    elseif remaining_at_event <= 0 then
        class = "ready"
        mode = MODE_OBSERVED
    elseif remaining <= 0 then
        -- The countdown ran out and no new event has arrived. READY here is
        -- computed from the client's own duration, not observed, and the source
        -- row says which of the two it is.
        class = "ready"
        mode = MODE_READY_CALC
    else
        class = "reloading"
        mode = MODE_INTERPOLATED
    end

    -- Displayed value, and the key it is a pure function of.
    local tenths = math.floor(remaining * 10.0 + 0.5)
    if class ~= shown_class or tenths ~= shown_tenths then
        shown_class = class
        shown_tenths = tenths
        if class == "ready" then
            put(ROW_VALUE, "ГОТОВ")
        elseif class == "paused" then
            put(ROW_VALUE, string.format("%.1f с  ПАУЗА", tenths / 10.0))
        else
            put(ROW_VALUE, string.format("%.1f с", tenths / 10.0))
        end
    end
    apply_color(class)

    -- The share. Derived from the same interpolated remaining when the client
    -- gave a cycle length; otherwise the progress the event carried, marked as
    -- an observation rather than a live value.
    if duration ~= nil then
        local share = 1.0 - remaining / duration
        if share < 0 then share = 0 end
        if share > 1 then share = 1 end
        local percent = math.floor(share * 100.0 + 0.5)
        if percent ~= shown_percent or duration ~= shown_duration then
            shown_percent = percent
            shown_duration = duration
            put(ROW_SHARE, string.format("[%s]  %d %%  ·  цикл %.1f с",
                bar(share), percent, duration))
        end
    else
        local observed = snap.reload_progress
        local percent = nil
        if type(observed) == "number" and observed == observed and
                observed >= 0 and observed <= 1 then
            percent = math.floor(observed * 100.0 + 0.5)
        end
        if percent ~= shown_percent or shown_duration ~= nil then
            shown_percent = percent
            shown_duration = nil
            if percent == nil then
                put(ROW_SHARE, "доля " .. DASH)
            else
                put(ROW_SHARE, string.format("доля %d %% (из события)", percent))
            end
        end
    end

    -- Provenance. The age is quantised to whole seconds - this row is a
    -- statement about where the number came from, not a second countdown, and
    -- a row that moves a decimal ten times a second is ten rebuilds a second -
    -- and it is only shown while the value is actually being interpolated. On
    -- a ready or paused panel the age would otherwise keep ticking upward
    -- forever, rebuilding a control once a second to say nothing new.
    local age_whole = nil
    if age ~= nil and class == "reloading" then age_whole = math.floor(age) end
    if mode ~= shown_mode or age_whole ~= shown_age then
        shown_mode = mode
        shown_age = age_whole
        put(ROW_SOURCE, source_text(mode, age_whole))
    end
end

local function render()
    local snap, snapshot_error = wotb.battle.snapshot()
    if snap == nil then
        render_unknown("состояние боя недоступно")
        report("battle.snapshot", snapshot_error)
        return
    end
    refresh_ammo(snap)
    render_ammo(snap)
    render_reload(snap)
end

-- ---- redraw timer ---------------------------------------------------------
--
-- The callback assigns one boolean and returns. Everything that touches a
-- control happens in on_frame, on the frame thread, exactly as the shipped
-- panels do: nothing is created, destroyed or mutated inside the client's own
-- event dispatch.

local function mark_due()
    due = true
end

local function start_timer(frame_index)
    if timer_id ~= nil or frame_index < timer_retry_frame then return end
    local id, timer_error = wotb.timer.every(REDRAW_MS, mark_due)
    if id == nil then
        report("timer.every", timer_error)
        timer_retry_frame = frame_index + TIMER_RETRY_FRAMES
        return
    end
    timer_id = id
end

local function stop_timer()
    if timer_id == nil then return end
    local id = timer_id
    timer_id = nil
    local ok, cancel_error = wotb.timer.cancel(id)
    if not ok then report("timer.cancel", cancel_error) end
end

-- Everything the panel is showing is now stale: a fresh mount created fresh
-- controls, whose text is the spec's and whose colour is the spec's.
local function invalidate()
    shown_class = nil
    shown_tenths = nil
    shown_percent = nil
    shown_duration = nil
    shown_mode = nil
    shown_age = nil
    applied_color_class = nil
    ammo_dirty = true
    due = true
end

local function reset_observations()
    shell_counts = {}
    shell_seen = {}
    shell_order = {}
    counts_ever_observed = false
    ammo_stamp = nil
    ammo_shell = nil
    invalidate()
end

-- ---- lifecycle ------------------------------------------------------------

function on_enable()
    -- Read before it is used: report() below logs through wotb.log, and a
    -- refused set_category would otherwise silently put this mod's lines in the
    -- shared "lua" channel with every other script's.
    local named, name_error = wotb.log.set_category(LOG_CATEGORY)

    -- A previous enable in this same lua_State must leave nothing behind.
    stop_timer()
    if panel ~= nil then
        local unmounted, unmount_error = panel:unmount()
        if not unmounted then report("panel.unmount", unmount_error) end
    end
    panel = nil
    showing = false
    error_count = 0
    last_error_scope = nil
    last_error_message = nil
    timer_retry_frame = 0
    reset_observations()

    if not named then report("log.set_category", name_error) end

    -- Published, not permitted: this only says whether the client answered
    -- query_interface. A grant refusal still arrives as a value from the call,
    -- and every call below reads it.
    if not wotb.available("ui") then
        report("startup", "wotb.ui не опубликован этим клиентом")
    end
    if not wotb.available("events") then
        report("startup", "wotb.events не опубликован этим клиентом")
    end

    local built, build_error = wotb.panel.new({
        id = "reload_timer",
        width = PANEL_WIDTH,
        height = PANEL_HEIGHT,
        anchor = PANEL_ANCHOR,
        margin = PANEL_MARGIN,
        padding = 14,
        background = COLORS.panel,
        color = COLORS.white,
        font_size = 16,
        -- The default gate is exactly right for a battle panel: visible in
        -- BATTLE, TRAINING and REPLAY, blocked by MOD_SCREEN and TEXT_INPUT,
        -- with the masks read from wotb.context by name.
        rows = {
            {text = "ПЕРЕЗАРЯДКА", x = 14, y = 10, width = 284, height = 20,
             size = 15, color = COLORS.muted},
            {text = NO_DATA, x = 14, y = 30, width = 284, height = 46,
             size = 38, color = COLORS.muted},
            {text = "доля " .. DASH, x = 14, y = 78, width = 284, height = 22,
             size = 16, color = COLORS.muted},
            {text = "СНАРЯД " .. DASH .. "  ·  запас " .. DASH,
             x = 14, y = 102, width = 284, height = 23,
             size = 17, color = COLORS.white},
            {text = "", x = 14, y = 125, width = 284, height = 20,
             size = 14, color = COLORS.faint, visible = false},
            {text = MODE_UNKNOWN, x = 14, y = 146, width = 284, height = 18,
             size = 13, color = COLORS.faint},
        },
        on_error = function(scope, message)
            report("panel." .. scope, message)
        end,
    })
    if built == nil then
        -- panel.new only validates and computes; it reads no interface and
        -- needs no permission, so a failure here is this file's spec being
        -- wrong and not something a retry can fix.
        report("panel.new", build_error)
        return
    end
    panel = built

    -- Only the three groups this mod reads. `lifecycle` is not decoration: the
    -- battle module clears its per-battle observations inside the handlers for
    -- battle.entered and battle.left, and those handlers exist only for topics
    -- it actually subscribed to. Without this group, last battle's reload would
    -- still be on screen at the start of the next one.
    local started, start_error =
        wotb.battle.start({"reload", "ammo", "lifecycle"})
    if not started then report("battle.start", start_error) end
end

-- delta_seconds is intentionally not taken: this mod's clock is the frame
-- event's timestamp, read through wotb.timer.now_ms(), because that is the
-- clock the reload event is stamped with.
function on_frame(frame_index)
    if panel == nil then return end

    local visible = panel:update(frame_index)
    if visible == nil then
        report("panel.update", "on_frame did not supply an integer frame index")
        return
    end
    if visible ~= showing then
        showing = visible
        -- A remount built new controls, so every cached row key and the colour
        -- override describe controls that no longer exist.
        if visible then invalidate() end
    end

    if not showing then
        -- The last timer going away takes wotb.timer's frame subscription with
        -- it, so outside battle this mod is not in the dispatcher at all.
        stop_timer()
        return
    end

    start_timer(frame_index)
    if due then
        due = false
        render()
    end
end

function on_disable()
    stop_timer()
    if panel ~= nil then
        local ok, unmount_error = panel:unmount()
        if not ok then report("panel.unmount", unmount_error) end
        panel = nil
    end
    -- Drops every subscription the battle module opened on this mod's behalf.
    -- It answers `true` unconditionally today; the second return is read anyway
    -- so that a version which can fail is not silently ignored here.
    local stopped, stop_error = wotb.battle.stop()
    if not stopped then report("battle.stop", stop_error) end
    showing = false
    reset_observations()
end
