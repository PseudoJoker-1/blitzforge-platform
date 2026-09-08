"""Package trust for the WotbMod developer CLI: ECDSA P-256 / SHA-256.

This is the pure-Python twin of the loader's `src/v3/package_trust.h` and
of `tools/sign_release_artifact.ps1`, so the CLI, the portal and the
launcher can judge a `.wotbmod` the same way the client does, on any
machine, without OpenSSL or a Windows CNG key store:

- a *sidecar* `<artifact>.sig` is five text lines (`WOTBMOD-SIGNATURE-V1`,
  `algorithm=ecdsa-p256-sha256`, `key_id=...`, `sha256=<artifact digest>`,
  `signature=<64-byte R||S in hex>`);
- the signature is ECDSA over the raw SHA-256 digest of the artifact bytes;
- a *trust store* `mods/trust/keys/<key_id>.p256` holds the signer's public
  key as 64 bytes `X||Y` in hex, plus an optional signed
  `mods/trust/revocations.list`.

Verification is deliberately explicit and slow-but-obvious: schoolbook
Jacobian point arithmetic on the NIST P-256 curve. It takes a few
milliseconds per signature, which is fine for a package installer.

The signer here (`sign_p256_sha256`) exists so that tests, a self-hosted
portal and developers with their own key file can produce signatures the
loader accepts. The public preview key is non-exportable and stays behind
`sign_release_artifact.ps1`.

Stdlib only.
"""

from __future__ import annotations

import hashlib
import re
import secrets
from dataclasses import dataclass
from pathlib import Path

SIDECAR_HEADER = "WOTBMOD-SIGNATURE-V1"
REVOCATIONS_HEADER = "WOTBMOD-REVOCATIONS-V1"
ALGORITHM = "ecdsa-p256-sha256"
KEY_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,126}$")
HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")
MAX_SIDECAR_BYTES = 16 * 1024
MAX_REVOCATIONS_BYTES = 1024 * 1024

# NIST P-256 (secp256r1) domain parameters.
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5


class TrustError(Exception):
    """A malformed sidecar, key, or revocation list."""


# --- curve arithmetic -------------------------------------------------------

def _inverse(value: int, modulus: int) -> int:
    return pow(value % modulus, modulus - 2, modulus)


def _on_curve(x: int, y: int) -> bool:
    if not (0 <= x < P and 0 <= y < P):
        return False
    return (y * y - (x * x * x + A * x + B)) % P == 0


def _jacobian_double(point):
    x, y, z = point
    if y == 0 or z == 0:
        return (0, 1, 0)
    ysq = (y * y) % P
    s = (4 * x * ysq) % P
    m = (3 * x * x + A * z * z * z * z) % P
    nx = (m * m - 2 * s) % P
    ny = (m * (s - nx) - 8 * ysq * ysq) % P
    nz = (2 * y * z) % P
    return (nx, ny, nz)


def _jacobian_add(left, right):
    if left[2] == 0:
        return right
    if right[2] == 0:
        return left
    x1, y1, z1 = left
    x2, y2, z2 = right
    z1z1 = (z1 * z1) % P
    z2z2 = (z2 * z2) % P
    u1 = (x1 * z2z2) % P
    u2 = (x2 * z1z1) % P
    s1 = (y1 * z2 * z2z2) % P
    s2 = (y2 * z1 * z1z1) % P
    if u1 == u2:
        if s1 != s2:
            return (0, 1, 0)
        return _jacobian_double(left)
    h = (u2 - u1) % P
    r = (s2 - s1) % P
    h2 = (h * h) % P
    h3 = (h * h2) % P
    u1h2 = (u1 * h2) % P
    nx = (r * r - h3 - 2 * u1h2) % P
    ny = (r * (u1h2 - nx) - s1 * h3) % P
    nz = (h * z1 * z2) % P
    return (nx, ny, nz)


def _jacobian_multiply(point, scalar: int):
    result = (0, 1, 0)
    addend = point
    while scalar > 0:
        if scalar & 1:
            result = _jacobian_add(result, addend)
        addend = _jacobian_double(addend)
        scalar >>= 1
    return result


def _to_affine(point):
    x, y, z = point
    if z == 0:
        return None
    inverse = _inverse(z, P)
    inverse2 = (inverse * inverse) % P
    return ((x * inverse2) % P, (y * inverse2 * inverse) % P)


def _scalar_multiply(x: int, y: int, scalar: int):
    return _to_affine(_jacobian_multiply((x, y, 1), scalar))


# --- public key / signature primitives ---------------------------------------

def decode_hex(text: str, length: int, label: str) -> bytes:
    text = text.strip()
    if len(text) != length * 2 or HEX_RE.fullmatch(text) is None:
        raise TrustError(f"{label} must be {length} bytes of strict hexadecimal")
    return bytes.fromhex(text)


def public_key_from_hex(key_hex: str) -> tuple[int, int]:
    raw = decode_hex(key_hex, 64, "P-256 public key")
    x = int.from_bytes(raw[:32], "big")
    y = int.from_bytes(raw[32:], "big")
    if not _on_curve(x, y):
        raise TrustError("P-256 public key is not a point on the curve")
    return x, y


def public_key_to_hex(x: int, y: int) -> str:
    return x.to_bytes(32, "big").hex() + y.to_bytes(32, "big").hex()


def verify_p256_sha256(public_key_hex: str, digest_hex: str, signature_hex: str) -> bool:
    """True when `signature_hex` (R||S) signs the 32-byte `digest_hex` under the key."""
    try:
        qx, qy = public_key_from_hex(public_key_hex)
        digest = decode_hex(digest_hex, 32, "SHA-256 digest")
        raw = decode_hex(signature_hex, 64, "P-256 signature")
    except TrustError:
        return False
    r = int.from_bytes(raw[:32], "big")
    s = int.from_bytes(raw[32:], "big")
    if not (1 <= r < N and 1 <= s < N):
        return False
    e = int.from_bytes(digest, "big")
    w = _inverse(s, N)
    u1 = (e * w) % N
    u2 = (r * w) % N
    point = _jacobian_add(
        _jacobian_multiply((GX, GY, 1), u1),
        _jacobian_multiply((qx, qy, 1), u2),
    )
    affine = _to_affine(point)
    if affine is None:
        return False
    return affine[0] % N == r


def generate_private_key() -> int:
    """A fresh random P-256 private scalar (for tests and developer keys)."""
    while True:
        candidate = secrets.randbelow(N)
        if 1 <= candidate < N:
            return candidate


def public_key_of(private_key: int) -> tuple[int, int]:
    if not 1 <= private_key < N:
        raise TrustError("private key is out of range")
    point = _scalar_multiply(GX, GY, private_key)
    assert point is not None
    return point


def _rfc6979_nonce(private_key: int, digest: bytes) -> int:
    """Deterministic k (RFC 6979, HMAC-SHA256) so signing needs no RNG."""
    import hmac

    holen = 32
    x = private_key.to_bytes(32, "big")
    h1 = digest
    v = b"\x01" * holen
    k = b"\x00" * holen
    k = hmac.new(k, v + b"\x00" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h1, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        candidate = int.from_bytes(v, "big")
        if 1 <= candidate < N:
            return candidate
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def sign_p256_sha256(private_key: int, digest_hex: str) -> str:
    """R||S in hex over a 32-byte digest, deterministic (RFC 6979)."""
    digest = decode_hex(digest_hex, 32, "SHA-256 digest")
    if not 1 <= private_key < N:
        raise TrustError("private key is out of range")
    e = int.from_bytes(digest, "big")
    while True:
        k = _rfc6979_nonce(private_key, digest)
        point = _scalar_multiply(GX, GY, k)
        assert point is not None
        r = point[0] % N
        if r == 0:
            digest = hashlib.sha256(digest).digest()
            continue
        s = (_inverse(k, N) * (e + r * private_key)) % N
        if s == 0:
            digest = hashlib.sha256(digest).digest()
            continue
        return r.to_bytes(32, "big").hex() + s.to_bytes(32, "big").hex()


# --- sidecars -----------------------------------------------------------------

@dataclass(frozen=True)
class Sidecar:
    algorithm: str
    key_id: str
    sha256: str
    signature: str

    def render(self) -> str:
        return (
            f"{SIDECAR_HEADER}\n"
            f"algorithm={self.algorithm}\n"
            f"key_id={self.key_id}\n"
            f"sha256={self.sha256}\n"
            f"signature={self.signature}\n"
        )


def parse_sidecar(text: str) -> Sidecar:
    """The loader's rules: header, name=value lines, no duplicates, no extras."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != SIDECAR_HEADER:
        raise TrustError("signature sidecar header is invalid")
    fields: dict[str, str] = {}
    for raw in lines[1:]:
        line = raw.strip()
        if not line:
            continue
        if "=" not in line:
            raise TrustError("signature sidecar entry is invalid")
        name, value = (part.strip() for part in line.split("=", 1))
        if not value or name in fields:
            raise TrustError("signature sidecar field is empty or duplicated")
        if name not in ("algorithm", "key_id", "sha256", "signature"):
            raise TrustError("signature sidecar contains an unknown field")
        fields[name] = value
    if len(fields) != 4:
        raise TrustError("signature sidecar is incomplete")
    return Sidecar(fields["algorithm"], fields["key_id"], fields["sha256"], fields["signature"])


def sidecar_path_for(artifact: Path) -> Path:
    """`<artifact>.sig`; a package directory signs as `<dir>.wotbmod.sig`."""
    if artifact.is_dir():
        return artifact.parent / (artifact.name + ".wotbmod.sig")
    return artifact.parent / (artifact.name + ".sig")


def sign_file(artifact: Path, private_key: int, key_id: str) -> Sidecar:
    if KEY_ID_RE.fullmatch(key_id) is None:
        raise TrustError("key id contains unsupported characters or has an invalid length")
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    return Sidecar(ALGORITHM, key_id, digest, sign_p256_sha256(private_key, digest))


# --- trust store ----------------------------------------------------------------

def _read_small(path: Path, maximum: int, label: str) -> str:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise TrustError(f"{label} cannot be queried: {exc}") from exc
    if size == 0 or size > maximum:
        raise TrustError(f"{label} is empty or oversized")
    try:
        return path.read_bytes().decode("utf-8", errors="replace")
    except OSError as exc:
        raise TrustError(f"{label} cannot be read: {exc}") from exc


def load_trusted_key(trust_root: Path, key_id: str) -> str | None:
    """The hex public key for `key_id`, or None when the store does not know it."""
    if KEY_ID_RE.fullmatch(key_id) is None:
        raise TrustError("signature key id is invalid")
    key_path = trust_root / "keys" / f"{key_id}.p256"
    if not key_path.is_file():
        return None
    key_hex = _read_small(key_path, 4096, "trusted key").strip()
    public_key_from_hex(key_hex)  # validates length and curve membership
    return key_hex


@dataclass(frozen=True)
class Revocations:
    key_ids: frozenset[str]
    releases: frozenset[str]


def parse_revocations(text: str) -> Revocations:
    lines = text.splitlines()
    if not lines or lines[0].strip() != REVOCATIONS_HEADER:
        raise TrustError("revocation-list header is invalid")
    key_ids: set[str] = set()
    releases: set[str] = set()
    for raw in lines[1:]:
        line = raw.strip()
        if not line:
            continue
        if "=" not in line:
            raise TrustError("revocation-list entry is invalid")
        name, value = (part.strip() for part in line.split("=", 1))
        if name == "revoke_key":
            if KEY_ID_RE.fullmatch(value) is None or value.lower() in key_ids:
                raise TrustError("revocation-list key entry is invalid or duplicated")
            key_ids.add(value.lower())
        elif name == "revoke_release":
            if value.count("@") != 1:
                raise TrustError("revoked release must be id@version")
            mod_id, version = value.split("@", 1)
            release = f"{mod_id.lower()}@{version}"
            if not mod_id or not version or release in releases:
                raise TrustError("revoked release is invalid or duplicated")
            releases.add(release)
        else:
            raise TrustError("revocation list contains an unknown field")
    return Revocations(frozenset(key_ids), frozenset(releases))


def load_revocations(trust_root: Path) -> Revocations:
    """`trust/revocations.list`, accepted only under a trusted, unrevoked signature."""
    list_path = trust_root / "revocations.list"
    if not list_path.exists():
        return Revocations(frozenset(), frozenset())
    text = _read_small(list_path, MAX_REVOCATIONS_BYTES, "revocation list")
    digest = hashlib.sha256(list_path.read_bytes()).hexdigest()
    sidecar = parse_sidecar(_read_small(
        list_path.parent / (list_path.name + ".sig"), MAX_SIDECAR_BYTES, "revocation-list signature"))
    if sidecar.algorithm.lower() != ALGORITHM or sidecar.sha256.lower() != digest:
        raise TrustError("revocation-list signature metadata does not match its bytes")
    key_hex = load_trusted_key(trust_root, sidecar.key_id)
    if key_hex is None or not verify_p256_sha256(key_hex, digest, sidecar.signature):
        raise TrustError("revocation list is not signed by a trusted key")
    revocations = parse_revocations(text)
    if sidecar.key_id.lower() in revocations.key_ids:
        raise TrustError("revocation list was signed by a key it revokes")
    return revocations


@dataclass(frozen=True)
class VerifyResult:
    """What the loader would conclude about an artifact.

    status: unsigned | valid | untrusted | invalid | unsupported | revoked
    """

    status: str
    key_id: str | None
    detail: str

    @property
    def trusted(self) -> bool:
        return self.status == "valid"


def verify_artifact(
    artifact: Path,
    trust_root: Path,
    *,
    sidecar: Path | None = None,
    release: str | None = None,
) -> VerifyResult:
    """Judge a file against `<trust_root>/keys` exactly like the loader.

    `release` is `id@version` for the revocation check (skipped when None).
    """
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    return verify_digest(digest, sidecar or sidecar_path_for(artifact), trust_root, release=release)


def verify_digest(
    digest: str,
    sidecar_path: Path,
    trust_root: Path,
    *,
    release: str | None = None,
) -> VerifyResult:
    """The same verdict for an already-computed package digest (a directory
    package hashes as a tree, so the caller supplies the loader's digest)."""
    digest = digest.lower()
    if not sidecar_path.is_file():
        return VerifyResult("unsigned", None, "no detached signature sidecar")
    try:
        parsed = parse_sidecar(_read_small(sidecar_path, MAX_SIDECAR_BYTES, "signature sidecar"))
    except TrustError as exc:
        return VerifyResult("invalid", None, str(exc))
    if parsed.algorithm.lower() != ALGORITHM:
        return VerifyResult("unsupported", parsed.key_id, "package signature algorithm is unsupported")
    if parsed.sha256.lower() != digest or HEX_RE.fullmatch(parsed.sha256) is None:
        return VerifyResult("invalid", parsed.key_id, "signed package SHA-256 does not match content")
    try:
        revocations = load_revocations(trust_root)
    except TrustError as exc:
        return VerifyResult("invalid", parsed.key_id, f"revocation list: {exc}")
    if parsed.key_id.lower() in revocations.key_ids:
        return VerifyResult("revoked", parsed.key_id, "package signer is revoked")
    if release is not None and release.lower() in revocations.releases:
        return VerifyResult("revoked", parsed.key_id, "package release is revoked")
    try:
        key_hex = load_trusted_key(trust_root, parsed.key_id)
    except TrustError as exc:
        return VerifyResult("invalid", parsed.key_id, str(exc))
    if key_hex is None:
        return VerifyResult("untrusted", parsed.key_id,
                            "signature key is not present in the trust store")
    if verify_p256_sha256(key_hex, digest, parsed.signature):
        return VerifyResult("valid", parsed.key_id, "trusted ECDSA P-256/SHA-256 signature")
    return VerifyResult("invalid", parsed.key_id, "signature does not verify under the trusted key")
