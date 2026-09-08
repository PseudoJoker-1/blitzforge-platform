-- example.lua_facade_tour - the facade layer, end to end
--
-- Every wotb.* call below is a facade call (docs/LUA_MODS_RU.md, «Фасады»):
-- wotb.mod, wotb.context, wotb.players, wotb.battle, wotb.hud, wotb.ges. No
-- raw table is touched by name, and no number is invented: a field the client
-- did not publish is printed as the reason the client gave.
--
-- PER-FRAME COST: none. This mod defines no on_frame. Everything it does is
-- driven by the events it subscribed to in on_enable, and on_disable gives all
-- of it back - the handlers through battle.off_all(), the HUD through
-- hud.reset(), the one GES observation through ges.off().
--
-- WHAT THE LOG SHOWS (category "facade_tour" in the loader log):
--   * at enable: the script's id and permissions, the HUD mode and the
--     client's own capability word for wotbmod.gameplay.hud;
--   * 45 s after the battle started (wotbmod.battle.started fires on the
--     loading screen, before any vehicle or HUD control exists - live
--     2026-09-06 it listed 0 vehicles at that instant, and a reticle tweak
--     made there left the validation sweep's overlay target destroyed) and
--     again at the end: every visible vehicle with its clan tag, localized
--     vehicle name, kills and pose - or the reason a field is unavailable;
--     the HUD tweaks are made at that same moment, when the HUD exists;
--   * every shot, every kill ("X destroyed Y"), the battle result;
--   * the first camera mode change, decoded by its GES schema;
--   * F7 (wotb.keys) shows a toast through wotb.screen.notify; the camera
--     (wotb.view.get) is logged when the battle settles; the last battle's
--     shot count is kept in wotb.store.

local log = wotb.log
log.set_category("facade_tour")

local shots = 0
local hud_touched = false
local ges_handle = nil

local function name_of(id)
  local record = wotb.players.by_id(id)
  if type(record) == "table" then return record.display_name end
  return "#" .. tostring(id)
end

local function pose_text(record)
  if not record.position_available then return "n/a" end
  local p = record.position
  return string.format("%.0f,%.0f,%.0f", p.x, p.y, p.z)
end

local function describe_players()
  local count, err = wotb.players.each_visible(function(record)
    local details, details_error = wotb.players.details(record)
    if details then
      log.info("%s [%s] %s kills=%s pose=%s", record.display_name,
               details.clan_tag or "?",
               details.vehicle_display_name or details.vehicle_name or "?",
               tostring(details.kills), pose_text(record))
      for name, reason in next, details.unavailable do
        log.info("  %s unavailable: %s", name, reason)
      end
    else
      log.info("%s: %s", record.display_name, tostring(details_error))
    end
  end)
  log.info("visible vehicles: %s", tostring(count or err))
end

-- Runs 45 s into the battle, once: the HUD controls exist by then, which
-- they do not on the loading screen where battle.started arrives.
local function tweak_hud()
  if hud_touched then return end
  hud_touched = true
  local ok, colour_error = wotb.hud.reticle.set_color({ r = 1, g = 0.25, b = 1 })
  log.info("reticle colour: %s", ok and "ok" or tostring(colour_error))
  local refused, why = wotb.hud.minimap.set_show_last_known(true)
  log.info("minimap last-known: %s", refused and "ok" or tostring(why))
end

local function describe_camera()
  local cam, cam_error = wotb.view.get()
  if cam == nil then
    log.info("camera: %s", tostring(cam_error))
    return
  end
  log.info("camera mode=%s fov=%s", tostring(cam.mode), tostring(cam.fov))
  for name, reason in next, cam.unavailable do
    log.info("  camera %s unavailable: %s", name, reason)
  end
end

local function battle_settled()
  describe_players()
  describe_camera()
  tweak_hud()
end

function on_enable()
  local info = wotb.mod.info()
  log.info("id=%s permissions=%s host=%s", tostring(info.id),
           table.concat(info.permissions, ","),
           info.host and info.host.id or tostring(info.host_unavailable))
  local mode, reason = wotb.hud.mode()
  log.info("hud mode=%s %s", mode, reason or "")
  -- Capability names are the runtime's own, not interface ids: the list
  -- below is what capability(name) can be asked about on this client.
  local caps, caps_error = wotb.mod.capabilities()
  if caps then
    for _, cap in ipairs(caps) do
      log.info("capability %s=%s %s", cap.name, cap.status, cap.reason or "")
    end
  else
    log.info("capabilities: %s", tostring(caps_error))
  end
  -- One capability by name. "settings" is a runtime capability on every
  -- client; an interface id such as wotbmod.gameplay.hud is not a
  -- capability and would come back as "not found".
  local status, detail = wotb.mod.capability("settings")
  if status then
    log.info("capability settings=%s", status)
  else
    log.info("capability settings: %s", tostring(detail))
  end
  log.info("in battle now: %s", tostring(wotb.battle.is_active()))
  describe_camera()

  wotb.battle.on_enter(function() log.info("battle entered") end)
  wotb.battle.on_start(function()
    log.info("battle started")
    -- The roster and the HUD fill in over the next seconds; one timer, given
    -- back in on_disable, is the whole per-frame cost of this mod while it
    -- waits.
    local timer, timer_error = wotb.timer.after(45000, battle_settled)
    if timer == nil then log.info("no timer: %s", tostring(timer_error)) end
  end)
  wotb.battle.on_shot(function(data)
    shots = shots + 1
    log.info("shot %d shell=%s", shots, tostring(data and data.shell_id))
  end)
  wotb.battle.on_vehicle_destroyed(function(kill)
    if not kill then
      log.info("vehicle destroyed: no typed payload on this build")
      return
    end
    log.info("%s destroyed %s (assist %s)", name_of(kill.killer_id),
             name_of(kill.victim_id),
             kill.assist_id ~= 0 and name_of(kill.assist_id) or "-")
  end)
  wotb.battle.on_end(function(data)
    log.info("battle ended: winner team %s, %d shots seen",
             tostring(data and data.winner_team), shots)
    describe_players()
    local stored, store_error = wotb.store.set("last_battle", {
      shots = shots, winner_team = data and data.winner_team })
    log.info("stored last battle: %s", stored and "ok" or tostring(store_error))
  end)

  -- F7 -> a toast. The action is namespaced by this script's id, so another
  -- mod's "notify" never collides with it.
  local action, bind_error = wotb.keys.bind("notify", { key = "F7" })
  if action then
    wotb.keys.on_pressed("notify", function()
      local ok, how = wotb.screen.notify("facade tour: F7", 2)
      log.info("F7: %s", ok and ("shown via " .. tostring(how)) or tostring(how))
    end)
  else
    log.info("keys: %s", tostring(bind_error))
  end
  local last, origin = wotb.store.get("last_battle")
  if origin == "stored" then
    log.info("last battle: %s shots", tostring(last.shots))
  end

  if wotb.available("ges") then
    ges_handle = wotb.ges.observe("Avatar::CameraModeChanged",
        { decode = true, once = true },
        function(fields) log.info("first camera mode: %s", tostring(fields.mode)) end)
  else
    log.info("ges is not published on this client")
  end

  wotb.mod.on_disable(function() log.info("disabling after %d shots", shots) end)
end

function on_disable()
  wotb.battle.off_all()
  wotb.timer.cancel_all()
  wotb.keys.unbind_all()
  if ges_handle ~= nil then
    wotb.ges.off(ges_handle)
    ges_handle = nil
  end
  if hud_touched then wotb.hud.reset() end
end
