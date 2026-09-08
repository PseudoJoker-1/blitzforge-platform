# `wotb.manifest`

Raw-таблица интерфейса `WotbModV3ManifestApiV1` (`include/wotbmod/manifest_v1.h`, версия `WOTBMOD_V3_MANIFEST_VERSION`). Функции ниже вызываются как `wotb.manifest.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `content`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.manifest.PACKAGE_NATIVE` | `WOTBMOD_V3_PACKAGE_NATIVE` | enum WotbModV3PackageType |
| `wotb.manifest.PACKAGE_CONTENT_ONLY` | `WOTBMOD_V3_PACKAGE_CONTENT_ONLY` | enum WotbModV3PackageType |
| `wotb.manifest.DEPENDENCY_REQUIRED` | `WOTBMOD_V3_DEPENDENCY_REQUIRED` | enum WotbModV3DependencyKind |
| `wotb.manifest.DEPENDENCY_OPTIONAL` | `WOTBMOD_V3_DEPENDENCY_OPTIONAL` | enum WotbModV3DependencyKind |
| `wotb.manifest.DEPENDENCY_INCOMPATIBLE` | `WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE` | enum WotbModV3DependencyKind |
| `wotb.manifest.SIGNATURE_UNSIGNED` | `WOTBMOD_V3_SIGNATURE_UNSIGNED` | enum WotbModV3SignatureStatus |
| `wotb.manifest.SIGNATURE_DECLARED` | `WOTBMOD_V3_SIGNATURE_DECLARED` | enum WotbModV3SignatureStatus |
| `wotb.manifest.SIGNATURE_VALID` | `WOTBMOD_V3_SIGNATURE_VALID` | enum WotbModV3SignatureStatus |
| `wotb.manifest.SIGNATURE_INVALID` | `WOTBMOD_V3_SIGNATURE_INVALID` | enum WotbModV3SignatureStatus |
| `wotb.manifest.SIGNATURE_UNSUPPORTED` | `WOTBMOD_V3_SIGNATURE_UNSUPPORTED` | enum WotbModV3SignatureStatus |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `parse_json` | json_utf8: table:ConstBuffer | manifest: handle |
| `parse_uri` | uri: string | manifest: handle |
| `validate` | manifest: handle | true |
| `get_info` | manifest: handle | info: table:ManifestInfo |
| `get_api_requirement` | manifest: handle, index: integer | requirement: table:ManifestApiRequirement |
| `get_dependency` | manifest: handle, index: integer | dependency: table:ManifestDependency |
| `get_permission` | manifest: handle, index: integer | string |
| `get_resource_pattern` | manifest: handle, index: integer | string |
| `get_locale_pattern` | manifest: handle, index: integer | string |
| `get_entrypoint` | manifest: handle, index: integer | entrypoint: table:ManifestEntrypoint |
| `get_client_build` | manifest: handle, index: integer | string |
| `get_client_executable_hash` | manifest: handle, index: integer | string |
| `resolve_dependencies` | manifest: handle, installed: array | report: table:DependencyReport |
| `verify_content_sha256` | physical_file: string, expected_sha256: string | true |
| `verify_signature` | manifest: handle, physical_package: string | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["content"]
local parse_json, err = wotb.manifest.parse_json(json_utf8)  -- manifest: handle
if parse_json == nil then wotb.log.warn("manifest.parse_json: %s", err) end
local parse_uri, err = wotb.manifest.parse_uri("...")  -- manifest: handle
if parse_uri == nil then wotb.log.warn("manifest.parse_uri: %s", err) end
local validate_ok, err = wotb.manifest.validate(handle)
if not validate_ok then wotb.log.warn("manifest.validate: %s", err) end
```

