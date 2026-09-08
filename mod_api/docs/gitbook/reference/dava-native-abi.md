# Приватный native DAVA ABI

## Назначение

`include/wotb_mod_dava_native.h` описывает версионированную внутреннюю границу
между loader и portable runtime. Это не публичный API мода и не расширение
замороженного V3 RC1: заголовок не включён в `wotb_mod_api_v3.h`, а Lua и DLL
мода не получают таблицу backend, provider token или указатель движка.

Граница нужна, чтобы exact-build адаптер мог подключать только доказанные
операции DAVA, не протаскивая C++ layout, STL, RTTI, exceptions или allocator
кода игры через публичный C ABI.

## Контракт владения

- Provider возвращает внутренний `provider_token` только registry loader-а.
- Registry заменяет его монотонным host token и привязывает к owner и object
  kind.
- Каждый вызов проверяет owner, kind и существование token до обращения к
  provider.
- Token другого owner и stale token отклоняются.
- Release удаляет token только после успешного release provider-а; временная
  ошибка сохраняет token для повторной попытки.
- Owner cleanup освобождает все его token. Снятие или замена backend ждёт
  завершения уже начатых callback и не публикует старые объекты в новое
  поколение.

Public YAML/archive API не удерживает native объект. Loader полностью копирует
bounded YAML UTF-8 или проверенные archive entries в portable owner-owned
snapshot, после чего сразу освобождает native token. Поэтому созданный public
handle не зависит от срока жизни provider-а и остаётся читаемым после его
снятия.

## Capability groups

Backend объявляется all-or-nothing по каждой группе:

| Capability | Обязательные callbacks | Статус exact 11.19.0.834 |
|---|---|---|
| `YAML` | parse file, export UTF-8, release | `LIVE PASS` на `11.19.0.834`: native parse/export/release |
| `RESOURCE_ARCHIVE` | open, count, entry, read, release | `LIVE PASS` на `11.19.0.834`: native open/enumerate/read/release |
| `NMATERIAL` | typed mutation, release | `LIVE PASS` на `11.19.0.834`: create, property mutation/removal и apply |
| `MESH_HOT_SWAP` | consumer/mesh typed swap | `LIVE PASS` на `11.19.0.834`: mesh load и typed hot-swap |
| `STOCK_TRACER` | typed create, release | `LIVE PASS` на `11.19.0.834`: штатный tracer bridge |
| `CLASS_FACTORY` | reviewed lookup/create, release | `LIVE PASS` для safe allowlist: `DAVA::UIControl`, `DAVA::Entity`, `DAVA::NMaterial`, `DAVA::Texture`, `DAVA::Mesh`, `DAVA::MeshConsumer` |

Неполная callback-группа отвергается целиком. Capability без exact fingerprint,
доказанных constructor/destructor, thread и ownership не выставляется.

## Reviewed class registry

Class factory намеренно не является pass-through к `DAVA::ObjectFactory`.
Provider обязан держать закрытый allowlist классов, для которых доказаны размер,
конструктор, деструктор, heap, поток и object kind. Текущий exact provider
принимает только `DAVA::UIControl`, `DAVA::Entity`, `DAVA::NMaterial`,
`DAVA::Texture`, `DAVA::Mesh` и `DAVA::MeshConsumer`. Lua и public ABI получают
только typed safe handles; произвольное имя DAVA-класса, payload произвольного
layout, component injection, raw vtable, FFI и адрес объекта запрещены.

Добавление нового класса требует reviewed adapter, host-тестов и отдельного
live-прогона exact client. Шесть перечисленных классов этот путь прошли;
неизвестное имя остаётся `SAFE_REVIEWED_ONLY`, а не превращается в raw create.

## Что остаётся непубличным

Наличие exact adapter не расширяет frozen public ABI до raw DAVA. Публичный Lua
слой работает только с typed handles, которые проверяются по owner, kind и
lifetime. Произвольные DAVA objects, layout payloads, component injection,
vtable/pointer access и общий `ObjectFactory` остаются недоступны намеренно:
они не имеют compiler-neutral ownership contract и не проходят permission,
teardown и unload/reload границы.

Exact-client live log на fingerprint `11.19.0.834` доказал успешные операции,
cleanup и отсутствие crash/safe-mode markers для всех шести capability-групп.
Это не отменяет fail-closed fingerprint gate: после обновления клиента нужен
новый binding pack и новый live PASS.

## Доказательства

- `v3_dava_native_registry_tests`: owner/kind/stale checks, all-or-nothing
  groups, related-object validation, release retry, concurrent replacement/
  removal и шесть typed groups.
- `v3_data_truth_tests`: native YAML/archive provider doubles, bounded snapshot,
  немедленный release и чтение snapshot после снятия backend.
- `test_dava_native_exact_abi.py`: проверка fingerprint и машинного кода точных
  ABI `FastName`, `NMaterial` и `ResourceArchive`.
- Полный clean `build.cmd` 16 августа 2026 года завершился общим PASS после
  удаления только `mod_api/build`.
- Финальный live evidence:
  `%LOCALAPPDATA%\wotbmod\live_evidence\20260816-235441-cb3d45b3`.
  Итоговая строка: `class-factory=PASS`,
  `arbitrary-object-factory=SAFE_REVIEWED_ONLY`, `native-yaml=PASS`,
  `resource-archive=PASS`, `nmaterial=PASS`, `mesh-hot-swap=PASS`,
  `stock-tracer=PASS`; crash dump delta `10 -> 10`, verdict `CLEAN`.
