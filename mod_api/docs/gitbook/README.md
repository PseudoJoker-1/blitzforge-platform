# BlitzForge: моды для World of Tanks Blitz

BlitzForge — это загрузчик модов, Lua API поверх клиента и портал [https://blitz-forge.org](https://blitz-forge.org), где моды публикуются, подписываются и ставятся одной кнопкой.

- **Игроку**: [установка](getting-started/install.md) и [каталог](https://blitz-forge.org).
- **Автору мода**: [быстрый старт](getting-started/quickstart.md), [руководство по Lua](lua/README.md), [фасады `wotb.*`](facades/README.md) и [справочник всех функций](reference/README.md).
- **Чужие моды**: [импорт существующих модов](packages/import.md) в формат пакета ресурсов — со снимком стоковых файлов и честным удалением.
- **Формат и правила**: [пакеты](packages/package-format.md), [портал](packages/portal.md), [политика проверки](packages/review-policy.md), [безопасность](packages/security-policy.md).
- **Что подтверждено живым клиентом**: [статус API](status/api-status.md), [заморозка API 1.0](status/api-freeze.md), [известные ограничения](status/limitations.md).
- **Разработчику на бете**: [первый мод за 15 минут](getting-started/first-mod.md), [правила модерации](status/moderation.md), [что делать после патча клиента](status/client-patch.md).

Документация собирается из репозитория [PseudoJoker-1/wotb-mod-api](https://github.com/PseudoJoker-1/wotb-mod-api) командой `python tools/generate_gitbook.py`; страницы справочника и фасадов генерируются из тех же заголовков, из которых собираются Lua-биндинги, поэтому они не отстают от кода.
