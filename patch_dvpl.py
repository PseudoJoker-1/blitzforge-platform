"""Edit-in-place helper for the game's .dvpl resources.

Every install() backs the original .dvpl up to _mod_tools/backup/ first (once,
never overwritten), so restore() always returns the pristine file.

    python patch_dvpl.py extract Materials/Shaders/Utilities/exposure-tonemapping-fp.sl
    python patch_dvpl.py install Materials/Shaders/Utilities/exposure-tonemapping-fp.sl  work/tonemap.sl
    python patch_dvpl.py restore Materials/Shaders/Utilities/exposure-tonemapping-fp.sl
    python patch_dvpl.py restore-all
    python patch_dvpl.py clearcache

Paths are given relative to Data/ and WITHOUT the trailing .dvpl.
"""
import shutil
import sys
from pathlib import Path

from dvpl import pack, unpack

HERE = Path(__file__).resolve().parent
DATA = HERE.parent / "Data"
BACKUP = HERE / "backup"
WORK = HERE / "work"
CACHE = Path.home() / "AppData" / "Local" / "wotblitz" / "DAVAProject" / "shader_cache"


def _slots(rel: str):
    """(live .dvpl in Data/, backup copy) for a Data-relative path."""
    rel = rel.replace("\\", "/").lstrip("/")
    if rel.endswith(".dvpl"):
        rel = rel[: -len(".dvpl")]
    return DATA / (rel + ".dvpl"), BACKUP / (rel.replace("/", "__") + ".dvpl")


def _ensure_backup(live: Path, backup: Path):
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.exists():
        shutil.copy2(live, backup)
        print(f"  backed up -> {backup.name}")


def extract(rel: str, out: Path | None = None):
    live, backup = _slots(rel)
    # always extract from the pristine copy when we have one
    src = backup if backup.exists() else live
    data = unpack(src.read_bytes())
    out = out or WORK / Path(rel).name
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"extracted {src.name} ({len(data)} B) -> {out}")
    return out


def install(rel: str, src: Path):
    live, backup = _slots(rel)
    if not live.exists():
        raise SystemExit(f"no such resource: {live}")
    _ensure_backup(live, backup)
    raw = Path(src).read_bytes()
    live.write_bytes(pack(raw))
    print(f"installed {src} ({len(raw)} B) -> {live.relative_to(DATA.parent)}")


def create(rel: str, src: Path):
    """Add a resource the game does not ship.

    Kept separate from install() on purpose: install() refuses to write a file
    that has no original, so a typo in a path cannot silently create a dead
    resource instead of patching the intended one. Created files are recorded
    in backup/created.txt so restore-all knows to delete rather than restore
    them.
    """
    live, _ = _slots(rel)
    if live.exists():
        raise SystemExit(f"already exists, use install: {live}")
    raw = Path(src).read_bytes()
    live.parent.mkdir(parents=True, exist_ok=True)
    live.write_bytes(pack(raw))
    BACKUP.mkdir(parents=True, exist_ok=True)
    ledger = BACKUP / "created.txt"
    seen = ledger.read_text(encoding="utf-8").splitlines() if ledger.exists() else []
    if rel not in seen:
        with ledger.open("a", encoding="utf-8") as f:
            f.write(rel + "\n")
    print(f"created {live.relative_to(DATA.parent)} ({len(raw)} B)")


def restore(rel: str):
    live, backup = _slots(rel)
    if not backup.exists():
        raise SystemExit(f"no backup for {rel}")
    shutil.copy2(backup, live)
    print(f"restored {live.relative_to(DATA.parent)}")


def restore_all():
    if not BACKUP.exists():
        print("nothing to restore")
        return
    for b in BACKUP.glob("*.dvpl"):
        rel = b.name[: -len(".dvpl")].replace("__", "/")
        live = DATA / (rel + ".dvpl")
        if live.exists():
            shutil.copy2(b, live)
            print(f"restored {rel}")
        else:
            print(f"!! backup has no live target: {rel}")

    # files we added have no original to put back; they have to go
    ledger = BACKUP / "created.txt"
    if ledger.exists():
        for rel in ledger.read_text(encoding="utf-8").split():
            live, _ = _slots(rel)
            if live.exists():
                live.unlink()
                print(f"removed created {rel}")
        ledger.unlink()


def clearcache():
    if not CACHE.exists():
        print(f"no shader cache at {CACHE}")
        return
    n = 0
    for f in CACHE.iterdir():
        if f.is_file():
            f.unlink()
            n += 1
    print(f"cleared {n} shader cache entries from {CACHE}")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd = sys.argv[1]
    if cmd == "extract":
        extract(sys.argv[2], Path(sys.argv[3]) if len(sys.argv) > 3 else None)
    elif cmd == "install":
        install(sys.argv[2], Path(sys.argv[3]))
    elif cmd == "create":
        create(sys.argv[2], Path(sys.argv[3]))
    elif cmd == "restore":
        restore(sys.argv[2])
    elif cmd == "restore-all":
        restore_all()
    elif cmd == "clearcache":
        clearcache()
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
