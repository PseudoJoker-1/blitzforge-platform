local first_frame = true

function on_enable()
    print("example.lua_hello enabled")

    local ok, err = wotb.storage.set_json("state", "{\"enabled\":true}")
    if not ok then
        print("could not save state: " .. tostring(err))
    end
end

function on_frame(frame_index, delta_seconds)
    if first_frame then
        first_frame = false
        print("first frame " .. frame_index .. ", dt=" .. delta_seconds)
    end
end

function on_disable()
    print("example.lua_hello disabled")
end
