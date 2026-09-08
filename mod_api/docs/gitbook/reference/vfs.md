# `wotb.vfs`

Raw-таблица интерфейса `WotbModV3VfsApiV2` (`include/wotbmod/vfs_v2.h`, версия `WOTBMOD_V3_VFS_VERSION_2`). Функции ниже вызываются как `wotb.vfs.<слот>(...)`; при отказе любая отвечает `nil, err`.

Короткий API поверх этой таблицы: [`wotb.files`](../facades/wotb-files.md). Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.

## Права

Забор host-а проверяет перед вызовом: `resources.mod`, `resources.overlay.game`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.vfs.URI_MAX` | `WOTBMOD_V3_VFS_URI_MAX` | define |
| `wotb.vfs.ENTRY_FILE` | `WOTBMOD_V3_VFS_ENTRY_FILE` | enum WotbModV3VfsEntryType |
| `wotb.vfs.ENTRY_DIRECTORY` | `WOTBMOD_V3_VFS_ENTRY_DIRECTORY` | enum WotbModV3VfsEntryType |
| `wotb.vfs.MOUNT_PACKAGE` | `WOTBMOD_V3_VFS_MOUNT_PACKAGE` | enum WotbModV3VfsMountKind |
| `wotb.vfs.MOUNT_OVERLAY` | `WOTBMOD_V3_VFS_MOUNT_OVERLAY` | enum WotbModV3VfsMountKind |
| `wotb.vfs.CHANGE_CREATED` | `WOTBMOD_V3_VFS_CHANGE_CREATED` | enum WotbModV3VfsChangeKind |
| `wotb.vfs.CHANGE_MODIFIED` | `WOTBMOD_V3_VFS_CHANGE_MODIFIED` | enum WotbModV3VfsChangeKind |
| `wotb.vfs.CHANGE_DELETED` | `WOTBMOD_V3_VFS_CHANGE_DELETED` | enum WotbModV3VfsChangeKind |
| `wotb.vfs.MAX_WRITE_BYTES` | `WOTBMOD_V3_VFS_MAX_WRITE_BYTES` | define |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `get_namespace` | — | string |
| `normalize_uri` | uri: string | string |
| `mount_package` | provider_id: string, physical_directory: string, priority: integer | mount: handle |
| `mount_overlay` | provider_id: string, target_game_uri: string, source_mod_uri: string, priority: integer | mount: handle |
| `unmount` | mount: handle | true |
| `resolve` | uri: string | string |
| `open` | uri: string | file: handle |
| `read` | file: handle, offset: integer | bytes |
| `list` | uri: string | entries: array |
| `stat` | uri: string | stat: table:VfsStat |
| `watch` | uri: string | token: handle |
| `get_providers` | uri: string | providers: array |
| `set_provider_priority` | mount: handle, priority: integer | true |
| `get_conflicts` | — | conflicts: array |
| `write_file` | uri: string, data: table:ConstBuffer | true |
| `append_file` | uri: string, data: table:ConstBuffer | true |
| `copy_file` | source_uri: string, destination_uri: string | true |
| `remove_file` | uri: string | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["resources.mod", "resources.overlay.game"]
local get_namespace, err = wotb.vfs.get_namespace()  -- string
if get_namespace == nil then wotb.log.warn("vfs.get_namespace: %s", err) end
local normalize_uri, err = wotb.vfs.normalize_uri("...")  -- string
if normalize_uri == nil then wotb.log.warn("vfs.normalize_uri: %s", err) end
local mount_package, err = wotb.vfs.mount_package("...", "...", 0)  -- mount: handle
if mount_package == nil then wotb.log.warn("vfs.mount_package: %s", err) end
```

