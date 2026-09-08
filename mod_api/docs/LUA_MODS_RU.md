# Lua-моды BlitzForge (WotbMod V3 ABI)

Lua host встраивает Lua 5.4.7 в обычный 32-битный native-мод и даёт скриптам
доступ к замороженному WotbMod V3 ABI без компилятора и без FFI.

Новичку: начните с [QUICKSTART_RU.md](QUICKSTART_RU.md) (мод из шаблона за
15 минут), держите под рукой [API_REFERENCE_RU.md](API_REFERENCE_RU.md)
(сгенерированный справочник по каждому слоту с правами) и подключите
`sdk/vscode/wotbmod.code-snippets` в VS Code; схемы для `manifest.json` и
для `wotb.config` лежат в `schemas/`.

Текущий структурный инвентарь — 622 слотов в 47 интерфейсах (сверено
8 сентября 2026 по `python tools/generate_lua_bindings.py --report`):

- 51 слот `storage`, `events`, ядра `ui` и `ges` имеет ручные
  ergonomic-биндинги;
- 569 слотов создаётся генератором из V3-заголовков;
- 2 слота намеренно не представлены в Lua, потому что требуют сырого
  нативного указателя: `unsafe_native.create_address_hook` и
  `intermod.export_interface`.

Итого Lua-автору доступны 620 из 622 слотов. Публичный C ABI при этом не
изменён (`WOTBMOD_V3_ABI_VERSION` = `0x00030000`): новые интерфейсы публикуются
рядом с замороженными таблицами, а не расширяют их.

Кроме функций генератор публикует 611 констант из V3-заголовков в 42 таблицах
`wotb.*` (см. «Константы из заголовков»), а host устанавливает convenience-модули
на чистом Lua: `wotb.context`, `wotb.players`, `wotb.log`, `wotb.json`,
`wotb.timer`, `wotb.battle`, `wotb.config`, `wotb.available`, `wotb.panel`,
`wotb.mod`, `wotb.hud`, `wotb.screen`, `wotb.vehicle`, `wotb.shells`,
`wotb.view`, `wotb.sound`, `wotb.keys`, `wotb.store`, `wotb.files` и расширения
`wotb.ges` (см. «Фасады»). Ни константы, ни эти модули не расширяют C ABI и не
добавляют ни одного permission.

Отдельно host может добавить loader-private таблицу `wotb.dava`. Это не часть
frozen public C ABI и не FFI: скрипт получает только typed userdata handles,
которые loader проверяет по owner, kind и lifetime.

## Установка host-а и готового Lua-мода

Игроку и автору мода собирать ничего не нужно: установщик
`BlitzForge-Setup-<версия>.exe` с [blitz-forge.org/download](https://blitz-forge.org/download)
кладёт loader и подписанный Lua host в игру сам, а готовые моды ставятся
кнопкой «Установить в игру» или командой `wotbmod install <id> --catalog
https://blitz-forge.org/api/v1`. Ниже — сборка тех же артефактов из исходников
SDK, она нужна только тем, кто правит host.

Собрать и проверить native host package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lua_host_package.ps1
```

Собрать подписанный public-preview bundle с безопасными install/verify/
uninstall scripts:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_public_preview.ps1 -SkipBuild
```

Перед `-SkipBuild` должны успешно завершиться `build.cmd`,
`proxy_dll\build.cmd` и `loader\build_live.cmd`. Без `-SkipBuild` release script
выполнит их самостоятельно. Приватный ECDSA P-256 ключ создаётся как
неэкспортируемый user key в Windows CNG; в bundle попадают только detached
signature и публичный X||Y trust key.

Команда сначала прогоняет Lua vendor/host tests, проверяет таблицу экспортов
release DLL, затем создаёт и валидирует:

```text
build\lua_host_packages\wotbmod.lua_host.wotbmod
build\lua_host_distribution\
├── wotbmod.lua_host.wotbmod
└── mods\lua\
    ├── example.lua_hello\...
    ├── example.lua_ui_framework\...
    ├── example.lua_ally_tracker\...
    ├── example.lua_battle_telemetry\...
    └── example.lua_session_stats\...
```

Положите `wotbmod.lua_host.wotbmod` в обычный каталог пакетов BlitzForge
`<game>\mods`. Установленный Lua-мод — это распакованная папка:

```text
<game>\mods\lua\example.lua_hello\
├── manifest.json
└── main.lua
```

Имя папки обязано совпадать с `id` без учёта регистра. Reparse-point каталоги
не сканируются. Установленный скрипт читается один раз на цикл enable/disable и
не перезагружается при изменении файла.

Готовый `example.lua_ui_framework` добавляет активную кнопку и полностью
создаёт окно во время работы без авторского YAML. Внутри есть native text,
image, buttons, `UITextField`, два drag-слайдера, toggle, scroll/list с восемью
клонированными строками и Lua-анимация. Мод меняет text/font/color/opacity/
texture/geometry/style и полностью освобождает callbacks, style overrides и
UI handles при `on_disable` или смене active screen.

Два других установленных примера являются не диагностическими матрицами, а
обычными игровыми модами:

- `example.lua_ally_tracker` показывает public ID/team/name/HP союзников,
  время последнего публичного обновления и последнюю позицию только при
  `position_available = true`;
- `example.lua_battle_telemetry` ведёт управляемый журнал типизированных
  damage/reload/ammo/camera/shot/hit событий;
- `example.lua_session_stats` создаёт в ангаре Blitz-панель статистики за
  текущий запуск клиента: история восьми боёв, полученный урон, локальные
  выстрелы, выживание, длительность и результат только при подтверждённом
  `winner_team`. Недоступные damage dealt/kills/hits показываются как `N/A`.
  Native loader публикует `HANGAR` по наличию уникального DAVA-маркера и
  `MOD_SCREEN` по видимости каталога,
  поэтому Lua-моду не нужен собственный доступ к дереву игровых контролов для
  определения видимости. Сам session-мод сохраняет уже подтверждённый корень
  ангара при кратковременном контексте `NONE`, но сбрасывает его на
  `LOADING`/`BATTLE`/`REPLAY`/`TRAINING`/`RESULTS` или при уничтожении root.

Боевые UI-моды удаляют controls вне боя; session statistics, наоборот,
существует только в ангаре. Все четыре всегда блокируются при
`MOD_SCREEN`/`TEXT_INPUT`, поэтому не перекрывают каталог модов.

## Manifest установленного Lua-мода

Минимальный manifest:

```json
{
  "id": "example.my_mod",
  "entrypoint": "main.lua",
  "permissions": [
    "core",
    "storage"
  ]
}
```

Правила production scanner-а:

- `id` обязателен: ASCII-буквы, цифры, `.`, `_`, `-`; начало и конец — буква
  или цифра; `..` запрещён;
- `entrypoint` по умолчанию равен `main.lua`; разрешён только один ASCII-файл
  `.lua` в корне мода, без каталогов и traversal;
- `permissions` — массив строк; дублирование полей `id`, `entrypoint` или
  `permissions` делает manifest недействительным;
- JSON читается по точному размеру: embedded NUL и trailing data не скрывают
  вторую часть документа;
- неизвестное plain-text permission не даёт прав; escape-обфускация permission
  и malformed JSON отклоняют manifest целиком;
- дополнительные metadata-поля (`name`, `version` и другие) допустимы и
  игнорируются host-ом.

Два мода с одним `id` или одним путём не запускаются. Проверка дубля выполняется
до компиляции и выполнения top-level Lua-кода.

## Режим разработки и hot reload

Для разработки положите отдельные `.lua`-файлы в:

```text
<game>\mods\lua-dev
```

Путь можно переопределить до запуска игры:

```powershell
$env:WOTBMOD_LUA_DEV_DIR = 'D:\mods\lua-dev'
```

Production root аналогично переопределяется через `WOTBMOD_LUA_MOD_DIR`.

Dev-файлы получают весь реально измеренный потолок прав host-а и следятся через
`ReadDirectoryChangesW` с debounce около 200 мс. При сохранении старая версия
полностью выключается и освобождает ресурсы, затем компилируется новая. Если в
новой версии синтаксическая или runtime-ошибка, старая версия не возвращается.

Dev и installed режимы имеют одинаковые таблицы и функции. Различаются только
источник permissions и наличие hot reload.

## Жизненный цикл

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

## Sandbox

Доступны base library и `table`, `string`, `math`. Не доступны `io`, `os`,
`package`, `debug`, `coroutine`, `utf8`, `require`, `dofile`, `loadfile`,
`collectgarbage` и `warn`.

Sandbox-версия `load(chunk [, chunkname [, mode [, env]]])` принимает только
строку исходного текста. Reader function и bytecode отклоняются. Аргументы
`mode` и `env` принимаются для совместимости, но игнорируются: используется
только text mode и окружение текущего скрипта.

`print(...)` с permission `core` пишет через `wotbmod.core.log`; без него
остаётся только debugger sink с именем скрипта.

## Таблицы API

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
параметров. Полный смысл каждого слота описан в [документации V3](API_V3_RU.md)
и соответствующем заголовке `include/wotbmod/*.h`.

## Константы из заголовков

Генератор публикует 611 констант в 42 таблицах `wotb.*`. Генератор при этом не
разбирает ни одного C-выражения: он выписывает само C-имя
(`SetConstant(state, "MODE_SNIPER", WOTBMOD_V3_CAMERA_MODE_SNIPER)`), а значение
вычисляет компилятор host-а по тем же заголовкам, поэтому разойтись с ними оно
не может. Сравнивайте с `wotb.camera.MODE_SNIPER`, а
не с `3`.

Правило имени: с C-имени снимается префикс `WOTBMOD_V3_`, затем применяется
таблица префиксов **этого интерфейса**; побеждает самый длинный совпавший
префикс. Правило не глобальное и не выводится из имени таблицы: `CAMERA_MODE_*`
объявлен в `camera_v1.h`, а `CAMERA_TRANSITION_*` — в `gameplay_camera_v1.h`, и
одно глобальное правило их не различит.

| C-имя | Lua-имя |
| --- | --- |
| `WOTBMOD_V3_CAMERA_MODE_SNIPER` | `wotb.camera.MODE_SNIPER` |
| `WOTBMOD_V3_UI_CONTROL_BUTTON` | `wotb.ui.CONTROL_BUTTON` |
| `WOTBMOD_V3_VFS_MOUNT_OVERLAY` | `wotb.vfs.MOUNT_OVERLAY` |
| `WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE` | `wotb.entity_public.TYPE_VEHICLE` |
| `WOTBMOD_V3_LOG_WARNING` | `wotb.core.LOG_WARNING` |
| `WOTBMOD_V3_ARCH_X86` | `wotb.device.ARCH_X86` |
| `WOTBMOD_V3_STORAGE_KEY_MAX` | `wotb.storage.KEY_MAX` |

Два интерфейса намеренно сохраняют собственный префикс. `core` оставляет `LOG_`:
без него получилось бы `wotb.core.ERROR` рядом с Lua-шным `error`, а
`wotb.core.DEBUG`/`INFO`/`FATAL` не сообщают, что это уровень лога. `device`
оставляет `ARCH_`: голые `UNKNOWN`/`X86`/`X64`/`ARM32`/`ARM64` на таблице,
которая заодно отдаёт объём памяти и размер экрана, нечитаемы.

`wotb.events` разводит две группы, которые `events_v1.h` называет одинаково для
22 понятий (`BATTLE_STARTED`, `UI_INPUT`, `SHOT_FIRED` и так далее):

- `TOPIC_*` — строка topic, которую принимает `wotb.events.subscribe`;
- `TYPE_*` — числовой `WotbModV3ClientEventType`, лежащий в `event.data.type`.

Это ровно то разделение, которое уже делал ручной биндинг. Слить их обратно в
один префикс нельзя: генератор отказывается выдать два одинаковых имени на одной
таблице и завершается ненулевым кодом. Молча победившая вторая константа сделала
бы каждое сравнение с первой навсегда ложным, а такое сравнение не падает и не
логируется.

`wotb.ui` разводит два выравнивания из `ui_v2.h`.
`ALIGN_LEFT`/`CENTER`/`RIGHT`/`JUSTIFY` — это `WotbModV3UiTextAlignment`,
выравнивание текста; имя сохранено, потому что так его публиковал ручной биндинг
и так написаны установленные примеры. `LAYOUT_ALIGN_START`/`CENTER`/`END`/
`STRETCH` — это `WotbModV3UiAlignment`, поперечное выравнивание managed layout;
оно столкнулось бы на `ALIGN_CENTER`, поэтому получило однозначный префикс и
стоит рядом с `LAYOUT_FLEX` и `LAYOUT_GRID`.

Размерные пределы — не словарь: `WOTBMOD_V3_STORAGE_KEY_MAX` говорит, какой
длины бывает ключ, но не называет одно из значений закрытого множества. Поэтому
они подключаются по одному вручную. Сейчас опубликовано 25 штук:
`wotb.storage.KEY_MAX`, `wotb.events.MAX_PAYLOAD`, `wotb.vfs.URI_MAX`,
`wotb.http.ABSOLUTE_MAX_RESPONSE` и другие. Любой новый `MAX_*` в заголовке не
появляется в Lua, пока его туда не внесли явно.

Констант не получают пять интерфейсов. `handles`, `archive` и `devtools` —
потому что их заголовки констант не объявляют. `render_native` — потому что
`render_v1.h` целиком отдан `render`: `render_native` это три слота для
native-указателя, а не отдельный словарь, а константа с двумя домами — это
константа, написание которой скрипту пришлось бы угадывать. У `unsafe_native`
нет ни констант, ни таблицы.

### Что не публикуется намеренно

77 констант из `base.h` в Lua не попадают.

- 24 кода `WotbModV3Result`. Вся Lua-конвенция — `return nil, message`.
  Числовые коды пригласили бы писать `if err == wotb.core.E_TIMEOUT`, то есть
  второй, более слабый протокол ошибок, конкурирующий с первым. Конкурирующий
  протокол хуже, чем его отсутствие: сообщение остаётся строкой, а сравнение с
  кодом молча перестаёт срабатывать везде, где ошибка переформулирована слоем
  выше.
- 34 значения `WotbModV3HandleType`. Скрипт не видит сырой handle: handles
  приходят типизированным userdata, тип проверяет host. Сравнение с числовым
  тегом было бы чтением факта, который изнутри sandbox получить нельзя.
- 10 значений `WotbModV3GameContext`. Они уже опубликованы как
  `wotb.context.HANGAR`/`BATTLE`/… Второе написание того же значения — ровно та
  путаница, ради устранения которой генератор и существует.
- 9 `#define` из `base.h`. `WOTBMOD_V3_INVALID_HANDLE` — сырое значение handle;
  остальные — пределы маршалинга host-а, а не пределы, в которые упирается
  скрипт. Единственный, в который упереться можно, доступен как
  `wotb.vfs.URI_MAX`.

Три группы из `base.h`, наоборот, размещены вручную, потому что описывают именно
то, о чём говорит принимающая таблица: `WotbModV3ThreadRole` →
`wotb.events.THREAD_*` (это содержимое `event.thread_role`),
`WotbModV3CapabilityStatus` → `wotb.capabilities.STATUS_*`,
`WotbModV3PermissionTier` → `wotb.permissions.TIER_*`.

## Опубликован, разрешён, поддержан

Раньше здесь было написано, что таблица неопубликованного интерфейса не
создаётся, и авторы писали `if wotb.settings then`. С появлением
сгенерированных констант это перестало быть верным: таблица констант создаётся
безусловно, поэтому `wotb.settings` существует на любом клиенте и содержит 18
констант даже там, где интерфейса settings нет вовсе. То же касается всех 35
таблиц с константами.

Надёжная проверка спрашивает функцию, а не таблицу, и под этот вопрос есть
отдельный примитив — `wotb.available(name)`:

```lua
if wotb.available("settings") then
    -- у wotb.settings есть хотя бы один вызываемый слот
end
```

Он описан ниже, в «Convenience-модули → `wotb.available`». Ручная проверка
(`type(wotb.settings) == "table" and type(wotb.settings.get_int) == "function"`)
делает ровно то же самое и остаётся верной; примитив лишь избавляет от
необходимости угадывать, какой слот назвать в проверке.

Три вопроса, которые легко перепутать:

- **опубликован ли интерфейс** — клиент ответил на `query_interface`, функции в
  таблице есть. Проверяется так, как выше;
- **разрешён ли вызов** — у скрипта есть нужные permissions. Функции лежат в
  таблице независимо от manifest-а, и отказ приходит из самого вызова как
  `nil, "permission denied: <name>"`. Поэтому `wotb.available` на этот вопрос не
  отвечает и ответить не может: permission не меняет форму API.
  `wotb.permissions.query(...)` отвечает про
  внешний grant host-пакета, а не про внутренний grant скрипта: native runtime
  видит один handle `wotbmod.lua_host`, поэтому эта функция подтверждает
  потолок, а не ваши права;
- **поддержан ли backend** — slot существует и разрешён, но реального
  безопасного backend-а под ним нет. Такой вызов возвращает
  `nil, "<слот>: not supported by this client build"`.

## Типизированные игровые события

`wotb.events.subscribe` по-прежнему передаёт исходный binary payload в
`event.payload`, но для известных native topics дополнительно создаёт
`event.data`. Декодирование выполняется только при точном совпадении topic,
версии и полного размера структуры; повреждённый или укороченный payload даёт
`data = nil`, а raw bytes сохраняются для совместимости.

Аргументы `subscribe` позиционные:
`subscribe(topic, callback [, priority [, receive_system_events]])`. Клиентские
topics публикует сам клиент, поэтому четвёртый аргумент для них обязан быть
`true`.

```lua
local token, err = wotb.events.subscribe(
    wotb.events.TOPIC_DAMAGE_RECEIVED,
    function(event)
        local data = event.data
        if data then
            print("Получен урон: " .. tostring(data.damage))
            print("HP: " .. tostring(data.previous_health) .. " -> " ..
                  tostring(data.health))
        end
    end,
    wotb.events.PRIORITY_NORMAL,
    true)
if token == nil then print(err) end
```

Полного HP в этом payload нет: `WotbModV3DamageEventData` несёт `damage`,
`previous_health`, `health`, `reason_code` и `source_entity_id`, и ничего
больше. Максимальное HP берётся из `wotb.players.local_player().max_health` —
это другой источник, и учитывать его нужно как другой.

**Два последних поля на текущей сборке клиента всегда нули, и это не «пока
никто не бил».** Единственный производитель `wotbmod.vehicle.damaged` в дереве
(`loader/wotb_mod_loader.cpp`) обнуляет структуру и заполняет только `damage`,
`previous_health` и `health`; `reason_code` и `source_entity_id` не пишутся
никогда. Автор, прочитавший этот список без оговорки, выводит «источник 0» и
считает, что это идентификатор сущности номер ноль. Не считайте: ноль здесь
означает «поле не заполнено». Словаря значений для `reason_code` тоже нигде
нет — среди 586 сгенерированных констант нет ни одной `..._DAMAGE_REASON_*`,
так что код непрозрачен и печатать его как число бессмысленно.

**И отдельно про `max_health` у `wotb.players`.** Когда проверенное чтение
поля не удаётся, лоадер подставляет максимальное HP, которое он когда-либо
видел у этой машины. В Lua это приходит обычным числом, и отличить измеренное
значение от выведенного из максимума **невозможно**: у `wotb.players` есть
флаги `team_available`, `display_name_available` и `position_available`, но
флага для `max_health` нет. Поэтому доля HP, построенная на этом значении, —
правдоподобное число, а не наблюдение. Если показываете её игроку, пишите
рядом, откуда она взялась.

`wotb.events.TOPIC_*` содержит подтверждённые battle, vehicle, camera,
public-entity, projectile, local-shell и observed-RPC topics. `TYPE_*` содержит
22 значения client-event envelope. Для `vehicle.local.created`,
`vehicle.local.destroyed`, `gameplay.damage_received`, `sniper_entered` и
`sniper_exited` используется исходный envelope события, из которого topic был
безопасно выведен. Несуществующие источники вроде `damage_dealt` не имитируются.

## Контекст и видимость UI

Lua convenience API `wotb.context` работает поверх frozen
`wotb.core.get_context()` и не меняет публичный C ABI. Он публикует маски
`NONE`, `LOADING`, `HANGAR`, `BATTLE`, `REPLAY`, `TRAINING`, `RESULTS`,
`MOD_SCREEN`, `TEXT_INPUT`, `ALL` и функции:

- `current()` — текущая native context mask;
- `contains(value, flag)` — проверка отдельного флага;
- `should_show(allowed, blocked [, current])` — fail-closed решение о
  видимости;
- `apply_visibility(control, allowed, blocked [, current])` — применяет это
  решение к owned control и возвращает `true, visible`.

Battle-only root без каталога и текстового ввода:

```lua
local play = wotb.context.BATTLE + wotb.context.TRAINING +
    wotb.context.REPLAY
local blocked = wotb.context.MOD_SCREEN + wotb.context.TEXT_INPUT

local visible, err = wotb.context.should_show(play, blocked)
if visible == nil then
    print(err)
elseif visible then
    mount_ui()
else
    unmount_ui()
end
```

Для полного отсутствия перекрытий рекомендуемый lifecycle именно
`mount/unmount`, а не только `root:set_visible(false)`: вместе с controls
удаляются input subscriptions и active-screen handle.

## Игроки и команды

Lua convenience API `wotb.players` строится поверх
`wotb.entity_public.enumerate_visible`:

- `snapshot()` — полный снимок с `local_player`, `our_team`, `enemy_team`,
  `unknown_team`, `visible_players` и флагами полноты;
- `local_player()`, `our_team()`, `enemy_team()`, `unknown_team()`, `visible()`;
- `find(public_id)` — поиск только в текущем публичном снимке.

Каждая запись содержит handle, `public_id`, type, visible/local flags, health,
max health, alive/health_percent, public vehicle type и optional team/name.
Также добавляются `relation`, `is_local`, `is_ally`, `is_enemy` и явные
`*_available` флаги. На 11.20.0.887 подтверждены live ID, visibility/local,
health/max health, public type, team, display name (ник из ростера арены) и
поза всех публичных машин. `position_available`/`direction_available`
выводятся из данных, а не задаются константой: у машины с подтверждённой позой
направление — единичный вектор, у машины без источника — нулевой, и нулевой
вектор никогда не выдаётся за позу. Короткие имена (`me`, `allies`, `enemies`,
`by_id`, `each_visible`) и поля ростера по запросу (`details`: тег клана,
account id, фраги, имя танка) описаны в разделе «Фасады».

`enemy_team` принципиально содержит только уже видимых клиенту противников.
После unspot нелокальная entity удаляется из public registry. Если team не
подтверждён, машина попадает в `unknown_team`, а `relation_available` и
`team_data_complete` показывают, что деление на команды неполно. Скрытых enemy
HP/position/name и native pointers в Lua нет.

## Convenience-модули

Кроме `wotb.context` и `wotb.players` host устанавливает в каждый state
восемнадцать модулей на чистом Lua: `wotb.log`, `wotb.json`, `wotb.timer`,
`wotb.battle`, `wotb.config`, `wotb.available`, `wotb.panel`, `wotb.mod`,
`wotb.hud`, `wotb.screen`, `wotb.vehicle`, `wotb.shells`, `wotb.view`,
`wotb.sound`, `wotb.keys`, `wotb.store`, `wotb.files` и `wotb.session`
(`loader/lua/lua_preludes.cpp`); фасады описаны в разделе «Фасады».
Собственных permissions у них нет и быть не может: они вызывают только то, что и
так может вызвать скрипт, поэтому забор, ownership registry и teardown
продолжают работать без единой новой строки в host-е. Всё, на чём модуль стоит, он разрешает в момент вызова, а не при
загрузке: модуль, закэшировавший `wotb.core` при загрузке, был бы навсегда
сломан на клиенте, опубликовавшем интерфейс позже.

Загрузка каждого модуля считается тем же instruction budget, что и любой другой
вход в VM, поэтому на этапе загрузки они только объявляют функции: ничего не
перечисляют и ни на что не подписываются. Модуль, которому дорого объявить даже
свои функции, вместо этого ставит заглушку и достраивается при первом
обращении; сейчас такой один — `wotb.panel`. Hot reload строит новый
`lua_State`, поэтому состояние этих модулей его не переживает.

Все они проверены host-тестами. Отдельного подтверждения в запущенном клиенте
для них пока нет.

### `wotb.log`

Уровневый лог поверх `wotb.core.log`, то есть требует permission `core`.

| Функция | Возвращает |
| --- | --- |
| `log.trace/debug/info/warn/error/fatal(format, ...)` | `true` либо `nil, message` |
| `log.write(level, format, ...)` | то же; `level` — целое из `[0, 5]` |
| `log.set_category(name)` | `true` либо `nil, message`; имя не длиннее 96 байт |
| `log.category()` | текущую категорию |

Уровни доступны как `log.TRACE` … `log.FATAL` (0 … 5) и совпадают с
`WotbModV3LogLevel`. `log.warning` — то же самое, что `log.warn`; ABI пишет
`WARNING`, авторы набирают `warn`, и ни одно из написаний не является неверной
догадкой.

Без дополнительных аргументов сообщение используется дословно, поэтому
`log.info("50% готово")` безопасен: `string.format` вызывается только когда
аргументы реально переданы, и вызывается под `pcall` — неверная директива
возвращается значением, а не убивает кадр.

Категория по умолчанию — `"lua"`, тот же канал, что у `print()`. Это не имя
вашего мода: id скрипта изнутри sandbox недоступен вовсе, ни через глобальную
переменную, ни через slot ABI. Чтобы свои строки можно было отличить в общем
логе, вызовите `set_category` один раз.

```lua
wotb.log.set_category("example.my_mod")

local ok, err = wotb.log.info("включён, HP %d", 1350)
if not ok then print(err) end
```

### `wotb.json`

В замороженном ABI JSON-интерфейса нет вовсе — единственный structured-text
интерфейс это `yaml`, — а `wotb.http` отдаёт тело ответа строкой.

| Имя | Значение |
| --- | --- |
| `json.encode(value [, options])` | строка либо `nil, message` |
| `json.decode(text [, options])` | значение либо `nil, message` |
| `json.as_array(t)` | та же таблица, помеченная как массив, либо `nil, message` |
| `json.is_array(v)` | `true`/`false` |
| `json.null` | sentinel, которым представлен JSON `null` |
| `json.MAX_DEPTH` | `64` |

`options` у `encode` — `{ sorted = true }`: по умолчанию ключи объекта
сортируются, поэтому вывод воспроизводим. `options` у `decode` —
`{ max_depth = n }` для любого `n` из `[1, 64]`; потолок можно только понизить.
`encode` использует ту же границу `MAX_DEPTH` и отдельной опции не имеет.

Обе стороны работают на явном стеке и не рекурсируют: вложенность стоит heap, а
не C-стека. Lua-ошибка — это longjmp, а переполнение C-стека — не ошибка, а
процесс.

**Пустой объект против пустого массива.** `decode` помечает каждый построенный
массив, поэтому `[]` и `{}` различимы и переживают round-trip. `encode` пишет
помеченную таблицу как массив всегда, даже пустую. Непомеченная таблица пишется
массивом, только если её ключи — ровно `1..n` при `n >= 1`; непомеченная пустая
таблица поэтому даёт `{}`. Список, который вы строите сами и который может
оказаться пустым, нужно пометить:

```lua
print(wotb.json.encode({}))                      --> {}
print(wotb.json.encode(wotb.json.as_array({})))  --> []
```

**Успешный `decode` никогда не возвращает Lua `nil`.** JSON `null` становится
`json.null`, потому что запись `nil` в таблицу удаляет ключ, и декодированный
`{"a":null}` иначе молча превратился бы в `{}`. Отсюда следствие, на котором
легко обжечься: проверять результат надо через `== nil`, а не через `not`.

```lua
local value, err = wotb.json.decode(body)
if value == nil then
    wotb.log.warn("ответ не разобран: %s", err)
    return
end
if value == wotb.json.null then
    -- пришёл JSON null, а не отсутствие значения
end

-- decode("false") возвращает ровно одно значение: false. Проверка `if not
-- value` ушла бы в ветку ошибки, где err равен nil.
```

`encode` отказывает значением (не исключением) на nan и бесконечности, на
значении типа function или userdata, на таблице, ссылающейся на себя, на ключе,
который не строка и не положительное целое, на таблице, смешивающей строковые и
целые ключи или имеющей дыры в целых, и на вложенности глубже `MAX_DEPTH`.

### `wotb.timer`

| Функция | Возвращает |
| --- | --- |
| `timer.after(delay_ms, fn)` | целочисленный id либо `nil, message` |
| `timer.every(interval_ms, fn)` | то же; интервал не меньше 1 мс |
| `timer.cancel(id)` | `true` либо `nil, message` |
| `timer.cancel_all()` | число снятых таймеров |
| `timer.count()` | число живых таймеров |
| `timer.subscribed()` | держит ли модуль подписку на кадр |
| `timer.now_ms()` | часы модуля либо `nil, message` до первого кадра |

Callback вызывается как `fn(id)`.

`wotb.async.timer_create` для этого не годится: он не принимает Lua-функцию, а
второго timer slot в ABI нет. Единственные часы, до которых дотягивается скрипт,
— событие `wotbmod.frame.update`, и время берётся из его `timestamp_ns`, который
клиент штампует steady-часами. Это не счётчик кадров, и никакая частота кадров
нигде не предполагается.

**Если кадры остановились — остановились и таймеры.** Таймер, срок которого
наступил во время паузы, сработает на первом кадре после неё, с опозданием на
длину паузы; он не теряется. Повторяющийся таймер, пропустивший N периодов,
сработает **один раз** и отсчитает следующий срок от этого кадра. Догоняющая
серия вызовов — это ровно тот всплеск на кадре, ради недопущения которого модуль
и написан.

Подписка ленивая и симметричная. Пока живых таймеров нет, подписки на
`wotbmod.frame.update` не существует вовсе — не обработчик, который сразу
выходит, а отсутствие подписки, так что диспетчер клиента даже не смотрит на
этот скрипт по этому topic. Последний снятый таймер снимает и подписку;
`timer.subscribed()` позволяет это проверить, а не поверить на слово.

Причина такой строгости записана в истории проекта: поставлявшийся мод получал
`wotbmod.frame.update` примерно с частотой кадра и делал в каждом событии
дисковый I/O, а итогом была просадка FPS у игрока. Разбор — в
[API_STATUS_RU.md](API_STATUS_RU.md), раздел «Исправление просадки FPS».

Ошибка в callback-е ловится `pcall` и сообщается через `wotb.log`; остальные
таймеры этого тика отрабатывают. Таймер, созданный до первого кадра, отсчитывает
задержку от первого кадра, а не от момента вызова: «сейчас» до первого кадра не
существует, и модуль его не выдумывает.

```lua
wotb.timer.every(1000, function()
    local snap = wotb.battle.snapshot()
    if snap and snap.health then wotb.log.info("HP %d", snap.health) end
end)

function on_disable()
    wotb.timer.cancel_all()
end
```

### `wotb.battle`

Один снимок вместо одиннадцати ручных подписок. Модуль подписывается только на
topics, которые host действительно публикует, и на каждое поле помнит, откуда и
когда оно пришло.

| Функция | Возвращает |
| --- | --- |
| `battle.snapshot()` | таблицу снимка либо `nil, message` |
| `battle.start([groups])` | `true` либо `nil, message` |
| `battle.stop()` | `true` |
| `battle.reset()` | `true` |
| `battle.tracking()` | `true`/`false` |
| `battle.groups()` | отсортированный список имён групп |

Группы: `lifecycle`, `local_vehicle`, `health`, `ammo`, `reload`, `camera`,
`damage`. Подписка ленивая: до первого `start()` или `snapshot()` не создаётся
ни одной. Запрос группы `health` автоматически включает `local_vehicle` — без
entity id локальной машины HP приписать некому.

Снимок содержит:

| Ключ | Что это |
| --- | --- |
| `values` | только реально наблюдавшиеся поля |
| `source` | topic, из которого пришло каждое поле |
| `updated_ns` | `timestamp_ns` события, из которого пришло поле |
| `unavailable` | имя поля → причина, по которой поля нет |
| `subscribe_errors` | topic → почему подписка не удалась |
| `ignored` | `health_without_local_entity_id` — сколько health-событий отброшено |
| `tracking`, `groups` | состояние самого модуля |

Поля из `values` продублированы прямо на снимке, поэтому пишется `snap.health`.

**Поле, которого никто не наблюдал, равно `nil` и одновременно перечислено в
`snapshot.unavailable` с причиной. Оно никогда не подменяется нулём.** Ноль,
выданный за реальные данные, неотличим от факта. Наблюдённый `false` — например,
`snap.sniper` после выхода из снайперского режима — это, наоборот, ответ, и он в
`unavailable` не попадает.

```lua
local snap, err = wotb.battle.snapshot()
if snap == nil then
    wotb.log.warn("battle: %s", err)
    return
end
if snap.health == nil then
    wotb.log.debug("HP пока нет: %s", snap.unavailable.health)
else
    wotb.log.info("HP %d (из %s)", snap.health, snap.source.health)
end
```

Три вещи, которые дешевле прочитать здесь, чем выяснить на практике.

- `max_health` недоступен **навсегда**: ни один публикуемый payload его не
  несёт, в `WotbModV3VehicleEventData` есть только `previous_health` и
  `health`. Строка причины в `unavailable.max_health` прямо называет замену —
  `wotb.players.local_player().max_health`, который приходит из
  `entity_public.enumerate_visible`. Это другой источник, и обращаться с ним
  нужно как с другим.
- `damage_dealt` не существует и не появится: у health-ingress нет доказанного
  источника атакующего, поэтому приписать урон локальному игроку можно было бы
  только угадав. Runtime по той же причине не публикует
  `wotbmod.gameplay.damage_dealt`. Полученный урон, наоборот, есть:
  `snap.damage_received` с полями `damage`, `previous_health`, `health`,
  `reason_code` и `source_entity_id`.
- вход в бой и выход из боя **очищают все per-battle поля**. HP прошлого боя,
  выданный за текущий, — ровно тот отказ, ради предотвращения которого правило и
  введено. `lifecycle` при этом сохраняется: «мы только что вышли из боя» —
  верное утверждение и после выхода.

Пока не пришло `wotbmod.vehicle.local.changed`, health-события приписать некому.
Они не записываются, а считаются, и счётчик виден в
`snapshot.ignored.health_without_local_entity_id`. Отдельно различаются «поле не
обновляется» и «этот клиент такой topic не публикует»: второе видно в
`snapshot.subscribe_errors`.

### `wotb.config`

Типизированный слой над сохранёнными значениями. Backend-а два, потому что это
разные задачи: `"settings"` (по умолчанию) пишет через `wotb.settings`, то есть
кладёт опции мода в штатный settings UI клиента; `"storage"` держит один
JSON-документ под одним ключом `wotb.storage` — это то, что нужно состоянию,
которое игрок не редактирует.

```lua
local store, err = wotb.config.new({
    backend = "storage",
    key = "config",
    schema = {
        enabled   = { type = "boolean", default = true },
        opacity   = { type = "number",  default = 0.8, min = 0, max = 1 },
        rows      = { type = "integer", default = 8, min = 1, max = 32 },
        placement = { type = "string",  default = "left",
                      values = { "left", "right" } },
    },
})
if store == nil then
    print(err)
    return
end

local rows, origin = store:get("rows")    --> 8, "default"
store:set("rows", 12)
local rows2, origin2 = store:get("rows")  --> 12, "stored"
```

Поле схемы описывается `type` (`boolean`, `integer`, `number` или `string`),
обязательным `default` и опциональными `min`/`max` для чисел, `max_length` и
`values` для строк. Имя поля — непустая строка не длиннее 95 байт
(`WOTBMOD_V3_SETTING_KEY_MAX` без терминатора). Схема без полей отвергается.
Объявленный `default` проверяется по собственным ограничениям поля прямо в
`new`: поле без default отвергается, потому что до первой записи у него нет
честного ответа.

`config.new` принимает `schema` (обязательно), `backend`, `key` (обязателен для
backend `storage`) и `autosave` (по умолчанию `true`). При `autosave = false`
backend `storage` копит изменения в памяти, пока не будет вызван `save()`;
backend `settings` пишет через ABI на каждый `set` в любом случае.

| Метод | Возвращает |
| --- | --- |
| `store:get(name)` | `value, "stored"` либо `value, "default"[, note]`; `nil, message` при отказе чтения |
| `store:set(name, value)` | `true` либо `nil, message` |
| `store:reset(name)` | `true` либо `nil, message` |
| `store:save()` | `true` либо `nil, message`; для backend `settings` всегда `true` |
| `store:reload()` | `true` либо `nil, message`; только для backend `storage` |
| `store:all()` | `values, source, notes` либо `nil, message` |
| `store:schema()`, `store:defaults()` | копии |

**Default никогда не затирает сохранённое значение.** Это устроено структурно, а
не аккуратностью:

- `new()` не пишет ничего и никогда — только проверяет схему;
- `get()` не пишет ничего и никогда: отсутствующее значение отвечается из
  default и **помечается** как `"default"`, но обратно не записывается;
- документ backend-а `storage` перед записью читается, и пишется прочитанный
  документ с наложенными явно установленными ключами. Ключ, который никто не
  ставил, сохраняет своё значение, а ключ вне схемы вообще остаётся нетронутым.

**Нечитаемое хранилище — ошибка, а не пустой документ.** `wotb.storage.contains`
отвечает `true, <есть или нет>` при успехе и `nil, message` при отказе: первое
значение говорит, сработал ли вызов, второе — есть ли ключ. Схлопывание этих
двух случаев в один превратило бы «хранилище не прочиталось» в «хранилище
пустое», а пустое хранилище — ровно то состояние, в котором запись всех
default выглядит безопасной. Поэтому неудачное чтение возвращает
`nil, message`, и `set` поверх него тоже не выполняется.

Успешный `get` никогда не возвращает `nil` первым значением: default обязателен
у каждого поля, поэтому `nil` из `get` всегда означает, что не удалось само
чтение. Третье значение — необязательная заметка: сохранённое значение оказалось
не того типа, либо клиент отказал в чтении и сообщил причину. Заметка не
скрывается и не превращается в отказ.

Одна деталь про boolean, о которой лучше узнать здесь. В этом ABI boolean —
это `uint32_t`, и генератор переносит его в Lua как есть:
`wotb.settings.get_bool` возвращает целое `0` или `1`, а
`wotb.settings.set_bool` принимает `0`/`1`, а не `true`/`false`.
`wotb.config` конвертирует в обе стороны сам,
поэтому в схеме поле объявляется обычным `type = "boolean"` и `get`/`set`
работают с Lua-булевыми значениями. Ответ не того типа `wotb.config` не
принимает молча: он уходит в default с приложенной причиной.

### `wotb.available`

Один вопрос, один ответ: **опубликовал ли этот клиент интерфейс `wotb.<name>`**.

| Функция | Возвращает |
| --- | --- |
| `wotb.available(name)` | `true`/`false`; `nil, message`, если `name` — не непустая строка |

`true` означает ровно одно: в таблице `wotb.<name>` прямо сейчас лежит хотя бы
один вызываемый слот. Это и есть определение публикации в этом host-е —
функции ставятся только после успешного `query_interface`, а константы ставятся
безусловно (см. «Опубликован, разрешён, поддержан»). Работает и для трёх
ручных интерфейсов: `wotb.storage`, `wotb.events` и `wotb.ui` собираются
`lua_newtable` + `luaL_setfuncs`, их слоты — обычные Lua-функции, и обходятся
тем же проходом, что и слоты сгенерированной таблицы.

**`available` отвечает «опубликован», а не «разрешён».** Это первое, на чём
здесь спотыкаются. Permission в этом host-е не меняет форму API: и таблица, и
её функции существуют независимо от того, есть ли у скрипта grant, а отказ
приходит **значением из самого вызова**. Поэтому `true` не обещает, что вызов
пройдёт, и код ниже всё так же обязан читать второй результат:

```lua
if wotb.available("ui") then
    local control, err = wotb.ui.create(spec)
    if control == nil then
        -- сюда попадает и "permission denied: ui.create"
        wotb.log.warn("создать control не удалось: %s", err)
        return
    end
end
```

Реализация намеренно устойчива к чужим метатаблицам: и таблица интерфейса, и
сам `wotb` читаются через `rawget`/`next`, поэтому ни `__index`, ни `__pairs`
скрипта не могут придумать ответ на этот вопрос.

Ответ не кэшируется. Он про таблицу в её нынешнем виде, а кэшированное «да»
пережило бы таблицу, которую скрипт с тех пор очистил. Стоит один сырой проход
по одной таблице с остановкой на первой функции; вызывать это положено один раз
сверху скрипта, а не каждый кадр.

### `wotb.panel`

Высокоуровневая обёртка над `wotb.ui` для самой частой задачи — прямоугольная
панель из строк текста и нескольких кнопок, которая появляется в бою, переживает
смену экрана и убирается сама. `wotb.ui` — это 74 слота дерева, геометрии,
текста, стиля, layout-а и событий; правильная форма для биндинга и неудобная для
автора.

Модуль называется `wotb.panel`, а не `wotb.ui.panel`, по той же причине, по
которой `wotb.config` не живёт внутри `wotb.settings`: обёртка над интерфейсом
обязана переживать **отсутствие** этого интерфейса, а изнутри него это
невозможно. На клиенте без UI нужный автору ответ — фраза
`"wotb.ui is unavailable on this client"`, и модуль, доступный только через
`wotb.ui`, произнести её не может.

**Модуль строится при первом обращении, а не при загрузке.** Это второе место,
где легко ошибиться в ожиданиях. До первого индексирования `wotb.panel` — пустая
таблица за метатаблицей: `next(wotb.panel) == nil`, и `next` сырой, так что
`__index` не может выдать её за построенную. Первое же обращение строит около 41
замыкания, кэширует их в ту же таблицу (идентичность модуля не меняется:
`local panel = wotb.panel`, взятый до первого касания, после него держит
построенный модуль) и снимает метатаблицу, так что промах по `wotb.panel` дальше
стоит промах, а не Lua-вызов.

Что это значит по цене, измеренной `lua_sethook` с `LUA_MASKCOUNT` при
`count = 1`, то есть в единицах того же instruction budget:

| Момент | Инструкций VM | Доля от 100 000 |
| --- | ---: | ---: |
| загрузка модуля | 14 | 0.014 % |
| первое обращение | 127, один раз | 0.127 % |

Для сравнения, теми же измерениями: `wotb.log` и `wotb.timer` стоят по 34
инструкции на загрузку, `wotb.json` — 46, `wotb.config` — 47, `wotb.battle` —
142. То есть самый большой модуль в файле оказывается самым дешёвым для скрипта,
который ничего не рисует.

Отложен именно **build**, но не **parse**. `luaL_loadbufferx` проходит по всему
чанку (около 45 КБ) для каждого скрипта независимо от того, коснётся тот
`wotb.panel` или нет — это работа на стороне C, она не считается ни в какой
budget и выполняется один раз на скрипт при загрузке, а не каждый кадр. Автор,
который не трогает панель, платит только этот parse.

#### Создание

```lua
local panel, err = wotb.panel.new({
    id = "ally_tracker",
    width = 360, height = 240,
    anchor = "top-right", margin = 24,
    rows = 6,
    buttons = {
        { id = "pause", text = "II", x = 310, y = 8, width = 36, height = 28,
          on_click = function(self) self:set_button_text("pause", ">") end },
    },
})
if panel == nil then
    print(err)
    return
end
```

`new` — это только проверка и арифметика: он ничего не создаёт, не читает ни
одного интерфейса и не требует ни одного permission. Поэтому панель можно
объявить в начале скрипта даже на клиенте, который UI не публикует вовсе;
отсутствие всплывёт из `mount()`, где его есть кому сообщить.

Обязательны `width` и `height` (положительные числа). Остальное:

| Поле spec | По умолчанию | Смысл |
| --- | --- | --- |
| `id` | `"panel"` | 1–24 байта из букв, цифр, `_`, `.`, `-` |
| `anchor` | `"top-left"` | одно из `wotb.panel.ANCHORS` |
| `margin` | `0` | число либо таблица `{left, right, top, bottom}` |
| `x`, `y` | нет | ставятся **только парой** и перебивают `anchor` |
| `padding` | `20` | отступ, от него же считается `rows_top` |
| `font` | `wotb.panel.FONT` | путь к шрифту |
| `font_size` | `18` | размер по умолчанию для строк и кнопок |
| `row_height`, `row_gap`, `rows_top` | `32`, `4`, `padding` | раскладка строк |
| `color`, `background` | белый, тёмно-синий с `a = 0.92` | таблицы с числовыми `r`, `g`, `b` |
| `visible` | `true` | начальная видимость |
| `rows` | нет строк | число строк либо массив спецификаций строк (не больше 64) |
| `buttons` | нет кнопок | массив спецификаций кнопок (не больше 16) |
| `contexts` | бой, тренировка, реплей | либо `false`, либо `{ visible = mask, blocked = mask }` |
| `context_frames`, `rebind_frames`, `probe_frames`, `retry_frames` | `15`, `30`, `60`, `60` | периодичности, в кадрах |
| `on_error` | нет | `function(scope, message)` для ошибок, которые вернуть некому |

Строка спецификации принимает `text`, `height`, `size`, `x`, `y`, `width`,
`color` и `visible`; кнопке обязательны `id`, `x`, `y`, `width`, `height`, а
`text`, `size`, `color`, `background` и `on_click` необязательны. `on_click`
вызывается как `handler(panel, button_id)` и оборачивается в `pcall`: сломанный
обработчик мода не должен становиться проблемой клиента, ошибка вместо этого
записывается на панель.

Маску контекста по умолчанию модуль собирает из `wotb.context` по именам
(`BATTLE`, `TRAINING`, `REPLAY` — показывать; `MOD_SCREEN`, `TEXT_INPUT` —
блокировать), а не числами: значения остаются host-овыми.
`contexts = false` выключает gate для панели, которая управляет видимостью сама.

#### Методы панели

| Метод | Возвращает |
| --- | --- |
| `panel:mount()` | `true` либо `nil, message`; на уже смонтированной — `true` |
| `panel:unmount()` | `true`; идемпотентен и безопасен на не смонтированной |
| `panel:mounted()` | `true`/`false` |
| `panel:update(frame_index)` | см. ниже |
| `panel:set_row(index, text)` | `true, "sent"` либо `true, "unchanged"`; `nil, message` |
| `panel:set_rows(list)` | `true, <сколько дошло до клиента>` либо `nil, message` |
| `panel:set_row_visible(index, on)` | как `set_row` |
| `panel:row(index)` | `text, visible` либо `nil, message` |
| `panel:row_count()` | число строк |
| `panel:set_button_text(name, text)` | как `set_row` |
| `panel:visible()` | `true`/`false` |
| `panel:set_visible(on)` | как `set_row` |
| `panel:show()` / `panel:hide()` | то же, что `set_visible(true)` / `set_visible(false)` |
| `panel:layout()` | `true` либо `nil, message` |
| `panel:set_size(w, h)`, `panel:set_anchor(a)`, `panel:set_position(x, y)` | результат `layout()` |
| `panel:position()`, `panel:size()`, `panel:viewport()` | по два числа |
| `panel:control(name)` | handle `wotb.ui` либо `nil, message` |
| `panel:errors()` | `error_count, last_error` |

Модуль публикует ещё две константы: `wotb.panel.ANCHORS` (массив из семи имён —
`"top-left"`, `"top-center"`, `"top-right"`, `"center"`, `"bottom-left"`,
`"bottom-center"`, `"bottom-right"`) и `wotb.panel.FONT` (шрифт по умолчанию).

`panel:control(name)` — это не потолок, а люк: каждый созданный control доступен
по имени (`"root"`, `"frame"`, `"row1"`…`"rowN"` и id каждой кнопки), и с ним
можно делать всё, что умеет `wotb.ui`. Владение при этом остаётся за панелью:
не вызывайте `destroy` на выданном handle — панель останется держать мёртвый.
Для этого есть `unmount()`.

#### Кадр

```lua
function on_frame(frame_index, delta_seconds)
    if panel:update(frame_index) then
        panel:set_row(1, "союзников живо: " .. alive)
    end
end
```

`update` — единственный вызов, который панели нужен от `on_frame`. Он
возвращает `true`, пока панель смонтирована, контекст её пускает и она не
скрыта; `false` плюс необязательную причину, когда она не показывается; и
`nil, message` только на неверный аргумент. Поэтому `if panel:update(frame) then`
читается правильно во всех трёх случаях. Монтированием и размонтированием он
занимается сам: панель поднимается при первом же разрешённом кадре и
снимается, когда контекст закрылся, — вызывать `mount()` вручную не нужно.

На кадре, где делать нечего, `update` стоит одно взятие остатка и два чтения
поля. Периодическая работа разнесена по четырём независимым периодичностям,
каждая из которых переопределяется из spec: `context_frames` (15) — один
`wotb.core.get_context`; `rebind_frames` (30) — перепривязка к активному экрану,
плюс немедленная реакция на topic смены экрана, если подписка удалась;
`probe_frames` (60) — `get_viewport_size` и один `control_get_snapshot`, то есть
проверка, не снёс ли клиент дерево панели; `retry_frames` (60) — пауза после
неудачного `mount`, чтобы отказывающий клиент не опрашивался шестьдесят раз в
секунду.

`set_row` сначала сравнивает, и только при отличии идёт через ABI: сравнение —
это одно чтение таблицы и одно сравнение строк, и происходит оно раньше всего
остального, включая поиск `wotb.ui`. Вызывать его каждый кадр с одной и той же
строкой бесплатно; вызывать каждый кадр со строкой из `string.format` — нет,
потому что форматирование выполняется до входа в функцию. Дорогую строку стоит
собирать под собственным флагом «что-то изменилось».

Ошибки, которые панель наживает по собственной инициативе — внутри frame driver,
внутри обработчика клика, внутри teardown, — возвращать некому. Они считаются и
запоминаются (`panel:errors()`), а если задан `spec.on_error`, то ещё и
отдаются ему. В лог они не пишутся сами: панель, которая пишет в лог раз в
кадр, — это тот же самый баг с падением FPS в новом обличье.

#### Permissions и владение

Своих permissions у модуля нет и быть не может: он вызывает только то, что и так
доступно скрипту. Нужны ровно те же разрешения, что и вызовам под ним — набор
таблицы `ui` для построения и обновления, `core` для `wotb.context` и
`wotb.handles.release`, `events.public` для подписки на смену экрана. Подписка
необязательна: если её не разрешили или клиент отказал, панель переходит на
опрос каждые `rebind_frames` кадров.

Каждый control, стиль и подписка записаны в ownership registry скрипта тем же
C-биндингом, что и при ручной работе с `wotb.ui`; модуль этого не обходит.
Разбирает он в порядке, обратном построению, отпуская каждый handle сразу после
уничтожения, а всё состояние держит в обычной Lua-таблице этого `lua_State` —
поэтому hot reload уносит его целиком, и инвалидировать на стороне host-а
нечего.

## Фасады: короткий API поверх raw-таблиц

Raw-таблицы (`wotb.gameplay_hud`, `wotb.entity_public`, ...) повторяют C ABI
один в один: упакованные целые вместо цветов, `0/1` вместо `true/false`,
handle-ы и выходные параметры. Это правильная форма для биндинга и неудобная
для автора. Фасад — модуль на чистом Lua, который прячет эту форму за
короткими именами и отвечает по одной конвенции: значение при успехе,
`nil, err` при отказе (или `false, err` там, где `false` — законный ответ).
Фасад ничего не добавляет к возможностям клиента и не имеет собственных
permissions: каждый его вызов — это вызов raw-слота, и отказ клиента
(`E_NOT_SUPPORTED`, `E_PERMISSION_DENIED`, `E_WRONG_THREAD`) приходит автору
теми же словами с префиксом фасада.

**Правило имён.** Таблица фасада никогда не совпадает по имени с таблицей
интерфейса. Причина та же, что у `wotb.panel` и `wotb.config`: модуль-обёртка
обязан переживать отсутствие интерфейса и отвечать фразой «wotb.gameplay_hud
недоступен», а изнутри `wotb.gameplay_hud` это невозможно; к тому же
Lua-функция на таблице интерфейса сделала бы `wotb.available('gameplay_hud')`
истинным на клиенте, который интерфейс не публиковал. Поэтому `wotb.hud` живёт
рядом с `wotb.gameplay_hud`, а не внутри него. Единственное исключение —
расширение таблицы, которую host построил сам и только при публикации
(`wotb.ges`, прецедент `wotb.dava.run_on_main`): такое расширение никогда не
создаёт таблицу.

Все фасады разрешают raw-таблицу в момент вызова, а не при загрузке, и при
загрузке не подписываются ни на что. Проверены host-тестами
(`tests/lua_host_tests.cpp`, блок «the facade layer»); отдельного
live-подтверждения у них, как и у остальных convenience-модулей, пока нет.
Пример целиком на фасадах — `examples/lua_facade_tour`.

### `wotb.context`: `is_*`

`is_hangar()`, `is_battle()`, `is_training()`, `is_replay()`,
`is_text_input()`, `is_mod_screen()` — по одному `core.get_context` на вызов;
ответ `true`/`false` или `nil, err`, если маску прочитать нельзя. Спрашивайте
не чаще раза в кадр.

```lua
if wotb.context.is_battle() then panel:show() end
```

### `wotb.players`: короткие имена, честная поза, `details`

- `me()` — то же, что `local_player()` (`local` — зарезервированное слово Lua,
  поэтому не `local()`);
- `allies()` — свои без локального игрока; `our_team()` по-прежнему включает
  его;
- `enemies()` — то же, что `enemy_team()`: только уже видимые клиенту;
- `by_id(public_id)` — то же, что `find`;
- `each_visible(fn)` — `fn(record)` для каждой видимой машины в порядке
  `public_id`; `return false` из `fn` останавливает обход; ответ — число
  посещённых записей.

Флаги `position_available`/`direction_available` выводятся из данных, а не
задаются константой `false`: loader публикует позу только машине, у которой
проверил цепочку внешности, и опубликованное направление — единичный вектор;
у машины без источника в снимке остаётся нулевой вектор, а `(0, 0, 0)` — не
направление. Единичная длина направления и есть признак настоящей позы;
позиция следует за ним, потому что обе величины берутся из одной матрицы. То
же правило применяет `event.data.snapshot` у `wotbmod.entity.public.*`.

`details(record_or_id)` отдаёт пять полей ростера, которые loader публикует
только по имени (snapshot ABI заморожен): `clan_tag` (пустая строка без
клана), `account_id`, `kills` (и `frags` — второе имя того же числа),
`vehicle_name` (`нация:тег`, например `usa:A100_T49`) и
`vehicle_display_name` (локализованное, «T49»). Это пять вызовов
`get_public_property` на машину, поэтому они делаются по запросу, а не внутри
`snapshot()`. Поле, которое клиент отказался отдать, отсутствует и названо в
`unavailable` с причиной клиента — ноль вместо него не подставляется.

```lua
local ally = wotb.players.allies()[1]
if ally then
    local d, err = wotb.players.details(ally)
    if d then
        print(ally.display_name, d.clan_tag, d.vehicle_display_name, d.kills)
    else
        print(err)      -- "players.details: entity_public is unavailable"
    end
end
```

### `wotb.battle`: обработчики по коротким именам

`on(name, fn)`, `once(name, fn)`, `off(handle)`, `off_all()`, `events()` и по
одной функции `on_<name>(fn)` на имя. Имена и topic-и — одна таблица в
`lua_preludes.cpp`, она же документация:

| Имя | Topic |
| --- | --- |
| `enter` / `start` / `end` / `leave` | `wotbmod.battle.entered` / `.started` / `.ended` / `.left` |
| `shot` | `wotbmod.gameplay.shot_fired` |
| `hit` | `wotbmod.gameplay.shell_hit` |
| `reload` | `wotbmod.gameplay.reload_state_changed` |
| `ammo` | `wotbmod.gameplay.ammo_changed` |
| `damage` | `wotbmod.gameplay.damage_received` |
| `death` | `wotbmod.vehicle.local.destroyed` |
| `vehicle_destroyed` | `wotbmod.vehicle.killed` (victim/killer/assist из ростера арены) |
| `spotted` / `unspotted` | `wotbmod.vehicle.spotted` / `.unspotted` |
| `camera_changed` | `wotbmod.gameplay.camera_mode_changed` |
| `sniper_entered` / `sniper_exited` | `wotbmod.gameplay.sniper_entered` / `_exited` |

Обработчик получает `fn(data, event)`: сначала типизированный payload
(`event.data`; `nil`, если эта сборка его не публикует — фасад ничего не
синтезирует), затем само событие. Каждый вызов идёт под `pcall`: сломанный
обработчик попадает в `wotb.log` как `battle.on_<name> handler raised`, а
следующий обработчик всё равно выполняется. Пока `on()` не вызван, подписок
нет; `off()` возвращает подписку клиенту; `once` снимает себя перед первым
вызовом.

```lua
wotb.battle.on_vehicle_destroyed(function(kill)
    local victim = wotb.players.by_id(kill.victim_id)
    local killer = wotb.players.by_id(kill.killer_id)
    print((type(killer) == "table" and killer.display_name or "?") ..
          " уничтожил " ..
          (type(victim) == "table" and victim.display_name or "?"))
end)
local h, err = wotb.battle.on("teleport", print)
-- h == nil, err == "battle.on: 'teleport' is not a battle event; battle.events() lists them"
```

`is_active()` — в бою ли игрок сейчас (`BATTLE` или `TRAINING` в маске
контекста, без подписок); `state()` — таблица с `active`, `context`,
`tracking` и, пока включён `start()`, последним lifecycle-событием
(`lifecycle`) или причиной его отсутствия (`lifecycle_unavailable`).

### `wotb.ges`: `schema`, `on`/`off`, `observe`, `decode`

- `schema(type_name)` — `{ id, size, field_count, fields = { {name, offset,
  kind, size}, ... } }` до всякой подписки (слоты ABI `get_schema` и
  `schema_field`, ручной биндинг); `nil, err` для типа без схемы. Имя — в
  написании `types()`: `Avatar::CameraModeChanged`;
- `on(pattern, fn)` / `off(handle)` — те же `subscribe`/`unsubscribe`;
- `decode(ev)` — все поля схемы по именам одной таблицей, через `ev:field`;
  второй результат — таблица полей, которые runtime не может отдать в Lua
  (FastName, байты), с причинами; без схемы — `nil, err`;
- `observe(pattern, options, fn)` — `options.once` снимает подписку после
  первой доставки, `options.decode` вызывает `fn(fields, ev, unreadable)`
  вместо `fn(ev)`; `is_available()`.

Расширение ставится только если клиент опубликовал GES: на клиенте без него
таблицы `wotb.ges` нет вовсе, и `wotb.available('ges')` отвечает `false`.

```lua
wotb.ges.observe("Avatar::CameraModeChanged", { decode = true },
    function(fields) print("режим камеры", fields.mode) end)
```

### `wotb.mod`: кто я и что мне можно

- `id()` — id скрипта (из `manifest.json`; в dev-папке — имя файла), C-часть;
- `permissions()` — имена прав, которыми скрипт реально владеет: запрос
  манифеста, пересечённый с измеренным потолком host-а;
- `has_permission(name)` — тот же ответ по одному имени; совпадает с тем, что
  скажет забор при вызове;
- `capabilities()` — все capability, которые зарегистрировал runtime, по
  именам (на 11.20.0.887 6 сентября 2026: `catalog`, `content`,
  `filesystem.mod_data`, `input.actions`, `manifest`, `resources`,
  `settings`), отсортированный массив записей со статусом словом и причиной;
- `capability(name)` — статус одной capability словом (`available`,
  `degraded`, `unavailable`, `client_mismatch`, `permission_denied`,
  `context_restricted`) плюс таблица с `reason` клиента. Имя — capability
  runtime-а, а не идентификатор интерфейса: `wotbmod.gameplay.hud` — это
  интерфейс, capability-записи у него нет, и ответ на него —
  `nil, "... capability is not registered"` (измерено на 11.20.0.887);
- `info()` — всё вместе, плюс `host` (id, состояние и tier Lua host-а из
  `lifecycle.get_info`);
- `on_disable(fn)` / `off_disable(handle)` — несколько обработчиков рядом с
  глобальным `on_disable`; выполняются новейший первым, до глобального, каждый
  под `pcall`; host вызывает диспетчер по имени (`__run_disable_handlers`),
  потому что глобалы он читает через rawget.

Не входит: `version()` (production scanner манифеста читает только `id`,
`entrypoint` и `permissions`), `on_reload` (host не отличает reload от
disable — reload начинается с disable), зависимости и настройки
(`wotb.config`).

```lua
print(wotb.mod.id(), table.concat(wotb.mod.permissions(), ", "))
if not wotb.mod.has_permission("gameplay.tweak.hud") then
    print("HUD недоступен: попросите gameplay.tweak.hud в манифесте")
end
wotb.mod.on_disable(function() wotb.storage.set_json("last", "{}") end)
```

### `wotb.hud`: штатный HUD по группам

Фасад над 34 слотами `wotb.gameplay_hud`, сгруппированными как в заголовке
`gameplay_hud_v1.h`; строится при первом обращении, как `wotb.panel`.

- `mode()` — `native`, когда интерфейс `wotb.gameplay_hud` опубликован
  (runtime публикует его только с backend-ом), иначе
  `unavailable, reason`; правду о каждом слоте говорит ответ самого вызова;
  `available()`; `status()` — какие из 34 слотов опубликованы;
- `reticle.set_texture/set_sniper_texture/set_color/set_size/
  set_reloading_indicator/set_dispersion_circle`;
- `damage_log.show/hide/set_enabled/set_position/set_max_entries/
  set_show_blocked/set_show_ricochet/set_show_module_damage/set_format/
  set_filter_own`;
- `session_stats.show/hide/set_enabled/set_fields`;
- `minimap.set_size/set_opacity/set_show_last_known/
  set_show_artillery_range/set_show_drawing/add_marker/remove_marker`;
- `sixth_sense.set_texture/set_sound/set_position/set_scale/set_delay`;
- `hit_indicator.set_style/set_color_hit/set_color_pen/set_color_ricochet/
  set_color_crit`;
- `reset()` — единственный сброс в ABI, общий для всех групп; `rgba(color)`.

Аргументы: цвет — `{r, g, b[, a]}` в `0..1` или целое `0xRRGGBBAA`; флаги —
boolean; якорь, стиль и поля счётчиков — имя (`"top-right"`, `"compact"`,
`{"damage", "shots"}`) или число из констант `wotb.gameplay_hud.*`. Неверный
аргумент отклоняется до вызова ABI; отказ клиента приходит с префиксом фасада.
На 11.20.0.887 клиент отказывает `minimap.set_show_last_known`,
`damage_log.set_show_blocked`, `hit_indicator.set_color_pen`, стилю
`directional` и полю `kills` — фасад этого не скрывает.

```lua
if wotb.hud.mode() == "native" then
    wotb.hud.reticle.set_color({ r = 1, g = 0.25, b = 1 })
    wotb.hud.damage_log.set_position("top-right")
    local ok, err = wotb.hud.minimap.set_show_last_known(true)
    -- ok == nil, err == "hud.minimap.set_show_last_known: ... not supported"
end
```

### `wotb.screen`: дерево UI по коротким именам

Читающая половина UI поверх `wotb.ui` и `wotb.ui_read`; строящая половина —
`wotb.panel`, к которому делегирует `mount`.

- `root()` — handle активного экрана; он принадлежит скрипту, верните его через
  `wotb.handles.release`. Все остальные функции берут и отдают экран сами;
- `find(id_or_path)` — контрол под активным экраном по id или по пути с `/`
  (`control_find_by_id` / `control_find_by_path`); ответ клиента на
  отсутствующее имя — `nil, err` его словами;
- `children(control)`, `rect(control)`, `visible(control)`,
  `game_owned(control)`, `info(control)` — из `control_get_snapshot`, флаги —
  булевыми полями;
- `text(control)` — зеркало текста, который записал мод; `live_text(control)`
  — текст, который движок рисует сейчас (`ui_read.control_get_live_text`);
  для штатного контрола честен только второй;
- `set_text(control, text)`, `set_visible(control, on)` — перед вызовом
  проверяют, не штатный ли контрол: без `ui.modify.game` ответ
  `nil, "...game-owned; ui.modify.game is required..."` ещё до ABI;
- `mount(spec)` — `wotb.panel.new(spec)` плюс `mount()`, `unmount(panel)`;
- `notify(text [, seconds])` — штатный toast клиента; `popup({ title, message,
  accept, cancel, modal })` — штатный диалог (`cancel` делает его confirm).

```lua
local timer = wotb.screen.find("BattleScreen/TimerLabel")
if timer then print(wotb.screen.live_text(timer)) end
wotb.screen.notify("мод загружен", 2)
```

### `wotb.session`: кластер входа без перезапуска

Поверх `wotb.session_cluster` (API 1.1, 8 сентября 2026) и `wotb.events`.
Клиент сам умеет переключать кластер внутри региона
(`LoginManager::ChangeCluster`); фасад только называет это по-человечески.
Право `session.cluster.read` — на чтение и событие, `session.cluster.change`
(REVIEWED) — на переключение.

- `clusters()` — `{ {id, name, current, alive, allowed, ccu}, ... }` своего
  региона, отсортировано по `id` (`ccu == -1`, когда клиент не знает онлайн);
- `cluster()` — запись кластера, к которому клиент подключён сейчас;
- `change_cluster(4 | "EU_C4" | "auto")` → `true` или `nil, why`. Только из
  ангара; занято, пока предыдущее переключение в полёте и ещё 10 с после него;
  неизвестный id и мёртвый кластер отклоняются словами клиента. `"auto"` —
  штатный автовыбор клиента (ABI `-1`);
- `on_cluster_changed(fn)` → handle, `fn({ from, to, status })` со `status` из
  `queued | started | connected | failed`; `off_cluster_changed(handle)`,
  `off_all()`. `failed` означает и то, что флаг «ручной выбор» снят: следующий
  вход клиент сделает автовыбором.

Переключение действует на текущий сеанс; после перезапуска клиент снова
выбирает кластер сам.

### `wotb.vehicle`: своя машина и скины

Поверх `wotb.vehicle_visual`; всё client-only, как и сам интерфейс.

- `local_vehicle()` — handle своей машины (отдать через `wotb.handles.release`);
  `visible()` — записи `wotb.players.visible()`;
- `is_local(v)`, `is_hangar(v)`, `position(v)`, `appearance_state(v)` →
  `"unknown" | "loading" | "ready" | "destroyed"`, код вторым результатом;
- `skin.register(desc)` → pack (`asset_count` заполняется из `assets`),
  `skin.apply(pack, v)`, `skin.rollback(pack)`, `skin.state(pack)` →
  `{ applied, asset_count, mounted_asset_count, requires_model_reload,
  vehicle }` с булевыми флагами (`requires_model_reload == true` — «применено
  к ресурсам, но не к модели на экране»), `skin.release(pack)`;
- `appearance.reset(v)` — `restore_appearance`; `set_skin(v, uri)`,
  `set_camouflage(v, uri)`.

### `wotb.shells`: снаряды, попадания, трассеры

Наблюдение — через `wotb.events` (`created`, `updated`, `impact`,
`destroyed`, `local_shot`), визуал — через `wotb.projectile`. Траекторию,
физику снаряда и серверный результат фасад не трогает: таких слотов нет.

- `on(name, fn)`, `on_created/on_updated/on_impact/on_destroyed(fn)` —
  `fn(snapshot, data, event)`; `snapshot.fields` — булевы флаги
  `valid_fields` по именам полей (`origin`, `visible_direction`, ...), чтобы
  не читать поле, которое клиент не заполнил; `on_local_shot(fn)` —
  `fn(data, event)`; `off(handle)`, `off_all()`;
- `snapshot(projectile)`, `visual(projectile)`;
- `impact.show(desc)` → handle, `impact.update(handle, desc)`,
  `impact.hide(handle)`; `tracer.register(desc)` → handle,
  `tracer.unregister(handle)`.

### `wotb.view`: камера

Поверх `wotb.camera`, `wotb.gameplay_camera` и `wotb.camera_state`.

- `get()` — `mode` словом (`arcade`, `sniper`, ...), `transform`, `fov`,
  `near_plane`, `far_plane`, `observed` (только валидные поля); чтение, в
  котором клиент отказал, перечислено в `unavailable` с причиной;
- `set_fov(degrees [, "hangar" | "battle" | "sniper"])`, `fov()`, `reset()`;
- `project(point)` / `unproject(point)` — `world_to_screen` /
  `screen_to_world` через активную камеру;
- `transition({ target = { position, rotation, scale }, duration, easing,
  preserve_game_control })`, `shake({ amplitude, frequency, duration,
  falloff })`;
- `on_changed(fn)` — смена режима и вход/выход из снайперского; `off(handle)`.

Free, postmortem и cinematic режимы не предлагаются и не имитируются: клиент
не публикует способа в них войти.

### `wotb.sound`: проиграть файл, подменить звук

Поверх `wotb.audio`.

- `play(uri, { loop, spatial, volume, pitch, bus, priority, on_finished })` →
  handle (create + play); `stop(handle [, fade_seconds])`, `release(handle)`,
  (`fade_seconds` только 0: Windows-бэкенд 11.20 не умеет плавных затуханий — ненулевой fade у `stop` и ненулевая длительность у `fade_to` честно отвечают `NOT_SUPPORTED`, live 8 сентября 2026),
  `set_volume(handle, 0..1)`, `is_playing(handle)`, `on_finished(handle, fn)`;
- `replace(event_name, uri [, priority])` → token; `reset(event_name | token)`,
  `reset_all()`. Что клиент не умеет подменять, он говорит сам.

### `wotb.keys`: горячие клавиши

Поверх `wotb.input`. Id действия в ABI — `"<id скрипта>.<id>"`, поэтому два
мода с `bind("toggle", ...)` не мешают друг другу, и никто не перехватит
чужое действие по имени.

- `bind(id, { key = "F7" | code = 0x76, modifiers = { ctrl = true }, contexts,
  display_name, description, axis })` → action; `unbind(id)`, `unbind_all()`;
  `key_code("F7")`;
- `pressed(id)` (нажата в этом кадре), `down(id)` (удерживается), `axis(id)`;
- `on_pressed(id, fn)`, `on_released(id, fn)` — `fn(id, value)`; `off(token)`;
- `bindings(id)`, `conflicts(id)`, `capture_begin([contexts])` /
  `capture_end()` → `{ device, code, modifiers, scale }`.

```lua
wotb.keys.bind("notify", { key = "F7" })
wotb.keys.on_pressed("notify", function() wotb.screen.notify("F7") end)
```

### `wotb.packages`: мост к `wotbmod.exe` (loader-private)

Не часть замороженного C ABI: библиотека Lua-хоста, которая запускает
`<игра>\wotbmod\wotbmod.exe` скрытым процессом и отдаёт результат. Так
каталог в игре (`examples/catalog`) ставит, удаляет и выключает моды тем же
кодом, что и команда `wotbmod`: подписи, леджер, отзыв релизов и `sync`
остаются в одном месте. Право `packages.manage` (REVIEWED); без него любой
вызов отвечает `nil, "permission denied: packages.manage"`.

| Имя | Значение |
| --- | --- |
| `packages.run(verb [, args])` | номер задачи либо `nil, message` |
| `packages.poll(job [, wait_ms])` | `{ running = true }`, либо `{ exit_code, stdout, stderr }` (один раз, после ответа задача забыта), либо `nil, message` |
| `packages.cancel(job)` | `true`/`false`; живой процесс завершается |
| `packages.executable()` | путь к `wotbmod.exe` либо `nil, message` |
| `packages.MAX_OUTPUT`, `packages.MAX_POLL_WAIT_MS` | 256 КБ на поток вывода, 5000 мс ожидания в `poll` |

`verb` — только из списка, и у каждого своё число аргументов:

| verb | args | что запускается |
| --- | --- | --- |
| `"list"` | — | `wotbmod list --json --game-root <игра>` |
| `"info"` | `{id}` | `wotbmod info <id> --json …` |
| `"uninstall"` | `{id}` | `wotbmod uninstall <id> --yes …` |
| `"enable"` / `"disable"` | `{id}` | `wotbmod enable|disable <id> …` (`[mods] <id>=1|0`) |
| `"launcher-open"` | `{"wotbmod://install/<id>@<v>?source=<url>"}` | `wotbmod launcher open <link>` — установка с каталога |
| `"restart-client"` | — | `wotbmod restart-client …`, отсоединённый процесс; задача сразу завершена |
| `"sync"` | — | `wotbmod sync --yes …` |

Аргумент — строка из `[A-Za-z0-9._@:/%?=-]` не длиннее 512 байт; всё другое —
`nil, "argument refused"`. Командную строку собирает мост, а не скрипт, и
одновременно у мода живёт одна задача (`nil, "busy"`). В окружение потомка
добавляются `WOTBMOD_LAUNCHER_YES=1` и `WOTBMOD_LAUNCHER_NO_PAUSE=1`, чтобы
launcher не ждал клавиши. `poll` без `wait_ms` не блокирует: опрашивайте из
`on_frame` или таймера. Задачи, не опрошенные к `on_disable`, завершаются
вместе со скриптом.

```lua
local job = assert(wotb.packages.run("list", {}))
wotb.timer.every(250, function(id)
  local state = wotb.packages.poll(job)
  if state and not state.running then
    wotb.timer.cancel(id)
    local rows = wotb.json.decode(state.stdout).packages
    print(#rows .. " packages installed, exit " .. state.exit_code)
  end
end)
```

Установленные Lua-папки хост читает один раз при включении, ресурсные
пакеты меняют файлы, поэтому после успешной команды нужен перезапуск
клиента — `restart-client` делает это сам (закрывает клиент, убирает маркер
сессии, чтобы не попасть в safe mode, запускает через Steam).

### `wotb.store`: типизированные значения

Поверх `wotb.storage` и `wotb.json`: `set(key, value)` кодирует таблицу,
`get(key [, default])` возвращает её обратно и второй результат `"stored"`
или `"default"`; `delete`, `has`, `flush`; `keys()` и `clear()` работают по
индексу, который модуль держит под зарезервированным ключом
`wotb.store.index` — ABI хранилище не перечисляет.

### `wotb.files`: файлы и ресурсы

Поверх `wotb.loaders`, `wotb.vfs`, `wotb.resources`, `wotb.yaml`.

- `read_text(uri [, max_bytes])`, `read_binary(uri [, max_bytes])`,
  `read_json(uri)`; лимит по умолчанию 4 MiB;
- `read_yaml(uri [, limits])` → документ с `doc:get("a.b[2].c")` (скаляр,
  либо `{ kind = "map" | "sequence", size = n }`) и `doc:release()`. Целиком
  в таблицу YAML не превращается: ABI не перечисляет ключи map;
- `load_texture(uri)`, `load_audio(uri)`, `load_scene(uri)` → resource
  (`resources.load` с ожидаемым типом), `info(resource)`, `release(resource)`;
- `exists(uri)` → `true` или `false, reason` (ответ `vfs.stat`), `stat(uri)`,
  `list(uri)`;
- `watch(uri, fn)` — `vfs.watch` плюс подписка на `wotbmod.vfs.invalidated`;
  `fn(event)` на каждую инвалидацию; `unwatch(watch)`.

### `wotb.panel`: контролы после `new()`

`label(opts)`, `button(opts)`, `image(opts)`, `scroll(opts)` добавляют контрол
в любой момент: смонтированная панель получает его сразу, несмонтированная —
при следующем `mount()`; ответ — имя для `panel:control(name)` (`3`, `"go"`,
`"image1"`, `"scroll1"`). `row({ x, y, gap, items = {...} })` и
`column({...})` раскладывают элементы (`kind = "label" | "button" | "image"`)
по оси; `row(index)` по-прежнему читает текст строки. `on("mounted" |
"unmounted" | "visibility" | "error", fn)` — слушатели панели; `destroy()`
размонтирует и забывает spec.

## Runtime UI из Lua

Ниже описан сам интерфейс `wotb.ui`. Если нужна обычная панель из строк и
кнопок, живущая в бою, начните с `wotb.panel` — она собрана ровно из этих
вызовов и снимает с автора монтирование, привязку к экрану, контекстный gate и
разбор при выгрузке.

Основной runtime-конструктор принимает одну Lua-таблицу:

```lua
local label, err = wotb.ui.create({
    type = wotb.ui.CONTROL_TEXT,
    id = "VolumeLabel",
    text = "Громкость: 75%",
    x = 40, y = 60, width = 320, height = 48,
    font = "~res:/Fonts/WarHeliosCondCBold.ttf",
    font_size = 24,
    color = {r=1, g=1, b=1, a=1},
    background_color = {r=0.05, g=0.08, b=0.12, a=0.95},
    alignment = wotb.ui.ALIGN_LEFT,
    opacity = 1.0,
    visible = true,
})
if not label then
    print(err)
    return
end

label:set_text("Громкость: 80%")
label:set_color({r=1, g=0.65, b=0.1, a=1})
```

Изменения, которым нужна перестройка native DAVA control (`text`, `texture`,
`font`, `color`, `opacity`, background, alignment и wrap), объединяются до
следующего кадра. Поэтому setters безопасно вызывать прямо из click/drag
callback: за кадр применяется только последнее значение, а control не
освобождается посреди обработки input event. Geometry, visibility, enabled и
interactable применяются через прямые native setters.

Поддерживаются все frozen `CONTROL_*`: container, text, image, button,
checkbox, slider, dropdown, text input, scroll view, list и tabs. Object handle
даёт методы `clone`, `destroy`, `add_child`, `set_parent`, `set_text`,
`set_texture`, `set_font`, `set_font_size`, `set_color`, `set_opacity`,
geometry/layout, `on` и `push_style`. Ошибки сохраняют общий контракт
`nil, message`.

Динамический список строится обычным Lua-циклом:

```lua
local list = wotb.ui.create({
    type = wotb.ui.CONTROL_LIST,
    id = "Players", x = 40, y = 140, width = 360, height = 400,
})
local row = wotb.ui.create({
    type = wotb.ui.CONTROL_BUTTON,
    id = "RowTemplate", text = "", width = 330, height = 38,
    parent = list, visible = false,
})

for index, player in ipairs(players) do
    local copy = row:clone()
    copy:set_parent(list)
    copy:set_position(0, (index - 1) * 44)
    copy:set_text(player.name)
    copy:set_visible(true)
    copy:on(wotb.ui.EVENT_CLICK, function()
        label:set_text("Выбран: " .. player.name)
    end)
end
```

Для совместимости остаётся template-backed форма `control_create(type, id,
uri)`, но runtime UI и пример от неё не зависят. Произвольные raw DAVA class
names/component payloads и FFI намеренно недоступны; выбираются безопасные
типы/свойства API. Native animation/effect component injection пока не
предоставляется — анимация выполняется из `on_frame` через property setters.

## Loader-private `wotb.dava`

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

## Generated DAVA loaders

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

## Аргументы, результаты и ошибки

Ожидаемый отказ ABI, отсутствие permission или неверный аргумент возвращается
значением, а не выбрасывает Lua exception:

```lua
local value, err = wotb.storage.get_json("settings")
if value == nil then
    print(err)
    return
end
```

Общие формы:

- команда без output возвращает `true`;
- один обязательный output возвращается напрямую;
- несколько output возвращаются несколькими Lua-значениями в порядке ABI;
- `storage.contains` и `ui.control_is_alive` возвращают `true, boolean`, чтобы
  успешный `false` не выглядел как отказ;
- строки сохраняют точную длину и могут содержать `\0`;
- структуры передаются таблицами с именованными полями;
- массивы передаются Lua-таблицами;
- handles и tokens — типизированный userdata, не число;
- callback передаётся Lua-функцией и хранится в registry её собственной VM.

Сырой указатель, function pointer или произвольный адрес получить нельзя.
Именно поэтому две pointer-only функции не связаны.

## Разрешения и двухуровневый забор

Native runtime видит один `wotbmod.lua_host` handle. Поэтому действует два
уровня:

1. внешний runtime grant host-пакета — абсолютный потолок;
2. внутренний grant скрипта — `manifest.permissions ∩ потолок host-а`.

Если host не может измерить свой grant через `wotbmod.permissions`, он не
запускает ни одного скрипта. Запрещённый вызов возвращает
`nil, "permission denied: <name>"` до разбора аргументов и до вызова ABI.

Host знает все 51 permission names V3. Release-пакет запрашивает 45 разрешений
до tier `REVIEWED` и намеренно не запрашивает шесть `UNSAFE`:

```text
native.memory
native.memory_patch
native.hook.address
native.hooks
render.native
bigworld.rpc.modify
```

Полный vocabulary по tier:

| Tier | Permission names |
| --- | --- |
| `SAFE` | `core`, `ui`, `ui.create`, `ui.modify.own`, `localization`, `audio`, `audio.custom`, `audio.events`, `resources`, `resources.mod`, `filesystem.mod_data`, `input`, `input.actions`, `settings`, `storage`, `events.public`, `entity.public.visible`, `hangar.scene`, `vehicle.local.cosmetic`, `camera.hangar`, `camera.replay`, `network.http.allowlisted`, `content` |
| `GAMEPLAY_TWEAK` | `gameplay.tweak.camera`, `gameplay.tweak.hud`, `gameplay.tweak.hangar`, `gameplay.tweak.replay`, `gameplay.tweak.cosmetic`, `gameplay.tweak.vehicle`, `gameplay.tweak.projectile_visual`, `gameplay.tweak.freecam` |
| `REVIEWED` | `battle.ui`, `battle.render.overlay`, `camera.battle.read`, `visible.projectile.events`, `game.entity.public`, `ui.modify.game`, `resources.overlay.game`, `hooks.symbol`, `render.callbacks`, `bigworld.observe`, `bigworld.rpc.observe`, `bigworld.rpc.metadata`, `client.leave_to_hangar`, `network.http`, `packages.manage` |
| `UNSAFE` | `native.memory`, `native.memory_patch`, `native.hook.address`, `native.hooks`, `render.native`, `bigworld.rpc.modify` |

Чтобы один script permission не мог «одолжить» другой grant общего native
handle, интерфейс с независимыми операционными правами закрыт консервативно:
скрипт должен запросить весь набор своей таблицы.

| Lua table(s) | Требуемый набор script permissions |
| --- | --- |
| `core`, `capabilities`, `permissions`, `handles`, `lifecycle`, `async`, `intermod`, `device`, `diagnostics`, `devtools` | `core` |
| `hooks` | `hooks.symbol` |
| `unsafe_native` | таблица отсутствует: единственная функция требует raw pointer |
| `events` | `events.public` |
| `ui` | `ui.modify.game`, `ui.create`, `ui.modify.own`, `battle.ui` |
| `settings` | `settings` |
| `storage` | `storage` |
| `input` | `input.actions` |
| `vfs` | `resources.mod`, `resources.overlay.game` |
| `resources`, `yaml`, `archive`, `loaders` | `resources.mod` |
| `dava` | `gameplay.tweak.cosmetic`; `create_texture`, `create_mesh` и `create_mesh_consumer` также требуют `resources.mod` |
| `http` | `network.http` |
| `render` | `battle.render.overlay`, `render.callbacks` |
| `render_native` | `render.native` (не входит в release ceiling) |
| `camera` | `camera.battle.read`, `camera.hangar`, `camera.replay`, `gameplay.tweak.camera` |
| `scene` | `hangar.scene`, `resources.mod` |
| `audio` | `audio.custom`, `audio.events` |
| `vehicle_visual` | `gameplay.tweak.vehicle`, `vehicle.local.cosmetic`, `game.entity.public`, `resources.mod`, `resources.overlay.game` |
| `gameplay_camera` | `gameplay.tweak.camera`, `gameplay.tweak.freecam` |
| `gameplay_hud` | `gameplay.tweak.hud`, `battle.ui` |
| `gameplay_hangar` | `gameplay.tweak.hangar`, `hangar.scene` |
| `gameplay_replay` | `gameplay.tweak.replay` |
| `entity_public` | `entity.public.visible`, `game.entity.public` |
| `bigworld_rpc` | `bigworld.rpc.observe`, `bigworld.observe`, `bigworld.rpc.metadata` |
| `projectile` | `gameplay.tweak.projectile_visual`, `visible.projectile.events` |
| `client` | `client.leave_to_hangar` |
| `manifest`, `catalog` | `content` |
| `content` | `content`, `resources.mod`, `resources.overlay.game` |

Это сознательно может дать более узкий доступ, чем отдельная native-операция.
Без per-call identity в замороженном ABI более широкий inner gate был бы
эскалацией до совокупных прав host-а.

## Владение и callback-и

На каждый `lua_State` ведётся ownership registry. Созданные handles,
subscriptions и callback records записываются до передачи скрипту. Успешный
явный `release`/`destroy` удаляет запись; teardown не освобождает её второй раз.

При выгрузке host:

- прекращает новые callback deliveries и ждёт уже начатые;
- снимает subscriptions и generated callback registrations;
- уничтожает controls и другие owned handles;
- откатывает незавершённые storage transactions;
- закрывает Lua state.

Event и generated callbacks синхронны и могут приходить с render/worker thread.
Все входы в один state сериализованы его recursive mutex. Не рассчитывайте, что
callback выполняется в main thread. `events.stop_propagation` имеет смысл только
внутри текущей синхронной доставки.

## Instruction budget

Каждый внешний вход в VM получает 100 000 инструкций Lua VM: top-level chunk,
`on_enable`, `on_frame`, `on_disable` и callback. Вложенный Lua `pcall` и
синхронный nested callback делят внешний бюджет и не получают новый.

При превышении hook атомарно помечает скрипт неисправным. Он не освобождает
ресурсы изнутри текущего вызова: это конфликтовало бы с lock order ownership
registry. Текущий callback возвращается, новые входы блокируются, а ближайший
native frame отсоединяет скрипт и освобождает его без Lua-вызова. Уже выполненный
`stop_propagation` остаётся в силе; открытые transactions откатываются.

Бюджет считает VM-инструкции, а не миллисекунды. Долгий native ABI call он не
прерывает, поэтому callback-и всё равно должны быть короткими.

## Ограничения

- Host и Lua собраны для Windows x86.
- Installed Lua-моды пока являются папками под `mods\lua`, а не отдельными
  `.wotbmod` archives; `.wotbmod` нужен самому native host-у.
- Public-preview bundle подписывает host detached ECDSA P-256/SHA-256 подписью
  и устанавливает соответствующий публичный trust key. Отдельный результат
  `build_lua_host_package.ps1` остаётся локальным unsigned build artifact.
- Системный риск native SEH + C++ locks в основном runtime остаётся отдельной
  задачей и не устраняется Lua sandbox-ом. Закрыт один участок — резервирование
  VFS provider slot (`src/v3/data_services.cpp`, `ReserveVfsProviderSlot`);
  остальные десять критических секций `g_vfs_mutex` относятся к тому же классу
  и не исправлены.
- Проверки ветки выполняются synthetic host-ами. Фактическое поведение UI,
  камеры и других client bridges требует отдельного live-game прогона.
