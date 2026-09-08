# Generated DAVA loaders

`wotb.loaders.load_dava_yaml` и `wotb.loaders.open_dava_archive` создаются
обычным генератором Lua bindings из `loaders_v1.h`. Они требуют таблицу
`wotb.loaders`, permission `resources.mod` и native DAVA capability в loader.

```lua
local document, err = wotb.loaders.load_dava_yaml("mod://fixtures/probe.yaml")
if not document then
    print(err)
    return
end

local root = assert(wotb.yaml.get_root(document))
print("root node: " .. tostring(root))
assert(wotb.handles.release(document))
```

```lua
local archive, err = wotb.loaders.open_dava_archive("mod://fixtures/probe.pack")
if not archive then
    print(err)
    return
end

local count = assert(wotb.archive.get_entry_count(archive))
print("entries: " .. tostring(count))
assert(wotb.archive.cancel(archive))
```

Эти snippets показывают форму вызовов и error handling. Exact-client маршруты
native DAVA YAML/archive, material/texture, mesh hot-swap и stock tracer
подтверждены live на `11.19.0.834`; после обновления клиента fingerprint gate
закроет их до нового binding pack и повторной проверки.
