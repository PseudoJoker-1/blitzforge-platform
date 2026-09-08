"""Keep the fixed-RVA binding pack coupled to the installed client binary."""
from hashlib import sha256
from pathlib import Path
import re


TOOLS = Path(__file__).resolve().parent
GAME = TOOLS.parent
SOURCE = TOOLS / "mod_api" / "loader" / "v3_native_bindings.cpp"

text = SOURCE.read_text(encoding="utf-8")
match = re.search(
    r"kExpectedExecutableSha256\[32\]\s*=\s*\{(?P<body>.*?)\};",
    text,
    re.DOTALL,
)
assert match, "native binding SHA-256 constant is missing"
expected = bytes(
    int(value, 16)
    for value in re.findall(r"0x([0-9A-Fa-f]{2})u", match.group("body"))
)
assert len(expected) == 32, "native binding SHA-256 must contain 32 bytes"
actual = sha256((GAME / "wotblitz.exe").read_bytes()).digest()
assert expected == actual, (
    "native binding fingerprint does not match installed wotblitz.exe: "
    f"expected={expected.hex()} actual={actual.hex()}"
)

print(f"native binding fingerprint: {actual.hex()} (matching)")
