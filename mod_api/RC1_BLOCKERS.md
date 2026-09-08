# WotbMod API V3 RC1 blockers

RC1 является кандидатом для закрытой developer preview. Выпуск блокируется,
если хотя бы один обязательный автоматический или live-сценарий имеет FAIL.

## Обязательные blockers

- crash, access violation, deadlock или необратимый hang в обязательном
  сценарии;
- callback/event/render invocation после unload/owner disable;
- dangling owner handle, subscription, modifier, draw object или async job;
- resource transaction нельзя rollback/release до исходного exact path;
- unsigned/unknown package проходит mandatory-signature policy;
- modified package, invalid signature или signed revoked release загружается;
- native backend/hook включается при несовпадающем exact fingerprint;
- native capability получает `SUPPORTED` без manual PASS/evidence этого
  fingerprint;
- validation-мод при скрытой панели держит render callback, frame subscription
  или создаёт per-frame disk I/O;
- high-frequency trace включается без opt-in, durable flush выполняется на
  каждое event либо snapshot пишется без dirty state;
- sample-мод оставляет UI object, overlay, callback, modifier, handle или
  изменённый resource после unload;
- safe mode не позволяет запустить diagnostics без third-party mods;
- portable-only safe mode загружает native package/loose DLL;
- повторно падающий attributed mod не получает auto-disable marker;
- Validation Bundle содержит token, Authorization/Bearer, e-mail, chat или
  полный RPC payload;
- headers, interface tables, docs, capability/permission registries, manifest
  schema, preflight и samples расходятся;
- build, contract snapshot, package, sample или validation infrastructure test
  красный.

## Не являются blockers RC1

При честном `NOT_SUPPORTED`/experimental status RC1 не блокируют:

- штатный tracer style и native visual attachment;
- exact native DAVA YAML/ResourceArchive adapters; private typed bridge и
  portable snapshots сами по себе не закрывают live evidence;
- native typed UI creation/rendering;
- полная game-owned Scene/Material enumeration;
- live `NMaterial` mutation и hot-swap уже закэшированного tank mesh;
- произвольная DAVA ObjectFactory/component injection; разрешён только reviewed
  class allowlist с доказанным lifecycle;
- semantic audio/hangar/localization interception;
- camera modes без доказанного native source;
- full BigWorld RPC payload или mutation/injection.

Эти gaps нельзя маскировать `OK`, fabricated event или автоматическим
`SUPPORTED`.

## Критерий закрытия RC1

1. Fresh `build.cmd` и все включённые focused tests завершены с 0 FAIL.
2. Три `.wotbmod` sample-пакета собраны и проходят package validation.
3. Обязательные шаги `MANUAL_LIVE_VALIDATION_RU.md` выполнены на одном exact
   fingerprint; FAIL/SKIP обоснованы.
4. Cleanup проверен после unload и 30 reload cycles.
5. Safe-mode recovery и повторный запуск клиента успешны.
6. Validation Bundle экспортирован и privacy-check пройден.

До ручного live-прогона native пункты остаются `LIVE_TEST_PENDING`; это не
автоматический FAIL RC1, но developer preview нельзя объявлять завершённой.
