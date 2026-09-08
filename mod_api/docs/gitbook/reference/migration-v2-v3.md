# Миграция модов с API V2 на API V3

## Что изменилось принципиально

V2 остаётся совместимым слоем для уже собранных модов. V3 является новым ABI-контрактом и не передаёт модификации сырые указатели на внутренние таблицы V2, адреса клиента или `WotbModRuntimeFrame`.

Основные изменения:

1. Одна большая таблица заменена на версионированные интерфейсы, получаемые через `bootstrap->query_interface`.
2. Каждый мод получает собственный `WotbModV3Handle`; все создаваемые объекты имеют владельца и поколение.
3. Доступ проверяется одновременно по permission tier, текущему контексту игры и capability.
4. Наличие интерфейсной таблицы не означает наличие native backend. Конкретная операция обязана вернуть `WOTBMOD_V3_E_NOT_SUPPORTED`, `WOTBMOD_V3_E_CLIENT_MISMATCH` или `WOTBMOD_V3_E_PERMISSION_DENIED`, если выполнить её корректно нельзя.
5. Жизненный цикл и выгрузка имеют барьер callback: runtime прекращает принимать новую работу, отменяет принадлежащие моду задачи и хуки, ждёт завершения активных callback, вызывает `on_disable`, освобождает owned handles и вызывает `on_unload`.
6. BigWorld RPC доступен только как наблюдение метаданных уже прошедших штатных RPC. Payload, отправка, изменение, drop и replay отсутствуют в публичном API.

## Новый entry point

V2 экспортировал старую функцию загрузки и заполнял монолитную структуру. V3 экспортирует `WotbModLoadV3`:

```cpp
#include "wotb_mod_api_v3.h"

#include <cstring>

namespace {

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    const void* table = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            &table) != WOTBMOD_V3_OK) {
        return;
    }
    const WotbModV3CoreApiV1* core =
        static_cast<const WotbModV3CoreApiV1*>(table);
    core->log(
        mod,
        WOTBMOD_V3_LOG_INFO,
        "example",
        "V3 mod enabled");
}

}

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info ||
        bootstrap->api_version != WOTBMOD_V3_ABI_VERSION ||
        mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    CopyText(out_info->id, sizeof(out_info->id), "example.migrated");
    CopyText(out_info->name, sizeof(out_info->name), "Migrated mod");
    CopyText(out_info->version, sizeof(out_info->version), "3.0.0");
    CopyText(out_info->author, sizeof(out_info->author), "Developer");
    out_info->on_enable = &OnEnable;
    return WOTBMOD_V3_OK;
}
```

Runtime сначала ищет `WotbModLoadV3`. Если символ отсутствует, он может перейти к legacy entry point. Один DLL не должен одновременно полагаться на обе модели состояния.

## Правильный запрос интерфейса

Запрашивайте только минимальную поддерживаемую версию и всегда проверяйте код результата:

```cpp
template <typename T>
const T* Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t minimum_version) {
    const void* table = nullptr;
    const WotbModV3Result result =
        bootstrap->query_interface(
            mod,
            name,
            minimum_version,
            &table);
    if (result != WOTBMOD_V3_OK) return nullptr;
    return static_cast<const T*>(table);
}
```

После успешного запроса проверьте:

- `table != nullptr`;
- `table->struct_size >= sizeof(ожидаемый_тип)`;
- `table->api_version >= минимальная_версия`;
- нужный function pointer не равен `nullptr`.

Не кэшируйте raw native pointer из `wotbmod.render.native` между кадрами или после `device_lost`. Интерфейсную таблицу можно кэшировать до `on_disable`; handles и native objects имеют отдельный срок жизни.

## Соответствие основных областей V2 и V3

| Задача V2 | Интерфейс V3 | Миграционное действие |
|---|---|---|
| Лог, кадр, пути, контекст | `wotbmod.core` | Передавать собственный `mod` в каждый вызов |
| Проверка доступности функции | `wotbmod.capabilities` и `get_interface_info` | Проверять до включения опциональной функции и реагировать на change callback |
| Ручное хранение объектов | `wotbmod.handles` | Хранить handle, не указатель; освобождать через профильный API или `handles.release` |
| Enable/disable/unload | `wotbmod.lifecycle` и callbacks в `WotbModV3Info` | Перенести регистрацию в `on_enable`, прекращение работы в `on_disable`, финальное освобождение внешних ресурсов в `on_unload` |
| Native hook по RVA | `wotbmod.hooks` или `wotbmod.unsafe.native` | Предпочитать symbol/vtable hook; адресный hook требует UNSAFE и остаётся привязанным к build |
| Собственные события | `wotbmod.events` | Использовать topic, приоритет и owner-bound subscription token |
| Настройки | `wotbmod.settings` | Зарегистрировать schema version и definitions, затем читать typed getters |
| Произвольный файл мода | `wotbmod.storage` | Использовать ключи и scoped paths вместо записи в каталог игры |
| Горячие клавиши | `wotbmod.input` | Зарегистрировать action и context mask; не перехватывать Win32 сообщения вручную |
| Поиск ресурсов | `wotbmod.vfs` | Использовать `mod://`, `game://`, overlay providers и нормализацию URI |
| Загрузка байтов | `wotbmod.resources` | Выбрать expected type, max size и optional SHA-256 |
| YAML/DVPL/archive | `wotbmod.data.yaml`, `wotbmod.archive`, `wotbmod.loaders` | Задать limits; не вызывать внутренние DAVA функции напрямую |
| UIControl | `wotbmod.ui` V3 | Создавать owned controls либо получать active screen/tree snapshot; для game-owned mutation запрашивать `ui.modify.game` |
| Scene object и модель | `wotbmod.scene` | Использовать scene handles, attachment policy, transform и material parameters |
| Кастомный звук | `wotbmod.audio` V2 | Создавать audio по VFS URI; штатные sound events менять через override/event methods |
| Present/D3D drawing | `wotbmod.render` | Регистрировать callback по render phase и рисовать через portable commands |
| ID3D11 pointers | `wotbmod.render.native` | Запрашивать на нужном кадре; требуется UNSAFE |
| GameCamera | `wotbmod.camera` | Получать active camera handle и проверять текущий game context |
| FOV/zoom/free camera | `wotbmod.gameplay.camera` | Использовать gameplay-tweak слой вместо записи полей объекта |
| Шкурка/mesh/material танка | `wotbmod.vehicle.visual` V2 | Зарегистрировать transactional exact-path skin pack с отдельными LOD; после apply учитывать `requires_model_reload` и иметь rollback |
| HUD, ангар, replay | `wotbmod.gameplay.hud`, `wotbmod.gameplay.hangar`, `wotbmod.gameplay.replay` | Вызывать только в разрешённом контексте |
| BigWorld entity | `wotbmod.entity.public` | Читать только публичный snapshot видимых клиенту объектов |
| BigWorld RPC hook | `wotbmod.bigworld.rpc` | Подписываться только на direction/method metadata |
| Shell/tracer | `wotbmod.projectile` V2 | Читать provenance/valid-fields lifecycle и использовать managed impact visual; stock tracer style не считать доступным без capability |
| LeaveToHangar | `wotbmod.client.leave_to_hangar` | Вызывать только после проверки capability и состояния клиента |
| DeviceInfo/DXGI adapter | `wotbmod.client.device` | Читать нормализованную информацию без доступа к внутренней структуре игры |
| Thread/task/timer | `wotbmod.async` | Не создавать unmanaged worker, если работу можно оформить owner-bound task |
| HTTP | `wotbmod.http` | Обрабатывать `E_NOT_SUPPORTED`; сетевой backend может отсутствовать |
| API между модами | `wotbmod.intermod` | Экспортировать versioned service и освобождать export token |
| Отчёт/профилирование | `wotbmod.diagnostics`, `wotbmod.devtools` | Отправлять fault context и закрывать profiler spans |
| Метаданные пакета | `wotbmod.manifest`, `wotbmod.catalog`, `wotbmod.content` | Выполнять preflight до загрузки DLL |

## Handles вместо указателей

Каждый handle кодирует тип, slot и generation. Проверка владельца выполняется в runtime.

Правила миграции:

1. Поле класса с `UIControl*`, `Entity*`, `Sound*` или `Resource*` заменяется соответствующим `WotbModV3UiHandle`, `WotbModV3SceneHandle`, `WotbModV3AudioHandle` или `WotbModV3ResourceHandle`.
2. Нулевое значение заменяется на `WOTBMOD_V3_INVALID_HANDLE`.
3. Перед отложенным использованием вызывается `handles.is_alive`.
4. Shared ownership оформляется `handles.retain` и `handles.release`.
5. Профильная операция `destroy`, `unregister`, `unmount` или `unsubscribe` предпочтительнее общего `release`, потому что она выполняет доменную очистку.
6. Handle другого мода нельзя использовать как свой. Ожидаемый ответ — `WOTBMOD_V3_E_INVALID_HANDLE` или `WOTBMOD_V3_E_PERMISSION_DENIED`.
7. После `on_disable` нельзя планировать callback, использующий сохранённые owned handles.

## Permission tier и manifest

Запрошенный tier в `WotbModV3Info` является верхней границей запроса, а не автоматическим разрешением. Loader выдаёт фактический tier по политике установки.

- `SAFE`: локальные данные мода, собственный UI, собственные ресурсы, базовые события.
- `GAMEPLAY_TWEAK`: косметические и клиентские изменения gameplay UI/camera/vehicle visuals.
- `REVIEWED`: воздействие на существующий UI, render callbacks, публичные battle entities, observed RPC metadata, projectile events, LeaveToHangar, allowlisted HTTP.
- `UNSAFE`: native render pointers, address hooks и другие явно native операции.

Если V2-мод использовал фиксированный RVA, его manifest должен явно запросить native permission и ограничить `client.builds` и `client.executable_hashes`. Отсутствие hash/build match должно отключать соответствующую capability, а не переключать мод на неизвестный адрес.

## Контексты

V3 различает:

- `LOADING`;
- `HANGAR`;
- `BATTLE`;
- `REPLAY`;
- `TRAINING`;
- `RESULTS`;
- `MOD_SCREEN`;
- `TEXT_INPUT`.

Перед переносом вызова проверьте контекст через `core.get_context`. Например:

- hangar background вызывается только в `HANGAR`;
- HUD вызывается в `BATTLE`, `REPLAY` или `TRAINING`;
- replay controls вызываются только в `REPLAY`;
- entity/RPC/projectile наблюдение доступно в `BATTLE`, `REPLAY` или `TRAINING`;
- camera доступна в `HANGAR`, `BATTLE`, `REPLAY` или `TRAINING`.

Не используйте повторный вызов каждый кадр как обход контекстного отказа. Подпишитесь на `wotbmod.game.state_changed` или профильное событие и повторите операцию после перехода в допустимый контекст.

## Ошибки и fallback

Каждый вызов возвращает `WotbModV3Result`. После ошибки можно получить thread-local подробности через `bootstrap->get_last_error`.

Рекомендуемая обработка:

| Код | Действие мода |
|---|---|
| `WOTBMOD_V3_E_NOT_SUPPORTED` | Отключить только зависимую функцию и оставить мод работоспособным |
| `WOTBMOD_V3_E_PERMISSION_DENIED` | Показать требуемое разрешение; не повторять вызов без изменения grant |
| `WOTBMOD_V3_E_CLIENT_MISMATCH` | Отключить native feature и вывести client version/hash |
| `WOTBMOD_V3_E_WRONG_THREAD` | Перенести вызов через `wotbmod.async` |
| `WOTBMOD_V3_E_INVALID_HANDLE` | Удалить stale handle из состояния |
| `WOTBMOD_V3_E_OBJECT_DESTROYED` | Пересоздать объект после соответствующего ready event |
| `WOTBMOD_V3_E_BUFFER_TOO_SMALL` | Использовать размер, возвращённый через `inout_size`, и повторить чтение |
| `WOTBMOD_V3_E_CANCELLED` | Завершить owner-bound операцию без повторного запуска при unload |
| `WOTBMOD_V3_E_HASH_MISMATCH` | Не загружать пакет или ресурс |
| `WOTBMOD_V3_E_SIGNATURE_INVALID` | Не устанавливать пакет |

Не трактуйте `E_NOT_SUPPORTED` как успешное выполнение. Это честный ответ runtime о том, что безопасного backend сейчас нет.

## Перенос UI

Порядок:

1. Получить `WotbModV3UiApiV2`.
2. Найти named slot через `slot_find`.
3. Создать control или compound widget.
4. Задать ID, layout, position, size, style и accessibility label.
5. Подписаться на UI event.
6. Подключить control через `slot_attach`.
7. На disable выполнить `event_unsubscribe`, `slot_detach`, `control_destroy`.

Изменение штатного UI требует более высокого permission, чем работа с собственным деревом. Если slot отсутствует на текущем экране, дождитесь `wotbmod.ui.screen_changed` или `wotbmod.ui.root_ready`.

## Перенос моделей и шкурок

Для собственного scene object:

1. Загрузить модель через `scene.entity_load`.
2. Проверить доступные nodes и animations.
3. Настроить transform, render layer, material и shader parameters.
4. Присоединить к scene или vehicle attachment point.

Для полной визуальной замены танка:

1. Подготовить `WotbModV3VehicleVisualProfile`.
2. Указать `vehicle_name`, URI hull/turret/gun/chassis, material overlay и texture pack.
3. Вызвать `profile_register`.
4. После `wotbmod.vehicle.local.appearance_ready` получить vehicle handle.
5. Вызвать `profile_apply`.
6. На смене appearance повторно применить профиль.
7. На disable вызвать `restore_appearance` и `profile_release`.

Это клиентская визуальная подмена. Она не меняет серверную collision, hitbox, характеристики, баллистику или сетевую сущность.

## Перенос аудио

Кастомный файл должен находиться в package resources и открываться через VFS URI. `audio.create` и `audio.create_stream` не принимают произвольный raw Win32 path.

Порядок:

1. Сформировать `WotbModV3AudioDescriptor`.
2. Выбрать flags: spatial, loop, stream.
3. Создать handle.
4. Для короткого звука вызвать `preload`, затем `play`.
5. Для 3D-звука задать position, min distance и max distance.
6. Для штатного события использовать `sound_override_register` или `sound_event_create`.
7. На disable снять subscriptions/overrides и уничтожить audio handles.

Если native sound engine не подключён, операция возвращает `WOTBMOD_V3_E_NOT_SUPPORTED`. Мод не должен показывать пользователю сообщение «замена применена» до `WOTBMOD_V3_OK`.

## Перенос хуков

Предпочтительный порядок:

1. Публичное событие V3.
2. Публичный domain interface.
3. `hooks.create_symbol`.
4. `hooks.create_vtable`.
5. `unsafe.native.create_address_hook`.

Для hook задаются mode, priority и ordering constraints. Перед включением проверьте `get_conflicts`. Для around chain используйте `call_next`; не вызывайте сохранённый trampoline напрямую после unload.

Фиксированный RVA допустим только в UNSAFE-моде с точным executable SHA-256. При несовпадении hash hook не создаётся.

## Перенос BigWorld и игровых событий

Вместо чтения внутренних entity maps используйте `entity.public.enumerate_visible` и `get_snapshot`. Runtime обязан фильтровать данные по фактической видимости для локального клиента.

Вместо detour RPC dispatcher используйте `bigworld.rpc.subscribe_observed`. Callback получает:

- направление;
- публичный entity ID;
- sequence;
- timestamp;
- публичный entity type;
- имя метода.

Callback не получает аргументы или packet bytes. Публичный интерфейс не содержит send, modify, drop или replay.

Для выстрелов используйте:

- `wotbmod.gameplay.local_shell_fired`;
- `wotbmod.gameplay.visible_tracer_created`;
- `wotbmod.gameplay.visible_tracer_destroyed`;
- `wotbmod.gameplay.projectile.created/updated/impacted/destroyed`;
- `projectile.tracer_style_register`;
- `projectile.projectile_attach_visual`;
- `projectile.impact_visual_register`.

Lifecycle V2 и managed impact Scene visual работают по подтверждённым
shot/impact ingress. Stock tracer style/attachment требуют отдельного native
ownership-safe backend и сейчас могут вернуть `E_NOT_SUPPORTED`. Скрытые enemy
projectiles и непринятые серверные данные API не раскрывает.

## Packaging

V3 package проходит preflight до `LoadLibrary`:

1. Прочитать `manifest.json`.
2. Проверить schema и package type.
3. Проверить ID/version.
4. Разрешить dependencies.
5. Проверить client build/hash allowlist.
6. Проверить catalog status и SHA-256.
7. Определить permission tier.
8. Для native package выбрать `entrypoints.windows-x86`.
9. Для content-only package прочитать content descriptor без загрузки DLL.
10. Только после успешного preflight загрузить native entrypoint.

Поддерживается ECDSA P-256/SHA-256. Ключ доверия хранится в
`mods/trust/keys/<key_id>.p256`, detached sidecar — рядом с package. Invalid
signature блокирует загрузку; unknown signer остаётся warning либо блокируется
policy `REQUIRE_TRUSTED_SIGNATURE`. `PACKAGE_PLAN_READY` означает loadability;
trust проверяется отдельно по `signature_status`.

## Проверка миграции

Минимальный набор проверок:

- DLL собирается как Windows x86 с `/W4 /WX`;
- экспорт содержит `WotbModLoadV3`;
- `id` стабилен и совпадает с manifest;
- version является semver;
- каждый запрошенный interface обрабатывает отказ;
- permission tier не ниже фактически используемых операций;
- native функции ограничены build/hash;
- все subscription, callback, task, timer, hook, render callback и override снимаются;
- все created handles уничтожаются или освобождаются;
- callback после `on_disable` не обращается к состоянию мода;
- UI переживает смену screen/root;
- scene/vehicle visuals переживают уничтожение appearance;
- audio переживает смену audio device;
- render переживает device lost/restored и swapchain resize;
- работа в hangar, battle, replay и training проверена отдельно;
- `E_NOT_SUPPORTED` не отображается как успех;
- отсутствуют чтение скрытых enemy data, aim automation, autofire, packet injection, anti-cheat bypass и изменение серверных hitbox.

Два готовых V3 probe-мода находятся в:

- `examples/v3_core_runtime_test_mod`;
- `examples/v3_client_resources_test_mod`.

Они запрашивают все 40 интерфейсов, проверяют ABI/function slots и выполняют только безопасные наблюдающие операции либо проверку ожидаемого `WOTBMOD_V3_E_NOT_SUPPORTED`.
