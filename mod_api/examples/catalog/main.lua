-- blitzforge.catalog: the BlitzForge mod catalogue inside the hangar.
--
-- The screen itself is stock UI: the resource package blitzforge.catalog.ui
-- splices an icon button into the hangar's left column and a hidden
-- ModCatalogScreen (header, tabs, eight card frames with one caption "face"
-- per action, pager, restart bar, confirm dialog) into Hangar.yaml, all on
-- the client's own style classes. This script only shows and hides those
-- controls (the real SetVisibilityFlag), hit-tests pointer releases against
-- their rectangles (stock buttons never deliver EVENT_CLICK to a mod), and
-- draws the dynamic lines - names, versions, status - with its own text
-- controls in the game's font on top of the frames.
--
-- Data: «Каталог» is index.json from the portal (wotb.http) against
-- `wotbmod.exe list --json` (wotb.packages); «Кастомные моды» is everything
-- installed without the portal. Every action is a wotbmod.exe command; a
-- successful one raises «нужен перезапуск» and shows the restart bar.

local log = wotb.log
log.set_category("catalog")

local PORTAL = "https://blitz-forge.org"
local PORTAL_API = PORTAL .. "/api/v1"
local PORTAL_API_ENCODED = "https%3A%2F%2Fblitz-forge.org%2Fapi%2Fv1"
local INDEX_URL = PORTAL_API .. "/index.json"
local CLIENT_BUILD = "11.20.0.887"
local PAGE_SIZE = 8
local POLL_EVERY_FRAMES = 15
local PROBE_DELAY_MS = 150
local CLICK_DEBOUNCE_FRAMES = 20
local SYSTEM_IDS = { ["blitzforge.catalog"] = true, ["blitzforge.catalog.ui"] = true, ["wotbmod.lua_host"] = true }
local FONT = "~res:/Fonts/WarHeliosCondCBold.ttf"

local WHITE = { r = 1, g = 1, b = 1, a = 1 }
local GREY = { r = 0.66, g = 0.70, b = 0.74, a = 1 }
local GOLD = { r = 0.96, g = 0.78, b = 0.32, a = 1 }
local GREEN = { r = 0.45, g = 0.85, b = 0.45, a = 1 }
local RED = { r = 0.92, g = 0.40, b = 0.36, a = 1 }

-- ---------------------------------------------------------------- state --

local S = {
  open = false,
  tab = "catalog",          -- "catalog" | "custom"
  page = 1,
  items = {},
  page_items = {},          -- slot -> item on the visible page
  index = nil,              -- decoded index.json
  index_source = nil,       -- "portal" | "cache"
  installed = nil,          -- rows of `wotbmod list --json`
  installed_error = nil,
  job = nil,                -- { id, verb, mod_id, on_done }
  need_restart = false,
  status = "",
  status_colour = GREY,
  http_request = nil,
  confirm = nil,            -- { text, on_accept } while the dialog is up
  last_click_frame = -1000,
  frame = 0,
  probe_pending = false,
  hidden_stock = {},        -- stock controls hidden while the window is open
}

local FACES = { "FaceInstall", "FaceUpdate", "FaceRemove", "FaceDisable", "FaceEnable", "FaceLocked", "FaceUnsupported" }
local FACE_SET = {}
for _, name in ipairs(FACES) do FACE_SET[name] = true end
local tokens = {}           -- event subscriptions

-- ------------------------------------------------------------- helpers --

local function starts_with(text, prefix)
  return type(text) == "string" and text:sub(1, #prefix) == prefix
end

local function is_portal_row(row)
  return starts_with(row.catalog, PORTAL)
end

local function parse_version(text)
  local out = {}
  if type(text) ~= "string" then return out end
  local core = text:match("^([^-+]*)") or ""
  for part in core:gmatch("[^.]+") do
    out[#out + 1] = tonumber(part) or 0
  end
  out.pre = text:find("-", 1, true) ~= nil
  return out
end

local function version_newer(a, b)
  local va, vb = parse_version(a), parse_version(b)
  for i = 1, math.max(#va, #vb) do
    local x, y = va[i] or 0, vb[i] or 0
    if x ~= y then return x > y end
  end
  if va.pre ~= vb.pre then return vb.pre end
  return false
end

local function short(text, max)
  if type(text) ~= "string" then return "" end
  if #text <= max then return text end
  return text:sub(1, max - 1) .. "…"
end

local function last_line(text)
  local result = ""
  for line in tostring(text or ""):gmatch("[^\r\n]+") do
    if line:match("%S") then result = line end
  end
  return result
end

local function sorted_by_name(list)
  table.sort(list, function(a, b)
    local x, y = (a.name or a.id or ""):lower(), (b.name or b.id or ""):lower()
    if x ~= y then return x < y end
    return (a.id or "") < (b.id or "")
  end)
  return list
end

-- ------------------------------------------------------------ the model --

local function installed_by_id()
  local map = {}
  for _, row in ipairs(S.installed or {}) do map[row.id] = row end
  return map
end

local function catalog_items()
  local items = {}
  local packages = S.index and S.index.packages
  if type(packages) ~= "table" then return items end
  local installed = installed_by_id()
  for id, entry in pairs(packages) do
    if type(entry) == "table" then
      local latest = entry.latest
      local record = type(entry.versions) == "table" and entry.versions[latest] or nil
      local supported = true
      if type(record) == "table" and type(record.client) == "table" then
        local builds = record.client.builds
        if type(builds) == "table" and #builds > 0 then
          supported = false
          for _, build in ipairs(builds) do
            if build == CLIENT_BUILD then supported = true end
          end
        end
      end
      local row = installed[id]
      local state = "absent"
      if row then
        state = (latest and version_newer(latest, row.version)) and "outdated" or "installed"
      elseif not supported then
        state = "unsupported"
      end
      items[#items + 1] = {
        kind = "portal", id = id, name = entry.name or id, developer = entry.developer or "",
        verified = entry.verified == true, latest = latest,
        installed_version = row and row.version or nil,
        installed_from_file = row and not is_portal_row(row) or false,
        package_type = type(record) == "table" and record.type or nil,
        state = state,
      }
    end
  end
  return sorted_by_name(items)
end

local function custom_items()
  local items = {}
  local packages = S.index and S.index.packages or {}
  for _, row in ipairs(S.installed or {}) do
    if not is_portal_row(row) or packages[row.id] == nil then
      local origin
      if row.managed == false then
        origin = row.kind == "lua" and "папка mods\\lua" or "в папке mods, без леджера"
      elseif is_portal_row(row) then
        origin = "снят с портала"
      elseif row.source == "rollback" then
        origin = "откат к резервной копии"
      elseif row.source == "catalog" then
        origin = "зависимость из каталога"
      elseif type(row.source) == "string" and row.source ~= "" then
        origin = "из файла: " .. short(row.source:match("([^\\/]+)$") or row.source, 40)
      else
        origin = "поставлен вручную"
      end
      items[#items + 1] = {
        kind = "custom", id = row.id, name = row.id, version = row.version,
        package_kind = row.kind, enabled = row.enabled ~= false, managed = row.managed == true,
        origin = origin, system = SYSTEM_IDS[row.id] == true, quarantined = row.quarantined == true,
      }
    end
  end
  return sorted_by_name(items)
end

local function rebuild_items()
  S.items = S.tab == "catalog" and catalog_items() or custom_items()
  local pages = math.max(1, math.ceil(#S.items / PAGE_SIZE))
  if S.page > pages then S.page = pages end
  if S.page < 1 then S.page = 1 end
end

-- face name, line1, line2, colour for a card
local function card_view(item)
  if item.kind == "portal" then
    local line1 = string.format("%s  ·  v%s  ·  %s%s", short(item.name, 40), tostring(item.latest or "?"),
      short(item.developer, 24), item.verified and "  ·  проверен" or "")
    local line2, colour, face = "", GREY, "FaceInstall"
    if item.state == "installed" then
      line2, colour, face = "установлен v" .. tostring(item.installed_version), GREEN, "FaceRemove"
    elseif item.state == "outdated" then
      line2 = string.format("установлен v%s, доступна v%s", tostring(item.installed_version), tostring(item.latest))
      colour, face = GOLD, "FaceUpdate"
    elseif item.state == "unsupported" then
      line2, face = "не для клиента " .. CLIENT_BUILD, "FaceUnsupported"
    else
      line2 = "не установлен" .. (item.package_type == "resource" and "  ·  меняет файлы игры" or "")
    end
    if item.installed_from_file then line2 = line2 .. "  ·  поставлен из файла" end
    if SYSTEM_IDS[item.id] and item.state ~= "outdated" then face = "FaceLocked" end
    return face, line1, line2, colour
  end
  local line1 = string.format("%s  ·  v%s  ·  %s", short(item.name, 44), tostring(item.version or "?"),
    tostring(item.package_kind or "?"))
  local parts = { item.enabled and "включён" or "выключен", item.origin }
  if item.quarantined then parts[#parts + 1] = "в карантине после крэшей" end
  local line2 = table.concat(parts, "  ·  ")
  local colour = item.enabled and GREEN or GREY
  local face
  if item.system then
    face = "FaceLocked"
  elseif item.package_kind == "resource" then
    face = "FaceRemove"
  elseif item.enabled then
    face = "FaceDisable"
  else
    face = "FaceEnable"
  end
  return face, line1, line2, colour
end

-- ------------------------------------------------- the stock screen (UI) --

local UI = {
  root = nil,               -- ModCatalogScreen handle
  button = nil,             -- ModCatalogButton in the hangar's left column
  h = {},                   -- name -> handle for header/pager/dialog controls
  cards = {},               -- slot -> { frame = handle, faces = { name = handle } }
  texts = {},               -- own text controls: status, page, empty, body, card lines
  missing_logged = false,
}

local function find(path)
  local control = wotb.screen.find(path)
  return control or nil
end

local function release(handle)
  if handle ~= nil then pcall(wotb.handles.release, handle) end
end

local function forget_ui()
  for _, text in pairs(UI.texts) do pcall(function() text:destroy() end) end
  UI.texts = {}
  for _, handle in pairs(UI.h) do release(handle) end
  UI.h = {}
  for _, card in pairs(UI.cards) do
    release(card.frame)
    for _, face in pairs(card.faces) do release(face) end
  end
  UI.cards = {}
  release(UI.root); UI.root = nil
  release(UI.button); UI.button = nil
  S.hidden_stock = {}
end

local HEADER_CONTROLS = {
  marker = "ModCatalogScreen/ModCatalogStateMarker",
  -- the square around the stock IconButton: the prototype's own rectangle is
  -- not a reliable hit target, the 72x72 frame is
  back = "ModCatalogScreen/HeaderBar/BackSquare",
  tab_catalog = "ModCatalogScreen/HeaderBar/TabCatalog",
  tab_custom = "ModCatalogScreen/HeaderBar/TabCustom",
  mark_catalog = "ModCatalogScreen/HeaderBar/TabCatalogMark",
  mark_custom = "ModCatalogScreen/HeaderBar/TabCustomMark",
  refresh = "ModCatalogScreen/HeaderBar/RefreshButton",
  list = "ModCatalogScreen/ListPage",
  prev = "ModCatalogScreen/PrevButton",
  next = "ModCatalogScreen/NextButton",
  restart = "ModCatalogScreen/RestartButton",
  overlay = "ModCatalogScreen/ConfirmOverlay",
  dialog = "ModCatalogScreen/ConfirmOverlay/Dialog",
  confirm = "ModCatalogScreen/ConfirmOverlay/Dialog/ConfirmRemove",
  cancel = "ModCatalogScreen/ConfirmOverlay/Dialog/CancelRemove",
}

-- Finds the stock screen and every control the script drives. Returns true
-- when the whole set is there; a partial hangar (mid-rebuild) counts as
-- absent and is retried on the next probe.
local function locate_ui()
  if UI.root ~= nil then return true end
  local root = find("ModCatalogScreen")
  if root == nil then return false end
  local h = {}
  for key, path in pairs(HEADER_CONTROLS) do
    h[key] = find(path) or find(path:match("([^/]+)$"))
    if h[key] == nil then
      if not UI.missing_logged then
        log.warn("stock control missing: %s", path)
        UI.missing_logged = true
      end
      for _, handle in pairs(h) do release(handle) end
      release(root)
      return false
    end
  end
  local cards = {}
  for slot = 1, PAGE_SIZE do
    local frame = find("ModCatalogScreen/ListPage/Card" .. slot) or find("Card" .. slot)
    if frame == nil then
      for _, handle in pairs(h) do release(handle) end
      for _, card in pairs(cards) do release(card.frame); for _, f in pairs(card.faces) do release(f) end end
      release(root)
      return false
    end
    -- Face names repeat across the eight cards, so they are read off the
    -- card's own children rather than searched by name from the root.
    local faces = {}
    for _, child in ipairs(wotb.screen.children(frame) or {}) do
      local info = wotb.screen.info(child)
      local id = info and info.id
      if id and FACE_SET[id] then faces[id] = child else release(child) end
    end
    cards[slot] = { frame = frame, faces = faces }
  end
  UI.root, UI.h, UI.cards = root, h, cards
  UI.button = find("ModCatalogButton")
  log.info("stock screen located (%d cards)", PAGE_SIZE)
  return true
end

local function set_visible(handle, on)
  if handle == nil then return end
  local ok, err = wotb.screen.set_visible(handle, on and true or false)
  if not ok and err then log.warn("set_visible: %s", tostring(err)) end
end

-- The loader derives the MOD_SCREEN context bit from the marker's input
-- flag; other mods' panels hide while it is set. Raised with the screen,
-- lowered with it.
local function set_marker(on)
  if UI.h.marker == nil then return end
  local ok, err = pcall(wotb.ui.control_set_interactable, UI.h.marker, on and 1 or 0)
  if not ok then log.warn("marker: %s", tostring(err)) end
end

local function text_control(key, parent, x, y, width, height, size, colour, align)
  local existing = UI.texts[key]
  if existing then return existing end
  local control, err = wotb.ui.create({
    type = wotb.ui.CONTROL_TEXT, id = "bf_catalog_" .. key, parent = parent,
    x = x, y = y, width = width, height = height,
    font = FONT, font_size = size, color = colour or WHITE,
    alignment = align or wotb.ui.ALIGN_LEFT, text = "", visible = true,
  })
  if control == nil then
    log.warn("text %s: %s", key, tostring(err))
    return nil
  end
  UI.texts[key] = control
  return control
end

local function set_text(key, text, colour)
  local control = UI.texts[key]
  if control == nil then return end
  pcall(function() control:set_text(text or "") end)
  if colour then pcall(function() control:set_text_color(colour) end) end
end

local function ensure_texts()
  if UI.root == nil then return end
  local rect = wotb.screen.rect(UI.root) or { width = 1024, height = 768 }
  text_control("status", UI.root, 40, 86, math.max(300, rect.width - 80), 26, 18, GREY)
  text_control("page", UI.root, 170, rect.height - 40 - 36, 300, 30, 18, GREY)
  text_control("empty", UI.h.list, 16, 120, math.max(300, rect.width - 120), 34, 24, GREY)
  text_control("body", UI.h.dialog, 20, 84, 480, 30, 18, GREY, wotb.ui.ALIGN_CENTER)
  for slot = 1, PAGE_SIZE do
    local card = UI.cards[slot]
    text_control("name" .. slot, card.frame, 16, 6, 640, 28, 22, WHITE)
    text_control("meta" .. slot, card.frame, 16, 34, 640, 24, 16, GREY)
  end
end

local function set_status(text, colour)
  S.status, S.status_colour = text or "", colour or GREY
  set_text("status", S.status, S.status_colour)
end

local function render()
  if UI.root == nil then return end
  rebuild_items()
  ensure_texts()
  local pages = math.max(1, math.ceil(#S.items / PAGE_SIZE))
  local first = (S.page - 1) * PAGE_SIZE
  S.page_items = {}
  for slot = 1, PAGE_SIZE do
    local item = S.items[first + slot]
    local card = UI.cards[slot]
    if item then
      S.page_items[slot] = item
      local face, line1, line2, colour = card_view(item)
      set_visible(card.frame, true)
      for name, handle in pairs(card.faces) do set_visible(handle, name == face and S.job == nil) end
      set_text("name" .. slot, line1, WHITE)
      set_text("meta" .. slot, line2, colour)
    else
      set_visible(card.frame, false)
    end
  end
  local empty = ""
  if #S.items == 0 then
    if S.tab == "catalog" then
      if S.installed == nil and S.installed_error == nil then
        empty = "Читаю установленное…"
      elseif S.index == nil then
        empty = "Каталог ещё не загружен"
      else
        empty = "В каталоге пока нет модов"
      end
    else
      empty = S.installed_error or "Своих модов нет: всё установленное пришло с портала"
    end
  end
  set_text("empty", empty, GREY)
  set_text("page", string.format("стр. %d / %d  ·  %d", S.page, pages, #S.items), GREY)
  set_visible(UI.h.prev, S.page > 1)
  set_visible(UI.h.next, S.page < pages)
  set_visible(UI.h.mark_catalog, S.tab == "catalog")
  set_visible(UI.h.mark_custom, S.tab == "custom")
  set_visible(UI.h.restart, S.need_restart and S.job == nil)
  set_text("status", S.status, S.status_colour)
end

-- Two stock controls draw above the blur and would sit on the catalogue.
local STOCK_HIDDEN_WHILE_OPEN = { "TanksPanelHolder", "SideBar" }

local function show_stock(on)
  for _, name in ipairs(STOCK_HIDDEN_WHILE_OPEN) do
    local handle = S.hidden_stock[name]
    if handle == nil then
      handle = find(name)
      S.hidden_stock[name] = handle
    end
    if handle then set_visible(handle, on) end
  end
end

-- ---------------------------------------------------- installed (list) --

local function parse_list_output(text)
  local value, err = wotb.json.decode(text)
  if value == nil then return nil, "list: " .. tostring(err) end
  if type(value) ~= "table" or type(value.packages) ~= "table" then
    return nil, "list: unexpected shape"
  end
  local rows = {}
  for _, row in ipairs(value.packages) do
    if type(row) == "table" and type(row.id) == "string" then
      local clean = {}
      for key, field in pairs(row) do
        if field ~= wotb.json.null then clean[key] = field end
      end
      rows[#rows + 1] = clean
    end
  end
  return rows
end

local function start_job(verb, args, mod_id, on_done)
  if S.job then return nil, "busy" end
  local id, err = wotb.packages.run(verb, args or {})
  if id == nil then
    log.warn("%s %s: %s", verb, tostring(mod_id), tostring(err))
    return nil, err
  end
  S.job = { id = id, verb = verb, mod_id = mod_id, on_done = on_done }
  return id
end

local function refresh_installed()
  local ok, err = start_job("list", {}, nil, function(result)
    if result.exit_code == 0 then
      local rows, why = parse_list_output(result.stdout)
      if rows then
        S.installed, S.installed_error = rows, nil
        log.info("installed: %d packages", #rows)
      else
        S.installed_error = why
        log.warn("%s", why)
      end
    else
      S.installed_error = "wotbmod list: " .. last_line(result.stderr)
      log.warn("%s", S.installed_error)
    end
    render()
  end)
  if not ok then
    S.installed_error = "wotbmod: " .. tostring(err)
    set_status(S.installed_error, RED)
    render()
  end
end

local function finish_job(result)
  local job = S.job
  S.job = nil
  local ok = result.exit_code == 0
  if job.verb ~= "list" then
    if ok then
      S.need_restart = true
      set_status(string.format("Готово: %s %s. Изменения вступят после перезапуска клиента.",
        job.verb, tostring(job.mod_id)), GREEN)
      log.info("%s %s: ok", job.verb, tostring(job.mod_id))
    else
      local why = last_line(result.stderr)
      if why == "" then why = last_line(result.stdout) end
      set_status(string.format("Ошибка (%s %s): %s", job.verb, tostring(job.mod_id), short(why, 120)), RED)
      log.warn("%s %s: exit %d: %s", job.verb, tostring(job.mod_id), result.exit_code, why)
    end
  end
  if job.on_done then job.on_done(result) end
  if job.verb ~= "list" and ok then refresh_installed() end
  render()
end

local function poll_job()
  if not S.job then return end
  local state, err = wotb.packages.poll(S.job.id)
  if state == nil then
    log.warn("poll: %s", tostring(err))
    S.job = nil
    set_status("Команда потеряна: " .. tostring(err), RED)
    render()
    return
  end
  if state.running then return end
  finish_job(state)
end

-- ---------------------------------------------------- portal (index) --

local function apply_index(text, source)
  local value, err = wotb.json.decode(text)
  if value == nil or type(value) ~= "table" or type(value.packages) ~= "table" then
    return false, "index.json: " .. tostring(err or "unexpected shape")
  end
  S.index, S.index_source = value, source
  if source == "portal" then wotb.store.set("catalog.index", text) end
  if type(value.notice) == "string" and value.notice ~= "" then
    set_status("Портал: " .. short(value.notice, 140), GOLD)
  end
  return true
end

local function load_cached_index()
  local cached = wotb.store.get("catalog.index")
  if type(cached) == "string" and cached ~= "" then
    local ok, why = apply_index(cached, "cache")
    if not ok then log.warn("cached %s", why) end
  end
end

local function fetch_index()
  if S.http_request then return end
  local request, err = wotb.http.request_create()
  if request == nil then
    set_status("Сеть недоступна: " .. tostring(err), RED)
    return
  end
  wotb.http.request_set_method(request, "GET")
  wotb.http.request_set_url(request, INDEX_URL)
  wotb.http.request_set_timeout(request, 15000)
  wotb.http.request_set_max_response_size(request, 4 * 1024 * 1024)
  S.http_request = request
  local sent, send_err = wotb.http.request_send_async(request, function(handle, result)
    S.http_request = nil
    local status = wotb.http.response_get_status(handle)
    local body = wotb.http.response_get_body(handle)
    release(handle)
    if result ~= 0 or status ~= 200 or type(body) ~= "string" then
      set_status(string.format("Портал недоступен (код %s, HTTP %s)%s", tostring(result), tostring(status),
        S.index and "; показана офлайн-копия" or ""), RED)
      render()
      return
    end
    local ok, why = apply_index(body, "portal")
    if ok then
      if S.status == "" or S.status:find("Портал недоступен", 1, true) or S.status:find("Загружаю", 1, true) then
        set_status(S.index_source == "portal" and "Каталог обновлён" or "", GREY)
      end
    else
      set_status(why, RED)
    end
    render()
  end)
  if not sent then
    S.http_request = nil
    release(request)
    set_status("Сеть: " .. tostring(send_err), RED)
  else
    set_status("Загружаю каталог…", GREY)
  end
end

-- -------------------------------------------------------------- actions --

local function open_window()
  if not locate_ui() then
    log.warn("the stock screen is not in this hangar (is blitzforge.catalog.ui installed?)")
    return
  end
  S.open = true
  show_stock(false)
  set_visible(UI.root, true)
  set_marker(true)
  ensure_texts()
  if S.installed == nil then refresh_installed() end
  if S.index == nil then load_cached_index() end
  fetch_index()
  render()
end

local function close_window()
  S.open = false
  S.confirm = nil
  if UI.root then
    set_visible(UI.h.overlay, false)
    set_visible(UI.root, false)
    set_marker(false)
  end
  show_stock(true)
end

local function ask_confirm(text, on_accept)
  S.confirm = { on_accept = on_accept }
  set_text("body", text, GREY)
  set_visible(UI.h.overlay, true)
end

local function close_confirm()
  S.confirm = nil
  set_visible(UI.h.overlay, false)
end

local function run_action(verb, args, mod_id, label)
  local ok, err = start_job(verb, args, mod_id)
  if ok then set_status(label, GOLD) else set_status("Не запустилось: " .. tostring(err), RED) end
  render()
end

local function card_clicked(slot)
  local item = S.page_items[slot]
  if not item or S.job then return end
  if item.kind == "portal" then
    if item.state == "installed" then
      ask_confirm("Удалить " .. item.name .. "? Резервная копия останется в mods\\cache.", function()
        run_action("uninstall", { item.id }, item.id, "Удаляю " .. item.id .. "…")
      end)
    elseif item.state == "absent" or item.state == "outdated" then
      local link = string.format("wotbmod://install/%s@%s?source=%s", item.id, tostring(item.latest), PORTAL_API_ENCODED)
      run_action("launcher-open", { link }, item.id,
        (item.state == "outdated" and "Обновляю " or "Устанавливаю ") .. item.id .. " v" .. tostring(item.latest) .. "…")
    end
    return
  end
  if item.system then return end
  if item.package_kind == "resource" then
    ask_confirm("Удалить " .. item.id .. " и вернуть штатные файлы игры?", function()
      run_action("uninstall", { item.id }, item.id, "Удаляю " .. item.id .. "…")
    end)
  elseif item.enabled then
    run_action("disable", { item.id }, item.id, "Выключаю " .. item.id .. "…")
  else
    run_action("enable", { item.id }, item.id, "Включаю " .. item.id .. "…")
  end
end

local function restart_clicked()
  if S.job then return end
  local ok, err = start_job("restart-client", {}, nil)
  if ok then set_status("Перезапуск клиента…", GOLD) else set_status("Перезапуск не начался: " .. tostring(err), RED) end
  render()
end

-- ----------------------------------------------------------- pointer --

-- Absolute rectangle of a stock control: wotb.screen.rect is local to the
-- parent, so the parents' offsets are summed up the tree (cluster_picker).
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
  for _, handle in ipairs(borrowed) do release(handle) end
  return { x = x, y = y, width = r.width, height = r.height }
end

local function hit(control, px, py)
  if control == nil then return false end
  local visible = wotb.screen.visible(control)
  if visible == false then return false end
  local r = absolute_rect(control)
  return r ~= nil and px >= r.x and px <= r.x + r.width and py >= r.y and py <= r.y + r.height
end

-- Debounced by frame, not by wotb.timer.now_ms(): the timer clock only runs
-- while a timer is live, and a nil clock would have swallowed every click
-- after the first.
local function on_pointer_release(px, py)
  local now = S.frame
  if now - S.last_click_frame < CLICK_DEBOUNCE_FRAMES then return end
  if not S.open then
    if UI.button == nil then
      if not locate_ui() then return end
    end
    if hit(UI.button, px, py) then
      S.last_click_frame = now
      open_window()
    end
    return
  end
  if UI.root == nil then return end
  if S.confirm then
    if hit(UI.h.confirm, px, py) then
      S.last_click_frame = now
      local accept = S.confirm.on_accept
      close_confirm()
      accept()
    elseif hit(UI.h.cancel, px, py) then
      S.last_click_frame = now
      close_confirm()
    end
    return
  end
  local actions = {
    { UI.h.back, close_window },
    { UI.h.tab_catalog, function() S.tab = "catalog"; S.page = 1; render() end },
    { UI.h.tab_custom, function() S.tab = "custom"; S.page = 1; render() end },
    { UI.h.refresh, function() if S.job then return end refresh_installed(); fetch_index(); render() end },
    { UI.h.prev, function() if S.page > 1 then S.page = S.page - 1; render() end end },
    { UI.h.next, function() S.page = S.page + 1; render() end },
    { UI.h.restart, restart_clicked },
  }
  for _, pair in ipairs(actions) do
    if hit(pair[1], px, py) then
      S.last_click_frame = now
      pair[2]()
      return
    end
  end
  for slot = 1, PAGE_SIZE do
    local card = UI.cards[slot]
    if S.page_items[slot] and card then
      for _, face in pairs(card.faces) do
        if hit(face, px, py) then
          S.last_click_frame = now
          card_clicked(slot)
          return
        end
      end
    end
  end
end

-- ------------------------------------------------------------ lifecycle --

local function on_screen_changed()
  -- The hangar was rebuilt or left: every stock handle is stale and so is
  -- every text control parented under one. Start over on the next probe.
  local was_open = S.open
  S.open = false
  S.confirm = nil
  forget_ui()
  if was_open then log.info("screen changed while open; window dropped") end
end

function on_enable()
  local exe, why = wotb.packages.executable()
  if exe then log.info("wotbmod: %s", exe) else log.warn("wotbmod.exe: %s", tostring(why)) end
  load_cached_index()
  tokens[#tokens + 1] = wotb.events.subscribe(wotb.events.TOPIC_UI_SCREEN_CHANGED, function()
    on_screen_changed()
  end, wotb.events.PRIORITY_NORMAL, true)
  tokens[#tokens + 1] = wotb.events.subscribe(wotb.events.TOPIC_UI_INPUT, function(e)
    local data = e and e.data
    if type(data) ~= "table" or data.action ~= 3 then return end
    local px, py = data.screen_x, data.screen_y
    if type(px) ~= "number" or type(py) ~= "number" then return end
    if UI.root == nil and not S.probe_pending then
      -- the page is built after the event that announces it
      S.probe_pending = true
      wotb.timer.after(PROBE_DELAY_MS, function()
        S.probe_pending = false
        if locate_ui() then on_pointer_release(px, py) end
      end)
      return
    end
    on_pointer_release(px, py)
  end, wotb.events.PRIORITY_NORMAL, true)
  log.info("catalog armed")
end

function on_frame(frame_index)
  S.frame = frame_index
  if S.job and frame_index % POLL_EVERY_FRAMES == 0 then poll_job() end
end

function on_disable()
  wotb.timer.cancel_all()
  for _, token in ipairs(tokens) do pcall(wotb.events.unsubscribe, token) end
  tokens = {}
  if S.http_request then
    pcall(wotb.http.request_cancel, S.http_request)
    release(S.http_request)
    S.http_request = nil
  end
  if S.job then
    pcall(wotb.packages.cancel, S.job.id)
    S.job = nil
  end
  if UI.root then
    set_visible(UI.h.overlay, false)
    set_visible(UI.root, false)
    show_stock(true)
  end
  forget_ui()
  S.open = false
end
