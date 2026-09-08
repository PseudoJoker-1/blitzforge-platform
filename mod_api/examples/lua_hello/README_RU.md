# Lua Hello Example

Скопируйте всю папку как
`<game>\mods\lua\example.lua_hello`. Имя папки обязано совпадать с `id` в
`manifest.json`. Host загрузит `main.lua` при включении и не будет перечитывать
установленный мод до следующего цикла disable/enable.

Для hot reload копируйте только `main.lua` в `<game>\mods\lua-dev`; в dev-режиме
manifest не используется, а скрипт получает измеренный потолок host-а.
