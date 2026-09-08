# V3 Client Resources Gameplay Probe

Этот DLL-мод проверяет 17 интерфейсов client/resources/gameplay.

Проверяются:

- UI V2;
- resources;
- Render;
- Render Native;
- Camera;
- Scene;
- Audio V2;
- vehicle visuals;
- gameplay camera;
- gameplay HUD;
- gameplay hangar;
- gameplay replay;
- public entities;
- BigWorld RPC policy;
- projectile/tracer;
- client compatibility;
- device info и DXGI adapter metadata.

Для каждой доступной таблицы проверяются `struct_size`, interface version и все function-pointer slots.

Behavioural probes не меняют состояние игры:

- читают scale/safe area/viewport;
- читают render backend/frame/viewport;
- запрашивают native pointers только при выданном UNSAFE;
- читают active camera и её параметры;
- читают active scene;
- проверяют ошибку загрузки отсутствующего resource/audio fixture;
- читают local vehicle, gameplay camera/replay state;
- перечисляют только visible public entities;
- проверяют, что BigWorld RPC policy не разрешает payload/injection/modify/drop/replay;
- проверяют invalid projectile handle;
- читают client compatibility, missing bindings и device info.

Мод намеренно не вызывает `leave_to_hangar`, не меняет camera/HUD/hangar/vehicle mesh и не устанавливает tracer style. Function slots этих операций проверяются структурно, а деструктивные действия оставлены для управляемого live test.

Сборка из корня `mod_api`:

```bat
cl /nologo /std:c++17 /EHsc /MD /W4 /WX /permissive- /LD ^
  /Iinclude ^
  examples\v3_client_resources_test_mod\v3_client_resources_test_mod.cpp ^
  /link /OUT:build\v3_client_resources_test_mod.dll
```

Package layout:

```text
manifest.json
bin/windows-x86/v3_client_resources_test_mod.dll
```
