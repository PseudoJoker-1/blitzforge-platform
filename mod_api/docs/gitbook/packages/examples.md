# Примеры модов

Каждый пример — готовая папка `mod_api/examples/<имя>` с `manifest.json`; Lua-примеры копируются в `mods/lua/<id>` игры или собираются в пакет командой `wotbmod release`.

| Пример | Что показывает |
| --- | --- |
| [`api_selftest_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/api_selftest_mod) |  |
| [`cluster_picker`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/cluster_picker) | Пара пакетов: |
| [`cluster_picker_ui`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/cluster_picker_ui) | Импортировано командой `wotbmod import` из `mirror`: 1 файл(ов) игры. |
| [`custom_audio_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/custom_audio_test_mod) |  |
| [`hello_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/hello_mod) |  |
| [`lua_ally_tracker`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_ally_tracker) | Боевой Lua-мод, который показывает только публичные данные союзной команды: |
| [`lua_battle_telemetry`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_battle_telemetry) | Практический боевой event feed на Lua. Он показывает последние типизированные |
| [`lua_damage_log`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_damage_log) | Что по вам попало, на сколько и что это сделало с вашим HP. То, что игрок обычно |
| [`lua_dava_workshop`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_dava_workshop) | Обычный интерактивный мод, а не PASS/FAIL self-test. Он показывает состояние |
| [`lua_facade_battle`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_facade_battle) | Телеметрия боя на фасадах: `wotb.battle.on_shot/on_hit/on_damage/ |
| [`lua_facade_panel`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_facade_panel) | Панель из динамических контролов `wotb.panel` (`label`, `row` с кнопками, |
| [`lua_facade_tour`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_facade_tour) | Пример на фасадном слое Lua API: ни одна raw-таблица (`wotb.gameplay_hud`, |
| [`lua_hello`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_hello) | Скопируйте всю папку как |
| [`lua_host`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_host) | Это native-пакет, который запускает установленные Lua-моды из |
| [`lua_hud_tweaks`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_hud_tweaks) | Цвет и размер прицела, размер миникарты, цвет индикатора попадания, позиция |
| [`lua_reload_timer`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_reload_timer) | Компактная боевая панель перезарядки и боекомплекта. Показывает остаток |
| [`lua_session_stats`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_session_stats) | Полноценная ангарная панель статистики за текущий запуск клиента. Мод |
| [`lua_skin_switcher`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_skin_switcher) | Список скинов читается из `skins.json` рядом со скриптом |
| [`lua_ui_framework`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/lua_ui_framework) | Тестовый Lua-мод полностью создаёт интерфейс во время работы. Авторский YAML |
| [`native_live_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/native_live_test_mod) |  |
| [`native_validation_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/native_validation_mod) | Внутренний V3-мод для ручного live-прогона уже реализованного API. Пакет |
| [`new_gameplay_events_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/new_gameplay_events_test_mod) |  |
| [`night_mode`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/night_mode) | Пакет ресурсов (`"type": "resource"`): заменяет два шейдера цветокоррекции в |
| [`object260_t3485_model_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/object260_t3485_model_mod) |  |
| [`sample_camera_render`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/sample_camera_render) | RC1-пример portable render/camera. Он регистрирует managed `AFTER_UI` callback, |
| [`sample_ui_transaction`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/sample_ui_transaction) | RC1-пример инспекции и транзакционного изменения существующего игрового |
| [`sample_vehicle_cosmetic`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/sample_vehicle_cosmetic) | RC1-пример транзакционного cosmetic pack конкретного танка по точным путям. |
| [`v3_client_resources_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/v3_client_resources_test_mod) | Этот DLL-мод проверяет 17 интерфейсов client/resources/gameplay. |
| [`v3_core_runtime_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/v3_core_runtime_test_mod) | Этот DLL-мод проверяет bootstrap и 23 интерфейса core/runtime/data. |
| [`vehicle_skin_test_mod`](https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/vehicle_skin_test_mod) |  |

