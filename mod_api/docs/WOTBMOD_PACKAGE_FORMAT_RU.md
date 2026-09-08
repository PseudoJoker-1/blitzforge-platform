# Формат пакетов и preflight WotbMod API V3

Этот документ описывает реально реализованный слой, который loader должен
вызывать **до `LoadLibrary`**. Он отделяет чтение недоверенного пакета от
загрузки нативного кода и формирует детерминированный план загрузки.

## Поддерживаемые раскладки

### Пакет-каталог

```text
mods/
  author.mod-name/
    manifest.json
    bin/
      windows-x86/
        mod.dll
    assets/
```

Для такого пакета `source_kind` равен
`PACKAGE_SOURCE_DIRECTORY`. SHA-256 пакета вычисляется по всему дереву:
файлы сортируются по относительному UTF-8 пути, затем в digest для каждого
файла попадают длина пути, путь, размер и содержимое. Reparse points,
символические ссылки и нерегулярные файлы запрещены.

### Loose/sidecar manifest

```text
mods/
  example.manifest.json
  example.dll
```

Также распознаётся корневой `manifest.json`. Content-only пакет
использует тот же `*.manifest.json`, но объявляет `"type": "content"`;
сам `content.json` не сканируется как второй manifest. Entrypoint или
content descriptor берётся из manifest и обязан находиться внутри
`mods_root`. Hash loose-пакета покрывает manifest и указанный payload.
Для полного контроля всех assets рекомендуется пакет-каталог или
`.wotbmod`.

### `.wotbmod`

Реализованный `.wotbmod` V1 — обычный ZIP с обязательным корневым
`manifest.json`, но с намеренно узким безопасным профилем:

- один диск, без ZIP64;
- compression method `0` (`store`) для каждого entry;
- без encryption, data descriptor и patch-флагов;
- один корневой `manifest.json`;
- CRC32 каждого entry проверяется до materialization;
- одинаковые с точки зрения Windows пути, пересечения ZIP ranges,
  абсолютные пути, `..`, device names (`CON`, `NUL`, `COM1` и т. п.) и
  запрещённые Windows-символы отклоняются;
- SHA-256 каталожной записи проверяется по исходным байтам `.wotbmod`.

Deflate (`method 8`) и другое сжатие возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED`. Loader не сообщает успешную загрузку
сжатого архива.

При флаге `PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES` содержимое
атомарно раскладывается в:

```text
<archive_staging_root>/<package-sha256>/
```

Кэш имеет marker `.wotbmod-source.sha256`. При повторном запуске marker,
размеры и CRC32 всех materialized-файлов проверяются заново. После этого
`entrypoint_path` или `content_path` в плане указывает на реальный
физический файл.

## Manifest V1

Manifest кодируется UTF-8 JSON и ограничен 4 MiB. Для текущего Windows
клиента нативный пакет обязан объявить `windows-x86`.

```json
{
  "manifest_version": 1,
  "type": "native",
  "id": "author.mod-name",
  "name": "Mod Name",
  "version": "1.2.0",
  "developer": "Author",
  "api": {
    "core": "^1.0",
    "ui": "^1.0"
  },
  "client": {
    "builds": ["11.19.0.834"],
    "executable_hashes": [
      "41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad"
    ]
  },
  "entrypoints": {
    "windows-x86": "bin/windows-x86/mod.dll"
  },
  "dependencies": {
    "author.common": "^2.0"
  },
  "optional_dependencies": {
    "author.devtools": "~1.4"
  },
  "incompatibilities": {
    "legacy.old-mod": "*"
  },
  "permissions": [
    "ui.create",
    "resources.mod",
    "storage"
  ],
  "settings": "settings.schema.json",
  "locales": ["locales/*.json"],
  "resources": ["assets/**"]
}
```

Content-only пакет не содержит DLL:

```json
{
  "manifest_version": 1,
  "type": "content",
  "id": "author.visual-pack",
  "name": "Visual Pack",
  "version": "1.0.0",
  "developer": "Author",
  "content": "content.json",
  "permissions": [
    "resources.mod",
    "resources.overlay.game"
  ]
}
```

`content` по умолчанию равен `content.json`. Content-only manifest с
нативным entrypoint отклоняется. В текущем runtime транзакционно
применяются content-only `TEXTURE`, `UI` и `MODEL` overlays. Семантические
`AUDIO`, `HANGAR` и `LOCALIZATION` overrides возвращают
`WOTBMOD_V3_E_NOT_SUPPORTED`.

## Проверки preflight

`BuildPackageLoadPlan` выполняет следующие проверки:

1. Канонизирует `mods_root`, manifest и payload.
2. Не допускает выход из package root, traversal, symlink/reparse point.
3. Разбирает manifest через `PreflightManifestFile` или
   `PreflightManifestUtf8`.
4. Сверяет точный client build и SHA-256 `wotblitz.exe` с allowlist.
5. Сравнивает запрошенный permission tier с пользовательским
   `max_permission_tier`.
6. Вычисляет SHA-256 пакета и payload.
7. Применяет catalog/blocklist.
8. Разрешает required/optional/incompatible dependencies.
9. Блокирует дубликаты ID и циклы зависимостей.
10. Выдаёт стабильный topological order: dependency всегда раньше
    dependent, независимые пакеты упорядочены по ID/version/source path.

Ошибка одного пакета не ломает диагностику остальных. Сам вызов обычно
возвращает `WOTBMOD_V3_OK`, а конкретный результат находится в
`PackagePlanEntry.result`. Глобальная ошибка возвращается только для
невалидных аргументов, недоступного `mods_root`, неверного списка уже
установленных модов или превышения общего лимита discovery.

Вызов с `out_entries == nullptr` и `entry_capacity == 0` является
count-only: он заполняет `required_capacity`, возвращает
`WOTBMOD_V3_E_BUFFER_TOO_SMALL` при наличии пакетов и не materialize-ит
архивы. Для готового плана loader делает второй вызов с буфером нужного
размера.

`PackagePlanEntry.status == PACKAGE_PLAN_READY` и
`result == WOTBMOD_V3_OK` — единственная комбинация, разрешающая
дальнейший `LoadLibrary` или mount content descriptor.

## Permissions

Manifest parser вычисляет максимальный tier всех permissions:

- `SAFE`;
- `GAMEPLAY_TWEAK`;
- `REVIEWED`;
- `UNSAFE`.

Preflight сохраняет в плане как вычисленный tier, так и точный bounded
список permission names из manifest. Runtime package loader устанавливает
этот список как allowlist публичного WotbMod API до вызова V3 entrypoint.
Операция разрешается только при одновременном выполнении двух условий:

1. требуемое permission name входит в allowlist либо покрыто объявленным
   родительским namespace;
2. фактически выданный tier не ниже tier операции.

Рекомендуется объявлять канонические минимальные имена, а не широкий
namespace. Для чтения и загрузки собственных package assets используется
`resources.mod`. Подмена `game://` требует дополнительно
`resources.overlay.game`; эта операция имеет tier `REVIEWED`. Поэтому
content-only `TEXTURE`, `UI` и `MODEL` overlay обычно объявляет оба имени.

Preflight не выдаёт tier самостоятельно. Он проверяет, что запрошенный tier
не выше уже принятого пользователем `max_permission_tier`. Это значение
является default; индивидуальные решения передаются массивом
`PackagePermissionGrant` по ID мода и имеют приоритет над default. Сравнение
ID grant с package ID регистронезависимое, а дубликаты, отличающиеся только
регистром, считаются конфликтом. Запрошенный и реально выданный tier
сохраняются в `requested_permission_tier` и `granted_permission_tier`
плана. Unsafe sideload технически возможен, если пользователь явно дал
этому моду tier `UNSAFE`; в плане остаётся
`PACKAGE_WARNING_UNSAFE_PERMISSION_TIER`.

Loose V3 DLL без manifest/package plan сохраняет compatibility-режим с
проверкой только tier. Loose DLL с sidecar manifest проходит обычный
package preflight и получает named allowlist.

Named permissions ограничивают только операции WotbMod API. Это не
процессная песочница: Windows выполняет `DllMain` внутри `LoadLibrary` до
V3 entrypoint, а direct Win32-вызовы нативной DLL не проходят через runtime
permission checks. Нельзя загружать недоверенный native package, полагаясь
только на manifest allowlist.

## Каталог

В `PackagePreflightOptions.catalog_records` передаются записи
`WotbModV3CatalogRecord`.

- `VERIFIED`/`COMMUNITY`: ID, version и SHA-256 должны совпасть.
- `UNREVIEWED`: разрешается только с
  `PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED`, всегда с warning.
- отсутствующая запись трактуется как manual sideload; флаг
  `PACKAGE_PREFLIGHT_REQUIRE_CATALOG_RECORD` делает её обязательной.
- `BANNED`: совпадение ID или SHA-256 всегда блокирует пакет.

Каталожный SHA-256 даёт реальную проверку целостности. Встроенная
криптографическая подпись `.wotbmod` в этой версии **не проверяется**:
план выставляет
`PACKAGE_WARNING_EMBEDDED_SIGNATURE_UNVERIFIED`. Поле signer в каталоге
не подменяет криптографическую проверку. До подключения trust store и
signature backend loader не должен показывать «signature valid».

## Лимиты по умолчанию

| Лимит | Значение |
|---|---:|
| Кандидаты | 256 |
| Entries архива/каталога | 4096 |
| Глубина | 16 |
| Размер `.wotbmod` | 512 MiB |
| Суммарный unpacked size | 512 MiB |
| Один файл | 128 MiB |
| Пакет-каталог | 512 MiB |
| Manifest | 4 MiB |

Loader может уменьшить лимиты через `PackagePreflightLimits`, но не может
передать нулевые или структурно неверные значения.

## Developer CLI

Один и тот же stdlib-only CLI доступен тремя способами: как команда `wotbmod`
после установщика (собранный `wotbmod.exe` в папке игры, Python не нужен), как
`tools\wotbmod.cmd` в чекауте SDK и как `python tools\wotbmod.py` где угодно:

```text
wotbmod <команда>
tools\wotbmod.cmd <команда>
python tools\wotbmod.py <команда>
```

Команды разработчика (проект → пакет):

```text
wotbmod new        создать проект (native, content или lua из шаблона)
wotbmod build      собрать проект явной командой без shell-подстановок
wotbmod run        положить пакет в mods/ и запустить точный клиент
wotbmod pack       собрать детерминированный .wotbmod
wotbmod validate   строго проверить manifest, каталог или .wotbmod
wotbmod inspect    показать метаданные и хеши файлов
wotbmod doctor     проверить SDK, компилятор, runtime и отпечаток клиента
```

Команды платформы (пакет → релиз → каталог → установка), добавлены в CLI 1.2
(`tools/wotbmod_packages.py`, подпись в `tools/wotbmod_trust.py`):

```text
wotbmod release    упаковать, подписать и записать release-запись версии
wotbmod publish    положить релиз в каталог (папка или портал)
wotbmod install    проверить, показать хеш/права/зависимости, установить
wotbmod update     обновить один пакет или все из каталога
wotbmod rollback   вернуть предыдущую версию из резервной копии
wotbmod uninstall  удалить пакет (копия остаётся)
wotbmod list       что установлено: включён ли, tier, подпись, карантин
wotbmod verify     вынести вердикт о подписи так же, как loader
wotbmod keygen     создать пару ключей P-256 разработчика
wotbmod policy     показать/задать политику подписей в mods.ini
wotbmod scan       статическая проверка пакета: импорты DLL, секции, Lua-вызовы, права
wotbmod quarantine показать/снять карантин loader-а после crash loop
wotbmod info       всё об одном моде: ledger, mods.ini, подпись, карантин, ссылка, лог
wotbmod report-crash отправить запись о крэше на портал (сводка совместимости)
```

Подробно они описаны в разделе «Установка, зависимости и релизы» ниже.

### Создание проекта

Native V3:

```bat
tools\wotbmod.cmd new C:\mods\author-example ^
  --id author.example ^
  --name "Example Mod" ^
  --developer "Author"
```

Команда создаёт `manifest.json`, `src/mod.cpp`, `assets/` и ожидаемый
`bin/windows-x86/<name>.dll`. Source содержит рабочий V3 entrypoint,
enable/disable callbacks и логирование через `wotbmod.core`. Manifest
сразу ограничен текущим клиентом `11.19.0.834` и его точным SHA-256.
Существующий каталог не перезаписывается.

Content-only:

```bat
tools\wotbmod.cmd new C:\mods\author-content ^
  --id author.content ^
  --name "Content Pack" ^
  --developer "Author" ^
  --type content
```

Кроме manifest создаётся валидный `content.json` без DLL.

### Безопасный build

CLI не принимает shell-строку и не интерполирует её. После `--` передаётся
явный argv, а процесс запускается с `shell=False` и рабочим каталогом
проекта:

```bat
tools\wotbmod.cmd build C:\mods\author-example -- cmd /c build.cmd
```

Если разработчик явно указал `cmd /c`, shell является частью его argv;
сам CLI не склеивает аргументы, не добавляет кавычки и не выполняет
метасимволы. Exit code дочерней сборки проверяется, и неуспешная сборка
не объявляется успешной.

### Безопасный run

`run` принимает готовый package-каталог или уже собранный `.wotbmod`.
Для native-проекта сначала отдельно выполняется `build`; `run` не угадывает
build system и не выполняет скрытую shell-команду.

```bat
tools\wotbmod.cmd run C:\mods\author-example ^
  --game-root "D:\Games\World of Tanks Blitz"
```

Package-каталог детерминированно упаковывается во временный ZIP-store
архив. Готовый `.wotbmod` копируется во временный staging-файл. В обоих
случаях staging повторно проходит полный строгий validator, после чего
устанавливается ровно как:

```text
<game-root>/mods/<manifest-id>.wotbmod
```

Замена выполняется через атомарный `os.replace` на том же томе. Если
предыдущий пакет существовал, его байты сохраняются как временный
last-good. При ошибке подготовки запуска прежний пакет восстанавливается;
если пакета раньше не было, неуспешно установленный файл удаляется.
Невалидный source, несовпадение fingerprint клиента, reparse path или
staging внутри source tree не перезаписывают установленный пакет.

Перед любыми изменениями `run` проверяет именно целевой
`wotblitz.exe`: build `11.19.0.834`, architecture `x86` и точный SHA-256.
Переопределить fingerprint флагом у `run` нельзя.

Для CI или ручного запуска клиента позже:

```bat
tools\wotbmod.cmd run C:\mods\author-example ^
  --game-root "D:\Games\World of Tanks Blitz" ^
  --no-launch
```

Аргументы клиента передаются после `--` как literal argv. CLI вызывает
ровно целевой `wotblitz.exe` через `subprocess.Popen(..., shell=False)`:

```bat
tools\wotbmod.cmd run C:\mods\author-example ^
  --game-root "D:\Games\World of Tanks Blitz" ^
  -- --some-client-flag "literal & value"
```

`run` не является native DLL hot reload. Он только атомарно устанавливает
пакет для следующей загрузки runtime и, если не указан `--no-launch`,
запускает точный проверенный клиент.

### Validate и inspect

```bat
tools\wotbmod.cmd validate C:\mods\author-example
tools\wotbmod.cmd validate author.example-1.0.0.wotbmod --json
tools\wotbmod.cmd inspect author.example-1.0.0.wotbmod
tools\wotbmod.cmd inspect author.example-1.0.0.wotbmod --json
```

Обе команды принимают package-каталог, loose manifest JSON или
`.wotbmod`. Проверяются:

- UTF-8 JSON без BOM, дубликатов ключей и превышения 4 MiB;
- Manifest V1, ID, semver, API ranges, dependencies и permissions;
- `windows-x86` DLL либо content descriptor;
- client build/hash allowlist;
- существование payload внутри package root;
- отсутствие traversal, абсолютных путей, symlink/reparse point и
  нерегулярных файлов;
- лимиты числа файлов и размера;
- ZIP-store-only профиль `.wotbmod`, CRC32, Windows-safe имена и
  case-insensitive дубликаты.

Для анализа пакета под другой клиент можно явно передать
`--client-build` и `--client-sha256`. `--no-client-check` отключает только
сопоставление allowlist; структура, пути, payload и архивные ограничения
продолжают проверяться.

`inspect` дополнительно печатает package/payload SHA-256 и SHA-256 каждого
файла. JSON-режим предназначен для CI.

### Детерминированный pack

```bat
tools\wotbmod.cmd pack C:\mods\author-example ^
  --output C:\packages\author.example-1.0.0.wotbmod
```

`pack` сначала полностью валидирует исходный package-каталог, затем:

1. перечисляет только regular files без symlink/reparse points;
2. сортирует их по UTF-8 relative path;
3. пишет ZIP method `store`, без ZIP64, encryption и data descriptor;
4. фиксирует timestamp каждого entry как `1980-01-01 00:00:00`;
5. повторно открывает временный архив тем же строгим validator;
6. атомарно заменяет output только после успешной проверки;
7. печатает SHA-256 архива, payload и каждого entry.

Output обязан находиться вне source tree, поэтому предыдущий архив не
может случайно войти в следующий. Одинаковые байты проекта дают
байт-в-байт одинаковый `.wotbmod`, независимо от файловых timestamps.

### Doctor

```bat
tools\wotbmod.cmd doctor
tools\wotbmod.cmd doctor --json
tools\wotbmod.cmd doctor --game-root "D:\Games\World of Tanks Blitz"
```

`doctor` проверяет SDK headers/runtime sources, наличие `cl` либо
`vcvars32.bat`, пути собранного runtime/loader и установленного proxy.
Для `wotblitz.exe` он независимо читает PE architecture, Windows file
version и SHA-256. Успех требует точного fingerprint:

```text
build: 11.19.0.834
architecture: x86
sha256: 41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad
```

Отсутствующие build artifacts показываются отдельно и не подменяются
фиктивным статусом. Mismatch клиента завершает `doctor` с ненулевым
exit code.

## Установка, зависимости и релизы

Этот раздел для того, кто ни разу не публиковал мод. Словарь: **пакет** —
файл `.wotbmod` (ZIP без сжатия с `manifest.json` в корне); **релиз** —
пакет плюс подпись плюс `*.release.json` с его хешем и метаданными;
**каталог** — папка или сайт, где лежат релизы и `index.json` со списком
версий; **ledger** — журнал того, что CLI установил
(`mods/cache/wotbmod_installs.json`).

### Путь релиза

```bat
rem 1. Один раз: ключ разработчика. Приватный файл храните как пароль.
tools\wotbmod.cmd keygen --out C:\keys\me.key --key-id me-2026

rem 2. Релиз: детерминированный .wotbmod, подпись и release-запись.
tools\wotbmod.cmd release C:\mods\author-example -o C:\mods\releases ^
  --sign-with-key C:\keys\me.key --key-id me-2026

rem 3. Публикация в каталог-папку (или на портал: --to https://.../api/v1).
tools\wotbmod.cmd publish C:\mods\releases\author.example-1.0.0.release.json ^
  --to C:\catalog
```

`release` кладёт в папку три файла: `<id>-<version>.wotbmod`,
`<id>-<version>.wotbmod.sig` (detached-подпись формата
`WOTBMOD-SIGNATURE-V1`, см. `PACKAGE_SIGNATURE_RC1.md`) и
`<id>-<version>.release.json`. В записи — `artifact.sha256`, размер, права
и их tier, `dependencies`/`optional_dependencies`/`incompatibilities` из
manifest, `client.builds`, публичный ключ подписи и `suggested` —
рекомендации уровня каталога (`--suggest id=range`), которых в manifest v1
нет: manifest заморожен RC1-контрактом, поэтому «suggested» живёт в записи
релиза, «required» — в `dependencies`, «recommended» — в
`optional_dependencies`.

**Версия неизменяема.** `release` откажется писать `1.0.0` с другим хешем,
если запись `1.0.0` уже есть; `publish` откажется класть в каталог версию с
другим хешем; `install` откажется заменить установленную `1.0.0` другими
байтами той же версии (кроме `--force`, который для отладки). Изменили код —
поднимите версию.

Подпись через Windows CNG (`--sign`, ключ `blitzforge-preview-2026` из
`tools/sign_release_artifact.ps1`) остаётся для превью-сборок; `--sign-with-key`
делает то же самое на чистом Python (RFC 6979, детерминированно), чтобы
разработчик и портал подписывали без PowerShell. Публичный ключ кладут в
`<game>/mods/trust/keys/<key_id>.p256` — только тогда loader и CLI считают
подпись доверенной.

### Каталог-папка

```text
catalog/
  index.json
  releases/<id>/<version>/<id>-<version>.wotbmod
  releases/<id>/<version>/<id>-<version>.wotbmod.sig
```

`index.json` — `{"schema": 1, "packages": {"<id>": {"name", "developer",
"latest", "versions": {"<version>": <release-запись + artifact.path +
signature.path>}}}}`. Тот же файл по адресу `https://host/что-угодно/index.json`
делает каталогом любой статический сервер; относительные пути
`artifact.path` разрешаются от адреса индекса. Портал (этап 4) отдаёт такой
же индекс по `/api/v1/index.json` и принимает `publish --to https://host/api/v1`
(`POST releases`, multipart: `record`, `artifact`, `signature`,
`Authorization: Bearer <token>` или переменная `WOTBMOD_PORTAL_TOKEN`).

### Установка

```bat
tools\wotbmod.cmd install C:\catalog\releases\author.example\1.0.0\author.example-1.0.0.wotbmod ^
  --game-root "C:\Games\World of Tanks Blitz" --catalog C:\catalog
```

Перед тем как что-то изменить, `install`:

1. проверяет отпечаток клиента (архитектура, build, SHA-256 `wotblitz.exe`)
   и allowlist клиента в manifest — пакет для другого build не ставится;
2. валидирует пакет как `validate` (CRC каждого entry, безопасные пути,
   лимиты) — испорченный архив отклоняется до всего остального;
3. выносит вердикт о подписи ровно как loader: `valid`, `unsigned`,
   `untrusted` (ключа нет в `mods/trust/keys`), `invalid` (хеш не сходится),
   `revoked` (в подписанном `mods/trust/revocations.list`). `invalid`,
   `revoked` и неподдержанный алгоритм — всегда отказ; `unsigned`/`untrusted`
   — отказ при `--require-signature` или при `[policy]
   require_trusted_signature=1` в `mods.ini`;
4. разрешает зависимости: `dependencies` берутся из установленного (по
   `mods/*.wotbmod`, каталогам с `manifest.json` и loose-manifest) или из
   `--catalog`; диапазоны — те же, что у loader (`*`, `^1.2.3`, `~1.2.3`,
   `>=1.0.0 <2.0.0`; prerelease ниже релиза). Нет требуемой зависимости —
   отказ с подсказкой; есть в каталоге — она встаёт в план **раньше**
   зависимого. `incompatibilities` проверяются в обе стороны: и у нового
   пакета против установленных, и у установленных против нового;
5. печатает сводку по каждому шагу плана: id/версия/автор, **SHA-256**,
   размер, вердикт подписи, права по tier, requires/recommends/conflicts, и
   какой tier будет выдан в `mods.ini`; ждёт `y` (или `--yes`).

Только потом: копия в `mods/<id>.wotbmod.stage`, сверка хеша, резервная копия
старого файла и его `.sig` в `mods/cache/install_backups/packages/<id>/`,
атомарная замена `os.replace`, `.sig` рядом (или удаление устаревшего),
`[mods] <id>=1` и `[permissions] <id>=<tier>` в `mods.ini` (остальные строки
файла не трогаются) и запись в ledger. Сбой после замены — возвращается
резервная копия.

`update <id>|--all [--catalog ...] [--check] [--pre]` находит в каталоге
самую свежую версию под текущий build (prerelease только с `--pre`) и
устанавливает её тем же путём; каталог по умолчанию — тот, что записан в
ledger при установке. `rollback <id>` возвращает последнюю резервную копию
(байты и `.sig` сверяются по хешу, allowlist клиента проверяется заново);
повторный `rollback` возвращает обратно — это стек. `uninstall <id>`
откажется, если пакет нужен другому установленному (`--force` снимает),
удаляет файл и `.sig`, чистит ключи `mods.ini` и запись карантина
`auto_disabled_mod.ini`, копию оставляет — после `uninstall` тоже работает
`rollback`. `list [--json]` показывает включён ли пакет, requested/granted
tier, вердикт подписи, карантин, число резервных копий, политику и «застрявший»
session marker (safe mode при следующем старте).

### Аудит состояния (этап 6)

`wotbmod list` — что установлено, включено ли (`[mods]` в `mods.ini`),
requested/granted tier, вердикт подписи, карантин, число резервных копий.
`wotbmod info <id>` собирает всё об одном моде: версию и путь, права и
зависимости, запись ledger (откуда, когда, из какого каталога), резервные
копии, карантин и число крэшей подряд, ссылку на страницу мода на портале и
последние строки `wotb_mod_loader.log` с этим id — то есть точную причину,
если loader его заблокировал. `wotbmod quarantine` объясняет карантин
(`mods/cache/auto_disabled_mod.ini`, `crash_history.ini`) и `--clear <id>`
снимает его; `wotbmod rollback` возвращает прошлую версию без ручного
копирования DLL. Всё это покрыто регрессионными тестами
(`tests/test_wotbmod_packages.py`, класс `AuditTests`).

### Статическая проверка (этап 7)

`wotbmod scan <пакет>` (`tools/wotbmod_scan.py`) ничего не запускает: для
нативных пакетов разбирает PE-файлы вручную — импорты (инъекция в чужие
процессы и системные хуки блокируют; HTTP, запуск программ, реестр —
предупреждение, если manifest не объясняет их правом `network:https://…`
или `native.*`), секции write+execute, упакованные секции (энтропия),
встроенные URL и адреса; для Lua — `os.execute`, `io.popen`,
`package.loadlib`, `debug.*`, FFI (блокируют), `load`/`dofile`, файловый
доступ мимо `wotb.files`, `wotb.<модуль>` без соответствующего права,
обфусцированные блобы (предупреждают); скрипты и установщики в любом
пакете — блокирующая находка. Три уровня: `block`, `warn`, `info`; итог
`risk` low/medium/high. `wotbmod release` отказывается выпускать пакет с
`block` (кроме `--allow-scan-findings`), портал отказывает такому пакету
при загрузке и хранит отчёт в release-записи (`scan`), `wotbmod install`
показывает находки и отказывает при `block` (кроме `--force`), а при
обновлении пишет `PERMISSION ESCALATION`, если новая версия просит права,
которых у установленной не было.

### Политика подписей в mods.ini

```ini
[policy]
require_trusted_signature=1
```

Loader читает этот ключ в `WotbModRuntime_LoadAll` и при перезагрузке одного
мода и включает `PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE`: unsigned и
untrusted пакеты блокируются до `LoadLibrary`, а не только получают warning.
`wotbmod policy --require-signature on|off` пишет ключ и показывает список
доверенных ключей.

## Интеграция с runtime

```cpp
using namespace wotbmod::v3;

PackagePreflightOptions options = {};
options.struct_size = sizeof(options);
options.api_version = WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
options.flags =
    PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED |
    PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES;
options.max_permission_tier = user_granted_tier;
// Заполнить mods_root, archive_staging_root, client_build,
// client_executable_sha256, permission_grants, installed_mods
// и catalog_records.
options.limits = DefaultPackagePreflightLimits();

std::vector<PackagePlanEntry> entries(
    WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES);
PackagePlanSummary summary = {};
summary.struct_size = sizeof(summary);
summary.api_version = WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;

WotbModV3Result result = BuildPackageLoadPlan(
    &options,
    entries.data(),
    static_cast<uint32_t>(entries.size()),
    &summary);

if (result == WOTBMOD_V3_OK) {
    for (uint32_t i = 0; i < summary.discovered_count; ++i) {
        const PackagePlanEntry& item = entries[i];
        if (item.status != PACKAGE_PLAN_READY ||
            item.result != WOTBMOD_V3_OK) {
            LogBlocked(item.id, item.reason);
            continue;
        }
        if (item.package_type == WOTBMOD_V3_PACKAGE_NATIVE) {
            // Runtime связывает item.permissions[0..permission_count) с mod handle.
            LoadNativeAfterPreflight(item.entrypoint_path);
        } else {
            MountContentAfterPreflight(item.content_path);
        }
    }
}
```

До `BuildPackageLoadPlan` loader вычисляет hash клиента через
`Sha256FileUtf8`. После preflight нельзя заменять путь на исходное имя
DLL: загружаться должен именно канонический `entrypoint_path` из готового
плана. Точно так же нельзя заменять permission allowlist плана только одним
`granted_permission_tier`: package-bound runtime использует обе границы.

## Пакеты ресурсов (`"type": "resource"`)

Что это. Часть модов меняет не поведение клиента, а его файлы: шейдер
цветокоррекции, YAML с настройками, текстуру, которую runtime-оверлей
(`TEXTURE`/`UI`/`MODEL` из content-пакета) не умеет подменить. Такие файлы
лежат в `Data/` в контейнерах `.dvpl` (payload + 20-байтовый footer с CRC и
типом сжатия, обычно LZ4). Пакет ресурсов — обычный подписанный `.wotbmod`,
который `wotbmod install` раскладывает не в `mods/`, а прямо в `Data/`, а
loader его не читает вовсе.

Manifest:

```json
{
  "manifest_version": 1,
  "type": "resource",
  "id": "blitzforge.night_mode",
  "name": "Ночной режим",
  "version": "1.3.0",
  "developer": "pseud",
  "description": "Ночная цветокоррекция боевой сцены.",
  "permissions": ["resources.overlay.game"],
  "client": {"builds": ["11.20.0.887"]},
  "files": [
    {
      "target": "Materials/Shaders/debug-modify-color.slh",
      "source": "files/Materials/Shaders/debug-modify-color.slh",
      "stock_sha256": "3737d3ff…",
      "sha256": "631b811b…"
    }
  ]
}
```

- `target` — путь внутри `Data/` без суффикса `.dvpl`, только буквы, цифры,
  `_ . -` и `/`; `..`, обратные слэши и абсолютные пути отклоняются;
- `source` — файл внутри пакета с новым (распакованным) содержимым;
- `stock_sha256` — SHA-256 распакованного стокового файла той сборки
  клиента, для которой сделан пакет; `sha256` — SHA-256 файла `source`;
- `permissions` обязан содержать `resources.overlay.game` (tier `REVIEWED`,
  на портале такой релиз получает метку «нужна проверка»);
- до 64 файлов, каждый `target` один раз.

Что делает `wotbmod install`:

1. распаковывает каждый `Data/<target>.dvpl` и сверяет хеш: либо стоковый
   файл (`stock_sha256`), либо версия этого же пакета (обновление/откат).
   Любой другой файл — отказ без записи: значит, игра обновилась или файл
   менял кто-то ещё;
2. если два пакета ресурсов претендуют на один `target`, второй не ставится,
   пока не удалён первый;
3. стоковый `.dvpl` сохраняется в `mods/cache/game_files/<target>.dvpl`
   (один раз), новый файл упаковывается в DVPL (LZ4) и записывается
   атомарно; кэш скомпилированных шейдеров клиента очищается, если менялись
   `Materials/Shaders/*`;
4. архив и подпись пакета остаются в `mods/cache/resource_packages/`, в
   ledger пишется `kind: resource` и список `targets` с хешами; `mods.ini`
   не трогается.

`wotbmod uninstall` возвращает сохранённые оригиналы (если файл после
установки изменила игра, нужен `--force`), `wotbmod rollback` ставит
предыдущую версию из резервной копии, `wotbmod list/info` показывают
`kind resource` и подпись из кэшированного архива. После обновления клиента
стоковые хеши меняются — пакет надо переиздать под новую сборку.

Пример: `examples/night_mode`. DVPL читается и пишется чистым Python
(`tools/wotbmod_dvpl.py`); модуль `lz4`, если установлен, только ускоряет.

### Новые файлы и импорт чужих модов

`stock_sha256: null` означает файл, которого в стоковой игре нет: `install`
требует, чтобы по этому пути ничего не лежало (или лежала наша же версия),
оригинал не сохраняет, `uninstall` файл удаляет. Команда `wotbmod import
<папка или zip> --id author.mod --name ... -o <проект>` собирает такой пакет из
обычного мода-подмены файлов (структура `Data/`, `.dvpl` или сырые файлы):
снимает стоковые хеши с локального клиента, пропускает файлы, совпадающие со
стоковыми, и пишет проект для `wotbmod release`. Стоковые хеши берутся с вашей
игры, поэтому импортируйте на проверенном через Steam клиенте нужной сборки.
