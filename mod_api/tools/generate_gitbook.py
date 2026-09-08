"""Generate docs/gitbook/ - the GitBook space of the SDK - from the headers
and the hand-written docs.

    python tools/generate_gitbook.py          # writes docs/gitbook/**
    python tools/generate_gitbook.py --check  # exits 1 when the tree is stale

What goes where:

- README.md / SUMMARY.md: the space's front page and table of contents;
- getting-started/: install (players), quickstart (authors);
- lua/: docs/LUA_MODS_RU.md split into one page per top-level section;
- facades/: one page per `wotb.*` facade module, its methods, and the
  section of LUA_MODS_RU.md that explains it (with its Lua examples);
- reference/: one page per raw interface from the same header model the
  bindings are generated from - every slot with its Lua signature, the
  permission family, the constants, and a call pattern for the first slots;
- packages/, status/: the package/portal/launcher/policy docs and the live
  status, copied with links rewritten to the new layout.

Copied Markdown is rewritten only in its links; the source files stay the
single place to edit. Published at https://pd0-2.gitbook.io/blitzforge (GitBook
space KAD7CcMgJYgSHlSTShg7, site site_4yBXV): the tree is pushed with the
GitBook content API, and .gitbook.yaml at the repository root is there for
Git Sync once the repository is connected.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from generate_api_reference import FACADES, HAND_WRITTEN, lua_signature  # noqa: E402
from generate_lua_bindings import NATIVE_ONLY, PERMISSIONS, constant_tables  # noqa: E402
from lua_api_model import INTERFACES, HeaderModel, _default_include_dir  # noqa: E402

MOD_API = TOOLS.parent
DOCS = MOD_API / "docs"
OUTPUT = DOCS / "gitbook"
PORTAL = "https://blitz-forge.org"

# hand-written docs -> their page in the space
COPIED = {
    "QUICKSTART_RU.md": "getting-started/quickstart.md",
    "FIRST_MOD_15MIN_RU.md": "getting-started/first-mod.md",
    "WOTBMOD_PACKAGE_FORMAT_RU.md": "packages/package-format.md",
    "PORTAL_RU.md": "packages/portal.md",
    "LAUNCHER_RU.md": "packages/launcher.md",
    "CONTRIBUTING_RU.md": "packages/contributing.md",
    "REVIEW_POLICY_RU.md": "packages/review-policy.md",
    "SECURITY_POLICY_RU.md": "packages/security-policy.md",
    "PACKAGE_SIGNATURE_RC1.md": "packages/signatures.md",
    "DAVA_NATIVE_PRIVATE_ABI_RU.md": "reference/dava-native-abi.md",
    "API_V3_RU.md": "reference/api-v3.md",
    "API_RU.md": "reference/api-overview.md",
    "HOOK_MODES_RU.md": "reference/hook-modes.md",
    "MIGRATION_V2_TO_V3_RU.md": "reference/migration-v2-v3.md",
    "API_STATUS_RU.md": "status/api-status.md",
    "API_FREEZE_RU.md": "status/api-freeze.md",
    "LIVE_EVIDENCE_RU.md": "status/live-evidence.md",
    "KNOWN_LIMITATIONS_RU.md": "status/limitations.md",
    "MODERATION_RULES_RU.md": "status/moderation.md",
    "CLIENT_PATCH_PLAYBOOK_RU.md": "status/client-patch.md",
}
LINK_TARGETS = {**COPIED, "LUA_MODS_RU.md": "lua/README.md", "API_REFERENCE_RU.md": "reference/README.md"}

TRANSLIT = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "e", "ж": "zh", "з": "z", "и": "i", "й": "y",
    "к": "k", "л": "l", "м": "m", "н": "n", "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u", "ф": "f",
    "х": "h", "ц": "c", "ч": "ch", "ш": "sh", "щ": "sch", "ъ": "", "ы": "y", "ь": "", "э": "e", "ю": "yu", "я": "ya",
}

# Raw interfaces that a facade wraps: reference pages point to the short API.
FACADE_FOR_RAW: dict[str, str] = {}
for _module, _raw, _methods in FACADES:
    if " / " in _module:
        continue
    for _table in _raw.split(", "):
        FACADE_FOR_RAW.setdefault(_table.strip(), _module)

# Argument placeholders for the generated call patterns, by declared type.
PLACEHOLDERS = {
    "string": '"..."', "number": "0.0", "integer": "0", "handle": "handle", "function": "function(...) end",
    "array": "{}", "boolean": "true",
}


def slug(text: str) -> str:
    text = re.sub(r"[`*_]", "", text).strip().lower()
    text = "".join(TRANSLIT.get(char, char) for char in text)
    text = re.sub(r"[^a-z0-9]+", "-", text).strip("-")
    return text[:60] or "page"


def rewrite_links(markdown: str, page: str) -> str:
    """Relative links between the copied docs follow them into the space."""
    depth = page.count("/")
    up = "../" * depth

    def replace(match: re.Match[str]) -> str:
        target = match.group(2)
        base, _, anchor = target.partition("#")
        name = base.rsplit("/", 1)[-1]
        if name in LINK_TARGETS:
            return f"[{match.group(1)}]({up}{LINK_TARGETS[name]}{'#' + anchor if anchor else ''})"
        if base.startswith("../") or base.startswith("../../"):
            # a path into the repository (tests, tools, examples): GitHub keeps it readable
            return f"[{match.group(1)}](https://github.com/PseudoJoker-1/wotb-mod-api/blob/main/mod_api/{base.lstrip('./')})"
        return match.group(0)

    return re.sub(r"\[([^\]]+)\]\((?!https?://|#|mailto:)([^)]+)\)", replace, markdown)


def split_sections(markdown: str, level: str = "## ") -> list[tuple[str, str]]:
    """(heading text, body including sub-headings) for each `level` heading."""
    sections: list[tuple[str, list[str]]] = []
    intro: list[str] = []
    in_code = False
    for line in markdown.split("\n"):
        if line.startswith("```"):
            in_code = not in_code
        if not in_code and line.startswith(level) and not line.startswith(level + "#"):
            sections.append((line[len(level):].strip(), []))
            continue
        (sections[-1][1] if sections else intro).append(line)
    result = [("", "\n".join(intro).strip())] if "".join(intro).strip() else []
    result += [(title, "\n".join(body).strip()) for title, body in sections]
    return result


def facade_sections(lua_doc: str) -> dict[str, str]:
    """`### `wotb.x` ...` sections of LUA_MODS_RU.md keyed by module name."""
    found: dict[str, str] = {}
    for _title, body in split_sections(lua_doc):
        for sub_title, sub_body in split_sections(body, "### "):
            match = re.match(r"`(wotb\.[a-z_]+)`", sub_title)
            if match:
                text = f"### {sub_title}\n\n{sub_body}".strip()
                found[match.group(1)] = found.get(match.group(1), "") + ("\n\n" if match.group(1) in found else "") + text
    return found


def call_pattern(interface_name: str, slot, arguments: str, results: str) -> str:
    args = []
    for item in [part.strip() for part in arguments.split(",") if part.strip()]:
        name, _, kind = item.partition(":")
        kind = kind.strip()
        base = kind.split(":")[0]
        args.append(PLACEHOLDERS.get(base, f"{name.strip()}"))
    call = f"wotb.{interface_name}.{slot.name}({', '.join(args)})"
    var = re.sub(r"[^a-z0-9_]", "_", slot.name.lower())
    if results == "true":
        return f"local {var}_ok, err = {call}\nif not {var}_ok then wotb.log.warn(\"{interface_name}.{slot.name}: %s\", err) end"
    return f"local {var}, err = {call}  -- {results}\nif {var} == nil then wotb.log.warn(\"{interface_name}.{slot.name}: %s\", err) end"


def render_reference_page(model: HeaderModel, interface, tables) -> str:
    slots = model.flatten_slots(interface)
    family = PERMISSIONS.get(interface.lua_name, ())
    lines = [f"# `wotb.{interface.lua_name}`", ""]
    lines.append(f"Raw-таблица интерфейса `{interface.api_type}` (`include/wotbmod/{interface.header}`, "
                 f"версия `{interface.version_macro}`). Функции ниже вызываются как "
                 f"`wotb.{interface.lua_name}.<слот>(...)`; при отказе любая отвечает `nil, err`.")
    lines.append("")
    facade = FACADE_FOR_RAW.get(interface.lua_name)
    if facade:
        lines.append(f"Короткий API поверх этой таблицы: [`{facade}`](../facades/{facade.replace('.', '-')}.md). "
                     "Начинайте с него; raw-таблица нужна, когда фасаду не хватает слота.")
        lines.append("")
    lines.append("## Права")
    lines.append("")
    if family:
        lines.append("Забор host-а проверяет перед вызовом: " + ", ".join(f"`{name}`" for name in family) + ". "
                     "Имена объявляются в `permissions` манифеста.")
    else:
        lines.append("Без собственного забора: право проверяется по слоту (см. таблицу).")
    lines.append("")
    if interface.lua_name in HAND_WRITTEN:
        lines.append(f"Биндинг ручной: `{HAND_WRITTEN[interface.lua_name]}`.")
        lines.append("")
    constants = tables.get(interface.lua_name, [])
    if constants:
        lines.append("## Константы")
        lines.append("")
        lines.append("| Имя в Lua | В заголовке | Тип |")
        lines.append("| --- | --- | --- |")
        for name, constant in constants:
            kind = "строка" if constant.is_string else ("enum " + constant.enum_type if constant.enum_type else constant.kind)
            lines.append(f"| `wotb.{interface.lua_name}.{name}` | `{constant.c_name}` | {kind} |")
        lines.append("")
    lines.append("## Функции")
    lines.append("")
    lines.append("| Функция | Аргументы | Результат |")
    lines.append("| --- | --- | --- |")
    visible = []
    for slot in slots:
        full = f"{interface.lua_name}.{slot.name}"
        arguments, results = lua_signature(slot)
        if full in NATIVE_ONLY:
            lines.append(f"| `{slot.name}` | — | *не публикуется в Lua: нужен сырой нативный указатель* |")
            continue
        visible.append((slot, arguments, results))
        lines.append(f"| `{slot.name}` | {arguments or '—'} | {results} |")
    lines.append("")
    if visible:
        lines.append("## Пример вызова")
        lines.append("")
        lines.append("Шаблон, собранный из сигнатур (подставьте свои значения; `handle` — "
                     "то, что вернул создающий вызов этой же таблицы):")
        lines.append("")
        lines.append("```lua")
        lines.append(f"-- manifest.json: \"permissions\": [{', '.join(chr(34) + n + chr(34) for n in family) or '...'}]")
        for slot, arguments, results in visible[:3]:
            lines.append(call_pattern(interface.lua_name, slot, arguments, results))
        lines.append("```")
        lines.append("")
    return "\n".join(lines) + "\n"


INTERFACE_NAMES = {interface.lua_name for interface in INTERFACES}


def raw_table_link(name: str) -> str:
    """A link when the name is a published interface; plain code when the facade
    stands on a convenience module (`wotb.json`, `wotb.context`) that has no
    header of its own."""
    return f"[`wotb.{name}`](../reference/{name}.md)" if name in INTERFACE_NAMES else f"`wotb.{name}`"


def render_facade_page(module: str, raw: str, methods: str, section: str | None) -> str:
    lines = [f"# `{module}`", ""]
    lines.append("Фасад поверх raw-таблиц: " + ", ".join(raw_table_link(t.strip()) for t in raw.split(",")) + ".")
    lines.append("")
    lines.append("## Методы")
    lines.append("")
    for method in [m.strip() for m in methods.split(",") if m.strip()]:
        lines.append(f"- `{module}.{method}`")
    lines.append("")
    if section:
        lines.append("## Как пользоваться")
        lines.append("")
        lines.append(rewrite_links(section, f"facades/{module}.md"))
        lines.append("")
    else:
        lines.append("Описание и примеры: [руководство по Lua-модам](../lua/README.md).")
        lines.append("")
    return "\n".join(lines) + "\n"


def render_examples_page() -> str:
    lines = ["# Примеры модов", "",
             "Каждый пример — готовая папка `mod_api/examples/<имя>` с `manifest.json`; Lua-примеры копируются в "
             "`mods/lua/<id>` игры или собираются в пакет командой `wotbmod release`.", "",
             "| Пример | Что показывает |", "| --- | --- |"]
    for folder in sorted((MOD_API / "examples").iterdir()):
        if not folder.is_dir():
            continue
        readme = folder / "README_RU.md"
        summary = ""
        if readme.is_file():
            for line in readme.read_text(encoding="utf-8", errors="replace").split("\n"):
                text = line.strip()
                if text and not text.startswith("#"):
                    summary = text
                    break
        url = f"https://github.com/PseudoJoker-1/wotb-mod-api/tree/main/mod_api/examples/{folder.name}"
        lines.append(f"| [`{folder.name}`]({url}) | {summary} |")
    lines.append("")
    return "\n".join(lines) + "\n"


def render_install_page() -> str:
    return f"""# Установка для игрока

1. Закройте игру и скачайте установщик `BlitzForge-Setup-<версия>.exe` со страницы [{PORTAL}/download]({PORTAL}/download).
2. Запустите его: обычный мастер Windows найдёт игру через Steam, проверит сборку клиента, докачает недостающие зависимости (Visual C++ Runtime x86; Python только для разработчиков) и поставит загрузчик модов, Lua-хост, команду `wotbmod` и кнопку «Установить в игру».
3. Откройте [каталог модов]({PORTAL}) и нажмите «Установить в игру» у любого мода. Установщик покажет права и хеш и спросит подтверждение.

Из терминала (после установки откройте новое окно):

```bat
wotbmod list
wotbmod install blitzforge.night_mode --catalog {PORTAL}/api/v1
wotbmod uninstall blitzforge.night_mode
wotbmod rollback blitzforge.night_mode
```

Удаление: «Приложения» Windows → BlitzForge → Удалить. Исходные файлы игры возвращаются, запись в PATH и регистрация `wotbmod://` снимаются.

Набор подходит только той сборке клиента, для которой собран; после обновления игры скачайте новую версию.
"""


def render_readme() -> str:
    return f"""# BlitzForge: моды для World of Tanks Blitz

BlitzForge — это загрузчик модов, Lua API поверх клиента и портал [{PORTAL}]({PORTAL}), где моды публикуются, подписываются и ставятся одной кнопкой.

- **Игроку**: [установка](getting-started/install.md) и [каталог]({PORTAL}).
- **Автору мода**: [быстрый старт](getting-started/quickstart.md), [руководство по Lua](lua/README.md), [фасады `wotb.*`](facades/README.md) и [справочник всех функций](reference/README.md).
- **Чужие моды**: [импорт существующих модов](packages/import.md) в формат пакета ресурсов — со снимком стоковых файлов и честным удалением.
- **Формат и правила**: [пакеты](packages/package-format.md), [портал](packages/portal.md), [политика проверки](packages/review-policy.md), [безопасность](packages/security-policy.md).
- **Что подтверждено живым клиентом**: [статус API](status/api-status.md), [заморозка API 1.0](status/api-freeze.md), [известные ограничения](status/limitations.md).
- **Разработчику на бете**: [первый мод за 15 минут](getting-started/first-mod.md), [правила модерации](status/moderation.md), [что делать после патча клиента](status/client-patch.md).

Документация собирается из репозитория [PseudoJoker-1/wotb-mod-api](https://github.com/PseudoJoker-1/wotb-mod-api) командой `python tools/generate_gitbook.py`; страницы справочника и фасадов генерируются из тех же заголовков, из которых собираются Lua-биндинги, поэтому они не отстают от кода.
"""


def render_import_page() -> str:
    return f"""# Импорт существующих модов

Большинство модов для World of Tanks Blitz — это подмена файлов игры: папка или zip, повторяющая структуру `Data/`, с запакованными `.dvpl` или сырыми файлами (текстуры, модели, шейдеры, YAML, звуки). Такой мод не знает о нашем API, но его можно перенести в формат [пакета ресурсов](package-format.md#пакеты-ресурсов-type-resource) одной командой:

```bat
wotbmod import <папка-или-zip мода> --id author.mod_name --name "Название" --version 1.0.0 --developer "Автор" -o my_mod
wotbmod release my_mod --sign-with-key <ключ> --key-id <id>
wotbmod publish my_mod\\author.mod_name-1.0.0.release.json --to {PORTAL}/api/v1 --token <токен>
```

Что делает `import`:

1. находит файлы игры в источнике: снимает обёртку `Data/`, `patched/` или `files/`, распаковывает `.dvpl`, пропускает readme, манифесты старых форматов и `.diff`;
2. для каждого файла читает стоковый файл локального клиента и записывает его SHA-256 (`stock_sha256`); файла, которого в стоковой игре нет, помечает как новый (`stock_sha256: null`); файлы, совпадающие со стоковыми, пропускает;
3. пишет проект `manifest.json` + `files/` + `README_RU.md`, готовый к `wotbmod release`.

Важно: стоковые хеши снимаются с вашего клиента, поэтому импортируйте на чистой игре (Steam → Свойства → Проверить целостность файлов) и той сборки, для которой публикуете. У игроков `wotbmod install` откажется ставить пакет, если их файл не стоковый и не наш, а `uninstall` вернёт оригинал или удалит новый файл.

Что импорт не переносит: Python/JS-скрипты, DLL и любые исполняемые файлы (пакет ресурсов их не содержит по правилам проверки), моды, требующие изменений в самом клиенте. Такие вещи пишутся заново на Lua API.
"""


def generate(model: HeaderModel) -> dict[str, str]:
    pages: dict[str, str] = {}
    tables = constant_tables(model)
    lua_doc = (DOCS / "LUA_MODS_RU.md").read_text(encoding="utf-8")

    pages["README.md"] = render_readme()
    pages["getting-started/install.md"] = render_install_page()
    for source, target in COPIED.items():
        path = DOCS / source
        if path.is_file():
            pages[target] = rewrite_links(path.read_text(encoding="utf-8"), target)

    # the Lua guide, one page per top-level section
    lua_pages: list[tuple[str, str]] = []
    sections = split_sections(lua_doc)
    intro = ""
    for index, (title, body) in enumerate(sections):
        if not title:
            intro = body
            continue
        name = f"lua/{index:02d}-{slug(title)}.md"
        pages[name] = rewrite_links(f"# {title}\n\n{body}\n", name)
        lua_pages.append((title, name))
    guide_lines = [rewrite_links(intro, "lua/README.md") if intro else "# Lua-моды", "", "## Разделы", ""]
    guide_lines += [f"- [{title}]({name.split('/', 1)[1]})" for title, name in lua_pages]
    pages["lua/README.md"] = "\n".join(guide_lines) + "\n"

    # facades
    facade_docs = facade_sections(lua_doc)
    facade_pages: list[tuple[str, str]] = []
    for module, raw, methods in FACADES:
        modules = [m.strip() for m in module.split(" / ")]
        for one in modules:
            name = f"facades/{one.replace('.', '-')}.md"
            pages[name] = render_facade_page(one, raw, methods if len(modules) == 1 else "см. описание", facade_docs.get(one))
            facade_pages.append((one, name))
    facade_index = ["# Фасады `wotb.*`", "",
                    "Короткий API для авторов: каждый модуль стоит поверх одной или нескольких raw-таблиц и никогда не "
                    "совпадает с ними по имени. Начинайте с фасада; raw-таблица нужна, когда фасаду не хватает слота.", "",
                    "| Модуль | Поверх raw-таблиц |", "| --- | --- |"]
    facade_index += [f"| [`{one}`]({name.split('/', 1)[1]}) | `{raw}` |" for (one, name), (_m, raw, _x)
                     in zip(facade_pages, [f for f in FACADES for _ in f[0].split(' / ')])]
    pages["facades/README.md"] = "\n".join(facade_index) + "\n"

    # reference
    reference_pages: list[tuple[str, str]] = []
    total_lua = 0
    for interface in INTERFACES:
        name = f"reference/{interface.lua_name}.md"
        pages[name] = render_reference_page(model, interface, tables)
        reference_pages.append((interface.lua_name, name))
        total_lua += sum(1 for slot in model.flatten_slots(interface) if f"{interface.lua_name}.{slot.name}" not in NATIVE_ONLY)
    reference_index = ["# Справочник raw-таблиц", "",
                       f"Все {total_lua} Lua-функции в {len(INTERFACES)} интерфейсах, из тех же заголовков "
                       "`include/wotbmod/*.h`, из которых генерируются биндинги. Аргументы даны так, как их принимает "
                       "Lua-функция (скрытый `mod` и `user_data` опущены, буферы приходят строкой, callback — функцией); "
                       "при отказе любая функция отвечает `nil, err`.", "",
                       "| Таблица | Заголовок | Фасад |", "| --- | --- | --- |"]
    for interface in INTERFACES:
        facade = FACADE_FOR_RAW.get(interface.lua_name, "")
        reference_index.append(f"| [`wotb.{interface.lua_name}`]({interface.lua_name}.md) | `{interface.header}` | "
                               + (f"[`{facade}`](../facades/{facade.replace('.', '-')}.md)" if facade else "—") + " |")
    pages["reference/README.md"] = "\n".join(reference_index) + "\n"

    pages["packages/import.md"] = render_import_page()
    pages["packages/examples.md"] = render_examples_page()

    summary = ["# Содержание", "", "* [BlitzForge](README.md)", "", "## Начало", "",
               "* [Установка для игрока](getting-started/install.md)",
               "* [Быстрый старт автора](getting-started/quickstart.md)",
               "* [Первый мод за 15 минут](getting-started/first-mod.md)", "", "## Lua-моды", "",
               "* [Руководство](lua/README.md)"]
    summary += [f"  * [{title}]({name})" for title, name in lua_pages]
    summary += ["", "## Фасады wotb.*", "", "* [Обзор фасадов](facades/README.md)"]
    summary += [f"  * [{one}]({name})" for one, name in facade_pages]
    summary += ["", "## Справочник", "", "* [Все raw-таблицы](reference/README.md)"]
    summary += [f"  * [wotb.{one}]({name})" for one, name in reference_pages]
    summary += ["* [Архитектура V3](reference/api-v3.md)", "* [Обзор API](reference/api-overview.md)",
                "* [Режимы хуков](reference/hook-modes.md)", "* [Миграция V2 → V3](reference/migration-v2-v3.md)",
                "* [Loader-private DAVA ABI](reference/dava-native-abi.md)",
                "", "## Пакеты и портал", "",
                "* [Формат пакета](packages/package-format.md)", "* [Импорт существующих модов](packages/import.md)",
                "* [Примеры модов](packages/examples.md)", "* [Портал](packages/portal.md)",
                "* [Launcher wotbmod://](packages/launcher.md)", "* [Подписи](packages/signatures.md)",
                "* [Как публиковать](packages/contributing.md)", "* [Политика проверки](packages/review-policy.md)",
                "* [Безопасность](packages/security-policy.md)", "", "## Статус", "",
                "* [Статус API](status/api-status.md)", "* [Заморозка API 1.0](status/api-freeze.md)",
                "* [Живые свидетельства](status/live-evidence.md)",
                "* [Известные ограничения](status/limitations.md)", "* [Правила модерации](status/moderation.md)",
                "* [Когда обновился клиент](status/client-patch.md)"]
    pages["SUMMARY.md"] = "\n".join(summary) + "\n"
    return pages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="fail when docs/gitbook is stale")
    parser.add_argument("--output", default=str(OUTPUT))
    args = parser.parse_args()
    pages = generate(HeaderModel(_default_include_dir()))
    output = pathlib.Path(args.output)
    if args.check:
        stale = []
        for name, text in pages.items():
            path = output / name
            current = path.read_text(encoding="utf-8").replace("\r\n", "\n") if path.is_file() else ""
            if current != text.replace("\r\n", "\n"):
                stale.append(name)
        existing = {p.relative_to(output).as_posix() for p in output.rglob("*.md")} if output.is_dir() else set()
        extra = sorted(existing - set(pages))
        if stale or extra:
            print(f"STALE: {len(stale)} page(s) differ, {len(extra)} extra: {', '.join((stale + extra)[:6])}", file=sys.stderr)
            return 1
        print(f"GitBook tree is current: {output} ({len(pages)} pages)")
        return 0
    if output.is_dir():
        shutil.rmtree(output)
    for name, text in pages.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {len(pages)} pages under {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
