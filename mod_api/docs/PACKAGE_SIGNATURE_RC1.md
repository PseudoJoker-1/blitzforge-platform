# Package signatures в WotbMod API V3 RC1

RC1 использует detached signature sidecar `WOTBMOD-SIGNATURE-V1` и локальный
trust store. Поддерживается только `ecdsa-p256-sha256` через Windows CNG.

## Что именно подписывается

Sidecar содержит ровно четыре поля:

```text
WOTBMOD-SIGNATURE-V1
algorithm=ecdsa-p256-sha256
key_id=<trusted key id>
sha256=<64 lower/upper hex digits>
signature=<128 hex digits: raw 32-byte R || 32-byte S>
```

DER ECDSA signature, поле неизвестного имени, duplicate/missing field,
недопустимый key ID и неправильная длина отклоняются. Embedded manifest
signature не подменяет detached verification.

Для directory package SHA-256 считается по отсортированному списку файлов:
`LE32(path_utf8_length) || canonical_relative_posix_path_utf8 ||
LE64(file_size) || exact_file_bytes`. Windows case-fold collision запрещён.

Для `.wotbmod` SHA-256 считается по точным байтам deterministic ZIP-store
archive. Packer сортирует paths, использует relative POSIX names, ZIP store,
нулевой comment/extra и фиксированное время. Loader дополнительно сверяет
local/central metadata, size и CRC32 каждого entry до materialization.

Следовательно подпись связывает:

- точные UTF-8 bytes корневого `manifest.json`;
- `id`, `version`, `permissions`, `dependencies` и entrypoint/content path,
  поскольку они находятся в подписанных manifest bytes;
- точный регистр и bytes пути каждого файла;
- размер и все bytes каждого файла;
- вычисляемый SHA-256 каждого файла: CLI выводит per-file hashes, а изменение
  любого file byte меняет и per-file hash, и подписанный package digest.

Отдельного непроверяемого file-hash списка нет. Directory envelope хеширует
каждый payload напрямую; archive envelope хеширует целиком canonical archive.
Whitespace/key-order изменение manifest тоже инвалидирует подпись, даже если
JSON семантически эквивалентен.

## Trust store и обязательная политика

Public key хранится в `mods/trust/keys/<key_id>.p256` как ровно 64 bytes
`X || Y`, закодированные 128 hex symbols. Reparse-backed store/key запрещён.

- без `PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE` unsigned package может
  быть loadable только с явным `UNSIGNED` warning;
- unknown signer остаётся `DECLARED/UNTRUSTED` только в permissive policy;
- mandatory policy блокирует unsigned и unknown signer до `LoadLibrary`;
- invalid/hash-mismatched signature блокируется даже в permissive policy;
- `PACKAGE_PLAN_READY` означает loadability, а не authenticity.

Mandatory policy включается пользователем без пересборки: ключ
`[policy] require_trusted_signature=1` в `mods/mods.ini` (пишет `wotbmod policy
--require-signature on`). Runtime читает его перед каждым preflight.

Тот же вердикт без клиента выносит `wotbmod verify <artifact> [--trust-root
<dir>]` (`tools/wotbmod_trust.py`: чистый Python, P-256 в координатах Якоби,
проверено на векторе CNG-подписанта и на векторе RFC 6979 A.2.5; вердикты
`valid`/`unsigned`/`untrusted`/`invalid`/`unsupported`/`revoked` совпадают с
`ApplyPackageTrust`). `wotbmod keygen` создаёт пару ключей разработчика, а
`wotbmod release --sign-with-key` подписывает ими детерминированно (RFC 6979);
публичный `<key_id>.p256` должен попасть в trust store клиента, иначе подпись
остаётся `untrusted`.

## Signed revocation list

Опциональный `mods/trust/revocations.list` обязан иметь detached
`revocations.list.sig`, подписанный доверенным ключом тем же envelope. Без
валидной подписи список fail-closed.

```text
WOTBMOD-REVOCATIONS-V1
revoke_key=<key_id>
revoke_release=<mod.id>@<exact-version>
```

Duplicate/unknown/malformed entry запрещён. Ключ не может подписать список,
который отзывает его самого. Exact release revocation блокирует установку и
downgrade к отозванной версии.

## Негативные тесты RC1

`tests/build_v3_package_loader_truth_tests.cmd` проверяет 68 условий, включая:

- byte, payload, manifest, mod ID, permission и dependency tamper;
- internal rename и изменение регистра archive path;
- duplicate/case-colliding entry и повторный manifest;
- `../`, absolute и drive-qualified paths;
- unknown signer, revoked key и signed revoked downgrade;
- unknown sidecar field, malformed DER/raw length и изменённую signature;
- перенос подписи на другой mod ID;
- invalid revocation list fail-closed;
- mandatory-signature rejection до загрузки кода.

RC1 не определяет network PKI, online timestamping или OCSP. Trust store и
revocation list локальны и управляются loader/distribution policy.
