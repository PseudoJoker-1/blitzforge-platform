# Константы из заголовков

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
