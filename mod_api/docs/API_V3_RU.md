# WotbMod API V3 — полная русская документация

## 1. Статус и граница гарантий

API V3 — модульный C ABI для Windows x86. Публичная точка входа находится в `include/wotb_mod_api_v3.h`; этот агрегатор включает все стабильные заголовки `include/wotbmod/*.h`.

Текущие версии:

- ABI: `WOTBMOD_V3_ABI_VERSION == 0x00030000`;
- SDK: `WOTBMOD_V3_SDK_VERSION == 0x00030000`;
- bootstrap: `WOTBMOD_V3_BOOTSTRAP_VERSION == 1`;
- UI: interface V3, бинарно prefix-compatible с V2;
- Audio: interface V2;
- Devtools: interface V3, prefix-compatible с V2;
- Projectile: interface V2, prefix-compatible с V1;
- Vehicle Visual: interface V2, prefix-compatible с V1;
- остальные публичные интерфейсы из матрицы ниже: V1, если в их заголовке не
  указана более новая версия.

Важное правило достоверности: успешный `query_interface` подтверждает доступ к таблице ABI, но не обещает, что каждая native-зависимая операция может быть выполнена на текущем клиенте. Перед вызовом проверяются:

1. named permission из фактически выданного allowlist;
2. permission tier мода;
3. текущий game context;
4. capability и состояние binding pack;
5. owner/type/generation переданного handle;
6. требуемый thread;
7. наличие реального host/native backend.

Если безопасного backend нет, операция возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`, `WOTBMOD_V3_E_CLIENT_MISMATCH`, `WOTBMOD_V3_E_PERMISSION_DENIED`, `WOTBMOD_V3_E_WRONG_THREAD` или `WOTBMOD_V3_E_PLATFORM`. Эти коды являются корректным отказом, а не успехом.

Компиляционный тест подтверждает ABI и наличие function slots. Фактическую работу UI, Scene, Audio, Camera, Render, vehicle visuals, entity/RPC, projectile и LeaveToHangar подтверждает только запуск на поддерживаемом клиенте с установленным binding pack.

### 1.1. Матрица фактической поддержки этой сборки

Матрица ниже описывает текущую реализацию, а не только наличие методов в C ABI.

| Блок | Реально поддерживается | Явно не поддерживается |
|---|---|---|
| DVPL | Проверка footer, размеров и CRC; type `0` без сжатия, type `1` LZ4 и type `2` LZ4HC | Type `4` streaming LZ4 без guarded native stream backend возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` |
| YAML/archive/loaders | Strict minimal YAML, bounded text/binary loaders, безопасный directory archive и ограниченный ZIP/`.wotbmod` reader с enumerate/read/extract/CRC/SHA-256; exact 11.19 DAVA YAML/ResourceArchive adapter копирует native YAML/archive в owner-owned snapshot и имеет live `PASS` | ZIP64, encryption, compression и другие неподдержанные ZIP features не принимаются как успешное чтение |
| Content-only | `TEXTURE`, `UI` и `MODEL` применяются как транзакционные VFS overlays; ошибка откатывает уже подключённые mounts, disable снимает их, enable подключает повторно | Семантические `AUDIO`, `HANGAR` и `LOCALIZATION` overrides возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`; custom audio и exact-path overlays доступны через отдельные API |
| Native UIControl | UI V3 получает active screen, создаёт API-owned DAVA controls всех 11 безопасных типов, меняет native text/texture/font/color/opacity/background, управляет деревом/layout/style stack, независимо клонирует controls и доставляет input events | Произвольное имя внутреннего DAVA-класса и raw component injection не выдаются Lua; localization/tooltip/accessibility/focus, native animation components и semantic named slots требуют отдельных доказанных backends |
| Input | Actions, contexts, bindings, conflict detection, WndProc/DAVA ingress, bounded polling/event queues, capture contracts и coalescing | Неизвестные device-specific поля внутреннего DAVA payload не выдумываются |
| Camera | Native active/transform/FOV/clipping/projection; штатный callback даёт `ARCADE`/`SNIPER`, context даёт `HANGAR`/`REPLAY`; loader-owned `AFTER_GAME` modifiers/transition/shake очищаются вместе с owner | `POSTMORTEM`, `FREE`, `CINEMATIC`, `BEFORE_GAME` и штатное переключение gameplay camera требуют отдельных доказанных sources |
| Render/RHI/DXGI | Один loader-owned Present ingress; D3D11 backend/viewport/frame telemetry, managed texture/material/sprite/text/line и managed `draw_mesh` через временное DAVA Scene attachment; guarded native device/context/swapchain | `BEFORE_UI`/`AFTER_UI`, D3D12/Vulkan/Metal/OpenGL и native access без UNSAFE permission возвращают явный отказ |
| Public entity | Только local или уже visible public entities; native ingress заполняет public ID/type, visibility/local flags, health/max health, public vehicle type, team, position/direction (своя машина из блока позы PlayerController, остальные видимые по объекту внешности) и display name (ник из ростера арены); сверх snapshot по имени доступны `clan_tag`, `account_id`, `kills`, `vehicle_name` | Поле без источника не выдаётся нулём как успешный результат: оно возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` |
| BigWorld RPC | Только metadata уже наблюдаемого входящего RPC: public entity ID/type, sequence, timestamp и method name | Outgoing observation, payload, send/injection, modify, drop и replay возвращают `WOTBMOD_V3_E_NOT_SUPPORTED` |
| Shell/projectile/tracer | Projectile V2: provenance-aware `CREATED/IN_FLIGHT/IMPACTED/DESTROYED`, native shot/hit ingress, owner/generation/timeout cleanup, snapshots, managed DAVA Scene impact visuals и live-проверенный loader-private stock tracer create route | Безопасная корреляция каждого stock impact с предыдущим shot и public stock tracer style mutation не доказаны |
| Vehicle visuals | Transactional skin packs подменяют exact `game://` mesh/material/texture paths, отдельные LOD `0..15`, apply/state/rollback/release; loader-private exact 11.19 mesh load/hot-swap и NMaterial/Texture mutation+apply имеют live `PASS` | Уже закэшированный model требует штатного reload, если мод не использует отдельный typed hot-swap path |
| Runtime hardening | Per-mod callback budget/coalescing/profiler; exact-build binding validation report; ECDSA P-256/SHA-256 detached signatures и filesystem trust store | Validator не генерирует новые RVA автоматически; несовпавший binding отключается fail-closed |
| Client/device | Штатный `LeaveToHangar` и generic device info через Win32/DXGI доступны при корректном context/thread/grant | Внутренний DAVA `DeviceInfo` не читается через непроверенные object layouts |
| Lua/WASM | Официальный `wotbmod.lua_host` встраивает Lua 5.4.7 и переводит вызовы в reviewed V3 interfaces; scripting API, manifest lifecycle, runtime UI, typed events и instruction budget документированы отдельно | Прямой Lua/WASM entrypoint не входит в замороженный C ABI V3; WASM runtime не реализован, raw pointers/FFI и произвольные native calls Lua не выдаются |
| Legacy raw native API | В shipping loader raw process/RVA/export/pattern и legacy raw hooks выключены по умолчанию | Runtime option или переменная окружения сами по себе не включают их без отдельной developer-unsafe сборки |

### 1.2. Проверка текущей реализации

Свежий полный прогон от 2026-08-02 подтвердил: V3 core `105/105`,
header-only C++ wrapper `128/128`, package/trust `68/68`, runtime services
`PASS`, safe gameplay/projectile bridge `63/63`, DVPL `17/17`, data/input
truth `272/272`, interface truth `72/72`, HTTP `106/106`, tooling/callback
budget `269/269`, named permissions `155/155`, operation/vehicle skin
permissions `327/327`, data-named permissions `90/90`, truthful client slices
`365/365`, camera layout `5/5`, native UIControl bridge `49/49` и public UI
bridge `66/66`, native validation `194/194`, safe-mode recovery `84/84`,
RC1 samples `62/62` и validation bundle `29/29`. ABI compile включает UI V3,
Projectile V2, Vehicle Visual V2 и Devtools V3 prefix assertions. Полный host
результат перечислен в `tests/API_TEST_REPORT.md`; live loader собран с exit
code `0`, но игра в этом прогоне не запускалась.

## 2. Подключение

```cpp
#include "wotb_mod_api_v3.h"
```

Собирать мод нужно как x86 DLL с C++17:

```bat
cl /nologo /std:c++17 /EHsc /MD /W4 /WX /permissive- /LD ^
  /Iinclude mod.cpp /link /OUT:mod.dll
```

Публичный ABI является C-совместимым. Реализация мода может быть на C или C++, если экспорт, calling convention, размеры структур и ownership соблюдены.

### 2.1. Header-only C++17 wrapper

Для C++-модов доступен необязательный слой `include/wotbmod/wotbmod.hpp`.
Он остаётся поверх того же C ABI: не передаёт STL-типы или исключения через
границу модуля, не выделяет ABI-память и возвращает исходный
`WotbModV3Result` через `Status`/`Result<T>`.

```cpp
#include "wotbmod/wotbmod.hpp"

WotbModV3Result LogReady(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    const wotbmod::v3::ModContext context(bootstrap, mod);
    const auto core =
        context.query_interface<WotbModV3CoreApiV1>();
    if (!core.ok()) return core.status().code();

    return core.value_unchecked()->log(
        mod,
        WOTBMOD_V3_LOG_INFO,
        "example",
        "C++ wrapper ready");
}
```

`query_interface<T>()` использует `InterfaceTraits<T>` и проверяет имя,
минимальную версию, размер таблицы и её `api_version`. Для нестандартной
таблицы есть overload с явными `interface_name` и `minimum_version`.
`Result<T>::get_if()` возвращает `nullptr` при ошибке;
`value_unchecked()` используется только после явной проверки `ok()`.

`OwnedHandle` управляет одной ссылкой из `wotbmod.handles`:

```cpp
WotbModV3Result UseOwnedHandle(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3Handle newly_created_handle) {
    const wotbmod::v3::ModContext context(bootstrap, mod);
    auto adopted = wotbmod::v3::OwnedHandle::adopt(
        context,
        newly_created_handle);
    if (!adopted.ok()) return adopted.status().code();

    wotbmod::v3::OwnedHandle owned =
        std::move(adopted.value_unchecked());
    WotbModV3HandleInfo info = {};
    const wotbmod::v3::Status info_status = owned.info(info);
    if (!info_status.ok()) return info_status.code();

    return owned.close().code();
}
```

- `adopt()` принимает уже принадлежащую вызывающему ссылку без `retain`;
- `retain()` создаёт дополнительную owned-ссылку;
- копирование вызывает `retain`, а move только переносит ownership;
- ошибка копирующего конструктора доступна через `last_status()`;
- для проверяемого копирования и присваивания используются `clone()` и
  `copy_from()`;
- `close()` возвращает ошибку `release`; destructor является
  best-effort fallback, поэтому при важной диагностике вызывается именно
  `close()`;
- wrapper должен быть уничтожен до unload его мода и runtime.

Focused-тест `tests/build_v3_cpp_wrapper_tests.cmd` компилирует слой как
C++17 с `/W4 /WX` и проверяет typed query, exact error propagation,
adopt/retain/copy/move/close, foreign и stale handles на реальном V3 core.

## 3. Entry point и bootstrap

Мод экспортирует:

```cpp
WOTBMOD_V3_ENTRY {
    return WOTBMOD_V3_OK;
}
```

Макрос разворачивается в экспорт `WotbModLoadV3` с сигнатурой:

```cpp
WotbModV3Result WOTBMOD_V3_CALL WotbModLoadV3(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3Info* out_info);
```

`WotbModV3Bootstrap` содержит:

- `query_interface` — получить таблицу с минимальной версией;
- `get_interface_info` — получить version/status/reason без выдачи таблицы;
- `get_last_error` — получить подробную ошибку текущего thread;
- `get_client_info` — получить client version, executable SHA-256, architecture, compatibility state, binding pack version и список отсутствующих bindings.

`WotbModV3Info` заполняется модом:

- `id` — стабильный ASCII ID до 95 символов;
- `name`;
- `version` — semver;
- `author`;
- `description`;
- `requested_permission_tier`;
- `on_enable`;
- `on_disable`;
- `on_unload`;
- `on_frame`.

Все ABI-структуры сначала обнуляются, затем получают `struct_size` и `api_version`:

```cpp
WotbModV3ClientInfo info = {};
WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
```

Для interface table поле `api_version` равно версии конкретного интерфейса. Для data/output structs используется ABI version, если профильный заголовок не требует иного.

## 4. Результаты и ошибки

| Код | Значение |
|---|---|
| `WOTBMOD_V3_OK` | Операция выполнена |
| `WOTBMOD_V3_E_INVALID_ARGUMENT` | Неверный pointer, size, enum, range, URI или структура |
| `WOTBMOD_V3_E_INVALID_HANDLE` | Handle имеет неверный тип, owner, slot или generation |
| `WOTBMOD_V3_E_NOT_SUPPORTED` | Реализация или backend недоступны |
| `WOTBMOD_V3_E_NOT_FOUND` | Объект, файл, key, slot, entity или service не найден |
| `WOTBMOD_V3_E_ALREADY_EXISTS` | ID, registration или object уже существует |
| `WOTBMOD_V3_E_WRONG_THREAD` | Вызов сделан не на требуемом thread |
| `WOTBMOD_V3_E_PERMISSION_DENIED` | Фактически выданный permission tier недостаточен |
| `WOTBMOD_V3_E_CLIENT_MISMATCH` | Client build/hash/binding pack не соответствует операции |
| `WOTBMOD_V3_E_OBJECT_DESTROYED` | Native object уже уничтожен |
| `WOTBMOD_V3_E_CONFLICT` | Конфликт hook, input binding, provider или override |
| `WOTBMOD_V3_E_CANCELLED` | Task, timer, HTTP, archive или callback отменён |
| `WOTBMOD_V3_E_BUFFER_TOO_SMALL` | Буфер мал; требуемый размер возвращён через `inout_size` |
| `WOTBMOD_V3_E_LIMIT_REACHED` | Превышен лимит handles, payload, files, nodes или bytes |
| `WOTBMOD_V3_E_BUSY` | Объект занят или transition уже выполняется |
| `WOTBMOD_V3_E_IO` | Ошибка файловой системы или потока |
| `WOTBMOD_V3_E_PARSE` | Ошибка JSON, YAML, manifest или descriptor |
| `WOTBMOD_V3_E_HASH_MISMATCH` | SHA-256 не совпал |
| `WOTBMOD_V3_E_SIGNATURE_INVALID` | Подпись проверена и невалидна |
| `WOTBMOD_V3_E_DEPENDENCY_MISSING` | Нет обязательной зависимости |
| `WOTBMOD_V3_E_INCOMPATIBLE` | Несовместимость версии, пакета или мода |
| `WOTBMOD_V3_E_CALLBACK_FAULT` | Callback завершился аварийно |
| `WOTBMOD_V3_E_PLATFORM` | Операция не поддерживается платформой/backend |
| `WOTBMOD_V3_E_TIMEOUT` | Истёк timeout |

После неуспешного вызова:

```cpp
WotbModV3ErrorInfo error = {};
WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
bootstrap->get_last_error(mod, &error);
```

`WotbModV3ErrorInfo` содержит code, thread role, owner mod, message и `context_json`.

## 5. Permission tiers

| Tier | Назначение |
|---|---|
| `WOTBMOD_V3_PERMISSION_SAFE` | Собственные данные, ресурсы, UI, settings, storage, публичные runtime services |
| `WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK` | Клиентские косметические изменения camera/HUD/hangar/replay/vehicle visual |
| `WOTBMOD_V3_PERMISSION_REVIEWED` | Изменение штатного UI, render callback, публичные battle entities, observed RPC metadata, projectile events, LeaveToHangar, allowlisted network |
| `WOTBMOD_V3_PERMISSION_UNSAFE` | Native pointers и address hooks |

`requested_permission_tier` не выдаёт разрешение. Loader вычисляет фактический grant по manifest, catalog policy и пользовательскому решению. Фактический tier читается через `wotbmod.permissions.get_granted_tier`.

Для package-bound V3 мода runtime устанавливает точный allowlist permission names из прошедшего preflight manifest. Профильная операция выполняется только при одновременном выполнении двух условий:

1. allowlist содержит требуемое имя либо его родительский namespace;
2. фактически выданный tier не ниже tier этой операции.

Например, доступ к собственным пакетным ресурсам требует `resources.mod`. Подмена `game://` дополнительно требует `resources.overlay.game` и tier `REVIEWED`; один `resources.mod` не разрешает overlay, а высокий tier без named permission также недостаточен.

Loose V3 DLL без package manifest работает в compatibility-режиме только с tier-проверкой. Это сохранено для старых установок и не эквивалентно package allowlist.

Named permissions ограничивают вызовы WotbMod API, но не являются OS sandbox. Windows выполняет `DllMain` внутри `LoadLibrary` до V3 entrypoint, а произвольный direct Win32-код нативной DLL вообще не проходит через API permission checks. Поэтому недоверенный native package нельзя считать изолированным только из-за manifest permissions; catalog review, hash policy и запрет загрузки остаются обязательной границей доверия.

Permission names проверяются через `permissions.query`. Методы `get_count` и `get_at` перечисляют известные runtime разрешения и их состояния `DENIED`, `GRANTED`, `REVIEW_REQUIRED`, `UNAVAILABLE`.

## 6. Game contexts

`WotbModV3GameContext` является bitmask:

- `WOTBMOD_V3_CONTEXT_LOADING`;
- `WOTBMOD_V3_CONTEXT_HANGAR`;
- `WOTBMOD_V3_CONTEXT_BATTLE`;
- `WOTBMOD_V3_CONTEXT_REPLAY`;
- `WOTBMOD_V3_CONTEXT_TRAINING`;
- `WOTBMOD_V3_CONTEXT_RESULTS`;
- `WOTBMOD_V3_CONTEXT_MOD_SCREEN`;
- `WOTBMOD_V3_CONTEXT_TEXT_INPUT`;
- `WOTBMOD_V3_CONTEXT_ALL`.

Текущий mask возвращает `core.get_context`. Interface может существовать, но быть недоступным в текущем context.

## 7. Thread roles

Роли:

- `UNKNOWN`;
- `MAIN`;
- `RENDER`;
- `AUDIO`;
- `WORKER`;
- `IO`.

Роль читается через `core.get_thread_role` или `async.thread_get_current_role`.
В текущем shipping host гарантирован только `RENDER` ingress, который вызывается
из `Present`/`DispatchFrame`; `dispatch_to_render_thread` ставит реальную
owner-bound задачу в эту очередь. Отдельные host ingress для `MAIN` и `AUDIO`
пока не установлены, поэтому `dispatch_to_main_thread` и
`dispatch_to_audio_thread` возвращают `WOTBMOD_V3_E_NOT_SUPPORTED` и не создают
task handle.

## 8. Handles и ownership

Все handles имеют тип `uint64_t`. Нулевое значение — `WOTBMOD_V3_INVALID_HANDLE`.

Публичные типы:

- mod;
- resource;
- UI control;
- UI slot;
- scene entity;
- scene;
- audio;
- sound event;
- render resource;
- camera;
- entity;
- projectile;
- archive;
- task;
- timer;
- HTTP request;
- hook;
- subscription;
- style override;
- input action;
- profiler span;
- diagnostic scope;
- capability subscription;
- lifecycle cleanup;
- event subscription;
- intermod export;
- intermod subscription;
- render callback;
- input binding;
- resource mount;
- audio override;
- projectile style;
- UI event subscription.

`wotbmod.handles`:

- `retain` — увеличить reference count;
- `release` — уменьшить reference count и запустить cleanup при последней ссылке;
- `get_info` — вернуть type, generation, reference count, alive и owner;
- `is_alive` — проверить актуальность.

Handle нельзя передавать другому моду как owned object. После уничтожения старый generation не становится валидным при повторном использовании slot.

## 9. Матрица интерфейсов

| Interface ID | C table | Версия | Минимальный tier | Контексты |
|---|---|---:|---|---|
| `wotbmod.core` | `WotbModV3CoreApiV1` | 1 | SAFE | ALL |
| `wotbmod.capabilities` | `WotbModV3CapabilitiesApiV1` | 1 | SAFE | ALL |
| `wotbmod.permissions` | `WotbModV3PermissionsApiV1` | 1 | SAFE | ALL |
| `wotbmod.handles` | `WotbModV3HandlesApiV1` | 1 | SAFE | ALL |
| `wotbmod.lifecycle` | `WotbModV3LifecycleApiV1` | 1 | SAFE | ALL |
| `wotbmod.hooks` | `WotbModV3HooksApiV1` | 1 | GAMEPLAY_TWEAK | ALL |
| `wotbmod.unsafe.native` | `WotbModV3UnsafeNativeApiV1` | 1 | UNSAFE | ALL |
| `wotbmod.events` | `WotbModV3EventsApiV1` | 1 | SAFE | ALL |
| `wotbmod.ui` | `WotbModV3UiApiV2` | 2 | SAFE, отдельные штатные операции требуют более высокий grant | ALL |
| `wotbmod.settings` | `WotbModV3SettingsApiV1` | 1 | SAFE | ALL |
| `wotbmod.storage` | `WotbModV3StorageApiV1` | 1 | SAFE | ALL |
| `wotbmod.input` | `WotbModV3InputApiV1` | 1 | SAFE | ALL |
| `wotbmod.vfs` | `WotbModV3VfsApiV1` | 1 | SAFE | ALL |
| `wotbmod.resources` | `WotbModV3ResourcesApiV1` | 1 | SAFE | ALL |
| `wotbmod.async` | `WotbModV3AsyncApiV1` | 1 | SAFE | ALL |
| `wotbmod.http` | `WotbModV3HttpApiV1` | 1 | SAFE table, request policy может требовать REVIEWED | ALL |
| `wotbmod.intermod` | `WotbModV3IntermodApiV1` | 1 | SAFE | ALL |
| `wotbmod.render` | `WotbModV3RenderApiV1` | 1 | SAFE table, render callback policy может требовать REVIEWED | ALL |
| `wotbmod.render.native` | `WotbModV3RenderNativeApiV1` | 1 | UNSAFE | ALL |
| `wotbmod.camera` | `WotbModV3CameraApiV1` | 1 | REVIEWED | HANGAR, BATTLE, REPLAY, TRAINING |
| `wotbmod.scene` | `WotbModV3SceneApiV1` | 1 | SAFE | ALL |
| `wotbmod.audio` | `WotbModV3AudioApiV2` | 2 | SAFE | ALL |
| `wotbmod.vehicle.visual` | `WotbModV3VehicleVisualApiV1` | 1 | GAMEPLAY_TWEAK | HANGAR, BATTLE, REPLAY, TRAINING |
| `wotbmod.gameplay.camera` | `WotbModV3GameplayCameraApiV1` | 1 | GAMEPLAY_TWEAK | HANGAR, BATTLE, REPLAY, TRAINING |
| `wotbmod.gameplay.hud` | `WotbModV3GameplayHudApiV1` | 1 | GAMEPLAY_TWEAK | BATTLE, REPLAY, TRAINING |
| `wotbmod.gameplay.hangar` | `WotbModV3GameplayHangarApiV1` | 1 | GAMEPLAY_TWEAK | HANGAR |
| `wotbmod.gameplay.replay` | `WotbModV3GameplayReplayApiV1` | 1 | GAMEPLAY_TWEAK | REPLAY |
| `wotbmod.entity.public` | `WotbModV3EntityPublicApiV1` | 1 | REVIEWED | BATTLE, REPLAY, TRAINING |
| `wotbmod.bigworld.rpc` | `WotbModV3BigWorldRpcApiV1` | 1 | REVIEWED | BATTLE, REPLAY, TRAINING |
| `wotbmod.projectile` | `WotbModV3ProjectileApiV1` | 1 | REVIEWED | BATTLE, REPLAY, TRAINING |
| `wotbmod.data.yaml` | `WotbModV3YamlApiV1` | 1 | SAFE | ALL |
| `wotbmod.archive` | `WotbModV3ArchiveApiV1` | 1 | SAFE | ALL |
| `wotbmod.loaders` | `WotbModV3LoadersApiV1` | 1 | SAFE | ALL |
| `wotbmod.client` | `WotbModV3ClientApiV1` | 1 | SAFE table; LeaveToHangar требует policy/binding | ALL |
| `wotbmod.client.device` | `WotbModV3DeviceApiV1` | 1 | SAFE | ALL |
| `wotbmod.diagnostics` | `WotbModV3DiagnosticsApiV2` (V1 prefix) | 2 | SAFE | ALL |
| `wotbmod.devtools` | `WotbModV3DevtoolsApiV2` (V1 prefix) | 2 | SAFE | ALL |
| `wotbmod.manifest` | `WotbModV3ManifestApiV1` | 1 | SAFE | ALL |
| `wotbmod.catalog` | `WotbModV3CatalogApiV1` | 1 | SAFE | ALL |
| `wotbmod.content` | `WotbModV3ContentApiV1` | 1 | SAFE | ALL |
| `wotbmod.ui.read` | `WotbModV3UiApiV4` | 4 | SAFE (`ui`), game-owned control требует `ui.modify.game` | ALL |
| `wotbmod.camera.state` | `WotbModV3CameraApiV2` | 2 | REVIEWED (`camera.battle.read`) | HANGAR, BATTLE, REPLAY, TRAINING |
| `wotbmod.audio.intercept` | `WotbModV3AudioApiV3` | 3 | SAFE (`audio.events`) | ALL |
| `wotbmod.scene.enumerate` | `WotbModV3SceneApiV2` | 2 | REVIEWED (`game.entity.public`) | ALL |
| `wotbmod.tracer` | `WotbModV3TracerApiV1` | 1 | REVIEWED (`visible.projectile.events`) | BATTLE, REPLAY, TRAINING |
| `wotbmod.ges` | `WotbModV3GesApiV1` | 1 | SAFE (`ges.observe`), `publish` — REVIEWED (`ges.publish`) | ALL |
| `wotbmod.session.cluster` | `WotbModV3SessionClusterApiV1` | 1 | SAFE (`session.cluster.read`), `change` — REVIEWED (`session.cluster.change`) | ALL (`change` — только HANGAR) |

Последние пять строк объявлены 2026-08-16 и пока **не имеют backend**: каждый
их slot возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`, а capability status у них —
`UNAVAILABLE`. Подробности — раздел 58.

## 10. `wotbmod.core`

Методы:

- `log` — записать сообщение уровня TRACE, DEBUG, INFO, WARNING, ERROR или FATAL;
- `get_context` — получить текущий game-context mask;
- `get_thread_role` — получить роль текущего thread;
- `get_frame_index` — получить runtime frame index;
- `get_game_directory` — скопировать путь каталога игры;
- `get_mod_data_directory` — scoped writable data path;
- `get_mod_cache_directory` — scoped cache path;
- `get_mod_config_directory` — scoped config path.

Copy-path методы используют `char* buffer` и `uint32_t* inout_size`. Допустим двухпроходный вызов для определения размера.

## 11. `wotbmod.capabilities`

Capability описывается `WotbModV3CapabilityInfo`: name, interface version, status, allowed contexts, permission tier, unavailable reason.

Методы:

- `get_count`;
- `get_at`;
- `query`;
- `subscribe`;
- `unsubscribe`.

Статусы:

- `AVAILABLE`;
- `UNAVAILABLE`;
- `CLIENT_MISMATCH`;
- `PERMISSION_DENIED`;
- `CONTEXT_RESTRICTED`;
- `DEGRADED`.

Capability может измениться после device lost/restored, смены context, смены binding pack или revoke permission. Подписка owner-bound и снимается при выгрузке.

## 12. `wotbmod.permissions`

Методы:

- `get_granted_tier`;
- `query`;
- `get_count`;
- `get_at`.

`WotbModV3PermissionInfo` возвращает permission name, tier, state и reason. Проверка permission до операции улучшает UX, но профильный вызов всё равно должен проверять grant повторно.

## 13. `wotbmod.lifecycle`

Состояния:

- `LOADED`;
- `ENABLED`;
- `DISABLED`;
- `UNLOADING`;
- `UNLOADED`.

Transitions:

- `PRELOAD`;
- `LOADED`;
- `ENABLED`;
- `DISABLED`;
- `UNLOADING`;
- `UNLOADED`.

Методы:

- `get_current` — получить handle текущего мода;
- `get_info` — получить ID, state, tier и hot-reload status;
- `get_install_path`;
- `get_resource_path`;
- `get_data_path`;
- `get_cache_path`;
- `get_config_path`;
- `register_cleanup`;
- `unregister_cleanup`;
- `request_enable`;
- `request_disable`;
- `request_reload`;
- `can_hot_reload`.

Cleanup callback получает причину `RELEASED`, `MOD_DISABLED`, `MOD_UNLOADING` или `RUNTIME_SHUTDOWN`.

`get_install_path` возвращает канонический каталог DLL entrypoint. Для
package-мода `get_resource_path` возвращает корень смонтированного package,
переданный loader до входа в мод; для loose DLL он совпадает с install path.
Пути data/cache/config остаются отдельными owner-scoped каталогами runtime и
не вычисляются относительно текущего рабочего каталога процесса.

Порядок штатной выгрузки: остановить приём новой работы → отменить owned work/hooks → дождаться активных callbacks → `DISABLED`/`on_disable` → освободить owned handles → `UNLOADING` → `on_unload` → `UNLOADED`.

## 14. `wotbmod.hooks`

Hook modes:

- `BEFORE`;
- `AFTER`;
- `AROUND`;
- `REPLACE`;
- `OBSERVE`.

Методы:

- `create_symbol` — создать hook по стабильному symbol/binding;
- `create_vtable` — создать hook vtable slot объекта;
- `enable`;
- `disable`;
- `remove`;
- `set_priority`;
- `run_before`;
- `run_after`;
- `get_original`;
- `call_next`;
- `get_info`;
- `get_owner`;
- `get_status`;
- `get_chain`;
- `get_conflicts`;
- `enumerate`.

`WotbModV3HookCreateInfo` задаёт mode, priority, flags и detour. `ALLOW_PENDING` позволяет зарегистрировать hook до готовности backend; это не означает, что hook уже установлен. `get_status` различает `PENDING_BACKEND`, `DISABLED`, `ENABLED`, `CONFLICT`, `REMOVED`, `FAILED`.

Начальный `priority` сохраняется как query metadata и используется только для
стабильного представления `get_chain`; MinHook не предоставляет native
chain-dispatcher. Поэтому `set_priority`, `run_before` и `run_after` возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED` без изменения metadata или порядка исполнения.

В shipping loader для exact build `11.19.0.834` подключён проверенный MinHook
backend. Он реально выполняет `create`, `enable`, `disable`, `remove` и
`get_original` для одиночных `AROUND`/`REPLACE` hooks. Перед включением статус
равен `DISABLED`, после успешного `enable` — `ENABLED`; owned cleanup сначала
выключает и удаляет native hook. `OBSERVE` и `AFTER` работают на 22 именах,
за которыми стоят функции с собственным детуром лоадера: callback мода с
native-сигнатурой цели вызывается изнутри этого детура до (`OBSERVE`) или
после (`AFTER`) оригинала, результат callback отбрасывается, `get_original`
для такой записи отвечает `E_NOT_SUPPORTED`. На остальных именах они
отклоняются с текстом «signature of this target is not described», `BEFORE` —
с текстом «BEFORE has no cancel protocol yet»; с явным `ALLOW_PENDING` запись
остаётся `PENDING_BACKEND`. Таблица сигнатур и границы — в
[`HOOK_MODES_RU.md`](HOOK_MODES_RU.md).

Операции дополнительно проверяют точный manifest grant. Обычный symbol hook
требует `hooks.symbol` и tier REVIEWED. Управляемые targets
`Camera::setFOV`, `SniperCamera::exit`, `PostProcess::apply` требуют
`gameplay.tweak.camera`; HUD targets требуют `gameplay.tweak.hud`.
Решение принимается по адресу, в который имя разрешилось, а не по написанию:
если два опубликованных имени указывают на одну функцию (например,
`Camera::setFOV` и `DAVA::Camera::SetFovY`), они стоят одинаково, и выбор
другого написания не обходит `gameplay.tweak.camera`.
`create_vtable` работает с raw object address и поэтому требует
`native.hook.address` и tier UNSAFE — наличие только `hooks.symbol` его не
открывает.

`resolve_symbol` публикует 35 отревершенных целей под 42 именами: часть
целей доступна под несколькими эквивалентными написаниями (например,
`Camera::setFOV` и `DAVA::Camera::SetFovY` указывают на одну функцию;
`GameCamera::ctor`/`GameCamera::dtor`, `BWEntity::ctor`/`BWEntity::dtor`,
`TracerManager::ctor` и `CameraModeChanged` — короткие имена из binding list,
которые принимаются наравне с каноническими C++-именами
`GameCamera::GameCamera`/`~GameCamera`, `BWEntity::BWEntity`/`~BWEntity`,
`TracerManager::TracerManager` и `GES::Avatar::CameraModeChanged`). Оба
написания обязаны разрешаться: `test_hook_symbol_table.py` проверяет, что
каждое имя из binding list разрешимо через `resolve_symbol`.

Не публикуются намеренно:
- 13 якорей на данные (vtable, синглтоны) — они не исполняемые;
- 5 CRT-заготовок (`operator new` и подобные) — хук на них бьёт по всему
  процессу, а не по цели мода;
- 6 функций, чьё каноническое имя не установлено реверсом — публиковать
  выдуманное имя опаснее, чем не публиковать ничего.

Неизвестное имя мод наблюдает как `WOTBMOD_V3_E_NOT_SUPPORTED` от
`create_symbol` с сообщением «symbol is not present in the reviewed binding
pack»: `WOTBMOD_V3_E_NOT_FOUND` — это внутреннее значение backend'а, runtime
его поглощает и наружу не отдаёт (с флагом `ALLOW_PENDING` тот же вызов вместо
ошибки создаёт hook в статусе `PENDING_BACKEND`). При несовпадении отпечатка
клиента — `WOTBMOD_V3_E_CLIENT_MISMATCH`.

`get_original` возвращает реальный trampoline для установленного
`AROUND`/`REPLACE` hook; разработчик вызывает его с точной native-сигнатурой
target. Универсальный `call_next` с byte buffers не может безопасно восстановить
регистры, calling convention и C++ object ABI, поэтому в текущей версии честно
возвращает `E_NOT_SUPPORTED`. Несколько native detours на одном target
отклоняются как `E_CONFLICT`; priority/ordering остаются доступны для metadata
chain и будущих typed hooks.

Shipping loader не предоставляет legacy raw process/RVA/export/pattern API и не устанавливает legacy raw hooks. Они доступны только в отдельно собранном developer-unsafe варианте; один runtime option или env opt-in не меняет shipping policy.

## 15. `wotbmod.unsafe.native`

Единственный метод:

- `create_address_hook` — hook по raw native address.

Требуется UNSAFE и точный named grant `native.hook.address`. Backend передаётся
runtime только при полном совпадении client fingerprint; target дополнительно
должен находиться внутри executable image текущей игры. При mismatch или адресе
из другого модуля hook не устанавливается. Этот V3 путь не включает legacy raw
process/RVA/pattern API.

## 16. `wotbmod.events`

Методы:

- `subscribe`;
- `unsubscribe`;
- `set_priority`;
- `post`;
- `stop_propagation`;
- `get_dispatch_info`;
- `get_thread`;
- `get_timestamp`;
- `get_context`.

Subscription использует topic pattern, priority и флаг получения system events. Event содержит dispatch token, publisher, timestamp, context, thread role, flags, topic и payload.

Флаги:

- `STOPPABLE`;
- `MUTABLE_PAYLOAD`;
- `SYSTEM`.

`subscribe` проверяет наличие реального publisher до создания token. Exact
topic или wildcard, который не может совпасть ни с одним доступным источником,
возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`, а `out_token` остаётся invalid.
System-only pattern также требует `receive_system_events != 0`; пользовательские
`mod.<id>.*` и общий `*` остаются доступны через `post`. Wildcard поддерживается
только как завершающий `*`.

Доступность системных topics вычисляется динамически для текущего запуска.
Loader передаёт runtime маску только тех poll/native ingress, которые реально
установились и прошли проверку текущего client fingerprint. Поэтому при нулевой
маске `wotbmod.gameplay.*` возвращает `E_NOT_SUPPORTED`, а широкий
`wotbmod.*` остаётся доступен благодаря runtime-owned `frame`, lifecycle и
shutdown topics. Частичная установка открывает только exact topics и wildcard,
которые могут совпасть хотя бы с одним подтверждённым источником. UI screen и
Scene topics открываются только при успешной установке их собственного
poll/hook path; `local_shell_fired` дополнительно требует подтверждённый native
shell hook и рабочий local-vehicle registry ingress.

Текущие системные topics с реальным runtime/native publish path:

- `wotbmod.client.shutting_down`;
- `wotbmod.frame.update`;
- `wotbmod.ui.screen_changed`;
- `wotbmod.ui.input`;
- `wotbmod.scene.activated`;
- `wotbmod.scene.deactivated`;
- `wotbmod.battle.entered`;
- `wotbmod.battle.started`;
- `wotbmod.battle.ended`;
- `wotbmod.battle.left`;
- `wotbmod.vehicle.local.changed`;
- `wotbmod.vehicle.local.created`;
- `wotbmod.vehicle.local.destroyed`;
- `wotbmod.vehicle.spawned`;
- `wotbmod.vehicle.despawned`;
- `wotbmod.vehicle.health_changed`;
- `wotbmod.vehicle.damaged`;
- `wotbmod.gameplay.damage_received`;
- `wotbmod.vehicle.destroyed`;
- `wotbmod.vehicle.spotted`;
- `wotbmod.vehicle.unspotted`;
- `wotbmod.vehicle.killed` (подтверждённое сервером уничтожение: в
  `payload.kill` поля `victim_id`, `killer_id` (0, если никто не засчитан),
  `assist_id`, `reason` (код причины арены), `ammo_bay_exploded`;
  `primary_entity_id` = жертва, `other_entity_id` = убийца; id совпадают с
  `public_id` в `wotbmod.entity.public`; источник
  `WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED`, хук `ClientArena::VehicleKilled`;
  в Lua доступен как `TOPIC_VEHICLE_KILLED`);
- `wotbmod.gameplay.shot_fired`;
- `wotbmod.gameplay.shell_hit`;
- `wotbmod.gameplay.reload_state_changed`;
- `wotbmod.gameplay.ammo_changed`;
- `wotbmod.gameplay.aim_target_changed`;
- `wotbmod.gameplay.camera_mode_changed`;
- `wotbmod.gameplay.sniper_entered`;
- `wotbmod.gameplay.sniper_exited`;
- `wotbmod.gameplay.local_shell_fired`;
- `wotbmod.entity.public.added`;
- `wotbmod.entity.public.updated`;
- `wotbmod.entity.public.removed`;
- `wotbmod.bigworld.rpc.observed`;
- `wotbmod.mod.preload`;
- `wotbmod.mod.loaded`;
- `wotbmod.mod.enabled`;
- `wotbmod.mod.disabled`;
- `wotbmod.mod.unloading`;
- `wotbmod.mod.unloaded`;
- `wotbmod.capabilities.changed` — публикуется при каждом успешном
  `WotbModV3Runtime_SetCapability`. Payload: одна структура
  `WotbModV3CapabilityInfo`. Источник не требуется — событие внутреннее и
  приходит в том числе при несовпадающем отпечатке клиента.

`OK` у подписки означает, что publish path существует; событие всё равно может
не возникнуть в конкретной сессии, а native gameplay topics зависят от
совместимости и успешной установки соответствующего hook.

Пять дополнительных topics выводятся только из уже подтверждённых событий:
`local.created` из назначения ненулевой local vehicle, `local.destroyed` из
уничтожения текущей local vehicle, `damage_received` из `vehicle.damaged` для
текущего local ID, а `sniper_entered`/`sniper_exited` из переходов
`camera_mode_changed`. Payload остаётся исходным типизированным client-event
envelope. `damage_dealt` не выводится: у health ingress нет доказанного
источника атакующего, поэтому приписывать урон локальному игроку было бы
недостоверно.

Следующие ABI topics пока зарезервированы, но publisher для них не установлен;
exact subscription и wildcard, совпадающий только с ними, возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED`:

- `wotbmod.client.ready`;
- `wotbmod.game.state_changed`;
- `wotbmod.frame.fixed_update`;
- `wotbmod.ui.root_ready`;
- `wotbmod.ui.root_destroyed`;
- `wotbmod.ui.scale_changed`;
- `wotbmod.ui.safe_area_changed`;
- `wotbmod.hud.ready`;
- `wotbmod.hud.destroyed`;
- `wotbmod.hud.reticle_changed`;
- `wotbmod.battle.countdown_started`;
- `wotbmod.vehicle.local.appearance_ready`;
- `wotbmod.audio.device_changed`;
- `wotbmod.audio.sound_created`;
- `wotbmod.audio.sound_finished`;
- `wotbmod.input.device_changed`;
- `wotbmod.input.action`;
- `wotbmod.gameplay.fov_changed`;
- `wotbmod.gameplay.zoom_changed`;
- `wotbmod.gameplay.camera_transition_started`;
- `wotbmod.gameplay.camera_transition_ended`;
- `wotbmod.gameplay.postprocessing_toggled`;
- `wotbmod.replay.speed_changed`;
- `wotbmod.replay.seek`;
- `wotbmod.hangar.background_changed`;
- `wotbmod.gameplay.damage_dealt`;
- `wotbmod.gameplay.sixth_sense_triggered`;
- `wotbmod.gameplay.minimap_marker_added`;
- `wotbmod.gameplay.session_stats_updated`;
- `wotbmod.gameplay.visible_tracer_created`;
- `wotbmod.gameplay.visible_tracer_destroyed`.

Render lifecycle topics не входят в этот reserved-список: loader сравнивает
состояние, реально наблюдаемое в D3D11 Present ingress, и публикует
`device_created`, `device_lost`, `device_restored`, `swapchain_resized` и
`backend_changed`. Их public payload содержит backend, availability,
изменившиеся компоненты, frame index и viewport, но не содержит COM pointers.

`post` публикует пользовательский topic от имени мода. Мод не может выставить системный publisher или подделать `SYSTEM`.

## 17. `wotbmod.settings`

Типы settings:

- BOOL;
- INT;
- FLOAT;
- STRING;
- ENUM;
- COLOR;
- KEYBIND;
- FILE;
- FOLDER;
- TITLE;
- BUTTON;
- CUSTOM.

Definition включает key, title, description, default, min/max/step, enum values, visibility/enabled expressions, flags и platform mask.

Методы:

- `register_schema`;
- `register_preset`;
- `get_schema_version`;
- `get_bool`;
- `get_int`;
- `get_float`;
- `get_string`;
- `get_color`;
- `set_bool`;
- `set_int`;
- `set_float`;
- `set_string`;
- `set_color`;
- `reset`;
- `reset_all`;
- `apply_preset`;
- `subscribe`;
- `unsubscribe`;
- `run_migration`.

Schema version должна расти монотонно. Migration callback получает old/new version. Значения scoped по owner mod.

## 18. `wotbmod.storage`

Методы:

- `get_json`;
- `set_json`;
- `get_bytes`;
- `set_bytes`;
- `erase`;
- `contains`;
- `flush`;
- `begin_transaction`;
- `transaction_set_json`;
- `transaction_set_bytes`;
- `transaction_erase`;
- `commit`;
- `rollback`;
- `get_path`.

Path kinds:

- DATA;
- CONFIG;
- CACHE;
- TEMP.

Ключи не являются raw paths. Transaction token принадлежит моду; незавершённая transaction откатывается при cleanup.

## 19. `wotbmod.input`

Input devices:

- keyboard;
- mouse;
- gamepad;
- touch.

Action имеет ID, display name, description, button/axis type, context mask и default bindings.

Методы:

- `register_action`;
- `unregister_action`;
- `set_contexts`;
- `get_bindings`;
- `set_bindings`;
- `subscribe`;
- `is_action_down`;
- `is_action_pressed`;
- `get_axis`;
- `capture_begin`;
- `capture_end`;
- `find_conflicts`.

Bindings содержат device, code, modifiers и scale. `find_conflicts` учитывает overlapping contexts.

Loader подключает native ingress из Win32 WndProc и нормализованной DAVA input
очереди. Hook только записывает bounded event; action matching, capture,
polling и callbacks выполняются из frame pump. Button transitions сохраняют
down/pressed state, axis события одного chord coalesce-ятся, а focus/device
reset очищает залипшие states. Callback подчиняется общему per-mod budget;
device-specific поля, не подтверждённые native ABI, не добавляются в payload.

## 20. `wotbmod.vfs`

URI пространства:

- `mod://<mod-id>/<path>` — ресурсы пакета мода;
- `game://<path>` — разрешённые ресурсы клиента;
- scoped writable paths выдаются storage/core, а не произвольным URI.

Методы:

- `get_namespace`;
- `normalize_uri`;
- `mount_package`;
- `mount_overlay`;
- `unmount`;
- `resolve`;
- `open`;
- `read`;
- `list`;
- `stat`;
- `watch`;
- `get_providers`;
- `set_provider_priority`;
- `get_conflicts`.

Provider имеет owner, priority, kind, provider ID и target URI. Доступ к namespace ресурсов мода требует `resources.mod`. Создание, изменение priority и снятие provider, который подменяет `game://`, дополнительно требует `resources.overlay.game` и tier `REVIEWED`. Overlay не даёт права записи в game files; он меняет разрешение URI внутри runtime. Нормализация отвергает traversal и выход из package root.

`watch` использует content fingerprint с SHA-256 и различает create/modify/delete.
Одинаковое состояние дедуплицируется. Watcher owner-bound, ограничен лимитами и
удаляется при disable/unload. Для resource watch изменение сначала загружается
и проверяется; успешная версия заменяет текущую транзакционно, а parse/hash/IO
ошибка сохраняет last-good resource и сообщает invalidation без частично
применённого объекта.

## 21. `wotbmod.resources`

Resource types:

- binary;
- text;
- image;
- audio;
- model;
- YAML;
- JSON.

Backing:

- NONE;
- RAW_VFS_BYTES;
- NATIVE_CLIENT_OBJECT.

Методы:

- `load`;
- `load_async_ex`;
- `get_info`;
- `copy_data`;
- `retain`;
- `release`;
- `preload_group`;
- `unload_group`;
- `reload`;
- `watch`;
- `get_total_memory_usage`.

`WotbModV3ResourceLoadDesc` задаёт expected type, flags, max bytes, URI, group и optional SHA-256. Флаг `REQUIRE_NATIVE_CLIENT` требует реальный client resource backend; при его отсутствии возвращается `E_NOT_SUPPORTED`, а raw bytes не выдаются как fake native object.

File-backed resource хранит URI, type, bytes/memory usage и SHA-256 fingerprint.
`reload` и watcher используют тот же bounded loader, поэтому обновление
атомарно с точки зрения handle: новый payload становится видимым только после
полной проверки.

`load_async_ex` создаёт реальный owner-bound resource handle и выполняет I/O
через общий worker runtime, а не через отдельный скрытый thread на каждый
ресурс. Состояния, progress/error и payload синхронизированы; disable/unload
отменяет queued work и удерживает объект до завершения уже запущенной
операции. Reload публикует новый payload только после полной проверки и при
ошибке или исключении сохраняет предыдущий bytes/hash/URI/type/backing.
Запрос с `REQUIRE_NATIVE_CLIENT` без native backend завершается состоянием
`FAILED` и кодом `E_NOT_SUPPORTED`, а не выдаёт raw bytes за native object.

## 22. `wotbmod.data.yaml`

Node types:

- NULL;
- MAP;
- SEQUENCE;
- STRING;
- BOOL;
- INT;
- FLOAT.

Методы:

- `parse`;
- `parse_uri`;
- `get_root`;
- `get_type`;
- `get_size`;
- `map_get`;
- `sequence_get`;
- `get_string`;
- `get_bool`;
- `get_int`;
- `get_float`.

Parser принимает `WotbModV3YamlLimits`: max depth, max nodes и max bytes. Node ID действует только внутри document handle. Document освобождается через handles/resource ownership. Это реальный bounded generic minimal-YAML backend; он не выдаётся за объект DAVA YAML.

`load_dava_yaml` использует отдельный loader-private typed provider только если
тот объявил полную capability-группу YAML. Native document экспортируется в
bounded UTF-8, немедленно освобождается и повторно разбирается в обычный
owner-owned V3 document. Provider token и указатель DAVA не покидают loader;
public handle остаётся валиден после снятия provider-а. Exact 11.19 provider
реализует этот route; parse/export/release подтверждены live на fingerprint
`11.19.0.834`.

## 23. `wotbmod.archive`

Методы:

- `open_directory`;
- `open_package_file`;
- `get_entry_count`;
- `get_entry`;
- `read_entry`;
- `extract_entry`;
- `extract_all`;
- `get_sha256`;
- `verify_sha256`;
- `cancel`.

Limits:

- max depth;
- max files;
- max total unpacked bytes;
- max single-file bytes;
- optional expected SHA-256.

Extraction блокирует absolute/device/UNC paths, drive prefixes, `..`, symlink
escape, Windows case-fold duplicate/prefix collisions и выход за destination
root. Извлечение разрешено только в owner-scoped `data://` или `cache://`.

`open_directory` является реальным hardened backend: он проверяет owned root,
limits, symlinks, размеры и SHA-256. `open_package_file` поддерживает
детерминированный безопасный профиль package-архива: single-disk ZIP32,
store-only entries, строгая сверка central/local headers, CRC32 каждого entry,
package/entry SHA-256 и только regular files/directories. Data descriptor,
encryption, ZIP64, compression, multi-disk, symlink/non-regular entries и legacy
non-ASCII names отклоняются с явным result code; они не трактуются как пустой
или успешно открытый archive.

`open_dava_archive` имеет loader-private typed provider route. При наличии
полной capability-группы loader проверяет entry count, пути, глубину, collisions,
flags и размеры, копирует все entries в bounded memory-backed snapshot и сразу
освобождает native archive. `read_entry`, `extract_entry` и hashes после этого
не зависят от provider lifetime. Exact 11.19 ResourceArchive adapter реализует
этот route; open/enumerate/read/release подтверждены live на fingerprint
`11.19.0.834`.

## 24. `wotbmod.loaders`

Backends:

- UTF8_TEXT;
- BINARY;
- MINIMAL_YAML;
- DAVA_YAML;
- DVPL;
- DAVA_ARCHIVE.

Методы:

- `load_text_utf8`;
- `load_binary`;
- `load_yaml`;
- `load_dava_yaml`;
- `unpack_dvpl`;
- `open_dava_archive`;
- `get_backend_info`.

Minimal YAML и raw VFS loaders не зависят от адресов игры. Portable DVPL
decoder проверяет footer, compressed/uncompressed sizes и CRC, распаковывает
type `0`, type `1` LZ4 и type `2` LZ4HC. Type `4` streaming LZ4 требует
guarded native stream backend и возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`.

Exact-build provider для `11.19.0.834` рекламирует native DAVA YAML/archive
capabilities только через private registry. `load_dava_yaml` и
`open_dava_archive` возвращают обычные public V3 handles на snapshots, а не
native object pointers. Если capability не установлена или adapter отказал,
вызов fail-closed с result code; если он прошёл в synthetic/host среде, это не
становится live PASS. Для модов по-прежнему доступны portable bounded minimal
YAML, directory/ZIP archive и DVPL. Полный внутренний контракт описан в
`DAVA_NATIVE_PRIVATE_ABI_RU.md`.

## 25. `wotbmod.async`

Task states:

- QUEUED;
- RUNNING;
- COMPLETED;
- FAILED;
- CANCELLED.

Методы:

- `task_submit`;
- `task_cancel`;
- `task_get_info`;
- `task_get_state`;
- `task_get_progress`;
- `task_set_progress`;
- `task_set_result`;
- `task_get_result`;
- `task_is_cancellation_requested`;
- `dispatch_to_main_thread`;
- `dispatch_to_render_thread`;
- `dispatch_to_audio_thread`;
- `pump_current_thread`;
- `timer_create`;
- `timer_cancel`;
- `timer_pause`;
- `timer_resume`;
- `timer_get_info`;
- `thread_get_current_role`;
- `thread_is_main`.

Task result ограничен `WOTBMOD_V3_MAX_TASK_RESULT`. Task/timer owner-bound. При disable runtime выставляет cancellation и не допускает новый callback мода после callback barrier.

Worker task реально исполняется пулом runtime. Completion и timer callback можно
назначить на `WORKER` либо на автоматически прокачиваемый `RENDER` ingress.
Назначение callback на `MAIN`/`AUDIO` возвращает
`WOTBMOD_V3_E_NOT_SUPPORTED` до создания handle: очередь без host pump не
выдаётся за работающую. `pump_current_thread` выполняет только очередь уже
назначенной текущему потоку роли и не подменяет отсутствующий shipping ingress.

## 26. `wotbmod.http`

Методы:

- `request_create`;
- `request_set_method`;
- `request_set_url`;
- `request_set_header`;
- `request_set_body`;
- `request_set_timeout`;
- `request_set_max_response_size`;
- `request_send_async`;
- `request_cancel`;
- `request_get_info`;
- `response_get_status`;
- `response_get_header`;
- `response_get_body`.

Default max response: 8 MiB. Absolute max: 64 MiB.

Текущая Windows-сборка использует реальный асинхронный WinHTTP transport.
Разрешён только `https://host[:443]/...`; proxy, redirect, cookies, HTTP auth,
локальные/private/link-local адреса и произвольный порт запрещены. До соединения
все DNS-ответы проверяются, выбранный публичный IP pin-ится в WinHTTP, а после
соединения сверяется фактический remote endpoint. TLS-проверка Windows не
отключается.

Отправка требует либо REVIEWED permission `network.http`, либо точный REVIEWED
grant `network:https://host`. Совпадение host регистронезависимое, но строгое:
grant для `api.example.com` не покрывает `api.example.com.evil`. Request body,
headers, response, timeout и очереди имеют жёсткие лимиты; cancel/disable/unload
останавливают transport, а completion доставляется через гарантированный
`RENDER` ingress на следующем `DispatchFrame` только пока owner-мод жив. Callback
не выполняется на произвольном WinHTTP/worker thread.

## 27. `wotbmod.intermod`

Методы:

- `mod_find`;
- `mod_is_loaded`;
- `mod_get_version`;
- `mod_get_dependency`;
- `export_interface`;
- `unexport_interface`;
- `import_interface`;
- `enumerate_interfaces`;
- `message_publish`;
- `message_subscribe`;
- `message_unsubscribe`.

Экспортируемый service имеет stable service ID, interface version, table size, provider mod и export token. Consumer обязан проверить version/table size. Imported table недействительна после unload provider; lifecycle event или dependency contract должен остановить вызовы.

`mod_get_dependency` видит только зависимости, объявленные текущим модом в
manifest и переданные loader до entrypoint. Для найденной required/optional
зависимости возвращаются согласованная версия и handle provider. Отсутствующая
optional dependency возвращает `E_NOT_FOUND` с `out_optional == 1`;
необъявленные установленные моды через этот метод не раскрываются. Runtime
копирует descriptor-ы и ограничивает список 128 записями.

Message bus ограничивает payload значением `WOTBMOD_V3_MAX_INTERMOD_PAYLOAD`.

## 28. `wotbmod.ui` V3

UI API работает с owned controls и содержит ABI для named client slots. Native UIControl создаётся или изменяется только при наличии UI host backend.

V3 сохраняет полный бинарный prefix V2 и добавляет `get_active_screen` и
`control_get_snapshot`. Текущий adapter оборачивает настоящий active-screen
root, выполняет native find/parent/child traversal, обновляет geometry/state и
помечает штатные controls флагом `WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED`.
Изменение game-owned control требует reviewed permission `ui.modify.game`;
API-owned controls остаются owner-scoped.

Layout descriptor хранится runtime и при `layout_invalidate` вычисляет
геометрию детей для `ABSOLUTE`, `HORIZONTAL`, `VERTICAL`, `GRID`, `FLEX` и
`OVERLAY`. Учитываются padding, margin, min/max size, weight, spacing,
direction и alignment. Если native geometry setter одного ребёнка завершается
ошибкой, уже изменённые дети откатываются к предыдущей геометрии.

Pointer/click/drag/scroll события поступают из native input queue и вызываются
только из loader frame pump под общим callback budget. Это не вызов мода из
WndProc/hook.

API-owned controls создаются во время работы для всех значений
`WotbModV3UiControlType`. Native bridge отображает безопасные типы на
проверенные DAVA классы: `UIStaticText`, `UIButton`, `UITextField`,
`UIScrollView`/`UIScrollViewContainer` и базовый `UIControl`. Text, image,
button, фон, font/color/alignment/opacity и texture создаются из descriptor и
могут изменяться после создания. Изменение пересобирает только native resource
этого control, сохраняя публичный handle, parent/children и subscriptions.
`control_clone` создаёт отдельный native resource: clone не является вторым
handle на исходный объект.

Для `SCROLL_VIEW` и `LIST` дочерние controls присоединяются к внутреннему
`UIScrollViewContainer` размером под динамическое содержимое. Поэтому Lua может
создать неизвестное заранее число строк и наполнить их текстом после clone.

Legacy template-backed контейнер также поддерживается: для
`CONTROL_CONTAINER` непустой `texture_uri` задаёт доверенный `mod://` или
`game://` YAML, а `id` — имя top-level control. Это дополнительный путь;
runtime controls не требуют от автора YAML.

Граница намеренно безопасная: Lua выбирает один из 11 frozen control types, а
loader-private `wotb.dava` возвращает только typed safe handles для reviewed
classes. Произвольное имя C++ DAVA-класса, сырые pointers, FFI и произвольный
component payload не публикуются. Native DAVA animation/effect components не
инжектируются; анимация делается обновлением разрешённых свойств из `on_frame`.

### Дерево и идентификация

- `control_create` — создать control выбранного типа;
- `control_clone` — клонировать существующий control;
- `control_destroy`;
- `control_add_child`;
- `control_remove_child`;
- `control_set_parent`;
- `control_get_parent`;
- `control_get_child_count`;
- `control_get_child_at`;
- `control_set_id`;
- `control_get_id`;
- `control_find_by_id`;
- `control_find_by_path`;
- `control_get_owner_mod`;
- `control_is_alive`.

### Named slots

- `slot_find`;
- `slot_attach`;
- `slot_detach`;
- `slot_enumerate`.

Slot представляет конкретную разрешённую точку расширения существующего UI, а не весь active-screen root. Текущий binding pack не разрешает точные native extension points для `hangar.top_bar.*`, `battle.hud.*` и остальных объявленных semantic IDs:

- `slot_find` для известного ID возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` и записывает `WOTBMOD_V3_INVALID_HANDLE`;
- неизвестный ID возвращает `WOTBMOD_V3_E_NOT_FOUND`;
- `slot_enumerate` возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` и не вызывает visitor;
- `slot_attach`/`slot_detach` не используют generic active-screen fallback.

Наличие этих function slots сохраняет ABI для будущего точного native resolver,
но не означает доступность named slots в текущем клиенте. Для инспекции всего
active-screen tree используется V3 `get_active_screen` плюс tree methods;
доступ и mutation штатного control зависят от `ui.modify.game`, context и
binding pack.

### Геометрия и layout

- `control_set_position`;
- `control_get_position`;
- `control_set_size`;
- `control_get_size`;
- `control_set_anchor`;
- `control_set_pivot`;
- `control_set_margin`;
- `control_set_padding`;
- `control_set_min_size`;
- `control_set_max_size`;
- `control_set_z_order`;
- `layout_set`;
- `layout_set_type`;
- `layout_set_direction`;
- `layout_set_spacing`;
- `layout_set_alignment`;
- `layout_set_weight`;
- `layout_invalidate`;
- `get_scale_factor`;
- `get_safe_area`;
- `get_viewport_size`.

`get_viewport_size` возвращает размер client area окна swap chain, а не render
backbuffer и не логический размер DAVA active-screen. Это важно в оконном
режиме и при UI scale: например, backbuffer и active-screen могут иметь размер
`1920x1080`, когда DAVA-контролы размещаются в client area `1536x864`.

### Внешний вид и текст

- `control_set_text`;
- `control_set_texture`;
- `control_set_color`;
- `control_set_opacity`;
- `control_set_visible`;
- `control_set_font`;
- `control_set_font_size`;
- `control_set_text_alignment`;
- `control_set_text_wrap`;
- `control_set_rich_text`;
- `control_set_localization_key`;
- `control_set_tooltip`;
- `control_set_accessibility_label`.

### Состояние и события

- `control_set_enabled`;
- `control_set_interactable`;
- `control_set_focus`;
- `event_subscribe`;
- `event_unsubscribe`.

### Compound widgets

- `button_create`;
- `checkbox_create`;
- `slider_create`;
- `dropdown_create`;
- `text_input_create`;
- `scroll_view_create`;
- `list_create`;
- `tabs_create`;
- `dialog_show`;
- `confirm_show`;
- `toast_show`.

### Style stack

- `style_push`;
- `style_update`;
- `style_pop`.

Style override применяет color/background/opacity/font/font size/texture и
поддерживаемый z-order поверх текущего состояния control. Overrides принадлежат
control, обновляются и снимаются строго в LIFO-порядке, а при destroy/unload
восстанавливаются и освобождаются автоматически. Если конкретное native поле
не поддержано backend, операция возвращает `NOT_SUPPORTED`, не фиктивный успех.

В текущем native bridge `control_set_localization_key`, tooltip,
accessibility label, focus и произвольные native DAVA animation/effect
components остаются отдельными operation-level gaps.

При `ui.root_destroyed` handles на native tree становятся stale/destroyed.
После `ui.root_ready` мод повторно вызывает `get_active_screen` и заново
получает snapshot/children. `slot_find` имеет смысл только после появления
точного semantic-slot resolver. Managed layout и event subscriptions работают;
native text/image/style operation без host component не возвращает fake `OK`.

## 29. `wotbmod.scene`

Scene API управляет собственными entities и разрешёнными attachments.

### Создание и иерархия

- `entity_create`;
- `entity_load`;
- `entity_clone`;
- `entity_destroy`;
- `get_active_scene`;
- `entity_get_parent`;
- `entity_set_parent`;
- `entity_add_child`;
- `entity_remove_child`;
- `entity_attach_ex`.

Attachment policy:

- detach on parent destroy;
- destroy on parent destroy;
- keep world transform;
- keep local transform.

### Transform и rendering

- `entity_set_transform`;
- `entity_get_transform`;
- `entity_get_world_transform`;
- `entity_set_world_transform`;
- `entity_get_bounds`;
- `entity_set_render_layer`;
- `entity_set_render_order`;
- `entity_set_lod_bias`.

### Nodes и animation

- `entity_list_nodes`;
- `entity_find_node_by_path`;
- `entity_list_animations`;
- `entity_get_animation_duration`;
- `entity_set_animation_speed`;
- `entity_set_animation_loop`;
- `entity_blend_animation`.

### Material/shader

- `entity_set_material_parameter`;
- `entity_clear_material_parameter`;
- `entity_set_shader_parameter`.

Parameter types: float, vec2, vec3, vec4, color, matrix4, texture URI. Native model/scene load требует resource/scene backend.

## 30. `wotbmod.audio` V2

Audio source описывается URI, flags, priority, volume, pitch и bus.

### Файл и playback

- `create`;
- `create_stream`;
- `preload`;
- `play`;
- `pause`;
- `resume`;
- `stop`;
- `destroy`;
- `is_playing`;
- `get_state`;
- `set_volume`;
- `set_pitch`;
- `set_pan`;
- `set_bus`;
- `set_position_3d`;
- `set_min_distance`;
- `set_max_distance`;
- `fade_to`;
- `seek`;
- `get_position`;
- `get_duration`;
- `set_loop`.

### Lifecycle callbacks

- `on_started`;
- `on_finished`;
- `on_error`;
- `unsubscribe`.

### Замена штатных звуков

- `sound_override_register`;
- `sound_override_unregister`;
- `sound_override_set_priority`;
- `sound_override_get_conflicts`.

Override связывает stock event name с replacement VFS URI. Conflict list показывает owner и priority.

### Native sound events

- `sound_event_create`;
- `sound_event_trigger`;
- `sound_event_stop`;
- `sound_event_set_paused`;
- `sound_event_set_volume`;
- `sound_event_set_position`;
- `sound_event_set_parameter`;
- `sound_event_get_parameter`;
- `sound_event_has_parameter`;
- `sound_event_get_name`;
- `sound_event_get_bus`;
- `sound_event_set_speed`;
- `sound_event_set_direction`;
- `sound_event_set_velocity`;
- `sound_event_set_loop_count`;
- `sound_event_set_priority`;
- `sound_event_destroy`.

### Listener и buses

- `set_listener_transform`;
- `set_bus_volume`.

Custom OGG/WAV или другой поддерживаемый Media Foundation формат размещается
в package и адресуется через `mod://`. Windows custom-audio backend реализует
play/pause/resume/force-stop, duration, current position и seek; доступность
конкретного codec всё равно определяется установленным decoder.

Отдельный native DAVA sound adapter управляет stock events из уже загруженных
клиентских sound banks, включая speed, direction, velocity, loop count и
priority через проверенные vtable slots. Это не регистрация нового Wwise bank:
если stock sound engine/событие недоступны, native sound-event methods и stock
overrides возвращают `E_NOT_SUPPORTED`; custom loose audio продолжает идти
через отдельный Windows backend.

## 31. `wotbmod.render`

Backends:

- NONE;
- D3D11;
- D3D12;
- Vulkan;
- Metal;
- OpenGL.

Render phases:

- BEFORE_UI;
- AFTER_UI;
- PRESENT.

### Callback и frame telemetry

- `register_callback`;
- `unregister_callback`;
- `set_callback_priority`;
- `get_backend`;
- `get_viewport`;
- `get_frame_index`;
- `get_delta_time`.

### Resources

- `create_texture`;
- `update_texture`;
- `destroy_texture`;
- `create_material`;
- `set_material_parameter`;
- `destroy_material`.

### Draw commands и state

- `draw_sprite`;
- `draw_text`;
- `draw_line`;
- `draw_mesh`;
- `push_state`;
- `pop_state`.

Draw вызовы выполняются только в допустимом render callback/thread. `push_state` и `pop_state` должны быть сбалансированы внутри callback.

Texture descriptor содержит размер, format, row pitch, dynamic flag, initial data и debug name. Material descriptor содержит shader URI, blend и depth-test. Render parameter поддерживает scalar, vectors, color, matrix и texture handle.

Текущий shipping backend получает D3D11 device/context/swapchain из единственного
loader-owned Present ingress. Portable callback реально поддержан только для
`PRESENT`; `BEFORE_UI` и `AFTER_UI` не создаются без отдельного подтверждённого
render ingress. Managed backend реализует texture/material и draw
sprite/text/line. `draw_mesh` помещает API-managed mesh/scene entity во
временное DAVA Scene attachment с model matrix текущего frame. Повторные
submission одного mesh в кадре coalesce-ятся до последней matrix; attachment
удаляется при завершении frame, scene/device reset или owner cleanup. Это не
raw `DrawIndexed` и не выдача game-owned mesh pointer.

Изменение наблюдаемого Present-state публикует:

- `wotbmod.render.device_created`;
- `wotbmod.render.device_lost`;
- `wotbmod.render.device_restored`;
- `wotbmod.render.swapchain_resized`;
- `wotbmod.render.backend_changed`.

`WotbModV3RenderLifecycleEvent` содержит только public metadata: предыдущий и
текущий backend, availability, маску изменившихся компонентов, frame index и
старый/новый viewport. COM pointers в event payload не передаются.

## 32. `wotbmod.render.native`

Методы:

- `get_native_device`;
- `get_native_context`;
- `get_native_swapchain`.

На D3D11 возвращаются pointer текущего device, immediate context и DXGI swapchain. Они:

- требуют UNSAFE;
- действуют только пока устройство/цепочка не потеряны;
- не сохраняются после `render.device_lost`;
- запрашиваются заново после `render.device_restored` или `swapchain_resized`;
- не освобождаются модом, если API явно не передал ownership.

Отсутствие pointer возвращается как `E_NOT_SUPPORTED` или `E_OBJECT_DESTROYED`, не как `OK` с фиктивным адресом.

## 33. `wotbmod.camera`

Camera modes:

- UNKNOWN;
- HANGAR;
- ARCADE;
- SNIPER;
- POSTMORTEM;
- REPLAY;
- FREE;
- CINEMATIC.

Методы:

- `get_active`;
- `get_mode`;
- `get_transform`;
- `set_transform`;
- `get_fov`;
- `set_fov`;
- `get_near_plane`;
- `get_far_plane`;
- `world_to_screen`;
- `screen_to_world`;
- `add_modifier`;
- `remove_modifier`;
- `transition_to`;
- `add_shake`.

Modifier phases в ABI: BEFORE_GAME и AFTER_GAME. В текущем backend реально
подключён только `AFTER_GAME`; регистрация `BEFORE_GAME` возвращает
`WOTBMOD_V3_E_NOT_SUPPORTED`. Callback получает mutable
`WotbModV3CameraState`.

`transition_to` выполняется loader-owned frame pump: duration ограничен
`0..30 s`, easing — известными значениями ABI, rotation интерполируется через
slerp. `add_shake` создаёт локальный детерминированный additive effect с
ограничениями amplitude `<=25`, frequency `<=120 Hz`, duration `<=30 s`,
не более 8 shake на мод и 64 глобально; сумма clamp-ится до 25. Effects
привязаны к owner и camera generation, отменяются при замене camera,
disable/unload и истечении времени.

В текущем x86 binding native FOV имеет реальный read/write backend. Поля clipping plane читаются по проверенному layout активной DAVA camera: near plane `this+0x20`, far plane `this+0x24`. Эти offsets относятся только к привязанному client build и не являются переносимым ABI объекта DAVA.

Текущий binding перехватывает штатный
`CameraModeChanged@Avatar@GES` callback: native `0` нормализуется в
`WOTBMOD_V3_CAMERA_MODE_ARCADE`, native `1` — в
`WOTBMOD_V3_CAMERA_MODE_SNIPER`. `get_mode` возвращает последнее
подтверждённое значение; до первого callback — `WOTBMOD_V3_E_NOT_FOUND`, а не
`OK` с `UNKNOWN`. Изменение публикуется как
`wotbmod.gameplay.camera_mode_changed`; payload — общий
`WotbModV3ClientEventEnvelope`, его `payload.camera` содержит
`previous_mode`, `mode`, `native_mode` и `flags`.

Transition с target mode, отличным от `UNKNOWN`, по-прежнему отклоняется:
generic effect не выдаётся за переключение штатного sniper/replay camera mode.

Active camera handle меняется при переходе hangar/battle/sniper/postmortem/replay. Мод не должен хранить handle без `is_alive`.

## 34. `wotbmod.gameplay.camera`

Методы:

- `set_fov`;
- `get_fov`;
- `set_fov_hangar`;
- `set_fov_battle`;
- `set_fov_sniper`;
- `reset_fov`;
- `set_zoom_steps`;
- `get_zoom_steps`;
- `set_zoom_multiplier`;
- `set_max_zoom`;
- `set_transition_mode`;
- `set_transition_duration`;
- `set_transition_easing`;
- `set_shake_enabled`;
- `set_shake_intensity`;
- `set_postprocessing_enabled`;
- `set_bloom_enabled`;
- `set_motion_blur_enabled`;
- `set_vignette_enabled`;
- `set_color_grading`;
- `set_dof_enabled`;
- `set_dof_params`;
- `free_enable`;
- `free_set_speed`;
- `free_set_position`;
- `free_set_rotation`;
- `free_get_position`;
- `free_get_rotation`;
- `get_config`;
- `apply_config`.

Gameplay camera является клиентской настройкой. Она не меняет server aim, dispersion, projectile direction или fire command. Free camera должна быть ограничена разрешёнными contexts/policy, особенно вне replay/training.

Фактический backend этой сборки поддерживает `set_fov`, `get_fov`, `reset_fov`
и context-specific FOV там, где текущий context можно достоверно сопоставить.
`set_zoom_steps`, zoom multiplier/max zoom, штатный sniper transition mode,
shake disable/intensity, post-processing, free camera, `get_config` и
`apply_config` возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`: соответствующие методы
присутствуют в ABI, но их native ingress не подтверждён. Generic
`wotbmod.camera.transition_to/add_shake` выше не является реализацией этих
штатных gameplay-настроек.

## 35. `wotbmod.vehicle.visual` V2

### Наблюдение

- `get_local_player_vehicle`;
- `is_local_player`;
- `is_hangar_vehicle`;
- `get_appearance_state`;
- `get_enemy`;
- `get_enemy_name`;
- `get_position`;
- `get_part_entity`;
- `find_attachment_point`.

### Attachments и overrides

- `attach_entity_ex`;
- `set_decal_override`;
- `set_texture_override`;
- `set_color_override`;
- `get_available_animation`;
- `play_animation`;
- `restore_appearance`;
- `set_part_visible`;
- `set_material_override`;
- `skin_apply_to_entity`;
- `set_custom_skin`;
- `set_custom_camouflage`;
- `set_emblem`;
- `set_inscription`;
- `set_engine_sound`;
- `set_gun_sound`;
- `play_hangar_animation`;
- `set_hangar_idle_animation`.

### Полные visual profiles

- `profile_register`;
- `profile_apply`;
- `profile_release`.

`WotbModV3VehicleVisualProfile` задаёт:

- profile ID;
- target vehicle name;
- hull mesh URI;
- turret mesh URI;
- gun mesh URI;
- chassis mesh URI;
- material overlay URI;
- texture pack URI;
- priority;
- hangar-only flag.

Profile registry, validation, priority и owner cleanup реализованы как
контрактная часть V1. Операции V1, которым нужна неизвестная live appearance
структура, продолжают fail-closed. Loader-private exact 11.19 adapter отдельно
реализует typed `NMaterial`/`Texture` mutation и apply; exact-client route
подтверждён live.

### Transactional skin packs V2

- `skin_pack_register`;
- `skin_pack_apply`;
- `skin_pack_rollback`;
- `skin_pack_get_state`;
- `skin_pack_release`.

Pack содержит до 64 записей `MESH`, `MATERIAL` или `TEXTURE`. Каждая запись
задаёт exact stock `game://` URI, replacement `mod://`/`data://`/`cache://`,
part и LOD `-1` либо `0..15`. Перед apply runtime проверяет все URI и готовит
все mounts; ошибка любой записи не оставляет частично установленную шкурку.
Повторный apply идемпотентен. Rollback/release снимает каждый overlay и
возвращает state к inactive.

Перехват `DAVA::File::Create` меняет ресурс при следующей штатной загрузке.
`WotbModV3VehicleSkinState::requires_model_reload == 1` честно сообщает, что
уже закэшированный tank model сам по себе не перестроился: необходим штатный
reload/пересоздание appearance. Отдельный loader-private DAVA route реализует
typed mesh load/hot-swap и `NMaterial` apply через mesh consumer, но public skin
pack не выдаёт этот exact-client результат за PASS до live logs.

Любой visual override меняет только client resource resolution. Collision,
hitbox, armor, gun, shell и network entity не изменяются. `restore_appearance`
не заявляет успешное восстановление override, который не был применён.

Enemy information доступна только для сущности, которую штатный клиент уже считает видимой и публичной. API не даёт скрытые позиции или имена.

## 36. `wotbmod.gameplay.hud`

### Reticle

- `reticle_set_texture`;
- `reticle_set_color`;
- `reticle_set_size`;
- `reticle_set_sniper_texture`;
- `reticle_set_reloading_indicator`;
- `reticle_set_dispersion_circle`.

### Damage log

- `damagelog_set_enabled`;
- `damagelog_set_position`;
- `damagelog_set_max_entries`;
- `damagelog_set_show_blocked`;
- `damagelog_set_show_ricochet`;
- `damagelog_set_show_module_damage`;
- `damagelog_set_format`;
- `damagelog_set_filter_own`.

### Session stats

- `session_stats_set_enabled`;
- `session_stats_set_fields`.

### Minimap

- `minimap_set_size`;
- `minimap_set_opacity`;
- `minimap_set_show_last_known`;
- `minimap_set_show_artillery_range`;
- `minimap_set_show_drawing`;
- `minimap_add_marker`;
- `minimap_remove_marker`.

### Sixth sense

- `sixth_sense_set_texture`;
- `sixth_sense_set_sound`;
- `sixth_sense_set_position`;
- `sixth_sense_set_scale`;
- `sixth_sense_set_delay_ms`.

### Hit indicator

- `hit_indicator_set_style`;
- `hit_indicator_set_color_hit`;
- `hit_indicator_set_color_pen`;
- `hit_indicator_set_color_ricochet`;
- `hit_indicator_set_color_crit`.

### Сброс

- `reset`.

Custom minimap marker не может представлять скрытую entity или координату, не полученную штатным клиентом. HUD API не содержит aim automation или fire commands.

**Штатные контролы (с 5 сентября 2026, 11.20.0.887).** Часть слотов
теперь ведёт настоящие контролы боевого экрана. Имена взяты из живого дампа
дерева тренировочного боя (`LIVE_UI_TREE.jsonl`, копия в
`docs/evidence/LIVE_UI_TREE_battle_11_20_0_887.jsonl`), каждое имя на
`BattleScreen` уникально:

| Ключ | Контрол на боевом экране | Что делает |
|---|---|---|
| `minimap` | `HUDLayer/MinimapHolder/Minimap` | `minimap_set_size` |
| `sixth_sense` | `HUDLayer/NotificationHolder/NotificationIconContainer/Lamp` | `sixth_sense_set_position`, `sixth_sense_set_scale` |
| `reticle` | `BattleUILayer/AimContainer/LayoutProtector/GunAim` | `reticle_set_size` |
| `damagelog` | `HUDLayer/RibbonsContainerLayoutProtector/RibbonsContainer` (лента урона и ассистов, текстового лога урона в Blitz нет) | `damagelog_set_enabled`, `damagelog_set_position` |
| `session_stats` | `.../DamageStatisticsContainer/DamageStatistics` (счётчики урона, ассиста, блока слева) | `session_stats_set_enabled` |

`reset` возвращает всем затронутым контролам исходные геометрию и видимость.
Живой прогон 5 сентября (строка `hud.stock_controls` validation-мода в
тренировочном бою): размер миникарты, масштаб лампы и прицела, скрытие и
показ ленты и счётчиков, `reset` вернули `WOTBMOD_V3_OK`.

**Расширение той же ночью (коммит abe68f7).** Ещё 25 слотов получили
бэкенд на тех же штатных контролах. Цвет фона игрового контрола читается и
пишется через компонент `DAVA::UIControlBackground` (RGBA по смещению
`+0x60`, из декомпиляции reflection-геттера и сеттера), а покадровый тик
runtime на главном потоке DAVA находит контролы, применяет записанное
состояние и удерживает то, что игра перезаписывает (видимость вспышек и
лент, цвет прицела, который игра меняет по цели).

| Слот | Что делает на штатном экране |
|---|---|
| `reticle_set_color` | тонирует фон `GunAim` и его сегментов разброса каждый кадр |
| `reticle_set_texture`, `reticle_set_sniper_texture` | мод-контрол с картинкой кладётся внутрь `GunAim` / `sightCursorZoom`, родная картинка становится прозрачной |
| `reticle_set_dispersion_circle` | прячет и возвращает сегменты `aimSpread*` |
| `reticle_set_reloading_indicator` | прячет и возвращает `reloadAndFuelContainer` и `DrumReload` |
| `hit_indicator_set_style` | `COMPACT` сжимает `HitHighlight` и `RecochetHighlight` до центральных 40 %, `MINIMAL` держит их скрытыми; `DIRECTIONAL` честно не поддержан |
| `hit_indicator_set_color_hit`, `..._ricochet` | RGB вспышек, альфа остаётся игровой; `pen` и `crit` не имеют своего контрола |
| `damagelog_set_max_entries` | прячет самые старые ленты сверх лимита |
| `damagelog_set_filter_own` | `1` уже верно (лента только своя), `0` не поддержан |
| `session_stats_set_fields` | `DAMAGE` показывает ряд `DamageDealt`, `0` прячет блок; остальные биты не поддержаны |
| `minimap_set_opacity` | умножает альфу фона `Minimap` и всех его потомков |
| `sixth_sense_set_texture` | картинка-оверлей внутри `Lamp` вместо `Icon` |
| `sixth_sense_set_sound` | аудио-объект мода играет при каждом зажигании лампы (штатный звук не глушится) |
| `sixth_sense_set_delay_ms` | минимальное время свечения лампы: если игра гасит раньше, лампа держится до истечения |

Слот, вызванный на главном потоке DAVA (например, из колбэка
`dispatch_to_main_thread`), применяется сразу и возвращает настоящий
результат; с других потоков изменение ложится в следующий кадр, и слот
ручается только за записанное состояние. `reset` возвращает всё, включая
цвета и оверлеи; мод обязан вызвать его до выгрузки, иначе runtime только
забудет состояние, а восстановить экран без хэндлов мода не сможет.

Живая проверка 5 сентября 22:34 в тренировочном бою: строка
`hud.stock_controls` вернула `WOTBMOD_V3_OK` целиком, все 22 рабочих шага `OK`
и 5 ожидаемых отказов (`pen`, `DIRECTIONAL`, `show_blocked`, поле `KILLS`,
`last_known`) на месте; `hud.stock_reset` вернул `OK`. По дороге выяснилось
три факта об экране боя: сегменты `aimSpread*` игра создаёт и убирает по ходу
прицеливания (32 в одном бою, ноль в следующем на том же танке), поэтому тик
переобходит детей `GunAim` при смене их числа; у скрытых вспышек компонент
фона появляется только с первым попаданием, цвет дожидается его; большинство
картинок HUD рисует не `UIControlBackground`, а `UIDynamicAtlasImageComponent`
(цвет по `+0x3C`), и бэкенд читает и пишет оба. Для текстурных оверлеев моду
нужно право `ui.modify.own`, потому что оверлей это его собственный контрол, а
URI ресурсов пишутся как `mod://self/...` (authority VFS это пространство мода,
пакетный id не подходит).

Остаются `WOTBMOD_V3_E_NOT_SUPPORTED`: `damagelog_set_show_blocked`,
`..._ricochet`, `..._module_damage`, `damagelog_set_format` (тип ленты
не читается без локализованного текста), `minimap_set_show_last_known`,
`..._artillery_range`, `..._show_drawing` (таких функций в Blitz нет),
`minimap_add_marker` / `remove_marker` (нужна калибровка мир-миникарта),
`hit_indicator_set_color_pen` / `_crit`, `DIRECTIONAL`. Интерфейс требует
именное право `gameplay.tweak.hud` и контекст BATTLE/REPLAY/TRAINING; статус
`DEGRADED`. Использовать общий UI API для собственного mod-owned overlay можно,
но это не считается изменением штатного reticle/minimap/sixth-sense control.

## 37. `wotbmod.gameplay.hangar`

Методы:

- `set_background`;
- `set_background_video`;
- `set_music`;
- `set_lighting`;
- `set_vehicle_preview_angle`;
- `set_vehicle_preview_zoom`;
- `set_floor_texture`;
- `set_skybox`;
- `hide_ui_elements`;
- `set_camera_orbit_speed`;
- `reset`.

Все операции ограничены HANGAR. URI должны разрешаться через VFS. `reset` снимает изменения владельца и возвращает штатное состояние.

Native Hangar adapter для перечисленных semantic operations в этой сборке не
подключён; методы возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`. Обычный VFS overlay
файла не выдаётся за изменение background/music/lighting, пока не доказано,
какой штатный consumer прочитал и применил ресурс.

## 38. `wotbmod.gameplay.replay`

Методы:

- `set_speed`;
- `seek`;
- `get_duration`;
- `get_position`;
- `add_marker`;
- `remove_marker`;
- `set_camera_mode`;
- `set_follow_vehicle`;
- `export_clip`.

Replay camera modes:

- FREE;
- FOLLOW_VEHICLE;
- TOP_DOWN;
- CINEMATIC;
- FIRST_PERSON.

Interface доступен только в REPLAY. `export_clip` пишет только в разрешённый scoped output URI и может вернуть `E_NOT_SUPPORTED`, если encoder backend отсутствует.

В текущей сборке replay state/camera/encoder ingress не подтверждён, поэтому
все replay methods возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`; API table остаётся
стабильной для будущего loader-owned adapter.

## 39. `wotbmod.entity.public`

Public entity types:

- UNKNOWN;
- VEHICLE;
- PROJECTILE;
- EFFECT.

Public values:

- BOOL;
- INT64;
- DOUBLE;
- VEC3;
- STRING.

Методы:

- `get_public_id`;
- `get_public_type`;
- `get_public_property`;
- `subscribe_public_property`;
- `unsubscribe_public_property`;
- `is_visible_to_player`;
- `get_snapshot`;
- `enumerate_visible`.

Полный ABI snapshot имеет поля handle, public ID/type, visible/local flags, team, health/max health, visible position/direction, public vehicle type и display name. Native ingress (11.20.0.887) заполняет их все:

- public ID/type, visible/local flags, health/max health, public vehicle type и team приходят с объекта Vehicle (раскладка проверена по vtable);
- position/direction своей машины лоадер снимает с покадрового блока позы `PlayerController` (RVA `0x011B8C10`, мировая матрица внешности по `+108` блока; мир Y-up, `position` это начало матрицы, `direction` единичная ось forward корпуса с учётом наклона). Для остальных видимых машин та же матрица читается по цепочке объекта внешности (`entity+56 → +8 → +1344`), которая перед первой публикацией сверяется с блоком позы своей машины и без совпадения не используется. Публикация не чаще 10 Гц и только при сдвиге от 5 см или повороте от 1°, пока машина жива; после гибели остаётся последняя поза. Чужие машины отдаются только пока они публичны (союзники и засвеченные враги), при потере засвета запись удаляется;
- display name это ник игрока из ростера арены (`ClientArena::VehicleInfo`, приходит со списком машин на экране загрузки).

Сверх snapshot через `get_public_property` доступны свойства ростера, которые в snapshot не входят (его ABI заморожен) и читаются только по имени: `clan_tag` (STRING, пустая строка без клана), `account_id` (INT64, Wargaming account id), `kills` или `frags` (INT64, фраги в текущем бою по серверной статистике арены, обновляются с каждым пакетом statistics), `vehicle_name` (STRING, «нация:тег», например `france:F127_ELC_AMX_901_Proto`), `vehicle_display_name` (STRING, локализованное название техники на языке клиента, например `T49`; лоадер берёт его из таблиц `DAVA::LocalizationSystem` самого клиента по ключу `#usa_vehicles:A100_T49`, обходя `std::map` без вызова игрового кода). `subscribe_public_property` работает с ними так же, как со snapshot-полями. Поле без источника отвечает `WOTBMOD_V3_E_NOT_SUPPORTED`, а не `OK` с нулём или пустой строкой. Проверено в случайных боях 6 сентября 2026 (01:19 движением своей машины, 02:38 и 02:48 по ростеру всех 14 игроков, 03:0x по имени техники).

Security contract:

- перечисляются только объекты, уже видимые/публичные для локального клиента;
- при потере видимости handle/property не используется как источник актуальной скрытой позиции;
- не выдаются internal entity pointer, server-only properties, hidden enemy state или непринятые RPC payloads.

Native registry принимает локальную entity независимо от visibility и нелокальную entity только после штатного visible/spot ingress. Unspot удаляет нелокальную entity из public registry; legacy vehicle enumeration использует ту же проверку и не обходит этот контракт.

## 40. `wotbmod.bigworld.rpc`

Interface намеренно metadata-only.

Методы:

- `get_policy`;
- `subscribe_observed`;
- `unsubscribe_observed`.

Observed RPC ABI содержит:

- INCOMING или OUTGOING direction mask;
- public entity ID;
- sequence;
- timestamp;
- public entity type;
- method name.

Policy поля:

- `metadata_observation`;
- `payload_access`;
- `outgoing_injection`;
- `packet_modification`;
- `packet_drop`;
- `packet_replay`.

В публичном API только `metadata_observation` может быть включён. Payload access, outgoing injection, packet modification, packet drop и packet replay остаются выключенными. Нет метода send/modify/drop/replay.

Текущий native ingress публикует только metadata уже наблюдаемого incoming RPC: public entity ID/type, sequence, timestamp и method name. Подписка с `OUTGOING`, включая смешанный `INCOMING | OUTGOING` mask, возвращает `WOTBMOD_V3_E_NOT_SUPPORTED` и не создаёт subscription. Payload bytes не копируются в public API; outgoing observation, payload access, send/injection, modify, drop и replay также возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`.

## 41. `wotbmod.projectile` V2

Owner scopes:

- UNKNOWN;
- LOCAL_PLAYER;
- ALLY_VISIBLE;
- ENEMY_VISIBLE;
- REPLAY.

Методы:

- `tracer_style_register`;
- `tracer_style_unregister`;
- `tracer_set_texture`;
- `tracer_set_color`;
- `tracer_set_width`;
- `tracer_set_lifetime`;
- `tracer_set_fade`;
- `projectile_get_visual_entity`;
- `projectile_attach_visual`;
- `projectile_get_owner_scope`.
- `projectile_get_snapshot`;
- `impact_visual_register`;
- `impact_visual_update`;
- `impact_visual_unregister`.

Tracer style descriptor содержит ID, texture URI, color, width, lifetime, fade start и priority.

Публичные event IDs:

- `wotbmod.gameplay.local_shell_fired` с `WotbModV3LocalShellFiredEvent`;
- `wotbmod.gameplay.visible_tracer_created` с `WotbModV3VisibleTracerEvent`;
- `wotbmod.gameplay.visible_tracer_destroyed` с `WotbModV3VisibleTracerEvent`.
- `wotbmod.gameplay.projectile.created`;
- `wotbmod.gameplay.projectile.updated`;
- `wotbmod.gameplay.projectile.impacted`;
- `wotbmod.gameplay.projectile.destroyed`.

V2 создаёт owner/generation handle и provenance-aware snapshot. `valid_fields`
является единственным источником истины: runtime не придумывает origin,
direction, shell type, attacker/target или shot ID, если native callback этого
не доказал. `VehicleGameLogic::showShooting` порождает `CREATED`, затем
`IN_FLIGHT`; `GameSceneController::OnVehicleHitDamage` порождает native-impact
record `IMPACTED`. Завершённый impact получает `DESTROYED` с reason
`IMPACT_COMPLETE`, оставшийся in-flight record — `TIMEOUT`, а shutdown —
`SHUTDOWN`.

Текущие safe hooks не доказывают, что hit record относится к конкретному ранее
полученному showShooting record, поэтому runtime не склеивает их по догадке.
Snapshot отдельно сообщает source `STOCK_SHOT` или `NATIVE_IMPACT`.

`impact_visual_register` создаёт ограниченный owner-scoped registry. На
native impact loader загружает указанный `scene_uri`, прикрепляет настоящий
DAVA Scene entity в impact position и удаляет его по `lifetime_seconds`;
`max_instances`, priority, shell/scope filters и cleanup не позволяют эффектам
расти без границ. Это native Scene visual, но не подмена внутреннего stock
impact effect manager.

IDs `visible_tracer_created` и `visible_tracer_destroyed` существуют в ABI,
однако ownership-safe ingress штатного tracer lifecycle пока не подключён.
Loader-private exact 11.19 route для stock tracer create вызывает проверенный
bridge к `TracerManager::ShowTracer` и возвращает только typed completion token;
game-owned manager, visual node и style object не пересекают public ABI. Stock
tracer style mutation и public visual attachment остаются
`WOTBMOD_V3_E_NOT_SUPPORTED`, а typed create route подтверждён live. API не
меняет ballistics, server projectile, penetration, damage, shell
speed или hit result.

## 42. `wotbmod.client`

Compatibility states:

- UNKNOWN;
- SUPPORTED;
- DEGRADED;
- BINDINGS_MISSING;
- HASH_MISMATCH.

Методы:

- `is_supported`;
- `get_binding_pack_version`;
- `get_compatibility_state`;
- `enumerate_missing_bindings`;
- `leave_to_hangar`.

`enumerate_missing_bindings` возвращает capability, symbol и reason для каждого отсутствующего native binding.
Для частично подключённых подсистем список также содержит operation-level gaps:
semantic UI slots, advanced Scene nodes/bounds/animation/material operations,
audio streaming/mixer/listener operations, дополнительные render/camera phases,
полную vehicle appearance mutation, HUD/Hangar/Replay, optional public-entity
fields, generic RPC, stock tracer styling и raw DAVA object pointers.
Поэтому `is_supported == 1` означает, что binding pack пригоден для
поддерживаемого подмножества API, а не что каждая функция имеет native backend.

Loader проверяет executable fingerprint и сигнатуру/границы каждого RVA до
установки hook. Для exact build атомарно пишется
`mods/cache/binding_pack_validation.json` с binding ID, RVA, expected/actual
bytes и verdict. После обновления игры mismatch отключает соответствующий hook
fail-closed. Проверка не является автоматическим reverse engineering и не
создаёт новый pack: новые RVA добавляются только после анализа и подтверждения.

`leave_to_hangar` имеет реальный штатный client backend и capability `AVAILABLE`. Он выполняет действие через захваченный валидный owner и native callback. Вызов разрешён только при named permission `client.leave_to_hangar`, достаточном tier, совместимом binding pack и допустимом context/thread. Если вызов сделан уже в HANGAR, owner не захвачен или обнаружен client mismatch, результат не может быть `OK`.

## 43. `wotbmod.client.device`

Методы:

- `get_info`;
- `get_graphics_adapter_count`;
- `get_graphics_adapter_at`.

Device info:

- process architecture;
- OS architecture;
- logical processor count;
- graphics adapter count;
- physical/available memory;
- OS name;
- processor name;
- primary graphics adapter.

Adapter info:

- index;
- vendor ID;
- device ID;
- subsystem ID;
- revision;
- software-adapter flag;
- dedicated video memory;
- dedicated system memory;
- shared system memory;
- name.

Capability generic device info имеет статус `AVAILABLE`: данные реально собираются через Win32/DXGI и не требуют чтения неописанных полей внутреннего `DeviceInfo` игры. Это системная/adapter metadata, а не borrowed pointer на DAVA `DeviceInfo`.

## 44. `wotbmod.diagnostics`

Методы:

- `get_stats`;
- `copy_report_json`;
- `report_fault`.
- `crash_add_context`;
- `crash_set_last_action`;
- `crash_add_breadcrumb`;
- `export_bundle`;
- `get_mod_health`.

Stats содержат loaded/enabled mods, live handles, registered interfaces/capabilities, frame index и context mask.

V2 сохраняет полный бинарный prefix V1. Runtime удерживает не более 32 context
entries и 64 breadcrumbs на мод, экранирует JSON и считает явно сообщённые
faults. `export_bundle` атомарно записывает JSON только в
`<mod-data>/diagnostics`; компоненты пути, traversal и reparse directory
отклоняются. `get_mod_health` возвращает только измеряемое runtime-состояние,
permission tier, число faults, profiler aggregates и память, которой владеет
сам diagnostics backend. Это не оценка native heap, FPS или вероятности crash.

`report_fault` принимает category, message и context JSON. Не помещайте tokens,
passwords и персональные данные в diagnostics. V2 не ставит глобальный
exception handler и не обещает minidump: crash context предназначен для
атрибуции и экспортируемого отчёта.

## 45. `wotbmod.devtools` V3

Методы:

- `marker`;
- `span_begin`;
- `span_end`;
- `counter_set`.
- `inspect_ui`;
- `inspect_scene`;
- `inspect_material`;
- `inspect_resource`;
- `inspect_hook_chain`;
- `inspect_events`;
- `profiler_get_mod_cpu_time`;
- `profiler_get_mod_memory`;
- `get_callback_profile`;
- `set_callback_budget`;
- `reset_callback_profile`.

V3 сохраняет полный бинарный prefix V2. Span info возвращает start ticks,
elapsed ticks, milliseconds, category и name. Каждый успешный `span_begin`
закрывается `span_end` либо owner cleanup. Для завершённых spans backend
измеряет CPU текущего thread через `GetThreadTimes`; отдельно возвращается wall
time через QPC. Memory aggregate учитывает только retained diagnostics data и
живые span objects. `counter_set` хранит до 128 именованных counters на мод,
повторная запись обновляет значение, а текущие значения входят в diagnostics
JSON вместе с frame последнего обновления; `NaN` и infinity отклоняются.

Callback profile измеряется runtime независимо от ручных spans: callbacks и
CPU time текущего/предыдущего frame, total/max/slow callbacks, coalesced count
и число over-budget frames. `set_callback_budget` принимает `100..8000 us` и
`1..1024` callbacks/frame. После исчерпания бюджета coalescible high-frequency
callbacks пропускаются; lifecycle/state callbacks, не помеченные coalescible,
не теряются. Профиль и budget принадлежат моду и очищаются при его unload.

`inspect_resource`, `inspect_hook_chain` и `inspect_events` возвращают
owner-scoped JSON snapshot реальных объектов portable runtime. Поддерживаются
двухшаговый buffer contract, exact selector, JSON escaping и пустой
`count: 0`; результат явно содержит `native_enumeration: false`, поэтому
snapshot не выдаётся за перечисление внутренних объектов клиента.

`inspect_ui`, `inspect_scene` и `inspect_material` в devtools пока не имеют
универсального JSON native-enumeration backend и не создают фиктивные деревья.
Инспекция active UI tree доступна отдельно через UI V3; API-owned Scene и
materials — через их typed handles. Полное перечисление всех game-owned Scene
entities/`NMaterial` по-прежнему `E_NOT_SUPPORTED`.

## 46. `wotbmod.manifest`

Package types:

- NATIVE;
- CONTENT_ONLY.

Dependency kinds:

- REQUIRED;
- OPTIONAL;
- INCOMPATIBLE.

Signature states:

- UNSIGNED;
- DECLARED;
- VALID;
- INVALID;
- UNSUPPORTED.

Методы:

- `parse_json`;
- `parse_uri`;
- `validate`;
- `get_info`;
- `get_api_requirement`;
- `get_dependency`;
- `get_permission`;
- `get_resource_pattern`;
- `get_locale_pattern`;
- `get_entrypoint`;
- `get_client_build`;
- `get_client_executable_hash`;
- `resolve_dependencies`;
- `verify_content_sha256`;
- `verify_signature`.

Полный native manifest:

```json
{
  "manifest_version": 1,
  "type": "native",
  "id": "author.example",
  "name": "Example",
  "version": "1.2.3",
  "developer": "Author",
  "api": {
    "wotbmod.core": ">=1 <2",
    "wotbmod.ui": ">=2 <3",
    "wotbmod.audio": ">=2 <3"
  },
  "client": {
    "builds": [
      "11.19.0.834"
    ],
    "executable_hashes": [
      "41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD"
    ]
  },
  "entrypoints": {
    "windows-x86": "bin/windows-x86/example.dll"
  },
  "dependencies": {
    "author.common": ">=2 <3"
  },
  "optional_dependencies": {
    "author.integration": ">=1 <2"
  },
  "incompatible_dependencies": {
    "other.conflicting-mod": "*"
  },
  "permissions": [
    "core",
    "ui.create",
    "audio.custom",
    "resources.mod"
  ],
  "settings": "settings/schema.json",
  "locales": [
    "locales/*.json"
  ],
  "resources": [
    "assets/**"
  ]
}
```

Preflight обязан выполняться до `LoadLibrary`. Для native package проверяются ID, semver, API requirements, dependencies, permissions, client build/hash, entrypoint path, catalog decision, content SHA-256 и signature policy. Точные permission names из manifest копируются в package load plan и устанавливаются как runtime allowlist до вызова V3 entrypoint. Они не ограничивают код, исполняемый Windows в `DllMain`, и не перехватывают direct Win32-вызовы DLL.

Поддерживаемый алгоритм — `ecdsa-p256-sha256`. Public key хранится в
`mods/trust/keys/<key_id>.p256` как strict 64-byte `X||Y` hex; reparse-backed
store/key и недопустимый key ID отклоняются. `manifest.verify_signature`
вычисляет SHA-256 указанного owned package path и проверяет manifest signature
через Windows CNG.

Package preflight также понимает detached sidecar
`<package>.wotbmod.sig`/`<archive>.sig` формата `WOTBMOD-SIGNATURE-V1` с ровно
четырьмя полями: `algorithm`, `key_id`, `sha256`, `signature`. Digest обязан
совпасть с уже рассчитанным package SHA-256. Invalid signature блокирует пакет
даже в permissive режиме; неизвестный signer остаётся явно `DECLARED` с
warning либо блокируется флагом `PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE`;
trusted signer получает `SIGNATURE_VALID` до `LoadLibrary`.

`PACKAGE_PLAN_READY` остаётся состоянием loadability, не синонимом trust.
Достоверность читается по `signature_status`, `signature_key_id` и warning
flags. Unsigned package может быть READY только если policy не требует trusted
signature.

## 47. `wotbmod.catalog`

Statuses:

- VERIFIED;
- COMMUNITY;
- UNREVIEWED;
- BANNED.

Методы:

- `validate_record`;
- `evaluate_install`;
- `status_name`.

Catalog record содержит ID, version, SHA-256, status, reason и signer ID. Decision содержит load allowed, warning required и hash verified.

Policy:

- VERIFIED — запись прошла curated verification и hash match;
- COMMUNITY — community-reviewed, правила предупреждения задаёт loader;
- UNREVIEWED — требует явного предупреждения/решения;
- BANNED — загрузка запрещена;
- ручная установка unsafe native package не превращает его в VERIFIED.

Catalog curation не является заменой API permission проверки. Обратное тоже верно: named permissions WotbMod API не являются OS sandbox для нативной DLL.

## 48. `wotbmod.content`

Override kinds:

- AUDIO;
- TEXTURE;
- UI;
- HANGAR;
- MODEL;
- LOCALIZATION.

Методы:

- `parse_json`;
- `parse_uri`;
- `validate`;
- `get_info`;
- `get_override`;
- `apply`;
- `unapply`.

Content-only descriptor:

```json
{
  "type": "content",
  "id": "author.content-pack",
  "name": "Content Pack",
  "version": "1.0.0",
  "overrides": {
    "audio": {
      "ui/button_click": {
        "asset": "audio/button_click.ogg",
        "priority": 100
      }
    },
    "textures": {
      "game://3d/Tanks/example/skin.dds": "textures/skin.dds"
    },
    "ui": {
      "game://ui/example.yaml": "ui/example.yaml"
    },
    "hangar": {
      "default_background": "hangar/background.png"
    },
    "models": {
      "game://3d/Tanks/example/model.sc2": "models/model.sc2"
    },
    "localization": {
      "mod.example.title": "locales/ru.json"
    }
  }
}
```

Content-only manifest:

```json
{
  "manifest_version": 1,
  "type": "content",
  "id": "author.content-pack",
  "name": "Content Pack",
  "version": "1.0.0",
  "developer": "Author",
  "content": "content.json",
  "permissions": [
    "resources.mod",
    "resources.overlay.game"
  ],
  "resources": [
    "audio/**",
    "textures/**",
    "ui/**",
    "hangar/**",
    "models/**",
    "locales/**"
  ]
}
```

Parser и validator работают без загрузки DLL. Реальное применение content-only overlay требует одновременно `resources.mod`, `resources.overlay.game` и tier `REVIEWED`. Для `TEXTURE`, `UI` и `MODEL` `apply` сначала проверяет все source assets, затем транзакционно создаёт VFS overlays; при ошибке уже созданные mounts откатываются. `unapply` снимает их в обратном порядке, disable выполняет unapply, а повторный enable снова применяет descriptor. Семантические `AUDIO`, `HANGAR` и `LOCALIZATION` overrides возвращают `WOTBMOD_V3_E_NOT_SUPPORTED`, потому что для них недостаточно обычного file overlay. Descriptor parsing нельзя выдавать за применённую замену.

### 48.1. Developer CLI `wotbmod`

`tools/wotbmod.py` предоставляет команды:

- `new` — создать native/content project;
- `validate` — строго проверить manifest, directory или `.wotbmod`;
- `inspect` — показать проверенные metadata и file hashes;
- `pack` — создать детерминированный ZIP-store `.wotbmod`;
- `build` — запустить явно переданный executable/argv без shell interpolation;
- `run` — атомарно staged-install проверенного package и, если не указан
  `--no-launch`, запуск точного клиента;
- `doctor` — проверить SDK/compiler/runtime artifacts и fingerprint клиента.

`run` перед установкой требует совпадения architecture, build и SHA-256
`wotblitz.exe`, staging выполняет в отдельное место, а ошибка сохраняет или
восстанавливает last-good install. CLI не содержит команды `publish`: загрузка
в каталог/production backend не смешана с локальными build/install операциями.

## 49. Camera, Render/RHI/DXGI и device lifecycle

Рекомендуемая последовательность:

1. Подписаться на `render.device_created`, `render.device_lost`, `render.device_restored`, `render.swapchain_resized`.
2. Зарегистрировать portable callback через `render.register_callback`.
3. Получать backend/viewport/frame из `render`.
4. Использовать portable draw calls, если native API не нужен.
5. Для native D3D11 запросить UNSAFE и `wotbmod.render.native`.
6. Получать device/context/swapchain в текущем render lifecycle.
7. На device lost удалить собственные GPU resources и забыть raw pointers.
8. На restored пересоздать resources.
9. Camera handle получать после соответствующего camera/context event.

DXGI adapter metadata читается через `wotbmod.client.device`; raw swapchain — только через `wotbmod.render.native`.

Render lifecycle формируется из diff фактически наблюдаемого Present-state.
Первый полный D3D11 snapshot даёт `device_created`; исчезновение/removed device —
`device_lost`; восстановление — `device_restored`; замена device публикуется как
последовательность lost/restored. Изменение backbuffer размера даёт
`swapchain_resized`, backend — `backend_changed`. Неполный или removed D3D11
state нормализуется в `NONE`.

Для текущего x86 client binding подтверждены native active camera, transform,
FOV, projection/unprojection и layout clipping planes: near `+0x20`, far
`+0x24`. `camera.get_mode` читает последнее событие штатного
`CameraModeChanged` для подтверждённых native значений `0=ARCADE` и
`1=SNIPER`. Если native mode ещё неизвестен, context достоверно нормализует
`HANGAR` и `REPLAY`; в остальных contexts до callback возвращается
`WOTBMOD_V3_E_NOT_FOUND`.
Loader-owned `AFTER_GAME` transition/shake работают через frame pump, имеют
bounded limits и owner cleanup; это не включает штатное переключение
gameplay zoom/sniper/freecam. Значения `POSTMORTEM`, `FREE` и `CINEMATIC` не
угадываются из соседних native enum и требуют отдельного source.

## 50. BigWorld entity/RPC safety contract

Разрешено:

- публичный snapshot entity, уже видимой штатному клиенту;
- локальная entity;
- public ID/type, visibility/local flags, health/max health и public vehicle type;
- metadata наблюдаемого входящего stock RPC;
- timestamp, method name и public entity ID/type входящего наблюдения.

Не предоставляется:

- скрытая enemy position;
- team, position, direction и display name без подтверждённого native source;
- server-only entity map;
- RPC payload bytes;
- outgoing RPC observation;
- packet injection;
- изменение outgoing/incoming packet;
- packet drop;
- packet replay;
- вызов server method;
- anti-cheat bypass.

Мод, которому нужен визуальный эффект на видимой entity, использует `entity.public` для snapshot и `scene`/`vehicle.visual` для визуала.

## 51. Shell/projectile/tracer contract

Projectile V2 использует safe ingress `VehicleGameLogic::showShooting` и
`GameSceneController::OnVehicleHitDamage`, создаёт generation handles,
provenance-aware snapshots и события created/in-flight/impacted/destroyed.
Fields доступны только по `valid_fields`; timeout/shutdown/impact cleanup
детерминирован. Из-за отсутствия доказанного общего identity shot и hit records
не связываются искусственно.

`wotbmod.gameplay.local_shell_fired` отражает уже созданный локальный stock
client shell только после подтверждения owner. Достоверные public shell ID/type
текущий ingress не предоставляет.

`wotbmod.gameplay.visible_tracer_created` и `wotbmod.gameplay.visible_tracer_destroyed` зарезервированы в ABI, но native source жизненного цикла штатного tracer пока отсутствует. Поэтому runtime не синтезирует эти события из локального состояния.

Managed impact visuals загружают API-разрешённый `.sc2` и прикрепляют DAVA
entity к активной Scene на bounded lifetime. Методы texture/color/width/
lifetime/fade, stock tracer style registration и attachment к внутреннему
tracer object существуют в table, но без доказанного ownership возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED`.

Не изменяются:

- server shell ID semantics;
- trajectory;
- velocity;
- gravity;
- dispersion;
- penetration;
- damage;
- hitbox;
- hit result.

## 52. LeaveToHangar

Перед вызовом:

1. Получить `WotbModV3ClientInfo`.
2. Проверить `supported`.
3. Проверить `compatibility_state == SUPPORTED` или документированное DEGRADED с доступной capability.
4. Убедиться, что missing binding для LeaveToHangar отсутствует.
5. Проверить permission `client.leave_to_hangar`.
6. Вызвать на main thread.
7. Обработать `E_NOT_SUPPORTED`, `E_CLIENT_MISMATCH`, `E_WRONG_THREAD`, `E_BUSY`.

API вызывает штатный action/callback. Он не симулирует сетевой пакет выхода.

Для поддерживаемого client build backend и capability `LeaveToHangar` имеют статус `AVAILABLE`; это не отменяет runtime-проверки permission, context, main thread и валидного owner.

## 53. Проверка capability и операции

```cpp
WotbModV3CapabilityInfo capability = {};
WOTBMOD_V3_INIT_STRUCT(capability, WOTBMOD_V3_ABI_VERSION);

const WotbModV3Result capability_result =
    capabilities->query(
        mod,
        "camera.native",
        &capability);

if (capability_result == WOTBMOD_V3_OK &&
    (capability.status == WOTBMOD_V3_CAPABILITY_AVAILABLE ||
     capability.status == WOTBMOD_V3_CAPABILITY_DEGRADED)) {
    WotbModV3CameraHandle camera =
        WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result operation_result =
        camera_api->get_active(mod, &camera);
    if (operation_result == WOTBMOD_V3_OK) {
        // Handle используется только после успешной операции.
    }
}
```

Даже после capability check профильный вызов может отказать из-за race со сменой context/device/owner. Код всегда проверяется.

## 54. Два test-mod

### `v3_core_runtime_test_mod`

Путь: `examples/v3_core_runtime_test_mod/v3_core_runtime_test_mod.cpp`.

Проверяет:

- bootstrap;
- core;
- capabilities;
- permissions;
- handles;
- lifecycle;
- hooks;
- unsafe native;
- events;
- settings;
- storage;
- input;
- VFS;
- async;
- HTTP;
- intermod;
- YAML;
- archive;
- loaders;
- diagnostics;
- devtools;
- manifest;
- catalog;
- content.

### `v3_client_resources_test_mod`

Путь: `examples/v3_client_resources_test_mod/v3_client_resources_test_mod.cpp`.

Проверяет:

- UI;
- resources;
- Render;
- Render Native;
- Camera;
- Scene;
- Audio;
- vehicle visuals;
- gameplay camera;
- HUD;
- hangar;
- replay;
- public entity;
- BigWorld RPC;
- projectile/tracer;
- client compatibility/LeaveToHangar function slot;
- device info.

Оба мода:

- запрашивают интерфейсы через `query_interface`;
- сверяют `struct_size` и version;
- проверяют каждый function pointer в таблице;
- выполняют только наблюдающие или локальные безопасные probes;
- считают ожидаемую native/backend недоступность отдельно;
- не вызывают автоматически `leave_to_hangar`;
- не устанавливают address hook;
- не меняют HUD, camera, vehicle mesh или gameplay во время smoke-test.

Компиляция:

```bat
cl /nologo /std:c++17 /EHsc /MD /W4 /WX /permissive- /LD ^
  /Iinclude ^
  examples\v3_core_runtime_test_mod\v3_core_runtime_test_mod.cpp ^
  /link /OUT:build\v3_core_runtime_test_mod.dll

cl /nologo /std:c++17 /EHsc /MD /W4 /WX /permissive- /LD ^
  /Iinclude ^
  examples\v3_client_resources_test_mod\v3_client_resources_test_mod.cpp ^
  /link /OUT:build\v3_client_resources_test_mod.dll
```

## 55. Lua/WASM runtimes

Замороженный C ABI V3 не содержит прямого Lua/WASM entrypoint, engine handle
или универсального FFI. Вместо расширения frozen ABI официальный native-пакет
`wotbmod.lua_host` встраивает Lua 5.4.7 и переводит scripting calls в те же
reviewed V3 interfaces с обычными permission/context/owner checks. Он запускает
manifest-based скрипты из `mods\lua`, предоставляет runtime UI, typed events,
public-player snapshots и ограничивает каждый внешний вход бюджетом Lua VM.
Полный scripting contract описан в `docs/LUA_MODS_RU.md`.

Lua-мод не объявляется отдельным package entrypoint V3: `.wotbmod` нужен
native host-у, а script manifest находится рядом с `main.lua`. WASM runtime в
этой версии не реализован. Raw pointers, произвольные DAVA class/component
payloads и native calls без опубликованного reviewed interface остаются
недоступными.

## 56. Финальный checklist разработчика

- Используется только `WotbModLoadV3`.
- Все структуры обнулены и имеют корректный `struct_size`.
- Каждый `query_interface` проверяет код.
- Каждый профильный вызов проверяет код.
- `E_NOT_SUPPORTED` не показывается как успех.
- Фактический permission tier достаточен.
- Package manifest перечисляет точные named permissions; runtime grant содержит требуемое имя.
- Для game resource overlay перечислены оба разрешения: `resources.mod` и `resources.overlay.game`.
- Native package ограничивает client build/hash.
- Named permissions не считаются sandbox для `DllMain` или direct Win32-кода.
- Все handles имеют owner и освобождаются.
- Все subscriptions, timers, tasks, hooks, callbacks, overrides и mounts снимаются.
- UI перестраивается после root recreation.
- GPU resources пересоздаются после device restore.
- Camera/entity/projectile handles проверяются на alive.
- Stock visual/audio override снимается на disable.
- Archive/YAML/resource limits заданы.
- HTTP имеет allowlist, timeout и response limit.
- Intermod table version/size проверены.
- Diagnostics не содержат секретов.
- Нет скрытых enemy data.
- Нет aim automation.
- Нет autofire.
- Нет packet injection/modify/drop/replay.
- Нет anti-cheat bypass.
- Нет изменения server hitbox, armor, ballistics, penetration или damage.

## 57. Exact-fingerprint native validation

Внутренний package `wotbmod.native_validation` находится в
`examples/native_validation_mod`. Он не расширяет public ABI: панель рисуется
через `wotbmod.render`, управление выполняется локальными hotkeys, а данные
берутся только из уже существующих V3 interfaces.

Package manifest ограничен клиентом `11.19.0.834` и executable SHA-256
`41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad`.
Runtime results дополнительно включают client/build/hash, loader SDK/bootstrap,
binding pack, validation-mod version и timestamp. При несовпадении
`fingerprint_key` прежние verdicts не загружаются.

Для каждой из 92 capability сохраняются section, interface, binding ID,
backend availability, status/verdict, call count, last result/error,
callback thread, event time, opaque handle/generation и active-handle count.
Native status ограничен `LIVE_TEST_PENDING`, пока пользователь не нажмёт PASS.
Намеренно отключённый или отсутствующий backend остаётся `NOT_SUPPORTED` даже
после ручного подтверждения ожидаемого отказа.

`LIVE_EVENT_TRACE.jsonl` содержит только bounded summaries. RPC сохраняет
public entity ID, стабильный method key, direction, timestamp и сведения о
наличии схемы/размера; payload bytes не записываются. Native D3D11 pointers
сохраняются только как `present/absent`. Chat, account tokens, персональные
данные, writable pointers и hidden-entity state запрещены.

Экспериментальный native test предварительно записывает
`ACTIVE_NATIVE_TEST.json`. Если процесс завершился до очистки marker, следующий
запуск помечает соответствующий test как crash-disabled. DLL никогда не
выгружается непосредственно из callback; callback-вариант reload проходит
только через loader-owned deferred queue.

Текущие честные границы:

- portable YAML/DVPL/ZIP доступны;
- loader-private typed DAVA registry и portable YAML/archive snapshots
  host-tested; exact YAML/ResourceArchive capability реализована для
  `11.19.0.834` и получила live `PASS`;
- UI V3 инспектирует и меняет active tree; private reviewed class registry
  допускает только `DAVA::UIControl`, `DAVA::Entity`, `DAVA::NMaterial`,
  `DAVA::Texture`, `DAVA::Mesh` и `DAVA::MeshConsumer`; произвольный class/FFI
  и полная enumeration всех game-owned Scene/Material не публикуются;
- exact `NMaterial`/`Texture` mutation+apply и mesh load/hot-swap реализованы
  через typed private handles и получили live `PASS`;
- projectile V2 lifecycle и managed impact Scene visuals доступны; loader-private
  stock tracer create route получил live `PASS`, но public tracer
  style/lifecycle object остаётся `E_NOT_SUPPORTED`;
- vehicle skin V2 подменяет exact resource paths и LOD транзакционно, но
  cached appearance требует model reload;
- WndProc/DAVA input ingress, UI event routing, managed layout/draw_mesh и
  per-mod callback budget подключены;
- binding pack проверяется автоматически, но новые RVA не генерируются;
- package trust использует ECDSA P-256/SHA-256; READY остаётся loadability,
  signature verdict читается отдельно;
- lifecycle `request_reload` ставит перезагрузку в очередь и выполняет её на
  границе кадра (unload через `DestroyMod` + `FreeLibrary`, затем повторный
  package preflight по id); цикл покрыт `tests/v3_package_runtime_tests.cpp`,
  живой статус — `LIVE_TEST_PENDING`; counters, cleanup и 30-cycle evidence
  сохраняются;
- секция `Hooks` содержит по строке на каждое из 42 имён, публикуемых
  `resolve_symbol`. Проба вызывает `hooks->create_symbol` в режиме `OBSERVE`
  с нулевыми флагами и no-op callback нужной формы: на 22 именах с точкой
  наблюдения в лоадере наблюдатель реально подключается и тут же снимается
  («OBSERVE attached and detached»), на остальных ожидается отказ «signature
  of this target is not described». Клавиша `H` прогоняет все пробы разом.

Пошаговый live-сценарий: `MANUAL_LIVE_VALIDATION_RU.md`. Runtime artifacts
пишутся в `mods/data/wotbmod.native_validation`; итоговый ZIP создаётся
`tools/export_live_validation_bundle.ps1` только после проверки совпадения
fingerprint matrix/results.

## 58. Контракт 2026-08-16: пять новых declaration-only интерфейсов

Пять интерфейсов ниже добавлены **после** RC1 и объявлены осознанно, как новый
release contract, а не как правка RC1. `API_V3_RC1_FREEZE.md` запрещает
добавлять slot в уже опубликованную таблицу; поэтому ни одна существующая
таблица не расширена. Вместо этого рядом объявлены новые версионированные
таблицы под новыми interface ID — ровно так, как уже сосуществуют
`WotbModV3UiApiV2`/`V3` и `WotbModV3DevtoolsApiV1`/`V2`/`V3`.

**Backend ни у одного из них нет.** Каждый slot возвращает
`WOTBMOD_V3_E_NOT_SUPPORTED`, а `kInterfaceAvailabilityDefaults` держит для них
`UNAVAILABLE` (не `DEGRADED`: `DEGRADED` означает «часть работает», а здесь не
работает ничего). Новых permission не введено — все пять переиспользуют уже
замороженные имена.

### 58.1 `wotbmod.ui.read` — `WotbModV3UiApiV4`

`control_get_text`, `control_get_texture`, `control_get_font`,
`control_get_style`. Reverse engineering не участвует: loader держит
собственную C++ копию всего, что этот API записал (`NativeUi::text/texture/font`
и стили), а `UiSetString` коммитит зеркало только после успешного host-вызова.
Поэтому getter возвращает ровно «последнее значение, которое этот API успешно
записал».

Четыре зеркальных getter-а движок не читают; флаг
`WOTBMOD_V3_UI_READ_GAME_OWNED` честно предупреждает, что игра могла изменить
контрол мимо зеркала. Флаги `*_SET` дают провенанс каждого поля: снятый бит
означает «значение не измерялось», а не «значение равно нулю».

`control_get_live_text` (4 сентября 2026) — единственный slot, который читает
**движок**: UTF-8 строку `DAVA::UITextComponent` контрола (11.20.0.887: vftable
RVA `0x0329AAA8`, `std::string` по `+0x44`; второй принимаемый класс —
`DAVA::UIDynamicAtlasTextComponent`, RVA `0x032B6D58`, текст по `+0x60`). Компонент
ищется сначала по полю `UIStaticText+0x140`, затем в таблице компонентов
контрола (`+0xE4`, вектор векторов по типам). Работает и на game-owned контролах
(tier REVIEWED, `ui.modify.game`), и на созданных этим API. Провайдер лоадера
находит список компонентов контрола во время выполнения — пара `{begin, end}`
внутри `UIControl`, каждый элемент которой указывает обратно на контрол через
`UIComponent+0x08` — и кэширует смещение; до проверки ничего не
разыменовывается, чтение идёт под SEH. Ответы: `E_NOT_FOUND` — у контрола нет
текстового компонента (`flags` без `LIVE_TEXT_SET`), `E_NOT_SUPPORTED` — у
лоадера нет live-text бэкенда. Статус `LIVE_TEST_PENDING` до строки
`ui.live_text` validation-мода. Маршрут «найти штатный элемент»:
`get_active_screen` → `control_find_by_name`/`control_get_child_at` →
`control_get_snapshot` (rect, видимость) → `control_get_live_text`.

Это закрывает пункт `API_V3_RC1_FREEZE.md` про отсутствие text getter в RC1.
`sample.ui_transaction` при этом остаётся с `text=NOT_EXPOSED_RC1` — sample
пинится к RC1 и меняется отдельной работой, когда backend появится.

### 58.2 `wotbmod.camera.state` — `WotbModV3CameraApiV2`, READ ONLY

Два независимых доказанных значения:

- `CameraController+0x5C` — это машина состояний **контроллеров анимации**, а
  не enum режимов камеры. Семь состояний; имена 2/3/4/6 восстановлены по RTTI
  (`LookOut`, `PostMortem`, `LookOnTarget`, `Observer`), 5 — начальное/пустое.
  Состояния **0 и 1 намеренно не названы**: статические данные противоречивы,
  поэтому они опубликованы как `ORDINARY_A` и `ORDINARY_B`. Это **не** аркада и
  снайпер.
- `GameCamera+0x320` — отдельный 2-значный индекс, и вот он про аркаду/снайпер.
  **Коррекция:** `0 = SNIPER`, ненулевое = `ARCADE`. Ранее опубликованная
  таблица «0=ARCADE, 1=SNIPER» была **инвертирована**; она исправлена в
  `re_anchors.md` 2026-08-15, и ни один backend не должен возить старую
  раскладку.

Setter'а состояния в этой таблице нет и не будет. Политику переходов движок
проверяет **на восьми местах вызова**, а не внутри `SwitchState` — сам
`SwitchState` примет любое число. Прямой вызов обошёл бы единственную проверку
и мог бы, например, выдернуть камеру из `PostMortemAnimationController`, пока
тот владеет анимацией смерти. Непредставимость записи здесь — и есть смысл
интерфейса.

### 58.3 `wotbmod.audio.intercept` — `WotbModV3AudioApiV3`

Перехват vtable-слота `+0x0C` (`CreateSoundEvent`). `DAVA::FastName` — один
dword с интернированным `const char*`, поэтому сравнение имени — это `strcmp`
без аллокаций и блокировок. Ровно три исхода: `PASS_THROUGH`, `SUBSTITUTE`
(вызвать оригинал с другим именем), `SUPPRESS`.

`SUPPRESS` обязан вернуть **штатный `SoundEventStub` движка**, никогда не
`nullptr`: шесть из восьми реальных мест вызова не проверяют результат и пишут
его прямо в поле. В ABI это сделано невозможным по построению — callback
возвращает решение и, максимум, имя; ни один `SoundEvent`-указатель или handle
не пересекает границу ни в одну сторону, так что выражения «вернуть null»
просто не существует.

Детур исполняется на **произвольном потоке** (путь создания защищён мьютексом
внутри `sub_24F39C0`, то есть движок сам допускает конкурентность) и способен
реентрировать, потому что гибридная система зовёт публичный путь внутренней.
ABI спроектирован так, чтобы thread-local флага хватило: из callback нельзя
создать звук через этот интерфейс, а `is_intercept_active` публикует тот же
флаг, чтобы остальные audio-операции честно падали в `WOTBMOD_V3_E_BUSY`.
Фильтр имени — только точное совпадение; префиксы и «перехватить всё»
намеренно невыразимы.

### 58.4 `wotbmod.scene.enumerate` — `WotbModV3SceneApiV2`, READ ONLY

Доказанная раскладка: дети — непрерывный `Entity*[begin, end)` на
`Entity+0x08/+0x0C` (capacity `+0x10`), parent `+0x18`, имя — `FastName` (один
dword, интернированный `const char*`, **не** refcounted) на `+0x1C`,
`TransformComponent*` на `+0x3C`, мировая `Matrix4` на `TC+0x60`.

`AddNode`/`RemoveNode` двигают вектор через `memmove` **без блокировок**, а
`RemoveNode` делает `Release` ребёнка сразу после уплотнения. Ни лока, ни
счётчика версий нет, поэтому обход обязан быть только в главном потоке — и это
сделано **контрактом ABI, а не надеждой**:

- нет ни handle, ни курсора, ни итератора, ни объекта узла: ничего не переживает
  вызов и, значит, ничего нельзя унести в другой поток или другой кадр;
- нет visitor-callback: он исполнялся бы посреди чужого вектора и дал бы моду
  точку реентранса в него;
- единственный вход — один синхронный вызов, который всё копирует до возврата и
  отвечает `WOTBMOD_V3_E_WRONG_THREAD` где угодно, кроме главного потока.

Наружу выходит только POD-клон: `index`, `parent_index`, `depth`, ограниченная
копия имени, 16 float мировой матрицы, `child_count`, флаг `IS_SCENE`. `index`
и `parent_index` — позиции в **собственном буфере вызывающего** и вне этого
вызова не значат ничего. Ни указатель, ни handle, производный от адреса, к моду
не попадает. Глубина, число детей на узел и суммарное число узлов ограничены;
буфер фиксированной ёмкости даёт вызывающий, и слишком маленький буфер — это
`WOTBMOD_V3_E_BUFFER_TOO_SMALL`, а не аллокация за его спиной.

### 58.5 `wotbmod.tracer` — `WotbModV3TracerApiV1`, READ ONLY

Таблица из 25 записей на `0x03FD6168` отображает байтовый код типа снаряда
(0..24; 25 и больше — ошибка, не clamp) на одно из восьми имён стиля:
`ARMOR_PIERCING`, `ARMOR_PIERCING_CR`, `HIGH_EXPLOSIVE`, `HOLLOW_CHARGE`,
`ANTI_TANK_GUIDED_MISSILE`, `RAILGUN`, `IMPROVED_DETECTION`, `STT_TRACER`.
Штатный RGBA по умолчанию лежит в `record+0x34`. Имя и цвет валидны независимо
друг от друга: имя приходит из статической таблицы, цвет — из runtime-записи,
и backend, у которого есть одно, не выдумывает второе.

Создание трассера непредставимо. `ShowTracer` и explosion-path — это research
anchors, а не backend: ownership, lifetime и раскладка аргументов не проверены
живьём. Это ровно тот отказ «вернуть OK, ничего не сделав, и упасть позже»,
который запрещает `API_V3_RC1_FREEZE.md`.

### 58.6 `wotbmod.ges` — `WotbModV3GesApiV1`

Внутренняя шина клиента `GES::GameEventSystem` (601 тип структур
`GES::<Owner>::<Name>` на 11.20.0.887) как system-события `events_v1`: топик
`wotbmod.ges.<Owner>.<Name>`, payload — `WotbModV3GesEvent` с именем типа,
указателем на объект события движка и честными флагами `SIZE_KNOWN` /
`SCHEMA_KNOWN`. Указатель живёт только внутри доставляющего колбэка; слоты
`read_i32/u32/f32/bool/ptr/cstring` читают по смещению с проверкой читаемости и
отвечают `WOTBMOD_V3_E_OBJECT_DESTROYED` на протухшее событие. Схемы
(`get_schema`, `schema_field`) есть только у типов, доказанных декомпиляцией
точки публикации; всё остальное отдаётся с `payload_size = 0`. Таблица схем
(`src/v3/ges_schemas.cpp`, 11.20.0.887):

| Тип | Размер | Поля | Источник |
| --- | --- | --- | --- |
| `Avatar::CameraModeChanged` | 8 | `mode` i32 @0, `flag` bool @4 | публикатор + живая трасса |
| `Avatar::BattleStatusUpdated` | 4 | `status` i32 @0 | публикатор `sub_157C5C0` |
| `Avatar::HighlightTankChanged` | 4 | `id` i32 @0 | публикатор `sub_15B1EA0` |
| `Avatar::VehicleExploded` | 4 | `source` ptr @0 | публикатор `sub_1670C50` |
| `Session::RoundFinished` | 8 | `value0` i32 @0, `value1` i32 @4 | публикатор `sub_15A6E10` |
| `InputMode::InputModeChanged` | 1 | `mode` u8 @0 | публикатор `sub_122E0F0` |
| `Avatar::PlayerRespawned`, `Avatar::PeriodBattleFinished` | 1 | `flag` bool @0 | публикатор |
| `Avatar::DirectShootReady` | 1 | `ready` bool @0 | публикатор |
| `Avatar::ArenaFreeze` | 1 | `frozen` bool @0 | публикатор |
| `HUDLayer::ChangeVisibility` | 1 | `visible` bool @0 | публикатор |
| `Avatar::PlayerDied`, `Avatar::PeriodBattleStarted`, `Avatar::AmmoExpanded`, `Avatar::UnlockZoom` | 1 | без полей | публикатор |

Записи от 4 сентября 2026 сняты только статически: размер и раскладка
доказаны, смысл полей с именами `flag`/`value*`/`id` не подтверждён живой
трассой. Вид поля `WOTBMOD_V3_GES_FIELD_U8` читается слотом `read_bool` и
означает байт-значение, а не истинность.
`publish` (REVIEWED, `ges.publish`) принимает только тип со схемой и payload
ровно её размера, выполняется на главном потоке и повторяет цикл рассылки
движка. Подписки на топики регистрируют слушателя в движке лениво, по первому
матчингу паттерна; источник — `WOTBMOD_V3_EVENT_SOURCE_GES`. Дизайн:
`docs/superpowers/specs/2026-09-03-ges-event-bus-design.md`.

Без native-бэкенда интерфейс `UNAVAILABLE`, `query_interface` отвечает
`NOT_SUPPORTED`; с бэкендом — `DEGRADED` и `LIVE_TEST_PENDING` до PASS
валидационного мода на этом fingerprint.

### 58.7 `wotbmod.session.cluster` — `WotbModV3SessionClusterApiV1` (API 1.1, 8 сентября 2026)

Кластер входа своего региона без перезапуска клиента. Клиент уже умеет
переключаться сам (`LoginManager::ChangeCluster`, `0x1CF1C40`): интерфейс
только читает его структуры и зовёт эту функцию.

- `enumerate(items, inout_count)` — кластеры региона (`WotbModV3ClusterInfo`:
  `cluster_id`, `name` «EU_C3», `current`, `alive` — какой-то адрес входа
  ответил на ping, `allowed` — клиент не пометил хост мёртвым, `ccu` = -1);
  двухпроходный (`items == NULL` — только счётчик). Живой 11.20.0.887 отдаёт
  три хоста (`EU_C0/EU_C3/EU_C4`), yaml публикует четыре.
- `get_current(out_info)` — куда подключён клиент сейчас
  (`ConnectionManager+12`).
- `change(cluster_id)` — `WOTBMOD_V3_SESSION_CLUSTER_AUTO` (-1) = штатный
  автовыбор: флаг ручного выбора снимается, затем
  `ConnectionManager::Disconnect(12)` → `HandleDisconnect` → `TryNextHost` →
  `DetermineBestCluster` (`ChangeCluster(-1)` из ангара клиент не переживает —
  диалог «Вы отключены от сервера», живая проверка 8 сентября 2026). Отказы до постановки в очередь: не `HANGAR` → `E_CONFLICT`;
  переключение в полёте или меньше 10 с после предыдущего → `E_BUSY`;
  неизвестный id → `E_NOT_FOUND`; кластер не `alive`/`allowed` → `E_CONFLICT`;
  backend не поднят или `LoginManager` ещё не захвачен → `E_NOT_SUPPORTED`
  (текст в `bootstrap.get_last_error`). Сам вызов уходит на главный поток.
- Событие `wotbmod.session.cluster.changed` (`WotbModV3ClusterChangedEvent`:
  `from_cluster_id`, `to_cluster_id`, `status` `QUEUED → STARTED → CONNECTED |
  FAILED`). `CONNECTED`/`FAILED` судятся возвратом контекста `HANGAR`
  (текущий кластер равен цели или, для AUTO, любой) либо таймаутом 60 с;
  `FAILED` снимает флаг «выбран вручную» (`LoginManager+796`), чтобы
  следующий вход снова был автовыбором.

Backend: детур `LoginManager::OnHostChosen` (`0x1D08FF0`) — единственная
точка, где клиент отдаёт `LoginManager*` при каждом входе; `services` =
`LoginManager+32`, от него `ConnectionManager` (`vtbl+100`), `Region`
(`vtbl+316 → +40 → +1592`, вектор `ClusterHost` по 208 байт, дескриптор
`+188`: имя `+0`, url `+24`, id `+120`). Права: `session.cluster.read` (SAFE),
`session.cluster.change` (REVIEWED — мод меняет цель сетевого подключения).
Lua: `wotb.session_cluster` (raw), фасад `wotb.session`.

Право `packages.manage` (REVIEWED, добавлено 8 сентября 2026) не имеет
C-интерфейса: его держит только loader-private библиотека Lua-хоста
`wotb.packages`, которая запускает `<игра>\wotbmod\wotbmod.exe` с закрытым
списком команд (см. `LUA_MODS_RU.md`, раздел `wotb.packages`). Native-мод с
этим правом ничего дополнительного не получает.

Судья `CONNECTED`: контекст `HANGAR` вернулся после не-ангара и держится не меньше
500 мс (`kHangarSettleMs`) — обработчики события не попадают на кадр, в котором клиент
ещё разрушает старую сцену; loader во время переключения (`SessionClusterChangeInFlight()`)
раз в ~60 кадров взводит полный UI-probe, иначе новый ангар классифицируется с опозданием
до минуты и судья даёт ложный `FAILED`.
