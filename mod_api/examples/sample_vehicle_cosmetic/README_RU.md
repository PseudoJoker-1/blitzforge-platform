# sample.vehicle_cosmetic

RC1-пример транзакционного cosmetic pack конкретного танка по точным путям.
Мод загружает JSON descriptor, регистрирует mesh/material/texture entries с LOD,
применяет их к выбранному локальному `R110_Object_260`, читает состояние и
`requires_model_reload`, реагирует на смену локального танка и перед release
откатывает все overlays.

Воспроизводимый staging-скрипт копирует валидные DVPL mesh/geometry и текстуры
T-34-85 из установленного клиента прямо в локальный sample-пакет. Эти игровые
файлы не хранятся в исходниках SDK. Отдельный material YAML показывает только
замороженную exact-path транзакцию: live DAVA NMaterial mutation остаётся
experimental и не заявляется как RC1-функция.

## Ожидаемый результат

- Загрузка descriptor и регистрация pack успешны.
- `WotbSampleVehicle_GetState()` показывает registration/application/state.
- После mount mesh/texture `WotbSampleVehicle_RequiresReload()` равен `1`.
- При смене выбранного танка предыдущий mount откатывается и выполняется новая
  попытка применения по policy.
- Кэшированная модель ангара может потребовать выйти и снова открыть preview.

## Проверка очистки

Disable/unload обязан снять event subscription, откатить все exact-path mounts,
освободить pack, vehicle handle и descriptor. Ни один replacement не должен
остаться смонтированным. Обратную последовательность проверяет host-тест.

## Ручной чек-лист

1. Использовать пакет из `tools\build_rc1_sample_packages.ps1` только с тем
   client installation, из которого он собран.
2. Выбрать Объект 260, включить мод; при кэше закрыть и снова открыть preview.
3. Переключиться на другой танк и обратно; проверить state/reapply counters.
4. Отключить мод, перезагрузить preview и проверить возврат штатной модели.

Сборка и host-тест: `tests\build_rc1_samples_tests.cmd`.
