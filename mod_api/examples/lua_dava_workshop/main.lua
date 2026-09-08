local SAFE_LEFT = 128
local SAFE_TOP = 112
local PANEL_WIDTH = 690
local PANEL_HEIGHT = 520
local VISIBLE_CONTEXTS = wotb.context.HANGAR + wotb.context.BATTLE +
    wotb.context.TRAINING + wotb.context.REPLAY
local BLOCKED_CONTEXTS = wotb.context.MOD_SCREEN + wotb.context.TEXT_INPUT

-- Set these to two mod:// .sc2 files to enable the live hot-swap button.
-- The SDK does not redistribute game meshes.
local MESH_BASE_URI = nil
local MESH_REPLACEMENT_URI = nil

local COLORS = {
    white = {r = 1.00, g = 1.00, b = 1.00, a = 1.00},
    muted = {r = 0.68, g = 0.75, b = 0.82, a = 1.00},
    panel = {r = 0.025, g = 0.040, b = 0.058, a = 0.94},
    row = {r = 0.065, g = 0.095, b = 0.125, a = 0.97},
    amber = {r = 0.96, g = 0.66, b = 0.14, a = 1.00},
    green = {r = 0.18, g = 0.70, b = 0.36, a = 1.00},
    red = {r = 0.80, g = 0.22, b = 0.18, a = 1.00},
    blue = {r = 0.14, g = 0.45, b = 0.76, a = 1.00},
}

local screen = nil
local root = nil
local controls = {}
local control_order = {}
local ui_subscriptions = {}
local event_subscriptions = {}
local mounted = false
local open = false
local context_mask = wotb.context.NONE
local context_dirty = true
local screen_rebind_pending = false
local viewport_width = 1920
local viewport_height = 1080
local material = nil
local mesh = nil
local mesh_consumer = nil
local tracer = nil
local yaml_document = nil
local archive = nil
local material_value = 0.25
local status_text = "Инициализация"
local resource_text = "YAML/archive ещё не загружены"
local status_dirty = true
local resource_dirty = true
local pending_jobs = {}

local function log_error(scope, err)
    print("Lua DAVA Workshop " .. scope .. ": " .. tostring(err))
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

local function label(name, parent, text, x, y, width, height, size, color)
    return create(name, {
        type = wotb.ui.CONTROL_TEXT,
        text = text,
        x = x, y = y, width = width, height = height,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = size,
        color = color or COLORS.white,
        alignment = wotb.ui.ALIGN_LEFT,
        visible = true,
        interactable = false,
    }, parent)
end

local function button(name, parent, text, x, y, width, color)
    return create(name, {
        type = wotb.ui.CONTROL_BUTTON,
        text = text,
        x = x, y = y, width = width, height = 44,
        font = "~res:/Fonts/WarHeliosCondCBold.ttf",
        font_size = 18,
        color = COLORS.white,
        background_color = color,
        alignment = wotb.ui.ALIGN_CENTER,
        visible = true,
        enabled = true,
        interactable = true,
    }, parent)
end

local function set_text(control, text)
    if control == nil then return end
    local ok, err = control:set_text(text)
    if not ok then log_error("set_text", err) end
end

local function set_status(text)
    status_text = tostring(text or "")
    status_dirty = true
end

local function set_resource_text(text)
    resource_text = tostring(text or "")
    resource_dirty = true
end

local function flush_native_results()
    if status_dirty then
        status_dirty = false
        set_text(controls.Status, status_text)
    end
    if resource_dirty then
        resource_dirty = false
        set_text(controls.Resources, resource_text)
    end
end

local function set_cursor_unlocked(unlocked)
    if type(wotb.input) ~= "table" or
            type(wotb.input.set_cursor_unlocked) ~= "function" then return end
    local ok, err = wotb.input.set_cursor_unlocked(unlocked)
    if not ok then log_error("cursor", err) end
end

local function release_handle(handle)
    if handle == nil then return end
    local ok, err = wotb.handles.release(handle)
    if not ok then log_error("handle release", err) end
end

local function release_dava(handle)
    if handle == nil then return end
    local ok, err = wotb.dava.release(handle)
    if not ok then log_error("DAVA release", err) end
end

local function queue_dava(name, queued_text, callback)
    if pending_jobs[name] then
        set_status(queued_text .. " — уже выполняется")
        return false
    end
    pending_jobs[name] = true
    set_status(queued_text)
    local queued, err = wotb.dava.run_on_main(function()
        local ok, callback_error = pcall(callback)
        pending_jobs[name] = nil
        if not ok then
            set_status(name .. ": " .. tostring(callback_error))
        end
    end)
    if not queued then
        pending_jobs[name] = nil
        set_status(err)
        return false
    end
    return true
end

local function capability_line()
    local names = {"yaml", "archive", "material", "mesh", "stock_tracer",
                   "class_factory"}
    local result = {}
    for _, name in ipairs(names) do
        local available = wotb.dava.is_supported(name)
        result[#result + 1] = name .. "=" .. (available and "ON" or "OFF")
    end
    return table.concat(result, "  ·  ")
end

local function load_native_resources()
    queue_dava("resources", "YAML / ResourceArchive поставлены в MAIN", function()
        release_handle(yaml_document)
        release_handle(archive)
        yaml_document = nil
        archive = nil

        local errors = {}
        yaml_document, errors.yaml = wotb.loaders.load_dava_yaml(
            "mod://fixtures/workshop.yaml")
        local title = nil
        if yaml_document ~= nil then
            local root_node = wotb.yaml.get_root(yaml_document)
            local workshop_node = root_node and
                wotb.yaml.map_get(yaml_document, root_node, "Workshop")
            local title_node = workshop_node and
                wotb.yaml.map_get(yaml_document, workshop_node, "title")
            if title_node ~= nil then title = wotb.yaml.get_string(
                yaml_document, title_node) end
        end

        archive, errors.archive = wotb.loaders.open_dava_archive(
            "mod://fixtures/workshop.zip")
        local archive_count = nil
        local payload = nil
        if archive ~= nil then
            archive_count = wotb.archive.get_entry_count(archive)
            payload = wotb.archive.read_entry(archive, "archive_payload.txt")
        end

        if yaml_document ~= nil and archive ~= nil then
            set_resource_text(string.format(
                "YAML: %s  ·  archive: %d entry  ·  %s",
                title or "прочитан", archive_count or 0,
                (payload or "payload пуст"):gsub("[\r\n]+", "")))
            set_status("Native YAML и ResourceArchive готовы")
            print(string.format(
                "Lua DAVA Workshop resources ready: yaml=%s archive_entries=%d",
                title or "прочитан", archive_count or 0))
        else
            set_resource_text(
                "Ресурсы: " .. tostring(errors.yaml or "YAML OK") ..
                " · " .. tostring(errors.archive or "archive OK"))
            set_status("Native resource load завершён с ошибкой")
        end
    end)
end

local function mutate_material()
    queue_dava("nmaterial", "NMaterial mutation поставлена в MAIN", function()
        if material == nil then
            material, status_text = wotb.dava.create_material("lua.workshop")
            if material == nil then
                set_status(status_text)
                return
            end
        end
        material_value = material_value < 0.5 and 0.85 or 0.25
        local ok, err = wotb.dava.material_set_property(
            material, "blitzforgeWorkshopValue", material_value)
        set_status(ok and string.format(
            "NMaterial property изменён во время работы: %.2f",
            material_value) or err)
        if ok then
            print(string.format(
                "Lua DAVA Workshop NMaterial ready: value=%.2f",
                material_value))
        end
    end)
end

local function hot_swap_mesh()
    if type(MESH_BASE_URI) ~= "string" or
            type(MESH_REPLACEMENT_URI) ~= "string" then
        set_status("Укажите MESH_BASE_URI и MESH_REPLACEMENT_URI в main.lua")
        return
    end
    queue_dava("mesh", "Mesh hot-swap поставлен в MAIN", function()
        if mesh_consumer == nil then
            mesh_consumer, status_text =
                wotb.dava.create_mesh_consumer(MESH_BASE_URI)
            if mesh_consumer == nil then set_status(status_text); return end
        end
        if mesh == nil then
            mesh, status_text = wotb.dava.create_mesh(MESH_REPLACEMENT_URI)
            if mesh == nil then set_status(status_text); return end
        end
        local ok, err = wotb.dava.mesh_hot_swap(mesh_consumer, mesh)
        if ok and material ~= nil then
            ok, err = wotb.dava.material_apply(material, mesh_consumer)
        end
        set_status(ok and "RenderBatch получил новый PolygonGroup" or err)
    end)
end

local function rotate_vector(rotation, vector)
    local qx, qy, qz, qw = rotation.x or 0, rotation.y or 0,
        rotation.z or 0, rotation.w or 1
    local tx = 2 * (qy * vector.z - qz * vector.y)
    local ty = 2 * (qz * vector.x - qx * vector.z)
    local tz = 2 * (qx * vector.y - qy * vector.x)
    return {
        x = vector.x + qw * tx + qy * tz - qz * ty,
        y = vector.y + qw * ty + qz * tx - qx * tz,
        z = vector.z + qw * tz + qx * ty - qy * tx,
    }
end

local function spawn_camera_tracer()
    if not wotb.context.contains(context_mask, wotb.context.BATTLE) and
            not wotb.context.contains(context_mask, wotb.context.TRAINING) and
            not wotb.context.contains(context_mask, wotb.context.REPLAY) then
        set_status("Stock tracer доступен только в активном бою")
        return
    end
    local camera, err = wotb.camera.get_active()
    if camera == nil then set_status(err); return end
    local transform
    transform, err = wotb.camera.get_transform(camera)
    release_handle(camera)
    if transform == nil or type(transform.position) ~= "table" then
        set_status(err or "Нет transform активной камеры")
        return
    end
    local forward = rotate_vector(transform.rotation or {}, {x = 0, y = 1, z = 0})
    local p = transform.position
    local request = {
        origin = {p.x + forward.x * 2, p.y + forward.y * 2,
                  p.z + forward.z * 2},
        destination = {p.x + forward.x * 90, p.y + forward.y * 90,
                       p.z + forward.z * 90},
        shell_type = 2,
    }
    queue_dava("tracer", "Stock tracer поставлен в MAIN", function()
        release_dava(tracer)
        tracer, err = wotb.dava.create_stock_tracer(request)
        set_status(tracer and
            "Штатный tracer создан из позиции камеры" or err)
        if tracer then print("Lua DAVA Workshop stock tracer ready") end
    end)
end

local function set_open(value)
    open = value
    if controls.Panel then controls.Panel:set_visible(value) end
    if controls.OpenButton then controls.OpenButton:set_visible(not value) end
    set_cursor_unlocked(value)
end

local function subscribe_ui(control, callback)
    local token, err = control:on(wotb.ui.EVENT_CLICK, callback)
    if token then ui_subscriptions[#ui_subscriptions + 1] = token
    else log_error("UI subscribe", err) end
end

local function read_viewport()
    local size = wotb.ui.get_viewport_size()
    if type(size) == "table" and size.x and size.y then return size.x, size.y end
    return 1920, 1080
end

local function layout()
    if root == nil then return end
    root:set_size(viewport_width, viewport_height)
    controls.Panel:set_position(SAFE_LEFT, SAFE_TOP)
    controls.OpenButton:set_position(
        math.max(SAFE_LEFT, viewport_width - 330), SAFE_TOP)
end

local function build_ui()
    root = create("DavaWorkshopRoot", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = 0, y = 0, width = viewport_width, height = viewport_height,
        visible = true, interactable = false,
    }, screen)
    if root == nil then return false end
    button("OpenButton", root, "F9 · DAVA WORKSHOP", 0, 0, 290, COLORS.blue)
    local panel = create("Panel", {
        type = wotb.ui.CONTROL_CONTAINER,
        x = SAFE_LEFT, y = SAFE_TOP, width = PANEL_WIDTH, height = PANEL_HEIGHT,
        background_color = COLORS.panel, visible = false, interactable = false,
    }, root)
    if panel == nil then return false end
    label("Title", panel, "LUA · NATIVE DAVA WORKSHOP", 24, 18, 560, 42,
        28, COLORS.amber)
    button("Close", panel, "X", 622, 16, 44, COLORS.red)
    label("Capabilities", panel, capability_line(), 24, 68, 642, 54,
        16, COLORS.muted)
    label("Resources", panel, resource_text, 24, 128, 642, 82,
        16, COLORS.white)
    label("Status", panel, status_text, 24, 218, 642, 64,
        18, COLORS.white)
    button("Reload", panel, "ПЕРЕЧИТАТЬ YAML / ARCHIVE", 24, 300, 300, COLORS.blue)
    button("Material", panel, "F11 · МЕНЯТЬ NMATERIAL", 342, 300, 300, COLORS.green)
    button("Mesh", panel, "MESH HOT-SWAP", 24, 360, 300, COLORS.blue)
    button("Tracer", panel, "F10 · STOCK TRACER", 342, 360, 300, COLORS.amber)
    label("Footer", panel,
        "Typed userdata · owner cleanup · no raw DAVA pointers · safe reviewed factory",
        24, 438, 642, 48, 15, COLORS.muted)
    subscribe_ui(controls.OpenButton, function() set_open(true) end)
    subscribe_ui(controls.Close, function() set_open(false) end)
    subscribe_ui(controls.Reload, load_native_resources)
    subscribe_ui(controls.Material, mutate_material)
    subscribe_ui(controls.Mesh, hot_swap_mesh)
    subscribe_ui(controls.Tracer, spawn_camera_tracer)
    layout()
    set_open(false)
    return true
end

local function unmount()
    set_cursor_unlocked(false)
    for index = #ui_subscriptions, 1, -1 do
        wotb.ui.event_unsubscribe(ui_subscriptions[index])
    end
    ui_subscriptions = {}
    for index = #control_order, 1, -1 do control_order[index]:destroy() end
    controls = {}
    control_order = {}
    root = nil
    release_handle(screen)
    screen = nil
    mounted = false
    open = false
end

local function mount()
    if mounted then return true end
    screen = wotb.ui.get_active_screen()
    if screen == nil then return false end
    viewport_width, viewport_height = read_viewport()
    if not build_ui() then unmount(); return false end
    mounted = true
    load_native_resources()
    return true
end

local function refresh_context()
    local value = wotb.context.current()
    if type(value) == "number" then context_mask = value end
    local visible = wotb.context.should_show(
        VISIBLE_CONTEXTS, BLOCKED_CONTEXTS, context_mask)
    context_dirty = false
    if not visible and mounted then unmount() end
    if visible and not mounted then mount() end
end

local function subscribe_events()
    local topics = {wotb.events.TOPIC_UI_SCREEN_CHANGED,
                    wotb.events.TOPIC_BATTLE_ENTERED,
                    wotb.events.TOPIC_BATTLE_LEFT}
    for _, topic in ipairs(topics) do
        local current_topic = topic
        local token, err = wotb.events.subscribe(current_topic, function(event)
            if type(event) == "table" and type(event.context_mask) == "number" then
                context_mask = event.context_mask
            end
            context_dirty = true
            if current_topic == wotb.events.TOPIC_UI_SCREEN_CHANGED then
                screen_rebind_pending = true
            end
        end, wotb.events.PRIORITY_NORMAL, true)
        if token then event_subscriptions[#event_subscriptions + 1] = token
        else log_error("event subscribe", err) end
    end
end

function on_enable()
    subscribe_events()
    refresh_context()
end

function on_frame(frame_index)
    if context_dirty or frame_index % 15 == 0 then refresh_context() end
    if not mounted then return end
    flush_native_results()
    if screen_rebind_pending then
        screen_rebind_pending = false
        unmount()
        mount()
    end
    if frame_index % 60 == 0 then
        viewport_width, viewport_height = read_viewport()
        layout()
    end
    if type(wotb.input) == "table" and
            type(wotb.input.hotkey_pressed) == "function" then
        local f9 = wotb.input.hotkey_pressed(wotb.input.KEY_F9 or 0x78)
        if f9 == true then set_open(not open) end
        local f10 = wotb.input.hotkey_pressed(wotb.input.KEY_F10 or 0x79)
        if f10 == true then spawn_camera_tracer() end
        local f11 = wotb.input.hotkey_pressed(wotb.input.KEY_F11 or 0x7A)
        if f11 == true then mutate_material() end
    end
    if open then set_cursor_unlocked(true) end
end

function on_disable()
    for index = #event_subscriptions, 1, -1 do
        wotb.events.unsubscribe(event_subscriptions[index])
    end
    event_subscriptions = {}
    unmount()
    release_dava(tracer)
    release_dava(mesh)
    release_dava(mesh_consumer)
    release_dava(material)
    release_handle(archive)
    release_handle(yaml_document)
    tracer, mesh, mesh_consumer, material = nil, nil, nil, nil
    archive, yaml_document = nil, nil
end
