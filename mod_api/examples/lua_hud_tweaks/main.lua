-- example.lua_hud_tweaks - stock HUD colours and sizes through wotb.hud
--
-- The HUD does not exist on the loading screen where wotbmod.battle.started
-- arrives (live 2026-09-06: a reticle tweak made there left the overlay
-- target destroyed), so the tweaks wait 20 s and are applied once per
-- battle; wotb.hud.reset() in on_disable puts everything back. wotb.view
-- reports the camera at the same moment and sets the battle FOV.

local log = wotb.log
log.set_category("hud_tweaks")

local applied = false

local function apply()
  if applied then return end
  applied = true
  local mode, reason = wotb.hud.mode()
  if mode ~= "native" then
    log.info("hud: %s", tostring(reason))
    return
  end
  local steps = {
    { "reticle colour", wotb.hud.reticle.set_color, { r = 1, g = 0.85, b = 0.2 } },
    { "reticle size", wotb.hud.reticle.set_size, 1.2 },
    { "minimap size", wotb.hud.minimap.set_size, 1.15 },
    { "hit colour", wotb.hud.hit_indicator.set_color_hit, { r = 0.2, g = 1, b = 0.3 } },
    { "damage log position", wotb.hud.damage_log.set_position, "top-right" },
    { "session stats", wotb.hud.session_stats.set_fields, { "damage", "shots" } },
  }
  for _, step in ipairs(steps) do
    local ok, why = step[2](step[3])
    log.info("%s: %s", step[1], ok and "ok" or tostring(why))
  end
  local cam = wotb.view.get()
  if cam then log.info("camera mode=%s fov=%s", tostring(cam.mode), tostring(cam.fov)) end
  local fov_ok, fov_error = wotb.view.set_fov(80, "battle")
  log.info("battle fov: %s", fov_ok and "80" or tostring(fov_error))
end

function on_enable()
  wotb.battle.on_start(function()
    applied = false
    wotb.timer.after(20000, apply)
  end)
  wotb.battle.on_end(function()
    if applied then wotb.hud.reset() end
    wotb.view.reset()
    applied = false
  end)
end

function on_disable()
  wotb.battle.off_all()
  wotb.timer.cancel_all()
  if applied then wotb.hud.reset() end
  wotb.view.reset()
end
