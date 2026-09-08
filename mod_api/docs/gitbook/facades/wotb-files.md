# `wotb.files`

Фасад поверх raw-таблиц: [`wotb.loaders`](../reference/loaders.md), [`wotb.vfs`](../reference/vfs.md), [`wotb.resources`](../reference/resources.md), [`wotb.yaml`](../reference/yaml.md), [`wotb.events`](../reference/events.md).

## Методы

- `wotb.files.read_text`
- `wotb.files.read_binary`
- `wotb.files.read_json`
- `wotb.files.read_yaml (doc:get`
- `wotb.files.doc:release)`
- `wotb.files.load_texture`
- `wotb.files.load_audio`
- `wotb.files.load_scene`
- `wotb.files.info`
- `wotb.files.release`
- `wotb.files.exists`
- `wotb.files.stat`
- `wotb.files.list`
- `wotb.files.watch`
- `wotb.files.unwatch`

## Как пользоваться

### `wotb.files`: файлы и ресурсы

Поверх `wotb.loaders`, `wotb.vfs`, `wotb.resources`, `wotb.yaml`.

- `read_text(uri [, max_bytes])`, `read_binary(uri [, max_bytes])`,
  `read_json(uri)`; лимит по умолчанию 4 MiB;
- `read_yaml(uri [, limits])` → документ с `doc:get("a.b[2].c")` (скаляр,
  либо `{ kind = "map" | "sequence", size = n }`) и `doc:release()`. Целиком
  в таблицу YAML не превращается: ABI не перечисляет ключи map;
- `load_texture(uri)`, `load_audio(uri)`, `load_scene(uri)` → resource
  (`resources.load` с ожидаемым типом), `info(resource)`, `release(resource)`;
- `exists(uri)` → `true` или `false, reason` (ответ `vfs.stat`), `stat(uri)`,
  `list(uri)`;
- `watch(uri, fn)` — `vfs.watch` плюс подписка на `wotbmod.vfs.invalidated`;
  `fn(event)` на каждую инвалидацию; `unwatch(watch)`.

