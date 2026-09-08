-- example.lua_skin_switcher - skin packs from skins.json, F9 cycles them
--
-- wotb.files reads mod://self/skins.json (a list of { id, vehicle_name,
-- assets }); wotb.vehicle.skin registers and applies a pack to the local
-- vehicle, reports requires_model_reload honestly, and rolls back before the
-- next; wotb.store remembers the chosen index; wotb.keys binds F9.
--
-- skins.json next to this file, for example:
--   [ { "id": "desert", "vehicle_name": "usa:A100_T49",
--       "assets": [ { "kind": 3, "target_uri": "...", "source_uri": "..." } ] } ]

local log = wotb.log
log.set_category("skin_switcher")

local packs = {}
local current = nil
local index = 0

local function unapply()
  if current then
    wotb.vehicle.skin.rollback(current)
    wotb.vehicle.skin.release(current)
    current = nil
  end
end

local function apply(which)
  unapply()
  local desc = packs[which]
  if desc == nil then return end
  local vehicle, vehicle_error = wotb.vehicle.local_vehicle()
  if vehicle == nil then
    log.info("no local vehicle: %s", tostring(vehicle_error))
    return
  end
  local pack, register_error = wotb.vehicle.skin.register(desc)
  if pack == nil then
    log.info("%s: %s", desc.id, tostring(register_error))
  else
    local ok, apply_error = wotb.vehicle.skin.apply(pack, vehicle)
    if ok then
      current = pack
      local state = wotb.vehicle.skin.state(pack)
      log.info("%s applied (%d/%d assets, model reload %s)", desc.id,
               state and state.mounted_asset_count or 0,
               state and state.asset_count or 0,
               state and state.requires_model_reload and "needed" or "not needed")
    else
      log.info("%s: %s", desc.id, tostring(apply_error))
      wotb.vehicle.skin.release(pack)
    end
  end
  wotb.handles.release(vehicle)
  wotb.store.set("index", which)
end

function on_enable()
  local list, read_error = wotb.files.read_json("mod://self/skins.json")
  if type(list) ~= "table" then
    log.info("skins.json: %s", tostring(read_error))
    return
  end
  packs = list
  index = wotb.store.get("index", 0)
  if index > #packs then index = 0 end
  if index > 0 then apply(index) end
  local action, bind_error = wotb.keys.bind("next", { key = "F9" })
  if action then
    wotb.keys.on_pressed("next", function()
      index = index % #packs + 1
      apply(index)
    end)
  else
    log.info("F9: %s", tostring(bind_error))
  end
end

function on_disable()
  unapply()
  wotb.keys.unbind_all()
end
