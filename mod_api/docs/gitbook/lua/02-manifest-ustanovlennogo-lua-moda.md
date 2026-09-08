# Manifest установленного Lua-мода

Минимальный manifest:

```json
{
  "id": "example.my_mod",
  "entrypoint": "main.lua",
  "permissions": [
    "core",
    "storage"
  ]
}
```

Правила production scanner-а:

- `id` обязателен: ASCII-буквы, цифры, `.`, `_`, `-`; начало и конец — буква
  или цифра; `..` запрещён;
- `entrypoint` по умолчанию равен `main.lua`; разрешён только один ASCII-файл
  `.lua` в корне мода, без каталогов и traversal;
- `permissions` — массив строк; дублирование полей `id`, `entrypoint` или
  `permissions` делает manifest недействительным;
- JSON читается по точному размеру: embedded NUL и trailing data не скрывают
  вторую часть документа;
- неизвестное plain-text permission не даёт прав; escape-обфускация permission
  и malformed JSON отклоняют manifest целиком;
- дополнительные metadata-поля (`name`, `version` и другие) допустимы и
  игнорируются host-ом.

Два мода с одним `id` или одним путём не запускаются. Проверка дубля выполняется
до компиляции и выполнения top-level Lua-кода.
