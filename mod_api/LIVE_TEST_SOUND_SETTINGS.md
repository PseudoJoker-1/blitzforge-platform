# Звуковые уведомления Live Test Center

Validation-мод использует только собственные loose WAV-файлы, созданные в каталоге данных мода:

`mods/data/wotbmod.native_validation/audio/pass.wav`  
`mods/data/wotbmod.native_validation/audio/warn.wav`  
`mods/data/wotbmod.native_validation/audio/fail.wav`  
`mods/data/wotbmod.native_validation/audio/complete.wav`

Это не подмена штатного sound event клиента. URI монтируются как owner-scoped `mod://wotbmod.native_validation/audio/...`; удаление мода удаляет только его mount.

## Режимы

| Режим | Поведение |
|---|---|
| `OFF` | никаких feedback tones |
| `FAIL only` | только ручные/активные FAIL и ошибки |
| `PASS + FAIL` | PASS и FAIL; default |
| `selected events` | PASS/WARN/FAIL только для включённых тестов |

`M` циклически меняет режим, `-`/`+` изменяют громкость на 2%, `B` включает `Без звука во время боя`. Громкость ограничена `0..1` и сохраняется в `settings.live.json`.

## Ограничения

- глобальный rate limit — не чаще одного tone примерно за 180 ms;
- при переполнении несколько PASS объединяются в bounded counter;
- FAIL имеет приоритет над WARN, WARN над PASS;
- high-frequency `projectile.updated`, `entity.lifecycle`, `events.callback_thread` не ставят звук по умолчанию;
- play/preload/stop/destroy выполняются только на видимой панели, поэтому звук не создаёт скрытый render callback;
- при ошибке audio API событие остаётся в логе как WARN, crash не вызывается.

Путь к loose-файлу и fingerprint не пересекаются с игровым кэшем. Не используйте в audio directory токены, chat logs или файлы третьих модов.
