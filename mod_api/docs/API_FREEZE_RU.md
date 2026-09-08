# Заморозка BlitzForge API 1.0 (6 сентября 2026)

Этот документ отвечает на один вопрос: на что автор мода может опираться,
не боясь, что следующая сборка SDK или loader-а сломает его мод. Всё, что
перечислено в разделе «Заморожено», меняется только добавлением, а не
заменой; всё из раздела «Не заморожено» может меняться свободно.

«API 1.0» здесь означает RC1-контракт, снимок которого лежит в
`rc1/contract_snapshot.json` и перезаморожен 6 сентября 2026 под клиент
`11.20.0.887` (`wotbmod.gameplay.hud` стал `DEGRADED`, обновлены дайджесты
enum и публичных заголовков). Проверка: `tools/verify_rc1_contract.py`
сравнивает текущие заголовки, схемы и примеры со снимком, а
`tests/build_v3_rc1_contract_tests.cmd` (в составе `build.cmd`) не даёт
собрать SDK при расхождении.

## Заморожено

- **Native ABI.** `WOTBMOD_V3_ABI_VERSION = 0x00030000`; 51 таблица x86 с
  зафиксированными размерами; 46 интерфейсов и их версии; 54 именованных
  разрешения (`tools/wotbmod.py`, `REGISTERED_PERMISSIONS`); коды результатов
  и значения enum (дайджест в снимке). Новый слот появляется только новой
  версией интерфейса (`*_V2`), старый слот не переименовывается и не меняет
  сигнатуру.
- **Raw Lua-имена.** 617 из 619 слотов ABI доступны в Lua под именами
  `wotb.<интерфейс>.<слот>` (`docs/API_REFERENCE_RU.md`, генерируется из
  заголовков; `tests/test_sdk_docs.py` следит, что справочник не отстал).
  Имя таблицы интерфейса и имя слота не меняются; соглашение ответа
  `значение` / `nil, "почему"` не меняется.
- **Фасады.** Имена `wotb.context`, `wotb.players`, `wotb.battle`, `wotb.ges`,
  `wotb.mod`, `wotb.hud`, `wotb.screen`, `wotb.vehicle`, `wotb.shells`,
  `wotb.view`, `wotb.sound`, `wotb.keys`, `wotb.store`, `wotb.files`,
  `wotb.panel` и их публичные функции из `docs/LUA_MODS_RU.md`. Фасад
  никогда не получает имя raw-таблицы; новая функция может добавиться,
  существующая не меняет смысл.
- **Manifest v1.** Набор свойств `manifest_version, type, id, name, version,
  developer, api, client, entrypoints, permissions, resources, locales,
  settings, content, dependencies, optional_dependencies, incompatibilities,
  signature` (`schemas/wotbmod-manifest-v1.schema.json`) и Lua-manifest
  (`schemas/wotbmod-lua-manifest-v1.schema.json`). Новые поля возможны только
  как необязательные.
- **Пакет и подпись.** `.wotbmod` = ZIP store без сжатия с корневым
  `manifest.json`; sidecar `WOTBMOD-SIGNATURE-V1` (ECDSA P-256/SHA-256, ключ
  `mods/trust/keys/<key_id>.p256`, список отзывов
  `WOTBMOD-REVOCATIONS-V1`); хеш пакета = SHA-256 байтов архива.
- **Каталог и релиз.** `index.json` schema 1, `*.release.json` schema 1,
  ledger `mods/cache/wotbmod_installs.json` schema 1, ссылка
  `wotbmod://install/<id>@<версия>?source=<каталог>` и
  `wotbmod://uninstall/<id>`, портальный API `/api/v1/*` и старая форма
  `/api/mods`. Новые поля добавляются, существующие не переименовываются.
- **Политики loader-а.** `mods.ini`: `[mods] <id>=0|1`, `[permissions]
  <id>=0..3`, `[policy] require_trusted_signature=0|1`; карантин
  `mods/cache/auto_disabled_mod.ini`; отпечаток клиента (build + SHA-256
  `wotblitz.exe`) как условие загрузки нативных пакетов.

## Не заморожено

- Внутренности фасадов (`loader/lua/lua_preludes.cpp`), текст ошибок, порядок
  строк в логах.
- HTML и CSS портала, вывод CLI на экран (JSON-вывод `--json` заморожен по
  полям, текстовый нет).
- Статусы `capability`: они отражают факт на конкретном клиенте и меняются
  вместе с ним (`docs/API_STATUS_RU.md`).
- Список примеров и шаблонов.

## Как вносится изменение

1. Новая возможность: новый слот в новой версии интерфейса или новая функция
   фасада; старое остаётся.
2. Исправление поведения без смены сигнатуры: допустимо, фиксируется в
   `API_STATUS_RU.md` с датой live-проверки.
3. Удаление или переименование: только через объявленное устаревание не
   менее чем на один публичный релиз, с работающим старым именем на это
   время, и только с перезаморозкой снимка (`tools/verify_rc1_contract.py
   --emit > rc1/contract_snapshot.json`) в отдельном коммите, где написано,
   что и почему изменилось.
4. Смена клиента (новый build `wotblitz.exe`): переякорение адресов и
   перезаморозка отпечатка; имена и сигнатуры API при этом не меняются.

## Что это даёт автору мода

Мод, который собирается и проходит `wotbmod validate` сегодня, останется
валидным пакетом для всех следующих сборок SDK 1.x; Lua-мод, который
пользуется только именами из `LUA_MODS_RU.md` и `API_REFERENCE_RU.md`, не
потребует правок при обновлении host-а; подпись и хеш релиза останутся
проверяемыми теми же ключами и тем же loader-ом.
