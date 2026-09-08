# Ручная live-валидация WotbMod API V3

Этот сценарий предназначен для `wotbmod.native_validation` версии `1.1.0`.
Автоматическая сборка и host-тест не запускают клиент и не доказывают работу
native hook в игре. `SUPPORTED` для native capability появляется только после
ручного `F7` на точном fingerprint клиента.

## Перед запуском

1. Соберите пакет командой `loader\build_live.cmd`.
2. Убедитесь, что пакет находится в
   `build\live_env\mods\wotbmod.native_validation`, а в `mods.ini` есть
   `wotbmod.native_validation=1` и permission tier `3`.
3. Запускайте клиент только через уже используемый проектом launcher/install
   flow. Validation-мод не должен загружаться на клиенте, отличном от
   `11.19.0.834` / SHA-256
   `41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`.
4. Перед новым прогоном сохраните старый каталог
   `mods\data\wotbmod.native_validation`, если он нужен. PASS из другого
   `fingerprint_key` мод не импортирует.

Панель: `F5` показать/скрыть, `Tab`/`Shift+Tab` раздел, `Up`/`Down` тест,
кнопка `Export Validation Bundle` или `F4` экспортирует Validation Bundle,
`F6` выполняет безопасный запуск, `F7` ставит PASS,
`F8` FAIL, `F9` SKIP, `F10` RESET,
`F11` подтвердить смену режима камеры, `F12` повторно включить тест после
crash marker. Для FAIL сначала запишите короткий комментарий в
`FAIL_COMMENT.txt`. Токены, пароли, e-mail, Authorization/Bearer и строки с
`@` заменяются на `[REDACTED_BY_PRIVACY_POLICY]`.

Панель по умолчанию скрыта. Пока она скрыта, validation-мод не держит render
callback. Системные события подписаны точечно; `wotbmod.frame.update` и
`wotbmod.frame.fixed_update` намеренно не записываются в trace. Snapshot JSON
сохраняется не чаще одного maintenance-прохода, а не внутри каждого callback.

Допустимые callback-потоки ниже указаны по роли: `MAIN` или `RENDER`.
`UNKNOWN`, `AUDIO`, `WORKER` и `IO` для gameplay/UI callback считаются
ошибкой, если строка шага не говорит обратное. Счётчик означает значение до
шага `N` и после него не меньше `N+1`; несколько штатных событий допускают
большее приращение.

## Категории RC1

### Обязательные для RC1

| Сценарий | Точный ожидаемый event/counter | Статус PASS | Файл evidence |
|---|---|---|---|
| Запуск и ангар | `wotbmod.client.ready`, `wotbmod.game.state_changed`; `lifecycle.info >= 1`, state=`HANGAR` | portable=`HOST_TESTED`; native остаётся `LIVE_TEST_PENDING` до `F7` | `LIVE_EVENT_TRACE.jsonl`, `LIVE_CAPABILITY_MATRIX.json`, loader log |
| Смена танка | `wotbmod.vehicle.local.changed`, затем `wotbmod.vehicle.local.appearance_ready`; `vehicle.local_changed +1` | payload относится к выбранному local vehicle, старый borrowed handle invalid | trace, matrix |
| UI inspection | `wotbmod.ui.screen_changed` при реальной смене; `ui.snapshot +1` | name/type/path/geometry реальны; text=`NOT_EXPOSED_RC1`, без fabricated value | trace, matrix |
| `sample.ui_transaction` | loader log `sample.ui_transaction`; state bits inspection/mutation/layout/event, затем rollback/cleanup | click получен; после unload geometry исходная, callback/object отсутствуют | loader log, sample state export |
| `sample.vehicle_cosmetic` | `wotbmod.vehicle.local.changed`; apply/state/reapply counters | exact mesh/texture/LOD mounts применены; `requires_model_reload=1` допустим; rollback возвращает stock | loader log, sample state, matrix |
| `sample.camera_render` | `wotbmod.render.swapchain_resized`/`backend_changed`; draw calls/frame=`1` при видимом overlay | mode реальный или `UNKNOWN`; после resize/device restore одна строка; unload удаляет callback/modifier | trace, loader log, profiler counters |
| Resize и Alt+Tab | `wotbmod.render.swapchain_resized`; при recreation `device_lost -> device_restored`; counters +1 | viewport обновлён, callback восстановлен без дубля | trace, matrix |
| Графические настройки | `wotbmod.render.backend_changed` либо честное отсутствие backend change; counter меняется только при факте | никаких stale borrowed native objects | trace, loader log |
| Training room | `wotbmod.battle.entered`, `countdown_started`, `battle.started`; каждый ожидаемый counter +1 | порядок не нарушен, callback thread разрешён | trace, matrix |
| Один выстрел | `wotbmod.gameplay.shot_fired`, затем `wotbmod.projectile.created` и `updated`; `shell.ingress +1` | correlation ID совпадает либо явно `unresolved`; нет дубля | trace, matrix |
| Серия выстрелов | на каждый принятый shot один `shot_fired` и один lifecycle start; counters растут на фактическое число | callback budget не превышен, trace coalescing не теряет lifecycle edges | trace, profiler counters |
| Projectile lifecycle | `projectile.created -> updated* -> impacted? -> destroyed`; created/destroyed balance возвращается к 0 active | provenance/valid-fields честны; timeout cleanup допустим и отмечен | trace, matrix |
| Результаты | `wotbmod.battle.ended`, `battle.left`, `scene.deactivated`, state=`RESULTS` | battle handles/callbacks очищены | trace, matrix |
| Возврат в ангар | `wotbmod.game.state_changed` state=`HANGAR`, local vehicle events | active battle/projectile handles=0 | trace, matrix |
| Unload/reload | `wotbmod.mod.disabled`, `unloading`, `unloaded`, затем load sequence; callbacks после unload=0 | inverse cleanup полностью выполнен | trace, loader log, results |
| 30 reload cycles | `load_cycle_count +30`; leaks/dangling/callback-after-unload=0 | все 30 циклов clean, без crash marker | results, failures, loader log |
| Корректное закрытие | `wotbmod.client.shutting_down`, `validation.disabling`; session marker отсутствует | JSON/JSONL читаются, `ACTIVE_NATIVE_TEST.json` отсутствует | results, trace, `mods/cache` |
| Повторный запуск после safe mode | первый запуск: full safe mode `mod_count=0`; portable-only: только content packages; override: auto-disabled culprit не загружен | клиент достигает ангара, diagnostics доступны, marker attribution сохранён | loader log, `runtime_session.marker`, `auto_disabled_mod.ini`, `crash_history.ini` |

### Optional experimental

- Replay и camera modes без доказанного native source.
- Read-only full Scene/Material inspection, native typed UI и live NMaterial.
- Stock tracer styles/visual attachment и native DAVA YAML/ResourceArchive.
- Semantic audio/hangar/localization overrides.

Отсутствующий backend здесь получает `SKIP`/`NOT_SUPPORTED`, а не fabricated
PASS.

### Destructive/stress

- 30 unload/reload cycles, серия выстрелов и длительный callback-budget run.
- Многократный resize/fullscreen/Alt+Tab и device recreation.
- Намеренно аварийный safe-mode fixture выполняется только в test environment;
  он не должен использоваться с обычным профилем модов.

### Unsafe, отключено по умолчанию

- `native.memory`, `native.memory_patch`, `native.hook.address`,
  `bigworld.rpc.modify`, `render.native` и raw COM/native objects.
- Unsafe шаг выполняется только по exact fingerprint, explicit tier `3` и
  отдельному ручному подтверждению. Для RC1 он не обязателен.

## Подробные 20 шагов live-прогона

| № | Действие | Ожидаемое событие и счётчик | Ожидаемый payload | Поток | Статус после проверки | Ошибка | Проверить |
|---:|---|---|---|---|---|---|---|
| 1 | Запустить игру, затем нажать `F5` | `validation.enabled`; `lifecycle.info` ≥1; render callback начинает расти только после открытия панели и перестаёт расти после закрытия | fingerprint, binding pack, client state без raw pointers | `MAIN`; render только `RENDER` | native не выше `LIVE_TEST_PENDING`; panel `LIVE_TEST_PENDING` | package не загружен, SHA mismatch, панель отсутствует после F5, callback растёт при скрытой панели | `LIVE_EVENT_TRACE.jsonl`, `LIVE_CAPABILITY_MATRIX.json`, loader log |
| 2 | Войти в ангар | hangar/client-state event; `client.fingerprint` и `lifecycle.info` можно запустить `F6` | `client_state=HANGAR`, version/build/SHA | `MAIN` | успешные portable проверки `HOST_TESTED`; native pending | state остаётся UNKNOWN/LOADING, duplicate callbacks | trace, matrix |
| 3 | Сменить танк | `local_vehicle_changed` и/или public entity lifecycle; entity count увеличивается | только public ID/type/local/visible; без скрытых enemy данных | `MAIN` | `entity.lifecycle` остаётся pending до `F7` | нет события, старый handle не invalidated, чужая скрытая entity | trace, matrix |
| 4 | Открыть настройки | `ui_screen_changed`, либо честное отсутствие native UI source | только screen/resource presence, без pointer | `MAIN` | event capability pending; `ui.readonly_inspector=NOT_SUPPORTED` | UI inspector вызывает mutation/crash или выдаёт выдуманное дерево | trace, matrix |
| 5 | Изменить графические настройки и переключить window/fullscreen, если доступно | `render.backend_changed`, device lifecycle или resize; соответствующий count растёт | backend, viewport, availability/component change flags | `RENDER` или доставленный `MAIN` | render native не выше pending до PASS | device/context/swapchain пропали без restore, callback остановился | trace, matrix, loader log |
| 6 | Переключить доступные режимы камеры | `wotbmod.camera.mode_changed`; `camera.mode_change` +1 на реальную смену | previous/mode/native_mode; ненадёжное значение=`UNKNOWN` | `MAIN` | после визуальной сверки `F11` или `F7` → `SUPPORTED` только для текущего SHA | mode угадан без события, дубли на неизменном режиме, неверный thread | trace, matrix |
| 7 | Открыть replay, если он есть; иначе `F9` | replay/context events; camera/entity/render продолжают обновляться | только публичные replay metadata | `MAIN`, render=`RENDER` | подтверждённые native capabilities можно PASS; отсутствующее `SKIP` | скрытые live enemy данные или mutable RPC | trace, results |
| 8 | Войти в тренировочную комнату | context/battle-enter sequence; entity lifecycle count растёт | context TRAINING, публичные entity IDs | `MAIN` | entity/events pending | событие пришло напрямую из network callback или на неизвестном thread | trace |
| 9 | Загрузить карту | scene activated, battle entered/started, render callbacks продолжаются | resource presence, public battle metadata; no native pointer | `MAIN`; render=`RENDER` | `scene.readonly_inspector=NOT_SUPPORTED`; battle hooks pending | ранний unsafe Scene call, render callback прекратился | trace, matrix |
| 10 | Произвести один выстрел | `local_shell_fired`/`shot_fired`; `shell.ingress` +1 | `shot_id`/`projectile_id` только если доказаны, иначе `unresolved` | `MAIN` | `shell.ingress` pending, после сверки можно `F7` | ложная correlation, событие чужого/невидимого снаряда | trace |
| 11 | Произвести серию выстрелов | shell ingress растёт на число реально принятых событий; `wotbmod.projectile.created -> updated* -> impacted? -> destroyed`; отсутствие дублей | каждый ID отдельно; unresolved допустим; provenance/valid-fields обязательны | `MAIN` | lifecycle остаётся `LIVE_TEST_PENDING` до сверки и `F7`; stock tracer style отдельно `NOT_SUPPORTED` | count меньше фактического, дубли, stale handle принимается, active projectile не очищен | trace, matrix |
| 12 | Дождаться попадания | visual impact и `shell_hit` только при реальном ingress; `impact.confirmed` +1 лишь если событие подтверждено | impact/shot ID или `unresolved`; position без скрытых данных | `MAIN` | confirmed impact можно PASS только при совпадении с экраном | visual effect выдан за confirmed hit, fabricated correlation | trace |
| 13 | Уничтожить или увидеть уничтожение техники | vehicle destroyed/despawn/public removal; entity lifecycle +1 | public ID, reason, alive=false; destruction time в trace | `MAIN` | entity lifecycle можно PASS после сверки | скрытая entity остаётся доступной, удалённый handle жив | trace, matrix |
| 14 | Выйти из боя | battle ended/left, scene deactivated, tracer/entity cleanup | reason/context; active handle counts стремятся к 0 | `MAIN` | cleanup/event capabilities после проверки PASS | callbacks после owner stop, leaked subscriptions/handles | trace, matrix |
| 15 | Открыть результаты | client state=`RESULTS`; render продолжает работать | только публичный state | `MAIN`, render=`RENDER` | lifecycle/events pending или PASS после сверки | UNKNOWN state, потеря render callback | trace |
| 16 | Вернуться в ангар | client state=`HANGAR`; scene/battle cleanup завершён | state, active handles; без raw objects | `MAIN` | cleanup можно PASS при нулевых лишних handles | battle entity/tracer handles не invalidated | matrix, trace |
| 17 | Выбрать `client.leave_to_hangar` и нажать `F6` только из допустимого не-ангарного состояния | штатный LeaveToHangar и переход; call_count +1 | result/error code и state transition | вызов `MAIN`, native callback нормализуется loader queue | при визуально корректном переходе `F7`; в HANGAR допустим отказ | crash, вызов без owner, `OK` без перехода, wrong thread | trace, matrix, loader log |
| 18 | Проверить Reload: hangar, активная подписка, async resource, deferred callback; затем накопить 30 чистых load cycles | reload capability count; `load_cycle_count` растёт; `request_reload` ставит перезагрузку в очередь и выполняет её на границе кадра (host-tested; сборки до 23.08.2026 отвечали `hot_reload=0`) | test ID, result, cleanup counts, cancellation/rollback result | запрос `MAIN`; callback reload только через deferred loader queue | unsupported остаётся `NOT_SUPPORTED`; 30 чистых циклов → `STRESS_TESTED` | DLL выгружена внутри callback, callback после unload, потерян rollback | results, matrix, failures, loader log |
| 19 | Изменить размер окна и выполнить Alt+Tab; повторить window/fullscreen | resize/device lost/restored/swapchain recreation counts растут при соответствующем факте | viewport before/after, backend, availability; pointer только presence/absence | `RENDER`/очередь `MAIN` | render capabilities PASS только после устойчивого restore | stale borrowed handle после frame, callback не восстановился | trace, matrix |
| 20 | Корректно закрыть клиент | `validation.disabling`, cleanup, затем отсутствие активного crash marker | owner, final counts, result; без секретов | `MAIN` | сохранённые PASS относятся только к точному fingerprint | `ACTIVE_NATIVE_TEST.json` остался после чистого выхода, повреждён JSON | results, matrix, failures, trace |

## Проверки разделов панели

- Camera должна показывать handle как opaque ID, generation, current/previous
  mode, event/duplicate counts, callback thread и lifetime. Если mode не
  доказан — `UNKNOWN`.
- BigWorld Entity допускает только local или visible public entities. Проверяйте
  public ID/type/generation/alive, create/destroy timestamps, pre-existing flag
  и invalidation старого handle по trace/matrix. Скрытые противники запрещены.
- RPC Metadata должна показывать public entity ID, method key, direction,
  timestamp, schema-known и payload-size-known. Полный payload не сохраняется;
  текущий ABI честно пишет unknown size как `0` + `payload_size_known=false`.
- Projectile/Tracer обязаны писать четыре correlation ID. Любая недоказанная
  связь — строка `unresolved`. `projectile.created/updated/impacted/destroyed`
  и managed `impact.visual` остаются `LIVE_TEST_PENDING` до подтверждённого
  shot/hit ingress и ручного PASS на точном fingerprint. Штатные
  `tracer.requested`/`tracer.style` остаются `NOT_SUPPORTED`;
  `impact.confirmed` проверяется отдельно только по подтверждённому ingress.
- Native Render выводит только `present/absent`; числовые COM pointer values не
  попадают в сохранённые файлы. Borrowed lifetime ограничен callback/frame.
- UI/Scene/Material inspectors read-only. В текущем backend native enumeration
  не доказана, поэтому ожидается `E_NOT_SUPPORTED`, без попытки mutation.
- Portable YAML, DVPL и ZIP — поддерживаемые portable backends. Native DAVA
  YAML/archive ожидаемо отключены до доказательства ABI/ownership/allocator/
  destructor/exceptions/thread/corrupted-input behavior.
- GES показывает `types`, `events`, `distinct`, `publish` и `echo_seen`.
  PASS: `types >= 500`, после 60 с боя `events > 0` и `distinct >= 20`,
  строка `ges.publish_echo` в бою даёт `publish=OK` и `echo_seen=1`. Вне боя
  publish честно пишет `not in battle` и `E_NOT_SUPPORTED`. В
  `LIVE_EVENT_TRACE.jsonl` каждый новый тип пишется один раз с
  `publisher_rva` и первыми 32 байтами payload. Живой PASS 4 сентября 2026
  (11.20.0.887, тренировочная комната): `types=601 events=10508 distinct=84
  publish=OK echo_seen=1`; publish выполняется через очередь главного потока
  DAVA — с render-потока лоадер честно отвечает `E_WRONG_THREAD`.
- Hooks (клавиша `H`): на 22 именах с детуром лоадера строка должна содержать
  «OBSERVE attached and detached (OK)», на остальных — «symbol RESOLVED; no
  observer point on this target». «UNEXPECTED OK» или «unclassified outcome»
  — FAIL. Проверка живая: на время пробы no-op наблюдатель действительно
  подключён к детуру.
- `ui.live_text` (UI): обход активного экрана в ширину, у каждого контрола
  запрашивается живой текст. PASS: `with_text > 0` и в `samples` видны
  реальные надписи экрана (например, названия кнопок ангара). `first_error`
  с `E_CALLBACK_FAULT` или `with_text=0` на экране с текстом — FAIL:
  раскладка `UITextComponent`/списка компонентов на этом fingerprint не
  подтвердилась. Та же проба пишет `LIVE_UI_TREE.jsonl` (индекс, родитель,
  id, тип, флаги, rect, число детей, живой текст каждого контрола активного
  экрана). Снимите его в ангаре и в бою: по нему HUD-бэкенд привязывается к
  штатным контролам (миникарта, прицел, лог урона, лампочка).## PASS, FAIL, SKIP и crash recovery

PASS нажимайте только после совпадения визуального действия, event trace,
счётчика, payload и потока. PASS не превращает отсутствующий или намеренно
отключённый backend в `SUPPORTED`. Для FAIL сохраните короткий комментарий в
`FAIL_COMMENT.txt`, затем нажмите `F8`. `F10` сбрасывает только выбранный тест.

Перед экспериментальным native-вызовом создаётся `ACTIVE_NATIVE_TEST.json`.
После нормального возврата marker удаляется. Если клиент упал, следующий запуск
показывает test ID, capability, binding, state, thread, owner и last action и
автоматически блокирует этот тест. Marker указывает последнюю активную
операцию, но сам по себе не доказывает причинность. Повторное включение — `F12`.

## Скриптовый прогон без клавиш (2026-09-04)

Панель опрашивает клавиши раз в отрисованный кадр, и в ангаре короткое
нажатие из `keybd_event` теряется. Для автоматических прогонов клиент
наследует переменные окружения runner-а:

- `WOTBMOD_VALIDATION_AUTOSWEEP_SECONDS=45` — показать панель при загрузке и
  через 45 с выполнить прогон всех безопасных тестов (клавиша `G`);
- `WOTBMOD_VALIDATION_AUTOSWEEP_BATTLE_SECONDS=20` — показать панель и выполнить
  прогон через 20 с после первого входа клиента в BATTLE (так снимается
  `LIVE_UI_TREE.jsonl` боевого HUD с именами контролов);
- `WOTBMOD_VALIDATION_AUTOPROBE_SECONDS=70` — через 70 с выполнить пробу
  хуков (клавиша `H`);
- `WOTBMOD_VALIDATION_ONLY=reload,ges` / `WOTBMOD_VALIDATION_SKIP=reload` —
  сузить прогон по префиксам id строк.

Взводится один раз на процесс (маркер `WOTBMOD_VALIDATION_AUTO_ARMED`), чтобы
hot reload не запускал прогон заново. Учтите: reload пересоздаёт мод и
`LIVE_CAPABILITY_MATRIX.json` пишется заново — полную матрицу снимайте в
прогоне со `SKIP=reload`, а reload проверяйте отдельным `ONLY=reload`.
Во время прогона строки `reload.*` ставят запрос reload в ту же очередь
главного потока, что и обход UI/инспекторы, — иначе выгрузка модуля под
ещё идущими задачами оставляла клиент с чёрным экраном (run 6, 2026-09-04).

## Экспорт отчёта

После чистого закрытия клиента выполните:

```powershell
powershell -ExecutionPolicy Bypass -File tools\export_live_validation_bundle.ps1 `
  -DataDirectory "<путь>\mods\data\wotbmod.native_validation" `
  -OutputPath "build\wotbmod-native-validation-bundle.zip"
```

Скрипт проверяет совпадение `fingerprint_key` matrix/results и включает trace,
results, matrix, binding-pack validation, mod/permission inventory, failures,
FAIL comment, auto-disable и crash marker, если он остался. Logs sanitization
удаляет tokens, e-mail, chat и full RPC payload. До экспорта не редактируйте
JSONL вручную. Тот же экспорт запускается кнопкой `Export Validation Bundle`
или клавишей `F4` из validation-мода.
