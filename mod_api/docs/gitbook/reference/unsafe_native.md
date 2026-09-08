# `wotb.unsafe_native`

Raw-таблица интерфейса `WotbModV3UnsafeNativeApiV1` (`include/wotbmod/unsafe_native_v1.h`, версия `WOTBMOD_V3_UNSAFE_NATIVE_VERSION`). Функции ниже вызываются как `wotb.unsafe_native.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `native.hook.address`. Имена объявляются в `permissions` манифеста.

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `create_address_hook` | — | *не публикуется в Lua: нужен сырой нативный указатель* |

