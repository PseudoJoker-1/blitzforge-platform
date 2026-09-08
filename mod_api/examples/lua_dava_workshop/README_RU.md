# Lua DAVA Workshop

Обычный интерактивный мод, а не PASS/FAIL self-test. Он показывает состояние
exact DAVA backend, загружает YAML через штатный DAVA parser, читает файл из
ResourceArchive, создаёт и меняет `NMaterial`, а в бою создаёт штатный tracer
из позиции камеры по `F10`.

- `F9` открывает/закрывает окно и освобождает мышь.
- `F10` создаёт tracer вперёд от активной камеры (только бой/тренировка/replay).
- Кнопка `МАТЕРИАЛ` меняет runtime property реального `NMaterial`.
- Кнопка `MESH SWAP` использует две `.sc2`, если разработчик укажет
  `MESH_BASE_URI` и `MESH_REPLACEMENT_URI` в начале `main.lua`.

`workshop.zip` создаётся упаковщиком distribution из
`fixtures/archive_payload.txt`, чтобы бинарный архив не хранился в Git.
Все DAVA-объекты являются typed userdata и освобождаются при выключении мода;
сырые адреса DAVA в Lua не публикуются.
