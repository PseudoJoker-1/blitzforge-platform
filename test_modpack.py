"""Tests for the diff engine behind the artifact format.

The apply path rewrites client resources, so the round-trip is checked against
real files with randomised edits rather than a couple of hand-picked cases.
"""
import json
import random
import sys
from pathlib import Path

from dvpl import unpack
from modpack import apply_diff, from_lines, make_diff, sha256, to_lines

HERE = Path(__file__).resolve().parent
BACKUP = HERE / "backup"

failures = 0


def check(name, condition, detail=""):
    global failures
    if condition:
        print(f"  ok   {name}")
    else:
        failures += 1
        print(f"  FAIL {name}" + (f"\n       {detail}" if detail else ""))


def roundtrip(old: bytes, new: bytes, label: str):
    diff = make_diff(old, new, "t")
    try:
        got = apply_diff(old, diff)
    except Exception as error:  # noqa: BLE001 - the failure detail is the point
        check(label, False, f"raised {error}")
        return
    check(label, got == new,
          f"sha {sha256(got)[:12]} != {sha256(new)[:12]}")


print("line model")
samples = sorted(BACKUP.glob("*.dvpl"))
if not samples:
    print("  no backups to test against; run patch_dvpl extract first")
    sys.exit(1)

for path in samples:
    raw = unpack(path.read_bytes())
    check(f"lossless round-trip: {path.name[:38]}",
          from_lines(to_lines(raw)) == raw)

print("\ndiff engine, synthetic")
base = b"alpha\r\nbeta\r\ngamma\r\ndelta\r\nepsilon"
roundtrip(base, base, "identical input produces an empty patch")
roundtrip(base, b"alpha\r\nbeta\r\nNEW\r\ngamma\r\ndelta\r\nepsilon", "insert in the middle")
roundtrip(base, b"NEW\r\n" + base, "insert at the start")
roundtrip(base, base + b"\r\nNEW", "append at the end")
roundtrip(base, b"alpha\r\ngamma\r\ndelta\r\nepsilon", "delete a line")
roundtrip(base, b"alpha\r\nBETA\r\ngamma\r\ndelta\r\nepsilon", "replace a line")
roundtrip(base, b"", "delete everything")
roundtrip(b"", base, "build from nothing")
roundtrip(base, b"epsilon\r\ndelta\r\ngamma\r\nbeta\r\nalpha", "reorder everything")

print("\ndiff engine, randomised edits on real client files")
random.seed(20260727)
for path in samples[:6]:
    raw = unpack(path.read_bytes())
    lines = to_lines(raw)
    for trial in range(3):
        edited = list(lines)
        for _ in range(random.randint(1, 12)):
            if not edited:
                break
            action = random.choice(("insert", "delete", "replace"))
            index = random.randrange(len(edited))
            if action == "insert":
                edited.insert(index, f"// injected {trial}-{index}\r")
            elif action == "delete":
                del edited[index]
            else:
                edited[index] = f"// replaced {trial}-{index}\r"
        roundtrip(raw, from_lines(edited), f"{path.name[:30]} trial {trial + 1}")

print("\nsafety: what the diff itself can and cannot catch")
drifted = b"alpha\r\nBETA-CHANGED\r\ngamma\r\ndelta\r\nepsilon"

# A patch that removes or replaces lines names them, so the drift is visible.
replacing = make_diff(base, b"alpha\r\nCHANGED\r\ngamma\r\ndelta\r\nepsilon", "t")
try:
    apply_diff(drifted, replacing)
    check("a replacing patch rejects a drifted file", False, "it applied silently")
except ValueError as error:
    check("a replacing patch rejects a drifted file", True)
    print(f"       -> {error}")

# A pure insertion names nothing, so there is nothing to compare and it will
# apply anywhere - at the wrong line if the file moved. This is inherent to
# zero-context diffs and is exactly why source_sha256 is mandatory rather than
# advisory: the hash is the only thing standing between an insertion patch and
# a silently misplaced edit.
inserting = make_diff(base, b"alpha\r\nbeta\r\nNEW\r\ngamma\r\ndelta\r\nepsilon", "t")
try:
    apply_diff(drifted, inserting)
    check("an insertion patch cannot detect drift on its own (documented)", True)
except ValueError:
    check("an insertion patch cannot detect drift on its own (documented)", False,
          "it raised, so this note in the format docs is now wrong")

check("the source hash is what catches it",
      sha256(base) != sha256(drifted))

# --- a native mod states its version three times, and the loader checks two --
#
# The loader compares the version compiled into the binary's V3 descriptor
# against manifest.json and refuses the package outright if they differ. A
# manifest-only version bump therefore produces a mod that installs cleanly,
# reports the new version everywhere the tooling looks, and cannot load:
#
#   [error] V3 descriptor identity wotbmod.native_validation@1.1.0 does not
#           exactly match manifest wotbmod.native_validation@1.1.1
#   [error] [wotbmod.native_validation] package entrypoint failed (result=1)
#
# That shipped. It was published, approved, installed, and only the client
# refused it - the one place with no test. The three declarations are pinned
# to each other here so the next bump cannot miss one.
print("\nnative mod version identity")
import re as _re

_example = HERE / "mod_api/examples/native_validation_mod"
_source = (_example / "native_validation_mod.cpp").read_text(encoding="utf-8")
_binary = _re.search(r'kModVersion\s*=\s*"([^"]+)"', _source)
_manifest = json.loads((_example / "manifest.json").read_text(encoding="utf-8"))
_host = ((HERE / "mod_api/tests/native_validation_host_tests.cpp")
         .read_text(encoding="utf-8"))
_asserted = _re.search(r'std::strcmp\(info\.version,\s*"([^"]+)"\)', _host)

# The manifest exists twice: once beside the source, and once inside the
# packaging tree that becomes the published artifact. The loader only ever
# reads the packaged copy, so a fix applied to the source copy alone ships a
# mod that still carries the old text - and the first version of this guard
# watched exactly one of them, which is how the second had to be caught by
# hand.
_packaged = (HERE /
             "mods/native-validation/files/mods/wotbmod.native_validation"
             "/manifest.json")
_shipped = json.loads(_packaged.read_text(encoding="utf-8"))

check("the binary declares a version", _binary is not None)
check("the host contract asserts one", _asserted is not None)
if _binary and _asserted:
    check("binary, both manifests and host contract agree on the version",
          _binary.group(1) == _manifest["version"] == _shipped["version"]
          == _asserted.group(1),
          f"binary={_binary.group(1)} source-manifest={_manifest['version']} "
          f"packaged-manifest={_shipped['version']} host={_asserted.group(1)} "
          f"- the loader refuses any package whose binary and manifest "
          f"disagree, whatever the tooling reports")

check("the packaged manifest is the source manifest, in full",
      _shipped == _manifest,
      "not just the version: a permission granted in the source copy and "
      "missing from the packaged one ships a mod refused the very capability "
      "it was rebuilt for")

print(f"\n{failures} failing" if failures else "\nall passing")
sys.exit(1 if failures else 0)
