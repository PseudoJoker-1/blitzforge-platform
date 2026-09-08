# `wotb.panel`

Фасад поверх raw-таблиц: [`wotb.ui`](../reference/ui.md), `wotb.context`, [`wotb.events`](../reference/events.md), [`wotb.handles`](../reference/handles.md).

## Методы

- `wotb.panel.new`
- `wotb.panel.mount`
- `wotb.panel.unmount`
- `wotb.panel.update`
- `wotb.panel.set_row`
- `wotb.panel.set_rows`
- `wotb.panel.set_row_visible`
- `wotb.panel.row`
- `wotb.panel.row_count`
- `wotb.panel.set_button_text`
- `wotb.panel.visible`
- `wotb.panel.set_visible`
- `wotb.panel.show`
- `wotb.panel.hide`
- `wotb.panel.layout`
- `wotb.panel.set_size`
- `wotb.panel.set_anchor`
- `wotb.panel.set_position`
- `wotb.panel.position`
- `wotb.panel.size`
- `wotb.panel.viewport`
- `wotb.panel.control`
- `wotb.panel.errors`
- `wotb.panel.label`
- `wotb.panel.button`
- `wotb.panel.image`
- `wotb.panel.scroll`
- `wotb.panel.column`
- `wotb.panel.on`
- `wotb.panel.destroy`

## Как пользоваться

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

### `wotb.panel`: контролы после `new()`

`label(opts)`, `button(opts)`, `image(opts)`, `scroll(opts)` добавляют контрол
в любой момент: смонтированная панель получает его сразу, несмонтированная —
при следующем `mount()`; ответ — имя для `panel:control(name)` (`3`, `"go"`,
`"image1"`, `"scroll1"`). `row({ x, y, gap, items = {...} })` и
`column({...})` раскладывают элементы (`kind = "label" | "button" | "image"`)
по оси; `row(index)` по-прежнему читает текст строки. `on("mounted" |
"unmounted" | "visibility" | "error", fn)` — слушатели панели; `destroy()`
размонтирует и забывает spec.

