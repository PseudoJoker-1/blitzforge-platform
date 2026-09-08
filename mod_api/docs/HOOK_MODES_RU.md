# Режимы хуков: что можно синтезировать, а что нельзя

Разбор от 2026-08-22 против клиента `11.19.0.834`. Документ появился потому,
что действующее объяснение в `API_V3_RU.md` верно по выводу, но слишком широко
по обоснованию — и из-за этого закрывает дорогу к варианту, который на самом
деле реализуем.

## Что есть сейчас

`include/wotbmod/hooks_v1.h` замораживает пять режимов:

```c
WOTBMOD_V3_HOOK_BEFORE  = 0,
WOTBMOD_V3_HOOK_AFTER   = 1,
WOTBMOD_V3_HOOK_AROUND  = 2,
WOTBMOD_V3_HOOK_REPLACE = 3,
WOTBMOD_V3_HOOK_OBSERVE = 4
```

`NativeModeSupported()` в `src/v3/runtime_services.cpp` принимает только
`AROUND` и `REPLACE`. Остальные три получают `E_NOT_SUPPORTED` с текстом
«native backend supports only AROUND and REPLACE modes», либо, с флагом
`ALLOW_PENDING`, повисают в `PENDING_BACKEND`.

Действующее объяснение в `API_V3_RU.md`:

> Универсальные `BEFORE`, `AFTER` и `OBSERVE` нельзя корректно синтезировать
> без описания сигнатуры target.

## Почему это правда

Backend — сырой трамплин:

```c
WotbModV3Result create(void* user_data, void* target,
                       void* detour, void** out_original);
```

Функция мода **становится** заменой целевой. Разницы между `AROUND` и
`REPLACE` на уровне установки нет вообще: оба ставятся одинаково, различаются
только контрактом (кто обязан вызвать оригинал) и правилами конфликтов.

Чтобы получить `BEFORE`, рантайм должен вставить свой переходник, который
получит те же аргументы, вызовет callback мода, затем вызовет оригинал и
вернёт его результат. Аргументы лежат на стеке, их количество и типы
рантайму неизвестны, а на x86 ещё и соглашение о вызове (`cdecl` /
`stdcall` / `thiscall`) определяет, кто чистит стек. Скопировать кадр
аргументов неизвестного размера нельзя. Для `AFTER` добавляется сохранение
возвращаемого значения — `EAX`/`EDX` для целых, стек x87 для `double`,
`XMM0` для SSE — вокруг вызова чужого кода.

Так что для **произвольной** цели вывод верен.

## Где обоснование слишком широко

Слово «универсальные» делает всю работу. Оно не покрывает случай, когда
сигнатура **известна**, а у нас таких целей 42.

`WotbModV3NativeBindings_ResolveHookSymbol` публикует 35 отревершенных целей
под 42 именами. Все они прошли ревью, все резолвятся на этой сборке (проверено:
42/42 в live-прогоне). Если для каждой известна сигнатура, переходник для неё
не синтезируется вслепую — он **генерируется типизированно**, обычным C++, без
единой строки ассемблера:

```cpp
// Пример: цель void __thiscall Vehicle::showShooting(void* self, int shellId)
void __fastcall ObserveThunk_ShowShooting(void* self, void*, int shellId) {
    auto detour = reinterpret_cast<void(__fastcall*)(void*, void*, int)>(
        g_hooks[kShowShooting].detour);
    detour(self, nullptr, shellId);          // callback мода, результат не нужен
    auto original = reinterpret_cast<void(__fastcall*)(void*, void*, int)>(
        g_hooks[kShowShooting].original);
    original(self, nullptr, shellId);        // оригинал выполняется всегда
}
```

Компилятор сам расставляет соглашение о вызове, сам чистит стек, сам
сохраняет возвращаемое значение. Ничего не изобретается.

**Важно: замороженный заголовок при этом не меняется.** Контракт остаётся
ровно тем же, что уже записан для `get_original`: «разработчик вызывает его с
точной native-сигнатурой target». `OBSERVE` означает «твоя функция получает
настоящие аргументы, её результат отбрасывается, оригинал выполняется всегда».
`BEFORE` — то же самое, но с правом отменить вызов (тогда нужен ещё и
осмысленный возврат по умолчанию, что для части целей неопределимо).
`AFTER` — вызвать оригинал, затем callback.

## Что сделано (4 сентября 2026)

`OBSERVE` и `AFTER` работают на 22 опубликованных именах — тех, за которыми
стоят функции, на которых лоадер уже держит собственный детур. Механизм
(spec `docs/superpowers/specs/2026-09-04-hook-observers-design.md`):

- `loader/v3_hook_observers.{h,cpp}` — реестр наблюдателей по адресу цели с
  неизменяемыми снимками списка; детур лоадера вызывает `Before<Fn>()` до
  оригинала (это `OBSERVE`) и `After<Fn>()` после него (это `AFTER`) с
  настоящими аргументами цели. Порядок — по убыванию `priority`, затем по
  порядку подключения. Callback мода исполняется под SEH-обёрткой: упавший
  наблюдатель отключается и один раз пишется в лог лоадера
  (`[v3] hook observer faulted: ...`); оригинал выполняется всегда.
- Loader-private контракт `WotbModV3NativeHookBackend` версии 2 добавил слоты
  `describe_target`, `attach`, `detach`, `set_attached_enabled`; версия 1
  по-прежнему принимается и даёт только `AROUND`/`REPLACE`.
- Рантайм проверяет пару (режим, цель): `NativeModeSupported(mode, target)`.
  Тексты отказов: `BEFORE` — «BEFORE has no cancel protocol yet»; `OBSERVE`/
  `AFTER` на цели без точки наблюдения — «signature of this target is not
  described; OBSERVE and AFTER need a loader-side observer point»; бэкенд v1 —
  «native backend supports only AROUND and REPLACE modes». `get_original` для
  наблюдателя отвечает `E_NOT_SUPPORTED` («observer hooks do not own a
  trampoline»): у него нет трамплина. Два наблюдателя на одной цели не
  конфликтуют; `REPLACE` по-прежнему конфликтует со всеми.
- `AROUND`/`REPLACE` на этих 22 целях дают `E_CONFLICT`: MinHook уже занят
  детуром лоадера (`MH_ERROR_ALREADY_CREATED`). Это не изменилось, только
  записано.

Контракт для автора мода прежний: callback пишется с точной native-сигнатурой
цели (для `__thiscall` — идиома `__fastcall(self, edx, ...)`), результат
callback отбрасывается.

### Таблица сигнатур

| Опубликованное имя | Callback (`__thiscall`, если не сказано иное) |
| --- | --- |
| `Vehicle::onEnterWorld` | `void(void* self)` |
| `Vehicle::onLeaveWorld` | `void(void* self)` |
| `Vehicle::showShooting` | `void(void* self, const uint8_t* shotCode)` |
| `Vehicle::set_health` | `void(void* self, const int16_t* previousHealth)` |
| `Avatar::updateVehicleHealth` | `void(void* self, const int16_t* previousHealth, const uint8_t* flags)` |
| `ReloadTimer::setState` | `void(void* self, int32_t state, float value)` |
| `GameSceneController::OnVehicleHitDamage` | `uint8_t(void* self, uint32_t firstEntityId, uint32_t secondEntityId, const float* position, const void* firstDetails, const void* secondDetails, uint8_t shellKind, uint8_t flags, uint32_t shotId)` |
| `UIShellSelectorControl::OnCurrentAmmoChanged` | `int32_t(void* self, int32_t shellId, int32_t context)` |
| `DAVA::UIControl::SystemInput` | `bool(void* self, const void* uiEvent)` |
| `DAVA::File::Create` | `void* __cdecl(const DavaFilePath32* path, uint32_t attributes)` |
| `DAVA::Scene::Draw` / `Activate` / `Deactivate` | `void(void* scene)` |
| `GameCamera::GameCamera` / `::ctor` | `void*(void* self, uint8_t kind, uint32_t first, uint32_t second)` |
| `GameCamera::~GameCamera` / `::dtor` | `void*(void* self, uint8_t flags)` |
| `Client::Initialize` | `void(void* self)` |
| `GES::Avatar::CameraModeChanged` / `CameraModeChanged` | `bool(void* self, const int32_t* event)` |
| `BWEntity::BWEntity` / `::ctor` | `void*(void* self, int32_t entityId)` |
| `BWEntity::~BWEntity` / `::dtor` | `void(void* self)` |
| `TracerManager::TracerManager` / `::ctor` | `void*(void* self, void* first, void* second)` |
| `TracerManager::~TracerManager` / `::dtor` | `void*(void* self, uint32_t deleteFlags)` |
| `TracerManager::ShowTracer` | `void(void* self, const float* origin, const uint32_t* sourceId, const float* destination, const uint32_t* destinationId, const uint8_t* shellType, const float* parameter, uint32_t count)` |

Цель попадает в `describe_target` только когда её детур реально установлен на
этой сборке; иначе ответ такой же, как для неописанной цели.

Проверки: `tests/v3_hook_observers_tests.cpp` (реестр на двойнике детура),
`tests/v3_runtime_services_compile.cpp` (гейт режимов и жизненный цикл записи
через фейковый бэкенд v2), проба `H` validation-мода (живое подключение и
отключение `OBSERVE` на 22 именах).

## Что осталось

- `BEFORE`: нужен протокол отмены вызова и подменного результата; до него
  режим честно отклоняется.
- 20 опубликованных имён без детура лоадера (`Camera::setFOV`,
  `Client::LeaveToHangar`, `DAVA::Scene::Scene`, `DAVA::Entity::Entity`,
  `DAVA::EntityCache::LoadEntityUnsafe`,
  `DAVA::TransformComponent::SetLocalTransform`, UI-пакеты и контролы,
  `DAVA::EngineContext::GetInstance`): для них нужны сигнатуры из реверса и
  типизированные переходники — отдельный подпроект.
- Рантайм не узнаёт о падении наблюдателя: статус записи остаётся `ENABLED`,
  хотя лоадер её отключил. Нужен обратный вызов из реестра в рантайм.
