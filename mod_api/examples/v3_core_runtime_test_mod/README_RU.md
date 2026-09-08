# V3 Core Runtime Data Probe

Этот DLL-мод проверяет bootstrap и 23 интерфейса core/runtime/data.

Проверка состоит из двух частей:

1. Для каждой доступной таблицы проверяются `struct_size`, точная interface version и все function-pointer slots.
2. Выполняются безопасные behavioural probes: client info, context/thread/frame, paths, capabilities, permissions, handles, lifecycle, event post, hook enumeration, async thread info, HTTP availability, self lookup через intermod, VFS namespace, storage path, YAML parse, loader backend info, manifest/content parse, catalog status, diagnostics и devtools marker.

`wotbmod.unsafe.native` запрашивается для проверки permission gating. Мод не создаёт address hook.

HTTP и content application считаются корректными в двух случаях:

- операция реально вернула `WOTBMOD_V3_OK`;
- backend честно вернул `WOTBMOD_V3_E_NOT_SUPPORTED`.

В log выводится:

```text
probe complete: passed=<count> failed=<count> unavailable=<count>
```

`failed=0` является необходимым условием успешного probe. `unavailable` показывает интерфейсы, закрытые permission/client/backend, и не маскируется под success.

Сборка из корня `mod_api`:

```bat
cl /nologo /std:c++17 /EHsc /MD /W4 /WX /permissive- /LD ^
  /Iinclude ^
  examples\v3_core_runtime_test_mod\v3_core_runtime_test_mod.cpp ^
  /link /OUT:build\v3_core_runtime_test_mod.dll
```

Package layout:

```text
manifest.json
bin/windows-x86/v3_core_runtime_test_mod.dll
```
