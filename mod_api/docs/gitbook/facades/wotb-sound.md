# `wotb.sound`

Фасад поверх raw-таблиц: [`wotb.audio`](../reference/audio.md).

## Методы

- `wotb.sound.play`
- `wotb.sound.stop`
- `wotb.sound.release`
- `wotb.sound.set_volume`
- `wotb.sound.is_playing`
- `wotb.sound.on_finished`
- `wotb.sound.replace`
- `wotb.sound.reset`
- `wotb.sound.reset_all`

## Как пользоваться

### `wotb.sound`: проиграть файл, подменить звук

Поверх `wotb.audio`.

- `play(uri, { loop, spatial, volume, pitch, bus, priority, on_finished })` →
  handle (create + play); `stop(handle [, fade_seconds])`, `release(handle)`,
  (`fade_seconds` только 0: Windows-бэкенд 11.20 не умеет плавных затуханий — ненулевой fade у `stop` и ненулевая длительность у `fade_to` честно отвечают `NOT_SUPPORTED`, live 8 сентября 2026),
  `set_volume(handle, 0..1)`, `is_playing(handle)`, `on_finished(handle, fn)`;
- `replace(event_name, uri [, priority])` → token; `reset(event_name | token)`,
  `reset_all()`. Что клиент не умеет подменять, он говорит сам.

