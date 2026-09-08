# Sandbox

Доступны base library и `table`, `string`, `math`. Не доступны `io`, `os`,
`package`, `debug`, `coroutine`, `utf8`, `require`, `dofile`, `loadfile`,
`collectgarbage` и `warn`.

Sandbox-версия `load(chunk [, chunkname [, mode [, env]]])` принимает только
строку исходного текста. Reader function и bytecode отклоняются. Аргументы
`mode` и `env` принимаются для совместимости, но игнорируются: используется
только text mode и окружение текущего скрипта.

`print(...)` с permission `core` пишет через `wotbmod.core.log`; без него
остаётся только debugger sink с именем скрипта.
