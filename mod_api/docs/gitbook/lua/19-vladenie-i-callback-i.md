# Владение и callback-и

На каждый `lua_State` ведётся ownership registry. Созданные handles,
subscriptions и callback records записываются до передачи скрипту. Успешный
явный `release`/`destroy` удаляет запись; teardown не освобождает её второй раз.

При выгрузке host:

- прекращает новые callback deliveries и ждёт уже начатые;
- снимает subscriptions и generated callback registrations;
- уничтожает controls и другие owned handles;
- откатывает незавершённые storage transactions;
- закрывает Lua state.

Event и generated callbacks синхронны и могут приходить с render/worker thread.
Все входы в один state сериализованы его recursive mutex. Не рассчитывайте, что
callback выполняется в main thread. `events.stop_propagation` имеет смысл только
внутри текущей синхронной доставки.
