"""Create the portal's P-256 signing key pair without the SDK's CLI.

    python tools/portal_keygen.py --out data/portal.key --key-id blitzforge-portal-2026

Writes the private key (64 hex characters; keep it like the admin password)
and `<key-id>.p256` next to it - the public key players put into
`<game>/mods/trust/keys/` so releases signed by this portal count as trusted.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

PORTAL_ROOT = Path(__file__).resolve().parents[1]
_SDK_TOOLS = PORTAL_ROOT.parent / "mod_api" / "tools"
sys.path.insert(0, str(_SDK_TOOLS if (_SDK_TOOLS / "wotbmod_trust.py").is_file() else PORTAL_ROOT / "backend" / "vendor"))

import wotbmod_trust as trust  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="portal signing key pair")
    parser.add_argument("--out", required=True)
    parser.add_argument("--key-id", required=True)
    args = parser.parse_args()
    if trust.KEY_ID_RE.fullmatch(args.key_id) is None:
        print("key id: letters, digits, '_', '.', '-', up to 127 characters", file=sys.stderr)
        return 2
    out = Path(args.out)
    if out.exists():
        print(f"refusing to overwrite {out}", file=sys.stderr)
        return 2
    out.parent.mkdir(parents=True, exist_ok=True)
    private_key = trust.generate_private_key()
    handle = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{private_key:064x}\n")
    public = out.parent / f"{args.key_id}.p256"
    public.write_text(trust.public_key_to_hex(*trust.public_key_of(private_key)) + "\n", encoding="utf-8", newline="\n")
    print(f"private key: {out}")
    print(f"public key:  {public}  (copy to <game>/mods/trust/keys/ on players' machines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
