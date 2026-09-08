# `wotb.log`

Фасад поверх raw-таблиц: [`wotb.core`](../reference/core.md), [`wotb.events`](../reference/events.md), [`wotb.settings`](../reference/settings.md), [`wotb.storage`](../reference/storage.md).

## Методы

- `wotb.log.см. описание`

## Как пользоваться

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

