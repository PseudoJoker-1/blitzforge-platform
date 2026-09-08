# Публикуемые тестовые моды API 2.9

Исходники артефактов находятся в соседних каталогах, готовые ZIP создаются в
`build/publish`. Native DLL объявляется целью `files/mods/<name>.dll`, поэтому
backend видит реальное изменение, а `modpack.py apply` устанавливает файл в
`<game>/mods`.

| Backend id | DLL | Назначение |
| --- | --- | --- |
| `api-23-selftest` | `api_selftest_mod.dll` | Стабильный backend id; безопасно вызывает весь 76-функционный `WotbModHostApi`; настоящий game hook не ставит. |
| `custom-audio-api-test` | `custom_audio_test_mod.dll` | Генерирует WAV и проверяет полный custom-audio lifecycle ABI 2.x. |
| `api-contract-fixture` | `api_contract_mod.dll` | Экспортирует host handle/counters для `api_full_host.exe`. |
| `api-ownership-peer` | `api_peer_mod.dll` | Второй owner для проверки изоляции handles. |
| `resource-overlay-test` | `resource_mod.dll` | Проверяет traversal rejection и mount priority; fixtures создаёт сам. |
| `fault-isolation-test` | `fault_mod.dll` | Намеренный SEH fault; только для test environment. |
| `native-client-api-test` | `native_live_test_mod.dll` | Проверяет ABI 2.9 в живом Blitz: DAVA sound, UI/Scene, existing UI, vehicle API, все gameplay events и регистрацию skin mappings. |
| `new-gameplay-events-test` | `new_gameplay_events_test_mod.dll` | Отдельно валидирует семь focused native events, включая camera mode, payload sizes, sequence и borrowed vehicle lifecycle. |
| `vehicle-skin-api-test` | `vehicle_skin_test_mod.dll` | Проверяет четыре skin calls и наглядно заменяет T-34-85 mesh/texture через owner-scoped mount. |
| `object260-t3485-model` | `object260_t3485_model_mod.dll` | Визуально перенаправляет штатные `.sc2/.scg` Object 260 на штатную модель T-34-85 без упаковки ресурсов игры. |

Локальный обязательный результат:

```text
API FULL OK: assertions=1650 all public functions passed
API SELFTEST: passes=92 skips=10 failures=0
AUDIO SELFTEST: passes=24 failures=0 finished=1
PUBLISHED SELFTEST OK
NEW EVENTS MOD: passes=14 failures=0 seen=0x003E1008 expected=0x003E1008 finished=1
VEHICLE SKIN MOD: passes=15 failures=0 finished=1
NEW API MODS OK
```

Текущие локальные ABI 2.9 артефакты после пересборки:

| Package | Version | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `api-23-selftest` | `2.9.0` | 11711 | `83c9b6dc8b301f8adb2e51cbfd5cee31266e968f000743f87cc757119850211a` |
| `api-contract-fixture` | `2.9.0` | 5246 | `5562f1ab0002b8c4dfb6d4d3cd63978a3cfaa3eadda008f3c9eb24d4902966da` |
| `native-client-api-test` | `1.6.0` | 19644 | `2c9fb0049a7461428227467b8f3c08ecdf4f203836bf370efd0d29dd0d0bde7d` |
| `new-gameplay-events-test` | `1.0.0` | 6705 | `2d39a5908a97f8ced42bcc7699e8b364fdb33dc703b22d067b3a3fd5159eb5dc` |
| `vehicle-skin-api-test` | `1.0.0` | 1134417 | `0db5645c0d54f2270ab6b9f611a4d8db6a08ec4a6f14d97333c4cb1669c34ff8` |
| `object260-t3485-model` | `1.0.0` | 5691 | `51923a63831f90f1fad212119571cf2d5e71b98acacd97339009e00b368c8b41` |

Десять standalone `SKIP` — DAVA/gameplay операции, для которых standalone host
намеренно не предоставляет клиентские backends. Custom audio использует
Media Foundation/XAudio2.

Предыдущие ABI 2.7 пакеты были загружены на backend и скачаны обратно с
совпадающими размером/SHA-256. `new-gameplay-events-test` и
`vehicle-skin-api-test` загружены и одобрены 29.07.2026; остальные ABI 2.9
пакеты в таблице в этом прогоне не перепубликовывались. `fault-isolation-test`
нельзя одобрять для общего каталога: он специально падает на первом frame.

`native-client-api-test` привязан к anchors текущей версии клиента. Bundled
live loader создаёт настоящие refcounted `UIPackage`, `UIControl`, `Entity`
и `Scene`; проверены load/reload/release, direct create, active screen,
geometry/visibility/transform/add/remove, ранний `NOT_FOUND`, последующий
active Scene lookup и custom `.sc2` attach на 180 кадров с detach/release;
UI hook hit=191, Scene load hook hit=38. Client events: UI=7,
Scene activated=2, Scene deactivated=1; borrowed handles и clone проверены.
Это ABI 2.7 live baseline. Текущий ABI 2.9 test mod подписан на все 22 маски,
проверяет typed payload, borrowed vehicle lifecycle и skin register/info/
enable/disable/release; новый loader собран, но свежий бой ещё не запускался.

`new-gameplay-events-test` пишет отдельный итог после получения семи
focused-событий, включая camera mode. `vehicle-skin-api-test` оставляет активной test skin:
T-34-85 получает mesh через mount и контрастную cyan/magenta texture. Это локальная
клиентская подмена; для отката достаточно отключить мод и пересоздать модель.
