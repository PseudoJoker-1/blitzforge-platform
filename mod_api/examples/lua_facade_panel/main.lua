-- example.lua_facade_panel - a panel from wotb.panel's dynamic controls
--
-- Facade-first: wotb.panel (label/button/row/on), wotb.keys (F6 toggles
-- the panel), wotb.store (the panel remembers whether it was hidden),
-- wotb.context (the panel gates itself to battle contexts), wotb.log.
--
-- PER-FRAME COST: panel:update(frame) once per frame - on a frame with
-- nothing to do that is one modulo and two field reads (see wotb.panel).
-- Nothing else runs per frame; the counter row is rewritten only when the
-- shot count changes, and set_row compares before it crosses the ABI.

local log = wotb.log
log.set_category("facade_panel")

local panel, panel_error = wotb.panel.new({
  id = "facade_panel", width = 320, height = 150,
  anchor = "top-right", margin = 24,
})
if panel == nil then
  log.error("panel: %s", tostring(panel_error))
  return
end

local shots = 0
local hidden = false

-- Rows and a button, added after new(): the same spec a rows= list would
-- have built, written as calls.
panel:label({ text = "FACADE PANEL", size = 20 })
local shots_row = panel:label({ text = "shots: 0" })
panel:row({ items = {
  { kind = "button", id = "reset", text = "reset", width = 90, height = 28,
    on_click = function() shots = 0 panel:set_row(shots_row, "shots: 0") end },
  { kind = "button", id = "hide", text = "hide", width = 90, height = 28,
    on_click = function() hidden = true panel:hide() end },
} })
panel:on("error", function(_, scope, why) log.warn("%s: %s", scope, why) end)

function on_enable()
  local was_hidden, origin = wotb.store.get("hidden", false)
  if origin == "stored" and was_hidden then hidden = true end

  wotb.battle.on_shot(function()
    shots = shots + 1
    panel:set_row(shots_row, "shots: " .. shots)
  end)

  local action, bind_error = wotb.keys.bind("toggle", { key = "F6" })
  if action then
    wotb.keys.on_pressed("toggle", function()
      hidden = not hidden
      if hidden then panel:hide() else panel:show() end
      wotb.store.set("hidden", hidden)
    end)
  else
    log.info("F6 is not available: %s", tostring(bind_error))
  end
end

function on_frame(frame_index)
  local showing = panel:update(frame_index)
  if showing and hidden then panel:hide() end
end

function on_disable()
  wotb.battle.off_all()
  wotb.keys.unbind_all()
  panel:destroy()
end
