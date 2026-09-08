# `wotb.session`

Фасад поверх raw-таблиц: [`wotb.session_cluster`](../reference/session_cluster.md), [`wotb.events`](../reference/events.md).

## Методы

- `wotb.session.clusters`
- `wotb.session.cluster`
- `wotb.session.change_cluster`
- `wotb.session.on_cluster_changed`
- `wotb.session.off_cluster_changed`
- `wotb.session.off_all`

## Как пользоваться

### `wotb.session`: кластер входа без перезапуска

Поверх `wotb.session_cluster` (API 1.1, 8 сентября 2026) и `wotb.events`.
Клиент сам умеет переключать кластер внутри региона
(`LoginManager::ChangeCluster`); фасад только называет это по-человечески.
Право `session.cluster.read` — на чтение и событие, `session.cluster.change`
(REVIEWED) — на переключение.

- `clusters()` — `{ {id, name, current, alive, allowed, ccu}, ... }` своего
  региона, отсортировано по `id` (`ccu == -1`, когда клиент не знает онлайн);
- `cluster()` — запись кластера, к которому клиент подключён сейчас;
- `change_cluster(4 | "EU_C4" | "auto")` → `true` или `nil, why`. Только из
  ангара; занято, пока предыдущее переключение в полёте и ещё 10 с после него;
  неизвестный id и мёртвый кластер отклоняются словами клиента. `"auto"` —
  штатный автовыбор клиента (ABI `-1`);
- `on_cluster_changed(fn)` → handle, `fn({ from, to, status })` со `status` из
  `queued | started | connected | failed`; `off_cluster_changed(handle)`,
  `off_all()`. `failed` означает и то, что флаг «ручной выбор» снят: следующий
  вход клиент сделает автовыбором.

Переключение действует на текущий сеанс; после перезапуска клиент снова
выбирает кластер сам.

