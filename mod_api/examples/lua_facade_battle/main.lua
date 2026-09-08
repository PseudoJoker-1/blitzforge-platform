-- example.lua_facade_battle - battle telemetry on the facades
--
-- wotb.battle.on_* for shots, hits, damage, kills and the battle's end;
-- wotb.players for names and details; wotb.timer for a roster listing 30 s
-- in; wotb.store for a running total across battles. No on_frame at all.

local log = wotb.log
log.set_category("facade_battle")

local battle = { shots = 0, hits = 0, damage = 0, kills = {} }

local function name_of(id)
  local record = wotb.players.by_id(id)
  if type(record) == "table" then return record.display_name end
  return "#" .. tostring(id)
end

local function roster()
  wotb.players.each_visible(function(record)
    local d = wotb.players.details(record)
    log.info("%s %s [%s] kills=%s", record.is_ally and "ally" or "enemy",
             record.display_name, d and d.clan_tag or "?", d and tostring(d.kills) or "?")
  end)
end

function on_enable()
  wotb.battle.on_start(function()
    battle = { shots = 0, hits = 0, damage = 0, kills = {} }
    wotb.timer.after(30000, roster)
  end)
  wotb.battle.on_shot(function() battle.shots = battle.shots + 1 end)
  wotb.battle.on_hit(function() battle.hits = battle.hits + 1 end)
  wotb.battle.on_damage(function(data)
    if data and data.damage then battle.damage = battle.damage + data.damage end
  end)
  wotb.battle.on_vehicle_destroyed(function(kill)
    if kill then
      battle.kills[#battle.kills + 1] = name_of(kill.killer_id) .. " > " ..
          name_of(kill.victim_id)
    end
  end)
  wotb.battle.on_end(function(data)
    log.info("shots=%d hits=%d damage taken=%d kills=%d winner=%s", battle.shots,
             battle.hits, battle.damage, #battle.kills,
             tostring(data and data.winner_team))
    for _, line in ipairs(battle.kills) do log.info("  %s", line) end
    local total = wotb.store.get("total", { battles = 0, shots = 0 })
    total.battles = total.battles + 1
    total.shots = total.shots + battle.shots
    wotb.store.set("total", total)
    log.info("career: %d battles, %d shots", total.battles, total.shots)
  end)
end

function on_disable()
  wotb.battle.off_all()
  wotb.timer.cancel_all()
end
