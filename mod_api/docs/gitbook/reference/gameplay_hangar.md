# `wotb.gameplay_hangar`

Raw-таблица интерфейса `WotbModV3GameplayHangarApiV1` (`include/wotbmod/gameplay_hangar_v1.h`, версия `WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION`). Функции ниже вызываются как `wotb.gameplay_hangar.<слот>(...)`; при отказе любая отвечает `nil, err`.

## Права

Забор host-а проверяет перед вызовом: `gameplay.tweak.hangar`, `hangar.scene`. Имена объявляются в `permissions` манифеста.

## Константы

| Имя в Lua | В заголовке | Тип |
| --- | --- | --- |
| `wotb.gameplay_hangar.LIGHTING_GAME_DEFAULT` | `WOTBMOD_V3_HANGAR_LIGHTING_GAME_DEFAULT` | enum WotbModV3HangarLighting |
| `wotb.gameplay_hangar.LIGHTING_DAY` | `WOTBMOD_V3_HANGAR_LIGHTING_DAY` | enum WotbModV3HangarLighting |
| `wotb.gameplay_hangar.LIGHTING_NIGHT` | `WOTBMOD_V3_HANGAR_LIGHTING_NIGHT` | enum WotbModV3HangarLighting |
| `wotb.gameplay_hangar.LIGHTING_STUDIO` | `WOTBMOD_V3_HANGAR_LIGHTING_STUDIO` | enum WotbModV3HangarLighting |
| `wotb.gameplay_hangar.LIGHTING_CINEMATIC` | `WOTBMOD_V3_HANGAR_LIGHTING_CINEMATIC` | enum WotbModV3HangarLighting |
| `wotb.gameplay_hangar.UI_BANNERS` | `WOTBMOD_V3_HANGAR_UI_BANNERS` | enum WotbModV3HangarUiElement |
| `wotb.gameplay_hangar.UI_NEWS` | `WOTBMOD_V3_HANGAR_UI_NEWS` | enum WotbModV3HangarUiElement |
| `wotb.gameplay_hangar.UI_OFFERS` | `WOTBMOD_V3_HANGAR_UI_OFFERS` | enum WotbModV3HangarUiElement |
| `wotb.gameplay_hangar.UI_CHAT` | `WOTBMOD_V3_HANGAR_UI_CHAT` | enum WotbModV3HangarUiElement |

## Функции

| Функция | Аргументы | Результат |
| --- | --- | --- |
| `set_background` | texture_uri: string | true |
| `set_background_video` | video_uri: string | true |
| `set_music` | audio_uri: string | true |
| `set_lighting` | preset: integer | true |
| `set_vehicle_preview_angle` | yaw: number, pitch: number | true |
| `set_vehicle_preview_zoom` | zoom: number | true |
| `set_floor_texture` | texture_uri: string | true |
| `set_skybox` | texture_uri: string | true |
| `hide_ui_elements` | mask: integer | true |
| `set_camera_orbit_speed` | multiplier: number | true |
| `reset` | — | true |

## Пример вызова

Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — то, что вернул создающий вызов этой же таблицы):

```lua
-- manifest.json: "permissions": ["gameplay.tweak.hangar", "hangar.scene"]
local set_background_ok, err = wotb.gameplay_hangar.set_background("...")
if not set_background_ok then wotb.log.warn("gameplay_hangar.set_background: %s", err) end
local set_background_video_ok, err = wotb.gameplay_hangar.set_background_video("...")
if not set_background_video_ok then wotb.log.warn("gameplay_hangar.set_background_video: %s", err) end
local set_music_ok, err = wotb.gameplay_hangar.set_music("...")
if not set_music_ok then wotb.log.warn("gameplay_hangar.set_music: %s", err) end
```

