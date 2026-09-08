# `wotb.http`

Raw-таблица интерфейса `WotbModV3HttpApiV1` (`include/wotbmod/http_v1.h`, версия `WOTBMOD_V3_HTTP_VERSION`). Функции ниже вызываются как `wotb.http.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `network.http`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.http.DEFAULT_MAX_RESPONSE` | `WOTBMOD_V3_HTTP_DEFAULT_MAX_RESPONSE` | define |
| `wotb.http.ABSOLUTE_MAX_RESPONSE` | `WOTBMOD_V3_HTTP_ABSOLUTE_MAX_RESPONSE` | define |
| `wotb.http.CREATED` | `WOTBMOD_V3_HTTP_CREATED` | enum WotbModV3HttpState |
| `wotb.http.CONFIGURED` | `WOTBMOD_V3_HTTP_CONFIGURED` | enum WotbModV3HttpState |
| `wotb.http.SENDING` | `WOTBMOD_V3_HTTP_SENDING` | enum WotbModV3HttpState |
| `wotb.http.COMPLETED` | `WOTBMOD_V3_HTTP_COMPLETED` | enum WotbModV3HttpState |
| `wotb.http.FAILED` | `WOTBMOD_V3_HTTP_FAILED` | enum WotbModV3HttpState |
| `wotb.http.CANCELLED` | `WOTBMOD_V3_HTTP_CANCELLED` | enum WotbModV3HttpState |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `request_create` | — | request: handle |
| `request_set_method` | request: handle, method: string | true |
| `request_set_url` | request: handle, url: string | true |
| `request_set_header` | request: handle, name: string, value: string | true |
| `request_set_body` | request: handle, data: void, size: integer | true |
| `request_set_timeout` | request: handle, timeout_ms: integer | true |
| `request_set_max_response_size` | request: handle, max_response_size: integer | true |
| `request_send_async` | request: handle, completion: function | true |
| `request_cancel` | request: handle | true |
| `request_get_info` | request: handle | info: table:HttpRequestInfo |
| `response_get_status` | request: handle | status: integer |
| `response_get_header` | request: handle, name: string | string |
| `response_get_body` | request: handle | body: table:Buffer |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["network.http"]
local request_create, err = wotb.http.request_create()  -- request: handle
if request_create == nil then wotb.log.warn("http.request_create: %s", err) end
local request_set_method_ok, err = wotb.http.request_set_method(handle, "...")
if not request_set_method_ok then wotb.log.warn("http.request_set_method: %s", err) end
local request_set_url_ok, err = wotb.http.request_set_url(handle, "...")
if not request_set_url_ok then wotb.log.warn("http.request_set_url: %s", err) end
```

