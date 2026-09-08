"""Fill a fresh local portal with the SDK's example mods, so the catalogue has
something to show on the first run.

    python seed_demo.py [--config config.json]

What it does, with the portal's own code (no HTTP): creates a developer
account `demo@example.org` (password `demo-demo-demo`) with a verified mod
and an unverified one, releases the five Lua examples and the RC1 sample
packages from ../mod_api with a demo developer key, publishes them, adds a
rating, a comment and a compatibility declaration. Needs the SDK checkout
next to this folder; on a host without it the script says so and exits.
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

PORTAL_ROOT = Path(__file__).resolve().parent
SDK_ROOT = PORTAL_ROOT.parent / "mod_api"
sys.path.insert(0, str(PORTAL_ROOT / "backend"))

import portal_server  # noqa: E402

wotbmod = portal_server.wotbmod
packages = portal_server.packages
trust = portal_server.trust

DEMO_EMAIL = "demo@example.org"
DEMO_PASSWORD = "demo-demo-demo"
SUMMARIES = {
    "example.lua_hello": "Самый короткий Lua-мод: одна строка в лог при включении.",
    "example.lua_facade_tour": "Экскурсия по фасадам: ростер, камера, прицел, клавиши, хранилище.",
    "example.lua_facade_panel": "Панель с кнопками и надписями поверх боя.",
    "example.lua_facade_battle": "События боя: выстрелы, попадания, уничтожения с никами.",
    "example.lua_hud_tweaks": "Прицел, миникарта и шестое чувство по вашему вкусу.",
    "example.lua_skin_switcher": "Переключение скинов своей машины из списка.",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=str(PORTAL_ROOT / "config.json"))
    args = parser.parse_args()
    if not (SDK_ROOT / "examples").is_dir():
        print(f"SDK examples not found at {SDK_ROOT}; nothing to seed")
        return 1
    config = portal_server.load_config(Path(args.config))
    portal = portal_server.portal_from_config(config)
    db = portal.connect()
    try:
        if db.execute("SELECT 1 FROM users WHERE email=?", (DEMO_EMAIL,)).fetchone():
            print("demo data already present")
            return 0
        user_id = portal.register(db, DEMO_EMAIL, "Demo Studio", DEMO_PASSWORD)
        db.execute("UPDATE users SET role='developer', email_verified=1 WHERE id=?", (user_id,))
        user = db.execute("SELECT * FROM users WHERE id=?", (user_id,)).fetchone()
        admin = db.execute("SELECT * FROM users WHERE role='administrator' ORDER BY id LIMIT 1").fetchone()
        with tempfile.TemporaryDirectory(prefix="wotbmod-seed-") as temporary:
            work = Path(temporary)
            key_file = work / "demo.key"
            private_key = trust.generate_private_key()
            key_file.write_text(f"{private_key:064x}\n", encoding="utf-8")
            projects = sorted(SDK_ROOT.glob("examples/lua_*"))
            published = []
            for project in projects:
                if not (project / "manifest.json").is_file():
                    continue
                out_dir = work / "rel"
                argv = ["release", str(project), "-o", str(out_dir), "--sign-with-key", str(key_file), "--key-id", "demo-studio"]
                sink = io.StringIO()
                with redirect_stdout(sink):
                    code = wotbmod.main(argv)
                if code != 0:
                    print(f"skip {project.name}: release failed")
                    continue
                manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
                record_path = out_dir / f"{manifest['id']}-{manifest['version']}.release.json"
                record = json.loads(record_path.read_text(encoding="utf-8"))
                artifact = out_dir / record["artifact"]["file"]
                sidecar = out_dir / record["signature"]["file"] if record.get("signature") else None
                lines = (project / "README_RU.md").read_text(encoding="utf-8").splitlines()
                paragraph = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
                notes = " ".join(paragraph[:3])[:400]
                release = portal.store_release(db, user, artifact.read_bytes(), sidecar.read_bytes() if sidecar else None,
                                               notes=notes, publish=True, record=record)
                db.execute("UPDATE mods SET summary=? WHERE id=?", (SUMMARIES.get(release["mod_id"], notes[:120]), release["mod_id"]))
                published.append(release["mod_id"])
                print(f"published {release['mod_id']} {release['version']}")
            if published and admin is not None:
                first = published[0]
                db.execute("UPDATE mods SET verified=1 WHERE id=?", (first,))
                db.execute("UPDATE users SET role='verified_developer' WHERE id=?", (user_id,))
                portal.audit(db, admin["id"], "mod.verify", first)
                db.execute("INSERT OR REPLACE INTO ratings(user_id, mod_id, stars, created_at) VALUES (?,?,?,?)",
                           (admin["id"], first, 5, portal_server.now_iso()))
                db.execute("INSERT INTO comments(mod_id, user_id, text, created_at) VALUES (?,?,?,?)",
                           (first, admin["id"], "Поставил через кнопку на сайте, всё встало с первого раза.", portal_server.now_iso()))
                version = db.execute("SELECT version FROM releases WHERE mod_id=? AND status='published' ORDER BY id DESC LIMIT 1", (first,)).fetchone()[0]
                db.execute("INSERT OR REPLACE INTO compat(mod_id, version, client_build, status, declared_by, created_at) VALUES (?,?,?,?,?,?)",
                           (first, version, portal.client_build, "works", user_id, portal_server.now_iso()))
        print(f"demo developer: {DEMO_EMAIL} / {DEMO_PASSWORD}")
        return 0
    finally:
        db.close()


if __name__ == "__main__":
    raise SystemExit(main())
