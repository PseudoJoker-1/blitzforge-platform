# WoT Blitz Mod API 2.9 — полная документация

Эта документация описывает публичный ABI для модов и runtime-интерфейс,
который встраивается в загрузчик. API рассчитан на нативные x86 DLL-моды и
отделён от UI загрузчика: список модов, настройки и визуальную оболочку можно
реализовать независимо.

Актуальные заголовки:

- `include/wotb_mod_api.h` — публичный ABI для разработчиков модов;
- `include/wotb_mod_runtime.h` — интеграция runtime в loader;
- `include/wotb_mod_windows_audio.h` — готовый custom-audio backend для
  Windows;
- `include/wotb_mod_dava_sound.h` — готовый bridge к native
  `DAVA::SoundSystem`;
- `../re_anchors.md` — адреса проверенных DAVA integration points.

Текущие версии:

| Компонент | Значение |
| --- | --- |
| ABI | `2.9`, `WOTBMOD_ABI_VERSION == 0x00020009` |
| Host | `1.9`, `WOTBMOD_HOST_VERSION == 0x00010009` |
| Архитектура текущей сборки | Windows x86 |
| Точка входа мода | `WotbModLoad` |
| Calling convention | `__cdecl` |

## 1. Модель работы

Система состоит из трёх уровней:

1. **Loader** внедряется или иным способом запускается внутри процесса игры.
   Он предоставляет backend для хуков, DAVA-ресурсов и звукового движка.
2. **Runtime** находит DLL в каталоге модов, проверяет ABI, управляет жизненным
   циклом, конфигурацией, ресурсами, ошибками и владением хуками.
3. **Mod DLL** экспортирует `WotbModLoad`, заполняет `WotbModInfo` и работает
   только через переданный `WotbModHostApi`.

Типовой поток:

```text
Loader worker thread
  -> WotbModRuntime_Initialize
  -> WotbModRuntime_LoadAll
       -> LoadLibrary(mod.dll)
       -> WotbModLoad
       -> on_enable

Render/frame integration
  -> WotbModRuntime_DispatchFrame
       -> main-thread queue (FIFO)
       -> client event callbacks (FIFO)
       -> on_frame каждого enabled-мода

Loader shutdown
  -> on_disable
  -> автоматическое удаление хуков и ресурсов
  -> on_unload
  -> FreeLibrary
```

## 2. Структура каталогов

Если `mods_directory` не задан, runtime использует `<game>\mods`.

```text
World of Tanks Blitz/
  wotblitz.exe
  mods/
    author.example.dll
    another_mod.dll
    mods.ini
    config/
      author.example.ini
      another_mod.ini
    data/
      author.example/
        resources/
          UI/
          3d/
      another_mod/
```

Имена каталогов `config` и `data` строятся из имени DLL без расширения, а не
из `WotbModInfo.id`.

Пример:

```text
DLL: mods\my-cool-mod.dll
Data: mods\data\my-cool-mod\
Config: mods\config\my-cool-mod.ini
```

`mods.ini` хранит глобальное состояние enable/disable:

```ini
[mods]
my-cool-mod=1
another_mod=0
```

Новый мод включён по умолчанию, если записи ещё нет.

## 3. Сборка SDK и тестов

Из каталога `_mod_tools\mod_api`:

```bat
build.cmd
```

Скрипт:

1. подключает x86 toolchain Visual Studio 2022;
2. компилирует runtime с `/W4`;
3. проверяет все публичные заголовки обычным C-компилятором;
4. собирает example/test DLL;
5. запускает базовый standalone smoke host;
6. запускает adversarial full-contract suite для всех host/runtime функций,
   ошибок, лимитов, ownership и malformed DLL.

Основные результаты:

```text
build\wotb_mod_runtime.lib
build\hello_mod.dll
build\smoke_host.exe
build\api_full_host.exe
```

Полная матрица сценариев поддерживается в
[`tests/API_TEST_MATRIX.md`](../tests/API_TEST_MATRIX.md), итоговый отчёт и
остаточные интеграционные риски — в
[`tests/API_TEST_REPORT.md`](../tests/API_TEST_REPORT.md).

## 4. Правила ABI

Публичный ABI является C-совместимым. Через границу DLL запрещено передавать:

- STL-контейнеры и строки;
- C++-классы DAVA;
- исключения C++;
- память, которую должен освобождать другой CRT;
- compiler-specific smart pointers.

Через ABI передаются только:

- целые числа фиксированной ширины;
- обычные C-строки;
- function pointers;
- opaque handles;
- структуры с `struct_size`.

Каждая расширяемая структура начинается с `uint32_t struct_size`. Перед
вызовом её нужно обнулить и установить фактический размер:

```cpp
WotbModResourceLoadRequest request = {};
request.struct_size = sizeof(request);
```

### Совместимость версий

Major-версия обязана совпадать:

```cpp
if (WOTBMOD_ABI_MAJOR(host->abi_version) !=
    WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
    return WOTBMOD_ERROR_UNSUPPORTED_ABI;
}
```

Minor-версия может отличаться. Новые поля добавляются только в конец структур.
Перед использованием добавленной функции проверяйте `host->struct_size`:

```cpp
static bool HasAudioApi(const WotbModHostApi* host) {
    return host &&
           host->struct_size >=
               offsetof(WotbModHostApi, audio_release) +
                   sizeof(host->audio_release) &&
           host->audio_play &&
           host->audio_release;
}

static bool HasCustomAudioFileApi(const WotbModHostApi* host) {
    return host &&
           host->struct_size >=
               offsetof(WotbModHostApi, audio_clip_load) +
                   sizeof(host->audio_clip_load) &&
           host->audio_clip_load;
}
```

## 5. Минимальный мод

```cpp
#include <string.h>
#include "wotb_mod_api.h"

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    host->log(mod, WOTBMOD_LOG_INFO, "enabled");
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    host->log(mod, WOTBMOD_LOG_INFO, "disabled");
}

WOTBMOD_ENTRY {
    if (!host || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "author.example";
    out_info->name = "Example Mod";
    out_info->version = "1.0.0";
    out_info->author = "Author";
    out_info->description = "Example native mod";
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_OK;
}
```

Экспорт должен называться строго `WotbModLoad`. Макрос `WOTBMOD_ENTRY`
создаёт правильное имя, calling convention и `extern "C"`.

## 6. `WotbModLoad` и `WotbModInfo`

Сигнатура:

```cpp
WotbModResult WOTBMOD_CALL WotbModLoad(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* out_info);
```

### Аргументы

| Аргумент | Назначение |
| --- | --- |
| `host` | Таблица функций runtime. Не освобождать и не изменять. |
| `mod` | Opaque handle текущего мода. Передавать обратно во все mod-scoped вызовы. |
| `out_info` | Дескриптор, который заполняет мод. |

### Поля `WotbModInfo`

| Поле | Требование |
| --- | --- |
| `struct_size` | Обязательно `sizeof(WotbModInfo)`. |
| `abi_version` | Обычно `WOTBMOD_ABI_VERSION`. |
| `id` | Уникальный id короче 64 байт. |
| `name` | Отображаемое имя. При `nullptr` используется `id`. |
| `version` | Строка версии. При `nullptr` используется `0.0.0`. |
| `author` | Автор, может быть `nullptr`. |
| `description` | Описание, может быть `nullptr`. |
| `flags` | Зарезервировано для будущего registry; сейчас ставить `0`. |
| `on_enable` | Вызывается при включении. Может быть `nullptr`. |
| `on_disable` | Вызывается при отключении. Может быть `nullptr`. |
| `on_unload` | Вызывается перед выгрузкой DLL. Может быть `nullptr`. |
| `on_frame` | Вызывается из `DispatchFrame`. Может быть `nullptr`. |

Допустимые символы `id`:

```text
A-Z a-z 0-9 . _ -
```

Сравнение id регистронезависимое. Два мода с одинаковым id загрузить нельзя.

Строки дескриптора должны оставаться валидными до `on_unload`. Текущий runtime
копирует их сразу, но мод не должен зависеть от этой детали реализации.

## 7. Жизненный цикл

### `on_enable`

```cpp
void OnEnable(const WotbModHostApi* host, WotbModHandle mod);
```

Вызывается:

- после успешного `WotbModLoad`;
- при ручном повторном включении;
- когда состояние мода уже переключено в `WOTBMOD_STATE_ENABLED`.

Здесь обычно:

- читают конфигурацию;
- создают и включают хуки;
- монтируют ресурсы;
- загружают UI package или scene resource.

### `on_frame`

```cpp
void OnFrame(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModFrameInfo* frame);
```

Вызывается только для enabled-модов. `frame` содержит:

| Поле | Значение |
| --- | --- |
| `frame_index` | Монотонный номер dispatch-вызова. |
| `delta_seconds` | Переданный loader delta или QPC-вычисление, если передано `<= 0`. |
| `swap_chain` | Opaque `IDXGISwapChain*`. |
| `device` | Opaque `ID3D11Device*`. |
| `device_context` | Opaque `ID3D11DeviceContext*`. |
| `back_buffer_width/height` | Размер backbuffer. |

Callback выполняется на том потоке, который вызвал
`WotbModRuntime_DispatchFrame`, обычно render thread. Нельзя выполнять здесь
долгий файловый I/O или блокировать поток ожиданием.

### `on_disable`

Runtime сначала ставит `enabled = 0` и состояние `DISABLED`, затем вызывает
callback. После возврата runtime принудительно:

- отключает и удаляет все оставшиеся хуки мода;
- останавливает и освобождает все оставшиеся audio playback handles;
- освобождает все оставшиеся resource handles;
- удаляет все оставшиеся resource mounts.

Мод может освободить их самостоятельно в `on_disable`; повторная runtime
очистка безопасна.

### `on_unload`

Вызывается при полном shutdown перед `FreeLibrary`. К этому моменту хуки,
audio playbacks и ресурсы уже удалены. Здесь следует освободить только
собственные объекты мода, потоки и память.

### Fault isolation

`WotbModLoad` и callbacks защищены Windows SEH. Если callback вызывает
access violation или другое SEH-исключение:

1. увеличивается `fault_count`;
2. мод получает состояние `WOTBMOD_STATE_FAULTED`;
3. мод выключается;
4. все его хуки, audio playbacks и ресурсы удаляются;
5. остальные моды продолжают работать.

Faulted-мод нельзя повторно включить через `set_mod_enabled` в текущей сессии.

## 8. Коды результата

| Код | Число | Значение |
| --- | ---: | --- |
| `WOTBMOD_OK` | 0 | Успех. |
| `WOTBMOD_ERROR_INVALID_ARGUMENT` | 1 | Нулевой/невалидный аргумент, структура или путь. |
| `WOTBMOD_ERROR_UNSUPPORTED_ABI` | 2 | Несовместимая major ABI. |
| `WOTBMOD_ERROR_NOT_FOUND` | 3 | Мод, mount, handle, файл или символ не найден. |
| `WOTBMOD_ERROR_ALREADY_EXISTS` | 4 | Runtime уже инициализирован, LoadAll уже вызван или hook уже существует. |
| `WOTBMOD_ERROR_PLATFORM` | 5 | Win32/backend operation завершилась ошибкой или backend отсутствует. |
| `WOTBMOD_ERROR_ACCESS_DENIED` | 6 | Чужой hook/mount, выход за data directory либо release audio clip с активным playback. |
| `WOTBMOD_ERROR_BUFFER_TOO_SMALL` | 7 | Буфер отсутствует или мал; требуемый размер возвращён отдельно. |
| `WOTBMOD_ERROR_DISABLED` | 8 | Runtime не активен либо мод выключен и пытается создать hook/mount/resource/playback. |
| `WOTBMOD_ERROR_LIMIT_REACHED` | 9 | Достигнут лимит модов, хуков, mounts, resources или audio playbacks. |
| `WOTBMOD_ERROR_CALLBACK_FAULT` | 10 | SEH fault в mod callback или backend callback. |
| `WOTBMOD_ERROR_WRONG_THREAD` | 11 | Синхронная engine-функция вызвана не с bound `DispatchFrame` thread. |

## 9. `WotbModHostApi`

Первые поля таблицы:

```cpp
uint32_t struct_size;
uint32_t abi_version;
uint32_t host_version;
uint32_t reserved;
```

`reserved` не использовать. Таблица принадлежит runtime и доступна до
shutdown.

### 9.1 Логирование

```cpp
void log(
    WotbModHandle mod,
    WotbModLogLevel level,
    const char* message);
```

Уровни:

- `WOTBMOD_LOG_TRACE`;
- `WOTBMOD_LOG_INFO`;
- `WOTBMOD_LOG_WARNING`;
- `WOTBMOD_LOG_ERROR`.

Runtime автоматически добавляет `[mod.id]`. В C++ доступен helper:

```cpp
wotbmod_logf(
    host,
    mod,
    WOTBMOD_LOG_INFO,
    "loaded %u objects",
    objectCount);
```

### 9.2 Пути

```cpp
WotbModResult get_path(
    WotbModHandle mod,
    WotbModPath path,
    char* buffer,
    uint32_t* inout_size);
```

Виды путей:

| Enum | Результат |
| --- | --- |
| `WOTBMOD_PATH_GAME` | Корень игры. |
| `WOTBMOD_PATH_MODS` | Общий каталог модов. |
| `WOTBMOD_PATH_MODULE` | Полный путь текущей DLL. |
| `WOTBMOD_PATH_DATA` | Персональный data directory. |
| `WOTBMOD_PATH_CONFIG` | Персональный INI-файл. |

Функция использует двухшаговый buffer contract:

```cpp
uint32_t size = 0;
WotbModResult result =
    host->get_path(mod, WOTBMOD_PATH_DATA, nullptr, &size);
if (result != WOTBMOD_ERROR_BUFFER_TOO_SMALL) {
    return;
}

char path[WOTBMOD_MAX_PATH] = {};
size = sizeof(path);
if (host->get_path(mod, WOTBMOD_PATH_DATA, path, &size) != WOTBMOD_OK) {
    return;
}
```

`inout_size` включает завершающий `NUL`.

### 9.3 Модуль игры и RVA

```cpp
void* get_game_module(void);
void* resolve_rva(uint32_t rva);
```

`get_game_module` возвращает base address главного PE-модуля.

`resolve_rva` проверяет PE image size и возвращает:

```text
game_module + rva
```

Если RVA выходит за `SizeOfImage`, возвращается `nullptr`.

Абсолютные адреса из IDA нужно переводить в RVA:

```text
RVA = VA - ImageBase
```

Для текущего x86 PE `ImageBase` указан в `re_anchors.md`.

### 9.4 Экспорты загруженных модулей

```cpp
void* get_proc_address(
    const char* loaded_module_name,
    const char* export_name);
```

Если `loaded_module_name == nullptr` или пустая строка, поиск выполняется в
главном модуле игры. Именованный модуль должен быть уже загружен.

### 9.5 Поиск byte pattern

```cpp
void* find_pattern(
    const char* loaded_module_name,
    const uint8_t* pattern,
    const char* mask);
```

Формат:

- `x` — байт обязан совпасть;
- `?` — wildcard.

```cpp
const uint8_t pattern[] = {
    0x55, 0x8B, 0xEC, 0x00, 0x00
};
void* address = host->find_pattern(
    nullptr,
    pattern,
    "xxx??");
```

Сканируются только committed/readable страницы внутри PE image. Возвращается
первое совпадение либо `nullptr`.

### 9.6 Управление хуками

```cpp
WotbModResult hook_create(
    WotbModHandle mod,
    void* target,
    void* detour,
    void** original);
WotbModResult hook_enable(WotbModHandle mod, void* target);
WotbModResult hook_disable(WotbModHandle mod, void* target);
WotbModResult hook_remove(WotbModHandle mod, void* target);
```

Runtime сам не содержит MinHook/PolyHook. Все операции делегируются
`WotbModRuntimeHookBackend`.

Пример:

```cpp
using TargetFn = void(WOTBMOD_CALL*)(void*);
static TargetFn g_original = nullptr;

static void WOTBMOD_CALL Detour(void* self) {
    // mod logic
    g_original(self);
}

void Install(
    const WotbModHostApi* host,
    WotbModHandle mod,
    void* target) {
    WotbModResult result = host->hook_create(
        mod,
        target,
        reinterpret_cast<void*>(&Detour),
        reinterpret_cast<void**>(&g_original));
    if (result == WOTBMOD_OK) {
        result = host->hook_enable(mod, target);
    }
}
```

Правила владения:

- hook принадлежит создавшему его моду;
- другой мод не может disable/remove этот target через API;
- повторный `hook_create` того же target для владельца возвращает
  `ALREADY_EXISTS`;
- при disable/fault/unload runtime вызовет backend `disable`, затем `remove`.
- `hook_create` и `hook_enable` доступны только включённому моду;
- `hook_disable` и `hook_remove` остаются доступны выключенному владельцу,
  чтобы он мог повторить cleanup после ошибки backend;
- если автоматический `remove` завершился ошибкой, disable возвращает эту
  ошибку, ownership сохраняется для retry, а runtime не выгружает DLL с
  потенциально живым detour.

### 9.7 Конфигурация

```cpp
int32_t config_get_int(
    WotbModHandle mod,
    const char* section,
    const char* key,
    int32_t default_value);

WotbModResult config_set_int(...);
WotbModResult config_get_string(...);
WotbModResult config_set_string(...);
```

Хранилище — персональный Win32 INI-файл мода.

```cpp
int32_t opacity =
    host->config_get_int(mod, "ui", "opacity", 100);

host->config_set_int(mod, "ui", "opacity", 85);

char theme[64] = {};
host->config_get_string(
    mod,
    "ui",
    "theme",
    "default",
    theme,
    sizeof(theme));

host->config_set_string(mod, "ui", "theme", "dark");
```

`config_get_int` при невалидных аргументах возвращает `default_value`.
`config_get_string` возвращает `BUFFER_TOO_SMALL`, если строка была усечена.

### 9.8 Перечисление и управление модами

```cpp
uint32_t get_mod_count(void);
WotbModResult get_mod_info(
    uint32_t index,
    WotbModPublicInfo* out_info);
WotbModResult set_mod_enabled(
    const char* id,
    int32_t enabled);
```

Пример для UI загрузчика:

```cpp
for (uint32_t i = 0; i < host->get_mod_count(); ++i) {
    WotbModPublicInfo info = {};
    info.struct_size = sizeof(info);
    if (host->get_mod_info(i, &info) == WOTBMOD_OK) {
        printf("%s %s enabled=%u faults=%u\n",
               info.name,
               info.version,
               info.enabled,
               info.fault_count);
    }
}
```

`get_mod_info` копирует столько байт, сколько разрешает caller
`struct_size`. Если структура вызывающей стороны короче текущей, заполненная
доступная часть возвращается вместе с `WOTBMOD_ERROR_BUFFER_TOO_SMALL`.

Поля `WotbModPublicInfo`:

| Поле | Содержимое |
| --- | --- |
| `struct_size` | Размер структуры вызывающей стороны. |
| `abi_version` | ABI-версия, с которой был загружен мод. |
| `state` | Одно из состояний из таблицы ниже. |
| `enabled` | `1`, если мод сейчас включён; иначе `0`. |
| `fault_count` | Количество перехваченных аварий в callbacks мода. |
| `flags` | Зарезервированные публичные флаги; сейчас `0`. |
| `id`, `name`, `version`, `author`, `description` | Копии metadata из `WotbModDescriptor`. |
| `module_path` | Абсолютный путь к загруженной DLL. |

Состояния:

| State | Значение |
| --- | --- |
| `DISCOVERED` | Слот создан, DLL ещё проходит загрузку. |
| `LOADED` | Descriptor принят, enable ещё не завершён. |
| `ENABLED` | Callback и frame dispatch разрешены. |
| `DISABLED` | Мод загружен, но выключен. |
| `FAULTED` | Callback аварийно завершился. |

## 10. Ресурсная система

Ресурсный API решает две разные задачи:

1. **Overlay resolution** — подмена игрового `~res:/...` пути файлом из
   data directory мода.
2. **Typed load** — создание DAVA UI/scene/YAML/texture/audio clip объекта
   через backend загрузчика.

Mount и path resolution работают внутри runtime. Для фактического перехвата
игрового файлового запроса loader должен вызвать
`WotbModRuntime_ResolveResourcePath` из своего file resolver hook.

Typed `resource_load` дополнительно требует
`WotbModRuntimeResourceBackend`. Исключение — `AUDIO_CLIP`: при наличии
`load_clip` и `release_clip` он загружается через audio backend непосредственно
из loose-файла mount.

### 10.1 Форматы виртуальных путей

API принимает:

```text
~res:/UI/MyScreen.yaml
res:/UI/MyScreen.yaml
Data/UI/MyScreen.yaml
UI/MyScreen.yaml
```

Все варианты нормализуются в:

```text
~res:/UI/MyScreen.yaml
```

Абсолютный путь `<game>\Data\...` также может быть нормализован runtime
resolver. Абсолютные пути вне game `Data` отклоняются.

Сегменты `.` и `..`, а также произвольные drive prefixes запрещены.

### 10.2 Mount directory

```cpp
WotbModResult resource_mount(
    WotbModHandle mod,
    const WotbModResourceMountInfo* mount,
    WotbModResourceMountId* out_mount_id);
```

`WotbModResourceMountInfo`:

| Поле | Значение |
| --- | --- |
| `struct_size` | `sizeof(WotbModResourceMountInfo)`. |
| `virtual_root` | Корень внутри DAVA `~res:/`. |
| `source_directory` | Путь относительно `WOTBMOD_PATH_DATA`. |
| `priority` | Чем больше число, тем выше приоритет. |
| `flags` | `WotbModResourceMountFlags`. |

Пример:

```cpp
WotbModResourceMountInfo mount = {};
mount.struct_size = sizeof(mount);
mount.virtual_root = "~res:/Mods/author.example/";
mount.source_directory = "resources";
mount.priority = 100;
mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;

WotbModResourceMountId mountId = 0;
WotbModResult result =
    host->resource_mount(mod, &mount, &mountId);
```

Выключенный мод не может создавать новые mounts: возвращается
`WOTBMOD_ERROR_DISABLED`. Уже созданный mount можно удалить в `on_disable`;
после callback runtime в любом случае удалит оставшиеся mounts владельца.

Фактическое сопоставление:

```text
Virtual:
~res:/Mods/author.example/UI/MyScreen.yaml

Physical:
mods/data/<module>/resources/UI/MyScreen.yaml
```

Mount разрешён только для enabled-мода. `source_directory` обязан существовать
и находиться внутри персонального data directory.

### 10.3 Приоритеты

Если несколько активных mounts содержат один virtual path:

1. выбирается mount с большим `priority`;
2. при равном priority выбирается mount, зарегистрированный позже.

Это позволяет делать theme/compatibility packs поверх базового мода.

### 10.4 Loose/DVPL fallback

Флаг:

```cpp
WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL
```

разрешает обе схемы:

```text
запрос MyScreen.yaml
  -> MyScreen.yaml
  -> MyScreen.yaml.dvpl

запрос MyScreen.yaml.dvpl
  -> MyScreen.yaml.dvpl
  -> MyScreen.yaml
```

То же применяется к `.sc2`, `.tex` и другим расширениям.

### 10.5 Resolve path из мода

```cpp
WotbModResult resource_resolve(
    WotbModHandle mod,
    const char* virtual_path,
    char* buffer,
    uint32_t* inout_size);
```

Используется тот же двухшаговый buffer contract, что у `get_path`.
Результат — физический путь файла из самого приоритетного enabled mount.
Поиск выполняется по общему реестру mount всех включённых модов. Переданный
`mod` нужен для проверки валидности caller handle, а не для ограничения поиска
ресурсами только этого мода.

`NOT_FOUND` означает, что mod override отсутствует. Это не означает, что
ресурса нет в оригинальном `Data`.

### 10.6 Unmount

```cpp
WotbModResult resource_unmount(
    WotbModHandle mod,
    WotbModResourceMountId mount_id);
```

Mount id принадлежит создавшему его моду. Чужой id возвращает
`ACCESS_DENIED`. Уже удалённый id возвращает `NOT_FOUND`.

### 10.7 Typed resource load

```cpp
WotbModResult resource_load(
    WotbModHandle mod,
    const WotbModResourceLoadRequest* request,
    WotbModResourceHandle* out_resource);
```

Типы:

| Type | Назначение backend |
| --- | --- |
| `WOTBMOD_RESOURCE_GENERIC` | Loader-specific generic asset. |
| `WOTBMOD_RESOURCE_YAML_DOCUMENT` | DAVA YAML parser/document. |
| `WOTBMOD_RESOURCE_UI_PACKAGE` | Полный UI package. |
| `WOTBMOD_RESOURCE_UI_CONTROL` | Named control из package. |
| `WOTBMOD_RESOURCE_SCENE` | `.sc2` scene/entity graph. |
| `WOTBMOD_RESOURCE_TEXTURE` | Texture resource. |
| `WOTBMOD_RESOURCE_AUDIO_CLIP` | Sound clip для Audio API. |

Запрос:

```cpp
WotbModResourceLoadRequest request = {};
request.struct_size = sizeof(request);
request.type = WOTBMOD_RESOURCE_UI_PACKAGE;
request.virtual_path =
    "~res:/Mods/author.example/UI/MyScreen.yaml";
request.object_name = nullptr;
request.flags = 0;

WotbModResourceHandle uiPackage = nullptr;
WotbModResult result =
    host->resource_load(mod, &request, &uiPackage);
```

`virtual_path` перед backend всегда приходит в нормализованном формате
`~res:/...`.

Поля `WotbModResourceLoadRequest`:

| Поле | Назначение |
| --- | --- |
| `struct_size` | Размер структуры; всегда задавайте `sizeof(request)`. |
| `type` | Тип требуемого объекта из таблицы выше. |
| `virtual_path` | Виртуальный путь `~res:/...`, который runtime сначала разрешает через mounts. |
| `object_name` | Необязательное имя вложенного объекта, прежде всего UI control. |
| `flags` | Backend-specific флаги; текущий runtime передаёт значение без изменений. |

`object_name` используется backend для
`WOTBMOD_RESOURCE_UI_CONTROL`, например:

```cpp
request.type = WOTBMOD_RESOURCE_UI_CONTROL;
request.virtual_path =
    "~res:/Mods/author.example/UI/Controls.yaml";
request.object_name = "SettingsButton";
```

Public resource handle не является DAVA pointer. Его нельзя разыменовывать.
Runtime хранит native pointer внутри собственного ownership record.

В bundled live loader используется `WotbModDavaResources_Create` из
`wotb_mod_dava_resources.h`. Для клиента 11.19.0.834 x86 он:

- создаёт настоящий `DAVA::UIPackage`;
- извлекает именованный refcounted `DAVA::UIControl`;
- создаёт `DAVA::Scene` через клиентский `EntityCache::LoadEntityUnsafe`;
- проверяет ожидаемые vtables до принятия объекта;
- выполняет reload по схеме create replacement → release old;
- освобождает объекты через родной DAVA `Release`.

Нулевые RVA в `WotbModDavaResourcesOptions` выбирают проверенные anchors
11.19. Для другой версии loader обязан передать заново найденные RVA.
Несовместимый PE/архитектура возвращает `WOTBMOD_ERROR_PLATFORM`.

### 10.8 Reload и release

```cpp
WotbModResult resource_reload(
    WotbModHandle mod,
    WotbModResourceHandle resource);

WotbModResult resource_release(
    WotbModHandle mod,
    WotbModResourceHandle resource);
```

`resource_reload` работает только если выбранный backend предоставил
`reload`/`reload_clip`. Иначе возвращается `WOTBMOD_ERROR_PLATFORM`.

`resource_release`:

- проверяет принадлежность handle текущему моду;
- возвращает `ACCESS_DENIED`, пока audio clip используется playback handle;
- вызывает backend `release`;
- инвалидирует public handle только после успешного backend `release`;
- при ошибке/fault backend сохраняет handle и native pointer для retry.

Для audio clip с активным playback запрещены и `resource_reload`, и
`resource_release`.

После успешного release использовать handle нельзя. Opaque handles содержат
уникальный идентификатор загрузки, поэтому старое значение не может случайно
освободить новый ресурс после повторного использования внутреннего slot.

### 10.9 Пример UI package

Файлы:

```text
mods/data/my-ui-mod/resources/
  UI/
    Main.yaml
    Main.style.yaml
```

Код:

```cpp
static WotbModResourceMountId g_mount = 0;
static WotbModResourceHandle g_package = nullptr;

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/my-ui-mod/";
    mount.source_directory = "resources";
    mount.priority = 100;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;

    if (host->resource_mount(mod, &mount, &g_mount) != WOTBMOD_OK) {
        return;
    }

    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = WOTBMOD_RESOURCE_UI_PACKAGE;
    request.virtual_path = "~res:/Mods/my-ui-mod/UI/Main.yaml";

    host->resource_load(mod, &request, &g_package);
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    if (g_package) {
        host->resource_release(mod, g_package);
        g_package = nullptr;
    }
    if (g_mount) {
        host->resource_unmount(mod, g_mount);
        g_mount = 0;
    }
}
```

### 10.10 Пример custom 3D scene/model

Файлы:

```text
mods/data/my-model-mod/resources/
  3d/
    GarageObject.sc2
    GarageObject/
      diffuse.tex
      normal.tex
    Materials/
      GarageObject.material
```

Код:

```cpp
WotbModResourceLoadRequest request = {};
request.struct_size = sizeof(request);
request.type = WOTBMOD_RESOURCE_SCENE;
request.virtual_path =
    "~res:/Mods/my-model-mod/3d/GarageObject.sc2";

WotbModResourceHandle scene = nullptr;
WotbModResult result =
    host->resource_load(mod, &request, &scene);
```

Ссылки на textures/materials внутри `.sc2` должны использовать пути,
доступные через тот же mount.

Bundled bridge проверен на loose `.sc2`. Registry умеет находить
`.sc2.dvpl`, но production object-тест текущего клиента использует loose
asset; для собственных моделей поставляйте loose `.sc2`, пока DVPL pipeline
вашего build tool не проверен отдельно.

API загружает и владеет настоящим scene resource. ABI 2.5 позволяет менять
его local transform и временно присоединять другой загруженный Scene через
typed handles; полный пример находится в разделе 10.13. Raw
`DAVA::Entity*` по-прежнему не пересекает публичный ABI.

### 10.11 Audio clips и playback

ABI 2.3 предоставляет короткую функцию для custom-файлов:

```cpp
WotbModResult audio_clip_load(
    WotbModHandle mod,
    const char* virtual_path,
    WotbModResourceHandle* out_audio_clip);

WotbModResourceHandle clip = nullptr;
WotbModResult result =
    host->audio_clip_load(
        mod,
        "~res:/Mods/author.example/Audio/notification.wav",
        &clip);
```

`virtual_path` должен разрешаться в loose-файл активного mount. Абсолютные
произвольные пути не принимаются. Это сохраняет изоляцию data directory и
приоритеты overlay.

Функция является convenience wrapper для совместимого typed-запроса:

```cpp
WotbModResourceLoadRequest request = {};
request.struct_size = sizeof(request);
request.type = WOTBMOD_RESOURCE_AUDIO_CLIP;
request.virtual_path =
    "~res:/Mods/author.example/Audio/notification.wav";

WotbModResourceHandle clip = nullptr;
WotbModResult result =
    host->resource_load(mod, &request, &clip);
```

Обе формы возвращают один и тот же opaque resource handle. Если audio backend
предоставил `load_clip`/`release_clip`, runtime передаёт ему физический путь
loose-файла. Иначе сохраняется поведение ABI 2.2: запрос уходит в общий
resource backend.

Затем из одного clip можно создать несколько независимых playback:

```cpp
WotbModResult audio_play(
    WotbModHandle mod,
    WotbModResourceHandle audio_clip,
    const WotbModAudioPlayInfo* play_info,
    WotbModAudioPlaybackHandle* out_playback);
```

`play_info == nullptr` включает безопасные 2D defaults:

| Поле | Default | Ограничение |
| --- | ---: | --- |
| `flags` | `WOTBMOD_AUDIO_PLAY_NONE` | Только известные audio flags. |
| `volume` | `1.0` | Конечное число, `>= 0`. |
| `pitch` | `1.0` | Конечное число, `> 0`. |
| `pan` | `0.0` | От `-1` слева до `1` справа. |
| `position_x/y/z` | `0` | Конечные числа; используются для spatial sound. |
| `min_distance` | `1.0` | `>= 0`. |
| `max_distance` | `100.0` | `>= min_distance`. |

Флаги:

| Flag | Назначение |
| --- | --- |
| `WOTBMOD_AUDIO_PLAY_LOOP` | Повторять звук, пока playback не остановлен. |
| `WOTBMOD_AUDIO_PLAY_SPATIAL` | Использовать 3D position и distance attenuation. |
| `WOTBMOD_AUDIO_PLAY_START_PAUSED` | Создать playback в состоянии `PAUSED`. |

Готовый Windows backend из `wotb_mod_windows_audio.h` декодирует форматы, для
которых в Media Foundation установлен decoder: обычно WAV, MP3, AAC/M4A и
WMA. Он поддерживает loop, pause/resume/stop, volume, pitch и 2D pan.
Встроенный OGG decoder не поставляется.

`WOTBMOD_AUDIO_PLAY_SPATIAL` зависит от backend. Готовый XAudio2 backend
возвращает для него `WOTBMOD_ERROR_PLATFORM`; engine-native 3D position и
distance attenuation доступны через клиентский sound-engine adapter.

Пример 2D loop для готового Windows backend:

```cpp
WotbModAudioPlayInfo play = {};
play.struct_size = sizeof(play);
play.flags = WOTBMOD_AUDIO_PLAY_LOOP;
play.volume = 0.8f;
play.pitch = 1.0f;
play.pan = -0.25f;

WotbModAudioPlaybackHandle playback = nullptr;
result = host->audio_play(mod, clip, &play, &playback);
```

Управление playback:

```cpp
WotbModResult audio_pause(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback);
WotbModResult audio_resume(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback);
WotbModResult audio_stop(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback);
WotbModResult audio_set_parameters(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback,
    const WotbModAudioPlayInfo* parameters);
WotbModResult audio_get_state(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback,
    WotbModAudioState* out_state);
WotbModResult audio_release(
    WotbModHandle mod,
    WotbModAudioPlaybackHandle playback);
```

Состояния:

| State | Значение |
| --- | --- |
| `WOTBMOD_AUDIO_STOPPED` | Playback остановлен. |
| `WOTBMOD_AUDIO_PLAYING` | Воспроизводится. |
| `WOTBMOD_AUDIO_PAUSED` | Приостановлен и может быть продолжен. |

`audio_set_parameters` заменяет весь набор параметров. Флаг
`START_PAUSED` здесь запрещён: состояние меняется через `audio_pause` и
`audio_resume`.

`audio_get_state` вызывает backend query, если loader его предоставил. Без
optional `get_state` runtime возвращает последнее известное состояние.

Ownership и cleanup:

- clip и playback должны принадлежать одному моду;
- чужой или stale playback handle отвергается;
- clip нельзя reload/освободить, пока существует связанный playback;
- `audio_release` обязан остановить и уничтожить native playback;
- при backend error/fault public handle остаётся активным для retry;
- при disable/fault/shutdown runtime сначала выполняет stop/release всех
  playbacks, затем освобождает clips и остальные resources;
- новые `audio_play`, `audio_resume` и `audio_set_parameters` запрещены
  выключенному моду; stop/release остаются доступны для cleanup.

### 10.12 Native DAVA/Wwise sound events

ABI 2.4 добавляет отдельный путь для событий, уже зарегистрированных в
клиентских sound banks. Это не замена `audio_clip_load`: custom WAV/MP3
используют file-audio API из предыдущего раздела, а `sound_event_*` работает
с именем нативного DAVA/Wwise event.

```cpp
WotbModResult sound_event_create(
    WotbModHandle mod,
    const char* event_name,
    WotbModSoundEventHandle* out_event);
WotbModResult sound_event_trigger(
    WotbModHandle mod,
    WotbModSoundEventHandle event);
WotbModResult sound_event_stop(
    WotbModHandle mod,
    WotbModSoundEventHandle event);
WotbModResult sound_event_set_paused(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    int32_t paused);
WotbModResult sound_event_set_volume(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    float volume);
WotbModResult sound_event_set_position(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    float x,
    float y,
    float z);
WotbModResult sound_event_get_state(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    WotbModAudioState* out_state);
WotbModResult sound_event_set_parameter(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    const char* parameter_name,
    float value);
WotbModResult sound_event_get_parameter(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    const char* parameter_name,
    float* out_value);
WotbModResult sound_event_has_parameter(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    const char* parameter_name,
    int32_t* out_has_parameter);
WotbModResult sound_event_get_name(
    WotbModHandle mod,
    WotbModSoundEventHandle event,
    char* buffer,
    uint32_t* inout_size);
WotbModResult sound_event_release(
    WotbModHandle mod,
    WotbModSoundEventHandle event);
```

Минимальный пример:

```cpp
WotbModSoundEventHandle event = nullptr;
WotbModResult result = host->sound_event_create(
    mod,
    "guns/tracers/tracer_hard",
    &event);
if (result == WOTBMOD_OK) {
    host->sound_event_set_volume(mod, event, 0.25f);
    host->sound_event_set_position(mod, event, 0.0f, 0.0f, 0.0f);
    host->sound_event_trigger(mod, event);
    host->sound_event_stop(mod, event);
    host->sound_event_release(mod, event);
}
```

Правила:

- `event_name` и `parameter_name` — непустые строки без CR/LF;
- длина event name меньше `WOTBMOD_MAX_SOUND_EVENT_NAME`;
- одновременно одному моду принадлежат не более 64 event handles;
- `volume` должен быть конечным и `>= 0`, position и RTPC value — конечными;
- handle принадлежит создавшему его моду; чужие и stale handles отвергаются;
- `trigger` и setters запрещены выключенному моду, но `stop` и `release`
  доступны для cleanup;
- `sound_event_get_name` использует общий size-query контракт:
  `buffer == nullptr` возвращает `BUFFER_TOO_SMALL` и требуемый размер;
- `release` уничтожает native event и навсегда инвалидирует public handle;
- disable, callback fault и shutdown автоматически выполняют stop/release.

`sound_event_get_state` возвращает последнее успешно применённое состояние
`STOPPED`, `PLAYING` или `PAUSED`. Это намеренно не прямой результат
`DAVA::SoundEvent::IsActive()`: в текущем клиенте `IsActive()` описывает
активность/lifetime event instance и остаётся истинным до trigger и сразу
после stop.

RTPC остаются engine-owned. Сначала вызывайте `has_parameter`. Успешный
`set_parameter` означает, что вызов принят native backend, но последующий
`get_parameter` может вернуть нормализованное, ограниченное или отложенное
значение Wwise, а не побитовую копию аргумента.

Имена событий зависят от версии банков клиента. Не встраивайте адреса DAVA в
мод: версия-зависимые детали находятся только в loader bridge.

### 10.13 Main-thread, UIControl и Scene API

ABI 2.5 добавляет восемь функций:

```cpp
WotbModResult main_thread_enqueue(
    WotbModHandle mod,
    WotbModMainThreadCallback callback,
    void* user_data);

WotbModResult ui_control_set_geometry(
    WotbModHandle mod,
    WotbModResourceHandle control,
    const WotbModUiControlGeometry* geometry);
WotbModResult ui_control_set_visible(
    WotbModHandle mod,
    WotbModResourceHandle control,
    int32_t visible);
WotbModResult ui_control_add_child(
    WotbModHandle mod,
    WotbModResourceHandle parent,
    WotbModResourceHandle child);
WotbModResult ui_control_remove_child(
    WotbModHandle mod,
    WotbModResourceHandle parent,
    WotbModResourceHandle child);

WotbModResult scene_set_transform(
    WotbModHandle mod,
    WotbModResourceHandle scene,
    const WotbModSceneTransform* transform);
WotbModResult scene_add_child(
    WotbModHandle mod,
    WotbModResourceHandle parent,
    WotbModResourceHandle child);
WotbModResult scene_remove_child(
    WotbModHandle mod,
    WotbModResourceHandle parent,
    WotbModResourceHandle child);
```

Перед обращением к ABI 2.5 мод, рассчитанный на несколько minor-версий,
проверяет размер таблицы:

```cpp
const bool hasObjectApi =
    host->struct_size >=
        offsetof(WotbModHostApi, scene_remove_child) +
            sizeof(host->scene_remove_child) &&
    host->main_thread_enqueue &&
    host->ui_control_set_geometry &&
    host->scene_set_transform;
```

#### Очередь main thread

Callback имеет сигнатуру:

```cpp
void WOTBMOD_CALL Callback(
    const WotbModHostApi* host,
    WotbModHandle mod,
    void* user_data);
```

`main_thread_enqueue` всегда откладывает callback до начала следующего
`WotbModRuntime_DispatchFrame`; даже вызов с render-потока не выполняется
рекурсивно. Очередь глобально сохраняет FIFO-порядок. На один мод принимается
не более 64 элементов, на runtime — не более 256. `user_data` не копируется:
его память должна оставаться валидной до callback.

Перед `on_frame` runtime:

1. привязывает текущий поток как dispatch/main thread;
2. извлекает очередь в FIFO-порядке;
3. повторно проверяет enabled-state и resource handles;
4. выполняет callbacks и queued UI/Scene операции;
5. затем вызывает обычные `on_frame`.

Disable, fault, unload и shutdown удаляют ещё не выполненные элементы мода до
выгрузки его DLL. SEH fault внутри main-thread callback переводит только его
владельца в `FAULTED` и запускает обычный ownership cleanup.

#### Геометрия и видимость UIControl

```cpp
WotbModUiControlGeometry geometry = {};
geometry.struct_size = sizeof(geometry);
geometry.x = 24.0f;
geometry.y = 32.0f;
geometry.width = 320.0f;
geometry.height = 180.0f;

host->ui_control_set_geometry(mod, control, &geometry);
host->ui_control_set_visible(mod, control, 1);
```

Координаты являются virtual coordinates DAVA. Все четыре значения должны
быть конечными; `width` и `height` не могут быть отрицательными. Bundled
bridge применяет прямоугольник через native `UIControl::SetPosition` и
`UIControl::SetSize`. Видимость меняется через проверенный visible-bit
объекта с инвалидированием dirty-state иерархии. Старый ошибочно размеченный
vtable slot 10 не вызывается: в текущем клиенте это `SystemInput`, а не
операция видимости.

Для дерева controls:

```cpp
host->ui_control_add_child(mod, parentControl, childControl);
// ...
host->ui_control_remove_child(mod, parentControl, childControl);
```

Оба handle должны принадлежать вызывающему моду, иметь тип
`WOTBMOD_RESOURCE_UI_CONTROL` и быть различными. Native DAVA `AddControl`
удерживает child и отсоединяет его от прежнего parent; `RemoveControl`
разрывает эту связь. Перед release всегда явно удаляйте child из parent.

#### Local transform и дерево Scene

```cpp
WotbModSceneTransform transform = {};
transform.struct_size = sizeof(transform);
transform.position_x = 0.0f;
transform.position_y = 2.0f;
transform.position_z = 5.0f;
transform.rotation_x = 0.0f;
transform.rotation_y = 0.0f;
transform.rotation_z = 0.0f;
transform.rotation_w = 1.0f; // quaternion x,y,z,w
transform.scale_x = 1.0f;
transform.scale_y = 1.0f;
transform.scale_z = 1.0f;

host->scene_set_transform(mod, modelScene, &transform);
host->scene_add_child(mod, parentScene, modelScene);
// ...
host->scene_remove_child(mod, parentScene, modelScene);
```

Все значения должны быть конечными. Quaternion не может состоять только из
нулей; каждый scale component должен быть ненулевым. Runtime не нормализует
quaternion и не меняет систему координат. Bundled bridge передаёт local
transform в `TransformComponent::SetLocalTransform`, после чего DAVA
обновляет world transform/dirty state. `scene_add_child` и
`scene_remove_child` используют native `Entity::AddNode`/`RemoveNode`.

Оба Scene handle принадлежат одному моду и должны быть различными. Для
загруженных custom `.sc2` resources эти функции работают как раньше; ABI 2.6
также позволяет создать пустой `Entity` и получить retained handle активной
корневой Scene.

### 10.14 Создание UIControl/Entity и active roots

ABI 2.6 добавляет четыре функции в конец `WotbModHostApi`:

```cpp
WotbModResult ui_control_create(
    WotbModHandle mod,
    const WotbModUiControlGeometry* geometry,
    WotbModResourceHandle* out_control);
WotbModResult ui_get_active_screen(
    WotbModHandle mod,
    WotbModResourceHandle* out_screen);
WotbModResult scene_entity_create(
    WotbModHandle mod,
    WotbModResourceHandle* out_entity);
WotbModResult scene_get_active(
    WotbModHandle mod,
    WotbModResourceHandle* out_scene);
```

Проверка availability для мода, который поддерживает несколько minor ABI:

```cpp
const bool hasFactoryApi =
    host->struct_size >=
        offsetof(WotbModHostApi, scene_get_active) +
            sizeof(host->scene_get_active) &&
    host->ui_control_create &&
    host->ui_get_active_screen &&
    host->scene_entity_create &&
    host->scene_get_active;
```

Все четыре функции синхронны и обращаются к DAVA. Их разрешено вызывать
только из `on_frame` либо из callback, переданного в
`main_thread_enqueue`. Из `WotbModLoad`, первоначального `on_enable` или
worker thread они возвращают `WOTBMOD_ERROR_WRONG_THREAD`.

`ui_control_create` вызывает штатный конструктор `DAVA::UIControl`. Параметр
`geometry` можно передать как `nullptr`; тогда остаётся engine-default rect.
При ненулевом параметре проверяются `struct_size`, конечность координат и
неотрицательные width/height.

`scene_entity_create` вызывает штатный конструктор `DAVA::Entity`. Публичный
тип handle остаётся `WOTBMOD_RESOURCE_SCENE`, потому что mutation API работает
с `Entity`-совместимыми узлами дерева Scene.

`ui_get_active_screen` получает текущий `UIControlSystem::GetScreen()`.
`scene_get_active` возвращает последнюю активированную или отрисованную
корневую `DAVA::Scene`, которую loader отслеживает через `Scene::Activate`,
`Scene::Deactivate` и `Scene::Draw`. Если клиент сейчас показывает только UI
и активной 3D Scene нет, корректный результат —
`WOTBMOD_ERROR_NOT_FOUND`.

Каждый успешный вызов возвращает новый handle, принадлежащий вызывающему моду.
Для borrowed active screen/Scene bridge сначала делает DAVA `Retain`; поэтому
все четыре handle освобождаются одинаково через `resource_release`. Перед
release созданного child сначала вызовите `ui_control_remove_child` или
`scene_remove_child`. Эти объекты не связаны с файлом:
`resource_reload` для них возвращает `WOTBMOD_ERROR_PLATFORM`.

Пример:

```cpp
static void WOTBMOD_CALL CreateOverlay(
    const WotbModHostApi* host,
    WotbModHandle mod,
    void*) {
    WotbModUiControlGeometry rect = {};
    rect.struct_size = sizeof(rect);
    rect.x = 30.0f;
    rect.y = 30.0f;
    rect.width = 240.0f;
    rect.height = 80.0f;

    WotbModResourceHandle screen = nullptr;
    WotbModResourceHandle control = nullptr;
    if (host->ui_get_active_screen(mod, &screen) == WOTBMOD_OK &&
        host->ui_control_create(mod, &rect, &control) == WOTBMOD_OK) {
        host->ui_control_add_child(mod, screen, control);
        // ... использовать control ...
        host->ui_control_remove_child(mod, screen, control);
    }
    if (control) host->resource_release(mod, control);
    if (screen) host->resource_release(mod, screen);
}

host->main_thread_enqueue(mod, &CreateOverlay, nullptr);
```

#### Поток выполнения и значение `OK`

После первого `DispatchFrame` UI/Scene вызов из bound dispatch thread
выполняется синхронно: его return code является результатом DAVA backend.
Вызов с любого другого потока валидируется, копируется в main-thread queue и
возвращает `WOTBMOD_OK`, если принят. В этом случае `OK` означает
«поставлено в очередь»; поздняя ошибка backend записывается в runtime log.
Если вызывающему коду нужен синхронный результат, выполняйте операцию из
`on_frame` или собственного `main_thread_enqueue` callback.

Отсутствующий object callback backend возвращает `WOTBMOD_ERROR_PLATFORM`.
Невалидный, чужой, stale или неверно типизированный handle возвращает
`WOTBMOD_ERROR_INVALID_ARGUMENT` до обращения к backend. Backend callbacks
защищены SEH; синхронный fault возвращает `WOTBMOD_ERROR_CALLBACK_FAULT` и не
выключает мод.

### 10.15 События клиента и сохранение event resources

ABI 2.7 добавляет три функции в конец `WotbModHostApi`:

```cpp
WotbModResult resource_clone(
    WotbModHandle mod,
    WotbModResourceHandle resource,
    WotbModResourceHandle* out_resource);
WotbModResult event_subscribe(
    WotbModHandle mod,
    uint32_t event_mask,
    WotbModClientEventCallback callback,
    void* user_data,
    WotbModEventSubscriptionId* out_subscription_id);
WotbModResult event_unsubscribe(
    WotbModHandle mod,
    WotbModEventSubscriptionId subscription_id);
```

Проверка availability:

```cpp
const bool hasClientEvents =
    host->struct_size >=
        offsetof(WotbModHostApi, event_unsubscribe) +
            sizeof(host->event_unsubscribe) &&
    host->resource_clone &&
    host->event_subscribe &&
    host->event_unsubscribe;
```

ABI 2.7 использует первые три маски. ABI 2.8 расширяет ту же таблицу
типизированными UI/gameplay событиями:

```cpp
WOTBMOD_EVENT_UI_SCREEN_CHANGED
WOTBMOD_EVENT_SCENE_ACTIVATED
WOTBMOD_EVENT_SCENE_DEACTIVATED
WOTBMOD_EVENT_UI_INPUT
WOTBMOD_EVENT_BATTLE_ENTERED
WOTBMOD_EVENT_BATTLE_STARTED
WOTBMOD_EVENT_BATTLE_ENDED
WOTBMOD_EVENT_BATTLE_LEFT
WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED
WOTBMOD_EVENT_VEHICLE_SPAWNED
WOTBMOD_EVENT_VEHICLE_DESPAWNED
WOTBMOD_EVENT_SHOT_FIRED
WOTBMOD_EVENT_SHELL_HIT
WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED
WOTBMOD_EVENT_VEHICLE_DAMAGED
WOTBMOD_EVENT_VEHICLE_DESTROYED
WOTBMOD_EVENT_RELOAD_STATE_CHANGED
WOTBMOD_EVENT_AMMO_CHANGED
WOTBMOD_EVENT_AIM_TARGET_CHANGED
WOTBMOD_EVENT_VEHICLE_SPOTTED
WOTBMOD_EVENT_VEHICLE_UNSPOTTED
WOTBMOD_EVENT_CAMERA_MODE_CHANGED
WOTBMOD_EVENT_ALL
```

Callback получает:

```cpp
typedef struct WotbModClientEvent {
    uint32_t struct_size;
    uint32_t type;
    uint64_t sequence;
    WotbModResourceHandle previous_resource;
    WotbModResourceHandle resource;
    WotbModVehicleHandle vehicle;
    WotbModVehicleHandle other_vehicle;
    uint32_t payload_size;
    uint32_t flags;
    WotbModClientEventPayload payload;
} WotbModClientEvent;
```

Соответствие payload:

| Тип | `previous_resource` | `resource` |
| --- | --- | --- |
| `UI_SCREEN_CHANGED` | прежний `UIControl` или `nullptr` | новый `UIControl` или `nullptr` |
| `SCENE_ACTIVATED` | `nullptr` | активированная `Scene` |
| `SCENE_DEACTIVATED` | деактивированная `Scene` | `nullptr` |

Оба resource-поля являются **borrowed callback-scoped handles**. Они валидны
только до возврата callback. Их нельзя передавать в `resource_release` или
`resource_reload`: эти функции возвращают `WOTBMOD_ERROR_ACCESS_DENIED`.
После callback handle становится stale и возвращает
`WOTBMOD_ERROR_INVALID_ARGUMENT`.

Чтобы сохранить объект, вызовите `resource_clone` непосредственно внутри
callback. Полученный clone является обычным mod-owned handle: его можно
изменять через UI/Scene API, передавать в последующие callbacks и освобождать
через `resource_release`. Clone также автоматически освобождается при
disable, fault, unload или shutdown. Для обычного owned generic resource
`resource_clone` работает так же; audio clip handles не клонируются и
возвращают `WOTBMOD_ERROR_PLATFORM`.

Пример:

```cpp
static WotbModResourceHandle g_screen = nullptr;

static void WOTBMOD_CALL OnClientEvent(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void*) {
    if (!event ||
        event->struct_size < sizeof(WotbModClientEvent) ||
        event->type != WOTBMOD_EVENT_UI_SCREEN_CHANGED ||
        !event->resource) {
        return;
    }

    WotbModResourceHandle clone = nullptr;
    if (host->resource_clone(
            mod, event->resource, &clone) == WOTBMOD_OK) {
        if (g_screen) host->resource_release(mod, g_screen);
        g_screen = clone;
    }
}

WotbModEventSubscriptionId subscription = 0;
host->event_subscribe(
    mod,
    WOTBMOD_EVENT_UI_SCREEN_CHANGED |
        WOTBMOD_EVENT_SCENE_ACTIVATED,
    &OnClientEvent,
    nullptr,
    &subscription);

// При необходимости удалить раньше disable:
host->event_unsubscribe(mod, subscription);
```

События ставятся loader-ом в bounded FIFO и доставляются только из
`WotbModRuntime_DispatchFrame`, после main-thread queue и до `on_frame`.
`sequence` ненулевой и монотонно возрастает для runtime. Подписка принадлежит
создавшему её моду; чужой id возвращает `ACCESS_DENIED`, неизвестный или уже
удалённый id — `NOT_FOUND`. Все подписки автоматически удаляются до
`on_disable`, fault cleanup, unload и освобождения DLL.

Callback защищён SEH. Fault переводит только владельца подписки в
`WOTBMOD_STATE_FAULTED`, удаляет его подписки и запускает обычный cleanup.
Callback должен быть коротким: блокирующая работа задерживает frame dispatch.

### 10.16 Работа с существующим UI

ABI 2.8 добавляет семь функций в конец `WotbModHostApi`:

```cpp
WotbModResult ui_control_find_by_name(
    WotbModHandle mod,
    WotbModResourceHandle root,
    const char* name,
    int32_t recursive,
    WotbModResourceHandle* out_control);
WotbModResult ui_control_get_parent(
    WotbModHandle mod,
    WotbModResourceHandle control,
    WotbModResourceHandle* out_parent);
WotbModResult ui_control_get_child_count(
    WotbModHandle mod,
    WotbModResourceHandle control,
    uint32_t* out_count);
WotbModResult ui_control_get_child_at(
    WotbModHandle mod,
    WotbModResourceHandle control,
    uint32_t index,
    WotbModResourceHandle* out_child);
WotbModResult ui_control_get_state(
    WotbModHandle mod,
    WotbModResourceHandle control,
    WotbModUiControlState* out_state);
WotbModResult ui_control_set_input_enabled(
    WotbModHandle mod,
    WotbModResourceHandle control,
    int32_t enabled,
    int32_t hierarchical);
WotbModResult ui_control_set_disabled(
    WotbModHandle mod,
    WotbModResourceHandle control,
    int32_t disabled,
    int32_t hierarchical);
```

Начальная точка обычно получается через `ui_get_active_screen`. Поиск
сравнивает native `FastName` control-а; при `recursive != 0` обходится всё
поддерево. `get_parent` и `get_child_at` возвращают новые mod-owned handles с
native `Retain`. Их обязательно освобождать через `resource_release`.
`get_child_at` возвращает `NOT_FOUND`, если индекс вышел за границы.

`WotbModUiControlState` содержит геометрию и флаги:

```cpp
WOTBMOD_UI_CONTROL_VISIBLE
WOTBMOD_UI_CONTROL_INPUT_ENABLED
WOTBMOD_UI_CONTROL_DISABLED
```

Параметр `hierarchical` передаётся штатным `SetInputEnabled`/`SetDisabled` и
просит DAVA применить состояние к поддереву. Вызовы поиска, traversal и
чтения состояния синхронны и разрешены только на dispatch thread. Операции
изменения используют те же правила очереди, что geometry/visibility.

```cpp
WotbModResourceHandle screen = nullptr;
WotbModResourceHandle ammo = nullptr;
if (host->ui_get_active_screen(mod, &screen) == WOTBMOD_OK &&
    host->ui_control_find_by_name(
        mod, screen, "AmmoPanel", 1, &ammo) == WOTBMOD_OK) {
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    host->ui_control_get_state(mod, ammo, &state);
    host->ui_control_set_visible(mod, ammo, 1);
    host->ui_control_set_input_enabled(mod, ammo, 1, 0);
    host->ui_control_set_disabled(mod, ammo, 0, 0);
}
if (ammo) host->resource_release(mod, ammo);
if (screen) host->resource_release(mod, screen);
```

Это позволяет менять существующие controls игры и добавлять собственные.
API не гарантирует стабильность имён/структуры Blitz UI между версиями
клиента: мод обязан корректно обрабатывать `NOT_FOUND`.

### 10.17 Vehicle API и основные игровые события

ABI 2.8 предоставляет безопасные снимки машин:

```cpp
WotbModResult vehicle_get_local(
    WotbModHandle mod,
    WotbModVehicleHandle* out_vehicle);
WotbModResult vehicle_get_by_entity_id(
    WotbModHandle mod,
    uint32_t entity_id,
    WotbModVehicleHandle* out_vehicle);
WotbModResult vehicle_get_count(
    WotbModHandle mod,
    uint32_t* out_count);
WotbModResult vehicle_get_at(
    WotbModHandle mod,
    uint32_t index,
    WotbModVehicleHandle* out_vehicle);
WotbModResult vehicle_clone(
    WotbModHandle mod,
    WotbModVehicleHandle vehicle,
    WotbModVehicleHandle* out_vehicle);
WotbModResult vehicle_release(
    WotbModHandle mod,
    WotbModVehicleHandle vehicle);
WotbModResult vehicle_get_info(
    WotbModHandle mod,
    WotbModVehicleHandle vehicle,
    WotbModVehicleInfo* out_info);
```

`WotbModVehicleInfo` содержит `entity_id`, `team`, `health`, `max_health`,
`player_name`, `vehicle_name` и флаги `ALIVE`, `LOCAL`, `DESTROYED`.
Публичный handle указывает на runtime snapshot, а не на BigWorld/DAVA объект.
Неизвестные текущему bridge поля остаются нулевыми. Полученные через query или
`vehicle_clone` handles принадлежат моду и освобождаются
`vehicle_release`. Максимум — 64 owned vehicle handles на мод.

Query-функции работают только на dispatch thread. Снимок является
last-known состоянием: для актуального значения после события получите новый
snapshot либо используйте event handle в callback.

```cpp
uint32_t count = 0;
host->vehicle_get_count(mod, &count);

WotbModVehicleHandle local = nullptr;
if (host->vehicle_get_local(mod, &local) == WOTBMOD_OK) {
    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    host->vehicle_get_info(mod, local, &info);
    host->vehicle_release(mod, local);
}
```

Текущий live loader формирует следующие события:

| Событие | Источник | Payload |
| --- | --- | --- |
| `UI_SCREEN_CHANGED` | смена `UIControlSystem::GetScreen()` | resource handles |
| `SCENE_ACTIVATED` / `SCENE_DEACTIVATED` | native Scene hooks | resource handles |
| `BATTLE_ENTERED` | первая машина вошла в world | `payload.battle` |
| `BATTLE_STARTED` | выводится из первого `onEnterWorld` | `payload.battle` |
| `BATTLE_ENDED` | выводится из последнего `onLeaveWorld` | `payload.battle` |
| `BATTLE_LEFT` | последняя машина вышла из world | `payload.battle` |
| `LOCAL_VEHICLE_CHANGED` | обновление health local Avatar | `vehicle`, `payload.vehicle` |
| `VEHICLE_SPAWNED` | `Vehicle::onEnterWorld` | `vehicle`, `payload.vehicle` |
| `VEHICLE_DESPAWNED` | `Vehicle::onLeaveWorld` | `vehicle`, `payload.vehicle` |
| `SHOT_FIRED` | `Vehicle::showShooting` | `vehicle`, `payload.shot` |
| `VEHICLE_HEALTH_CHANGED` | `Vehicle::set_health` | `vehicle`, `payload.vehicle` |
| `VEHICLE_DAMAGED` | health уменьшилось | `vehicle`, `payload.damage` |
| `VEHICLE_DESTROYED` | переход health из `> 0` в `<= 0` | `vehicle`, `payload.vehicle` |
| `RELOAD_STATE_CHANGED` | `ReloadTimer::setState` | `vehicle`, `payload.reload` |
| `UI_INPUT` | `DAVA::UIControl::SystemInput` | `payload.ui_input` |
| `SHELL_HIT` | `GameSceneController::OnVehicleHitDamage` | `vehicle`, `other_vehicle`, `payload.hit` |
| `AMMO_CHANGED` | `UIShellSelectorControl::OnCurrentAmmoChanged` | `vehicle`, `payload.ammo` |
| `AIM_TARGET_CHANGED` | native aim-target set/clear | `vehicle`, `other_vehicle`, `payload.vehicle` |
| `VEHICLE_SPOTTED` | arena observed-status handler | `vehicle`, `payload.vehicle` |
| `VEHICLE_UNSPOTTED` | arena observed-status handler | `vehicle`, `payload.vehicle` |
| `CAMERA_MODE_CHANGED` | штатный `CameraModeChanged@Avatar@GES` callback | `payload.camera`: `previous_mode`, `mode`, raw `native_mode`, `flags`; подтверждены native `0=ARCADE`, `1=SNIPER` |

`payload_size` говорит, сколько байтов соответствующей POD-структуры
заполнено. Проверяйте его перед чтением. `event->vehicle` и
`event->other_vehicle` являются borrowed callback-scoped handles. Их нельзя
освобождать; после callback они становятся stale. Чтобы сохранить снимок:

```cpp
WotbModVehicleHandle saved = nullptr;
host->vehicle_clone(mod, event->vehicle, &saved);
// ...
host->vehicle_release(mod, saved);
```

Vehicle handle в событии может быть `nullptr`, если entity уже удалён из
активного registry к моменту dispatch. Типизированный POD payload при этом
остаётся валидным.

Все перечисленные маски имеют native ingress в bundled loader и передаются в
`WotbModRuntime_NotifyClientEvent`. Для `UI_INPUT` неизвестные текущему hook
поля (`buttons`, `pointer_id`, `modifiers`) остаются нулевыми, а local
coordinates совпадают со screen coordinates. Для `AMMO_CHANGED` поле `count`
равно `-1`, если клиентский handler не предоставил количество. Native detour
читает shell ID из подтверждённого поля listener `+0x20` до и после original:
`shell_id` публикуется только если клиент действительно принял и записал новое
значение; отклонённый аргумент события не создаёт. Entity mapping
`SHELL_HIT` является best-effort и уточняется через текущий vehicle registry.
Адреса и ABI-наблюдения перечислены в `../re_anchors.md`. Эти шесть новых
источников установлены в 11.19 (`12/12` hooks), но ещё не стимулированы полным
набором действий в свежем реальном бою.

### 10.18 Полноценные шкурки и 3D-стили танков

ABI 2.9 добавляет четыре функции в конец `WotbModHostApi`:

```cpp
WotbModResult vehicle_skin_register(
    WotbModHandle mod,
    const WotbModVehicleSkinDescriptor* descriptor,
    WotbModVehicleSkinHandle* out_skin);
WotbModResult vehicle_skin_set_enabled(
    WotbModHandle mod,
    WotbModVehicleSkinHandle skin,
    int32_t enabled);
WotbModResult vehicle_skin_get_info(
    WotbModHandle mod,
    WotbModVehicleSkinHandle skin,
    WotbModVehicleSkinInfo* out_info);
WotbModResult vehicle_skin_release(
    WotbModHandle mod,
    WotbModVehicleSkinHandle skin);
```

Скин — это набор точных замен штатных DAVA virtual paths:

```cpp
WotbModVehicleSkinAsset assets[] = {
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_MESH,
     "~res:/3d/Tanks/USA/M4A3E8/hull.sc2",
     "~res:/Mods/author.skin/hull.sc2"},
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_MATERIAL,
     "~res:/3d/Tanks/USA/M4A3E8/materials.yaml",
     "~res:/Mods/author.skin/materials.yaml"},
    {sizeof(WotbModVehicleSkinAsset), WOTBMOD_SKIN_ASSET_TEXTURE,
     "~res:/3d/Tanks/USA/M4A3E8/hull.tex",
     "~res:/Mods/author.skin/hull.tex"},
};

WotbModResourceMountInfo mount = {};
mount.struct_size = sizeof(mount);
mount.virtual_root = "~res:/Mods/author.skin/";
mount.source_directory = "skin";
mount.priority = 100;
mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
WotbModResourceMountId mountId = 0;
host->resource_mount(mod, &mount, &mountId);

WotbModVehicleSkinDescriptor descriptor = {};
descriptor.struct_size = sizeof(descriptor);
descriptor.skin_id = "author.m4a3e8-style";
descriptor.vehicle_name = "M4A3E8";
descriptor.assets = assets;
descriptor.asset_count =
    sizeof(assets) / sizeof(assets[0]);
descriptor.priority = 100;
descriptor.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;

WotbModVehicleSkinHandle skin = nullptr;
host->vehicle_skin_register(
    mod, &descriptor, &skin);
```

`stock_virtual_path` сравнивается после нормализации без учёта регистра.
Физический запрос `path.ext.dvpl` сопоставляется с логическим
`stock_virtual_path = path.ext`, поэтому мод не обязан дублировать mappings
для packed и loose вариантов.
`replacement_virtual_path` должен разрешаться через активный mount того же
мода либо указывать на существующий штатный ресурс игры под `~res:/`.
Штатная цель проверяется в локальном каталоге `Data` с поддержкой loose и
`.dvpl` файлов. Поэтому stock-to-stock замены, например Object 260 на T-34-85,
не требуют копировать ресурсы игры в пакет мода. Произвольный absolute path и
path traversal запрещены. Для полноценной замены конкретного типа танка
перечислите его mesh `.sc2` и `.scg`; при необходимости отдельно добавьте
material/FX YAML и используемые `.tex`. `vehicle_name` — метаданные для UI
loader-а; фактический выбор выполняется точным stock path, поэтому скин не
затрагивает другие танки.

Если несколько включённых скинов заменяют один stock path, выигрывает больший
`priority`; при равенстве — более новая регистрация. Лимиты: 16 скинов на
мод и 32 asset mapping на скин. `vehicle_skin_get_info` возвращает id,
vehicle name, enabled, priority, flags и количество mappings. Все строки и
entries копируются runtime-ом, поэтому исходные массивы можно освободить после
успешного `register`.

Bundled loader ставит guarded hook на штатный `DAVA::File::Create`. Сначала
проверяется skin mapping, затем обычный global resource overlay, после чего
вызывается оригинальный file loader. Суффикс `.dvpl` нормализуется так, чтобы
штатный DAVA fallback продолжал работать. Уже загруженный объект не
переписывается в памяти: новое правило применяется к следующим file opens, а
существующая модель обновится при обычном reconnect/reload жизненного цикла
гаража или боя.

Handle принадлежит регистрирующему моду. Disable, callback fault, unload и
shutdown автоматически удаляют его mappings до удаления mounts. При ручной
очистке рекомендуется сначала `vehicle_skin_release`, затем
`resource_unmount`.

## 11. Интеграция runtime в loader

### 11.1 Инициализация

```cpp
WotbModRuntimeOptions options = {};
options.struct_size = sizeof(options);
options.game_directory = gameDirectory;
options.mods_directory = nullptr; // <game>\mods
options.game_module = GetModuleHandleA(nullptr);
options.log_sink = &LoaderLog;
options.log_user_data = logger;
options.hook_backend = &hooks;
options.resource_backend = &resources;
options.audio_backend = &audio;
options.sound_backend = &sound;
options.gameplay_backend = &gameplay;

WotbModResult result = WotbModRuntime_Initialize(&options);
if (result == WOTBMOD_OK) {
    result = WotbModRuntime_LoadAll();
}
```

Поля:

| Поле | Поведение |
| --- | --- |
| `game_directory` | Может быть `nullptr`; тогда берётся каталог exe. |
| `mods_directory` | Может быть `nullptr`; тогда `<game>\mods`. |
| `game_module` | Может быть `nullptr`; тогда `GetModuleHandleA(nullptr)`. |
| `log_sink` | Может быть `nullptr`; используется `OutputDebugStringA`. |
| `log_user_data` | Передаётся в log sink. |
| `hook_backend` | Опционально; без него hook API вернёт `PLATFORM`. |
| `resource_backend` | Опционально; без него non-audio typed load вернёт `PLATFORM`. |
| `audio_backend` | Опционально; отвечает за playback и, в ABI 2.3, может загружать custom audio files. |
| `sound_backend` | Опционально; ABI 2.4 native DAVA/Wwise events. Без него `sound_event_create` вернёт `PLATFORM`. |
| `gameplay_backend` | Опционально; ABI 2.8 vehicle snapshots. Без него `vehicle_*` query вернут `PLATFORM`. |

Не вызывайте инициализацию и загрузку модов из `DllMain`. `LoadLibrary`,
callbacks и файловые операции под loader lock небезопасны. Используйте worker
thread после полной загрузки процесса.

### 11.2 Log sink

```cpp
static void WOTBMOD_CALL LoaderLog(
    WotbModLogLevel level,
    const char* message,
    void* userData) {
    Logger* logger = static_cast<Logger*>(userData);
    logger->Write(level, message);
}
```

Sink может вызываться с worker, render и других потоков. Реализация должна
быть thread-safe.

### 11.3 Hook backend

```cpp
WotbModRuntimeHookBackend hooks = {};
hooks.struct_size = sizeof(hooks);
hooks.user_data = hookEngine;
hooks.create = &HookCreate;
hooks.enable = &HookEnable;
hooks.disable = &HookDisable;
hooks.remove = &HookRemove;
```

Backend обязан предоставлять все четыре операции. Частично заполненная
структура считается недоступным backend.

`create` должен записать trampoline/original pointer в `out original`.

### 11.4 Resource backend

```cpp
WotbModRuntimeResourceBackend resources = {};
resources.struct_size = sizeof(resources);
resources.user_data = davaBridge;
resources.load = &LoadDavaResource;
resources.reload = &ReloadDavaResource; // optional
resources.release = &ReleaseDavaResource;
resources.registry_changed = &ResourceRegistryChanged; // optional
resources.ui_set_geometry = &UiSetGeometry;             // ABI 2.5 optional
resources.ui_set_visible = &UiSetVisible;               // ABI 2.5 optional
resources.ui_add_child = &UiAddChild;                   // ABI 2.5 optional
resources.ui_remove_child = &UiRemoveChild;             // ABI 2.5 optional
resources.scene_set_transform = &SceneSetTransform;     // ABI 2.5 optional
resources.scene_add_child = &SceneAddChild;             // ABI 2.5 optional
resources.scene_remove_child = &SceneRemoveChild;       // ABI 2.5 optional
resources.ui_create = &UiCreate;                        // ABI 2.6 optional
resources.ui_get_active_screen = &UiGetActiveScreen;    // ABI 2.6 optional
resources.scene_entity_create = &SceneEntityCreate;     // ABI 2.6 optional
resources.scene_get_active = &SceneGetActive;           // ABI 2.6 optional
resources.clone = &CloneDavaResource;                   // ABI 2.7 optional
resources.ui_find_by_name = &UiFindByName;               // ABI 2.8 optional
resources.ui_get_parent = &UiGetParent;                  // ABI 2.8 optional
resources.ui_get_child_count = &UiGetChildCount;         // ABI 2.8 optional
resources.ui_get_child_at = &UiGetChildAt;               // ABI 2.8 optional
resources.ui_get_state = &UiGetState;                    // ABI 2.8 optional
resources.ui_set_input_enabled = &UiSetInputEnabled;     // ABI 2.8 optional
resources.ui_set_disabled = &UiSetDisabled;              // ABI 2.8 optional
```

`load` и `release` обязательны для готовности backend. `reload` и
`registry_changed` опциональны. Object callbacks также опциональны:
отсутствующая операция возвращает `WOTBMOD_ERROR_PLATFORM`. Runtime
prefix-копирует структуру по `struct_size`, поэтому старые loader backends
остаются совместимыми.

Этот backend обязателен для UI/YAML/scene/texture/generic объектов. Обработка
`AUDIO_CLIP` здесь нужна только для старого или engine-native маршрута: custom
loose audio может полностью обслуживаться audio backend ABI 2.3.

Пример диспетчера:

```cpp
static WotbModResult WOTBMOD_CALL LoadDavaResource(
    void* userData,
    const WotbModResourceLoadRequest* request,
    void** outNativeResource) {
    DavaBridge* bridge = static_cast<DavaBridge*>(userData);
    if (!request || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    switch (request->type) {
        case WOTBMOD_RESOURCE_UI_PACKAGE:
            return bridge->LoadUiPackage(
                request->virtual_path,
                outNativeResource);

        case WOTBMOD_RESOURCE_UI_CONTROL:
            return bridge->LoadUiControl(
                request->virtual_path,
                request->object_name,
                outNativeResource);

        case WOTBMOD_RESOURCE_YAML_DOCUMENT:
            return bridge->LoadYaml(
                request->virtual_path,
                outNativeResource);

        case WOTBMOD_RESOURCE_SCENE:
            return bridge->LoadScene(
                request->virtual_path,
                outNativeResource);

        case WOTBMOD_RESOURCE_TEXTURE:
            return bridge->LoadTexture(
                request->virtual_path,
                outNativeResource);

        case WOTBMOD_RESOURCE_AUDIO_CLIP:
            return bridge->LoadAudioClip(
                request->virtual_path,
                outNativeResource);

        default:
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
}
```

Backend возвращает native pointer только runtime. Он не попадает напрямую в
mod DLL.

Object callbacks получают те же native pointers, ранее возвращённые `load`.
Runtime гарантирует тип/ownership public handles и вызывает эти callbacks
только на bound `DispatchFrame` thread. Pair callback получает parent первым,
child вторым. Все callbacks защищены SEH.

Bundled `WotbModDavaResources_Create` заполняет полный набор ABI 2.5:
`UIControl::SetPosition`, `SetSize`, проверенную visible-flag mutation,
`AddControl`, `RemoveControl`,
`TransformComponent::SetLocalTransform`, `Entity::AddNode` и
`Entity::RemoveNode`. Для ABI 2.6 он также предоставляет конструкторы
`UIControl`/`Entity`, `UIControlSystem::GetScreen` и active Scene tracker.
Runtime backend callbacks: `ui_create`, `ui_get_active_screen`,
`scene_entity_create`, `scene_get_active`. Версия-зависимые vtable slots,
layout и RVA находятся в `../re_anchors.md`.

ABI 2.7 callback `clone` создаёт независимую backend-обёртку того же native
объекта и удерживает его собственным native `Retain`. Runtime передаст
полученный pointer в обычный `release`. `clone` обязателен только для
`resource_clone` и client-event payload; старый backend без него продолжает
загружаться, но эти операции возвращают `WOTBMOD_ERROR_PLATFORM`.

ABI 2.8 bridge добавляет поиск по `FastName`, parent/child traversal, чтение
геометрии/visible/input/disabled state и штатные
`SetInputEnabled`/`SetDisabled`. Возвращаемые backend wrappers владеют
отдельным native `Retain`.

### 11.5 Audio backend

Loader связывает opaque Audio API с выбранным звуковым движком клиента:

```cpp
WotbModRuntimeAudioBackend audio = {};
audio.struct_size = sizeof(audio);
audio.user_data = soundBridge;
audio.load_clip = &LoadAudioFile;
audio.reload_clip = &ReloadAudioFile;          // optional
audio.release_clip = &ReleaseAudioClip;
audio.play = &PlayAudioClip;
audio.pause = &PausePlayback;                  // optional
audio.resume = &ResumePlayback;                // optional
audio.stop = &StopPlayback;                    // optional
audio.set_parameters = &SetPlaybackParameters; // optional
audio.get_state = &GetPlaybackState;            // optional
audio.release = &ReleasePlayback;
```

Сигнатуры:

```cpp
WotbModResult LoadAudioFile(
    void* user_data,
    const char* resolved_file_path,
    void** out_native_audio_clip);

WotbModResult AudioClipOperation(
    void* user_data,
    void* native_audio_clip);

WotbModResult PlayAudioClip(
    void* user_data,
    void* native_audio_clip,
    const WotbModAudioPlayInfo* play_info,
    void** out_native_playback);

WotbModResult PlaybackOperation(
    void* user_data,
    void* native_playback);

WotbModResult SetPlaybackParameters(
    void* user_data,
    void* native_playback,
    const WotbModAudioPlayInfo* parameters);

WotbModResult GetPlaybackState(
    void* user_data,
    void* native_playback,
    WotbModAudioState* out_state);
```

`play` и `release` обязательны. `release` обязан остановить звук, если он ещё
играет, и уничтожить native playback. Остальные callbacks опциональны;
отсутствующая управляющая операция возвращает `WOTBMOD_ERROR_PLATFORM`.
Только `get_state` имеет fallback: runtime возвращает cached state.

Для custom-файлов одновременно нужны `load_clip` и `release_clip`.
`reload_clip` опционален. Runtime разрешает виртуальный путь через mounts и
передаёт `load_clip` уже готовый физический путь. Callback обязан декодировать
или открыть файл, вернуть opaque native clip и не сохранять pointer
`resolved_file_path` после возврата.

Все callbacks защищены SEH. При fault возвращается
`WOTBMOD_ERROR_CALLBACK_FAULT`, а public handle сохраняется для повторного
cleanup, если native playback уже был создан.

`native_audio_clip` — значение, ранее возвращённое `load_clip` либо resource
backend для `WOTBMOD_RESOURCE_AUDIO_CLIP`. `native_playback` никогда не
передаётся mod DLL. DAVA/sound-engine классы, calling conventions и адреса
остаются внутри loader bridge.

В `re_anchors.md` подтверждены singleton `DAVA::SoundSystem`, создание и
управление Wwise sound events. Эти anchors относятся к событиям, уже
зарегистрированным в клиентских Wwise banks. Произвольные внешние WAV/MP3 —
отдельный путь, реализованный callbacks `load_clip`/`release_clip` или готовым
Windows backend ниже.

#### Готовый Windows custom-audio backend

Если loader не должен писать собственный decoder/player, используйте
`wotb_mod_windows_audio.h`:

```cpp
WotbModWindowsAudioOptions audioOptions = {};
audioOptions.struct_size = sizeof(audioOptions);
audioOptions.max_decoded_bytes = 0; // default: 128 MiB на clip

WotbModWindowsAudioHandle audioHandle = nullptr;
WotbModRuntimeAudioBackend audio = {};

WotbModResult result = WotbModWindowsAudio_Create(
    &audioOptions,
    &audioHandle,
    &audio);
if (result != WOTBMOD_OK) {
    return result;
}

options.audio_backend = &audio;
result = WotbModRuntime_Initialize(&options);
```

Backend синхронно декодирует custom-файл через Media Foundation в PCM и
воспроизводит его через XAudio2. Обычно доступны WAV, MP3, AAC/M4A и WMA.
Фактический набор зависит от Media Foundation decoders в Windows. Один clip
не может занимать больше `max_decoded_bytes`; ноль выбирает 128 MiB.

Порядок shutdown:

```cpp
WotbModRuntime_Shutdown();
WotbModResult destroyResult =
    WotbModWindowsAudio_Destroy(audioHandle);
```

`Destroy` возвращает `WOTBMOD_ERROR_ACCESS_DENIED`, если runtime ещё держит
clip или playback. Это обнаруживает неправильный порядок cleanup. Финальный
loader должен линковать `xaudio2.lib`, `mfplat.lib`, `mfreadwrite.lib`,
`mfuuid.lib` и `ole32.lib`.

Готовый backend поддерживает 2D playback, loop, pause/resume/stop, state,
volume, pitch и pan. Spatial 3D и OGG требуют отдельного backend/decoder.

### 11.6 Native DAVA sound backend

Для текущего x86 клиента готовый bridge находится в
`wotb_mod_dava_sound.h`:

```cpp
WotbModDavaSoundOptions soundOptions = {};
soundOptions.struct_size = sizeof(soundOptions);
soundOptions.game_module = GetModuleHandleA(nullptr);

WotbModDavaSoundHandle soundHandle = nullptr;
WotbModRuntimeSoundBackend sound = {};
WotbModResult result = WotbModDavaSound_Create(
    &soundOptions,
    &soundHandle,
    &sound);
if (result != WOTBMOD_OK) {
    return result;
}

options.sound_backend = &sound;
```

Нулевые RVA-поля выбирают подтверждённые defaults клиента
`11.19.0.834 x86`. Loader может передать новые RVA после re-anchor:

- storage singleton `DAVA::SoundSystem*`;
- `FastName` constructor и ref-counted `Release`;
- default sound group;
- vtables direct hybrid system, live `SoundSystemProxy`, hybrid event и
  Wwise event.

В живом процессе singleton является `DAVA::SoundSystemProxy`. Bridge
проверяет vtable proxy, после чего вызывает его slot `+0x0C`; proxy
перенаправляет вызов во внутренний `WwiseHybridSoundSystem*` по `this+0x20`.
Все конкретные адреса и доказательства находятся в `../re_anchors.md`.

`create` и `release` обязательны для собственного backend. Остальные
callbacks опциональны и при отсутствии дают `WOTBMOD_ERROR_PLATFORM`, кроме
optional `get_state`: runtime может вернуть cached lifecycle state.
Third-party DLL никогда не получает `DAVA::SoundEvent*`.

Правильный shutdown:

```cpp
WotbModRuntime_Shutdown();
WotbModResult destroyResult =
    WotbModDavaSound_Destroy(soundHandle);
```

`Destroy` возвращает `ACCESS_DENIED`, пока runtime владеет хотя бы одним
native event. После обновления клиента bridge нельзя использовать до проверки
anchors и vtables.

### 11.7 File resolver integration

Проверенные адреса находятся в `../re_anchors.md`, раздел
`Client resources`. Для текущей версии клиента центральная точка разрешения
файла — `DAVA::File::CheckFsDelegateThenTagsOnAbsolutePath`.

Логика detour:

```cpp
Result FileResolverDetour(/* exact DAVA ABI args */) {
    const char* requestedPath = ExtractRequestedPath(/* args */);

    uint32_t required = 0;
    WotbModResult modResult =
        WotbModRuntime_ResolveResourcePath(
            requestedPath,
            nullptr,
            &required);

    if (modResult == WOTBMOD_ERROR_BUFFER_TOO_SMALL) {
        char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
        uint32_t capacity = sizeof(resolved);
        modResult = WotbModRuntime_ResolveResourcePath(
            requestedPath,
            resolved,
            &capacity);
        if (modResult == WOTBMOD_OK) {
            return CallOriginalWithReplacementPath(resolved);
        }
    }

    return CallOriginalUnchanged(/* args */);
}
```

Обязательное правило: при `NOT_FOUND` или любой ошибке resolver нужно вызвать
original без изменения пути. Тогда отсутствие mod override не ломает базовые
ресурсы игры.

Точная DAVA C++ сигнатура не входит в публичный ABI. Она принадлежит loader
bridge и повторно проверяется после обновления клиента.

### 11.8 Registry generation

```cpp
uint64_t WotbModRuntime_GetResourceGeneration(void);
```

Generation увеличивается при mount/unmount и автоматическом удалении mounts.
Loader может использовать её для инвалидирования собственного cache.

Если задан `registry_changed`, callback получает новое значение сразу после
изменения registry.

### 11.9 Frame dispatch

```cpp
WotbModRuntime_DispatchFrame(
    swapChain,
    device,
    deviceContext,
    width,
    height,
    deltaSeconds);
```

Вызывать один раз на кадр из стабильной render/frame integration point.
Первый вызов привязывает текущий thread id как runtime main thread. Каждый
следующий вызов сначала опустошает ABI 2.5 main-thread queue, затем вызывает
`on_frame`.

Если `deltaSeconds <= 0`, runtime вычисляет delta через
`QueryPerformanceCounter`.

### 11.10 Client event ingress

Loader сообщает runtime о подтверждённых переходах клиента:

```cpp
WotbModRuntime_NotifyUiScreenChanged(
    previous_native_resource,
    native_resource);
WotbModRuntime_NotifySceneActivated(native_resource);
WotbModRuntime_NotifySceneDeactivated(native_resource);
```

Аргументы — backend resources, не публичные mod handles. Runtime синхронно
клонирует ненулевые аргументы через `resource_backend.clone`, поэтому loader
может освободить свои wrappers сразу после возврата. Сами callbacks
выполняются позже из `DispatchFrame`. До initialize функции возвращают
`WOTBMOD_ERROR_DISABLED`; при заполненной очереди —
`WOTBMOD_ERROR_LIMIT_REACHED`.

Bundled live loader отслеживает UI root через
`UIControlSystem::GetScreen`, а Scene transitions — через проверенные
`Scene::Activate`, `Scene::Deactivate` и `Scene::Draw` hooks. Сравнение native
object identity подавляет повторное событие для того же root.

### 11.11 Gameplay backend и generic event ingress

Loader регистрирует приватные vehicle tokens:

```cpp
WotbModRuntimeGameplayBackend gameplay = {};
gameplay.struct_size = sizeof(gameplay);
gameplay.user_data = registry;
gameplay.vehicle_get_local = &VehicleGetLocal;
gameplay.vehicle_get_by_entity_id = &VehicleGetByEntityId;
gameplay.vehicle_get_count = &VehicleGetCount;
gameplay.vehicle_get_at = &VehicleGetAt;
gameplay.vehicle_clone = &VehicleClone;
gameplay.vehicle_release = &VehicleRelease;
gameplay.vehicle_get_info = &VehicleGetInfo;
```

Все семь callbacks нужны для полного backend. Token никогда не передаётся
mod DLL. Runtime оборачивает его owner-aware handle-ом и гарантирует
`vehicle_release` при явном release, disable, fault, unload и shutdown.

Нативный hook передаёт событие через единый ingress:

```cpp
WotbModRuntimeClientEvent event = {};
event.struct_size = sizeof(event);
event.type = WOTBMOD_EVENT_SHOT_FIRED;
event.primary_entity_id = entityId;
event.payload_size = sizeof(event.payload.shot);
event.payload.shot.shot_code = shotCode;
WotbModRuntime_NotifyClientEvent(&event);
```

`WotbModRuntime_NotifyClientEvent` копирует POD payload до возврата. Если
заданы `primary_entity_id`/`other_entity_id`, runtime при доставке создаёт два
borrowed callback-scoped vehicle handles через gameplay backend. Опциональные
resource wrappers клонируются так же, как в специализированных UI/Scene
ingress-функциях. При заполненной FIFO возвращается `LIMIT_REACHED`; до
initialize — `DISABLED`.

### 11.12 Shutdown

```cpp
WotbModRuntime_Shutdown();
```

Runtime выгружает моды в обратном порядке:

```text
on_disable (если enabled)
-> remove hooks
-> stop/release native sound events
-> stop/release audio playbacks
-> release resources / unmount
-> on_unload
-> FreeLibrary
```

Shutdown должен выполняться на безопасном control/worker thread, когда
`DispatchFrame` больше не вызывается. После него loader уничтожает
`WotbModDavaSoundHandle` и `WotbModWindowsAudioHandle`.

### 11.13 Crash-loop safe mode

До инициализации V3 runtime и до любого third-party `LoadLibrary` runtime
атомарно создаёт:

```text
mods\cache\runtime_session.marker
```

Marker содержит PID, время последнего обновления, последнюю фазу lifecycle и
ID/имя последнего мода, если оно уже известно. Runtime обновляет его перед
package preflight, загрузкой native/content-only мода, enable/disable и
shutdown callback. `WotbModRuntime_Shutdown` удаляет marker текущей сессии
только после полного cleanup. Bundled loader также регистрирует узкий CRT
exit-handler, который при штатном завершении процесса выполняет только
удаление файла; crash, `TerminateProcess` и аварийное выключение этот handler
не выполняют.

Если на следующем старте marker уже существует, runtime входит в safe mode до
сканирования manifest/DLL:

- V3 bootstrap, host table и logging продолжают работать для диагностики;
- `WotbModRuntime_LoadAll` возвращает `WOTBMOD_OK`, но не сканирует и не
  загружает packages/loose DLL;
- `get_mod_count()` остаётся равен нулю;
- явный `set_mod_enabled(..., 1)` возвращает `WOTBMOD_ERROR_DISABLED`;
- log содержит полный marker path, `last_phase` и `last_mod`.

Forensic marker, вызвавший safe mode, намеренно не удаляется при shutdown
этой safe-сессии: обычный повторный запуск не должен сам включать
подозрительные моды. Для восстановления нужно закрыть игру, изучить log и
удалить `runtime_session.marker`. Явный одноразовый обход:

```bat
set WOTBMOD_SAFE_MODE_OVERRIDE=1
WorldOfTanksBlitz.exe
```

Override удаляет stale marker и сразу создаёт новый marker текущей сессии.
Поэтому повторный crash снова включает safe mode. Если Windows не позволяет
удалить marker, override fail-closed: моды остаются заблокированы. После
диагностики переменную окружения следует удалить. Нормальный выход клиента
через `TerminateProcess` неотличим от crash и может дать безопасный false
positive; это устраняется тем же явным восстановлением.

## 12. Runtime API

### `WotbModRuntime_Initialize`

Инициализирует каталоги, module handle, logging и backends.

Возвращает:

- `OK`;
- `INVALID_ARGUMENT`;
- `ALREADY_EXISTS`;
- `PLATFORM`.

### `WotbModRuntime_LoadAll`

Сканирует только `mods\*.dll`, сортирует пути регистронезависимо по имени и
загружает в алфавитном порядке.

Вызвать можно один раз между initialize/shutdown.

В crash-loop safe mode вызов считается выполненным и возвращает `OK`, но
является диагностическим no-op: ни manifest, ни DLL не загружаются.

Если хотя бы один найденный мод завершил загрузку ошибкой, функция возвращает
`WOTBMOD_ERROR_PLATFORM`, но успешно загруженные моды остаются активными.
Смотрите log для списка ошибок.

### `WotbModRuntime_DispatchFrame`

Привязывает dispatch thread, выполняет отложенные callbacks/object operations
в FIFO-порядке, доставляет client events, затем формирует
`WotbModFrameInfo` и вызывает `on_frame` enabled-модов.
До initialize вызов просто ничего не делает.

### `WotbModRuntime_NotifyUiScreenChanged`

Ставит в очередь переход active UI screen. Оба backend resource могут быть
`nullptr`, но не одновременно.

### `WotbModRuntime_NotifySceneActivated`

Ставит в очередь активацию Scene. Backend resource обязателен.

### `WotbModRuntime_NotifySceneDeactivated`

Ставит в очередь деактивацию Scene. Backend resource обязателен.

### `WotbModRuntime_NotifyClientEvent`

Ставит в очередь generic UI/gameplay event, копирует payload и позже доставляет
его подписчикам из `DispatchFrame`. Проверяет `struct_size`, event mask,
`payload_size` и optional resource type.

### `WotbModRuntime_Shutdown`

Безопасно игнорирует повторный вызов после shutdown. Очищает runtime state,
backends, audio playbacks, resources, mounts и frame counter.

### `WotbModRuntime_GetHostApi`

Возвращает host table для UI/loader-кода.

### `WotbModRuntime_ResolveResourcePath`

Loader-level версия `resource_resolve`, не требует mod handle.

Возвращает:

- `OK` и физический override path;
- `BUFFER_TOO_SMALL` и требуемый размер;
- `NOT_FOUND`, если override отсутствует;
- `INVALID_ARGUMENT`;
- `DISABLED`, если runtime не активен.

### `WotbModRuntime_ResolveVehicleSkinPath`

Loader-level resolver ABI 2.9. Принимает запрошенный игрой stock virtual path,
выбирает включённый skin mapping по priority/registration sequence и
возвращает физический файл только из mount владельца скина. Buffer contract и
коды ошибок совпадают с `WotbModRuntime_ResolveResourcePath`.

Bundled loader вызывает сначала этот resolver из hook
`DAVA::File::Create`, затем обычный resource resolver. Третьим лицам эту
функцию как средство arbitrary path lookup предоставлять не требуется.

### `WotbModRuntime_GetResourceGeneration`

Возвращает текущую generation registry.

### `WotbModApi_GetHost` и `WotbModApi_GetVersion`

Опциональные host exports для loader UI:

```cpp
const WotbModHostApi* WotbModApi_GetHost(void);
uint32_t WotbModApi_GetVersion(void);
```

Обычный mod DLL должен использовать `host`, переданный в `WotbModLoad`.

## 13. Потоки и синхронизация

Гарантированные контексты:

| Операция | Обычный поток |
| --- | --- |
| `Initialize`, `LoadAll`, initial `WotbModLoad`, initial `on_enable` | Loader worker |
| `main_thread_enqueue`, client-event callback, queued UI/Scene backend, `on_frame` | Поток `DispatchFrame`, обычно render |
| UI `set_mod_enabled` | Поток UI/loader, выбранный архитектурой |
| UI/Scene API с dispatch thread | Синхронно на dispatch thread |
| UI/Scene API с другого thread | Валидация/queue на caller thread, backend позже на dispatch thread |
| Existing-UI query и Vehicle query | Только dispatch thread |
| Client-event callback и borrowed vehicle handles | Только dispatch thread |
| Audio API и его backend callbacks | Поток, вызвавший Audio API |
| File path resolution | Поток игрового файлового запроса |
| `Shutdown`, `on_disable`, `on_unload` | Loader control/worker |

Mount registry защищён SRW lock и поддерживает параллельные resolve-запросы.
Main-thread queue защищена отдельным SRW lock; geometry/transform копируются,
а resource handles повторно разрешаются перед выполнением. Client-event queue
также защищена SRW lock; loader resources клонируются при ingress и
освобождаются после dispatch либо shutdown.

При этом мод обязан самостоятельно синхронизировать свои глобальные данные
между `on_frame`, UI-командами и собственными потоками.

Не вызывайте одновременно `resource_reload` и `resource_release` одного
handle. Сначала остановите использующий его код, затем освободите ресурс.
Не управляйте одним playback одновременно из нескольких потоков; сериализуйте
pause/resume/stop/set/release на стороне мода.

## 14. Текущие лимиты runtime

Это лимиты текущей реализации, а не обещание вечной ABI-константы:

| Объект | Лимит |
| --- | ---: |
| Загруженные моды | 128 |
| Hooks на один мод | 64 |
| Resource mounts всего | 256 |
| Resource mounts на один мод | 32 |
| Loaded resource handles на один мод | 64 |
| Audio playback handles на один мод | 64 |
| Native sound event handles на один мод | 64 |
| Owned vehicle handles на один мод | 64 |
| Main-thread queue всего | 256 |
| Main-thread queue на один мод | 64 |
| Pending client events | 256 |
| Client-event subscriptions на один мод | 16 |
| Одновременные borrowed resource handles на callback | 2 |
| Одновременные borrowed vehicle handles на callback | 2 |
| `id` | 63 байта + NUL |
| `name` | 95 байт + NUL |
| `version` | 31 байт + NUL |
| `author` | 95 байт + NUL |
| `description` | 255 байт + NUL |
| Обычный path | 259 байт + NUL |
| Resource path | 1023 байта + NUL |

## 15. Упаковка мода

Минимальный release:

```text
author.example.dll
```

С ресурсами:

```text
author.example.dll
data-template/
  resources/
    UI/
    3d/
```

Installer/loader должен разложить template в:

```text
mods/data/author.example/resources/
```

Если DLL зависит от дополнительных DLL, размещайте их рядом с mod DLL либо в
каталоге, который разрешён loader dependency policy. Runtime использует
`LoadLibraryExA` с `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` и
`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, с fallback на `LoadLibraryA` для старой
системы.

## 16. Рекомендации разработчикам модов

- Проверяйте major ABI в `WotbModLoad`.
- Проверяйте `host->struct_size` перед использованием функций новой minor ABI.
- Храните `WotbModHandle` только как opaque value.
- Делайте callbacks короткими и не бросайте исключения через ABI.
- Освобождайте ресурсы в `on_disable`, даже несмотря на runtime cleanup.
- Используйте собственный `~res:/Mods/<id>/` namespace.
- Не монтируйте корень `~res:/` без необходимости.
- Для override существующего ресурса используйте явный priority и
  документируйте конфликт с другими модами.
- Не сохраняйте `WotbModFrameInfo*` после возврата из callback.
- Не сохраняйте borrowed resource из `WotbModClientEvent`; клонируйте его
  внутри callback через `resource_clone`.
- Удаляйте подписку через `event_unsubscribe`, если она больше не нужна;
  disable/unload также удаляет её автоматически.
- Не передавайте временный/stack `user_data` в `main_thread_enqueue`; callback
  выполняется только на следующем `DispatchFrame`.
- Если queued UI/Scene операции нужен точный backend result, вызывайте её из
  main-thread callback или `on_frame`.
- Перед release child resource выполните соответствующий
  `ui_control_remove_child`/`scene_remove_child`.
- Не сохраняйте pointer на `WotbModResourceLoadRequest`, полученный backend:
  он валиден только во время вызова.
- После `resource_release` сразу обнуляйте handle.
- Для custom audio предпочитайте `audio_clip_load`, если
  `HasCustomAudioFileApi(host)` вернул `true`.
- Сначала вызывайте `audio_release`, затем `resource_release` для clip.
- После `audio_release` сразу обнуляйте playback handle.
- Не выгружайте свою DLL самостоятельно.

## 17. Рекомендации автору loader

- Запускайте runtime вне `DllMain`.
- Останавливайте frame dispatch до shutdown.
- Делайте hook backend idempotent для disable/remove cleanup.
- Валидируйте текущий SHA-256 клиента перед применением известных RVA.
- После обновления клиента перепроверяйте `re_anchors.md`.
- Не передавайте DAVA/STL-типы в публичную mod DLL.
- Для UI, YAML, scene, texture и audio clip создавайте typed wrappers.
- Для custom audio подключайте готовый Windows backend либо реализуйте все
  `load_clip`/`release_clip` и `play`/`release`.
- При отсутствии override всегда вызывайте оригинальный file resolver.
- Resource backend `release` должен корректно освобождать любой объект,
  успешно возвращённый `load`.
- Audio backend `release` должен останавливать и освобождать playback даже
  при отсутствии optional `stop`.
- Log sink и file resolver bridge должны быть thread-safe.

## 18. Диагностика

### Мод не найден

Проверьте:

- DLL лежит непосредственно в `mods`, а не во вложенной папке;
- расширение `.dll`;
- архитектура DLL x86;
- все зависимости доступны;
- присутствует экспорт `WotbModLoad`.

Проверка:

```powershell
dumpbin /exports .\mods\author.example.dll
```

### `WOTBMOD_ERROR_UNSUPPORTED_ABI`

Проверьте `out_info->abi_version`, `struct_size` и major ABI. Мод, собранный
под ABI 1, не совместим с ABI 2.

### `hook_create` возвращает `PLATFORM`

Loader не передал полный `WotbModRuntimeHookBackend` либо его операция
вернула ошибку.

### `resource_mount` возвращает `NOT_FOUND`

`source_directory` ещё не существует внутри data directory мода. Создайте или
установите ресурсы до `on_enable`.

### `resource_mount` возвращает `ACCESS_DENIED`

Путь вышел за персональный data directory. Абсолютные пути и `..` запрещены.

### `resource_load` возвращает `PLATFORM`

Loader не передал `resource_backend`, не предоставил `load/release`, либо
DAVA wrapper не смог создать объект. Для `AUDIO_CLIP` ошибка также означает,
что audio backend не предоставил custom-file callbacks и общий resource
backend не умеет загружать clip.

### UI/Scene операция возвращает `PLATFORM`

Resource load backend активен, но его ABI 2.5 object callback отсутствует,
либо bundled DAVA bridge отклонил текущий vtable/layout/anchor. Если вызов был
поставлен в очередь с другого thread, смотрите строку
`queued main-thread work ... failed` в runtime log.

### UI/Scene операция возвращает `INVALID_ARGUMENT`

Проверьте ownership и тип handle: UI-функции принимают только
`WOTBMOD_RESOURCE_UI_CONTROL`, Scene-функции — только
`WOTBMOD_RESOURCE_SCENE`. Parent и child не могут совпадать. Для geometry и
transform также проверьте `struct_size`, конечность чисел, ненулевой
quaternion и ненулевые scale components.

### Existing-UI query возвращает `WRONG_THREAD`

`ui_control_find_by_name`, parent/child traversal и `ui_control_get_state`
синхронны. Вызывайте их из `on_frame`, event callback или собственного
`main_thread_enqueue` callback.

### `vehicle_get_*` возвращает `PLATFORM`, `WRONG_THREAD` или `NOT_FOUND`

`PLATFORM` означает, что loader не передал полный gameplay backend.
`WRONG_THREAD` — query вызван вне dispatch thread. `NOT_FOUND` является
нормальным результатом вне боя, до появления local vehicle или для уже
исчезнувшего entity id.

### `vehicle_release` возвращает `ACCESS_DENIED`

Event vehicle handle является borrowed и действует только внутри callback.
Клонируйте его через `vehicle_clone`, а затем освобождайте полученный owned
handle. Чужой handle также возвращает `ACCESS_DENIED`; stale — 
`INVALID_ARGUMENT`.

### `main_thread_enqueue` возвращает `LIMIT_REACHED`

У мода уже 64 ожидающих элемента либо заполнена глобальная очередь из 256
элементов. Не ставьте одну задачу на каждый объект; объединяйте короткие
операции в один callback и дождитесь следующего `DispatchFrame`.

### `audio_clip_load` возвращает `NOT_FOUND`

Виртуальный путь не разрешился в loose-файл активного mount. Проверьте
`resource_mount`, namespace, physical файл, priority и состояние мода.

### `audio_clip_load` возвращает `PLATFORM`

Loader не предоставил `load_clip`/`release_clip`, а общий resource backend не
поддерживает `WOTBMOD_RESOURCE_AUDIO_CLIP`. Подключите
`WotbModWindowsAudio_Create` либо собственный decoder/player.

### `audio_play` возвращает `PLATFORM`

Loader не передал полный `WotbModRuntimeAudioBackend`, отсутствует обязательный
`play`/`release`, либо sound bridge не смог создать native playback.

### `sound_event_create` возвращает `PLATFORM`

Loader не передал полный `WotbModRuntimeSoundBackend`, либо
`WotbModDavaSound_Create` отклонил module/anchors/vtable текущей версии
клиента. Повторно проверьте sound anchors из `re_anchors.md`.

### `sound_event_create` возвращает `NOT_FOUND`

Native `DAVA::SoundSystem` доступен, но событие не найдено в загруженных
клиентских banks или возвращённый object не соответствует проверенным event
vtables. Проверьте фактический event alias и момент загрузки банка.

### `resource_release` audio clip возвращает `ACCESS_DENIED`

Clip ещё используется хотя бы одним playback. Вызовите `audio_stop` при
необходимости, затем `audio_release`, и только после этого освобождайте clip.

### Overlay зарегистрирован, но игра использует оригинальный файл

Проверьте:

1. вызван ли file resolver detour;
2. передаёт ли он фактический requested path в
   `WotbModRuntime_ResolveResourcePath`;
3. существует ли physical файл;
4. включён ли мод;
5. совпадает ли virtual root;
6. нужен ли `.dvpl` fallback;
7. не перекрывает ли ресурс другой mount с большим priority.

### Мод перешёл в `FAULTED`

Смотрите SEH code и callback name в runtime log. `fault_count` доступен через
`get_mod_info`. После исправления требуется полный restart runtime/игры.

## 19. Проверка перед выпуском

Для SDK/runtime:

```bat
build.cmd
```

Успешный результат должен содержать обе строки:

```text
SMOKE OK
API FULL OK
```

Full-contract suite выполняет 1650 assertions, в том числе семь typed resource
типов, custom audio file routing, реальное Media Foundation WAV decoding и
XAudio2 playback, все 76 функций host API, native sound, UI/Scene backend
contracts, ABI 2.6 factories/active roots, ABI 2.7 client events/resource
clone, ABI 2.8 existing-UI traversal/state, vehicle snapshots, generic
gameplay-event payloads и borrowed/owned vehicle lifecycle, ABI 2.9 skin
registration/priority/owner cleanup/path resolution, main-thread
FIFO/limits/cancellation/worker dispatch, parallel resolver stress,
hook/resource/audio/sound/object/gameplay backend errors, stale handles,
legacy backend structs, malformed ABI/descriptors и повторные runtime cycles.
Тот же `build.cmd` загружает два focused test mods: event-мод получает
детерминированные UI input/hit/ammo/aim/spotted/unspotted payloads, а
skin-мод проходит mount/register/info/disable/enable/release/stale и
mesh/material/texture resolver.

Live integration:

```bat
loader\build_live.cmd
build\live_test_launcher.exe
```

`live_test_launcher.exe` читает моды из изолированного
`build\live_env\mods`. Для установленных каталогом модов из `<game>\mods`
используйте `build\wotb_mod_launcher.exe`. Production-launcher включает
раннюю регистрацию модов до первого открытия моделей ангара; это обязательно
для stock mesh redirects.

Проверенный результат текущего клиента:

```text
CLIENT: 11.19.0.834 x86
SHA-256: 41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD
GAMEPLAY HOOKS: mask=0xFFF (12/12)
API SELFTEST: passes=108 skips=2 failures=0
CUSTOM AUDIO: passes=24 failures=0
NATIVE LIVE: passes=142 skips=0 failures=0
VEHICLE SKIN MOD: passes=15 failures=0
```

Свежий standalone regression отдельно дал:

```text
API SELFTEST: passes=92 skips=10 failures=0
NEW EVENTS MOD: passes=14 failures=0
VEHICLE SKIN MOD: passes=15 failures=0
```

Десять standalone `SKIP` означают отсутствие DAVA/gameplay backends в
standalone host. В live run два
ранних `SKIP` были ожидаемым отсутствием vehicle/Scene до входа в ангар, а
финальный native summary завершился без пропусков. Native test получил active
Scene, прикрепил loose custom `.sc2` и
созданный `Entity`, подтвердил доступность Scene 180 кадров подряд, затем
снял и освободил оба объекта. UI package hook сработал 206 раз, Scene loader
hook — 38 раз; Wwise hook — 1931 раз. Полный локальный
create/enable/hit/disable/reenable/remove hook lifecycle также прошёл.
Native `UIControl`/`Entity` constructors, active screen acquisition,
`UIControl::SetPosition`/`SetSize`, visibility, add/remove child,
`TransformComponent::SetLocalTransform`, Scene add/remove child и release
вернули `WOTBMOD_OK` в живом клиенте.

В том же run доставлены 7 `UI_SCREEN_CHANGED`, 2 `SCENE_ACTIVATED` и
1 `SCENE_DEACTIVATED`. Проверены main-thread delivery, monotonic sequence,
payload types, запрет release/reload borrowed handle, его инвалидирование
после callback и жизнь `resource_clone` после callback.

Текущий ABI 2.9 loader содержит 22 event ingress, включая
`CAMERA_MODE_CHANGED`, и `DAVA::File::Create` skin bridge. Исторический
live-run до добавления camera hook прошёл initialization: установлены
`12/12` native detours, overlay включён, focused skin mod дал `15/0`.
В этом прогоне не создавались реальные
боевые стимулы для battle-only callback-ов и визуально не проверялась
подмена stock mesh конкретного отображаемого танка; эти два пункта остаются
полевой проверкой.

Для точечного live-run доступны два ZIP: `new-gameplay-events-test` пишет
summary после семи focused-событий, включая смену camera mode, а
`vehicle-skin-api-test` заменяет
T-34-85 mesh на mounted copy и его diffuse на cyan/magenta checker DDS.
Отключение второго мода удаляет mappings; уже закэшированную модель нужно
пересоздать штатной сменой танка/ангара.

Для отдельного мода:

1. собрать x86 Release DLL;
2. проверить экспорт `WotbModLoad`;
3. проверить загрузку с чистым `mods.ini`;
4. проверить enable → disable → enable;
5. проверить shutdown;
6. проверить отсутствие backend;
7. проверить отсутствие optional ресурсов;
8. проверить конфликт resource priorities;
9. проверить fault isolation на тестовой сборке;
10. проверить play/pause/resume/stop и 2D параметры, а для spatial backend
    отдельно проверить 3D position/distance;
11. для ABI 2.4+ проверить native event
    create/trigger/pause/stop/RTPC/release;
12. для ABI 2.5+ проверить main-thread defer, UI geometry/visibility/tree и
    Scene transform/tree;
13. для ABI 2.6+ проверить UIControl/Entity create/release, active screen,
    `NOT_FOUND` без активной 3D Scene, затем active Scene и custom `.sc2`
    attach/несколько кадров/detach в hangar;
14. для ABI 2.7+ проверить UI/Scene event masks, FIFO/sequence, unsubscribe,
    callback fault isolation, borrowed lifecycle и `resource_clone`;
15. для ABI 2.8+ проверить existing-UI find/parent/children/state и
    input/disabled mutations;
16. для ABI 2.8+ в реальном бою проверить vehicle query/clone/release,
    battle enter/start/end/left, spawn/despawn, shot, health/damage/destroy и
    reload payloads;
17. для ABI 2.9+ смонтировать полный набор `.sc2`/material YAML/`.tex`,
    зарегистрировать skin, проверить priority/enable/disable/release,
    обновление модели на обычном garage/battle reconnect и восстановление
    штатных ресурсов;
18. убедиться, что после disable не остаются
    hooks/sound-events/playbacks/resources.

## 20. Что API пока не предоставляет

ABI 2.9 не содержит:

- hot reload самой DLL;
- dependency graph между модами;
- semantic version constraints;
- публичные DAVA object pointers;
- lookup произвольного не-vehicle игрового Entity по id/имени;
- создание полноценной корневой `DAVA::Scene` без `.sc2` resource;
- принудительное горячее перестроение уже закэшированного DAVA tank model;
- semantic lookup tank assets по vehicle id: stock paths конкретной версии
  клиента мод указывает явно;
- UI layout/rendering самого loader;
- встроенный OGG decoder и spatial 3D в готовом Windows audio backend;
- сетевые или аккаунтные API;
- постоянный key-value storage кроме INI;
- named registry сырых игровых адресов.

Эти возможности можно добавлять следующими minor-версиями через новые поля в
конце `WotbModHostApi` и новые структуры с `struct_size`, не ломая ABI 2.x.
