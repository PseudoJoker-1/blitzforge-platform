# RC1 verification report — WoT Blitz Mod API V3

## Goal and success criteria

- Goal: динамически проверить каждую публичную функцию
  `WotbModHostApi`, `wotb_mod_runtime.h`, optional exports и lifecycle
  callbacks.
- Stop condition: свежие `build.cmd` и `loader\build_live.cmd` с exit code `0`, без незакрытых
  hooks/mounts/resources/audio playbacks и с нулевым балансом native
  allocations.
- Safety bounds: локальные build/test environments и отдельный контролируемый
  запуск живого клиента; без патча game executable и без изменения игровых
  ресурсов.

## Public preview acceptance — 2026-08-14

Первый public preview принят только для exact Windows x86 клиента
`11.19.0.834` с executable SHA-256
`41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`.
Это developer/public preview, не stable-релиз.

Финальный `build.cmd` был запущен из отсутствующего точного каталога
`mod_api\build`; отдельный пользовательский `_mod_tools\build` не удалялся.
Прогон завершился с exit code `0` и финальным banner PASS. Ключевые результаты:

| Проверка | Свежий результат |
| --- | ---: |
| V3 core / C++ wrapper | `105/0`, `128/0` |
| Package loader / gameplay / data truth | `70/0`, `68/0`, `335/335` |
| Native UI / public UI | `206/0`, `85/0` |
| Lua host / conversion | `759/0`, `103/0` |
| Full public API | `1661 assertions`, `0 failures` |
| Installer lifecycle | `27 assertions`, `0 failures` |

Первый live-запуск новой package revision обнаружил настоящий Windows race:
антивирусный/file-indexing lock мог временно запретить rename распакованного
archive staging directory. Публикация теперь повторяет только transient
`ACCESS_DENIED`/`SHARING_VIOLATION`/`LOCK_VIOLATION`; regression удерживает
временный каталог реальным Win32 handle и доказывает успешный retry. Имена
каталогов Lua-примеров в release bundle теперь берутся из `manifest.id`, поэтому
копирование создаёт `mods\lua\example.lua_*`, а не отклоняемые дубликаты.

Финальный signed bundle:

- ZIP SHA-256: `babd521d76a6875861bd1b9dad53fdd8d61207be1a3a1bdea201b6f988572ca0`;
- Lua package SHA-256: `e3256d35b3f454d34c8c2c78ab2e703c87136ae04af1f42a4c2bf148014f6ea6`;
- loader SHA-256: `6c11e8b496f20a5c9f977f131732ebda2d408d041a0785502cc99d8d91b31de7`;
- proxy SHA-256: `094a6b03548916fd51a6bd1204e8998e077aa80548a6ac36be0a198dbdb456f6`.

В реальном клиенте package preflight сообщил `ready=1`, `blocked=0`, detached
signature имела `signature-status=2`, `unsigned=no`, `untrusted-signer=no`,
key `blitzforge-preview-2026`. Host загрузился как
`wotbmod.lua_host@0.1.0-preview.1`; итог загрузки — `package-loaded=1`,
`failed=0`. Все четыре Lua-примера были приняты без `manifest refused`.

В replay/battle одновременно и стабильно отображались Battle Telemetry и шесть
строк Ally Tracker; native provider подтвердил семь публичных машин своей
команды с учётом local player. Два кадра через пять секунд сохранили обе панели.
F8 открыл полный Lua Runtime UI Lab; лог сообщил
`window=true cursor_unlocked=true`, независимый `GetCursorInfo` вернул
`CURSOR_FLAGS=1`, процесс оставался responding. После окончания battle панели
исчезли на экране replay list, то есть battle-only lifecycle соблюдён.

Новый loader-срез содержит 0 строк `error/fatal`, safe-mode, package block,
manifest refusal, instruction-budget fault, script disable или nonzero load
failure. Клиент закрыт через `WM_CLOSE`; `CrashDumps` остался `10 -> 10`, а
`runtime_session.marker` исчез после завершения shutdown cleanup (проверка с
ожиданием до восьми секунд). Evidence находится в `build/live_evidence`.

## Functional battle-only Lua mods — 2026-08-12

Вместо синтетического окна `PASS/FAIL/SKIP` добавлены и включены в Lua-host
package два прикладных battle-only мода:

- `example.lua_ally_tracker` показывает публичные атрибуты союзной машины и
  удерживает последний подтверждённый публичный snapshot после исчезновения
  сущности, помечая строку `ПОСЛЕДНЕЕ`;
- `example.lua_battle_telemetry` превращает типизированные боевые события в
  компактную live-ленту урона, уничтожения, смены локальной машины и входа/
  выхода из sniper mode.

Текущий `example.lua_ui_framework` также переведён в battle/training/replay
режим. Он полностью снимает свои controls вне разрешённого экрана и при
`MOD_SCREEN`/`TEXT_INPUT`, а safe-left `128 px` оставляет свободной область
кнопки каталога модов. Lua-only `wotb.context` построен поверх frozen
`core.get_context`; frozen ABI headers не менялись.

Ally Tracker не подделывает координаты или скрытые сведения. Live provider
пока не подтверждает world-position/team/name для произвольной машины, поэтому
позиция честно отображается как недоступная. Мод сохраняет только последний
публично полученный snapshot; hidden enemy state не запрашивается и не
накапливается.

Fresh focused evidence:

| Проверка | Результат |
| --- | ---: |
| Lua host / Lua conversion | `744/0`, `103/0` |
| Lua API model/static/package | `12/12` |
| Release Lua DLL export gate | `PASS` |
| Independent re-review | `APPROVE`, 0 blocking findings |

Discriminating proof: ожидаемая строка telemetry была временно изменена с
`УРОН 250` на `УРОН 251`. Suite упал ровно на функциональной проверке
Battle Telemetry с `743 passed, 1 failed`; после восстановления ожидания
результат вернулся к `744 passed, 0 failed`.

Первое независимое ревью обнаружило три реальных дефекта доказательной среды:
reentrant UI-event dispatch в mock ABI, недостаточную проверку lifecycle style
handles и беззвучное подавление ошибок встроенных Lua-библиотек. Dispatch
переведён на snapshot, stale/wrong style handles теперь доходят до ABI и
отклоняются, а ошибки библиотек логируются вместе с chunk name. Повторное
read-only ревью вернуло `APPROVE` без новых blocking findings.

Финальный полный прогон начат после проверки и удаления только точного
`mod_api\\build`; пользовательский `_mod_tools\\build` не затрагивался.
Чистый `build.cmd` завершился за `467.4 s`, exit `0`, с баннером
`=== Mod API build, smoke, and full contract tests passed ===`. Среди свежих
результатов: Lua `744/0`, conversion `103/0`, V3 core `105/0`, C++ wrapper
`128/0`, package `68/0`, safe gameplay `64/0`, native UI `191/0`, public UI
`85/0` и full API `1655/1655`.

Установленный Lua package и все три функциональных примера byte-identical
clean distribution. Package SHA-256:
`437D8A8E97276510BCF1D563AAE4D2823180396861632CDA3805D8AF47273B8A`;
loader SHA-256:
`FA735092BFD960BB1DED871C2FCD91C0C98448DD3E670ADC91944AA9F676E8E1`.
`wotbmod.py doctor` подтвердил production client `11.19.0.834` x86, его exact
fingerprint, proxy, forwarder, runtime library и установленный loader.

Direct production smoke загрузил именно установленный loader и staged
`wotbmod_lua_host.dll`; процесс оставался responding и завершился через
`WM_CLOSE`. Fresh DBWIN capture не содержал safe-mode, Lua error/fault/
exception, instruction-limit или nonzero package/load failure строк. Число
crash files не изменилось (`1 -> 1`), session marker после shutdown отсутствовал.
В бой во время этого smoke не входили, поэтому live evidence подтверждает
loadability и clean shutdown, а не фактическую отрисовку/события боя.

## Lua typed gameplay/events addendum — 2026-08-12

Lua event subscriptions now preserve the byte-exact `event.payload` and also
decode validated native payloads into `event.data`. Decoding is gated by the
exact topic, payload type, payload version and payload size; malformed or
truncated payloads leave `event.data == nil`. The typed surface covers all 22
currently published normalized client-event envelopes plus public-entity,
projectile, local-shell and metadata-only RPC lifecycle topics.

Five useful events are derived only from already confirmed publishers:
`local_vehicle_created`, `local_vehicle_destroyed`, `damage_received`,
`sniper_entered` and `sniper_exited`. `damage_dealt` was deliberately not
invented because the current source does not prove attacker attribution.

The trusted convenience module `wotb.players` exposes `snapshot`,
`local_player`, `our_team`, `enemy_team`, `unknown_team`, `visible` and `find`.
It is built exclusively over the public-entity enumeration boundary. Hidden
enemy entities are never synthesized or retained. The live provider currently
proves public id/handle, type, visibility, local-player status, health,
max-health and public type. Team, display name, position and direction remain
explicitly unavailable until a separately validated native source exists, so
non-local vehicles are placed in `unknown_team` instead of being guessed.

Fresh focused evidence:

| Проверка | Результат |
| --- | ---: |
| Lua API model/static safety | `12/12` |
| Safe gameplay bridge | `64/64` |
| V3 runtime services | `PASS` |
| Full API ingress contract | `1655/1655` |
| Lua host / Lua conversion | `719/719`, `103/103` |
| Release Lua DLL export gate | `PASS` |

Discriminating proof: the typed-damage Lua assertion was intentionally changed
from `275` to `274`. The suite failed at `typed event probe: probe:5: damage`
with `715 passed, 1 failed`. Restoring the assertion produced `716 passed,
0 failed`. This proves the new event-data check can detect a wrong decoder
result rather than merely executing an infallible path.

Independent review then found that the runtime could derive local-player and
camera topics from a short legacy payload before the Lua decoder rejected the
result. The ingress now rejects every event whose payload is smaller than its
event-specific struct, and the derived fan-out repeats that guard before
changing local-player state or publishing. Four runtime assertions cover short
local-change, damage, destroy and camera inputs. Lua regressions separately
cover an outer truncation, unknown envelope version, topic/type mismatch and a
short nested payload inside a full envelope. Fresh post-fix results are
`API FULL OK: assertions=1655`, `Lua host: 719 passed, 0 failed`, runtime
services `PASS`, and safe gameplay bridge `64/64`.

Final proof was run from an absent `mod_api\\build` directory. The exact path
was validated under `mod_api`, removed with `EXISTS_AFTER=False`, and the clean
`build.cmd` completed in `511.2 s` with exit `0` and the terminal banner
`=== Mod API build, smoke, and full contract tests passed ===`. The clean run
also reported `API FULL OK: assertions=1655`, Lua `719/0`, conversion `103/0`,
runtime services `PASS` and gameplay bridge `64/0`.

`loader\\build_live.cmd` then exited `0`. The installed production loader and
Lua-host package were byte-identical to the clean artifacts:

- loader SHA-256: `5C19DCEAD974221E92E842D4F139503085BA9C4F12DB11D0E39E642D39A7D3EC`;
- Lua package SHA-256: `A53AC21BA62EE82168A492075FD9970CE4BA82730AB550A3652A7AF7EEF48DCE`.

A direct proxy-only production launch (no test-launcher double injection)
loaded `wotbmod.lua_host` with `package-loaded=2`, total `loaded=3`, `failed=0`.
The fresh log slice had zero safe-mode lines, zero Lua error/fault/exception
lines and zero nonzero load failures. The client remained responding, closed
through `WM_CLOSE`, created no new crash dump (`10` before and after), and
removed its session marker on shutdown. Battle-only event stimuli were not
performed in this smoke run.

## Current verified snapshot — 2026-08-02

Это текущий источник истины для результата этой итерации. Исторические live
логи ниже сохранены как regression evidence, но в этой итерации клиент не
запускался: процесс игры отсутствовал.

| Проверка | Свежий результат |
| --- | ---: |
| Полный `build.cmd` | exit `0` |
| V3 runtime services | `PASS` |
| V3 core / C++ wrapper / package / gameplay | `105/105`, `128/128`, `68/68`, `63/63` |
| DVPL / data truth / interface truth | `17/17`, `272/272`, `72/72` |
| Tooling / named permissions / data named permissions | `269/269`, `155/155`, `90/90` |
| Operation permissions / truthful client slices | `327/327`, `365/365` |
| WinHTTP transport | `106/106`, completion на `RENDER` ingress |
| Native UI / public UI / managed renderer | `49/49`, `66/66`, `32/32` |
| CLI | `17/17` |
| Полный API 2.9 contract | `1651 assertions` |
| Предыдущий полный API stress | `10/10`, по `1650 assertions` |
| Client subscription/backend stress | `30/30` |
| Publish self-test | `92 PASS / 10 SKIP / 0 FAIL` |
| Audio self-test | `24 PASS / 0 FAIL` |
| New events / vehicle skin mods | `14/14`, `15/15` |
| Native validation / ABI freeze / RC1 samples | `194/194`, `46/46`, `62/62` |
| Validation bundle privacy/export | `29/29` |
| Crash-loop safe mode recovery | `84/84` |
| `loader\build_live.cmd` | exit `0`; isolated validation package обновлён |

Runtime-services regression подтверждает честные границы: loader передаёт
runtime маску только реально установленных UI/Scene/gameplay ingress;
system-event subscription при нулевой или неподходящей частичной маске
возвращает `E_NOT_SUPPORTED`;
автоматический host ingress есть для `RENDER`, а запросы `MAIN`/`AUDIO`
dispatch или completion без соответствующего ingress не возвращают ложный
успех; неподдерживаемые hook priority/ordering/chain-next операции также
возвращают `E_NOT_SUPPORTED`.

Crash-loop harness создаёт marker до `LoadAll`, затем запускает child process,
в котором `safe_mode_crash_mod.dll` вызывает `TerminateProcess` из `DllMain`
во время `LoadLibrary`. Child завершается с code `73`, не выполняет CRT
cleanup и оставляет `last_phase=loading_native_mod` /
`last_mod=safe_mode_crash_mod`. Следующая инициализация сохраняет V3 bootstrap/host
diagnostics, но загружает `0` DLL и возвращает `DISABLED` для explicit enable.
Clean safe-mode shutdown удаляет session marker, но сохраняет отдельную
`crash_history.ini` как forensic counter. Явный `WOTBMOD_SAFE_MODE_OVERRIDE=1`
создаёт marker новой сессии и разрешает portable-only recovery. Повторный crash
того же владельца доводит count до `2`, после чего runtime автоматически
disable-ит виновника; clean shutdown не оставляет session marker или env
override.

`python tools\wotbmod.py doctor` подтвердил точное совпадение клиента
WoT Blitz `11.19.0.834` x86 и SHA-256
`41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD`.
Установленный proxy найден и валиден; отдельный
`wotb_mod_loader.dll` в корне игры отсутствует и не выдаётся за найденный.

## Scenario matrix

Полная построчная матрица:
[`API_TEST_MATRIX.md`](API_TEST_MATRIX.md).

Итог свежего локального прогона: host-части API-001…API-042 и QA-001…QA-004
имеют контрактный статус `PASS`. Исторический live initialization API-026…API-029
сохранён ниже отдельно. Свежего live-прогона в этой итерации не было, поэтому
battle-only event stimuli и видимая stock-tank подмена всё ещё требуют полевой
проверки.

## V3 focused regression evidence

Эта таблица фиксирует фактическую поддержку текущей V3-реализации. Наличие
function slot без рабочего backend не считается `PASS`.

| Проверка | Результат | Что подтверждено |
| --- | ---: | --- |
| V3 core runtime | `105/105` | handles, ownership, permissions, contexts, lifecycle и cleanup |
| Header-only C++17 wrapper | `128/128` | typed interface query, exact error propagation и RAII ownership для adopt/retain/copy/move/clone/close, включая foreign и stale handles |
| Safe gameplay bridge | `63/63` | local-or-visible public entities, metadata-only incoming RPC, exact local projectile owner gate и atomic binding-validation report |
| Portable DVPL decoder | `17/17` | footer/size/CRC, type `0`, type `1` LZ4, type `2` LZ4HC и честный `NOT_SUPPORTED` для type `4` |
| Data truth regression | `272/272` | shared-worker resource lifecycle, atomic reload rollback, WndProc/DAVA input queue/action/poll/capture/coalescing и safe missing-path resolution |
| Public UI bridge | `66/66` | UI V3 active tree/snapshot, permission boundary, managed layout geometry/rollback и native input event delivery; unresolved semantic slots remain explicit |
| Native UIControl bridge | `49/49` | active screen/find/parent/children/geometry/state и native object cleanup без утечек |
| Managed D3D11 renderer | `32/32` | portable managed renderer и реальный D3D11 WARP draw path |
| Package runtime integration | `OK` | preflight, package mount/lifecycle и content-only UI overlay resolution с disable/unapply и enable/remount; production consumer — guarded `DAVA::File::Create` detour |
| Полный API 2.9 contract | `1651 assertions` | существующая публичная API 2.9 regression matrix без ошибок |

Подтверждённые ограничения:

- DVPL type `4` streaming LZ4 требует guarded native stream backend.
- Semantic content-only overrides `AUDIO`, `HANGAR` и `LOCALIZATION`
  возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`. `UI`, `TEXTURE` и `MODEL`
  транзакционно монтируются через VFS и потребляются guarded
  `DAVA::File::Create` detour.
- UI V3 inspection, runtime native text/image/button/input/scroll creation,
  text/texture/font/color/opacity/background mutation, independent clone,
  managed layout/style ownership и pointer/click/drag/scroll events работают.
  Raw DAVA class/component injection и native animation components не
  симулируются.
- Projectile V2 lifecycle и managed DAVA Scene impact visuals работают;
  stock tracer style/object ownership остаётся `E_NOT_SUPPORTED`.
- Vehicle Visual V2 подменяет exact mesh/material/texture paths и LOD с
  rollback; already-cached appearance требует model reload.
- Tracer styling/attachment остаётся `WOTBMOD_V3_E_NOT_SUPPORTED`.
- Public entity/RPC/projectile bridge не раскрывает hidden enemy state,
  RPC payload и произвольные send/modify/drop/replay операции.
- Shipping loader отключает legacy raw process/RVA/export/pattern API и raw
  hooks; runtime opt-in без developer-unsafe сборки их не активирует.

### V3 focused addendum — named grants и truthful native slices

Этот более поздний focused-прогон дополняет, но не переписывает исторический
snapshot таблицы выше.

| Проверка | Свежий результат | Что подтверждено |
| --- | ---: | --- |
| Runtime services truth | `PASS` | zero/partial/full event-source masks, publisher availability gate, только реально подключённый `RENDER` dispatch ingress и честный `E_NOT_SUPPORTED` для неподдерживаемого hook ordering/call-next |
| Named permission runtime | `155/155` | package allowlist и tier проверяются независимо; canonical permission definitions, parent namespace grants, case-insensitive package IDs и exact network-host policy не дают broad interface access превратиться в operation success |
| Operation permission runtime | `327/327` | operation grants плюс transactional vehicle skin V2: 3 assets/LOD, idempotent apply, rollback, failed preflight и release cleanup |
| WinHTTP transport | `106/106` | HTTPS:443 only, exact REVIEWED host grant, public DNS/IP pinning и remote-endpoint verification, TLS validation, response/time limits, cancel/unload и lifetime-safe completion через гарантированный `RENDER` ingress |
| Granular data/VFS permissions/content truth | `90/90` | `resources.mod` разрешает mod namespace, но не game overlay; `resources.overlay.game` и tier `REVIEWED` обязательны; UI/texture/model реально apply/resolve/unapply, а audio/hangar/localization дают `NOT_SUPPORTED` |
| Interface capability truth | `72/72` | capability различает exact-path vehicle skin support, cached hot-swap gap и неподдержанные semantic kinds |
| Package loadability/trust truth | `68/68` | real ECDSA P-256 signature, trusted/unknown/tampered signer policy; signed revocation list для key/release, downgrade rejection; `PACKAGE_PLAN_READY` остаётся loadability |
| Native camera layout | `5/5` | near plane `+0x20`, far plane `+0x24`; layout selector не возвращает прежний неверный offset |
| Truthful public entity/client slices | `365/365` | public entity/device/RPC truth; subscription unregister/destruction barriers, backend generation isolation, reentrant frame-pump replacement и native-release retry cleanup |
| Safe gameplay bridge | `63/63` | только incoming RPC metadata; projectile ingress и binding validation fail-closed; hidden state не раскрывается |
| Portable devtools/callback profiler | `269/269` | owner-scoped inspectors плюс budget `100..8000 us`, callbacks/frame, CPU counters, coalescing и reset/cleanup |

Отдельно зафиксированы текущие native-границы:

- `camera.get_mode` получает native `ARCADE/SNIPER` и context-derived
  `HANGAR/REPLAY`; `POSTMORTEM/FREE/CINEMATIC` не угадываются.
- RPC payload/send/modify/drop/replay и outgoing observation недоступны.
- До подключения отдельного host ingress async dispatch/completion на `MAIN`
  и `AUDIO` возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`; HTTP completion
  доставляется через подключённый `RENDER` ingress.
- Hook priority, `run_before`, `run_after` и `call_next` не симулируют
  отсутствующую native chain и возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`.
- System-event subscription без реального native publisher отклоняется с
  `WOTBMOD_V3_E_NOT_SUPPORTED`; frame и mod-owned topics остаются доступны.
- Public shell ID/type и stock visible-tracer style/object lifecycle недоступны;
  Projectile V2 shot/impact lifecycle и managed impact visuals доступны.
- Generic minimal YAML, DVPL `0/1/2` и directory archive имеют реальные
  backends; DAVA YAML/archive ABI возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`.
- `ContentApply` для `TEXTURE`, `UI` и `MODEL` создаёт transactional VFS
  overlays; `ResolveActiveGameOverlayPath` вызывается из штатного resource
  resolver, а loader ставит guarded `DAVA::File::Create` detour.
  `AUDIO`, `HANGAR` и `LOCALIZATION` остаются `NOT_SUPPORTED`.
- `PACKAGE_PLAN_READY` не является результатом trust: signature verdict
  хранится отдельно; strict policy требует trusted ECDSA P-256 signer.
- Generic device info через Win32/DXGI и штатный LeaveToHangar имеют реальные
  backends.
- Lua и WASM runtimes в V3 не реализованы.
- Package named permissions ограничивают WotbMod API, но не являются sandbox
  для `DllMain` или direct Win32-кода нативной DLL. Loose V3 DLL без manifest
  остаётся tier-only compatibility mode.

## Addendum 2026-08-16 — private DAVA native registry

Добавлен loader-private ABI v1, не входящий в замороженные public V3 headers.
Он поддерживает typed capability-группы YAML, ResourceArchive, NMaterial,
loaded-mesh hot-swap, stock tracer и reviewed class factory. Provider tokens
оборачиваются в owner/kind-checked host tokens; replacement/removal ждёт
завершения callbacks, а failed release сохраняет token для retry.

Свежие focused результаты и финальный clean build:

| Suite | Результат | Что доказано |
|---|---:|---|
| `build_v3_dava_native_registry_tests.cmd` | `87/87` | validation, six capability groups, globally unique host tokens, cross-owner/stale rejection, release retry, replacement/removal quiescence and forced rehash during in-flight release |
| `build_v3_data_truth_tests.cmd` | `368/368` | native YAML/archive provider doubles, bounded portable snapshots, CRC and case-folded file/child rejection, immediate native release, snapshot usable after provider removal |
| `build_v3_vfs_mount_fault_tests.cmd` | `19/19` | прямой compile path `data_services.cpp` включает новый registry без потери fault semantics |
| `build.cmd` из отсутствующего `mod_api\build` | exit `0`, final banner PASS | весь build/smoke/contract pipeline; package loader `71/71`; RC1 `56` files, `45` interfaces, `51` permissions; fixture inputs присутствуют в clean checkout |

Exact 11.19 provider выставляет только reviewed class factory для
`DAVA::UIControl` и `DAVA::Entity`. YAML/ResourceArchive, NMaterial, loaded mesh
и stock tracer capability не выставлены и остаются `E_NOT_SUPPORTED`, потому
что одного RVA недостаточно для ownership/destruction/thread/exception ABI.
Точный финальный banner: `=== Mod API build, smoke, and full contract tests passed ===`.

Live-проверка выполнена на реальном клиенте `11.19.0.834`, SHA-256
`41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD`.
Opt-in loader probe (`WOTBMOD_DAVA_LIVE_PROBE=1`) вызвал private registry на
кадре клиента, а не только прочитал capability mask:

- `DAVA::UIControl`: registered `1`, create `0`, release `0`, host token `1`;
- `DAVA::Entity`: registered `1`, create `0`, release `0`, host token `2`;
- `DAVA::NMaterial` через class factory: registered `0`, create `3`;
- native YAML, ResourceArchive, NMaterial mutation, loaded-mesh hot-swap и
  stock tracer: direct result `3` (`WOTBMOD_V3_E_NOT_SUPPORTED`);
- установленный mask: `0x20`, то есть только reviewed `CLASS_FACTORY`.

Итоговая строка клиента:

```text
DAVA PRIVATE LIVE STATUS class-factory=PASS arbitrary-object-factory=NOT_SUPPORTED native-yaml=NOT_SUPPORTED resource-archive=NOT_SUPPORTED nmaterial=NOT_SUPPORTED mesh-hot-swap=NOT_SUPPORTED stock-tracer=NOT_SUPPORTED
```

Три независимых harness-прогона получили `VERDICT: CLEAN`, без новых crash
dump (`10 -> 10`), без stale session marker и без bad-line совпадений:

- `%LOCALAPPDATA%\wotbmod\live_evidence\20260816-125032-90bbc79c`;
- `%LOCALAPPDATA%\wotbmod\live_evidence\20260816-125228-d5c5e7fc`;
- `%LOCALAPPDATA%\wotbmod\live_evidence\20260816-130116-a86849a5` — финальный
  artifact SHA-256
  `95496AAE1C857AEE6DFC9C7C4A44D05BCBD448C787C8A8BFC9D975B465E12D1D`.

Второй процесс оставался стабильным ещё три минуты после probe и визуально
дошёл до игрового promo-overlay. Строгий readiness `V3 context changed to
HANGAR` не наступил из-за этого модального экрана, поэтому переход в сам ангар
в этом прогоне — `SKIP`, не PASS. Reviewed UIControl/Entity route повышен до
`LIVE_PASS`; пять отсутствующих exact adapters и произвольный ObjectFactory
остаются `NOT_SUPPORTED`. Их result `3` доказывает честный отказ, а не работу
реального native YAML/archive/material/mesh/tracer backend-а.

## Commands run

- `[0] build\v3_core_runtime_tests.exe build\v3_core_env`:
  `V3 CORE OK: assertions=105 failures=0`.
- `[0] tests\build_v3_cpp_wrapper_tests.cmd`:
  `V3 C++ WRAPPER: 128 checks, 0 failures`; C++17 `/W4 /WX /permissive-`.
- `[0] build\v3_gameplay_bridge_tests.exe`:
  `V3 safe gameplay bridge: 63 passed, 0 failed`.
- `[0] build\v3_dvpl_decoder_tests.exe`:
  `V3 DVPL decoder: 17 passed, 0 failed`.
- `[0] build\v3_data_truth_tests.exe`:
  `v3 data truth tests passed: 272/272`.
- `[0] build\v3_ui_public_tests\test.exe`:
  `v3 public UI bridge checks: 66, failures: 0`.
- `[0] build\v3_native_ui_bridge_tests.exe`:
  `V3 native UI bridge: 49 passed, 0 failed`.
- `[0] build\v3_managed_renderer_tests.exe`:
  `V3 Managed Renderer: 32 passed, 0 failed`.
- `[0] build\v3_package_runtime_tests.exe ...`:
  `V3 package runtime integration: OK`.
- `[0] build\v3_runtime_services_tests.exe`:
  `V3 RUNTIME SERVICES OK`.
- `[0] tests\build_v3_http_transport_tests.cmd`:
  `V3 HTTP transport: 106 passed, 0 failed`; completion проверена на
  `RENDER` role.
- `[0] tests\build_v3_tooling_v2_tests.cmd`:
  `V3 TOOLING V2: 269 checks, 0 failures`.
- `[0] tests\build_v3_named_permissions_tests.cmd`:
  `V3 named permissions: 155 checks, 0 failures`.
- `[0] tests\build_v3_operation_permissions_tests.cmd`:
  `V3 operation permissions: 327 checks, 0 failures`.
- `[0] tests\build_v3_client_truthful_slices_tests.cmd`:
  `V3 truthful client slices: 365 checks, 0 failures`.
- `[0] build\v3_client_truthful_tests\test.exe` × 30:
  `client subscription/backend stress: 30/30 passed`.
- `[0] tests\build_v3_package_loader_truth_tests.cmd`:
  `V3 PACKAGE: checks=68 failures=0`.
- `[0] tests\build_v3_interface_truth_tests.cmd`:
  `V3 interface truth tests: 72 passed, 0 failed`.
- `[0] tests\build_v3_data_named_permission_tests.cmd`:
  `V3 data named permissions: 90 checks, 0 failures`.
- `[0] build\safe_mode_recovery_tests.exe build\safe_mode_env`:
  `SAFE MODE RECOVERY OK: checks=84`; crash child exit `73`, stale-marker
  blocking, forensic phase/mod, portable-only recovery, repeated crash count
  `2`, auto-disable и clean session-marker recovery подтверждены.
- `[0] build.cmd` — свежий ABI 2.9 run:
  `API FULL OK: assertions=1651`,
  `API SELFTEST: passes=92 skips=10 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] build\api_full_host.exe ...` × 10:
  предыдущий `10/10` прогон выполнялся по `1650 assertions`; текущий
  `1651` contract подтверждён одним чистым полным запуском.
- `[0] loader\build_live.cmd` — свежий успешный ABI 2.9/RC1 build; DAVA bridge, все event
  detours и stock-resource hook с проверкой prologue успешно скомпилированы;
  legacy `WotbModVec3` из hit payload явно копируется в `WotbModV3Vec3`
  перед Projectile V2 ingress, без aliasing между двумя ABI-типами.
- `[0] python tools\wotbmod.py doctor` — exact client build/arch/SHA match;
  installed proxy `OK`, root `wotb_mod_loader.dll` отсутствует.

### Historical live evidence — not rerun in this iteration

- `[0] build\live_test_launcher.exe` — WoT Blitz `11.19.0.834` x86,
  SHA-256
  `41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD`:
  gameplay hooks `mask=0xFFF (12/12)`,
  `API SELFTEST: passes=108 skips=2 failures=0`,
  `custom-audio-test: passes=24 failures=0`,
  `NATIVE LIVE TEST: passes=142 skips=0 failures=0`,
  Wwise `1931`, UI `206`, Scene loader `38`,
  UI events `7`, Scene activated `2`, Scene deactivated `1`.
- `[0] build.cmd` — два focused test mods:
  `NEW EVENTS MOD: passes=14 failures=0 seen=0x003E1008`,
  `VEHICLE SKIN MOD: passes=15 failures=0`,
  `NEW API MODS OK`.

- `[0] build.cmd` — baseline smoke после добавления harness.
- `[1] build.cmd` — первый adversarial run; обнаружены четыре группы
  runtime-дефектов.
- `[0] build.cmd` — полный прогон после исправлений.
- `[0] build.cmd` — первый полный Audio API run:
  `API FULL OK: assertions=1003`.
- `[1] build.cmd` — regression harness обнаружил оставшееся ожидание metadata
  fixture `2.1.0` после обновления contract-мода до `2.2.0`.
- `[0] build.cmd` — полный прогон после синхронизации fixture.
- `[0] build.cmd` — self-review regression для удержания audio clip после
  faulted playback release.
- `[1] build.cmd` — первый ABI 2.3 custom-audio run обнаружил два test/contract
  расхождения: старую ожидаемую metadata version и разрешённый resume после
  окончательного stop.
- `[0] build.cmd` — ABI 2.3 run после исправлений:
  `API FULL OK: assertions=1059`.
- `[0] build.cmd` × 2 — финальные последовательные flake-check после
  exception/overflow-boundary и legacy-struct regression с одинаковыми
  `API FULL OK: assertions=1059`.
- `[0] build.cmd` — publishable self-test run:
  `API SELFTEST: passes=35 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] build.cmd` — финальный ABI 2.4 contract run:
  `API FULL OK: assertions=1274`,
  `API SELFTEST: passes=47 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] loader\build_live.cmd` и первый запуск Blitz — реальный
  `DAVA::SoundSystemProxy`, native event lifecycle и игровые hook entry points:
  `API SELFTEST: passes=47 skips=0 failures=0`,
  `NATIVE LIVE TEST: passes=76 skips=2 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] loader\build_live.cmd` и финальный запуск Blitz после подключения
  object bridge — настоящие `UIPackage`, named `UIControl` и `Scene`,
  load/reload/release и native UI/Scene hooks:
  `API SELFTEST: passes=47 skips=0 failures=0`,
  `NATIVE LIVE TEST: passes=77 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] build.cmd` — ABI 2.5 object/main-thread contract:
  `API FULL OK: assertions=1404`,
  `API SELFTEST: passes=57 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] loader\build_live.cmd` и запуск Blitz с ABI 2.5 — реальные
  UI geometry, visibility, add/remove child,
  `TransformComponent::SetLocalTransform` и Scene add/remove child:
  `API SELFTEST: passes=57 skips=0 failures=0`,
  `NATIVE LIVE TEST: passes=96 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`,
  UI hook `6`, Scene hook `4`. Первый run также обнаружил и после этого
  исправил backend-dependent код ошибки invalid UI handle в standalone
  self-test; финальный run после исправления полностью прошёл.
- `[0] build.cmd` — ABI 2.6 create/active-root contract:
  `API FULL OK: assertions=1431`,
  `API SELFTEST: passes=69 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] loader\build_live.cmd` и запуск Blitz с ABI 2.6 — реальные
  `UIControl`/`Entity` constructors, active `UIControlSystem` screen,
  Scene Activate/Deactivate/Draw tracker и object ownership:
  `API SELFTEST: passes=67 skips=1 failures=0`,
  `NATIVE LIVE TEST: passes=108 skips=2 failures=0`,
  UI hook `55`, Scene load hook `4`. В раннем окне клиента активная 3D Scene
  отсутствовала, поэтому её getter корректно завершился `NOT_FOUND`/`SKIP`.
- `[0] loader\build_live.cmd` и продолжительный запуск Blitz до ангара —
  positive active-Scene/custom-model lifecycle: `scene_get_active`,
  transform, attach loose `.sc2` и созданного `Entity`, 180 стабильных
  кадров, detach/release/unmount:
  `NATIVE LIVE TEST: passes=119 skips=0 failures=0`,
  Wwise hook `1830`, UI hook `191`, Scene loader hook `37`.
- `[0] build.cmd` и отдельные direct executable runs — ABI 2.7
  client-event/resource-clone contract:
  `API FULL OK: assertions=1537`,
  `API SELFTEST: passes=74 skips=1 failures=0`,
  `AUDIO SELFTEST: passes=24 failures=0 finished=1`.
- `[0] loader\build_live.cmd` и финальный запуск Blitz с ABI 2.7 —
  UI-screen/Scene events, borrowed handles и clone на настоящих DAVA objects:
  `NATIVE LIVE TEST: passes=134 skips=0 failures=0`,
  UI events `7`, Scene activated `2`, Scene deactivated `1`,
  Wwise hook `1930`, UI hook `191`, Scene loader hook `38`.

Success определялся по exit code, assertions, live log и отсутствию crash/hang,
а не только по строкам `OK`.

## Failures found

1. `find_pattern` принимал любой символ mask кроме `x` как wildcard.
2. `config_get_string` возвращал `BUFFER_TOO_SMALL`, когда строка точно
   помещалась в buffer.
3. Hook slots не переиспользовались после `remove`, поэтому 64
   create/remove cycles навсегда исчерпывали лимит.
4. Ошибка автоматического hook cleanup скрывалась от caller, а ownership
   терялся даже при неуспешном backend `remove`.
5. Resource handle был адресом переиспользуемого slot: stale handle мог
   освободить новый ресурс.
6. `resource_release` инвалидировал handle до успешного backend release,
   исключая retry и оставляя native allocation.
7. Проверка существующей директории принимала обычный файл за directory на
   промежуточном шаге инициализации.
8. ABI 2.5 object API проверял наличие backend callback раньше public handle,
   поэтому один и тот же invalid handle давал `PLATFORM` без backend и
   `INVALID_ARGUMENT` с backend.
9. Первый ABI 2.7 live self-test использовал handle `1` как фиктивно
   невалидный, но в реальном runtime это был первый загруженный ресурс.
10. Повторный Scene transition мог снова клонировать payload после того, как
    первый clone уже был проверен и освобождён.
11. Ammo detour публиковал запрошенный shell ID из аргумента даже в native
    ветке, которая отклоняла значение и не меняла активный shell.

## Fixes applied

- Mask scanner теперь принимает только `x` и `?`.
- Config string сначала читается целиком, после чего точно проверяется
  требуемый размер caller buffer.
- Hook records переиспользуют свободные slots и сжимают trailing slots.
- Ошибка hook cleanup возвращается из disable; ownership сохраняется для
  retry. Новые hooks запрещены выключенному моду.
- При невозможности удалить живой hook shutdown не выгружает DLL в
  потенциально опасное состояние.
- Каждый resource load получает отдельный opaque numeric id; stale handle не
  совпадает с новым load.
- Неуспешный backend release сохраняет native pointer и active handle для
  повторной попытки.
- Ammo detour читает подтверждённый shell ID из listener `+0x20` до и после
  original и публикует событие только после реально записанного изменения.
- `EnsureDirectory` проверяет `FILE_ATTRIBUTE_DIRECTORY`.
- UI/Scene API теперь сначала валидирует ownership/type/stale handle и только
  затем сообщает об отсутствии конкретного object callback.

## Main-thread и UI/Scene API в ABI 2.5

- Добавлена owner-aware FIFO очередь: 256 элементов глобально, 64 на мод.
- `main_thread_enqueue` всегда откладывает callback до следующего
  `DispatchFrame`; disable/fault/unload удаляет pending work до `FreeLibrary`.
- UI/Scene вызовы на bound dispatch thread выполняются синхронно; с worker
  thread валидируются и выполняются позже на dispatch thread.
- Queued object work хранит public handles и копии geometry/transform, затем
  повторно разрешает native objects непосредственно перед вызовом backend.
- Проверены invalid/foreign/wrong-type/stale/self-parent handles, short
  structures, отрицательная UI size, нулевой quaternion/scale.
- Проверены FIFO, per-mod overflow, worker dispatch, disable cancellation,
  callback SEH и object backend SEH.
- Старый `WotbModRuntimeResourceBackend`, заканчивающийся после
  `registry_changed`, принимается prefix-copy и продолжает load/reload/release.
- Bundled DAVA bridge использует проверенные UIControl vtable slots,
  Scene AddNode/RemoveNode slots, `Entity+0x3C` TransformComponent и
  `SetLocalTransform` RVA `0x00916070`.
- Живой клиент вернул `WOTBMOD_OK` для UI rect, visibility, add/remove child,
  Scene transform и add/remove child; после release остался responsive.

## UIControl/Entity factories и active roots в ABI 2.6

- В конец `WotbModHostApi` добавлены четыре функции:
  `ui_control_create`, `ui_get_active_screen`, `scene_entity_create`,
  `scene_get_active`; старые offsets не изменились.
- Factory/getter вызовы синхронны и разрешены только на потоке
  `DispatchFrame`; worker получает `WOTBMOD_ERROR_WRONG_THREAD` без созданного
  handle.
- Созданные объекты владеют исходной constructor reference; active roots
  получают отдельный `Retain`. Оба вида освобождаются через
  `resource_release`.
- Проверены null/short arguments, feature detection, ownership, attach/detach,
  geometry/transform, запрет file reload для runtime-created/acquired objects,
  stale handles, release и backend SEH.
- Live bridge использует client `operator new`, реальные конструкторы
  `UIControl` и `Entity`, `EngineContext::UIControlSystem::GetScreen`, а также
  Scene `Activate`/`Deactivate`/`Draw` tracker.
- Отсутствие активной 3D Scene до входа в hangar/battle является нормальным
  `WOTBMOD_ERROR_NOT_FOUND`; API не возвращает фиктивный Scene object.

## Client events и resource clone в ABI 2.7

- В конец `WotbModHostApi` добавлены `resource_clone`, `event_subscribe` и
  `event_unsubscribe`; всего host table содержит 58 функций.
- Loader ingress покрывает `UI_SCREEN_CHANGED`, `SCENE_ACTIVATED` и
  `SCENE_DEACTIVATED`. Runtime синхронно клонирует backend wrappers, ставит
  событие в bounded FIFO и доставляет его до `on_frame`.
- Event payload handles являются borrowed: release/reload возвращают
  `ACCESS_DENIED`, а после callback — `INVALID_ARGUMENT`.
- `resource_clone` внутри callback создаёт обычный mod-owned handle, который
  переживает callback и освобождается через `resource_release`.
- Проверены mask validation, monotonically increasing sequence, main-thread
  delivery, payload shape, owner-only unsubscribe, stale ids, лимит 16
  подписок, disable/fault cleanup, callback SEH и backend clone SEH.
- Live run подтвердил 7 UI transitions, 2 Scene activations и 1 Scene
  deactivation без единого FAIL.

## Existing UI в ABI 2.8

- Добавлены `ui_control_find_by_name`, parent/child traversal,
  `ui_control_get_state`, `ui_control_set_input_enabled` и
  `ui_control_set_disabled`; host table вырос до 72 функций.
- Fake UI tree проверяет recursive/non-recursive find, parent, count/index,
  geometry и visible/input/disabled flags, hierarchical mutations,
  ownership, stale handles, wrong type/thread, backend errors и SEH.
- Все возвращаемые parent/child/find handles получают отдельный backend
  retain и освобождаются обычным `resource_release`.
- DAVA bridge использует подтверждённые x86 offsets и `FastName`,
  `SetPosition`, `SetSize`, `SetInputEnabled`, `SetDisabled`,
  `AddControl`/`RemoveControl`. Опасный старый slot 10 исключён: анализ
  показал, что это `SystemInput`, а не visibility operation.
- Visibility меняет проверенный visible bit и dirty-state иерархии без вызова
  неверно размеченного vtable slot.

## Vehicle API и gameplay events в ABI 2.8

- Добавлены семь owner-aware vehicle operations: local/by-id/count/index,
  clone/release/info. Публичный мод получает snapshot, а не указатель на
  игровой объект.
- Проверены два fake vehicle, local/team/health/names, partial `struct_size`,
  лимит 64, ownership, stale и borrowed handles, wrong thread, backend errors,
  SEH и нулевой token balance после cleanup.
- Generic event ingress проверен для 12 событий:
  battle entered/started/ended/left, local vehicle changed,
  spawn/despawn, shot, health changed, damaged, destroyed и reload state.
- Проверены FIFO, sequence, dispatch thread, `payload_size`, typed union
  payload, два callback-scoped vehicle handles, clone и инвалидирование
  borrowed handle после callback.
- Live loader содержит signature-guarded x86 hooks для
  `Vehicle::onEnterWorld`, `onLeaveWorld`, `showShooting`, `set_health`,
  Avatar health update и `ReloadTimer::setState`. Он успешно собран.
- `BATTLE_STARTED`/`BATTLE_ENDED` сейчас выводятся из первого enter/последнего
  leave.
- Добавлены native ingress для UI input, shell hit, ammo, aim target и
  spotted/unspotted. Все они копируют POD payload через
  `WotbModRuntime_NotifyClientEvent`; loader успешно собран.
- Добавлен отдельный `new_gameplay_events_test_mod.dll`: он подписывается
  на семь focused-масок, включая camera mode, и проверяет payload sizes, main-thread delivery,
  sequence и borrowed vehicle ownership. Deterministic host получил полный
  mask `0x003E1008`, 14 PASS и нулевой баланс backend tokens.

## Vehicle skin registry/resolver в ABI 2.9

- Добавлены `vehicle_skin_register`, `vehicle_skin_set_enabled`,
  `vehicle_skin_get_info`, `vehicle_skin_release`; host table вырос до
  76 функций.
- Full host проверяет validation, duplicate ids/paths, limits, ownership,
  stale handles, priority/tie, enable/disable fallback, owner-only mount
  resolution и cleanup при disable/fault/shutdown.
- Native loader содержит guarded `DAVA::File::Create` interception path для
  точных stock paths `.sc2`, material/FX YAML и `.tex`; наличие этого пути
  само по себе не доказывает, что rendered tank запросил и принял подмену.
- Native live-test регистрирует безопасный фиктивный tank path и проверяет
  info/enable/disable/release. Focused registry/resolver завершился `15/0`;
  применение mesh/material/texture к отображаемому танку не подтверждено.
- Добавлен отдельный `vehicle_skin_test_mod.dll`: low/high skin registrations,
  три asset kinds, info, disable/enable, release и stale handle дали
  15 PASS. Host отдельно подтвердил mesh `.dvpl`, material и texture resolver.
  Publish fixture содержит T-34-85 mesh/texture assets с контрастной checker
  DDS; registry/resolver live-проверены, но capability и отчёт не считают это
  доказательством native visual override (`VISUAL NOT VERIFIED`).

## Audio API в ABI 2.2 и custom files в ABI 2.3

- Добавлен typed resource `WOTBMOD_RESOURCE_AUDIO_CLIP`.
- Добавлены все семь playback operations: `audio_play`, `audio_pause`,
  `audio_resume`, `audio_stop`, `audio_set_parameters`, `audio_get_state`,
  `audio_release`.
- Playback handles имеют уникальные ids, ownership и stale-handle protection.
- Clip нельзя освободить до release всех связанных playbacks.
- Поддерживаются defaults, loop, start-paused, 2D pan, 3D position/distance,
  volume и pitch.
- Backend callbacks изолированы SEH; неуспешный release сохраняет handle для
  retry.
- Disable/fault/shutdown выполняют stop/release audio до resource cleanup.
- `WotbModRuntimeAudioBackend` допускает minimal `play/release`; cached
  state работает без optional `get_state`.
- ABI 2.3 добавляет `audio_clip_load` и callbacks
  `load_clip`/`reload_clip`/`release_clip`.
- Custom clip получает только физический loose-файл, разрешённый через
  активный mount; произвольные absolute paths через публичный API недоступны.
- Старый ABI 2.2 размер `WotbModRuntimeAudioBackend` принимается и
  динамически проверен.
- Готовый Windows backend реально декодирует тестовый WAV через Media
  Foundation и воспроизводит через XAudio2.
- Реальный backend проверен на load/reload/play/loop/pause/resume/state,
  volume/pitch/pan/stop/release и защиту lifetime clip/context.
- Готовый backend явно отклоняет spatial 3D; это остаётся возможностью
  client-specific sound adapter.

## Native sound events в ABI 2.4

- Добавлены 12 публичных operations: `sound_event_create`,
  `sound_event_trigger`, `sound_event_stop`, `sound_event_set_paused`,
  `sound_event_set_volume`, `sound_event_set_position`,
  `sound_event_set_parameter`, `sound_event_get_parameter`,
  `sound_event_has_parameter`, `sound_event_get_state`,
  `sound_event_get_name`, `sound_event_release`.
- Fake backend adversarial suite проверяет invalid args, ownership, stale
  handles, лимит 64, cleanup, optional callbacks, returned errors и SEH.
- Live bridge разрешает активный `DAVA::SoundSystemProxy`, извлекает внутренний
  hybrid sound system из `this+0x20` и использует proxy create thunk.
- Сигнатура native `Stop` исправлена на `Stop(bool force)`; bridge передаёт
  `true`.
- DAVA `IsActive` описывает активность event object, а не детальное состояние
  проигрывания. Поэтому ABI возвращает детерминированное lifecycle state,
  которое обновляется после trigger/pause/resume/stop.
- Реальный event `guns/tracers/tracer_hard` создан, получил volume, position и
  RTPC, прошёл trigger/pause/resume/stop и был освобождён без падения клиента.
- Native RTPC getter может вернуть нормализованное/текущее значение движка,
  отличное от последнего переданного float; контракт требует успешное конечное
  значение, а не побитовое echo.

## Dynamic coverage

- Все 72 функции `WotbModHostApi`.
- Все 11 функций `WotbModRuntime_*`, включая generic client-event ingress.
- `WotbModWindowsAudio_Create` и `WotbModWindowsAudio_Destroy`.
- `WotbModDavaSound_Create`, `WotbModDavaSound_Destroy` и полный native
  sound event backend.
- Самостоятельные `api_selftest_mod.dll` и `custom_audio_test_mod.dll`,
  которые работают без test-only exports основного host harness.
- `WotbModApi_GetHost`, `WotbModApi_GetVersion`, `wotbmod_logf`.
- Все lifecycle callbacks и SEH isolation.
- Все 7 `WotbModResourceType`.
- Hook/resource/audio/sound/UI/Scene/gameplay backend success, unavailable,
  optional callbacks, returned errors и SEH faults.
- Custom-audio route без generic resource backend и fallback старого
  typed-audio route.
- Hook/resource/audio limits 64, mount limit 32, ownership между двумя модами.
- Loose/DVPL fallback, priority/tie ordering, path normalization/traversal,
  exact/short buffers и четыре параллельных resolver threads.
- Empty, no-backend, no-reload, no-audio, minimal-audio,
  shutdown/reinitialize и malformed DLL cycles.
- Bad ABI, short descriptor, duplicate/invalid id, missing entry, returned
  entry error и entry SEH.
- Публичные headers компилируются как C; runtime и tests компилируются с `/W4`.
- Семь native ZIP packages проверены по manifest, DLL/resources и PE `MZ`,
  загружены в backend, повторно скачаны и побайтово подтверждены через размер
  и SHA-256. Шесть безопасных пакетов одобрены; намеренно падающий
  `fault-isolation-test` оставлен `pending`.
- Финальный live loader на 11.19 подтвердил 206 вызовов реального UI package entry hook
  и 38 вызовов Scene loader hook. Все `UIPackage`, named `UIControl`, `Scene`
  load/reload/release, ABI 2.5 mutations и ABI 2.6 direct
  `UIControl`/`Entity` creation завершились `PASS`. После раннего
  `NOT_FOUND` Scene getter получил активную hangar Scene; loose custom `.sc2`
  и созданный `Entity` были прикреплены на 180 кадров, затем сняты и
  освобождены. ABI 2.7 дополнительно доставил все три типа client events и
  подтвердил borrowed/clone lifecycle.
- ABI 2.9 contract дополнительно покрывает existing UI tree/state, все vehicle
  operations, все event payload families, borrowed vehicle lifecycle и
  skin registry/resolver без утверждения о native visual application.
  Нативный loader установил весь набор
  gameplay hooks с `mask=0xFFF (12/12)`.

## Cleanup and rollback

- Resolver thread handles дождались завершения и закрыты.
- Все mounts/hooks/resources/audio playbacks сняты либо проверены через
  runtime cleanup.
- Fake resource и audio backends завершили с balance `0`.
- Test environments находятся только под `build\` и являются намеренными
  build artifacts.
- Несвязанные файлы не удалялись и не перезаписывались.

## Residual risks

- ABI 2.9 gameplay hooks установлены в свежем клиенте `11.19.0.834`
  (`12/12`). Battle-only callback-ы в этом запуске не получили реальных
  выстрелов/урона/смены боеприпаса/spotting, поэтому их полевая доставка
  остаётся pending.
- `BATTLE_STARTED`/`BATTLE_ENDED` выводятся из vehicle world lifecycle, а не
  из отдельного arena-state callback.
- Все 22 event masks имеют loader ingress; семь focused-источников
  детерминированно проверены focused-модом, но не все были стимулированы
  реальным боем.
- Stock `.sc2`/material/`.tex` replacement реализован через native file
  bridge и contract-tested. Видимая замена конкретного реального танка и
  поведение DAVA cache остаются live pending.

- Реальный native sound adapter проверен внутри Blitz, но он управляет
  событиями из уже загруженных клиентских sound banks. Регистрация нового
  Wwise bank ABI 2.4 не заявлена.
- Public resource API проверяет mount/resolve/load/reload/release для UI YAML,
  `UI_PACKAGE`, `UI_CONTROL`, `SCENE`, texture и generic files. Live backend
  создаёт настоящие refcounted `UIPackage`, named/direct `UIControl`,
  `Entity` и `Scene`; reload заменяет file-backed объект, release проходит
  через DAVA. Active screen lookup, ранний Scene `NOT_FOUND`, последующий
  positive active Scene lookup и полный custom `.sc2`
  transform/attach/180-frame/detach/release lifecycle подтверждены live.
  Родные hooks дали UI hit=206 и Scene load hit=38.
- Готовый Windows backend не включает OGG decoder и spatial 3D.
- Одновременные subscription/backend/lifecycle операции проверены
  детерминированными race-тестами и stress `30/30`: unregister/destroy ждёт
  callback quiescence, backend replacement изолирует generation, а
  reentrant `DispatchFrame` не удерживает transition lock во время ожидания.
- Жёсткий лимит 128 одновременно обнаруженных DLL проверен анализом кода, но
  suite не создаёт 129 отдельных PE DLL.

## Evidence

```text
SMOKE OK: mods=3 lifecycle+fault-isolation+resources passed
API FULL OK: assertions=1651 all public functions passed
API FULL STRESS: 10/10 passed
CLIENT SUBSCRIPTION/BACKEND STRESS: 30/30 passed
API SELFTEST: passes=92 skips=10 failures=0
AUDIO SELFTEST: passes=24 failures=0 finished=1
PUBLISHED SELFTEST OK
NEW EVENTS MOD: passes=14 failures=0 seen=0x003E1008 expected=0x003E1008 finished=1
VEHICLE SKIN MOD: passes=15 failures=0 finished=1
NEW API MODS OK
=== Mod API build, smoke, and full contract tests passed ===
```

Последний исторический live-client snapshot ABI 2.9, не перезапускавшийся в
этой итерации: API `108/2/0`, native `142/0/0`, custom audio `24/0`,
gameplay hooks `12/12`.

## Native validation infrastructure (offline)

Свежий focused-прогон `tests\build_native_validation_tests.cmd` компилирует
`wotbmod.native_validation` с `/W4 /WX /permissive-`, загружает DLL обычным
Windows loader и проверяет 194 assertions:

- ровно 50 уникальных capability и все 16 требуемых разделов, включая
  отдельные `impact.visual` и `impact.confirmed`;
- запрос UI V3, Projectile V2 и Devtools V3; targeted subscriptions на четыре
  projectile lifecycle topic без frame-rate wildcard;
- наличие V3 entrypoint и внутренних test-contract exports;
- ID/version/callbacks и запрос permission tier `UNSAFE`;
- native status cap `LIVE_TEST_PENDING` до ручного PASS;
- PASS/FAIL/SKIP status transitions;
- privacy sanitizer для token/password/e-mail и buffer contract;
- invalid bootstrap/mod/output rejection.

Live client в этом прогоне не запускался. Следовательно, fixed-RVA hooks,
camera/entity/RPC/shell/tracer/render/audio native behavior подготовлены для
ручной проверки, но не переведены в `SUPPORTED`. Package preflight привязан к
build `11.19.0.834` и точному executable SHA-256.

`loader\build_live.cmd` собирает DLL, кладёт manifest и payload в
`build\live_env\mods\wotbmod.native_validation`, включает мод в `mods.ini` и
выдаёт ему явный tier `3`. Runtime JSONL/results/matrix/failures создаются
только при пользовательском live-запуске. Инструкция и export script находятся
в `MANUAL_LIVE_VALIDATION_RU.md` и
`tools\export_live_validation_bundle.ps1`.

### Backend artifacts (последний опубликованный ABI 2.7 snapshot)

Все записи прочитаны обратно из Blob registry после upload; скачанные ZIP
совпали с локальными артефактами.

| Package | Version | Bytes | SHA-256 | Review |
| --- | --- | ---: | --- | --- |
| `api-23-selftest` | `2.7.0` | 9674 | `249d98f5d0975e4a4327c13e44b92704571e2c0daf2cf49781333a722f10747c` | approved |
| `custom-audio-api-test` | `1.2.0` | 7730 | `065a700e2eed7879364e8b6387334f08127226c7941d2178f5b738610d3104bd` | approved |
| `api-contract-fixture` | `2.7.0` | 5247 | `af5c82b2adcbdf1e32dfd7ad6e130a38096a1c8a767d5786c609d265d17dfbc0` | approved |
| `api-ownership-peer` | `1.2.0` | 4627 | `fe5c1334b1aa0d2630d35697ce2cd1e73195d1cf769e647e36225bb3c381d273` | approved |
| `resource-overlay-test` | `1.2.0` | 5357 | `85bc1833963e041d65e82da6b7bc8dbc7b0df03f8501f957e11319dff504e98e` | approved |
| `fault-isolation-test` | `1.2.0` | 4610 | `6040d09723a4d2f2780b9483ee038f436e646bdb19b5f495f55e3092ee481796` | pending |
| `native-client-api-test` | `1.4.0` | 17954 | `f5d84ef83aceb6b947c251af51718760815535d48a478b18c4ec45ba087648c8` | approved |

### Локальные ABI 2.9 artifacts

Эти ZIP пересобраны из текущих DLL и проверены через `modpack.py inspect`.
`new-gameplay-events-test` и `vehicle-skin-api-test` также загружены и
одобрены на backend; остальные ABI 2.9 пакеты в этом прогоне не
перепубликовывались.

| Package | Version | Bytes | SHA-256 | Verification |
| --- | --- | ---: | --- | --- |
| `api-23-selftest` | `2.9.0` | 11711 | `83c9b6dc8b301f8adb2e51cbfd5cee31266e968f000743f87cc757119850211a` | contract PASS |
| `api-contract-fixture` | `2.9.0` | 5246 | `5562f1ab0002b8c4dfb6d4d3cd63978a3cfaa3eadda008f3c9eb24d4902966da` | contract PASS |
| `native-client-api-test` | `1.6.0` | 19644 | `8434e56bb955e4b938d6d4e8cdd8a6ab29e73f36e473be78482f9fee80626ec0` | ZIP inspect + 142/0 live PASS |
| `new-gameplay-events-test` | `1.0.0` | 6705 | `2d39a5908a97f8ced42bcc7699e8b364fdb33dc703b22d067b3a3fd5159eb5dc` | 13/0 contract PASS / hooks live / battle stimuli pending |
| `vehicle-skin-api-test` | `1.0.0` | 1134417 | `0db5645c0d54f2270ab6b9f611a4d8db6a08ec4a6f14d97333c4cb1669c34ff8` | 15/0 contract + live registry PASS / visual pending |

### V3 RC1 sample packages — 2026-08-02

Пакеты пересобраны `tools\build_rc1_sample_packages.ps1`, затем каждый прошёл
schema/package inspection. Они намеренно не подписаны и предназначены для
локальной разработки: mandatory-signature policy должна отклонять их до
подписи доверенным ключом.

| Package | Bytes | SHA-256 | Host verification |
| --- | ---: | --- | --- |
| `sample.ui_transaction.wotbmod` | 19729 | `342a188ae17d5168ff8055bfc38069a504559ee1013eecf8d265933538637485` | valid, REVIEWED, sample host PASS |
| `sample.vehicle_cosmetic.wotbmod` | 2367801 | `7e8466b07bc1895f7fbbbb7457315380dd28484276c42fa758a4f99b889174f0` | valid, REVIEWED, sample host PASS |
| `sample.camera_render.wotbmod` | 16292 | `294c6ee0b7ef7e60d9a0686ce420ddd26e310072efaf787e96cfb41ffeb107b4` | valid, REVIEWED, sample host PASS |

RC1 aggregate: 4 568 числовых checks/PASS, 145 frozen snapshot records,
10 явных `SKIP`, 0 `FAIL`; package-runtime, runtime-services и smoke suites
также завершились `PASS`, но не публикуют единый числовой assertion count.
Live/native verdict намеренно не повышен выше `LIVE_TEST_PENDING`.
