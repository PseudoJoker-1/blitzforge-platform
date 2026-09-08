# sample.camera_render

RC1-пример portable render/camera. Он регистрирует managed `AFTER_UI` callback,
рисует одну строку telemetry, читает backend/viewport/frame/delta time,
показывает camera mode, включая `UNKNOWN`, ставит низкоприоритетный
`AFTER_GAME` modifier, исправляющий только некорректный FOV, и наблюдает managed
render lifecycle. `wotbmod.render.native` и COM pointers не используются.

## Ожидаемый результат

- Одна overlay-строка показывает backend, viewport, frame, delta time и mode.
- Resize/backend/device lifecycle увеличивает счётчик, а тот же managed callback
  продолжает рисовать после восстановления.
- Некорректный FOV исправляется на 60 градусов; валидный результат игры не
  изменяется.

## Проверка очистки

Disable/unload обязан снять lifecycle subscription, render callback, camera
modifier и освободить active camera handle. Host-тест имитирует resize/backend
change и проверяет все cleanup-вызовы.

## Ручной чек-лист

1. Включить в ангаре и зайти в training/battle; наблюдать `UNKNOWN`, `ARCADE`
   или `SNIPER`, не требуя неподдерживаемый режим.
2. Изменить размер окна и переключить fullscreen/windowed.
3. При возможности вызвать безопасный device recreation и убедиться, что строка
   одна, без дублированных callbacks.
4. Отключить мод и проверить немедленное исчезновение overlay.

Сборка и host-тест: `tests\build_rc1_samples_tests.cmd`.
