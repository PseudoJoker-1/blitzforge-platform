# Lua-моды BlitzForge (WotbMod V3 ABI)

Lua host встраивает Lua 5.4.7 в обычный 32-битный native-мод и даёт скриптам
доступ к замороженному WotbMod V3 ABI без компилятора и без FFI.

Новичку: начните с [QUICKSTART_RU.md](../getting-started/quickstart.md) (мод из шаблона за
15 минут), держите под рукой [API_REFERENCE_RU.md](../reference/README.md)
(сгенерированный справочник по каждому слоту с правами) и подключите
`sdk/vscode/wotbmod.code-snippets` в VS Code; схемы для `manifest.json` и
для `wotb.config` лежат в `schemas/`.

Текущий структурный инвентарь — 622 слотов в 47 интерфейсах (сверено
8 сентября 2026 по `python tools/generate_lua_bindings.py --report`):

- 51 слот `storage`, `events`, ядра `ui` и `ges` имеет ручные
  ergonomic-биндинги;
- 569 слотов создаётся генератором из V3-заголовков;
- 2 слота намеренно не представлены в Lua, потому что требуют сырого
  нативного указателя: `unsafe_native.create_address_hook` и
  `intermod.export_interface`.

Итого Lua-автору доступны 620 из 622 слотов. Публичный C ABI при этом не
изменён (`WOTBMOD_V3_ABI_VERSION` = `0x00030000`): новые интерфейсы публикуются
рядом с замороженными таблицами, а не расширяют их.

Кроме функций генератор публикует 611 констант из V3-заголовков в 42 таблицах
`wotb.*` (см. «Константы из заголовков»), а host устанавливает convenience-модули
на чистом Lua: `wotb.context`, `wotb.players`, `wotb.log`, `wotb.json`,
`wotb.timer`, `wotb.battle`, `wotb.config`, `wotb.available`, `wotb.panel`,
`wotb.mod`, `wotb.hud`, `wotb.screen`, `wotb.vehicle`, `wotb.shells`,
`wotb.view`, `wotb.sound`, `wotb.keys`, `wotb.store`, `wotb.files` и расширения
`wotb.ges` (см. «Фасады»). Ни константы, ни эти модули не расширяют C ABI и не
добавляют ни одного permission.

Отдельно host может добавить loader-private таблицу `wotb.dava`. Это не часть
frozen public C ABI и не FFI: скрипт получает только typed userdata handles,
которые loader проверяет по owner, kind и lifetime.

## Разделы

- [Установка host-а и готового Lua-мода](01-ustanovka-host-a-i-gotovogo-lua-moda.md)
- [Manifest установленного Lua-мода](02-manifest-ustanovlennogo-lua-moda.md)
- [Режим разработки и hot reload](03-rezhim-razrabotki-i-hot-reload.md)
- [Жизненный цикл](04-zhiznennyy-cikl.md)
- [Sandbox](05-sandbox.md)
- [Таблицы API](06-tablicy-api.md)
- [Константы из заголовков](07-konstanty-iz-zagolovkov.md)
- [Опубликован, разрешён, поддержан](08-opublikovan-razreshen-podderzhan.md)
- [Типизированные игровые события](09-tipizirovannye-igrovye-sobytiya.md)
- [Контекст и видимость UI](10-kontekst-i-vidimost-ui.md)
- [Игроки и команды](11-igroki-i-komandy.md)
- [Convenience-модули](12-convenience-moduli.md)
- [Фасады: короткий API поверх raw-таблиц](13-fasady-korotkiy-api-poverh-raw-tablic.md)
- [Runtime UI из Lua](14-runtime-ui-iz-lua.md)
- [Loader-private `wotb.dava`](15-loader-private-wotb-dava.md)
- [Generated DAVA loaders](16-generated-dava-loaders.md)
- [Аргументы, результаты и ошибки](17-argumenty-rezultaty-i-oshibki.md)
- [Разрешения и двухуровневый забор](18-razresheniya-i-dvuhurovnevyy-zabor.md)
- [Владение и callback-и](19-vladenie-i-callback-i.md)
- [Instruction budget](20-instruction-budget.md)
- [Ограничения](21-ogranicheniya.md)
