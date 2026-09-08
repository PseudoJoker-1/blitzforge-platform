# Повторяемый live-прогон и сбор evidence

Этот документ описывает checked-in процедуру запуска настоящего клиента
World of Tanks Blitz с установленным loader, снятия скриншотов и среза лога, а
затем вынесения машинного вердикта.

Раньше такой harness писался заново под каждый прогон и существовал только
внутри transcript агента. Скриншоты последнего прогона были уничтожены
документированным clean build (`Remove-Item -Recurse -Force mod_api\build`).
Теперь процедура — два reviewed скрипта:

- [`../tools/run_live_evidence.ps1`](../tools/run_live_evidence.ps1) — гигиена,
  baselines, staging реплея, запуск, DPI-корректный захват, штатное закрытие,
  срез лога;
- [`../tools/judge_live_slice.ps1`](../tools/judge_live_slice.ps1) — пять ворот
  вердикта и exit code.

Ручной сценарий native-валидации по шагам остаётся в
[`../MANUAL_LIVE_VALIDATION_RU.md`](../MANUAL_LIVE_VALIDATION_RU.md); здесь —
автоматизированная обвязка вокруг него.

## Честная область действия

Читайте это до того, как ссылаться на результаты прогона.

- Все имеющиеся battle/UI-скриншоты получены на **офлайновых локальных
  реплеях**, а не в живом бою.
- Часть ранних loadability smokes выполнялась в **залогиненном ангаре**.
- **Доказательств живого PvP-матча с подключённым loader нет.** Ни один прогон
  этого не подтверждал, и данная процедура такого прогона не предполагает.
- **Ничто здесь не проверялось против каких-либо систем детекта.** Отсутствие
  проверки — не утверждение о безопасности.
- Любой PASS относится только к точному fingerprint клиента `11.19.0.834`,
  SHA-256 `41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`.

Чистый вердикт означает ровно одно: в срезе лога нет плохих строк, клиент
завершился штатно, session marker снят, новых crash dump нет и заявленные
позитивные строки действительно найдены. Он не означает, что capability
работает корректно, — для этого существует ручной checklist.

## Что проверено по исходникам, а что взято на веру

Строки-эмиттеры ниже сверены с checked-in исходниками репозитория.

| Строка | Эмиттер | Файл |
|---|---|---|
| `=== wotb_mod proxy loaded ===` | `OpenLog` | `proxy_dll\dllmain.cpp:255` |
| `Native loader bootstrapped: path=... module=...` | `ModThread` | `proxy_dll\dllmain.cpp:328` |
| `[loader] BlitzForge live loader starting` | `WriteLogLine` | `mod_api\loader\wotb_mod_loader.cpp:3479` |
| `[loader] DAVA UI viewport=WxH backbuffer=WxH dpi=N` | render hook | `mod_api\loader\wotb_mod_loader.cpp:3273` |
| `[native-v3] V3 native binding id=... kind=... rva=0x... state=BOUND` | `NativeBindingsLog` | `mod_api\loader\v3_native_bindings.cpp:868` |
| `[v3:lua.host] <id>: installed Lua mod loaded` | `HostLog` + формат `[v3:%s] %s` | `mod_api\loader\lua\lua_host_mod.cpp:767`, `mod_api\src\wotb_mod_runtime.cpp:7355` |
| `package preflight ... client-build=... client-sha256=...` | `RuntimeLog` | `mod_api\src\wotb_mod_runtime.cpp:7940` |
| `load complete package-discovered=N package-loaded=N ... failed=0` | `RuntimeLog` | `mod_api\src\wotb_mod_runtime.cpp:8125` |
| `signature-status=2 key=...` | `LogPackageWarnings` | `mod_api\src\wotb_mod_runtime.cpp:7293` |

Значение `2` — это `WOTBMOD_V3_SIGNATURE_VALID` из
`include\wotbmod\manifest_v1.h:25`.

**Важная оговорка про `signature-status`.** Строка выводится внутри
`LogPackageWarnings`, а та возвращается сразу, если `warning_flags == 0`.
Полностью «чистый» подписанный пакет её не печатает вовсе. Поэтому
`signature-status=2` **не входит** ни в один preset и не должен требоваться
безусловно: иначе корректный прогон получит false DIRTY.

Отчёт `mods\cache\binding_pack_validation.json` пишется
`WriteBindingValidationReport` (`v3_native_bindings.cpp:916`) и содержит
`compatibility`, `checked_count`, `passed_count`, `mandatory_failed_count`,
`expected_executable_sha256` и список bindings. Ожидаемое чистое состояние:
`"compatibility": "supported"` и `"mandatory_failed_count": 0`.

Взято из transcript прошлого прогона и по исходникам **не подтверждено**:

- `SetBattleState` с переходом `CREATED` → `LOADED` как признак готовности боя.
  Ни один checked-in файл эту строку не печатает. Она используется как
  `-ReadyPattern` по умолчанию при запуске с реплеем и помечена в скриптах и в
  `run.json` как `pattern_verified: false`. Если строка не появится, прогон не
  падает: ready-привязанные шаги пропускаются, evidence собирается, вердикт
  выносится честно.
- Строка парсера `Unknown argument: [1] <path>` при передаче реплея позиционным
  аргументом. Ожидаемый шум, не ошибка.
- Утверждение, что `PrintWindow` возвращает чёрный кадр на DXGI swapchain.
  `PrintWindow` в скриптах не используется вовсе; захват идёт через
  `System.Drawing.Graphics.CopyFromScreen`, то есть blit с desktop DC.

## Предусловия и гигиена

Скрипт отказывается стартовать и ничего не трогает, если:

- каталог `-GameRoot` не существует или в нём нет `wotblitz.exe`;
- рядом с exe нет `steam_appid.txt` (содержимое `444200`). Без него
  `steam_api.dll` не инициализируется вне лаунчера и прямой запуск зависает.
  Обход — `-AllowMissingSteamAppId`;
- `wotblitz` уже запущен. Harness никогда не подключается к чужому клиенту и
  никогда его не убивает;
- `-Replay` не существует или у файла не расширение `.wotbreplay`;
- `-OutputDirectory` находится внутри `mod_api\build`;
- момент в `-Captures`/`-Keys` не разбирается или regex невалиден.

Дальше, уже с записью на диск:

1. **Baselines снимаются ДО запуска**: длина `wotb_mod_loader.log` в байтах,
   длина `wotb_mod.log`, список `%LOCALAPPDATA%\CrashDumps\wotblitz*.dmp`.
   Loader-лог открывается с `FILE_APPEND_DATA` и никогда не ротируется; сейчас
   он ~144 МБ, поэтому читать его целиком нельзя — срез берётся строго по
   byte-offset от baseline.
2. **Stale marker переносится, а не удаляется.** Если
   `mods\cache\runtime_session.marker` существует, он получает имя
   `runtime_session.marker.pre-run-<stamp>.bak` и копируется в evidence.
   Оставленный marker отправляет следующий запуск в safe mode; один прогон уже
   был выброшен именно из-за force-kill, оставившего marker.
3. **Реплей копируется в свежий GUID-каталог** во `%TEMP%`. Оригиналу игрока
   клиент не передаётся никогда.

## Запуск

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_live_evidence.ps1 `
  -Replay "$env:LOCALAPPDATA\wotblitz\DAVAProject\replays\<файл>.wotbreplay" `
  -Captures ready,ready+6,ready+20,preclose `
  -Keys F8@ready+8 `
  -RequirePreset loader,lua-host
```

Сначала всегда полезно прогнать план без запуска клиента:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\run_live_evidence.ps1 `
  -Replay "<путь>" -Captures ready,preclose -DryRun
```

`-DryRun` выполняет весь preflight, печатает расписание и требуемые шаблоны и
завершается, не запуская клиент и ничего не записав.

### Аргументы-массивы указываются через запятую

Это не косметика. При вызове через `powershell -File` конструкция
`-Captures ready ready+6` привязывает к `-Captures` **только первое значение**,
а остальные уходят в следующий позиционный параметр — то есть `ready+6` молча
станет значением `-Replay`. Поэтому оба скрипта объявлены с
`[CmdletBinding(PositionalBinding = $false)]`: такая ошибка теперь падает
громко. Правильно: `-Captures ready,ready+6,preclose`. Для `-Captures` и
`-Keys` дополнительно принимается одна строка с запятыми.

Regex-параметры (`-RequirePattern`, `-BadLinePattern`) по запятым **не
режутся**: запятая допустима внутри regex, например `\d{2,3}`.

### Грамматика моментов

Одинакова для `-Captures` и для части `-Keys` после `@`.

| Форма | Значение |
|---|---|
| `ready` | момент срабатывания `-ReadyPattern` |
| `ready+N` | через N секунд после готовности |
| `launch` / `launch+N` | в момент запуска / через N секунд после запуска |
| `N`, `Ns`, `t=N` | то же, что `launch+N` |
| `preclose` | непосредственно перед отправкой `WM_CLOSE` |

Необязательный суффикс `:метка` задаёт имя артефакта:
`ready+3:hud-open` → `NN-hud-open.png`.

Ключи: `F8` (по умолчанию в момент `ready`), `F8@ready+4`, `0x77@launch+50`,
`ESC@preclose`. Принимаются `F1`..`F12`, одиночные `A-Z`/`0-9`, имена
`ESC`/`ENTER`/`SPACE`/`TAB`/`BACKSPACE` и коды вида `0xNN`. `F8` — это VK
`0x77`, он открывает Runtime UI Lab.

### Параметры `run_live_evidence.ps1`

| Параметр | По умолчанию | Назначение |
|---|---|---|
| `-GameRoot` | `%ProgramFiles(x86)%\Steam\steamapps\common\World of Tanks Blitz` | Корень игры; должен содержать `wotblitz.exe` |
| `-Replay` | пусто | Офлайновый локальный реплей. Пусто — запуск в ангар |
| `-OutputDirectory` | `%LOCALAPPDATA%\wotbmod\live_evidence` | Корень evidence; создаётся подкаталог прогона |
| `-Captures` | `launch+25,ready,ready+6,preclose` | Моменты скриншотов |
| `-Keys` | пусто | Нажатия клавиш |
| `-TimeoutSeconds` | `300` | Общий бюджет от запуска до начала завершения |
| `-ReadyTimeoutSeconds` | `180` | Бюджет ожидания готовности |
| `-ReadyPattern` | с реплеем `SetBattleState[^\r\n]*LOADED`, иначе `load complete package-discovered=` | Признак готовности в loader-логе |
| `-PostScheduleSeconds` | `3` | Пауза после последнего шага перед закрытием |
| `-MarkerSettleSeconds` | `30` | Сколько ждать снятия session marker после выхода процесса |
| `-CaptureMode` | `Window` | `Window` — прямоугольник окна клиента, `Screen` — весь виртуальный рабочий стол |
| `-BadLinePattern` | см. ниже | Regex плохих строк, передаётся судье |
| `-RequirePattern` | пусто | Дополнительные обязательные позитивные шаблоны |
| `-RequirePreset` | `loader` | `loader`, `native-bindings`, `lua-host`, `battle-unverified` |
| `-DryRun` | — | Только preflight и план |
| `-AllowMissingSteamAppId` | — | Разрешить запуск без `steam_appid.txt` |
| `-SkipJudge` | — | Собрать evidence без вердикта |
| `-KeepStagedReplay` | — | Не удалять временную копию реплея |

### Захват экрана

`SetProcessDPIAware()` вызывается в самом начале скрипта, **до любого чтения
геометрии**, в том же процессе PowerShell. Рабочий стол здесь масштабирован на
125%: DPI-неосведомлённый процесс сообщает `1536x864` и молча снимает
уменьшенный кадр. Loader независимо пишет в лог
`DAVA UI viewport=1536x864 backbuffer=1920x1080 dpi=120` — расхождение видно и
оттуда.

Перед каждым скриншотом и каждым нажатием окно поднимается через
`ShowWindow(SW_RESTORE)` + `SetForegroundWindow`, и факт получения фокуса
проверяется через `GetForegroundWindow`. `keybd_event` доходит до игры только с
фокусом; если фокус подтвердить не удалось, шаг помечается
`foreground: false`, а не выдаётся за успешный.

Для каждого кадра в `run.json` пишутся `mean_luma` и
`distinct_sample_colors`. Это дешёвый детектор чёрного кадра: если в выборке
оказалось два цвета или меньше, кадр помечается `looks_blank: true` и в
консоль идёт предупреждение.

## Содержимое каталога evidence

| Файл | Что это |
|---|---|
| `run.json` | Манифест прогона: параметры, baselines, расписание, метрики кадров, состояние процесса |
| `loader_slice.log` | Срез `wotb_mod_loader.log` строго по байтам `[baseline, конец)` |
| `wotb_mod.log` | Копия лога proxy. Он открывается в режиме `"w"` для `wotblitz.exe`, поэтому целиком относится к этому прогону |
| `captures\NN-<метка>.png` | Скриншоты в порядке срабатывания |
| `binding_pack_validation.json` | Копия отчёта валидации binding pack |
| `mods.ini.snapshot` | Какие моды были включены |
| `runtime_session.marker.pre-run` | Копия найденного stale marker, если он был |
| `runtime_session.marker.post-run` | Появляется, только если marker пережил прогон |
| `crashdumps_new.txt` | Имена новых dump, если они появились |
| `judge_request.json` | Точные входные данные судьи |
| `judge.txt`, `verdict.json` | Вывод судьи и машинный вердикт |

## Завершение клиента

Только `CloseMainWindow()` (`WM_CLOSE`) и `WaitForExit`. **Force-kill не
выполняется никогда** — именно он оставляет `runtime_session.marker` и
отправляет следующий запуск в safe mode. Если клиент не завершился за 120
секунд, скрипт сообщает об этом и просит закрыть клиент вручную; в `run.json`
остаётся `force_killed: false`.

Снятие marker отстаёт от выхода процесса. В наблюдавшемся прогоне handle
процесса сигнализировал в `22:44:22`, а marker был переименован в
`runtime_session.marker.ended-<stamp>` только в `22:44:54`, без каких-либо
запусков в промежутке. Поэтому и runner, и судья ждут снятия marker в пределах
`-MarkerSettleSeconds`, а не проверяют его мгновенно: мгновенная проверка даёт
false DIRTY.

## Вердикт

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\judge_live_slice.ps1 `
  -ParameterFile "<каталог прогона>\judge_request.json"
```

Runner вызывает судью сам, отдельным процессом, через файл параметров — так
метасимволы regex не портятся квотингом командной строки, а точные входные
данные остаются в evidence.

### Пять ворот

| Ворота | Условие |
|---|---|
| 1 `slice_clean` | в evidence-тексте нет строк, совпавших с `-BadLinePattern` |
| 2 `process_exited` | клиент действительно завершился |
| 3 `marker_absent` | `mods\cache\runtime_session.marker` отсутствует (с учётом settle-окна) |
| 4 `crash_dumps_unchanged` | число `wotblitz*.dmp` равно baseline |
| 5 `positives_present` | найден каждый обязательный позитивный шаблон |

Ворота 1–4 — исходный контракт из четырёх пунктов. Ворота 5 существуют потому,
что первые четыре **проходят на полностью тихом логе, в котором вообще ничего
не загрузилось**. Прогон, ничего не доказывающий, обязан падать, а не считаться
чистым. Поэтому `-RequirePattern` обязателен и не может быть пустым: пустой
список — это ошибка входных данных, а не чистый вердикт.

Regex плохих строк по умолчанию:

```
\[(error|fatal)\]|safe-mode=active|package blocked|manifest refused|instruction budget|script disabled|callback failure|access violation|load complete.*failed=[1-9]
```

Сравнение регистронезависимое. Плохие строки ищутся и в срезе, и в
дополнительных файлах evidence; каждая печатается с именем файла и номером
строки.

### Коды выхода

| Код | Значение |
|---|---|
| `0` | CLEAN — все пять ворот пройдены |
| `1` | DIRTY — хотя бы одни ворота не пройдены; нарушившие строки напечатаны |
| `2` | Ошибка входных данных: проверка не выполнялась. Это **не** вердикт |

Разделение `1` и `2` намеренное: «прогон грязный» и «судья не смог
отработать» — разные вещи, и их нельзя путать в CI.

### Параметры `judge_live_slice.ps1`

| Параметр | По умолчанию | Назначение |
|---|---|---|
| `-ParameterFile` | пусто | JSON, подставляющий любые не заданные в командной строке параметры |
| `-SlicePath` | — | Срез loader-лога; обязателен |
| `-AdditionalEvidenceFile` | пусто | Дополнительные файлы, сканируемые и на плохие строки, и на позитивы |
| `-MarkerPath` | — | Путь к session marker; обязателен |
| `-MarkerSettleSeconds` | `30` | Ожидание снятия marker. Runner передаёт `0`, потому что уже подождал сам |
| `-ProcessExited` | — | `true` или `false`; обязателен. Судья это не угадывает |
| `-BaselineCrashDumpCount` | `-1` | Число dump до запуска; обязателен |
| `-CrashDumpDirectory` | `%LOCALAPPDATA%\CrashDumps` | Где искать dump |
| `-CrashDumpFilter` | `wotblitz*.dmp` | Покрывает и `wotblitz.exe.N.dmp`, и `wotblitz.exe(1).N.dmp` |
| `-RequirePattern` | пусто | Обязательные позитивные шаблоны; пустой список — ошибка |
| `-BadLinePattern` | см. выше | Regex плохих строк |
| `-MaxOffendingLines` | `50` | Сколько плохих строк печатать полностью; счётчик всегда точен |
| `-JsonOutputPath` | пусто | Машинный вердикт |

Сбой записи `verdict.json` не меняет вердикт: он уже вынесен, ошибка
сериализации лишь печатает предупреждение.

## Почему evidence не лежит в `mod_api\build`

Документированный clean build — это `Remove-Item -Recurse -Force mod_api\build`
с последующим `build.cmd`. Каталог удаляется целиком, и именно так были
потеряны скриншоты прошлого прогона.

Выбрано следующее:

1. Каталог по умолчанию — `%LOCALAPPDATA%\wotbmod\live_evidence\<run-id>`, то
   есть вне репозитория и вне любого build-дерева. Его не трогают ни clean
   build, ни `git clean`.
2. `-OutputDirectory` внутри `mod_api\build` **запрещён**: скрипт падает на
   preflight с объяснением. Копирование артефактов «наружу» после прогона не
   используется — надёжнее просто никогда не писать в удаляемый каталог.
3. Если `-OutputDirectory` всё же указывает внутрь рабочего дерева, выводится
   предупреждение. Для случая внутри `mod_api\` добавлен
   [`../.gitignore`](../.gitignore) с шаблоном `live_evidence/`: кадры сняты с
   залогиненного клиента и не должны попадать в коммит.

## Откат и удаление

### Kill switch без удаления

Быстрее и безопаснее всего отключить Lua host, не трогая файлы: в
`<корень игры>\mods\mods.ini` в секции `[mods]` поставить

```ini
wotbmod.lua_host=0
```

Loader и proxy остаются на месте, host не загружается. Это штатный способ
локализовать проблему перед тем, как что-то удалять.

### Удаление public-preview bundle

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\uninstall.ps1
```

[`../release/public_preview/uninstall.ps1`](../release/public_preview/uninstall.ps1)
работает только по своему state-файлу `mods\cache\public-preview-install.json`,
сверяет SHA-256 каждого установленного файла и отказывается что-либо трогать,
если файл изменён посторонней программой. Оригиналы восстанавливаются из
`mods\cache\install_backups`, ключи `wotbmod.lua_host` в секциях `[mods]` и
`[permissions]` возвращаются в исходное состояние, а recovery backup
сохраняется для ручной проверки. Untracked-удаление запрещено.

### Удаление proxy

[`../../proxy_dll/uninstall.cmd`](../../proxy_dll/uninstall.cmd) удаляет
`version.dll`, `vorig.dll` и `wotb_mod_loader.dll` из корня игры и
восстанавливает `version.dll` из `version.dll.bak`, если бэкап есть. Если
бэкапа нет, это нормально: штатно игра использует системный `version.dll`.

### Датированные бэкапы loader

В корне игры накапливаются файлы вида
`wotb_mod_loader.dll.<YYYYMMDD-HHMMSS>.bak` и именованные срезы
(`wotb_mod_loader.dll.pre-rc1-20260802`,
`wotb_mod_loader.dll.pre-camera-write-fix-20260809-2103.bak` и подобные). Чтобы
откатиться на конкретную сборку, достаточно скопировать нужный бэкап поверх
`wotb_mod_loader.dll` при закрытом клиенте. Аналогично в
`mods\cache` лежат датированные копии session marker
(`runtime_session.marker.ended-*`, `*.stale-backup-*`, `*.pre-run-*`) — они
только для разбора инцидентов и на запуск не влияют.

После любого отката проверьте, что `mods\cache\runtime_session.marker`
отсутствует, иначе следующий запуск уйдёт в safe mode.

## Известные ограничения

- `-ReadyPattern` по умолчанию для реплея не подтверждён исходниками. Если он
  не сработает, ready-привязанные шаги будут пропущены и вердикт станет DIRTY
  из-за ненайденных позитивов — это честный результат, а не сбой harness.
- Клиент в наблюдавшихся прогонах возвращал exit code `1` при закрытии через
  `WM_CLOSE` на стадии загрузки. Код выхода фиксируется в `run.json`, но ни в
  какие ворота не входит: контракт требует только факта завершения.
- Захват берёт кадр с рабочего стола, поэтому перекрывающее окно попадёт в
  скриншот. Прогон должен идти на свободном рабочем столе.
- `SetForegroundWindow` может быть заблокирован системой. Скрипт делает
  несколько попыток и честно помечает неудачу, но не применяет обходов через
  инъекцию дополнительных клавиш.
- Harness не проверяет содержимое скриншотов по существу. Он доказывает, что
  кадр снят, не чёрный и относится к нужному моменту; смысловая сверка — задача
  ручного checklist.
