# Loader-private `wotb.dava`

`wotb.dava` появляется только когда Lua host видит `wotb_mod_loader.dll` с
DAVA native bridge exports. Таблица не выдаёт raw pointers, vtable, provider
token или произвольный `DAVA::ObjectFactory`. Каждый созданный object — typed
userdata, принадлежащий текущему Lua script; его можно передать только в
операцию, которая ожидает тот же kind. Teardown script-а освобождает забытые
DAVA handles через тот же release path.

Feature detection:

```lua
if wotb.dava and wotb.dava.is_supported("material") then
    print("DAVA material bridge is available")
end
```

Допустимые feature names: `yaml`, `archive`/`resource_archive`, `material`/
`nmaterial`/`texture`, `mesh`/`mesh_hot_swap`, `tracer`/`stock_tracer`,
`class_factory`/`object_factory`. Возврат `true` означает только наличие
capability в loader-private bridge. Для exact client `11.19.0.834` результат
всех перечисленных typed routes подтверждён live. Это не разрешает raw DAVA:
неизвестный class name по-прежнему отклоняется политикой reviewed allowlist.

Reviewed class allowlist:

```lua
assert(wotb.dava.class_is_registered("DAVA::UIControl"))
assert(not wotb.dava.class_is_registered("DAVA::Unreviewed"))

local control, err = wotb.dava.class_create(
    "DAVA::UIControl",
    wotb.dava.OBJECT_CLASS_INSTANCE)
if not control then
    print(err)
    return
end

assert(wotb.dava.release(control))
```

Текущий allowlist: `DAVA::UIControl`, `DAVA::Entity`, `DAVA::NMaterial`,
`DAVA::Texture`, `DAVA::Mesh` и `DAVA::MeshConsumer`. Любой другой class name
возвращает `nil, message`; arbitrary raw DAVA classes остаются недоступны
намеренно.

Material, texture и mesh:

```lua
if not (wotb.dava and wotb.dava.is_supported("mesh")) then
    return
end

local material = assert(wotb.dava.create_material("lua.probe"))
local texture = assert(wotb.dava.create_texture("mod://fixtures/probe.tex"))
local mesh = assert(wotb.dava.create_mesh("mod://fixtures/replacement.sc2"))
local consumer = assert(wotb.dava.create_mesh_consumer("mod://fixtures/base.sc2"))

assert(wotb.dava.material_set_property(material, "roughness", 0.5))
assert(wotb.dava.material_set_property(
    material,
    "tint",
    {1, 0.5, 0.25, 1},
    "float4"))
assert(wotb.dava.material_set_flag(material, "BLENDING", 1))
assert(wotb.dava.material_set_texture(material, "albedo", texture))
assert(wotb.dava.material_set_fx(material, "NormalizedBlinnPhong"))
assert(wotb.dava.material_set_quality(material, "High"))
assert(wotb.dava.material_apply(material, consumer))
assert(wotb.dava.mesh_hot_swap(consumer, mesh))

assert(wotb.dava.release(mesh))
assert(wotb.dava.release(consumer))
assert(wotb.dava.release(texture))
assert(wotb.dava.release(material))
```

`create_texture`, `create_mesh` и `create_mesh_consumer` дополнительно требуют
`resources.mod`, потому что host сначала резолвит `mod://`/`game://` URI через
VFS. Остальные `wotb.dava` functions требуют `gameplay.tweak.cosmetic`.

Stock tracer:

```lua
if wotb.dava and wotb.dava.is_supported("stock_tracer") then
    local tracer, err = wotb.dava.create_stock_tracer({
        origin = {0, 1, 2},
        destination = {10, 3, 4},
        shell_type = 2,
    })
    if tracer then
        assert(wotb.dava.release(tracer))
    else
        print(err)
    end
end
```

`create_stock_tracer` работает только при активном battle route и возвращает
typed completion token. Stock tracer manager, visual node и style object не
переходят в Lua.
