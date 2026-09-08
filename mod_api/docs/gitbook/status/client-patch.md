# Что делать, когда обновился клиент игры

Загрузчик привязан к точной сборке клиента (сейчас `11.20.0.887`, SHA-256
`4813544d…`, binding pack `112000887`). После патча Blitz он это честно
обнаружит: native-биндинги отключаются, моды с фиксированными адресами не
грузятся, портативные части (пакеты, Lua-хост без native-фасадов) остаются.
Игроки увидят в логе `client fingerprint not verified`. Ниже — порядок
действий, чтобы пауза заняла часы, а не дни.

## День патча (30 минут)

1. Объявление на сайте: `Модерация → Объявление на сайте` — текст вида
   «Клиент 11.21 вышел, набор для него в работе; моды временно отключены».
   Оно показывается на каждой странице и уходит в `index.json` (`notice`),
   его печатает `wotbmod sync`.
2. Снять с `/download` набор старой сборки не нужно: мастер сам откажет
   ставиться на другой клиент (`Installed client executable does not match
   this release`).
3. Сохранить новый `wotblitz.exe` в `_mod_tools/analysis/` с SHA-256 в имени —
   это референс для IDA и для рецептов.

## Переанкоровка (обычно 1–2 дня)

Инструмент: `_mod_tools/reanchor` (рецепты в `recipes.json`, 115 якорей на
11.20). Порядок:

```
python reanchor/cli.py resolve "<путь>\wotblitz.exe" -o new_pack.json
```

- Резолвер честно делит якоря на найденные и `REJECTED`. На мажорном
  патче (11.19 → 11.20) сигнатуры выжили только на 46 %, и два «найденных»
  были ложными — сигнатура лишь отправная точка. Проверять каждый найденный
  адрес по IDA: пролог, RTTI-vtable, строки, порядок вызовов у известных
  вызывающих. Методы и скрипты второго прохода — `analysis/pivot_tools_*`.
- Ненайденные якоря искать так, как описано в
  `analysis/…/re_anchors.md` (пивоты: RTTI, конструкторы с записью vtable,
  строковые литералы, соседи по списку `call`).
- Смещения структур (41 шт., `kNameOffset`) перепроверять отдельно: они
  меняются реже адресов, но ломаются тише — это чтение чужой памяти.

Записать новые значения в `loader/anchor_rvas.h`, обновить fingerprint
(`kExpectedClientSha256`, build, binding pack — см. `v3_native_bindings.cpp`),
перегенерировать рецепты:

```
python reanchor/cli.py generate "<путь>\wotblitz.exe"
python test_reanchor.py
```

Рецепт `GesListenerListAdd` пишется вручную и после `generate` возвращается
из предыдущего `recipes.json`.

## Проверка вживую (полдня)

1. `loader\build_live.cmd`, DLL в игру, `mods\mods.ini`: `wotbmod.lua_host=0`
   на время прогонов.
2. Ангар: четыре auto-sweep validation-мода (`ONLY=all SKIP=reload`),
   отдельно `ONLY=reload`; бой: тренировочная комната с Shift и одним
   выстрелом перед sweep. Скрипты и координаты — `docs/LIVE_EVIDENCE_RU.md` и
   `API_STATUS_RU.md`, раздел «Финальный прогон 8 сентября 2026».
3. Матрица `mods/data/native_validation_mod/LIVE_CAPABILITY_MATRIX.json` после
   каждого прогона — в `docs/evidence/`; строки без результата — в таблицу
   статуса с причиной.

## Выпуск (час)

1. `tools/build_public_preview.ps1 -SkipBuild -Version 0.1.0-preview.N`
   (fingerprint нового клиента берётся из SDK), проверка мастера тихой
   установкой на этой машине (`docs/…/setup-wizard`), загрузка на
   `/opt/wotbmod-portal/data/downloads/`, старый набор удалить.
2. `client_build` портала (`config.json`) и `index.json` — новая сборка;
   `tools/sync_sdk.py` и деплой `portal_server.py`/vendor.
3. Пакеты каталога: у ресурсных пакетов проверить `stock_sha256` (после
   патча штатные файлы другие — пакет честно откажет), авторам таких
   пакетов написать; Lua-пакеты с `client.builds` — попросить заявить
   совместимость.
4. Снять объявление.

## Что может пойти не так

- Патч сменил компоновку `UIControl`/`Scene` — смещения в
  `wotb_mod_dava_resources.cpp` (`kUi*Offset`) и `v3_native_camera_layout.h`.
  Симптом: validation-строки `ui.live_text`, `scene.readonly_inspector`
  падают при верных адресах.
- Изменился protobuf/GES тип событий — `ges: N event types indexed` в логе
  с другим N; схемы событий в `ges_schemas`.
- Клиент стал 64-битным — это не переанкоровка, а новый порт (MinHook,
  Lua-хост, все смещения); отдельный план.
