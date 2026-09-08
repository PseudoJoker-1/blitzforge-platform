# Live Test Event Map

`PASS` означает callback/payload соответствует контракту и затем может быть подтверждён `F7`. `WARN` — событие неполное, timeout или native capability ещё не подтверждена. `FAIL` — неверный result, client mismatch или ручной `F8`. `NS` — честно недоступно; активный вызов запрещён.

| API name | Русское название | Категория | Действие | Payload/критерий PASS | WARN / FAIL | Timeout | Звук | HF |
|---|---|---|---|---|---|---:|---|---|
| `client.fingerprint` | Проверка fingerprint клиента | Client | открыть панель | version/SHA совпали | mismatch / API error | 1 s | да | нет |
| `client.device_info` | Информация об устройстве | Client | F6 | device info returned | unavailable / error | 1 s | да | нет |
| `client.leave_to_hangar` | Возврат в ангар | LeaveToHangar | F6 после подтверждения | state HANGAR | отказ / mismatch | 5 s | да | нет |
| `lifecycle.info` | Состояние runtime | Lifecycle | F6 | lifecycle info | unavailable / error | 1 s | да | нет |
| `lifecycle.cleanup` | Очистка подписок и handle | Reload / Unload | выгрузить мод | tokens/handles released | stale handle / failure | 5 s | нет | нет |
| `lifecycle.stress_30` | 30 циклов загрузки | Lifecycle | перезапустить 30 раз | cycle counter ≥30 | crash marker | 30 циклов | нет | нет |
| `camera.active` | Активная камера | Camera | F6/войти в бой | valid handle + mode | no camera | 1 s | да | нет |
| `camera.mode_change` | Изменён режим камеры | Camera | ARCADE ↔ SNIPER | mode/previous mode event | duplicate/no event | 5 s | да | нет |
| `events.loader_queue` | Доставка через очередь | Diagnostics | F6 self event | callback on loader queue | wrong thread | 1 s | да | нет |
| `events.callback_thread` | Поток callback | Diagnostics | любое событие | MAIN/RENDER role | wrong thread | 1 s | нет | да |
| `entity.enumerate_visible` | Публичные entity | BigWorld Entity | F6/ангар | visible snapshot | empty/unavailable | 2 s | да | нет |
| `entity.lifecycle` | Жизненный цикл entity | BigWorld Entity | бой | added/updated/removed + handle | unresolved handle | 3 s | нет | да |
| `rpc.incoming_metadata` | Входящие RPC metadata | RPC Metadata | бой | method key, size=0 payload | unsupported/error | 3 s | да | нет |
| `shell.ingress` | Игрок произвёл выстрел | Shell | выстрел | shot event + id | no shot id | 5 s | да | нет |
| `projectile.created` | Создан projectile | Projectile | выстрел | projectile handle | unresolved | 3 s | да | нет |
| `projectile.updated` | Projectile IN_FLIGHT | Projectile | летящий снаряд | state update | coalesced/timeout | 3 s | нет | да |
| `projectile.destroyed` | Projectile уничтожен | Projectile | дождаться конца | destroy handle | chain timeout | 15 s | да | нет |
| `impact.visual` | Визуальный impact | Projectile | попадание | native visual event | visual unavailable | 3 s | да | нет |
| `impact.confirmed` | Зарегистрировано попадание | Projectile | попадание | hit/impact id | unresolved | 3 s | да | нет |
| `tracer.requested` | Запрошен tracer | Tracer | выстрел | NS on current pack | no native source | 3 s | нет | нет |
| `tracer.created` | Создан tracer | Tracer | выстрел | tracer handle | unresolved | 3 s | да | нет |
| `tracer.visible` | Tracer отображается | Tracer | выстрел | visible event | no visual | 3 s | да | нет |
| `tracer.destroyed` | Tracer уничтожен | Tracer | конец трассера | destroy event | unresolved | 5 s | да | нет |
| `tracer.style` | Стиль tracer | Tracer | штатный style | NS until native source | never mutate unknown pointer | — | нет | нет |
| `render.backend` | Render backend | Render / D3D11 / DXGI | открыть F5 | backend event | unknown backend | 1 s | да | нет |
| `render.native_borrowed` | D3D11 device/context | Render / D3D11 / DXGI | F6 | borrowed availability flags | unavailable | 1 s | да | нет |
| `render.resize` | Resize swapchain | Render / D3D11 / DXGI | resize/Alt+Tab | viewport dimensions | missed event | 5 s | да | нет |
| `render.device_lifecycle` | Device lifecycle | Render / D3D11 / DXGI | graphics/Alt+Tab | create/lost/restored | recreation incomplete | 10 s | да | нет |
| `render.callback` | Render callback | Render / D3D11 / DXGI | видимая F5 панель | draw accepted | draw error | 1 s | нет | нет |
| `ui.validation_panel` | Панель Test Center | UI | F5 | text draw accepted | backend unavailable | 1 s | да | нет |
| `ui.readonly_inspector` | UI-экран | UI | F6/смена экрана | read-only tree | unavailable | 2 s | да | нет |
| `scene.readonly_inspector` | Scene inspector | Scene | U + F6 | NS/доступ | forbidden/error | 2 s | нет | нет |
| `material.readonly_inspector` | Material inspector | Material | U + F6 | NS/доступ | forbidden/error | 2 s | нет | нет |
| `audio.custom_file` | Custom audio | Audio | F6 | create/preload/play/stop/destroy | API/audio error | 2 s | да | нет |
| `audio.native_sound_event` | Native sound event | Audio | штатное событие | metadata only | unsupported | 3 s | да | нет |
| `resources.portable_text` | Portable resource | Resources | F6 | owner VFS load/release | I/O/parse | 2 s | да | нет |
| `resources.async_reload` | Async resource | Async | F6 | pending→ready/failed | cancelled/timeout | 10 s | да | нет |
| `loaders.portable_yaml` | Portable YAML | Resources | F6 | root parsed | parse error | 2 s | да | нет |
| `loaders.dvpl` | DVPL loader | Resources | F6/availability | backend info | unavailable | 2 s | нет | нет |
| `archive.zip` | Archive open | Resources | F6 | bounded entry count | limit/I/O | 2 s | да | нет |
| `loaders.native_dava_yaml` | Native DAVA YAML | Resources | U + F6 | NS unless binding | permission/error | 2 s | нет | нет |
| `loaders.native_dava_archive` | Native DAVA archive | Resources | U + F6 | NS unless binding | permission/error | 2 s | нет | нет |
| `reload.hangar` | Reload request | Reload / Unload | F6 в ангаре | deferred request | conflict/deny | 10 s | да | нет |
| `reload.active_subscription` | Subscription barrier | Reload / Unload | F6 | callback removed safely | stale token | 5 s | да | нет |
| `reload.async_resource` | Async cancellation | Reload / Unload | F6 | cancel/release | timeout | 10 s | да | нет |
| `reload.callback_deferred` | Deferred main queue | Async | F6 | task queued/executed | wrong thread | 5 s | да | нет |
| `reload.rollback` | Resource rollback | Reload / Unload | reload scenario | last-good restored | conflict | 10 s | да | нет |
| `reload.cleanup` | Mod unload cleanup | Reload / Unload | unload/reload | no subscriptions/handles | stale cleanup | 5 s | да | нет |
| `errors.crash_recovery` | Crash marker | Diagnostics | F6 | marker clear/disable | stale marker | 2 s | нет | нет |
| `errors.last_error` | Последняя ошибка API | Diagnostics | вызвать ошибку | sanitized error | unavailable | 1 s | нет | нет |

Shot correlation uses only exact valid handles. If IDs cannot be joined, the log says `Correlation: UNRESOLVED`; no heuristic association is made.
