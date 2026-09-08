# Жизненный цикл

Скрипт может объявить три глобальные функции:

```lua
function on_enable()
    print("enabled")
end

function on_frame(frame_index, delta_seconds)
    -- frame_index: целое значение native lifecycle
    -- delta_seconds: длительность кадра
end

function on_disable()
    print("disabled")
end
```

Порядок запуска: top-level chunk, затем optional `on_enable()`, затем optional
`on_frame(frame_index, delta_seconds)` на каждом native frame. `on_disable()`
предлагается один раз перед обычной выгрузкой, hot reload и выключением host-а.

Ошибка top-level или `on_enable` не включает скрипт. Обычная ошибка `on_frame`
отключает скрипт и после этого вызывает его `on_disable`. Превышение instruction
budget отключает скрипт без нового входа в Lua, поэтому `on_disable` в этом
случае пропускается.
