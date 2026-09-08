"""Generate docs/API_REFERENCE_RU.md from the V3 headers.

One table per interface: every slot with the signature a Lua author calls
(hidden `mod` dropped, out-parameters turned into results, buffers into
strings), the permission family the generated binding guards it with, and
the constants the interface publishes. A hand-maintained section lists the
facade modules and the raw tables each stands on, so the two layers are
read side by side.

    python tools/generate_api_reference.py          # writes the file
    python tools/generate_api_reference.py --check  # exits 1 when stale

The model comes from tools/lua_api_model.py, the same source the Lua
bindings are generated from, so a header change that adds or renames a slot
fails --check until the reference is regenerated.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from generate_lua_bindings import NATIVE_ONLY, PERMISSIONS, constant_tables  # noqa: E402
from lua_api_model import INTERFACES, HeaderModel, _default_include_dir  # noqa: E402

MOD_API = TOOLS.parent
OUTPUT = MOD_API / "docs" / "API_REFERENCE_RU.md"

# Interfaces bound by hand (loader/lua/lua_bind_*.cpp) rather than generated.
HAND_WRITTEN = {
    "storage": "loader/lua/lua_bind_storage.cpp",
    "events": "loader/lua/lua_bind_events.cpp",
    "ges": "loader/lua/lua_bind_ges.cpp",
    "ui": "loader/lua/lua_bind_ui.cpp (ядро; остальные слоты — генератор)",
}

# The facade layer: module -> (raw tables it stands on, methods). Kept here
# rather than parsed from lua_preludes.cpp on purpose: the list is the
# contract an author reads, and a facade that appears here without a test
# block in tests/lua_host_tests.cpp is the review's job to catch.
FACADES: tuple[tuple[str, str, str], ...] = (
    ("wotb.context", "core",
     "current, contains, should_show, apply_visibility, is_hangar, is_battle, "
     "is_training, is_replay, is_text_input, is_mod_screen"),
    ("wotb.players", "entity_public",
     "snapshot, local_player, me, our_team, allies, enemy_team, enemies, "
     "unknown_team, visible, find, by_id, each_visible, details"),
    ("wotb.battle", "events, core",
     "start, stop, tracking, reset, snapshot, groups, on, once, off, off_all, "
     "events, on_<name> (enter, start, end, leave, shot, hit, reload, ammo, "
     "damage, death, vehicle_destroyed, spotted, unspotted, camera_changed, "
     "sniper_entered, sniper_exited), is_active, state"),
    ("wotb.session", "session_cluster, events",
     "clusters, cluster, change_cluster, on_cluster_changed, off_cluster_changed, "
     "off_all"),
    ("wotb.ges", "ges, events",
     "types, subscribe, unsubscribe, publish, schema, on, off, observe, decode, "
     "is_available"),
    ("wotb.mod", "lifecycle, permissions, capabilities",
     "id, permissions, has_permission, capability, capabilities, info, "
     "on_disable, off_disable"),
    ("wotb.hud", "gameplay_hud",
     "mode, available, status, reset, rgba, reticle.*, damage_log.*, "
     "session_stats.*, minimap.*, sixth_sense.*, hit_indicator.*"),
    ("wotb.screen", "ui, ui_read, handles",
     "root, find, children, text, live_text, rect, visible, game_owned, info, "
     "set_text, set_visible, mount, unmount, notify, popup"),
    ("wotb.vehicle", "vehicle_visual, entity_public",
     "local_vehicle, visible, is_local, is_hangar, position, appearance_state, "
     "skin.register, skin.apply, skin.rollback, skin.state, skin.release, "
     "appearance.reset, set_skin, set_camouflage"),
    ("wotb.shells", "projectile, events",
     "on, on_created, on_updated, on_impact, on_destroyed, on_local_shot, off, "
     "off_all, snapshot, visual, impact.show, impact.update, impact.hide, "
     "tracer.register, tracer.unregister"),
    ("wotb.view", "camera, gameplay_camera, camera_state, events",
     "get, set_fov, fov, reset, project, unproject, transition, shake, "
     "on_changed, off"),
    ("wotb.sound", "audio",
     "play, stop, release, set_volume, is_playing, on_finished, replace, reset, "
     "reset_all"),
    ("wotb.keys", "input, handles",
     "bind, unbind, unbind_all, key_code, pressed, down, axis, on_pressed, "
     "on_released, off, bindings, conflicts, capture_begin, capture_end"),
    ("wotb.store", "storage, json",
     "get, set, delete, has, keys, clear, flush"),
    ("wotb.files", "loaders, vfs, resources, yaml, events",
     "read_text, read_binary, read_json, read_yaml (doc:get, doc:release), "
     "load_texture, load_audio, load_scene, info, release, exists, stat, list, "
     "watch, unwatch"),
    ("wotb.panel", "ui, context, events, handles",
     "new, mount, unmount, update, set_row, set_rows, set_row_visible, row, "
     "row_count, set_button_text, visible, set_visible, show, hide, layout, "
     "set_size, set_anchor, set_position, position, size, viewport, control, "
     "errors, label, button, image, scroll, column, on, destroy"),
    ("wotb.log / wotb.json / wotb.timer / wotb.config / wotb.available", "core, events, settings, storage",
     "см. LUA_MODS_RU.md, «Convenience-модули»"),
)


def strip_prefix(text: str, prefix: str) -> str:
    return text[len(prefix):] if text.startswith(prefix) else text


def lua_signature(slot) -> tuple[str, str]:
    """(arguments, results) as an author sees them."""
    arguments: list[str] = []
    results: list[str] = []
    params = list(slot.function.parameters)
    index = 0
    while index < len(params):
        parameter = params[index]
        name = parameter.name
        c_type = parameter.c_type
        nxt = params[index + 1] if index + 1 < len(params) else None
        if name == "mod" and c_type == "WotbModV3Handle":
            index += 1
            continue
        # char* buffer, uint32_t* inout_size -> one string result
        if c_type == "char*" and nxt is not None and nxt.c_type == "uint32_t*" \
                and nxt.name.startswith("inout"):
            results.append("string")
            index += 2
            continue
        if c_type.endswith("Buffer*") and name.startswith("inout"):
            results.append("bytes")
            index += 1
            continue
        if name.startswith("out_") and c_type.endswith("*"):
            results.append(f"{strip_prefix(name, 'out_')}: {describe_type(c_type.rstrip('*'))}")
            index += 1
            continue
        if name.startswith("inout_count") or (c_type.endswith("*") and nxt is not None
                                              and nxt.name.startswith("inout_count")):
            # array out-parameter with a count: one table result
            if not name.startswith("inout_count"):
                results.append(f"{name}: array")
                index += 2
            else:
                index += 1
            continue
        if "Callback" in c_type or "Visitor" in c_type:
            arguments.append(f"{name}: function")
            if nxt is not None and nxt.name == "user_data":
                index += 2
                continue
            index += 1
            continue
        if name == "user_data":
            index += 1
            continue
        if parameter.array_extent or (c_type.startswith("const ") and c_type.endswith("*")
                                      and nxt is not None and nxt.name.endswith("_count")):
            arguments.append(f"{name}: array")
            index += 2 if nxt is not None and nxt.name.endswith("_count") else 1
            continue
        arguments.append(f"{name}: {describe_type(c_type)}")
        index += 1
    return ", ".join(arguments), ", ".join(results) if results else "true"


def describe_type(c_type: str) -> str:
    plain = c_type.replace("const ", "").strip()
    if plain.endswith("*"):
        plain = plain[:-1].strip()
    if plain in ("const char", "char"):
        return "string"
    if plain in ("float", "double"):
        return "number"
    if plain.startswith("int") or plain.startswith("uint") or plain in ("size_t",):
        return "integer"
    if plain.endswith("Handle") or plain.endswith("Token") or plain.endswith("NodeId"):
        return "handle"
    if plain.startswith("WotbModV3"):
        return "table:" + strip_prefix(plain, "WotbModV3")
    return plain


def render(model: HeaderModel) -> str:
    lines: list[str] = []
    lines.append("# Справочник Lua API WotbMod V3 (сгенерирован)")
    lines.append("")
    lines.append("Файл создаёт `tools/generate_api_reference.py` из заголовков "
                 "`include/wotbmod/*.h` — той же модели, из которой генерируются "
                 "Lua-биндинги. Не правьте его руками: `--check` в тестах "
                 "падает, когда он отстаёт от заголовков.")
    lines.append("")
    lines.append("Как читать сигнатуру: аргументы — как их принимает Lua-функция "
                 "(скрытый `mod` и `user_data` опущены, буферы приходят строкой, "
                 "callback — функцией); результат — что она возвращает при "
                 "успехе. При отказе любая функция отвечает `nil, err`. Права "
                 "— семейство permission-имён, которое проверяет забор host-а "
                 "перед вызовом; `capability`-статус и контексты — в "
                 "[API_V3_RU.md](API_V3_RU.md) и `wotb.mod.capabilities()`.")
    lines.append("")
    total_slots = 0
    total_lua = 0
    tables = constant_tables(model)
    lines.append("## Фасады (facade-first)")
    lines.append("")
    lines.append("| Модуль | Поверх raw-таблиц | Методы |")
    lines.append("| --- | --- | --- |")
    for module, raw, methods in FACADES:
        lines.append(f"| `{module}` | `{raw}` | {methods} |")
    lines.append("")
    lines.append("Правило имён: таблица фасада никогда не совпадает с таблицей "
                 "интерфейса; описание каждого модуля — в "
                 "[LUA_MODS_RU.md](LUA_MODS_RU.md), раздел «Фасады».")
    lines.append("")
    lines.append("## Raw-таблицы по интерфейсам")
    lines.append("")
    for interface in INTERFACES:
        slots = model.flatten_slots(interface)
        total_slots += len(slots)
        family = PERMISSIONS.get(interface.lua_name, ())
        lines.append(f"### `wotb.{interface.lua_name}` — `{interface.header}`")
        lines.append("")
        lines.append(f"- C ABI: `{interface.api_type}`, версия `{interface.version_macro}`;")
        if family:
            lines.append("- права: " + ", ".join(f"`{name}`" for name in family) + ";")
        else:
            lines.append("- права: без собственного забора (см. слоты);")
        if interface.lua_name in HAND_WRITTEN:
            lines.append(f"- биндинг: ручной, `{HAND_WRITTEN[interface.lua_name]}`;")
        else:
            lines.append("- биндинг: генератор `tools/generate_lua_bindings.py`;")
        constants = tables.get(interface.lua_name, [])
        if constants:
            names = ", ".join(f"`{name}`" for name, _ in constants)
            lines.append(f"- константы ({len(constants)}): {names}.")
        else:
            lines.append("- константы: нет.")
        lines.append("")
        lines.append("| Слот | Аргументы | Результат |")
        lines.append("| --- | --- | --- |")
        for slot in slots:
            full = f"{interface.lua_name}.{slot.name}"
            arguments, results = lua_signature(slot)
            if full in NATIVE_ONLY:
                lines.append(f"| `{slot.name}` | — | *не публикуется в Lua: нужен сырой нативный указатель* |")
                continue
            total_lua += 1
            lines.append(f"| `{slot.name}` | {arguments or '—'} | {results} |")
        lines.append("")
    summary = (f"Итого: {total_slots} слотов в {len(INTERFACES)} интерфейсах, "
               f"{total_lua} доступны из Lua, {len(NATIVE_ONLY)} намеренно нет.")
    lines.insert(4, summary)
    lines.insert(5, "")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="fail when the file is stale")
    parser.add_argument("--output", default=str(OUTPUT))
    args = parser.parse_args()
    model = HeaderModel(_default_include_dir())
    text = render(model)
    output = pathlib.Path(args.output)
    if args.check:
        current = output.read_text(encoding="utf-8") if output.is_file() else ""
        if current.replace("\r\n", "\n") != text:
            print(f"STALE: {output} differs from the headers; regenerate it", file=sys.stderr)
            return 1
        print(f"API reference is current: {output}")
        return 0
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
