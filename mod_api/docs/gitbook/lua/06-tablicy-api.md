# Таблицы API

Каждый представимый интерфейс доступен как `wotb.<имя>`. Одна такая таблица
собирается из двух независимых источников:

- функции интерфейса регистрируются только если клиент ответил на
  `query_interface`. Если интерфейс не опубликован, ни одной функции в таблице
  нет;
- константы интерфейса регистрируются безусловно — см. «Константы из
  заголовков». Константа описывает заголовки, против которых собран host, а не
  capability клиента.

Из-за второго пункта существование `wotb.<имя>` больше не доказывает, что
интерфейс опубликован; как проверять это правильно, описано в разделе
«Опубликован, разрешён, поддержан».

Если интерфейс есть, но конкретный optional slot равен null, вызов возвращает
`nil, "...the client did not publish this slot"`. `unsafe_native` — единственное
исключение: его единственный slot требует raw pointer, а собственных констант у
него нет, поэтому таблица не создаётся вовсе.

| Lua table | Слотов | Lua table | Слотов |
| --- | ---: | --- | ---: |
| `core` | 8 | `capabilities` | 5 |
| `permissions` | 4 | `handles` | 4 |
| `lifecycle` | 13 | `hooks` | 16 |
| `unsafe_native` | 1 | `events` | 9 |
| `ui` | 74 | `settings` | 19 |
| `storage` | 14 | `input` | 12 |
| `vfs` | 14 | `resources` | 11 |
| `async` | 20 | `http` | 13 |
| `intermod` | 11 | `render` | 19 |
| `render_native` | 3 | `camera` | 14 |
| `scene` | 28 | `audio` | 49 |
| `vehicle_visual` | 35 | `gameplay_camera` | 30 |
| `gameplay_hud` | 34 | `gameplay_hangar` | 11 |
| `gameplay_replay` | 9 | `entity_public` | 8 |
| `bigworld_rpc` | 3 | `projectile` | 14 |
| `yaml` | 11 | `archive` | 10 |
| `loaders` | 7 | `client` | 5 |
| `device` | 3 | `diagnostics` | 8 |
| `devtools` | 15 | `manifest` | 15 |
| `catalog` | 3 | `content` | 7 |
| `ui_read` | 4 | `camera_state` | 3 |
| `audio_intercept` | 4 | `scene_enumerate` | 2 |
| `tracer` | 2 | `session_cluster` | 3 |
| `ges` | 14 | рукописная таблица `wotb.ges` (`loader/lua/lua_bind_ges.cpp`): `types()`, `subscribe(pattern, fn)`, `unsubscribe(h)` за `ges.observe`, `publish(type, fields[, flags])` за `ges.publish`. Подписка едет поверх `wotb.events.subscribe` (нужен и `events.public`); обработчик получает объект события с полями `type/size/schema/publisher_rva` и методами `i32/u32/f32/bool/ptr/str(offset)`, `field(name)`, `expired()`, который действителен только внутри вызова | |

Последние пять интерфейсов добавлены новым релизным контрактом от 16 августа
2026 года. Замороженные таблицы при этом не расширялись — рядом опубликованы
новые версии, ровно как `ui_v2` и `ui_v3` уже сосуществуют. Пока под ними нет
backend'а, каждый слот честно отвечает `E_NOT_SUPPORTED`, а runtime
регистрирует интерфейс как `UNAVAILABLE` — то есть на клиенте без backend'а
таблица функций вообще не создаётся, и это правильный ответ, а не ошибка.
Константы этих интерфейсов публикуются всегда: константа — факт времени
компиляции, а не capability клиента.

Названия функций совпадают с именами слотов C ABI. Например,
`WotbModV3CameraApiV1::get_state` становится `wotb.camera.get_state(...)`.
Аргументы сохраняют порядок ABI за исключением скрытого `mod` и выходных
параметров. Полный смысл каждого слота описан в [документации V3](../reference/api-v3.md)
и соответствующем заголовке `include/wotbmod/*.h`.
