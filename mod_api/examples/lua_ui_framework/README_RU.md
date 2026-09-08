# Lua Runtime UI Lab

Тестовый Lua-мод полностью создаёт интерфейс во время работы. Авторский YAML
ему не нужен: `wotb.ui.create({...})` создаёт native DAVA controls, после чего
объектные методы меняют их свойства.

Пример демонстрирует:

- `UIStaticText`, `UIButton`, `UITextField`, image и scroll/list controls;
- runtime text через `label:set_text("Громкость: 75%")`;
- шрифты, цвет, выравнивание, прозрачность, текстуры и фон;
- click, pointer-down и drag callbacks;
- два слайдера и toggle с обновляемым style override;
- Lua-анимацию позиции и opacity;
- динамический список неизвестной заранее длины;
- независимый `template:clone()` для каждой строки.
- `wotb.context` и battle-only lifecycle без UI на экране каталога.

Text/style setters внутри drag callback объединяются до следующего кадра:
быстрое движение слайдера не пересоздаёт и не освобождает DAVA control посреди
обработки pointer event.

## Установка

Скопируйте папку как:

```text
<game>\mods\lua\example.lua_ui_framework\
├── manifest.json
├── main.lua
└── README_RU.md
```

Имя папки обязано совпадать с `id` в `manifest.json`. Также должен быть
установлен пакет `wotbmod.lua_host.wotbmod`, собранный из той же ревизии API.

## Ожидаемый результат

1. В ангаре и каталоге мод не создаёт UI. После входа в бой справа появляется
   `OPEN LUA UI LAB`.
2. Кнопка открывает окно, созданное Lua без шаблона.
3. Слайдеры двигаются мышью, и подписи сразу меняются.
4. Toggle меняет текст и фон, action меняет runtime-текст и стиль.
5. Строки списка созданы клонированием и реагируют независимо.
6. В лог пишется `Lua Runtime UI Lab mounted without an author YAML template`.
7. При смене active screen мод безопасно перемонтируется, а при отключении
   освобождает callbacks, style overrides и controls.
8. В `MOD_SCREEN`/`TEXT_INPUT` и после выхода из боя всё дерево controls
   полностью размонтируется. Окно также не заходит в левую системную зону
   шириной 128 px.
