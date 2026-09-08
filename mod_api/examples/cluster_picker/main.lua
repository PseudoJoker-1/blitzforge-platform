-- blitzforge.cluster_picker - the login cluster row in the stock settings.
--
-- The resource package blitzforge.cluster_picker.ui adds a hidden row
-- (bf_cluster_row: bf_cluster_label + bf_cluster_auto + bf_cluster_0..3) to
-- UI/Screens/Common/Settings/PrimaryTab/PrimaryOptionsPage.yaml. This script
-- fills the row from wotb.session.clusters() when the settings screen opens,
-- shows it, and turns a click into wotb.session.change_cluster() after a
-- confirmation popup. Everything is event-driven: no on_frame.
--
-- The settings page is not a screen of its own: the client builds it inside
-- the hangar screen, so wotbmod.ui.screen_changed never fires for it (live
-- 11.20.0.887). The row is therefore looked for after every pointer release
-- (wotbmod.ui.input, action 3) as well - one find() call, throttled.

local log = wotb.log
log.set_category("cluster_picker")

local TEXT = {
  ru = {
    label = "Кластер:", auto = "AUTO",
    confirm = "Переподключиться к %s?", accept = "Переподключить", cancel = "Отмена",
    connected = "Сервер: %s", failed = "Не удалось переключиться, следующий вход — автовыбор",
    started = "Подключение к %s...",
  },
  en = {
    label = "Cluster:", auto = "AUTO",
    confirm = "Reconnect to %s?", accept = "Reconnect", cancel = "Cancel",
    connected = "Server: %s", failed = "Switch failed, the next login auto-selects",
    started = "Connecting to %s...",
  },
}

local function texts()
  local lang = wotb.store.get("language")
  if lang == nil and wotb.settings and wotb.settings.get_string then
    lang = wotb.settings.get_string("language")
  end
  if type(lang) ~= "string" then lang = "ru" end
  return TEXT[string.sub(lang, 1, 2)] or TEXT.ru
end

-- Button name -> cluster id. The captions are static in the yaml (AUTO,
-- C0..C4: the union of the EU/NA/SG catalogues); buttons whose cluster this
-- account does not see are hidden, the current one is disabled.
local BUTTONS = {
  bf_cluster_auto = -1,
  bf_cluster_0 = 0, bf_cluster_1 = 1, bf_cluster_2 = 2, bf_cluster_3 = 3, bf_cluster_4 = 4,
}

local buttons = {}        -- name -> { control, cluster_id }
local ui_tokens = {}      -- name -> wotb.ui.event_subscribe token
local input_token = nil   -- wotb.events token for the pointer fallback
local screen_token = nil
local pointer_token = nil -- wotb.events token for the settings-page probe
local subscribed_clicks = false
local probe_pending = false
DEBUG_PROBES = 0  -- set to 8 to log the first probes, rects and pointer hits
local row_bound = false

local function name_of(cluster_id)
  if cluster_id == -1 then return texts().auto end
  for _, item in ipairs(wotb.session.clusters() or {}) do
    if item.id == cluster_id then return item.name end
  end
  return "#" .. tostring(cluster_id)
end

-- The confirmation is a panel of the mod's own (wotb.panel): the client's
-- dialog slot (ui.confirm_show) is not published on 11.20, and a stock
-- dialog could not carry the mod's handlers anyway.
local confirm = nil
local function close_confirm()
  if confirm ~= nil then
    local panel = confirm
    confirm = nil
    pcall(panel.destroy, panel)
  end
end

local function on_click(entry)
  local t = texts()
  local target = name_of(entry.cluster_id)
  close_confirm()
  local panel, why = wotb.panel.new({
    id = "bf_cluster_confirm", width = 440, height = 160, anchor = "center",
    padding = 16, font_size = 20, contexts = false,
    rows = { { text = string.format(t.confirm, target), height = 36 } },
    buttons = {
      { id = "accept", text = t.accept, x = 24, y = 96, width = 220, height = 44,
        on_click = function()
          close_confirm()
          local ok, err = wotb.session.change_cluster(entry.cluster_id)
          if not ok then
            log.warn("change_cluster(%s) refused: %s", tostring(entry.cluster_id), tostring(err))
            wotb.screen.notify(tostring(err), 5)
          end
        end },
      { id = "cancel", text = t.cancel, x = 260, y = 96, width = 156, height = 44,
        on_click = function() close_confirm() end },
    },
  })
  if panel == nil then
    log.warn("confirm panel: %s", tostring(why))
    return
  end
  local ok, mount_error = panel:mount()
  if not ok then
    log.warn("confirm panel mount: %s", tostring(mount_error))
    return
  end
  confirm = panel
  log.info("confirm shown for %s", target)
end

-- A control's rect on screen: its own (live) rect plus every ancestor's
-- position. The runtime's own click hit-test only knows the parents it has
-- wrapped, which for a stock control found from the screen root is the root
-- alone - so a game-owned button never matches there and the click has to
-- be resolved here.
local function absolute_rect(control)
  local r = wotb.screen.rect(control)
  if not r then return nil end
  local x, y = r.x, r.y
  local node = control
  local borrowed = {}
  for _ = 1, 24 do
    local parent = wotb.ui.control_get_parent(node)
    if parent == nil then break end
    borrowed[#borrowed + 1] = parent
    local pr = wotb.screen.rect(parent)
    if not pr then break end
    x, y = x + pr.x, y + pr.y
    node = parent
  end
  for _, handle in ipairs(borrowed) do pcall(wotb.handles.release, handle) end
  return { x = x, y = y, width = r.width, height = r.height }
end

local last_click_ms = 0
local function click_once(entry)
  -- The same press may arrive through the runtime's click event and the
  -- pointer fallback; one popup per press.
  local now = wotb.timer.now_ms() or (os.clock() * 1000)
  if now - last_click_ms < 500 then return end
  last_click_ms = now
  on_click(entry)
end

local function pointer_fallback(e)
  local d = e and e.data
  if type(d) ~= "table" or d.action ~= 3 then return end   -- 3 = pointer ended (up)
  for name, entry in pairs(buttons) do
    local r = absolute_rect(entry.control)
    if DEBUG_PROBES > 0 then
      log.info("pointer up at %.0f,%.0f vs %s rect=%s", d.screen_x, d.screen_y, name,
               r and string.format("%.0f,%.0f %.0fx%.0f", r.x, r.y, r.width, r.height) or "nil")
    end
    if r and d.screen_x >= r.x and d.screen_x <= r.x + r.width and
       d.screen_y >= r.y and d.screen_y <= r.y + r.height then
      click_once(entry)
      return
    end
  end
end

local function hook_clicks()
  local hooked = 0
  local ui = wotb.ui
  for name, entry in pairs(buttons) do
    if ui_tokens[name] == nil and type(ui) == "table" and type(ui.event_subscribe) == "function" then
      local token, why = ui.event_subscribe(entry.control, ui.EVENT_CLICK, function() click_once(entry) end)
      if token ~= nil then
        ui_tokens[name] = token
        hooked = hooked + 1
      elseif not subscribed_clicks then
        log.info("ui.event_subscribe on %s refused (%s); using the pointer fallback", name, tostring(why))
      end
    end
  end
  subscribed_clicks = true
  -- Always: the runtime's click event is not delivered for stock controls
  -- (see absolute_rect), so the pointer fallback is the working path.
  if input_token == nil then
    local token, why = wotb.events.subscribe(wotb.events.TOPIC_UI_INPUT, pointer_fallback,
                                             wotb.events.PRIORITY_NORMAL, true)
    if token == nil then
      log.warn("pointer fallback unavailable: %s", tostring(why))
    else
      input_token = token
    end
  end
end

-- A refused write is logged once per name; the row is still shown with
-- whatever did apply, so the player sees something rather than nothing.
local warned = {}
local function apply(what, name, ok, why)
  if ok then return true end
  local key = what .. ":" .. name
  if not warned[key] then
    warned[key] = true
    log.warn("%s(%s) refused: %s", what, name, tostring(why))
  end
  return false
end

local function refresh()
  local list, why = wotb.session.clusters()
  if not list then
    log.info("clusters unavailable: %s", tostring(why))
    return false
  end
  local current = wotb.session.cluster()
  local row = wotb.screen.find("bf_cluster_row")
  if not row then return false end
  local by_id = {}
  for _, item in ipairs(list) do by_id[item.id] = item end
  buttons = {}
  local shown = 0
  for name, cluster_id in pairs(BUTTONS) do
    local control = wotb.screen.find(name)
    if control then
      local item = by_id[cluster_id]
      local present = cluster_id == -1 or item ~= nil
      apply("set_visible", name, wotb.screen.set_visible(control, present))
      if present then
        local is_current = item ~= nil and current ~= nil and item.id == current.id
        local usable = not is_current and (item == nil or item.alive ~= false)
        apply("set_enabled", name, wotb.ui.control_set_enabled(control, usable and 1 or 0))
        buttons[name] = { control = control, cluster_id = cluster_id }
        shown = shown + 1
        if DEBUG_PROBES > 0 then
          local r = wotb.screen.rect(control)
          log.info("%s rect=%s current=%s", name,
                   r and string.format("%.0f,%.0f %.0fx%.0f", r.x, r.y, r.width, r.height) or "nil",
                   tostring(is_current))
        end
      end
    end
  end
  apply("set_visible", "bf_cluster_row", wotb.screen.set_visible(row, true))
  if DEBUG_PROBES > 0 then
    local r = wotb.screen.rect(row)
    log.info("row rect=%s buttons=%d current=%s", r and string.format("%.0f,%.0f %.0fx%.0f", r.x, r.y, r.width, r.height) or "nil",
             shown, current and current.name or "?")
  end
  return shown > 0
end

local function unbind()
  for _, token in pairs(ui_tokens) do pcall(wotb.ui.event_unsubscribe, token) end
  ui_tokens = {}
  buttons = {}
  row_bound = false
end

local function probe_row(reason)
  -- The page is built after the event that announces it; give the client a
  -- frame before looking for the row. Not the settings page, or the
  -- resource package is missing: find() answers nil and nothing happens.
  if probe_pending then return end
  probe_pending = true
  wotb.timer.after(150, function()
    probe_pending = false
    local row, row_why = wotb.screen.find("bf_cluster_row")
    if DEBUG_PROBES > 0 then
      DEBUG_PROBES = DEBUG_PROBES - 1
      log.info("probe (%s): row=%s %s", reason, tostring(row ~= nil), row == nil and tostring(row_why) or "")
    end
    if row == nil then
      if row_bound then
        log.info("settings page closed (%s); row released", reason)
        unbind()
      end
      return
    end
    if row_bound then return end
    if refresh() then
      row_bound = true
      hook_clicks()
      log.info("settings page open (%s); cluster row shown", reason)
    else
      log.info("settings page open (%s) but the row could not be filled", reason)
    end
  end)
end

local function on_screen_changed()
  probe_row("screen changed")
end

local function on_pointer(e)
  local d = e and e.data
  if type(d) ~= "table" or d.action ~= 3 then return end   -- 3 = pointer ended (up)
  probe_row("pointer")
end

function on_enable()
  if not wotb.available("session_cluster") then
    log.info("wotbmod.session.cluster is not published on this client; the row stays hidden")
    return
  end
  local token, why = wotb.events.subscribe(wotb.events.TOPIC_UI_SCREEN_CHANGED, on_screen_changed,
                                           wotb.events.PRIORITY_NORMAL, true)
  if token == nil then
    log.warn("screen subscription refused: %s", tostring(why))
  else
    screen_token = token
  end
  token, why = wotb.events.subscribe(wotb.events.TOPIC_UI_INPUT, on_pointer,
                                     wotb.events.PRIORITY_NORMAL, true)
  if token == nil then
    log.warn("pointer subscription refused: %s", tostring(why))
  else
    pointer_token = token
  end
  wotb.session.on_cluster_changed(function(ev)
    local t = texts()
    if ev.status == "started" then
      wotb.screen.notify(string.format(t.started, name_of(ev.to)), 4)
    elseif ev.status == "connected" then
      local now = wotb.session.cluster()
      wotb.screen.notify(string.format(t.connected, now and now.name or name_of(ev.to)), 5)
    elseif ev.status == "failed" then
      wotb.screen.notify(t.failed, 6)
    end
  end)
  -- Already on the settings page (hot reload): fill the row now.
  on_screen_changed()
  log.info("cluster picker armed")
end

function on_disable()
  close_confirm()
  wotb.session.off_all()
  wotb.timer.cancel_all()
  if screen_token ~= nil then wotb.events.unsubscribe(screen_token) end
  if pointer_token ~= nil then wotb.events.unsubscribe(pointer_token) end
  if input_token ~= nil then wotb.events.unsubscribe(input_token) end
  for _, token in pairs(ui_tokens) do pcall(wotb.ui.event_unsubscribe, token) end
  for _, entry in pairs(buttons) do pcall(wotb.screen.set_visible, entry.control, false) end
  local row = wotb.screen.find("bf_cluster_row")
  if row then pcall(wotb.screen.set_visible, row, false) end
end
