# `wotb.config`

Фасад поверх raw-таблиц: [`wotb.core`](../reference/core.md), [`wotb.events`](../reference/events.md), [`wotb.settings`](../reference/settings.md), [`wotb.storage`](../reference/storage.md).

## Методы

- `wotb.config.см. описание`

## Как пользоваться

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

