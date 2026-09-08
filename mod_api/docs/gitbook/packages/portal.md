# Портал модов BlitzForge

Портал переехал в отдельную самодостаточную папку `_mod_tools/wotbmod-portal`
(бекенд `backend/portal_server.py`, фронтенд `frontend/`, запуск `run.cmd`,
развёртывание `deploy/`). Описание, запуск на ноутбуке и на хостинге, роли,
API и тесты — в `wotbmod-portal/README_RU.md`.

Что важно для SDK:

- `wotbmod publish --to https://<портал>/api/v1 --token …` отправляет релиз
  на портал; `wotbmod install/update --catalog https://<портал>/api/v1`
  читают его `index.json`; `wotbmod report-crash` шлёт записи о крэшах.
- Портал проверяет пакеты теми же модулями, что и CLI (`tools/wotbmod.py`,
  `wotbmod_packages.py`, `wotbmod_trust.py`, `wotbmod_scan.py`); их копии
  для хостинга лежат в `wotbmod-portal/backend/vendor/` и обновляются
  `wotbmod-portal/tools/sync_sdk.py` — после правок этих модулей запускайте
  его.
- Сквозной тест портала `wotbmod-portal/tests/test_portal.py` входит в
  `tests/build_wotbmod_cli_tests.cmd`.
