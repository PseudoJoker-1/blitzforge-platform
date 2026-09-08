# Lua Skin Switcher

Список скинов читается из `skins.json` рядом со скриптом
(`wotb.files.read_json("mod://self/skins.json")`), пакет регистрируется и
применяется к своей машине через `wotb.vehicle.skin` (с честным
`requires_model_reload`), F9 (`wotb.keys`) переключает на следующий,
выбор хранится через `wotb.store`. Пример `skins.json` в папке —
заготовка без assets; заполните `assets` по разделу
`vehicle_visual` в API_V3_RU.md.

Шаблон: `wotbmod new ... --type lua --template vehicle`.
