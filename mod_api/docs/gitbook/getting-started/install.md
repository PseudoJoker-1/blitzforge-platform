# Установка для игрока

1. Закройте игру и скачайте установщик `BlitzForge-Setup-<версия>.exe` со страницы [https://blitz-forge.org/download](https://blitz-forge.org/download).
2. Запустите его: обычный мастер Windows найдёт игру через Steam, проверит сборку клиента, докачает недостающие зависимости (Visual C++ Runtime x86; Python только для разработчиков) и поставит загрузчик модов, Lua-хост, команду `wotbmod` и кнопку «Установить в игру».
3. Откройте [каталог модов](https://blitz-forge.org) и нажмите «Установить в игру» у любого мода. Установщик покажет права и хеш и спросит подтверждение.

Из терминала (после установки откройте новое окно):

```bat
wotbmod list
wotbmod install blitzforge.night_mode --catalog https://blitz-forge.org/api/v1
wotbmod uninstall blitzforge.night_mode
wotbmod rollback blitzforge.night_mode
```

Удаление: «Приложения» Windows → BlitzForge → Удалить. Исходные файлы игры возвращаются, запись в PATH и регистрация `wotbmod://` снимаются.

Набор подходит только той сборке клиента, для которой собран; после обновления игры скачайте новую версию.
