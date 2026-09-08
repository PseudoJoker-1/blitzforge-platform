# `wotb.store`

Фасад поверх raw-таблиц: [`wotb.storage`](../reference/storage.md), `wotb.json`.

## Методы

- `wotb.store.get`
- `wotb.store.set`
- `wotb.store.delete`
- `wotb.store.has`
- `wotb.store.keys`
- `wotb.store.clear`
- `wotb.store.flush`

## Как пользоваться

### `wotb.store`: типизированные значения

Поверх `wotb.storage` и `wotb.json`: `set(key, value)` кодирует таблицу,
`get(key [, default])` возвращает её обратно и второй результат `"stored"`
или `"default"`; `delete`, `has`, `flush`; `keys()` и `clear()` работают по
индексу, который модуль держит под зарезервированным ключом
`wotb.store.index` — ABI хранилище не перечисляет.

