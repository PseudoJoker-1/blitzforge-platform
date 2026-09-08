# Производительность и границы Live Test Center

Цель: панель не должна возвращать прежний FPS drop от frame callback и синхронного I/O.

## Runtime budget

- callback scope измеряется QPC и попадает в profiler snapshot;
- `WOTBMOD_EVENT_FRAME_UPDATE` не подписывается и не записывается;
- high-frequency live events coalesce с окном около 120 ms;
- экранный log — ring-buffer на 128 записей;
- disk queue — bounded 4096 строк; при переполнении удаляется самая старая диагностика;
- отдельный writer worker пакетно пишет human log и JSONL, ждёт максимум 1 s;
- `FlushFileBuffers` выполняется для atomic snapshots, не для каждой строки события;
- snapshots отмечаются dirty и пишутся периодическим maintenance pass;
- native pointer values и произвольная память в trace не сохраняются.

## Скрытая панель

После F5 render token снимается через `unregister_callback`. В скрытом состоянии нет draw calls, `DrawLiveTestCenter`, `PumpFeedback` и UI snapshot maintenance. Событийные callbacks не делают JSON snapshot на каждый вызов; нез выбранные события не попадают в live ring/log.

## Проверка

Внизу панели отображаются `callbacks/s`, `trace/s`, `bytes/s`, `snapshots/min`, `draw/frame`, average/max callback time. Эти значения также сохраняются в `LIVE_VALIDATION_RESULTS.json` и экспортируемом report.

Если queue переполняется или запись невозможна, это фиксируется как WARN; игровой поток не блокируется ожиданием диска. Для проверки после обновления клиента сначала сверяйте exact fingerprint и binding pack, затем повторяйте manual live run.
