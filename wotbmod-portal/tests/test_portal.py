"""The WotbMod portal (wotbmod-portal/), end to end over loopback HTTP.

Acceptance, in the roadmap's words:

- a user can register, find a mod and see its permissions, dependencies,
  client compatibility and hash;
- a developer can upload a versioned package but cannot replace an
  immutable artifact;
- an unverified mod is never marked verified (only a moderator does that);
- the site serves the same catalogue the client-side tools read: the
  Stage 3 CLI installs from it (`--catalog http://.../api/v1`, dependency
  first) and the legacy in-hangar registry shape (`/api/mods`) is intact.

Stdlib only: the server is `portal/wotbmod_portal.py` on 127.0.0.1:0 in a
thread, the client is http.client.
"""

from __future__ import annotations

import contextlib
import hashlib
import http.client
import importlib.util
import io
import json
import sys
import tempfile
import threading
import unittest
import urllib.parse
from pathlib import Path
from unittest import mock

PORTAL_ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = PORTAL_ROOT.parent / "mod_api"
if not (SDK_ROOT / "tools" / "wotbmod.py").is_file():  # a hosting checkout without the SDK next to it
    SDK_ROOT = PORTAL_ROOT / "backend" / "vendor"
    TOOLS_DIR = SDK_ROOT
else:
    TOOLS_DIR = SDK_ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))
sys.path.insert(0, str(PORTAL_ROOT / "backend"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
SPEC = importlib.util.spec_from_file_location("wotbmod_cli", TOOLS_DIR / "wotbmod.py")
assert SPEC is not None and SPEC.loader is not None
wotbmod = importlib.util.module_from_spec(SPEC)
sys.modules["wotbmod_cli"] = wotbmod
SPEC.loader.exec_module(wotbmod)
wotbmod.build_parser()
packages = sys.modules["wotbmod_packages"]
trust = sys.modules["wotbmod_trust"]
import portal_server as wotbmod_portal  # noqa: E402
from pe_fixture import build_pe  # noqa: E402

PORTAL_KEY = trust.generate_private_key()
DEV_KEY = trust.generate_private_key()
DEV_PUBLIC = trust.public_key_to_hex(*trust.public_key_of(DEV_KEY))
PORTAL_PUBLIC = trust.public_key_to_hex(*trust.public_key_of(PORTAL_KEY))


class Client:
    """A tiny HTTP client that remembers the session cookie and CSRF token."""

    def __init__(self, host: str, port: int) -> None:
        self.host, self.port = host, port
        self.cookie: str | None = None
        self.csrf: str | None = None
        self.bearer: str | None = None

    def request(self, method: str, path: str, *, body: bytes | None = None, content_type: str | None = None,
                headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection(self.host, self.port, timeout=30)
        request_headers = dict(headers or {})
        if content_type:
            request_headers["Content-Type"] = content_type
        if self.cookie:
            request_headers["Cookie"] = f"wotbmod_session={self.cookie}"
        if self.csrf:
            request_headers.setdefault("X-CSRF-Token", self.csrf)
        if self.bearer:
            request_headers["Authorization"] = f"Bearer {self.bearer}"
        connection.request(method, path, body=body, headers=request_headers)
        response = connection.getresponse()
        data = response.read()
        response_headers = {key.lower(): value for key, value in response.getheaders()}
        connection.close()
        return response.status, response_headers, data

    def json(self, method: str, path: str, payload: dict | None = None) -> tuple[int, dict]:
        body = json.dumps(payload or {}).encode("utf-8")
        status, _headers, data = self.request(method, path, body=body, content_type="application/json",
                                              headers={"Accept": "application/json"})
        try:
            return status, json.loads(data.decode("utf-8"))
        except ValueError:
            return status, {"raw": data.decode("utf-8", errors="replace")}

    def get(self, path: str) -> tuple[int, str]:
        status, _headers, data = self.request("GET", path)
        return status, data.decode("utf-8", errors="replace")

    def form(self, path: str, fields: dict[str, str]) -> tuple[int, dict[str, str], bytes]:
        fields = dict(fields)
        if self.csrf:
            fields.setdefault("csrf", self.csrf)
        body = urllib.parse.urlencode(fields).encode("utf-8")
        return self.request("POST", path, body=body, content_type="application/x-www-form-urlencoded")

    def login(self, email: str, password: str) -> dict:
        status, payload = self.json("POST", "/api/v1/auth/login", {"email": email, "password": password})
        assert status == 200, payload
        self.cookie = payload["session"]
        self.csrf = payload["csrf"]
        return payload

    def register(self, email: str, name: str, password: str) -> dict:
        status, payload = self.json("POST", "/api/v1/auth/register",
                                    {"email": email, "display_name": name, "password": password})
        assert status == 200, payload
        self.cookie = payload["session"]
        self.csrf = payload["csrf"]
        return payload


def make_project(root: Path, mod_id: str, version: str, *, dependencies: dict[str, str] | None = None,
                 payload: bytes | None = None, permissions: list[str] | None = None) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    manifest = {
        "manifest_version": 1, "type": "native", "id": mod_id, "name": f"Portal {mod_id}", "version": version,
        "developer": "Portal Tests", "api": {"wotbmod.core": ">=1 <2"},
        "client": {"builds": [wotbmod.EXPECTED_CLIENT_BUILD], "executable_hashes": [wotbmod.EXPECTED_CLIENT_SHA256]},
        "entrypoints": {"windows-x86": "bin/windows-x86/mod.dll"},
        "permissions": permissions or ["core", "storage", "gameplay.tweak.hud"], "resources": ["assets/**"],
    }
    if dependencies:
        manifest["dependencies"] = dependencies
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    dll = root / "bin" / "windows-x86" / "mod.dll"
    dll.parent.mkdir(parents=True, exist_ok=True)
    dll.write_bytes(payload or build_clean_pe(f"{mod_id} {version}"))
    (root / "assets").mkdir(exist_ok=True)
    (root / "assets" / "a.txt").write_bytes(b"asset\n")
    return root


def build_injector_pe() -> bytes:
    """A minimal 32-bit PE importing kernel32!CreateRemoteThread."""
    return build_pe({"kernel32.dll": ["CreateRemoteThread"]})


def build_clean_pe(tag: str) -> bytes:
    """A minimal, harmless 32-bit PE; `tag` makes each package's bytes distinct."""
    return build_pe({"kernel32.dll": ["Sleep"]}, extra=tag.encode("utf-8") + b"\0")


def cli(argv: list[str], game_root: Path | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    context = fingerprint(game_root) if game_root else contextlib.nullcontext()
    with context, contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = wotbmod.main(argv)
    return code, out.getvalue(), err.getvalue()


@contextlib.contextmanager
def fingerprint(game_root: Path):
    executable = game_root / "wotblitz.exe"
    real = wotbmod._sha256_file

    def sha256_file(path: Path) -> str:
        return wotbmod.EXPECTED_CLIENT_SHA256 if Path(path) == executable else real(Path(path))

    with (mock.patch.object(wotbmod, "_pe_architecture", return_value=wotbmod.EXPECTED_CLIENT_ARCH),
          mock.patch.object(wotbmod, "_windows_file_version", return_value=wotbmod.EXPECTED_CLIENT_BUILD),
          mock.patch.object(wotbmod, "_sha256_file", side_effect=sha256_file)):
        yield


class PortalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="wotbmod_portal_")
        cls.root = Path(cls.temporary.name)
        key_file = cls.root / "portal.key"
        key_file.write_text(f"{PORTAL_KEY:064x}\n", encoding="utf-8")
        (cls.root / "dev.key").write_text(f"{DEV_KEY:064x}\n", encoding="utf-8")
        cls.portal = wotbmod_portal.Portal(cls.root / "data", "http://127.0.0.1:0",
                                           signing_key=key_file, key_id="portal-test")
        cls.server = wotbmod_portal.make_server(cls.portal, "127.0.0.1", 0, quiet=True)
        cls.port = cls.server.server_address[1]
        cls.portal.base_url = f"http://127.0.0.1:{cls.port}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.port}"
        # The first account is the administrator (verified, no mail); both tests log in as it.
        db = cls.portal.connect()
        try:
            cls.portal.register(db, "admin@example.test", "Admin", "adminpass123")
        finally:
            db.close()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.temporary.cleanup()

    def client(self) -> Client:
        return Client("127.0.0.1", self.port)

    def release(self, project: Path, out_dir: Path) -> Path:
        code, out, err = cli(["release", str(project), "-o", str(out_dir), "--sign-with-key",
                              str(self.root / "dev.key"), "--key-id", "dev-test"])
        self.assertEqual(code, 0, err)
        manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
        return out_dir / f"{manifest['id']}-{manifest['version']}.release.json"

    def test_portal_end_to_end(self) -> None:
        admin = self.client()
        self.assertEqual(admin.login("admin@example.test", "adminpass123")["role"], "administrator")
        self.assertEqual(admin.json("GET", "/api/v1/mods")[1]["mods"], [])

        # A visitor registers, confirms the address from the outbox mail, applies as a developer.
        dev = self.client()
        dev.register("dev@example.test", "Dev", "devpass12345")
        mail = self.portal.sent_mail[-1]
        self.assertEqual(mail["to"], "dev@example.test")
        token = mail["body"].split("token=")[1].strip()
        status, payload = dev.json("POST", "/api/v1/developer/apply", {"text": "I make HUD mods"})
        self.assertEqual(status, 403, payload)  # not verified yet
        status, payload = dev.json("POST", "/api/v1/auth/verify", {"token": token})
        self.assertEqual(status, 200, payload)
        status, payload = dev.json("POST", "/api/v1/developer/apply", {"text": "I make HUD mods"})
        self.assertEqual(status, 200, payload)
        application_id = payload["application_id"]
        # A developer cannot publish before approval, and cannot approve themselves.
        status, payload = dev.json("POST", f"/api/v1/moderation/applications/{application_id}/approve")
        self.assertEqual(status, 403, payload)
        status, payload = admin.json("POST", f"/api/v1/moderation/applications/{application_id}/approve")
        self.assertEqual(status, 200, payload)
        self.assertEqual(dev.json("POST", "/api/v1/auth/login", {"email": "dev@example.test", "password": "devpass12345"})[1]["role"],
                         "developer")

        # An API token for `wotbmod publish`.
        status, headers, data = dev.request("POST", "/dashboard/tokens", body=b"name=ci&csrf=" + dev.csrf.encode(),
                                            content_type="application/x-www-form-urlencoded",
                                            headers={"Accept": "application/json"})
        self.assertEqual(status, 200, data)
        api_token = json.loads(data)["token"]

        # Publish a dependency and a dependent mod through the Stage 3 CLI.
        out_dir = self.root / "releases"
        dep_record = self.release(make_project(self.root / "src" / "dep", "portal.dep", "1.0.0"), out_dir)
        main_record = self.release(make_project(self.root / "src" / "main", "portal.main", "1.0.0",
                                                dependencies={"portal.dep": "^1.0.0"}), out_dir)
        code, out, err = cli(["publish", str(dep_record), "--to", f"{self.base}/api/v1"])
        self.assertEqual(code, 2, "no token, no publish")
        self.assertIn("401", err)
        for record in (dep_record, main_record):
            code, out, err = cli(["publish", str(record), "--to", f"{self.base}/api/v1", "--token", api_token])
            self.assertEqual(code, 0, err)
            self.assertIn("Published:", out)
        self.assertIn("review: unverified", out)

        # The catalogue index has the developer's own signature (not verified yet).
        status, index = admin.json("GET", "/api/v1/index.json")
        self.assertEqual(status, 200)
        self.assertEqual(sorted(index["packages"]), ["portal.dep", "portal.main"])
        entry = index["packages"]["portal.main"]
        self.assertFalse(entry["verified"])
        self.assertEqual(entry["latest"], "1.0.0")
        record = entry["versions"]["1.0.0"]
        self.assertEqual(record["signature"]["key_id"], "dev-test")
        self.assertEqual(record["dependencies"], {"portal.dep": "^1.0.0"})
        self.assertIn("gameplay.tweak.hud", record["permissions"])
        self.assertEqual(record["permission_tier"], "GAMEPLAY_TWEAK")

        # A visitor sees hash, permissions, dependencies and client build on the pages.
        visitor = self.client()
        status, page = visitor.get("/mods/portal.main")
        self.assertEqual(status, 200)
        self.assertIn(record["artifact"]["sha256"], page)
        self.assertIn("gameplay.tweak.hud", page)
        self.assertIn("portal.dep", page)
        self.assertIn(wotbmod.EXPECTED_CLIENT_BUILD, page)
        self.assertNotIn("проверен</span>", page)
        status, page = visitor.get("/?q=portal.main")
        self.assertIn("Portal portal.main", page)
        self.assertNotIn("Portal portal.dep", page)
        status, page = visitor.get("/install/portal.main@1.0.0")
        self.assertIn("wotbmod://install/portal.main@1.0.0?source=", page)
        self.assertIn(urllib.parse.quote(f"{self.base}/api/v1", safe=""), page)

        # The artifact is served byte-exact, downloads are counted, the sidecar verifies.
        status, headers, artifact = visitor.request("GET", "/api/v1/releases/portal.main/1.0.0/artifact")
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(artifact).hexdigest(), record["artifact"]["sha256"])
        status, headers, sidecar = visitor.request("GET", "/api/v1/releases/portal.main/1.0.0/signature")
        parsed = trust.parse_sidecar(sidecar.decode("utf-8"))
        self.assertTrue(trust.verify_p256_sha256(DEV_PUBLIC, parsed.sha256, parsed.signature))
        self.assertEqual(visitor.json("GET", "/api/v1/stats/portal.main")[1]["downloads"], 1)

        # Immutable: the same version with other bytes is refused, the same bytes are idempotent.
        changed = make_project(self.root / "src" / "main2", "portal.main", "1.0.0", dependencies={"portal.dep": "^1.0.0"},
                               payload=b"MZ other bytes\n")
        changed_record = self.release(changed, self.root / "releases2")
        code, out, err = cli(["publish", str(changed_record), "--to", f"{self.base}/api/v1", "--token", api_token])
        self.assertEqual(code, 2)
        self.assertIn("409", err)
        code, out, err = cli(["publish", str(main_record), "--to", f"{self.base}/api/v1", "--token", api_token])
        self.assertEqual(code, 0, err)
        status, headers, artifact_again = visitor.request("GET", "/api/v1/releases/portal.main/1.0.0/artifact")
        self.assertEqual(artifact_again, artifact)

        # The Stage 3 installer consumes the portal as a catalogue: dependency first.
        game = self.root / "game"
        game.mkdir()
        (game / "wotblitz.exe").write_bytes(b"MZ fake\n")
        keys = game / "mods" / "trust" / "keys"
        keys.mkdir(parents=True)
        (keys / "dev-test.p256").write_text(DEV_PUBLIC + "\n", encoding="utf-8")
        code, out, err = cli(["install", f"{self.base}/api/v1/releases/portal.main/1.0.0/artifact", "--game-root", str(game),
                              "--yes", "--catalog", f"{self.base}/api/v1"], game)
        self.assertEqual(code, 0, err)
        self.assertLess(out.index("Installed: portal.dep 1.0.0"), out.index("Installed: portal.main 1.0.0"))
        self.assertIn("Signature: valid (key dev-test)", out)
        self.assertTrue((game / "mods" / "portal.main.wotbmod.sig").is_file(), "the sidecar next to the artifact URL was fetched")
        code, out, err = cli(["update", "--all", "--game-root", str(game), "--check"], game)
        self.assertEqual(code, 0, err)
        self.assertIn("portal.main: 1.0.0 is current", out)

        # Community: rating, comment, report, compatibility declaration.
        status, payload = visitor.json("POST", "/api/v1/mods/portal.main/rating", {"stars": 5})
        self.assertEqual(status, 401)
        status, payload = admin.json("POST", "/api/v1/mods/portal.main/rating", {"stars": 4})
        self.assertEqual(status, 200, payload)
        status, payload = admin.json("POST", "/api/v1/mods/portal.main/comments", {"text": "works in 11.20"})
        self.assertEqual(status, 200, payload)
        status, payload = admin.json("POST", "/api/v1/mods/portal.main/reports", {"reason": "test report"})
        self.assertEqual(status, 200, payload)
        report_id = payload["id"]
        dev.bearer = api_token
        status, payload = dev.json("POST", "/api/v1/mods/portal.main/compat",
                                   {"version": "1.0.0", "client_build": wotbmod.EXPECTED_CLIENT_BUILD, "status": "works"})
        self.assertEqual(status, 200, payload)
        status, payload = visitor.json("GET", "/api/v1/mods/portal.main")
        self.assertEqual(payload["rating"], 4.0)
        self.assertEqual(payload["compat"][0]["status"], "works")
        status, page = visitor.get("/mods/portal.main")
        self.assertIn("works in 11.20", page)
        self.assertIn(f"{wotbmod.EXPECTED_CLIENT_BUILD}: работает", page)
        status, payload = admin.json("POST", f"/api/v1/moderation/reports/{report_id}/resolve")
        self.assertEqual(status, 200, payload)

        # Verification is a moderator's act; it flips the badge, the role and the portal signature.
        status, payload = dev.json("POST", "/api/v1/moderation/mods/portal.main/verify")
        self.assertEqual(status, 403)
        status, payload = admin.json("POST", "/api/v1/moderation/mods/portal.main/verify")
        self.assertEqual(status, 200, payload)
        self.assertTrue(admin.json("GET", "/api/v1/index.json")[1]["packages"]["portal.main"]["verified"])
        self.assertIn("проверен</span>", visitor.get("/mods/portal.main")[1])
        newer = self.release(make_project(self.root / "src" / "main11", "portal.main", "1.1.0",
                                          dependencies={"portal.dep": "^1.0.0"}), out_dir)
        code, out, err = cli(["publish", str(newer), "--to", f"{self.base}/api/v1", "--token", api_token])
        self.assertEqual(code, 0, err)
        self.assertIn("review: verified", out)
        record = admin.json("GET", "/api/v1/mods/portal.main/releases/1.1.0")[1]
        self.assertEqual(record["signature"]["key_id"], "portal-test")
        self.assertEqual(record["signature"]["signed_by"], "portal")
        status, headers, sidecar = visitor.request("GET", "/api/v1/releases/portal.main/1.1.0/signature")
        parsed = trust.parse_sidecar(sidecar.decode("utf-8"))
        self.assertTrue(trust.verify_p256_sha256(PORTAL_PUBLIC, parsed.sha256, parsed.signature))
        # With the portal key trusted, `wotbmod update` upgrades to 1.1.0 under a mandatory-signature policy.
        code, out, err = cli(["update", "portal.main", "--game-root", str(game), "--yes", "--require-signature"], game)
        self.assertEqual(code, 2, "the portal key is not trusted yet")
        self.assertIn("untrusted", err)
        (keys / "portal-test.p256").write_text(PORTAL_PUBLIC + "\n", encoding="utf-8")
        code, out, err = cli(["update", "portal.main", "--game-root", str(game), "--yes", "--require-signature"], game)
        self.assertEqual(code, 0, err)
        self.assertIn("Installed: portal.main 1.1.0", out)
        self.assertIn("Signature: valid (key portal-test)", out)
        # Two downloads of 1.1.0: the refused (untrusted) attempt fetched it too.
        self.assertEqual(visitor.json("GET", "/api/v1/stats/portal.main")[1]["versions"]["1.1.0"]["downloads"], 2)

        # The legacy in-hangar registry shape stays readable, and unlisting yields a tombstone.
        status, legacy = visitor.json("GET", "/api/mods")
        ids = {mod["id"]: mod for mod in legacy["mods"]}
        self.assertEqual(ids["portal.main"]["version"], "1.1.0")
        self.assertEqual(ids["portal.main"]["artifact"]["sha256"], record["artifact"]["sha256"])
        self.assertTrue(ids["portal.main"]["artifact"]["url"].startswith(self.base))
        self.assertEqual(legacy["removed"], [])
        status, payload = admin.json("POST", "/api/v1/moderation/mods/portal.dep/unlist")
        self.assertEqual(status, 200, payload)
        legacy = visitor.json("GET", "/api/mods")[1]
        self.assertEqual([entry["id"] for entry in legacy["removed"]], ["portal.dep"])
        self.assertNotIn("portal.dep", admin.json("GET", "/api/v1/index.json")[1]["packages"])

        # Password reset through the outbox, then the old session is gone.
        status, payload = visitor.json("POST", "/api/v1/auth/password-reset/request", {"email": "dev@example.test"})
        self.assertEqual(status, 200)
        reset_token = self.portal.sent_mail[-1]["body"].split("/reset/")[1].strip()
        status, payload = visitor.json("POST", "/api/v1/auth/password-reset/confirm",
                                       {"token": reset_token, "password": "newpass12345"})
        self.assertEqual(status, 200, payload)
        self.assertEqual(dev.json("POST", "/api/v1/auth/login", {"email": "dev@example.test", "password": "devpass12345"})[0], 401)
        self.assertEqual(dev.json("POST", "/api/v1/auth/login", {"email": "dev@example.test", "password": "newpass12345"})[0], 200)

        # Stage 7: the scan report travels with the release and shows on the page.
        record = admin.json("GET", "/api/v1/mods/portal.main/releases/1.1.0")[1]
        self.assertEqual(record["scan"]["risk"], "low")
        self.assertEqual(record["scan"]["summary"]["block"], 0)
        page = visitor.get("/mods/portal.main")[1]
        self.assertIn("низкий риск", page)
        self.assertIn("GAMEPLAY_TWEAK:", page)
        # A package with process-injection imports is refused by the portal's own scan.
        injector = self.root / "src" / "injector"
        make_project(injector, "portal.injector", "1.0.0")
        (injector / "bin" / "windows-x86" / "mod.dll").write_bytes(build_injector_pe())
        code, out, err = cli(["release", str(injector), "-o", str(self.root / "releases3"), "--allow-scan-findings"])
        self.assertEqual(code, 0, err)
        code, out, err = cli(["publish", str(self.root / "releases3" / "portal.injector-1.0.0.release.json"),
                              "--to", f"{self.base}/api/v1", "--token", api_token, "--allow-unsigned"])
        self.assertEqual(code, 2)
        self.assertIn("pe.import.inject", err)
        self.assertNotIn("portal.injector", admin.json("GET", "/api/v1/index.json")[1]["packages"])
        # Audit trail: every publish is on record, readable by moderators only.
        self.assertEqual(visitor.json("GET", "/api/v1/mods/portal.main/audit")[0], 401)
        status, payload = admin.json("GET", "/api/v1/mods/portal.main/audit")
        self.assertEqual(status, 200, payload)
        actions = [row["action"] for row in payload["audit"]]
        self.assertIn("release.publish", actions)
        self.assertIn("mod.verify", actions)
        # Crash reports from players feed the compatibility dashboard.
        status, payload = visitor.json("POST", "/api/v1/mods/portal.main/crashes",
                                       {"version": "1.1.0", "client_build": wotbmod.EXPECTED_CLIENT_BUILD, "count": 2})
        self.assertEqual(status, 200, payload)
        status, payload = visitor.json("GET", "/api/v1/compat")
        row = next(item for item in payload["mods"] if item["mod_id"] == "portal.main" and item["version"] == "1.1.0")
        self.assertEqual(row["crashes"][wotbmod.EXPECTED_CLIENT_BUILD]["crashes"], 2)
        self.assertIn("крэшей 2", visitor.get("/compat")[1])
        # The player's CLI sends what the loader's quarantine recorded.
        (game / "mods" / "cache" / "auto_disabled_mod.ini").write_bytes(b"[auto_disable]\r\nid=portal.main\r\n")
        (game / "mods" / "cache" / "crash_history.ini").write_bytes(b"[crash_history]\r\nlast_mod=portal.main\r\ncount=3\r\n")
        code, out, err = cli(["report-crash", "portal.main", "--game-root", str(game)], game)
        self.assertEqual(code, 0, err)
        self.assertIn("3 crash(es)", out)
        row = next(item for item in visitor.json("GET", "/api/v1/compat")[1]["mods"]
                   if item["mod_id"] == "portal.main" and item["version"] == "1.1.0")
        self.assertEqual(row["crashes"][wotbmod.EXPECTED_CLIENT_BUILD]["crashes"], 5)
        self.assertEqual(visitor.json("POST", "/api/v1/mods/portal.main/crashes", {"version": "x", "client_build": "y", "count": 1})[0], 400)
        # Takedown: one moderator action hides the mod, unpublishes it and closes its reports.
        status, payload = admin.json("POST", "/api/v1/mods/portal.main/reports", {"reason": "cheat"})
        self.assertEqual(status, 200, payload)
        status, payload = admin.json("POST", "/api/v1/moderation/mods/portal.main/takedown", {"reason": "confirmed report"})
        self.assertEqual(status, 200, payload)
        self.assertEqual(payload["unpublished"], 2)
        self.assertGreaterEqual(payload["reports_resolved"], 1)
        self.assertNotIn("portal.main", admin.json("GET", "/api/v1/index.json")[1]["packages"])
        self.assertIn("mod.takedown", [row["action"] for row in admin.json("GET", "/api/v1/mods/portal.main/audit")[1]["audit"]])
        self.assertEqual(sorted(entry["id"] for entry in visitor.json("GET", "/api/mods")[1]["removed"]), ["portal.dep", "portal.main"])

        # Roles: only an administrator assigns them; a moderator sees the moderation page.
        status, payload = admin.json("POST", "/api/v1/moderation/users/2/role", {"role": "moderator"})
        self.assertEqual(status, 200, payload)
        moderator = self.client()
        moderator.login("dev@example.test", "newpass12345")
        self.assertEqual(moderator.get("/moderation")[0], 200)
        self.assertEqual(moderator.json("POST", "/api/v1/moderation/users/1/role", {"role": "user"})[0], 403)

    def test_download_page_lists_bundles_and_serves_them(self) -> None:
        folder = self.portal.data_dir / "downloads"
        folder.mkdir(exist_ok=True)
        bundle = folder / "WotbMod-API-0.1.0-test-win32.zip"
        bundle.write_bytes(bytes([0x50, 0x4B, 0x05, 0x06]) + bytes(18))
        digest = hashlib.sha256(bundle.read_bytes()).hexdigest()
        with urllib.request.urlopen(self.base + "/download", timeout=10) as response:
            page = response.read().decode("utf-8")
        self.assertIn("WotbMod-API-0.1.0-test-win32.zip", page)
        self.assertIn(digest, page)
        self.assertIn("install.ps1 -RegisterLauncher", page)
        with urllib.request.urlopen(self.base + "/download/WotbMod-API-0.1.0-test-win32.zip", timeout=10) as response:
            self.assertEqual(response.headers["Content-Type"], "application/zip")
            self.assertEqual(response.read(), bundle.read_bytes())
        setup = folder / "WotbMod-Setup-0.1.0-test.exe"
        setup.write_bytes(bytes([0x4D, 0x5A]) + bytes(64))
        with urllib.request.urlopen(self.base + "/download", timeout=10) as response:
            page = response.read().decode("utf-8")
        self.assertLess(page.index("WotbMod-Setup-0.1.0-test.exe"), page.index("WotbMod-API-0.1.0-test-win32.zip"))
        with urllib.request.urlopen(self.base + "/download/WotbMod-Setup-0.1.0-test.exe", timeout=10) as response:
            self.assertEqual(response.headers["Content-Type"], "application/vnd.microsoft.portable-executable")
            self.assertEqual(response.read(), setup.read_bytes())
        with self.assertRaises(urllib.error.HTTPError) as refused:
            urllib.request.urlopen(self.base + "/download/..%2Fportal.sqlite3.zip", timeout=10)
        self.assertEqual(refused.exception.code, 404)

    def test_registration_rejects_bots_and_purges_stale_unverified_accounts(self) -> None:
        # A spam bot registered a random unverified account on the live portal
        # (2026-09-07). The honeypot field, the per-address limit and the purge of
        # stale unverified accounts are what keeps that out now.
        client = self.client()
        status, body = client.json("POST", "/api/v1/auth/register", {
            "email": "bot@example.test", "display_name": "bot", "password": "botbotbot1", "website": "http://spam"})
        self.assertEqual(status, 400, body)
        db = self.portal.connect()
        try:
            self.assertIsNone(db.execute("SELECT 1 FROM users WHERE email='bot@example.test'").fetchone())
            # An unverified account older than the TTL disappears on the next registration.
            db.execute("INSERT INTO users(email, display_name, password_hash, role, email_verified, created_at) "
                       "VALUES ('stale@example.test','stale','x','user',0,'2020-01-01T00:00:00Z')")
            db.commit()
        finally:
            db.close()
        seen = []
        for index in range(wotbmod_portal.REGISTER_LIMIT + 1):
            status, body = client.json("POST", "/api/v1/auth/register", {
                "email": f"person{index}@example.test", "display_name": f"p{index}", "password": "passpass1"})
            seen.append(status)
        # The window is per address and shared with the registrations other tests
        # already made from 127.0.0.1, so only the shape is fixed: some 200s, then 429.
        self.assertEqual(seen[0], 200, seen)
        self.assertEqual(seen[-1], 429, seen)
        self.assertEqual(seen, sorted(seen), seen)
        db = self.portal.connect()
        try:
            self.assertIsNone(db.execute("SELECT 1 FROM users WHERE email='stale@example.test'").fetchone())
        finally:
            db.close()
        # The administrator's role form preselects the current role.
        status, body = client.json("POST", "/api/v1/auth/login", {"email": "admin@example.test", "password": "adminpass123"})
        self.assertEqual(status, 200, body)
        client.cookie = body["session"]
        status, html = client.get("/moderation")
        self.assertEqual(status, 200)
        self.assertIn("<option value='administrator' selected>", html)
        # An unverified account can be removed by the administrator; verified ones cannot.
        db = self.portal.connect()
        try:
            victim = db.execute("SELECT id FROM users WHERE email='person0@example.test'").fetchone()["id"]
            db.execute("UPDATE users SET email_verified=1 WHERE id=?", (victim,))
            db.commit()
        finally:
            db.close()
        status, body = client.json("POST", f"/api/v1/moderation/users/{victim}/delete", {})
        self.assertEqual(status, 409, body)
        db = self.portal.connect()
        try:
            db.execute("UPDATE users SET email_verified=0 WHERE id=?", (victim,))
            db.commit()
        finally:
            db.close()
        status, body = client.json("POST", f"/api/v1/moderation/users/{victim}/delete", {})
        self.assertEqual(status, 200, body)
        status, html = client.get("/moderation")
        self.assertNotIn("person0@example.test", html)

    def test_beta_notice_comments_revocations_and_mail(self) -> None:
        client = self.client()
        status, body = client.json("POST", "/api/v1/auth/login", {"email": "admin@example.test", "password": "adminpass123"})
        self.assertEqual(status, 200, body)
        client.cookie = body["session"]
        # A mod to talk about: publish one as the admin (also a developer).
        out_dir = self.root / "beta_out"
        out_dir.mkdir(exist_ok=True)
        record = self.release(make_project(self.root / "src" / "beta", "portal.beta", "1.0.0"), out_dir)
        client.csrf = body["csrf"]
        status, headers, data = client.request("POST", "/dashboard/tokens", body=b"name=beta&csrf=" + client.csrf.encode(),
                                               content_type="application/x-www-form-urlencoded",
                                               headers={"Accept": "application/json"})
        self.assertEqual(status, 200, data)
        api_token = json.loads(data)["token"]
        code, out, err = cli(["publish", str(record), "--to", f"{self.base}/api/v1", "--token", api_token])
        self.assertEqual(code, 0, err)
        # Notice: shown on every page and carried in index.json.
        status, body = client.json("POST", "/api/v1/moderation/notice", {"text": "Клиент обновился, ждём переанкоровку"})
        self.assertEqual(status, 200, body)
        status, html = client.get("/docs")
        self.assertIn("ждём переанкоровку", html)
        status, index = client.json("GET", "/api/v1/index.json")
        self.assertEqual(index.get("notice"), "Клиент обновился, ждём переанкоровку")
        self.assertEqual(index["supported_client"]["build"], self.portal.client_build)
        status, body = client.json("POST", "/api/v1/moderation/notice", {"text": ""})
        status, html = client.get("/docs")
        self.assertNotIn("ждём переанкоровку", html)
        # Comments: author edits and deletes; a moderator hides someone else's.
        status, body = client.json("POST", "/api/v1/mods/portal.beta/comments", {"text": "первый отзыв"})
        self.assertEqual(status, 200, body)
        comment_id = body["id"]
        status, body = client.json("POST", f"/api/v1/mods/portal.beta/comments/{comment_id}/edit", {"text": "исправленный отзыв"})
        self.assertEqual(status, 200, body)
        status, html = client.get("/mods/portal.beta")
        self.assertIn("исправленный отзыв", html)
        self.assertIn("Удалить", html)
        status, body = client.json("POST", f"/api/v1/mods/portal.beta/comments/{comment_id}/delete", {})
        self.assertEqual(status, 200, body)
        status, html = client.get("/mods/portal.beta")
        self.assertNotIn("исправленный отзыв", html)
        # Revocation: an unpublished release lands in index.json's `revoked`.
        db = self.portal.connect()
        try:
            release_id = db.execute("SELECT id FROM releases WHERE mod_id='portal.beta'").fetchone()["id"]
        finally:
            db.close()
        mails_before = len(self.portal.sent_mail)
        status, body = client.json("POST", f"/api/v1/moderation/releases/{release_id}/unpublish", {})
        self.assertEqual(status, 200, body)
        status, index = client.json("GET", "/api/v1/index.json")
        self.assertIn({"id": "portal.beta", "version": "1.0.0", "reason": "release.unpublish"}, index["revoked"])
        # The owner is the moderator here, so no mail; a takedown by a moderator
        # of someone else's mod would mail - covered by notify_user's own guard.
        self.assertEqual(len(self.portal.sent_mail), mails_before)

    def test_html_forms_need_csrf_and_upload_validates_like_the_loader(self) -> None:
        admin = self.client()
        admin.login("admin@example.test", "adminpass123")
        csrf = admin.csrf
        admin.csrf = None  # a form without the session's token is refused
        status, headers, data = admin.request("POST", "/dashboard/mods", body=b"id=csrf.less&name=X",
                                              content_type="application/x-www-form-urlencoded")
        self.assertEqual(status, 403)
        admin.csrf = csrf
        status, headers, data = admin.form("/dashboard/mods", {"id": "admin.mod", "name": "Admin mod", "summary": "s"})
        self.assertEqual(status, 303, data)
        body, content_type = packages._multipart({"csrf": admin.csrf, "notes": "n", "publish": "1"},
                                                 {"artifact": ("bad.wotbmod", b"not a zip", "application/octet-stream")})
        status, headers, data = admin.request("POST", "/dashboard/upload", body=body, content_type=content_type,
                                              headers={"Accept": "application/json"})
        self.assertEqual(status, 400, data)
        self.assertIn("отклон", json.loads(data)["error"])


if __name__ == "__main__":
    unittest.main()
