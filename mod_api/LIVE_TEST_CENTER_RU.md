# Live API Test Center

`wotbmod.native_validation` 1.1.0 — интерактивный read-only/passive validation-мод для точного fingerprint клиента.

## Запуск

Панель открывается клавишей `F5`. Пока панель скрыта, render callback снимается, draw calls не выполняются, экранный ring-buffer не перерисовывается. Подписки событий остаются активны только для выбранных capability; payload нормализуется и очищается от токенов, чата и персональных данных.

Экранный managed-renderer использует компактный ASCII-шрифт. Поэтому русские подписи на F5 автоматически транслитерируются на границе отрисовки (например, `Proverka fingerprint klienta`), а сама панель рисуется на полупрозрачной тёмной подложке. Это устраняет вопросительные знаки и не меняет русский текст в логах, отчётах и JSONL-трейсе.

Панель состоит из трёх колонок:

1. Categories — категория, число выбранных тестов, PASS/FAIL/WAIT и `NS` для `NOT_SUPPORTED`.
2. Tests / Events — checkbox, русское название, API id, статус, вызовы, thread id и ожидаемое действие.
3. Live Log — последние события из bounded ring-buffer с фильтром, паузой и копированием.

## Управление

| Клавиша | Действие |
|---|---|
| `TAB` / `Shift+TAB` | следующая/предыдущая категория |
| `Left` / `Right` | фокус: категории → тесты → лог |
| `Up` / `Down`, `Home`, `End` | выбрать категорию или тест |
| `Space` | включить/выключить выбранную capability |
| `F6` | запустить один безопасный active test |
| `F7` / `F8` / `F9` | ручной PASS / FAIL / SKIP |
| `F10` или `R` | сбросить выбранный результат |
| `F11` | ручной PASS для camera mode |
| `F12` | повторно включить test, отключённый crash-marker |
| `1..9` | Minimal, Hangar, Camera, Battle, UI, Render, Audio, Resources, Reload Stress |
| `0` | все доступные backend capability |
| `A` / `N` / `S` | все безопасные / снять всё / все supported |
| `U` | подтверждение unsafe-сессии (не делает unsupported функцию рабочей) |
| `L` / `X` | фильтр лога / только выбранный test |
| `K` / `O` / `C` | пауза / автопрокрутка / очистить ring-buffer |
| `M` / `-` / `+` / `B` | режим звука / громкость / battle mute |
| `E` / `Y` | Markdown+TXT отчёт / копировать последнюю строку |
| `H` | проба всех 42 опубликованных hook-символов (OBSERVE, без detour) |
| `F4` | полный sanitized validation bundle |
| `Shift+стрелки` | размер панели |
| `Ctrl+стрелки` | положение панели |

Unsafe/experimental capability отключены по умолчанию. `U` только явно подтверждает попытку; при отсутствии native binding результат остаётся `NOT_SUPPORTED` и записывается `WARN`.

## Статусы и fingerprint

Native capability никогда не повышается выше `LIVE_TEST_PENDING` без ручного `F7` после реального события на том же `client_version`, build, SHA-256 executable, binding pack, loader ABI и версии validation-мода. При смене fingerprint settings/results не импортируются. Старые файлы остаются на диске как исторические данные.

## Файлы

- `mods/logs/native_validation_live.log` — читаемый русский поток событий, batch flush не чаще раза в секунду.
- `mods/data/native_validation_mod/LIVE_EVENT_TRACE.jsonl` — sanitized structured trace без полного RPC payload, токенов, чата и PII (имя data-папки задаёт loader).
- `mods/data/native_validation_mod/LIVE_CAPABILITY_MATRIX.json` — capability/backend matrix.
- `mods/data/native_validation_mod/LIVE_VALIDATION_RESULTS.json` — verdicts и profiler.
- `mods/data/native_validation_mod/settings.live.json` — selection, sound, filters и geometry для текущего fingerprint.
- `mods/logs/native_validation_live_report.md` и `.txt` — сводный экспорт.

## Первый ручной прогон

1. Откройте F5 в ангаре, выберите пресет `1`, убедитесь, что `render.backend`, `camera.active`, `ui.validation_panel`, `audio.custom_file` показывают backend status.
2. Нажмите `F6` только для безопасных тестов; active tests не стартуют автоматически.
3. Выберите `Battle` (`4`) или `Projectile/Tracer` через категории, войдите в тренировочный бой и выстрелите.
4. Проверьте цепочку `SHELL_FIRED → PROJECTILE_CREATED → IN_FLIGHT → IMPACTED → DESTROYED`; отсутствие ID помечается `UNRESOLVED`, а не связывается эвристикой.
5. Переключите ARCADE/SNIPER, сделайте resize/Alt+Tab и вернитесь в ангар.
6. Нажмите `E`, затем `F4` и передайте оба экспортированных отчёта вместе с JSONL.

Полный список API id и критерии находится в [LIVE_TEST_EVENT_MAP.md](LIVE_TEST_EVENT_MAP.md).
