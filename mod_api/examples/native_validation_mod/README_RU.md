# wotbmod.native_validation

Внутренний V3-мод для ручного live-прогона уже реализованного API. Пакет
жёстко ограничен клиентом `11.19.0.834` и SHA-256
`41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`.
На другом fingerprint preflight обязан заблокировать загрузку, поэтому старые
PASS не могут быть ошибочно перенесены на новый клиент.

Панель рисуется managed render API. Отдельный безопасный тест запрашивает
`wotbmod.ui` V3, получает active screen/snapshot и освобождает retained wrapper;
Projectile lifecycle проверяется через `wotbmod.projectile` V2, а callback
budget/profile — через `wotbmod.devtools` V3.
По умолчанию она скрыта и render callback не зарегистрирован: это исключает
постоянную диагностическую нагрузку. `F5` регистрирует callback на время
показа, повторный `F5` снимает его.
Управление:

- `F5` — показать или скрыть панель;
- кнопка `Export Validation Bundle` или `F4` — экспортировать privacy-safe
  архив с fingerprint, capability matrix, test results и sanitized logs;
- `Tab` / `Shift+Tab` — следующий или предыдущий раздел;
- `Up` / `Down` — выбрать capability;
- `F6` — выполнить только явно помеченный безопасный тест;
- `F7` — ручной PASS;
- `F8` — ручной FAIL; комментарий берётся из `FAIL_COMMENT.txt`;
- `F9` — SKIP;
- `F10` — RESET;
- `F11` — подтвердить событие смены режима камеры;
- `F12` — вручную включить тест, автоматически отключённый crash marker;
- `H` — прогнать пробу по всем опубликованным hook-символам сразу.

Раздел `Hooks` содержит по строке на каждое имя, которое binding pack
публикует через `resolve_symbol` (42 имени на 35 anchor'ов; часть функций
опубликована под несколькими написаниями, и каждое проверяется отдельно).
Проба вызывает `hooks->create_symbol` в режиме `OBSERVE` с нулевыми флагами.
Native backend поддерживает только `AROUND` и `REPLACE`, поэтому detour не
устанавливается никогда — это и есть смысл пробы: сигнатура и соглашение о
вызове большинства этих функций неизвестны, и неверный detour разрушил бы
стек. Классификация читается по коду возврата и тексту ошибки:
`only AROUND and REPLACE` — символ разрешился в этой сборке;
`not present in the reviewed binding pack` — не разрешился;
`native hook backend is unavailable` — backend выключен;
`E_CLIENT_MISMATCH` — fingerprint клиента не совпал;
`E_PERMISSION_DENIED` — нужен более высокий tier или именованный grant.

Все результаты находятся в
`mods/data/wotbmod.native_validation/`. Native capability не получает
`SUPPORTED`, пока пользователь не нажмёт PASS на текущем fingerprint.
Capability с отсутствующим backend или намеренно отключённым небезопасным
путём остаётся `NOT_SUPPORTED`.

Полный порядок действий и ожидаемые доказательства описаны в
`../../MANUAL_LIVE_VALIDATION_RU.md`.
