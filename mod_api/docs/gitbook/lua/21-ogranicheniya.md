# Ограничения

- Host и Lua собраны для Windows x86.
- Installed Lua-моды пока являются папками под `mods\lua`, а не отдельными
  `.wotbmod` archives; `.wotbmod` нужен самому native host-у.
- Public-preview bundle подписывает host detached ECDSA P-256/SHA-256 подписью
  и устанавливает соответствующий публичный trust key. Отдельный результат
  `build_lua_host_package.ps1` остаётся локальным unsigned build artifact.
- Системный риск native SEH + C++ locks в основном runtime остаётся отдельной
  задачей и не устраняется Lua sandbox-ом. Закрыт один участок — резервирование
  VFS provider slot (`src/v3/data_services.cpp`, `ReserveVfsProviderSlot`);
  остальные десять критических секций `g_vfs_mutex` относятся к тому же классу
  и не исправлены.
- Проверки ветки выполняются synthetic host-ами. Фактическое поведение UI,
  камеры и других client bridges требует отдельного live-game прогона.
