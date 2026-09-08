# WotbMod API V3 Release Candidate 1

Дата snapshot: 2 августа 2026 года. Это release candidate для закрытой
developer preview, не stable-релиз и не заявление полного native coverage.

Машиночитаемый snapshot находится в `rc1/contract_snapshot.json`. Проверка
`tests/build_v3_rc1_contract_tests.cmd` фиксирует 56 публичных файлов, 45
interface IDs, 51 x86 размер таблиц, числовые macro/enum/error/event values,
51 permission name/tier, manifest schema, package constants и manifests трёх
RC1 sample-модов.

Счётчики выше — это состояние **после re-baseline 2026-08-16**, описанного в
разделе «Новый release contract». Исходный RC1 от 2 августа 2026 фиксировал 51
публичный файл, 40 interface IDs и 46 размеров таблиц; permission-реестр за это
время не менялся.

## Замороженный публичный контракт

До V3 stable запрещено несовместимо изменять:

- все C headers в `include/wotbmod`, `include/wotb_mod_api_v3.h` и
  `include/wotb_mod_runtime_v3.h`;
- C++17 wrapper `include/wotbmod/wotbmod.hpp`;
- имена, версии, порядок slots и существующие префиксы interface tables;
- размеры таблиц V1/V2/V3 на `windows-x86`, зафиксированные ABI probe;
- существующие struct field order/type, callback signatures и calling
  convention;
- числовые enum, event, error, flag, permission-tier и context values;
- 45 interface IDs: `wotbmod.archive`, `async`, `audio`, `audio.intercept`,
  `bigworld.rpc`, `camera`, `camera.state`, `capabilities`, `catalog`,
  `client`, `client.device`, `content`, `core`, `data.yaml`, `devtools`,
  `diagnostics`, `entity.public`, `events`, `gameplay.camera`,
  `gameplay.hangar`, `gameplay.hud`, `gameplay.replay`, `handles`, `hooks`,
  `http`, `input`, `intermod`, `lifecycle`, `loaders`, `manifest`,
  `permissions`, `projectile`, `render`, `render.native`, `resources`, `scene`,
  `scene.enumerate`, `settings`, `storage`, `tracer`, `ui`, `ui.read`,
  `unsafe.native`, `vehicle.visual`, `vfs`;
- permission names и tiers из runtime registry;
- manifest schema V1 из `schemas/wotbmod-manifest-v1.schema.json`;
- `.wotbmod` ZIP-store path policy, limits, package hash/signature envelope,
  trust-store и signed-revocation formats;
- capability names и словари статусов `AVAILABLE/DEGRADED/UNAVAILABLE` и
  validation evidence `HOST_TESTED/LIVE_TEST_PENDING/SUPPORTED/NOT_SUPPORTED`.

Новый slot или field до stable допускается только в новой версии таблицы либо
как добавление в конец совместимого struct после отдельного review. RC1 сам по
себе такие расширения не разрешает.

## Новый release contract — 2026-08-16

Это первое осознанное расширение после RC1, и оно оформлено именно как новый
release contract, а не как правка RC1. Пять новых интерфейсов объявлены
**declaration-only**: slots существуют, зарегистрированы и честно возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED`, пока не появится backend.

Что добавлено:

| Interface ID | Таблица | Что доказано | Permission |
|---|---|---|---|
| `wotbmod.ui.read` | `WotbModV3UiApiV4` | loader-owned зеркало text/texture/font/style | `ui`, для game-owned `ui.modify.game` |
| `wotbmod.camera.state` | `WotbModV3CameraApiV2` | `CameraController+0x5C` (7 состояний аниматора) и `GameCamera+0x320` (`0 = SNIPER`) | `camera.battle.read` |
| `wotbmod.audio.intercept` | `WotbModV3AudioApiV3` | vtable `+0x0C`, `FastName` = один dword, штатный `SoundEventStub` | `audio.events` |
| `wotbmod.scene.enumerate` | `WotbModV3SceneApiV2` | `Entity` `+0x08/+0x0C/+0x18/+0x1C/+0x3C`, `TC+0x60` | `game.entity.public` |
| `wotbmod.tracer` | `WotbModV3TracerApiV1` | таблица из 25 записей на `0x03FD6168`, 8 имён стилей, RGBA на `record+0x34` | `visible.projectile.events` |

Правила, которые при этом соблюдены:

- ни одна существующая таблица не расширена ни на один slot и ни на один field;
  все пять — новые таблицы под новыми interface ID, как уже сосуществуют
  `UiApiV2`/`V3` и `DevtoolsApiV1`/`V2`/`V3`;
- ни одно существующее numeric enum/error/flag значение не переназначено;
- **новых permission не введено вообще.** Каждый интерфейс переиспользует уже
  замороженное имя с подходящей семантикой; добавление permission — обязательство
  большее, чем добавление slot, и здесь оно не потребовалось;
- capability status у всех пяти — `UNAVAILABLE`, а не `DEGRADED`. `DEGRADED`
  означает «работающее подмножество существует»; здесь не работает ничего, и
  назвать это «деградацией» было бы тем же завышением, что и возврат `OK` без
  выполненной операции;
- всё, что не доказано, сделано **невыразимым**, а не просто
  недокументированным: нет camera set (политику переходов движок проверяет на
  восьми местах вызова, а не внутри `SwitchState`); нет tracer create
  (ownership не доказан); нет способа вернуть `nullptr` из audio-детура (через
  ABI не ходит ни один `SoundEvent`-указатель); нет handle, курсора и
  visitor-callback при обходе сцены (единственная защита от беззамкового
  `memmove` в `AddNode`/`RemoveNode` — дисциплина главного потока, поэтому
  небезопасная форма не существует в ABI); нет чтения живой строки game-owned
  `UIStaticText` (getter'ы читают только собственное зеркало и помечают
  game-owned контрол флагом).

`rc1/contract_snapshot.json` пере-базирован в тот же день. Именно этот акт и
есть объявление нового контракта: счётчики в шапке документа изменились с
51/40/46 на 56/45/51. Полное описание — раздел 58 в `docs/API_V3_RU.md`.

## ABI-совместимость

Каждая передаваемая структура начинается с `struct_size` и версии. Consumer
инициализирует весь известный ему размер и не читает хвост за `struct_size`.
Provider проверяет минимальный обязательный префикс, записывает не больше
`min(consumer.struct_size, provider_size)` и сохраняет семантику уже
опубликованных полей. Старый префикс никогда не переупорядочивается.

Существующее numeric значение enum/event/error/flag нельзя переназначать,
удалять или менять. Новое значение в будущей совместимой версии добавляется с
новым числом; неизвестное значение consumer обрабатывает fail-closed или как
`UNKNOWN`, если это предусмотрено типом. Возврат `OK` без выполненной операции
запрещён: отсутствующий backend возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` либо
более точную ошибку.

V3 RC1 ABI целевой — 32-bit Windows/x86. Совпадение layout на x64 не заявлено.

## Experimental и намеренно неполные backends

Следующие slots/интерфейсы остаются частью замороженного ABI, но их native
backend имеет статус `DEGRADED`, `LIVE_TEST_PENDING` или `NOT_SUPPORTED`:

- native typed Text/Image/Button creation и полный game-owned UI style bridge;
- полная enumeration game-owned Scene/Material и live `NMaterial` mutation;
- exact native DAVA YAML/ResourceArchive adapters; loader-private typed ABI и
  portable snapshot conversion host-tested, публичный RC1 ABI не менялся;
- штатный tracer create/style/visual attachment с недоказанным ownership;
- hot replacement уже закэшированного tank mesh/material без штатного reload;
- произвольная DAVA ObjectFactory/component injection; private reviewed registry
  ограничен классами с доказанным constructor/destructor/ownership;
- camera modes `POSTMORTEM`, `FREE`, `CINEMATIC` без доказанного native source;
- semantic audio/hangar/localization interception;
- full RPC payload/injection и скрытые entity data;
- direct native render/COM access вне explicit unsafe permission;
- все пять интерфейсов контракта 2026-08-16 — `wotbmod.ui.read`,
  `wotbmod.camera.state`, `wotbmod.audio.intercept`, `wotbmod.scene.enumerate`
  и `wotbmod.tracer` — целиком: их slots объявлены и заморожены, статус
  `UNAVAILABLE`, каждый вызов возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`.

UI V3 snapshot в RC1 не содержит text getter. Поэтому
`sample.ui_transaction` фиксирует `text=NOT_EXPOSED_RC1`; добавлять новый slot
ради sample после freeze нельзя.

Обновление 2026-08-16: text getter появился, но **не** в `WotbModV3UiApiV3` —
он объявлен в новой таблице `WotbModV3UiApiV4` под отдельным ID
`wotbmod.ui.read` (см. раздел «Новый release contract»). Правило выше не
нарушено: V3 не расширялся. `sample.ui_transaction` продолжает фиксировать
`text=NOT_EXPOSED_RC1` — sample останется пришпилен к RC1, пока у
`wotbmod.ui.read` не появится backend; пока его нет, изменение sample было бы
ровно тем самым «добавить slot ради sample».

## Что остаётся mutable до stable

Без изменения ABI могут меняться внутренняя реализация, diagnostics/reason
strings, документация, sample-код, host tests, binding packs и их evidence,
allowlist точных fingerprints, performance budgets и фактический статус
конкретного native backend после ручного теста. Capability name и набор
допустимых status tokens при этом не меняются.

## Evidence и native support

Portable backend может получить `SUPPORTED` после полного автоматического
набора. Native capability не повышается автоматически по найденному RVA,
успешной установке hook или host test. Для `SUPPORTED` одновременно нужны:

1. точное совпадение build + executable SHA-256 fingerprint;
2. PASS binding-pack validation до установки hook;
3. `HOST_TESTED` текущей ABI/ownership/error границы;
4. ручной PASS сценария в `MANUAL_LIVE_VALIDATION_RU.md`;
5. сохранённое evidence в matrix/results для того же fingerprint.

До этого максимум — `BOUND`, `HOST_TESTED` или `LIVE_TEST_PENDING`. Fingerprint
не совпал — native backend выключается fail-closed.

## Проверка snapshot

```bat
tests\build_v3_rc1_contract_tests.cmd
```

Изменение frozen файла, interface table size, numeric value, permission,
schema, package constant, capability status slot или sample manifest без
осознанного нового release contract делает тест красным.
