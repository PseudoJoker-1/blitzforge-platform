"""Stage 3 of the platform roadmap: packages, dependencies, releases.

What these tests pin down, in the roadmap's own acceptance terms:

- a tampered package is refused before it reaches `mods/` (signature and
  hash checks in `wotbmod install`, the same verdict the loader reaches);
- a package for another client build, or with an unmet API/client range,
  is not installed;
- a required dependency is installed before the package that needs it,
  fetched from a catalog directory or an HTTP catalog;
- `rollback` puts the previous bytes (and the previous signature) back;
- a released version is immutable: same id@version with other bytes is
  refused by `release`, by `publish` and by `install`.

Plus the pure-Python P-256 verifier against a vector produced by the
Windows CNG signer (`tools/sign_release_artifact.ps1`) and the RFC 6979
P-256/SHA-256 test vector, and the line-preserving mods.ini editor.
Stdlib only, no network beyond a loopback http.server.
"""

from __future__ import annotations

import contextlib
import functools
import hashlib
import http.server
import importlib.util
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

SDK_ROOT = Path(__file__).resolve().parents[1]
TOOLS = SDK_ROOT / "tools"
CLI_PATH = TOOLS / "wotbmod.py"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("wotbmod_cli", CLI_PATH)
assert SPEC is not None and SPEC.loader is not None
wotbmod = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wotbmod
SPEC.loader.exec_module(wotbmod)
wotbmod.build_parser()  # registers wotbmod_packages / wotbmod_trust in sys.modules
packages = sys.modules["wotbmod_packages"]
trust = sys.modules["wotbmod_trust"]

# Produced once by tools/sign_release_artifact.ps1 (CNG, non-exportable key)
# over the bytes b"wotbmod p256 test vector\n"; the public key is the shipped
# blitzforge-preview-2026 key.
CNG_PUBLIC_KEY = (
    "19ac744b8842d446ac9b7f85f1be9944ebec295ea5bbc43adbfd874616a9daaa"
    "9e2f33dc4b733274f3b3d95a645ef40d4c60a86bbd08ac7f0702985bd1e5e56d"
)
CNG_DIGEST = "ee9953285cfc1f71ec27465916d972a61936c64612cca95c5e9cdea60f100906"
CNG_SIGNATURE = (
    "c3cee649c3bea36e4d8747d0023c8b8a2f89e2634f0a140beac81b16bacb1f00"
    "498c00418c18ef7cd14c3c78777b0b3f4054544b9e601c97eab9707df62bf3ed"
)
# RFC 6979 appendix A.2.5, P-256 with SHA-256, message "sample".
RFC_PRIVATE = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
RFC_PUBLIC = (
    "60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
    "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"
)
RFC_SIGNATURE = (
    "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
    "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"
)

KEY_ID = "tests-key"
PRIVATE_KEY = trust.generate_private_key()
PUBLIC_KEY = trust.public_key_to_hex(*trust.public_key_of(PRIVATE_KEY))


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8", newline="\n")


def make_project(root: Path, mod_id: str, version: str, *, payload: bytes | None = None,
                 dependencies: dict[str, str] | None = None,
                 optional: dict[str, str] | None = None,
                 incompatibilities: dict[str, str] | None = None,
                 client_builds: list[str] | None = None,
                 permissions: list[str] | None = None) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    manifest = {
        "manifest_version": 1,
        "type": "native",
        "id": mod_id,
        "name": f"Test {mod_id}",
        "version": version,
        "developer": "WotbMod Tests",
        "api": {"wotbmod.core": ">=1 <2"},
        "client": {
            "builds": client_builds or [wotbmod.EXPECTED_CLIENT_BUILD],
            "executable_hashes": [wotbmod.EXPECTED_CLIENT_SHA256],
        },
        "entrypoints": {"windows-x86": "bin/windows-x86/mod.dll"},
        "permissions": permissions or ["core", "storage"],
        "resources": ["assets/**"],
    }
    if dependencies:
        manifest["dependencies"] = dependencies
    if optional:
        manifest["optional_dependencies"] = optional
    if incompatibilities:
        manifest["incompatibilities"] = incompatibilities
    write_json(root / "manifest.json", manifest)
    dll = root / "bin" / "windows-x86" / "mod.dll"
    dll.parent.mkdir(parents=True, exist_ok=True)
    dll.write_bytes(payload if payload is not None else f"MZ {mod_id} {version}\n".encode())
    (root / "assets").mkdir(exist_ok=True)
    (root / "assets" / "a.txt").write_bytes(b"asset\n")
    return root


def pack(project: Path, out_dir: Path, *, client_check: bool = True) -> Path:
    manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
    out = out_dir / f"{manifest['id']}-{manifest['version']}.wotbmod"
    out_dir.mkdir(parents=True, exist_ok=True)
    argv = ["pack", str(project), "-o", str(out)]
    if not client_check:
        argv.append("--no-client-check")
    with contextlib.redirect_stdout(io.StringIO()):
        result = wotbmod.main(argv)
    assert result == 0, "pack failed"
    return out


def sign(artifact: Path, private_key: int = PRIVATE_KEY, key_id: str = KEY_ID) -> Path:
    sidecar = trust.sign_file(artifact, private_key, key_id)
    path = artifact.parent / (artifact.name + ".sig")
    path.write_text(sidecar.render(), encoding="utf-8", newline="\n")
    return path


def create_fake_game(root: Path, *, trusted: bool = True) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    (root / "wotblitz.exe").write_bytes(b"MZ fake exact-client fixture\n")
    mods = root / "mods"
    mods.mkdir(exist_ok=True)
    if trusted:
        keys = mods / "trust" / "keys"
        keys.mkdir(parents=True, exist_ok=True)
        (keys / f"{KEY_ID}.p256").write_text(PUBLIC_KEY + "\n", encoding="utf-8", newline="\n")
    return root


@contextlib.contextmanager
def matching_client_fingerprint(game_root: Path):
    executable = game_root / "wotblitz.exe"
    real_sha256_file = wotbmod._sha256_file

    def sha256_file(path: Path) -> str:
        if Path(path) == executable:
            return wotbmod.EXPECTED_CLIENT_SHA256
        return real_sha256_file(Path(path))

    with (
        mock.patch.object(wotbmod, "_pe_architecture", return_value=wotbmod.EXPECTED_CLIENT_ARCH),
        mock.patch.object(wotbmod, "_windows_file_version", return_value=wotbmod.EXPECTED_CLIENT_BUILD),
        mock.patch.object(wotbmod, "_sha256_file", side_effect=sha256_file),
    ):
        yield


def run(argv: list[str], game_root: Path | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    context = matching_client_fingerprint(game_root) if game_root else contextlib.nullcontext()
    with context, contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = wotbmod.main(argv)
    return code, out.getvalue(), err.getvalue()


def release_and_publish(project: Path, out_dir: Path, catalog: Path, *, signed: bool = True) -> Path:
    key_file = out_dir / "dev.key"
    if signed and not key_file.exists():
        out_dir.mkdir(parents=True, exist_ok=True)
        key_file.write_text(f"{PRIVATE_KEY:064x}\n", encoding="utf-8")
    argv = ["release", str(project), "-o", str(out_dir)]
    if signed:
        argv += ["--sign-with-key", str(key_file), "--key-id", KEY_ID]
    code, out, err = run(argv)
    assert code == 0, err
    manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
    record = out_dir / f"{manifest['id']}-{manifest['version']}.release.json"
    argv = ["publish", str(record), "--to", str(catalog)]
    if not signed:
        argv.append("--allow-unsigned")
    code, out, err = run(argv)
    assert code == 0, err
    return record


class TrustTests(unittest.TestCase):
    def test_cng_vector_and_rfc6979_vector(self) -> None:
        self.assertEqual(hashlib.sha256(b"wotbmod p256 test vector\n").hexdigest(), CNG_DIGEST)
        self.assertTrue(trust.verify_p256_sha256(CNG_PUBLIC_KEY, CNG_DIGEST, CNG_SIGNATURE))
        flipped = CNG_SIGNATURE[:-1] + ("0" if CNG_SIGNATURE[-1] != "0" else "1")
        self.assertFalse(trust.verify_p256_sha256(CNG_PUBLIC_KEY, CNG_DIGEST, flipped))
        self.assertFalse(trust.verify_p256_sha256(CNG_PUBLIC_KEY, "00" * 32, CNG_SIGNATURE))
        self.assertFalse(trust.verify_p256_sha256(PUBLIC_KEY, CNG_DIGEST, CNG_SIGNATURE))
        self.assertEqual(trust.public_key_to_hex(*trust.public_key_of(RFC_PRIVATE)), RFC_PUBLIC)
        digest = hashlib.sha256(b"sample").hexdigest()
        self.assertEqual(trust.sign_p256_sha256(RFC_PRIVATE, digest), RFC_SIGNATURE)
        self.assertTrue(trust.verify_p256_sha256(RFC_PUBLIC, digest, RFC_SIGNATURE))
        shipped = SDK_ROOT.parents[1] / "mods" / "trust" / "keys" / "blitzforge-preview-2026.p256"
        if shipped.is_file():
            self.assertEqual(shipped.read_text(encoding="utf-8").strip(), CNG_PUBLIC_KEY)

    def test_sidecar_parsing_follows_the_loader(self) -> None:
        good = trust.Sidecar(trust.ALGORITHM, KEY_ID, CNG_DIGEST, CNG_SIGNATURE).render()
        self.assertEqual(trust.parse_sidecar(good).key_id, KEY_ID)
        for broken in (
            good.replace("WOTBMOD-SIGNATURE-V1", "WOTBMOD-SIGNATURE-V2"),
            good + "extra=1\n",
            good + f"key_id={KEY_ID}\n",
            good.replace("algorithm=ecdsa-p256-sha256\n", ""),
            good.replace("signature=", "signature"),
        ):
            with self.assertRaises(trust.TrustError):
                trust.parse_sidecar(broken)

    def test_verify_artifact_reaches_every_status(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_trust_") as temporary:
            root = Path(temporary)
            trust_root = root / "trust"
            (trust_root / "keys").mkdir(parents=True)
            (trust_root / "keys" / f"{KEY_ID}.p256").write_text(PUBLIC_KEY + "\n", encoding="utf-8")
            artifact = root / "a.bin"
            artifact.write_bytes(b"artifact bytes\n")
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "unsigned")
            sidecar = sign(artifact)
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "valid")
            other = trust.generate_private_key()
            sign(artifact, other, "someone-else")
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "untrusted")
            sign(artifact)
            artifact.write_bytes(b"artifact bytes, tampered\n")
            result = trust.verify_artifact(artifact, trust_root)
            self.assertEqual(result.status, "invalid")
            self.assertIn("SHA-256", result.detail)
            artifact.write_bytes(b"artifact bytes\n")
            sidecar.write_text(sidecar.read_text(encoding="utf-8").replace("ecdsa-p256-sha256", "rsa"),
                               encoding="utf-8")
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "unsupported")
            sign(artifact)
            # A signed revocation list: releases and keys named in it are refused.
            revocations = trust_root / "revocations.list"
            revocations.write_text("WOTBMOD-REVOCATIONS-V1\nrevoke_release=tests.mod@1.0.0\n", encoding="utf-8")
            sign(revocations)
            self.assertEqual(trust.verify_artifact(artifact, trust_root, release="tests.mod@1.0.0").status, "revoked")
            self.assertEqual(trust.verify_artifact(artifact, trust_root, release="tests.mod@1.0.1").status, "valid")
            revocations.write_text(f"WOTBMOD-REVOCATIONS-V1\nrevoke_key={KEY_ID}\n", encoding="utf-8")
            sign(revocations)
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "invalid")  # signed by a key it revokes
            revocations.write_text("WOTBMOD-REVOCATIONS-V1\nrevoke_key=another\n", encoding="utf-8")
            sign(revocations, other, "someone-else")
            self.assertEqual(trust.verify_artifact(artifact, trust_root).status, "invalid")  # untrusted list signer


class VersionAndIniTests(unittest.TestCase):
    def test_matches_range_mirrors_the_loader(self) -> None:
        cases = [
            ("1.9.0", "^1.2.3", True), ("2.0.0", "^1.2.3", False), ("1.2.2", "^1.2.3", False),
            ("0.2.9", "^0.2.1", True), ("0.3.0", "^0.2.1", False),
            ("1.2.9", "~1.2.3", True), ("1.3.0", "~1.2.3", False),
            ("1.5.0", ">=1.0.0 <2.0.0", True), ("2.0.0", ">=1.0.0 <2.0.0", False),
            ("1.5.0", ">=1.0.0, <2.0.0", True), ("1.2.3", "1.2.3", True), ("1.2.4", "=1.2.3", False),
            ("9.9.9", "*", True), ("1.0.0-alpha", ">=1.0.0", False), ("1.0.0", ">1.0.0-alpha", True),
            ("garbage", "*", False),
        ]
        for version, range_text, expected in cases:
            self.assertEqual(packages.matches_range(version, range_text), expected, (version, range_text))
        self.assertLess(packages.version_key("1.0.0-alpha"), packages.version_key("1.0.0"))
        self.assertLess(packages.version_key("1.0.0"), packages.version_key("1.0.1"))

    def test_mods_ini_edits_preserve_other_lines_and_newlines(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_ini_") as temporary:
            path = Path(temporary) / "mods.ini"
            path.write_bytes(b"; loader settings\r\n[mods]\r\nother.mod=0\r\n\r\n[permissions]\r\nother.mod=2\r\n")
            ini = packages.ModsIni(path)
            self.assertEqual(ini.get("mods", "other.mod"), "0")
            self.assertIsNone(ini.get("mods", "tests.x"))
            ini.set("mods", "tests.x", "1")
            ini.set("permissions", "tests.x", "1")
            ini.set("policy", "require_trusted_signature", "1")
            ini.set("mods", "OTHER.MOD", "1")  # keys are case-insensitive like GetPrivateProfileInt
            ini.save()
            text = path.read_bytes().decode()
            self.assertIn("; loader settings\r\n[mods]\r\nOTHER.MOD=1\r\ntests.x=1\r\n\r\n[permissions]\r\nother.mod=2\r\ntests.x=1\r\n", text)
            self.assertTrue(text.endswith("[policy]\r\nrequire_trusted_signature=1\r\n"))
            again = packages.ModsIni(path)
            self.assertTrue(again.remove("mods", "tests.x"))
            self.assertFalse(again.remove("mods", "tests.x"))
            again.save()
            text = path.read_bytes().decode()
            self.assertIn("OTHER.MOD=1\r\n\r\n[permissions]", text)
            self.assertEqual(text.count("tests.x=1"), 1)
            self.assertIn("other.mod=2\r\ntests.x=1", text)


class InstallTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="wotbmod_pkg_")
        self.root = Path(self._temporary.name)
        self.game = create_fake_game(self.root / "game")
        self.mods = self.game / "mods"
        (self.mods / "mods.ini").write_bytes(b"[mods]\r\nother.mod=0\r\n\r\n[permissions]\r\nother.mod=2\r\n")
        self.dist = self.root / "dist"
        self.catalog = self.root / "catalog"

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def project(self, mod_id: str, version: str, **kwargs) -> Path:
        return make_project(self.root / "src" / f"{mod_id}-{version}", mod_id, version, **kwargs)

    def install(self, source: Path, *extra: str) -> tuple[int, str, str]:
        return run(["install", str(source), "--game-root", str(self.game), "--yes", *extra], self.game)

    def ledger(self) -> dict:
        return json.loads((self.mods / "cache" / packages.LEDGER_NAME).read_text(encoding="utf-8"))

    def test_install_shows_hash_and_permissions_then_installs_signed_package(self) -> None:
        artifact = pack(self.project("tests.alpha", "1.0.0"), self.dist)
        sign(artifact)
        view = wotbmod.load_package(artifact)
        self.assertEqual(view.package_sha256, hashlib.sha256(artifact.read_bytes()).hexdigest())
        code, out, err = self.install(artifact)
        self.assertEqual(code, 0, err)
        self.assertIn(f"SHA-256: {view.package_sha256}", out)
        self.assertIn("Permissions (tier", out)
        self.assertIn("Signature: valid (key tests-key)", out)
        self.assertTrue(out.index("SHA-256:") < out.index("Installed:"), "the hash is shown before the install")
        target = self.mods / "tests.alpha.wotbmod"
        self.assertEqual(target.read_bytes(), artifact.read_bytes())
        self.assertEqual((self.mods / "tests.alpha.wotbmod.sig").read_bytes(), (artifact.parent / (artifact.name + ".sig")).read_bytes())
        ini = (self.mods / "mods.ini").read_bytes().decode()
        self.assertIn("[mods]\r\nother.mod=0\r\ntests.alpha=1\r\n", ini)
        self.assertIn("[permissions]\r\nother.mod=2\r\ntests.alpha=0\r\n", ini)
        entry = self.ledger()["packages"]["tests.alpha"]
        self.assertEqual((entry["version"], entry["state"], entry["signature"]["status"]), ("1.0.0", "installed", "valid"))
        self.assertFalse(list(self.mods.glob("*.stage")))
        code, out, err = self.install(artifact)
        self.assertEqual(code, 0, err)
        self.assertIn("already installed with the same hash", out)

    def test_tampered_or_unsigned_packages_are_refused_under_policy(self) -> None:
        artifact = pack(self.project("tests.beta", "1.0.0"), self.dist)
        sidecar = sign(artifact)
        original = artifact.read_bytes()
        # Bytes changed inside the archive: the CRC check refuses it before
        # the signature is even consulted.
        artifact.write_bytes(original.replace(b"MZ tests.beta", b"MZ tests.bet4"))
        code, out, err = self.install(artifact)
        self.assertEqual(code, 2)
        self.assertIn("CRC", err)
        self.assertFalse((self.mods / "tests.beta.wotbmod").exists())
        # A well-formed archive with other bytes under the old signature: the
        # signature verdict is "invalid" (digest mismatch), as in the loader.
        swapped = pack(make_project(self.root / "src-swapped", "tests.beta", "1.0.0", payload=b"MZ swapped\n"),
                       self.dist / "swapped")
        (swapped.parent / (swapped.name + ".sig")).write_bytes(sidecar.read_bytes())
        code, out, err = self.install(swapped)
        self.assertEqual(code, 2)
        self.assertIn("signature invalid", err)
        self.assertFalse((self.mods / "tests.beta.wotbmod").exists())
        artifact.write_bytes(original)
        sidecar.unlink()
        code, out, err = self.install(artifact, "--require-signature")
        self.assertEqual(code, 2)
        self.assertIn("trusted signature is required", err)
        with (self.mods / "mods.ini").open("ab") as stream:
            stream.write(b"\r\n[policy]\r\nrequire_trusted_signature=1\r\n")
        code, out, err = self.install(artifact)
        self.assertEqual(code, 2)
        self.assertIn("policy", err)
        sign(artifact, trust.generate_private_key(), "unknown-signer")
        code, out, err = self.install(artifact)
        self.assertEqual(code, 2)
        self.assertIn("untrusted", err)
        self.assertFalse((self.mods / "tests.beta.wotbmod").exists())
        code, out, err = run(["policy", "--game-root", str(self.game), "--require-signature", "off"], self.game)
        self.assertEqual(code, 0, err)
        code, out, err = self.install(artifact)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.ledger()["packages"]["tests.beta"]["signature"]["status"], "untrusted")

    def test_incompatible_client_is_not_installed(self) -> None:
        artifact = pack(self.project("tests.gamma", "1.0.0", client_builds=["1.0.0.1"]), self.dist,
                        client_check=False)
        code, out, err = self.install(artifact)
        self.assertEqual(code, 2)
        self.assertIn("not in manifest allowlist", err)
        self.assertFalse((self.mods / "tests.gamma.wotbmod").exists())
        good = pack(self.project("tests.delta", "1.0.0"), self.dist)
        code, out, err = run(["install", str(good), "--game-root", str(self.game), "--yes"])  # no fingerprint patch
        self.assertEqual(code, 2)
        self.assertIn("client", err)
        self.assertFalse((self.mods / "tests.delta.wotbmod").exists())

    def test_required_dependency_is_installed_first_from_a_catalog(self) -> None:
        dep = self.project("tests.dep", "1.2.0")
        release_and_publish(dep, self.dist / "rel", self.catalog)
        main = pack(self.project("tests.main", "1.0.0", dependencies={"tests.dep": "^1.0.0"},
                                 optional={"tests.extra": "*"}), self.dist)
        sign(main)
        code, out, err = self.install(main)
        self.assertEqual(code, 2)
        self.assertIn("required dependency missing", err)
        self.assertIn("--catalog", err)
        self.assertFalse((self.mods / "tests.main.wotbmod").exists())
        code, out, err = self.install(main, "--catalog", str(self.catalog))
        self.assertEqual(code, 0, err)
        self.assertLess(out.index("Installed: tests.dep 1.2.0"), out.index("Installed: tests.main 1.0.0"))
        self.assertIn("recommended: tests.main recommends tests.extra *: not installed", out)
        self.assertTrue((self.mods / "tests.dep.wotbmod").is_file())
        self.assertTrue((self.mods / "tests.dep.wotbmod.sig").is_file())
        ledger = self.ledger()["packages"]
        self.assertEqual(ledger["tests.dep"]["catalog"], str(self.catalog))
        self.assertEqual(ledger["tests.dep"]["signature"]["status"], "valid")
        ini = (self.mods / "mods.ini").read_bytes().decode()
        self.assertIn("tests.dep=1", ini)
        self.assertIn("tests.main=1", ini)
        # A dependency outside the range is a refusal, not a silent mismatch.
        strict = pack(self.project("tests.strict", "1.0.0", dependencies={"tests.dep": "^2.0.0"}), self.dist)
        code, out, err = self.install(strict, "--catalog", str(self.catalog))
        self.assertEqual(code, 2)
        self.assertIn("tests.dep ^2.0.0", err)

    def test_sync_removes_revoked_releases_and_reports_the_loader_crash_record_once(self) -> None:
        release_and_publish(self.project("tests.revoked", "1.0.0"), self.dist / "rel", self.catalog)
        release_and_publish(self.project("tests.keep", "1.0.0"), self.dist / "rel", self.catalog)
        for mod_id in ("tests.revoked", "tests.keep"):
            artifact = self.catalog / f"{mod_id}-1.0.0.wotbmod"
            if not artifact.exists():
                artifact = next(self.catalog.rglob(f"{mod_id}-1.0.0.wotbmod"))
            code, out, err = self.install(artifact, "--catalog", str(self.catalog))
            self.assertEqual(code, 0, err)
        index_path = self.catalog / "index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["revoked"] = [{"id": "tests.revoked", "version": "1.0.0", "reason": "release.unpublish"}]
        index["notice"] = "Клиент обновился"
        index_path.write_text(json.dumps(index, ensure_ascii=False), encoding="utf-8")
        # --check only reports; the launcher refuses the revoked version outright.
        code, out, err = run(["sync", "--game-root", str(self.game), "--check"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("Клиент обновился", out)
        self.assertIn("tests.revoked 1.0.0: revoked by the portal (release.unpublish)", out)
        self.assertTrue((self.mods / "tests.revoked.wotbmod").is_file())
        catalog = packages.Catalog(str(self.catalog))
        self.assertEqual(catalog.is_revoked("tests.revoked", "1.0.0"), "release.unpublish")
        self.assertIsNone(catalog.is_revoked("tests.keep", "1.0.0"))
        # Telemetry off: the crash record stays local.
        (self.mods / "cache").mkdir(exist_ok=True)
        (self.mods / "cache" / "crash_history.ini").write_bytes(b"[crash_history]\r\nlast_mod=tests.keep\r\ncount=2\r\n")
        ini = self.mods / "mods.ini"
        ini.write_bytes(ini.read_bytes() + b"\r\n[telemetry]\r\ncrashes=0\r\n")
        code, out, err = run(["sync", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("crash telemetry is off", out)
        self.assertFalse((self.mods / "tests.revoked.wotbmod").exists(), "the revoked release is removed")
        self.assertTrue((self.mods / "tests.keep.wotbmod").is_file(), "the other one stays")
        self.assertFalse((self.mods / "cache" / "telemetry.json").exists())
        # Telemetry on with an HTTP portal: reported once, then remembered.
        received: list[dict] = []

        class Stub(http.server.BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                received.append({"path": self.path, "body": json.loads(self.rfile.read(length) or b"{}")})
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok": true}')

            def do_GET(self) -> None:  # noqa: N802
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"schema": 1, "packages": {}}).encode())

            def log_message(self, *args, **kwargs) -> None:
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            base = f"http://127.0.0.1:{server.server_address[1]}/api/v1"
            ledger = self.ledger()
            ledger["packages"]["tests.keep"]["catalog"] = base
            (self.mods / "cache" / packages.LEDGER_NAME).write_text(json.dumps(ledger), encoding="utf-8")
            ini.write_bytes(ini.read_bytes().replace(b"crashes=0", b"crashes=1"))
            code, out, err = run(["sync", "--game-root", str(self.game), "--yes"], self.game)
            self.assertEqual(code, 0, err)
            self.assertEqual(len(received), 1, out + err)
            self.assertEqual(received[0]["path"], "/api/v1/mods/tests.keep/crashes")
            self.assertEqual(received[0]["body"]["count"], 2)
            self.assertEqual(received[0]["body"]["version"], "1.0.0")
            code, out, err = run(["sync", "--game-root", str(self.game), "--yes"], self.game)
            self.assertEqual(len(received), 1, "the same record is not sent twice")
        finally:
            server.shutdown()

    def test_dependency_from_an_http_catalog(self) -> None:
        release_and_publish(self.project("tests.webdep", "1.0.0"), self.dist / "rel", self.catalog)
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(self.catalog))
        handler.log_message = lambda *args, **kwargs: None  # type: ignore[attr-defined]
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{server.server_address[1]}/"
            main = pack(self.project("tests.webmain", "1.0.0", dependencies={"tests.webdep": "*"}), self.dist)
            code, out, err = self.install(main, "--catalog", base)
            self.assertEqual(code, 0, err)
            self.assertLess(out.index("Installed: tests.webdep"), out.index("Installed: tests.webmain"))
            self.assertEqual(self.ledger()["packages"]["tests.webdep"]["signature"]["status"], "valid")
            # The bare artifact URL works as a source too, with its sidecar fetched next to it.
            code, out, err = self.install(base + "releases/tests.webdep/1.0.0/tests.webdep-1.0.0.wotbmod")
            self.assertEqual(code, 0, err)
            self.assertIn("already installed with the same hash", out)
        finally:
            server.shutdown()
            server.server_close()

    def test_incompatibilities_refuse_both_directions(self) -> None:
        self.assertEqual(self.install(pack(self.project("tests.a", "1.0.0", incompatibilities={"tests.b": "<2.0.0"}), self.dist))[0], 0)
        code, out, err = self.install(pack(self.project("tests.b", "1.5.0"), self.dist))
        self.assertEqual(code, 2)
        self.assertIn("incompatible", err)
        self.assertFalse((self.mods / "tests.b.wotbmod").exists())
        self.assertEqual(self.install(pack(self.project("tests.b", "2.0.0"), self.dist))[0], 0)
        code, out, err = self.install(pack(self.project("tests.c", "1.0.0", incompatibilities={"tests.a": "*"}), self.dist))
        self.assertEqual(code, 2)
        self.assertIn("incompatible with installed tests.a", err)

    def test_same_version_with_other_bytes_is_immutable(self) -> None:
        self.assertEqual(self.install(pack(self.project("tests.imm", "1.0.0"), self.dist))[0], 0)
        changed = pack(make_project(self.root / "src2", "tests.imm", "1.0.0", payload=b"MZ other bytes\n"), self.dist / "two")
        code, out, err = self.install(changed)
        self.assertEqual(code, 2)
        self.assertIn("immutable", err)
        self.assertNotEqual((self.mods / "tests.imm.wotbmod").read_bytes(), changed.read_bytes())
        code, out, err = self.install(changed, "--force")
        self.assertEqual(code, 0, err)
        self.assertEqual((self.mods / "tests.imm.wotbmod").read_bytes(), changed.read_bytes())
        older = pack(self.project("tests.imm", "0.9.0"), self.dist)
        code, out, err = self.install(older)
        self.assertEqual(code, 2)
        self.assertIn("older than installed", err)

    def test_rollback_restores_previous_bytes_and_signature(self) -> None:
        one = pack(self.project("tests.roll", "1.0.0"), self.dist)
        one_sig = sign(one)
        two = pack(self.project("tests.roll", "2.0.0"), self.dist)
        self.assertEqual(self.install(one)[0], 0)
        self.assertEqual(self.install(two)[0], 0)
        target = self.mods / "tests.roll.wotbmod"
        self.assertEqual(target.read_bytes(), two.read_bytes())
        self.assertFalse((self.mods / "tests.roll.wotbmod.sig").exists(), "an unsigned update removes the stale sidecar")
        backups = list((self.mods / "cache" / "install_backups" / "packages" / "tests.roll").glob("*.wotbmod"))
        self.assertEqual(len(backups), 1)
        code, out, err = run(["rollback", "tests.roll", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("Rollback: tests.roll 2.0.0 -> 1.0.0", out)
        self.assertEqual(target.read_bytes(), one.read_bytes())
        self.assertEqual((self.mods / "tests.roll.wotbmod.sig").read_bytes(), one_sig.read_bytes())
        self.assertEqual(self.ledger()["packages"]["tests.roll"]["version"], "1.0.0")
        code, out, err = run(["rollback", "tests.roll", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertEqual(target.read_bytes(), two.read_bytes(), "a second rollback rolls forward again")
        (self.mods / "cache" / "install_backups" / "packages" / "tests.roll").rename(self.root / "gone")
        code, out, err = run(["rollback", "tests.roll", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 2)
        self.assertIn("backup is missing", err)
        self.assertEqual(target.read_bytes(), two.read_bytes())

    def test_uninstall_respects_dependents_and_cleans_settings(self) -> None:
        release_and_publish(self.project("tests.base", "1.0.0"), self.dist / "rel", self.catalog)
        top = pack(self.project("tests.top", "1.0.0", dependencies={"tests.base": "*"}), self.dist)
        self.assertEqual(self.install(top, "--catalog", str(self.catalog))[0], 0)
        code, out, err = run(["uninstall", "tests.base", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 2)
        self.assertIn("required by tests.top", err)
        self.assertTrue((self.mods / "tests.base.wotbmod").is_file())
        quarantine = self.mods / "cache" / "auto_disabled_mod.ini"
        quarantine.write_bytes(b"[auto_disable]\r\nid=tests.top\r\n")
        code, out, err = run(["uninstall", "tests.top", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertFalse((self.mods / "tests.top.wotbmod").exists())
        ini = (self.mods / "mods.ini").read_bytes().decode()
        self.assertNotIn("tests.top", ini)
        self.assertIn("tests.base=1", ini)
        self.assertIn("other.mod=0", ini)
        self.assertNotIn("tests.top", quarantine.read_bytes().decode())
        self.assertEqual(self.ledger()["packages"]["tests.top"]["state"], "removed")
        code, out, err = run(["rollback", "tests.top", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertEqual((self.mods / "tests.top.wotbmod").read_bytes(), top.read_bytes())
        self.assertEqual(run(["uninstall", "tests.top", "--game-root", str(self.game), "--yes"], self.game)[0], 0)
        self.assertEqual(run(["uninstall", "tests.base", "--game-root", str(self.game), "--yes"], self.game)[0], 0)
        self.assertFalse((self.mods / "tests.base.wotbmod.sig").exists())

    def test_update_from_catalog(self) -> None:
        release_and_publish(self.project("tests.up", "1.0.0"), self.dist / "rel", self.catalog)
        first = self.catalog / "releases" / "tests.up" / "1.0.0" / "tests.up-1.0.0.wotbmod"
        self.assertEqual(self.install(first, "--catalog", str(self.catalog))[0], 0)
        release_and_publish(self.project("tests.up", "1.1.0"), self.dist / "rel", self.catalog)
        release_and_publish(self.project("tests.up", "2.0.0-beta.1"), self.dist / "rel", self.catalog)
        code, out, err = run(["update", "tests.up", "--game-root", str(self.game), "--check"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("tests.up: 1.0.0 -> 1.1.0", out)
        self.assertEqual((self.mods / "tests.up.wotbmod").read_bytes(), first.read_bytes())
        code, out, err = run(["update", "--all", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.ledger()["packages"]["tests.up"]["version"], "1.1.0")
        code, out, err = run(["update", "tests.up", "--game-root", str(self.game), "--yes"], self.game)
        self.assertIn("1.1.0 is current", out)
        code, out, err = run(["update", "tests.up", "--game-root", str(self.game), "--check", "--pre"], self.game)
        self.assertIn("1.1.0 -> 2.0.0-beta.1", out)

    def test_list_reports_enabled_tier_signature_and_quarantine(self) -> None:
        signed = pack(self.project("tests.l1", "1.0.0"), self.dist)
        sign(signed)
        self.assertEqual(self.install(signed)[0], 0)
        self.assertEqual(self.install(pack(self.project("tests.l2", "1.0.0"), self.dist))[0], 0)
        ini = packages.ModsIni(self.mods / "mods.ini")
        ini.set("mods", "tests.l2", "0")
        ini.save()
        (self.mods / "cache" / "auto_disabled_mod.ini").write_bytes(b"[auto_disable]\r\nid=tests.l1\r\n")
        code, out, err = run(["list", "--game-root", str(self.game), "--json"])
        self.assertEqual(code, 0, err)
        rows = {row["id"]: row for row in json.loads(out)["packages"]}
        self.assertEqual(rows["tests.l1"]["signature"], "valid")
        self.assertTrue(rows["tests.l1"]["quarantined"])
        self.assertTrue(rows["tests.l1"]["enabled"])
        self.assertEqual(rows["tests.l2"]["signature"], "unsigned")
        self.assertFalse(rows["tests.l2"]["enabled"])
        self.assertEqual(rows["tests.l2"]["granted_tier"], rows["tests.l2"]["requested_tier"])
        code, out, err = run(["list", "--game-root", str(self.game)])
        self.assertIn("quarantine: tests.l1", out)
        self.assertIn("disabled", out)


class AuditTests(unittest.TestCase):
    """Stage 6: the states a player asks about are answered by the CLI, not by reading ini files."""

    def test_quarantine_and_info_explain_the_loader_state(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_audit_") as temporary:
            root = Path(temporary)
            game = create_fake_game(root / "game")
            mods = game / "mods"
            artifact = pack(make_project(root / "src", "tests.audit", "1.0.0", dependencies={"tests.base": "*"}), root / "dist")
            sign(artifact)
            base = pack(make_project(root / "src2", "tests.base", "2.0.0"), root / "dist2")
            self.assertEqual(run(["install", str(base), "--game-root", str(game), "--yes"], game)[0], 0)
            code, out, err = run(["install", str(artifact), "--game-root", str(game), "--yes", "--catalog", str(root / "nocat")], game)
            self.assertEqual(code, 2, "a missing catalog directory is an error, not a silent fallback")
            self.assertEqual(run(["install", str(artifact), "--game-root", str(game), "--yes"], game)[0], 0)
            # The loader quarantined it after a crash loop.
            (mods / "cache" / "auto_disabled_mod.ini").write_bytes(b"[auto_disable]\r\nid=tests.audit\r\n")
            (mods / "cache" / "crash_history.ini").write_bytes(b"[crash_history]\r\nlast_mod=tests.audit\r\ncount=2\r\n")
            (game / "wotb_mod_loader.log").write_text(
                "[12:00:00.000] [warning] [loader] SAFE MODE: automatically disabled repeatedly crashing mod tests.audit; count=2\n"
                "[12:00:01.000] [info] [loader] other.mod loaded\n", encoding="utf-8")
            code, out, err = run(["quarantine", "--game-root", str(game)])
            self.assertEqual(code, 0, err)
            self.assertIn("quarantined: tests.audit", out)
            self.assertIn("--clear tests.audit", out)
            code, out, err = run(["info", "tests.audit", "--game-root", str(game), "--json"])
            self.assertEqual(code, 0, err)
            info = json.loads(out)
            self.assertTrue(info["installed"] and info["quarantined"])
            self.assertEqual((info["version"], info["crash_count"], info["signature"], info["signature_key"]), ("1.0.0", 2, "valid", KEY_ID))
            self.assertEqual(info["dependencies"], {"tests.base": "*"})
            self.assertEqual(info["granted_tier"], info["requested_tier"])
            self.assertEqual(len(info["log"]), 1)
            self.assertIn("repeatedly crashing mod tests.audit", info["log"][0])
            code, out, err = run(["info", "tests.audit", "--game-root", str(game)])
            self.assertIn("QUARANTINED", out)
            self.assertIn("SAFE MODE", out)
            code, out, err = run(["quarantine", "--game-root", str(game), "--clear", "tests.base"])
            self.assertEqual(code, 2)
            self.assertIn("not the quarantined mod", err)
            code, out, err = run(["quarantine", "--game-root", str(game), "--clear", "tests.audit", "--json"])
            self.assertEqual(code, 0, err)
            self.assertIsNone(json.loads(out)["quarantined"])
            self.assertNotIn("tests.audit", (mods / "cache" / "auto_disabled_mod.ini").read_bytes().decode())
            self.assertIn("count=0", (mods / "cache" / "crash_history.ini").read_bytes().decode())
            code, out, err = run(["info", "tests.base", "--game-root", str(game), "--json"])
            self.assertFalse(json.loads(out)["quarantined"])
            self.assertEqual(json.loads(out)["log"], [])
            # A portal catalog recorded at install time gives the mod page link.
            ledger_path = mods / "cache" / packages.LEDGER_NAME
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
            ledger["packages"]["tests.base"]["catalog"] = "https://portal.example/api/v1"
            ledger_path.write_text(json.dumps(ledger), encoding="utf-8")
            code, out, err = run(["info", "tests.base", "--game-root", str(game)])
            self.assertIn("page: https://portal.example/mods/tests.base", out)
            code, out, err = run(["info", "tests.none", "--game-root", str(game)])
            self.assertEqual(code, 0, err)
            self.assertIn("not installed", out)


class ReleaseTests(unittest.TestCase):
    def test_release_signs_records_and_is_immutable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_rel_") as temporary:
            root = Path(temporary)
            code, out, err = run(["keygen", "--out", str(root / "keys" / "dev.key"), "--key-id", "dev-key"])
            self.assertEqual(code, 0, err)
            public = (root / "keys" / "dev-key.p256").read_text(encoding="utf-8").strip()
            project = make_project(root / "proj", "tests.rel", "1.0.0")
            code, out, err = run(["release", str(project), "-o", str(root / "out"),
                                  "--sign-with-key", str(root / "keys" / "dev.key"), "--key-id", "dev-key",
                                  "--suggest", "tests.nice=^1.0.0", "--json"])
            self.assertEqual(code, 0, err)
            record = json.loads(out)
            artifact = root / "out" / "tests.rel-1.0.0.wotbmod"
            self.assertEqual(record["artifact"]["sha256"], hashlib.sha256(artifact.read_bytes()).hexdigest())
            self.assertEqual(record["signature"]["key_id"], "dev-key")
            self.assertEqual(record["signature"]["public_key"], public)
            self.assertEqual(record["suggested"], {"tests.nice": "^1.0.0"})
            sidecar = trust.parse_sidecar((root / "out" / "tests.rel-1.0.0.wotbmod.sig").read_text(encoding="utf-8"))
            self.assertTrue(trust.verify_p256_sha256(public, sidecar.sha256, sidecar.signature))
            trust_root = root / "trust"
            (trust_root / "keys").mkdir(parents=True)
            (trust_root / "keys" / "dev-key.p256").write_text(public + "\n", encoding="utf-8")
            code, out, err = run(["verify", str(artifact), "--trust-root", str(trust_root)])
            self.assertEqual(code, 0, err)
            self.assertIn("VALID", out)
            # Same version, same bytes: fine. Same version, other bytes: refused.
            self.assertEqual(run(["release", str(project), "-o", str(root / "out")])[0], 0)
            (project / "bin" / "windows-x86" / "mod.dll").write_bytes(b"MZ changed\n")
            code, out, err = run(["release", str(project), "-o", str(root / "out")])
            self.assertEqual(code, 2)
            self.assertIn("immutable", err)
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), record["artifact"]["sha256"])
            manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
            manifest["version"] = "1.0.1"
            write_json(project / "manifest.json", manifest)
            self.assertEqual(run(["release", str(project), "-o", str(root / "out")])[0], 0)
            self.assertTrue((root / "out" / "tests.rel-1.0.1.release.json").is_file())

    def test_publish_to_a_directory_catalog_is_immutable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_pub_") as temporary:
            root = Path(temporary)
            catalog = root / "catalog"
            project = make_project(root / "proj", "tests.pub", "1.0.0")
            record = release_and_publish(project, root / "out", catalog)
            index = json.loads((catalog / "index.json").read_text(encoding="utf-8"))
            entry = index["packages"]["tests.pub"]
            self.assertEqual(entry["latest"], "1.0.0")
            self.assertEqual(entry["versions"]["1.0.0"]["artifact"]["path"], "releases/tests.pub/1.0.0/tests.pub-1.0.0.wotbmod")
            self.assertEqual(entry["versions"]["1.0.0"]["signature"]["path"], "releases/tests.pub/1.0.0/tests.pub-1.0.0.wotbmod.sig")
            self.assertTrue((catalog / "releases" / "tests.pub" / "1.0.0" / "tests.pub-1.0.0.wotbmod.sig").is_file())
            self.assertEqual(run(["publish", str(record), "--to", str(catalog)])[0], 0)  # idempotent
            other = make_project(root / "proj2", "tests.pub", "1.0.0", payload=b"MZ different\n")
            code, out, err = run(["release", str(other), "-o", str(root / "out2")])
            self.assertEqual(code, 0, err)
            code, out, err = run(["publish", str(root / "out2" / "tests.pub-1.0.0.release.json"), "--to", str(catalog)])
            self.assertEqual(code, 2)
            self.assertIn("unsigned", err)
            code, out, err = run(["publish", str(root / "out2" / "tests.pub-1.0.0.release.json"), "--to", str(catalog), "--allow-unsigned"])
            self.assertEqual(code, 2)
            self.assertIn("immutable", err)
            release_and_publish(make_project(root / "proj3", "tests.pub", "1.1.0"), root / "out", catalog)
            index = json.loads((catalog / "index.json").read_text(encoding="utf-8"))
            self.assertEqual(index["packages"]["tests.pub"]["latest"], "1.1.0")
            # A record whose artifact was altered after release does not publish.
            tampered = root / "out" / "tests.pub-1.1.0.wotbmod"
            tampered.write_bytes(tampered.read_bytes() + b"\n")
            code, out, err = run(["publish", str(root / "out" / "tests.pub-1.1.0.release.json"), "--to", str(catalog)])
            self.assertEqual(code, 2)
            self.assertIn("hash does not match", err)

    def test_loader_reads_the_policy_key_the_cli_writes(self) -> None:
        source = (SDK_ROOT / "src" / "wotb_mod_runtime.cpp").read_text(encoding="utf-8", errors="replace")
        self.assertIn('"require_trusted_signature"', source)
        self.assertIn("PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE", source)



class ResourcePackageTests(unittest.TestCase):
    """A resource package replaces files under Data/ and the CLI can always
    put the stock file back: install checks the stock hash, keeps the
    original, uninstall restores it, rollback swaps versions, a foreign file
    or a second package on the same target is refused."""

    STOCK = b"// stock shader\n" + b"float4 color = tex2D(s, uv);\n" * 40
    NIGHT = b"// night shader\n" + b"float4 color = tex2D(s, uv) * 0.4;\n" * 40
    TARGET = "Materials/Shaders/debug-modify-color.slh"

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="wotbmod_res_")
        self.root = Path(self._temporary.name)
        self.game = create_fake_game(self.root / "game")
        self.mods = self.game / "mods"
        (self.mods / "mods.ini").write_bytes(b"[mods]\r\n\r\n[permissions]\r\n")
        self.dvpl = sys.modules["wotbmod_dvpl"]
        self.live = self.game / "Data" / "Materials" / "Shaders" / "debug-modify-color.slh.dvpl"
        self.live.parent.mkdir(parents=True)
        self.live.write_bytes(self.dvpl.pack(self.STOCK))
        self.dist = self.root / "dist"

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def project(self, version: str, content: bytes | None = None, mod_id: str = "tests.night") -> Path:
        content = self.NIGHT if content is None else content
        root = self.root / "src" / f"{mod_id}-{version}"
        (root / "files").mkdir(parents=True, exist_ok=True)
        (root / "files" / "shader.slh").write_bytes(content)
        write_json(root / "manifest.json", {
            "manifest_version": 1, "type": "resource", "id": mod_id, "name": "Night", "version": version,
            "developer": "Tests", "permissions": ["resources.overlay.game"],
            "files": [{"target": self.TARGET, "source": "files/shader.slh",
                       "sha256": hashlib.sha256(content).hexdigest(),
                       "stock_sha256": hashlib.sha256(self.STOCK).hexdigest()}],
        })
        return root

    def live_bytes(self) -> bytes:
        return self.dvpl.unpack(self.live.read_bytes())

    def install(self, source: Path, *extra: str) -> tuple[int, str, str]:
        return run(["install", str(source), "--game-root", str(self.game), "--yes", *extra], self.game)

    def test_dvpl_pure_python_round_trip_matches_the_extension(self) -> None:
        samples = [b"", b"x", b"abc" * 3, self.STOCK, bytes(range(256)) * 300, os.urandom(5000)]
        with mock.patch.object(self.dvpl, "_lz4", None):
            for sample in samples:
                packed = self.dvpl.pack(sample)
                self.assertEqual(self.dvpl.unpack(packed), sample)
                self.assertLess(len(self.dvpl.pack(self.STOCK)), len(self.STOCK))
            pure_block = self.dvpl.lz4_block_compress(self.STOCK)
        try:
            import lz4.block as reference  # type: ignore
        except Exception:
            reference = None
        if reference is not None:
            self.assertEqual(reference.decompress(pure_block, uncompressed_size=len(self.STOCK)), self.STOCK)
            with mock.patch.object(self.dvpl, "_lz4", None):
                decoded = self.dvpl.lz4_block_decompress(
                    reference.compress(self.STOCK, mode="high_compression", store_size=False), len(self.STOCK))
            self.assertEqual(decoded, self.STOCK)
        with self.assertRaises(self.dvpl.DvplError):
            self.dvpl.unpack(b"not a dvpl file at all")

    def test_install_replaces_the_game_file_and_uninstall_restores_it(self) -> None:
        code, out, err = self.install(self.project("1.0.0"))
        self.assertEqual(code, 0, err)
        self.assertIn("Game files replaced", out)
        self.assertIn(f"Data/{self.TARGET}", out)
        self.assertEqual(self.live_bytes(), self.NIGHT)
        pristine = self.mods / "cache" / "game_files" / "Materials__Shaders__debug-modify-color.slh.dvpl"
        self.assertEqual(self.dvpl.unpack(pristine.read_bytes()), self.STOCK)
        cached = self.mods / "cache" / "resource_packages" / "tests.night.wotbmod"
        self.assertTrue(cached.is_file())
        ledger = json.loads((self.mods / "cache" / packages.LEDGER_NAME).read_text(encoding="utf-8"))
        entry = ledger["packages"]["tests.night"]
        self.assertEqual((entry["kind"], entry["state"], entry["targets"][0]["target"]), ("resource", "installed", self.TARGET))
        self.assertNotIn("tests.night", (self.mods / "mods.ini").read_bytes().decode())
        code, out, err = run(["list", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("resource", out)
        code, out, err = self.install(self.project("1.0.0"))
        self.assertEqual(code, 0, err)
        self.assertIn("already installed with the same hash", out)
        code, out, err = run(["uninstall", "tests.night", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn(f"Restored original: Data/{self.TARGET}", out)
        self.assertEqual(self.live_bytes(), self.STOCK)
        self.assertFalse(pristine.exists())
        self.assertFalse(cached.exists())
        code, out, err = run(["list", "--game-root", str(self.game)], self.game)
        self.assertNotIn("tests.night", out)

    def test_a_foreign_file_on_disk_is_refused(self) -> None:
        self.live.write_bytes(self.dvpl.pack(b"someone else's shader"))
        code, out, err = self.install(self.project("1.0.0"))
        self.assertNotEqual(code, 0)
        self.assertIn("neither the stock file", err)
        self.assertEqual(self.live_bytes(), b"someone else's shader")

    def test_two_packages_on_one_target_conflict(self) -> None:
        code, _out, err = self.install(self.project("1.0.0"))
        self.assertEqual(code, 0, err)
        code, out, err = self.install(self.project("1.0.0", self.NIGHT + b"// other\n", mod_id="tests.other"))
        self.assertNotEqual(code, 0)
        self.assertIn("already replaced by tests.night", err)
        self.assertEqual(self.live_bytes(), self.NIGHT)

    def test_update_then_rollback_swaps_versions(self) -> None:
        second = self.NIGHT + b"// v2\n"
        code, _out, err = self.install(self.project("1.0.0"))
        self.assertEqual(code, 0, err)
        code, _out, err = self.install(self.project("1.1.0", second))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.live_bytes(), second)
        code, out, err = run(["rollback", "tests.night", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("Restored: tests.night 1.0.0", out)
        self.assertEqual(self.live_bytes(), self.NIGHT)
        code, _out, err = run(["uninstall", "tests.night", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.live_bytes(), self.STOCK)

    def test_release_and_scan_know_the_kind(self) -> None:
        key_file = self.dist / "dev.key"
        self.dist.mkdir(parents=True, exist_ok=True)
        key_file.write_text(f"{PRIVATE_KEY:064x}\n", encoding="utf-8")
        code, out, err = run(["release", str(self.project("1.0.0")), "-o", str(self.dist),
                              "--sign-with-key", str(key_file), "--key-id", KEY_ID])
        self.assertEqual(code, 0, err)
        record = json.loads((self.dist / "tests.night-1.0.0.release.json").read_text(encoding="utf-8"))
        self.assertEqual((record["type"], record["permission_tier"]), ("resource", "REVIEWED"))
        scan = sys.modules["wotbmod_scan"]
        report = scan.scan_package(self.dist / "tests.night-1.0.0.wotbmod", None)
        self.assertEqual(report.kind, "resource")
        self.assertFalse(report.blocked)
        self.assertTrue(any(f.code == "game.file" for f in report.findings))
        manifest_bad = self.project("1.0.1")
        manifest = json.loads((manifest_bad / "manifest.json").read_text(encoding="utf-8"))
        manifest["files"][0]["target"] = "../../wotblitz.exe"
        write_json(manifest_bad / "manifest.json", manifest)
        code, _out, err = run(["release", str(manifest_bad), "-o", str(self.dist)])
        self.assertNotEqual(code, 0)
        self.assertIn("target is unsafe", err)



class ImportTests(unittest.TestCase):
    """`wotbmod import` wraps an existing file-replacement mod (folder or zip
    mirroring Data/, packed or raw) into a resource-package project whose
    stock hashes come from the local game; files the stock game lacks are
    "new" entries that install creates and uninstall deletes."""

    STOCK = b"stock material " * 30
    MODDED = b"modded material " * 30

    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="wotbmod_imp_")
        self.root = Path(self._temporary.name)
        self.game = create_fake_game(self.root / "game")
        self.mods = self.game / "mods"
        (self.mods / "mods.ini").write_bytes(b"[mods]" + b"\r\n\r\n" + b"[permissions]" + b"\r\n")
        self.dvpl = sys.modules["wotbmod_dvpl"]
        data = self.game / "Data"
        (data / "Materials").mkdir(parents=True)
        (data / "Materials" / "PBR.material.dvpl").write_bytes(self.dvpl.pack(self.STOCK))
        (data / "Materials" / "same.slh.dvpl").write_bytes(self.dvpl.pack(b"unchanged"))
        # the legacy mod: Data/ layout, one packed replacement, one raw new file, one identical file, one readme
        legacy = self.root / "legacy"
        (legacy / "Data" / "Materials").mkdir(parents=True)
        (legacy / "Data" / "UI").mkdir(parents=True)
        (legacy / "Data" / "Materials" / "PBR.material.dvpl").write_bytes(self.dvpl.pack(self.MODDED))
        (legacy / "Data" / "Materials" / "same.slh.dvpl").write_bytes(self.dvpl.pack(b"unchanged"))
        (legacy / "Data" / "UI" / "extra.yaml").write_bytes(b"new: file")
        (legacy / "readme.txt").write_bytes(b"old readme")
        self.legacy = legacy

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def run_import(self, source: Path, output: Path) -> tuple[int, str, str]:
        return run(["import", str(source), "--id", "tests.imported", "--name", "Imported", "--version", "1.0.0",
                    "--developer", "Tests", "--client-build", "11.20.0.887", "-o", str(output),
                    "--game-root", str(self.game)], self.game)

    def test_import_maps_files_computes_stock_hashes_and_skips_identical(self) -> None:
        out = self.root / "project"
        code, text, err = self.run_import(self.legacy, out)
        self.assertEqual(code, 0, err)
        manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
        by_target = {item["target"]: item for item in manifest["files"]}
        self.assertEqual(set(by_target), {"Materials/PBR.material", "UI/extra.yaml"})
        self.assertEqual(by_target["Materials/PBR.material"]["stock_sha256"], hashlib.sha256(self.STOCK).hexdigest())
        self.assertEqual(by_target["Materials/PBR.material"]["sha256"], hashlib.sha256(self.MODDED).hexdigest())
        self.assertIsNone(by_target["UI/extra.yaml"]["stock_sha256"])
        self.assertEqual((out / "files" / "Materials" / "PBR.material").read_bytes(), self.MODDED)
        self.assertEqual(manifest["client"]["builds"], ["11.20.0.887"])
        self.assertIn("identical to the game's file", text)
        self.assertIn("(new file)", text)
        self.assertTrue((out / "README_RU.md").is_file())

    def test_import_from_zip_and_install_uninstall_round_trip(self) -> None:
        import shutil
        archive = shutil.make_archive(str(self.root / "legacy-mod"), "zip", root_dir=self.legacy)
        out = self.root / "project"
        code, _text, err = self.run_import(Path(archive), out)
        self.assertEqual(code, 0, err)
        code, text, err = run(["install", str(out), "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("UI/extra.yaml (new file)", text)
        live_new = self.game / "Data" / "UI" / "extra.yaml.dvpl"
        self.assertEqual(self.dvpl.unpack(live_new.read_bytes()), b"new: file")
        self.assertEqual(self.dvpl.unpack((self.game / "Data" / "Materials" / "PBR.material.dvpl").read_bytes()), self.MODDED)
        code, text, err = run(["uninstall", "tests.imported", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertFalse(live_new.exists())
        self.assertEqual(self.dvpl.unpack((self.game / "Data" / "Materials" / "PBR.material.dvpl").read_bytes()), self.STOCK)

    def test_import_refuses_when_nothing_maps(self) -> None:
        empty = self.root / "empty"
        empty.mkdir()
        (empty / "readme.md").write_bytes(b"only a readme and a patch")
        (empty / "0.diff").write_bytes(b"--- a/x +++ b/x")
        code, _text, err = self.run_import(empty, self.root / "p2")
        self.assertNotEqual(code, 0)
        self.assertIn("nothing to import", err)


if __name__ == "__main__":
    unittest.main()
