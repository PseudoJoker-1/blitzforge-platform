# sample.ui_transaction

RC1-пример инспекции и транзакционного изменения существующего игрового
`UIControl`. Мод читает имя, тип, вычисленный путь по родителям и геометрию
активного control, сдвигает его на один пиксель, подключает managed-контейнер с
vertical layout, подписывается на click, а при disable/unload возвращает всё
назад и освобождает handles.

Текст честно выводится как `NOT_EXPOSED_RC1`: в замороженном UI V3 snapshot нет
text getter. Этот пример проверяет только транзакционное изменение game-owned
дерева; runtime native Text/Image/Button показаны отдельно в
`example.lua_ui_framework`.

## Ожидаемый результат

- В логе есть `name=... type=... path=... text=NOT_EXPOSED_RC1 geometry=...`.
- После enable в `WotbSampleUi_GetState()` выставлены биты 0-3.
- Click managed-контейнера выставляет бит 6.
- Disable/unload выставляет биты rollback/cleanup 4-5.

## Проверка очистки

После отключения позиция штатного экрана совпадает с исходной, managed child
отсутствует и UI callback больше не вызывается. Host-тест проверяет прямые и
обратные операции.

## Ручной чек-лист

1. Включить пакет в ангаре и проверить loader log.
2. Убедиться, что штатные Text/Image/Button не подменялись.
3. Отключить мод и проверить возврат экрана в исходную позицию.
4. Сменить экран, снова включить мод и проверить отсутствие stale handles.

Сборка и host-тест: `tests\build_rc1_samples_tests.cmd`.
