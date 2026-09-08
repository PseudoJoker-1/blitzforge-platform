"""BlitzForge portal: a self-contained mod catalogue web site (Stage 4).

    python portal/wotbmod_portal.py init         --data <dir>
    python portal/wotbmod_portal.py create-admin --data <dir> --email a@b --password ...
    python portal/wotbmod_portal.py serve        --data <dir> --port 8080 --base-url http://host:8080
                                                 [--signing-key <file> --key-id <id>] [--smtp host:port]

For a newcomer: a *mod* is a folder with `manifest.json`; a *package* is the
same mod as one `.wotbmod` file; a *release* is a package with its signature
and a record of its hash; a *catalogue* is the list of releases both this site
and the installer on the player's machine read. The portal validates every
upload the way the game's loader does (`tools/wotbmod.py` + `wotbmod_trust.py`),
keeps a released version immutable, and serves the same `index.json` shape the
`wotbmod install --catalog` command and the `wotbmod://` launcher consume.

Roles: user (rate, comment, report, apply) < developer (publish own mods)
< verified developer (badge; releases get the portal's signature when a
signing key is configured) < moderator (applications, reports, verification,
unpublish) < administrator (roles). A mod is marked verified only by a
moderator or administrator, never by its developer.

Stdlib only: http.server (threaded), sqlite3, hashlib/hmac/secrets. Email is
an outbox directory by default (`<data>/outbox/*.eml`) or SMTP with --smtp.
"""

from __future__ import annotations

import argparse
import datetime
import email.message
import hashlib
import hmac
import html
import http.cookies
import http.server
import importlib.util
import io
import json
import os
import re
import secrets
import smtplib
import sqlite3
import sys
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path
from typing import Any, Callable

BACKEND_DIR = Path(__file__).resolve().parent
PORTAL_ROOT = BACKEND_DIR.parent
FRONTEND_DIR = PORTAL_ROOT / "frontend"
TEMPLATES_DIR = FRONTEND_DIR / "templates"
STATIC_DIR = FRONTEND_DIR / "static"
# The SDK modules the portal validates packages with: the checked-out SDK next
# to this folder when it is there, otherwise the vendored copies kept in step
# by tools/sync_sdk.py (that is what a hosting upload carries).
_SDK_TOOLS = PORTAL_ROOT.parent / "mod_api" / "tools"
TOOLS = _SDK_TOOLS if (_SDK_TOOLS / "wotbmod.py").is_file() else BACKEND_DIR / "vendor"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(BACKEND_DIR))


def _load_cli():
    """tools/wotbmod.py under the name the tests use, with the package commands registered."""
    module = sys.modules.get("wotbmod_cli") or sys.modules.get("wotbmod")
    if module is None:
        spec = importlib.util.spec_from_file_location("wotbmod_cli", TOOLS / "wotbmod.py")
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        sys.modules["wotbmod_cli"] = module
        spec.loader.exec_module(module)
    module.build_parser()
    return module


wotbmod = _load_cli()
packages = sys.modules["wotbmod_packages"]
trust = sys.modules["wotbmod_trust"]
scan = sys.modules["wotbmod_scan"]

PORTAL_VERSION = "0.1.0"
ROLES = ("user", "developer", "verified_developer", "moderator", "administrator")
ROLE_RANK = {role: index for index, role in enumerate(ROLES)}
SESSION_COOKIE = "wotbmod_session"
SESSION_TTL = 14 * 24 * 3600
TOKEN_TTL = 24 * 3600
MAX_UPLOAD_BYTES = 256 * 1024 * 1024
MAX_BODY_BYTES = MAX_UPLOAD_BYTES + 1024 * 1024
MOD_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,94}$")
EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")
TIER_NAMES = packages.TIER_NAMES
SORTS = {"updated": "обновлённые", "downloads": "популярные", "rating": "рейтинг", "name": "по имени"}
LOGIN_WINDOW = 300
LOGIN_LIMIT = 20
# Registration: a spam bot registered a random unverified account on 2026-09-07.
# Three signups per address per hour, a honeypot field bots fill and people
# never see, and unverified accounts older than three days are dropped.
REGISTER_WINDOW = 3600
REGISTER_LIMIT = 3
UNVERIFIED_TTL_DAYS = 3
# A developer may create this many new releases per hour (re-uploads of the
# same bytes are free): enough for a busy release day, not for a flood.
PUBLISH_LIMIT_PER_HOUR = 10
NOTICE_MAX = 500

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, display_name TEXT NOT NULL,
    password_hash TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'user',
    email_verified INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS email_tokens (
    token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, kind TEXT NOT NULL,
    expires_at REAL NOT NULL, used INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS sessions (
    token TEXT PRIMARY KEY, user_id INTEGER NOT NULL, csrf TEXT NOT NULL, expires_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS api_tokens (
    token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL, name TEXT NOT NULL,
    created_at TEXT NOT NULL, last_used TEXT);
CREATE TABLE IF NOT EXISTS developer_applications (
    id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, text TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending', reviewed_by INTEGER, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS mods (
    id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL, name TEXT NOT NULL, summary TEXT NOT NULL DEFAULT '',
    description TEXT NOT NULL DEFAULT '', verified INTEGER NOT NULL DEFAULT 0,
    listed INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS releases (
    id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL, version TEXT NOT NULL, sha256 TEXT NOT NULL,
    size INTEGER NOT NULL, artifact_path TEXT NOT NULL, signature_path TEXT,
    record_json TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'draft', notes TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL, published_at TEXT, downloads INTEGER NOT NULL DEFAULT 0,
    updates INTEGER NOT NULL DEFAULT 0, UNIQUE(mod_id, version));
CREATE TABLE IF NOT EXISTS compat (
    id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL, version TEXT NOT NULL, client_build TEXT NOT NULL,
    status TEXT NOT NULL, declared_by INTEGER NOT NULL, created_at TEXT NOT NULL,
    UNIQUE(mod_id, version, client_build));
CREATE TABLE IF NOT EXISTS ratings (
    user_id INTEGER NOT NULL, mod_id TEXT NOT NULL, stars INTEGER NOT NULL, created_at TEXT NOT NULL,
    PRIMARY KEY(user_id, mod_id));
CREATE TABLE IF NOT EXISTS comments (
    id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL, user_id INTEGER NOT NULL, text TEXT NOT NULL,
    created_at TEXT NOT NULL, hidden INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS reports (
    id INTEGER PRIMARY KEY, target_kind TEXT NOT NULL, target_id TEXT NOT NULL, reporter_id INTEGER NOT NULL,
    reason TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'open', resolved_by INTEGER, created_at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS downloads (
    id INTEGER PRIMARY KEY, release_id INTEGER NOT NULL, kind TEXT NOT NULL, at TEXT NOT NULL,
    client_build TEXT);
CREATE TABLE IF NOT EXISTS audit (
    id INTEGER PRIMARY KEY, actor_id INTEGER, action TEXT NOT NULL, target TEXT NOT NULL, at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS crashes (
    id INTEGER PRIMARY KEY, mod_id TEXT NOT NULL, version TEXT NOT NULL, client_build TEXT NOT NULL,
    count INTEGER NOT NULL, reporter_id INTEGER, client_ip TEXT, at TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL);
"""


class HttpError(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def hash_password(password: str, salt: bytes | None = None) -> str:
    salt = salt or secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, 200_000)
    return f"pbkdf2_sha256$200000${salt.hex()}${digest.hex()}"


def check_password(password: str, stored: str) -> bool:
    try:
        _algorithm, iterations, salt_hex, digest_hex = stored.split("$")
        digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), bytes.fromhex(salt_hex), int(iterations))
        return hmac.compare_digest(digest.hex(), digest_hex)
    except (ValueError, TypeError):
        return False


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


# --- multipart ------------------------------------------------------------------------

def parse_multipart(body: bytes, content_type: str) -> tuple[dict[str, str], dict[str, tuple[str, bytes]]]:
    """Fields and files of a multipart/form-data body (cgi is gone in 3.13)."""
    match = re.search(r'boundary="?([^";]+)"?', content_type)
    if not match:
        raise HttpError(400, "multipart boundary missing")
    boundary = match.group(1).encode("utf-8")
    fields: dict[str, str] = {}
    files: dict[str, tuple[str, bytes]] = {}
    delimiter = b"--" + boundary
    parts = body.split(delimiter)
    for part in parts[1:]:
        if part.startswith(b"--"):
            break
        if part.startswith(b"\r\n"):
            part = part[2:]
        head, separator, data = part.partition(b"\r\n\r\n")
        if not separator:
            continue
        if data.endswith(b"\r\n"):
            data = data[:-2]
        disposition = ""
        for line in head.decode("utf-8", errors="replace").split("\r\n"):
            if line.lower().startswith("content-disposition:"):
                disposition = line
        name_match = re.search(r'name="([^"]*)"', disposition)
        if not name_match:
            continue
        name = name_match.group(1)
        filename_match = re.search(r'filename="([^"]*)"', disposition)
        if filename_match is not None:
            files[name] = (Path(filename_match.group(1)).name, data)
        else:
            fields[name] = data.decode("utf-8", errors="replace")
    return fields, files


# --- the application ----------------------------------------------------------------------

class Portal:
    def __init__(self, data_dir: Path, base_url: str, *, signing_key: Path | None = None,
                 key_id: str | None = None, smtp: str | None = None, client_build: str | None = None,
                 smtp_user: str | None = None, smtp_password: str | None = None,
                 mail_from: str | None = None) -> None:
        self.data_dir = data_dir
        self.base_url = base_url.rstrip("/")
        self.db_path = data_dir / "portal.sqlite3"
        self.artifacts = data_dir / "artifacts"
        self.outbox = data_dir / "outbox"
        self.smtp = smtp
        self.smtp_user = smtp_user
        self.smtp_password = smtp_password
        self.mail_from = mail_from or "portal@wotbmod.local"
        self.client_build = client_build or wotbmod.EXPECTED_CLIENT_BUILD
        self.signing_key: int | None = None
        self.key_id = key_id
        self.public_key: str | None = None
        if signing_key is not None:
            self.signing_key = packages._read_private_key(signing_key)
            if not key_id:
                raise SystemExit("--signing-key needs --key-id")
            self.public_key = trust.public_key_to_hex(*trust.public_key_of(self.signing_key))
        self.lock = threading.Lock()
        self.login_attempts: dict[str, list[float]] = {}
        self.register_attempts: dict = {}
        data_dir.mkdir(parents=True, exist_ok=True)
        self.artifacts.mkdir(exist_ok=True)
        self.outbox.mkdir(exist_ok=True)
        self.sent_mail: list[dict[str, str]] = []
        self.init_db()

    # -- database ----------------------------------------------------------------------

    def connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.db_path, timeout=10, isolation_level=None)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA foreign_keys=ON")
        return connection

    def init_db(self) -> None:
        db = self.connect()
        try:
            db.executescript(SCHEMA)
        finally:
            db.close()

    def audit(self, db: sqlite3.Connection, actor_id: int | None, action: str, target: str) -> None:
        db.execute("INSERT INTO audit(actor_id, action, target, at) VALUES (?,?,?,?)",
                   (actor_id, action, target, now_iso()))

    # -- mail --------------------------------------------------------------------------

    def get_setting(self, db: sqlite3.Connection, key: str, default: str = "") -> str:
        row = db.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
        return row["value"] if row is not None else default

    def set_setting(self, db: sqlite3.Connection, key: str, value: str) -> None:
        db.execute("INSERT INTO settings(key, value, updated_at) VALUES (?,?,?) "
                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at",
                   (key, value, now_iso()))

    def notify_user(self, db: sqlite3.Connection, user_id: int | None, subject: str, body: str) -> None:
        """Mail to an account about a moderation decision. A relay failure is logged
        into the outbox and never fails the decision itself."""
        if user_id is None:
            return
        user = db.execute("SELECT email, email_verified FROM users WHERE id=?", (user_id,)).fetchone()
        if user is None or not user["email_verified"]:
            return
        try:
            self.send_mail(user["email"], subject, body + f"\n\n{self.base_url}\n")
        except Exception as exc:  # noqa: BLE001 - the relay is outside our control
            self.outbox.mkdir(parents=True, exist_ok=True)
            (self.outbox / f"failed-{int(time.time())}.txt").write_text(
                f"To: {user['email']}\nSubject: {subject}\nError: {exc}\n\n{body}\n", encoding="utf-8")

    def send_mail(self, to: str, subject: str, body: str) -> None:
        message = email.message.EmailMessage()
        message["From"] = self.mail_from
        message["To"] = to
        message["Subject"] = subject
        message.set_content(body)
        self.sent_mail.append({"to": to, "subject": subject, "body": body})
        if self.smtp:
            # Port 465 is implicit TLS; anything else upgrades with STARTTLS when the
            # relay offers it. A user/password pair means an authenticated relay
            # such as Resend, Brevo or a mailbox provider.
            host, _, port_text = self.smtp.partition(":")
            port = int(port_text or 25)
            client_type = smtplib.SMTP_SSL if port == 465 else smtplib.SMTP
            with client_type(host, port, timeout=15) as client:
                client.ehlo()
                if port != 465 and client.has_extn("starttls"):
                    client.starttls()
                    client.ehlo()
                if self.smtp_user and self.smtp_password:
                    client.login(self.smtp_user, self.smtp_password)
                client.send_message(message)
            return
        name = f"{int(time.time() * 1000)}-{secrets.token_hex(4)}.eml"
        (self.outbox / name).write_bytes(bytes(message))

    def issue_email_token(self, db: sqlite3.Connection, user_id: int, kind: str) -> str:
        token = secrets.token_urlsafe(32)
        db.execute("INSERT INTO email_tokens(token, user_id, kind, expires_at) VALUES (?,?,?,?)",
                   (token, user_id, kind, time.time() + TOKEN_TTL))
        return token

    def consume_email_token(self, db: sqlite3.Connection, token: str, kind: str) -> int:
        row = db.execute("SELECT * FROM email_tokens WHERE token=? AND kind=?", (token, kind)).fetchone()
        if row is None or row["used"] or row["expires_at"] < time.time():
            raise HttpError(400, "ссылка недействительна или устарела")
        db.execute("UPDATE email_tokens SET used=1 WHERE token=?", (token,))
        return int(row["user_id"])

    # -- users and sessions ------------------------------------------------------------

    def purge_unverified(self, db: sqlite3.Connection) -> int:
        """Drops accounts that never confirmed their address within UNVERIFIED_TTL_DAYS
        (with their sessions and tokens); never the first account, which is created verified."""
        cutoff = (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=UNVERIFIED_TTL_DAYS)).strftime("%Y-%m-%dT%H:%M:%SZ")
        stale = [row["id"] for row in db.execute(
            "SELECT id FROM users WHERE email_verified=0 AND created_at < ? AND role='user'", (cutoff,)).fetchall()]
        for user_id in stale:
            for table in ("sessions", "email_tokens", "api_tokens"):
                db.execute(f"DELETE FROM {table} WHERE user_id=?", (user_id,))
            db.execute("DELETE FROM users WHERE id=?", (user_id,))
        return len(stale)

    def register(self, db: sqlite3.Connection, email_address: str, display_name: str, password: str,
                 client_ip: str = "", honeypot: str = "") -> int:
        if honeypot.strip():
            # A field only a bot fills; answer like a success so the bot learns nothing.
            raise HttpError(400, "некорректная почта")
        attempts = self.register_attempts.setdefault(client_ip, [])
        cutoff = time.time() - REGISTER_WINDOW
        attempts[:] = [stamp for stamp in attempts if stamp > cutoff]
        if len(attempts) >= REGISTER_LIMIT:
            raise HttpError(429, "слишком много регистраций с этого адреса; подождите час")
        self.purge_unverified(db)
        email_address = email_address.strip().lower()
        if EMAIL_RE.fullmatch(email_address) is None:
            raise HttpError(400, "некорректная почта")
        if not 1 <= len(display_name.strip()) <= 40:
            raise HttpError(400, "имя: от 1 до 40 символов")
        if len(password) < 8:
            raise HttpError(400, "пароль: минимум 8 символов")
        if db.execute("SELECT 1 FROM users WHERE email=?", (email_address,)).fetchone():
            raise HttpError(409, "такая почта уже зарегистрирована")
        first = db.execute("SELECT COUNT(*) FROM users").fetchone()[0] == 0
        cursor = db.execute(
            "INSERT INTO users(email, display_name, password_hash, role, email_verified, created_at) VALUES (?,?,?,?,?,?)",
            (email_address, display_name.strip(), hash_password(password), "administrator" if first else "user",
             1 if first else 0, now_iso()))
        user_id = int(cursor.lastrowid)
        attempts.append(time.time())
        if not first:
            token = self.issue_email_token(db, user_id, "verify")
            self.send_mail(email_address, "Подтвердите почту на BlitzForge",
                           f"Откройте ссылку, чтобы подтвердить почту:\n{self.base_url}/verify?token={token}\n")
        return user_id

    def login(self, db: sqlite3.Connection, email_address: str, password: str, client_ip: str) -> sqlite3.Row:
        attempts = self.login_attempts.setdefault(client_ip, [])
        cutoff = time.time() - LOGIN_WINDOW
        attempts[:] = [stamp for stamp in attempts if stamp > cutoff]
        if len(attempts) >= LOGIN_LIMIT:
            raise HttpError(429, "слишком много попыток входа; подождите")
        attempts.append(time.time())
        user = db.execute("SELECT * FROM users WHERE email=?", (email_address.strip().lower(),)).fetchone()
        if user is None or not check_password(password, user["password_hash"]):
            raise HttpError(401, "неверная почта или пароль")
        return user

    def open_session(self, db: sqlite3.Connection, user_id: int) -> tuple[str, str]:
        token = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(16)
        db.execute("INSERT INTO sessions(token, user_id, csrf, expires_at) VALUES (?,?,?,?)",
                   (token, user_id, csrf, time.time() + SESSION_TTL))
        return token, csrf

    def session_user(self, db: sqlite3.Connection, token: str | None) -> tuple[sqlite3.Row | None, sqlite3.Row | None]:
        if not token:
            return None, None
        session = db.execute("SELECT * FROM sessions WHERE token=?", (token,)).fetchone()
        if session is None or session["expires_at"] < time.time():
            return None, None
        user = db.execute("SELECT * FROM users WHERE id=?", (session["user_id"],)).fetchone()
        return user, session

    def bearer_user(self, db: sqlite3.Connection, token: str) -> sqlite3.Row | None:
        row = db.execute("SELECT * FROM api_tokens WHERE token_hash=?", (token_hash(token),)).fetchone()
        if row is None:
            return None
        db.execute("UPDATE api_tokens SET last_used=? WHERE token_hash=?", (now_iso(), row["token_hash"]))
        return db.execute("SELECT * FROM users WHERE id=?", (row["user_id"],)).fetchone()

    # -- releases ----------------------------------------------------------------------

    def store_release(self, db: sqlite3.Connection, user: sqlite3.Row, artifact_bytes: bytes,
                      signature_bytes: bytes | None, *, notes: str, publish: bool,
                      record: dict[str, Any] | None = None, filename: str = "upload.wotbmod") -> sqlite3.Row:
        """Validate an uploaded package like the loader would, then keep it immutably."""
        if ROLE_RANK[user["role"]] < ROLE_RANK["developer"]:
            raise HttpError(403, "публиковать могут только разработчики; подайте заявку в аккаунте")
        if len(artifact_bytes) > MAX_UPLOAD_BYTES:
            raise HttpError(413, "пакет слишком большой")
        with tempfile.TemporaryDirectory(prefix="wotbmod-portal-") as temporary:
            staged = Path(temporary) / "upload.wotbmod"
            staged.write_bytes(artifact_bytes)
            try:
                # Native, content and Lua packages alike, the way the CLI reads them
                # (a Lua manifest has no manifest_version and installs into mods/lua).
                view = packages.load_any_package(staged, client_check=False)
            except wotbmod.CliError as exc:
                raise HttpError(400, f"пакет отклонён: {exc}") from exc
            sha256 = view.package_sha256
            mod_id, version = view.manifest["id"], view.manifest["version"]
            # The portal's own scan is the one the catalogue shows; a record
            # may carry the developer's, but it is not trusted for gating.
            report = scan.scan_package(staged, view.manifest)
            if report.blocked:
                first = next(f for f in report.findings if f.severity == "block")
                raise HttpError(400, f"пакет отклонён сканером: {first.code}: {first.message} ({first.where}); "
                                     f"всего блокирующих находок: {report.counts['block']}")
            if record is not None:
                if str(record.get("id")) != mod_id or str(record.get("version")) != version:
                    raise HttpError(400, "release-запись описывает другой пакет")
                if str(record.get("artifact", {}).get("sha256", "")).lower() != sha256:
                    raise HttpError(400, "release-запись называет другой хеш")
            sidecar = None
            if signature_bytes:
                try:
                    sidecar = trust.parse_sidecar(signature_bytes.decode("utf-8", errors="replace"))
                except trust.TrustError as exc:
                    raise HttpError(400, f"подпись отклонена: {exc}") from exc
                if sidecar.sha256.lower() != sha256:
                    raise HttpError(400, "подпись покрывает другие байты")
                public_key = (record or {}).get("signature", {}) or {}
                public_key = public_key.get("public_key") if isinstance(public_key, dict) else None
                if public_key and not trust.verify_p256_sha256(public_key, sha256, sidecar.signature):
                    raise HttpError(400, "подпись не проходит проверку под ключом из release-записи")
            mod = db.execute("SELECT * FROM mods WHERE id=?", (mod_id,)).fetchone()
            if mod is None:
                db.execute("INSERT INTO mods(id, owner_id, name, summary, description, created_at, updated_at) "
                           "VALUES (?,?,?,?,?,?,?)",
                           (mod_id, user["id"], view.manifest["name"], "", "", now_iso(), now_iso()))
                mod = db.execute("SELECT * FROM mods WHERE id=?", (mod_id,)).fetchone()
            elif mod["owner_id"] != user["id"] and ROLE_RANK[user["role"]] < ROLE_RANK["moderator"]:
                raise HttpError(403, f"мод {mod_id} принадлежит другому разработчику")
            existing = db.execute("SELECT * FROM releases WHERE mod_id=? AND version=?", (mod_id, version)).fetchone()
            if existing is not None:
                if existing["sha256"] != sha256:
                    raise HttpError(409, f"{mod_id} {version} уже выпущен с другим хешем; версия неизменяема - "
                                         "поднимите номер версии")
                if publish and existing["status"] != "published":
                    self.publish_release(db, user, existing["id"])
                return db.execute("SELECT * FROM releases WHERE id=?", (existing["id"],)).fetchone()
            recent = db.execute("SELECT COUNT(*) FROM releases r JOIN mods m ON m.id=r.mod_id WHERE m.owner_id=? AND r.created_at > ?",
                                (user["id"], (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(hours=1))
                                 .strftime("%Y-%m-%dT%H:%M:%SZ"))).fetchone()[0]
            if recent >= PUBLISH_LIMIT_PER_HOUR:
                raise HttpError(429, f"не больше {PUBLISH_LIMIT_PER_HOUR} новых релизов в час; подождите")
            folder = self.artifacts / mod_id / version
            folder.mkdir(parents=True, exist_ok=True)
            artifact_name = f"{mod_id}-{version}.wotbmod"
            artifact_path = folder / artifact_name
            packages._atomic_write_bytes(artifact_path, artifact_bytes)
            signature_path = None
            if sidecar is not None:
                signature_path = folder / (artifact_name + ".sig")
                packages._atomic_write_bytes(signature_path, sidecar.render().encode("utf-8"))
            release_record = record or packages.release_record(
                view, artifact_path, sidecar, public_key=None, notes=notes, suggested={})
            release_record = dict(release_record)
            release_record["artifact"] = {**release_record.get("artifact", {}), "file": artifact_name,
                                          "sha256": sha256, "size": len(artifact_bytes)}
            release_record["permissions"] = list(view.manifest.get("permissions", []))
            release_record["permission_tier"] = view.requested_permission_tier
            release_record["dependencies"] = dict(view.manifest.get("dependencies") or {})
            release_record["optional_dependencies"] = dict(view.manifest.get("optional_dependencies") or {})
            release_record["incompatibilities"] = dict(view.manifest.get("incompatibilities") or {})
            release_record["client"] = dict(view.manifest.get("client", {}))
            release_record["api"] = dict(view.manifest.get("api", {}))
            release_record["type"] = view.manifest["type"]
            release_record["name"] = view.manifest["name"]
            release_record["developer"] = view.manifest["developer"]
            release_record["notes"] = notes or str(release_record.get("notes") or "")
            if sidecar is not None:
                release_record["signature"] = {**(release_record.get("signature") or {}),
                                               "file": artifact_name + ".sig", "algorithm": sidecar.algorithm,
                                               "key_id": sidecar.key_id}
            else:
                release_record["signature"] = None
            release_record["scan"] = report.to_dict()
            cursor = db.execute(
                "INSERT INTO releases(mod_id, version, sha256, size, artifact_path, signature_path, record_json, "
                "status, notes, created_at) VALUES (?,?,?,?,?,?,?,?,?,?)",
                (mod_id, version, sha256, len(artifact_bytes), str(artifact_path.relative_to(self.artifacts)),
                 str(signature_path.relative_to(self.artifacts)) if signature_path else None,
                 json.dumps(release_record, ensure_ascii=False, sort_keys=True), "draft", notes or "", now_iso()))
            release_id = int(cursor.lastrowid)
            db.execute("UPDATE mods SET name=?, updated_at=? WHERE id=?", (view.manifest["name"], now_iso(), mod_id))
            self.audit(db, user["id"], "release.upload", f"{mod_id}@{version}")
            if publish:
                self.publish_release(db, user, release_id)
            return db.execute("SELECT * FROM releases WHERE id=?", (release_id,)).fetchone()

    def publish_release(self, db: sqlite3.Connection, user: sqlite3.Row, release_id: int) -> None:
        release = db.execute("SELECT * FROM releases WHERE id=?", (release_id,)).fetchone()
        if release is None:
            raise HttpError(404, "релиз не найден")
        mod = db.execute("SELECT * FROM mods WHERE id=?", (release["mod_id"],)).fetchone()
        if mod["owner_id"] != user["id"] and ROLE_RANK[user["role"]] < ROLE_RANK["moderator"]:
            raise HttpError(403, "не ваш мод")
        record = json.loads(release["record_json"])
        signature_path = release["signature_path"]
        # A verified developer's release carries the portal's own signature when
        # a signing key is configured: players trust one portal key rather than
        # every developer key.
        owner = db.execute("SELECT * FROM users WHERE id=?", (mod["owner_id"],)).fetchone()
        if self.signing_key is not None and self.key_id and \
                ROLE_RANK[owner["role"]] >= ROLE_RANK["verified_developer"]:
            artifact = self.artifacts / release["artifact_path"]
            sidecar = trust.sign_file(artifact, self.signing_key, self.key_id)
            target = artifact.parent / (artifact.name + ".sig")
            packages._atomic_write_bytes(target, sidecar.render().encode("utf-8"))
            signature_path = str(target.relative_to(self.artifacts))
            record["signature"] = {"file": target.name, "algorithm": sidecar.algorithm,
                                   "key_id": self.key_id, "public_key": self.public_key, "signed_by": "portal"}
        record["status"] = "published"
        record["published_at"] = now_iso()
        db.execute("UPDATE releases SET status='published', published_at=?, signature_path=?, record_json=? WHERE id=?",
                   (record["published_at"], signature_path, json.dumps(record, ensure_ascii=False, sort_keys=True),
                    release_id))
        db.execute("UPDATE mods SET updated_at=? WHERE id=?", (now_iso(), release["mod_id"]))
        self.audit(db, user["id"], "release.publish", f"{release['mod_id']}@{release['version']}")

    def unpublish_release(self, db: sqlite3.Connection, user: sqlite3.Row, release_id: int) -> None:
        release = db.execute("SELECT * FROM releases WHERE id=?", (release_id,)).fetchone()
        if release is None:
            raise HttpError(404, "релиз не найден")
        mod = db.execute("SELECT * FROM mods WHERE id=?", (release["mod_id"],)).fetchone()
        if mod["owner_id"] != user["id"] and ROLE_RANK[user["role"]] < ROLE_RANK["moderator"]:
            raise HttpError(403, "не ваш мод")
        record = json.loads(release["record_json"])
        record["status"] = "unpublished"
        db.execute("UPDATE releases SET status='unpublished', record_json=? WHERE id=?",
                   (json.dumps(record, ensure_ascii=False, sort_keys=True), release_id))
        self.audit(db, user["id"], "release.unpublish", f"{release['mod_id']}@{release['version']}")

    def takedown(self, db: sqlite3.Connection, user: sqlite3.Row, mod_id: str, reason: str) -> dict[str, Any]:
        """One moderator action: unlist the mod, unpublish every release, resolve its open reports."""
        mod = db.execute("SELECT * FROM mods WHERE id=?", (mod_id,)).fetchone()
        if mod is None:
            raise HttpError(404, "мод не найден")
        unpublished = 0
        for release in db.execute("SELECT id FROM releases WHERE mod_id=? AND status='published'", (mod_id,)).fetchall():
            self.unpublish_release(db, user, release["id"])
            unpublished += 1
        db.execute("UPDATE mods SET listed=0, verified=0, updated_at=? WHERE id=?", (now_iso(), mod_id))
        resolved = db.execute("UPDATE reports SET status='resolved', resolved_by=? WHERE status='open' AND "
                              "((target_kind='mod' AND target_id=?) OR (target_kind='release' AND target_id LIKE ?))",
                              (user["id"], mod_id, mod_id + "@%")).rowcount
        self.audit(db, user["id"], "mod.takedown", f"{mod_id}: {reason[:200]}")
        self.notify_user(db, mod["owner_id"], f"BlitzForge: мод {mod_id} снят из каталога",
                         f"Модератор снял ваш мод {mod_id} из каталога и отозвал его релизы ({unpublished}).\n"
                         f"Причина: {reason[:500]}\nЕсли вы не согласны, ответьте на это письмо.")
        return {"ok": True, "id": mod_id, "unpublished": unpublished, "reports_resolved": resolved}

    def audit_rows(self, db: sqlite3.Connection, mod_id: str | None, limit: int = 200) -> list[dict[str, Any]]:
        if mod_id is None:
            rows = db.execute("SELECT a.*, u.display_name FROM audit a LEFT JOIN users u ON u.id=a.actor_id "
                              "ORDER BY a.id DESC LIMIT ?", (limit,)).fetchall()
        else:
            rows = db.execute("SELECT a.*, u.display_name FROM audit a LEFT JOIN users u ON u.id=a.actor_id "
                              "WHERE a.target=? OR a.target LIKE ? OR a.target LIKE ? ORDER BY a.id DESC LIMIT ?",
                              (mod_id, mod_id + "@%", mod_id + ": %", limit)).fetchall()
        return [{"id": r["id"], "at": r["at"], "action": r["action"], "target": r["target"],
                 "actor_id": r["actor_id"], "actor": r["display_name"]} for r in rows]

    def compat_overview(self, db: sqlite3.Connection) -> list[dict[str, Any]]:
        """Per mod and version: declared compatibility per client build, and crash reports."""
        overview: dict[tuple[str, str], dict[str, Any]] = {}
        for row in db.execute("SELECT c.*, u.display_name FROM compat c JOIN users u ON u.id=c.declared_by").fetchall():
            entry = overview.setdefault((row["mod_id"], row["version"]), {"mod_id": row["mod_id"], "version": row["version"],
                                                                          "compat": {}, "crashes": {}})
            entry["compat"][row["client_build"]] = {"status": row["status"], "by": row["display_name"], "at": row["created_at"]}
        for row in db.execute("SELECT mod_id, version, client_build, SUM(count) AS total, COUNT(*) AS reports FROM crashes "
                              "GROUP BY mod_id, version, client_build").fetchall():
            entry = overview.setdefault((row["mod_id"], row["version"]), {"mod_id": row["mod_id"], "version": row["version"],
                                                                          "compat": {}, "crashes": {}})
            entry["crashes"][row["client_build"]] = {"crashes": row["total"], "reports": row["reports"]}
        return sorted(overview.values(), key=lambda item: (item["mod_id"], packages.version_key(item["version"])))

    # -- catalogue views ---------------------------------------------------------------

    def release_url(self, mod_id: str, version: str, what: str) -> str:
        return f"{self.base_url}/api/v1/releases/{urllib.parse.quote(mod_id)}/{urllib.parse.quote(version)}/{what}"

    def public_record(self, release: sqlite3.Row) -> dict[str, Any]:
        record = json.loads(release["record_json"])
        record["artifact"] = {**record.get("artifact", {}), "url": self.release_url(release["mod_id"], release["version"], "artifact")}
        if record.get("signature"):
            record["signature"] = {**record["signature"], "url": self.release_url(release["mod_id"], release["version"], "signature")}
        record["downloads"] = release["downloads"]
        record["status"] = release["status"]
        return record

    def index(self, db: sqlite3.Connection) -> dict[str, Any]:
        result: dict[str, Any] = {"schema": packages.INDEX_SCHEMA, "generated_at": now_iso(),
                                  "client_build": self.client_build, "packages": {}}
        rows = db.execute("SELECT r.*, m.name AS mod_name, m.verified, m.listed FROM releases r JOIN mods m ON m.id=r.mod_id "
                          "WHERE r.status='published' AND m.listed=1 ORDER BY r.mod_id").fetchall()
        for release in rows:
            entry = result["packages"].setdefault(release["mod_id"], {
                "name": release["mod_name"], "developer": json.loads(release["record_json"]).get("developer", ""),
                "verified": bool(release["verified"]), "versions": {}})
            entry["versions"][release["version"]] = self.public_record(release)
        for entry in result["packages"].values():
            entry["latest"] = max(entry["versions"], key=packages.version_key)
        # What the client-side `wotbmod sync` acts on: releases pulled by their
        # author or a moderator, and whole mods taken down. `reason` is the
        # audit vocabulary, so a player sees the same word the log shows.
        revoked: list[dict[str, str]] = []
        for row in db.execute("SELECT r.mod_id, r.version, m.listed FROM releases r JOIN mods m ON m.id=r.mod_id "
                              "WHERE r.status='unpublished' ORDER BY r.mod_id, r.id").fetchall():
            revoked.append({"id": row["mod_id"], "version": row["version"],
                            "reason": "mod.takedown" if not row["listed"] else "release.unpublish"})
        for row in db.execute("SELECT id FROM mods WHERE listed=0 ORDER BY id").fetchall():
            revoked.append({"id": row["id"], "version": "*", "reason": "mod.takedown"})
        result["revoked"] = revoked
        result["supported_client"] = {"build": self.client_build}
        notice = self.get_setting(db, "notice")
        if notice:
            result["notice"] = notice
        return result

    def legacy_mods(self, db: sqlite3.Connection) -> dict[str, Any]:
        """The legacy /api/mods shape kept for old clients; the in-game catalogue reads /api/v1/index.json."""
        mods = []
        for mod_id, entry in self.index(db)["packages"].items():
            latest = entry["versions"][entry["latest"]]
            mods.append({
                "id": mod_id, "name": entry["name"], "version": entry["latest"],
                "author": entry["developer"], "description": latest.get("notes", "")[:200],
                "long": latest.get("notes", ""), "type": latest.get("type", ""),
                "downloads": str(sum(v.get("downloads", 0) for v in entry["versions"].values())),
                "updated": latest.get("published_at", ""),
                "artifact": {"url": latest["artifact"]["url"], "sha256": latest["artifact"]["sha256"]},
            })
        removed = [{"id": row["id"], "deleted": row["updated_at"]}
                   for row in db.execute("SELECT id, updated_at FROM mods WHERE listed=0").fetchall()]
        return {"mods": mods, "removed": removed}

    def mod_summary(self, db: sqlite3.Connection, mod: sqlite3.Row) -> dict[str, Any] | None:
        latest = db.execute("SELECT * FROM releases WHERE mod_id=? AND status='published'", (mod["id"],)).fetchall()
        if not latest:
            return None
        release = max(latest, key=lambda row: packages.version_key(row["version"]))
        record = json.loads(release["record_json"])
        rating = db.execute("SELECT AVG(stars) AS avg, COUNT(*) AS votes FROM ratings WHERE mod_id=?", (mod["id"],)).fetchone()
        downloads = db.execute("SELECT SUM(downloads) FROM releases WHERE mod_id=?", (mod["id"],)).fetchone()[0] or 0
        owner = db.execute("SELECT display_name FROM users WHERE id=?", (mod["owner_id"],)).fetchone()
        return {
            "id": mod["id"], "name": mod["name"], "summary": mod["summary"], "description": mod["description"],
            "developer": owner["display_name"] if owner else record.get("developer", ""),
            "owner_id": mod["owner_id"], "verified": bool(mod["verified"]),
            "version": release["version"], "type": record.get("type", ""), "tier": record.get("permission_tier", "SAFE"),
            "rating": round(rating["avg"], 1) if rating["avg"] else 0.0, "votes": rating["votes"],
            "downloads": downloads, "updated_at": release["published_at"] or "", "release": release, "record": record,
        }

    def catalogue(self, db: sqlite3.Connection, *, q: str = "", type_filter: str = "", tier: str = "",
                  verified_only: bool = False, sort: str = "updated") -> list[dict[str, Any]]:
        items = []
        for mod in db.execute("SELECT * FROM mods WHERE listed=1").fetchall():
            summary = self.mod_summary(db, mod)
            if summary is None:
                continue
            haystack = " ".join([summary["id"], summary["name"], summary["summary"], summary["developer"]]).lower()
            if q and q.lower() not in haystack:
                continue
            if type_filter and summary["type"] != type_filter:
                continue
            if tier and TIER_NAMES.index(summary["tier"]) > TIER_NAMES.index(tier):
                continue
            if verified_only and not summary["verified"]:
                continue
            items.append(summary)
        keys: dict[str, Callable[[dict[str, Any]], Any]] = {
            "updated": lambda item: item["updated_at"], "downloads": lambda item: item["downloads"],
            "rating": lambda item: (item["rating"], item["votes"]), "name": lambda item: item["name"].lower(),
        }
        reverse = sort != "name"
        items.sort(key=keys.get(sort, keys["updated"]), reverse=reverse)
        return items


# --- HTTP layer -----------------------------------------------------------------------------

def esc(value: Any) -> str:
    return html.escape(str(value), quote=True)


_TEMPLATE_CACHE: dict[str, tuple[float, str]] = {}


def load_template(name: str) -> str:
    """frontend/templates/<name>.html, re-read when the file changes (edit and refresh)."""
    path = TEMPLATES_DIR / f"{name}.html"
    try:
        stamp = path.stat().st_mtime
    except OSError as exc:
        raise HttpError(500, f"template missing: {name}") from exc
    cached = _TEMPLATE_CACHE.get(name)
    if cached is None or cached[0] != stamp:
        cached = (stamp, path.read_text(encoding="utf-8"))
        _TEMPLATE_CACHE[name] = cached
    return cached[1]


def render(template_name: str, /, **values: Any) -> str:
    """`{{name}}` is HTML-escaped, `{{{name}}}` is HTML the server built itself."""
    template = load_template(template_name)

    def raw(match: re.Match[str]) -> str:
        return str(values.get(match.group(1), ""))

    def escaped(match: re.Match[str]) -> str:
        return esc(values.get(match.group(1), ""))

    template = re.sub(r"\{\{\{(\w+)\}\}\}", raw, template)
    return re.sub(r"\{\{(\w+)\}\}", escaped, template)


ROLE_LABELS = {"user": "пользователь", "developer": "разработчик", "verified_developer": "проверенный разработчик",
               "moderator": "модератор", "administrator": "администратор"}
TYPE_LABELS = {"native": "нативный мод", "content": "ресурсы", "lua": "Lua-мод", "resource": "файлы игры"}
TIER_LABELS = {"SAFE": "безопасные права", "GAMEPLAY_TWEAK": "HUD и камера", "REVIEWED": "нужна проверка", "UNSAFE": "полный доступ"}
TIER_EXPLAIN = {"SAFE": "только собственные данные мода", "GAMEPLAY_TWEAK": "HUD, камера или вид техники на этом клиенте",
                "REVIEWED": "сеть, ресурсы игры или чужие контролы", "UNSAFE": "полный доступ к процессу игры"}
STATIC_TYPES = {".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".svg": "image/svg+xml",
                ".png": "image/png", ".woff2": "font/woff2", ".ico": "image/x-icon", ".webmanifest": "application/manifest+json"}


_DOWNLOAD_DIGESTS: dict[tuple[str, int, int], str] = {}


def download_digest(path: Path) -> str:
    """SHA-256 of a bundle, hashed once per (name, size, mtime)."""
    stat = path.stat()
    key = (path.name, stat.st_size, int(stat.st_mtime))
    digest = _DOWNLOAD_DIGESTS.get(key)
    if digest is None:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        _DOWNLOAD_DIGESTS.clear()
        _DOWNLOAD_DIGESTS[key] = digest
    return digest


def hue_of(mod_id: str) -> int:
    value = 0
    for character in mod_id:
        value = (value * 31 + ord(character)) % 360
    return value


def initials_of(name: str, mod_id: str = "") -> str:
    base = re.sub(r"^[A-Za-z0-9_-]+\.", "", name or mod_id)
    parts = [part for part in re.split(r"[\s._-]+", base) if part]
    text = "".join(part[0] for part in parts[:2]) or (name or mod_id)[:2]
    return text.upper()


def plural(count: int, one: str, few: str, many: str) -> str:
    if count % 10 == 1 and count % 100 != 11:
        return one
    if 2 <= count % 10 <= 4 and not 12 <= count % 100 <= 14:
        return few
    return many


def rating_text(rating: float, votes: int) -> str:
    if not votes:
        return "нет оценок"
    return f"{rating} из 5, {votes} {plural(votes, 'оценка', 'оценки', 'оценок')}"


def rating_html(rating: float, votes: int) -> str:
    return esc(rating_text(rating, votes))


def downloads_text(count: int) -> str:
    return f"{count} {plural(count, 'загрузка', 'загрузки', 'загрузок')}"


def tier_chip(name: str) -> str:
    return f"<span class='tier-label' title='{esc(name)}'>{esc(TIER_LABELS.get(name, name))}</span>"


def format_size(size: int) -> str:
    if size < 1024:
        return f"{size} Б"
    if size < 1024 * 1024:
        return f"{size / 1024:.0f} КБ"
    return f"{size / (1024 * 1024):.1f} МБ"


def asset_version() -> str:
    """Changes whenever app.css or app.js change, so browsers fetch the new file after a deploy."""
    stamps = []
    for name in ("app.css", "app.js"):
        try:
            stamps.append(int((STATIC_DIR / name).stat().st_mtime))
        except OSError:
            pass
    return str(max(stamps) if stamps else 0)


def tier_index(name: str) -> int:
    return TIER_NAMES.index(name) if name in TIER_NAMES else 0


def card_html(item: dict[str, Any]) -> str:
    """One catalogue row; app.js renders the same shape for live search."""
    mod_id = item["id"]
    href = f"/mods/{esc(mod_id)}"
    verified = "<span class='verified'>проверен</span>" if item.get("verified") else ""
    summary = f". {esc(item['summary'])}" if item.get("summary") else ""
    tier = item.get("tier", "SAFE")
    return (f"<li class='row'><a class='mark' href='{href}' aria-hidden='true' tabindex='-1'>{esc(initials_of(item['name'], mod_id))}</a>"
            f"<div><div class='name'><a href='{href}'>{esc(item['name'])}</a>{verified}</div>"
            f"<div class='sub'>{esc(item['developer'])}{summary}</div></div>"
            f"<div class='meta'><b>{esc(item['version'])}</b>{rating_html(item['rating'], item['votes'])}, {downloads_text(item['downloads'])}<br>"
            f"{tier_chip(tier)}</div></li>")


def permission_html(record: dict[str, Any]) -> str:
    """Permissions grouped by tier, highest first: one line per group, codes in mono."""
    permissions = list(record.get("permissions", []))
    if not permissions:
        return "<p class='muted'>Мод не просит никаких прав.</p>"
    groups: dict[int, list[str]] = {}
    for permission in permissions:
        groups.setdefault(wotbmod._permission_tier(permission), []).append(permission)
    blocks = []
    for tier in sorted(groups, reverse=True):
        name = TIER_NAMES[tier]
        codes = ", ".join(esc(code) for code in sorted(groups[tier]))
        blocks.append(f"<div class='perm-group'><div class='k'>{esc(TIER_LABELS[name])}<span>{esc(name)}: {esc(TIER_EXPLAIN[name])}</span></div>"
                      f"<code>{codes}</code></div>")
    return "<div class='perm-groups'>" + "".join(blocks) + "</div>"


def deps_html(items: dict[str, str]) -> str:
    if not items:
        return "<span class='muted'>нет</span>"
    return ", ".join(f"<a href='/mods/{esc(key)}'>{esc(key)}</a> <code>{esc(value)}</code>" for key, value in items.items())


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "BlitzForgePortal/" + PORTAL_VERSION
    portal: Portal  # set on the server class

    # -- plumbing ---------------------------------------------------------------------

    def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
        if getattr(self.server, "quiet", False):
            return
        super().log_message(format, *args)

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY_BYTES:
            raise HttpError(413, "тело запроса слишком большое")
        return self.rfile.read(length) if length else b""

    def _form(self) -> tuple[dict[str, str], dict[str, tuple[str, bytes]]]:
        content_type = self.headers.get("Content-Type", "")
        body = self._read_body()
        if content_type.startswith("multipart/form-data"):
            return parse_multipart(body, content_type)
        if content_type.startswith("application/json"):
            try:
                data = json.loads(body.decode("utf-8") or "{}")
            except ValueError as exc:
                raise HttpError(400, "invalid JSON") from exc
            if not isinstance(data, dict):
                raise HttpError(400, "JSON object expected")
            return {key: str(value) if not isinstance(value, (dict, list)) else json.dumps(value)
                    for key, value in data.items()}, {}
        fields = {key: values[-1] for key, values in urllib.parse.parse_qs(body.decode("utf-8"), keep_blank_values=True).items()}
        return fields, {}

    def _cookie(self, name: str) -> str | None:
        cookie = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
        return cookie[name].value if name in cookie else None

    def _send(self, status: int, body: bytes, content_type: str = "text/html; charset=utf-8",
              extra: dict[str, str] | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, status: int, payload: Any, extra: dict[str, str] | None = None) -> None:
        self._send(status, json.dumps(payload, ensure_ascii=False, sort_keys=True).encode("utf-8"),
                   "application/json; charset=utf-8", extra)

    def _redirect(self, location: str, extra: dict[str, str] | None = None) -> None:
        self.send_response(303)
        self.send_header("Location", location)
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _session_cookie(self, token: str, *, clear: bool = False) -> dict[str, str]:
        if clear:
            return {"Set-Cookie": f"{SESSION_COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"}
        return {"Set-Cookie": f"{SESSION_COOKIE}={token}; Path=/; Max-Age={SESSION_TTL}; HttpOnly; SameSite=Lax"}

    def _flash_cookie(self, text: str, kind: str = "info") -> dict[str, str]:
        value = urllib.parse.quote(f"{kind}:{text}")
        return {"Set-Cookie": f"wotbmod_flash={value}; Path=/; Max-Age=60; SameSite=Lax"}

    def _page(self, title: str, body: str, *, status: int = 200, extra: dict[str, str] | None = None,
              description: str = "") -> None:
        user = getattr(self, "user", None)
        if user is not None:
            role_rank = ROLE_RANK[user["role"]]
            nav_user = (f"<a href='/dashboard'>Кабинет</a>"
                        + ("<a href='/moderation'>Модерация</a>" if role_rank >= ROLE_RANK["moderator"] else "")
                        + f"<span class='me'><a href='/account' title='{esc(user['email'])}'>{esc(user['display_name'])}</a>"
                        f"<form method='post' action='/logout'><input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
                        "<button type='submit'>Выйти</button></form></span>")
        else:
            nav_user = "<a href='/login'>Войти</a><a href='/register'>Регистрация</a>"
        headers = dict(extra or {})
        flash_text, flash_kind = "", ""
        flash = self._cookie("wotbmod_flash")
        if flash:
            flash_kind, _, flash_text = urllib.parse.unquote(flash).partition(":")
            headers.setdefault("Set-Cookie", "wotbmod_flash=; Path=/; Max-Age=0; SameSite=Lax")
        current = lambda prefix: "aria-current='page'" if self.route_path == prefix else ""  # noqa: E731
        notice_text = self.portal.get_setting(self.db, "notice") if getattr(self, "db", None) is not None else ""
        notice = f"<div class='notice' role='status'>{esc(notice_text)}</div>" if notice_text else ""
        page = render("layout", title=title, body_class="home" if self.route_path == "/" else "", description=description or "Каталог модов World of Tanks Blitz: права, зависимости и хеш видны до установки",
                      csrf=self.csrf, flash=flash_text, flash_kind=flash_kind, q=self.query.get("q", ""), notice=notice,
                      nav_home=current("/"), nav_compat=current("/compat"), nav_docs=current("/docs"),
                      nav_download=current("/download"),
                      nav_user=nav_user, body=body, version=PORTAL_VERSION, client_build=self.portal.client_build,
                      base_url=self.portal.base_url, asset_v=asset_version())
        self._send(status, page.encode("utf-8"), extra=headers)

    def _require_user(self, minimum_role: str = "user") -> sqlite3.Row:
        user = getattr(self, "user", None)
        if user is None:
            raise HttpError(401, "нужно войти")
        if ROLE_RANK[user["role"]] < ROLE_RANK[minimum_role]:
            raise HttpError(403, f"нужна роль {minimum_role}")
        return user

    def _check_csrf(self, fields: dict[str, str]) -> None:
        if getattr(self, "api_token_auth", False):
            return
        supplied = fields.get("csrf") or self.headers.get("X-CSRF-Token") or ""
        if not self.csrf or not hmac.compare_digest(supplied, self.csrf):
            raise HttpError(403, "форма устарела (CSRF); обновите страницу")

    # -- dispatch ------------------------------------------------------------------------

    def _dispatch(self, method: str) -> None:
        parsed = urllib.parse.urlsplit(self.path)
        self.route_path = parsed.path
        self.query = {key: values[-1] for key, values in urllib.parse.parse_qs(parsed.query, keep_blank_values=True).items()}
        self.user = None
        self.session = None
        self.csrf = ""
        self.api_token_auth = False
        db = self.portal.connect()
        self.db = db
        try:
            authorization = self.headers.get("Authorization", "")
            if authorization.lower().startswith("bearer "):
                self.user = self.portal.bearer_user(db, authorization[7:].strip())
                self.api_token_auth = self.user is not None
                if self.user is None:
                    raise HttpError(401, "invalid API token")
            else:
                self.user, self.session = self.portal.session_user(db, self._cookie(SESSION_COOKIE))
                self.csrf = self.session["csrf"] if self.session else ""
            handler = self._route(method, self.route_path)
            if handler is None:
                raise HttpError(404, "страница не найдена")
            fn, params = handler
            with self.portal.lock:
                fn(db, *params)
        except HttpError as exc:
            if self.route_path.startswith("/api/") or "application/json" in self.headers.get("Accept", ""):
                self._json(exc.status, {"ok": False, "error": exc.message})
            else:
                titles = {401: "Нужно войти", 403: "Нет доступа", 404: "Не найдено", 409: "Конфликт", 429: "Слишком часто"}
                self._page(titles.get(exc.status, f"Ошибка {exc.status}"),
                           render("message", title=titles.get(exc.status, f"Ошибка {exc.status}"), message=exc.message,
                                  extra="<p class='after'><a href='/login'>Войти</a></p>" if exc.status == 401 else ""),
                           status=exc.status)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:  # a bug: the traceback goes to the server log, never to the client
            import traceback
            sys.stderr.write(f"wotbmod_portal: unhandled {exc!r} on {method} {self.route_path}\n{traceback.format_exc()}")
            if self.route_path.startswith("/api/"):
                self._json(500, {"ok": False, "error": "internal error"})
            else:
                self._page("Ошибка", render("message", title="Что-то сломалось на сервере",
                                                message="Запись об ошибке уже в журнале сервера. Обновите страницу через минуту.", extra=""), status=500)
        finally:
            db.close()

    ROUTES: list[tuple[str, re.Pattern[str], str]] = []

    def _route(self, method: str, path: str) -> tuple[Callable[..., None], tuple[str, ...]] | None:
        for route_method, pattern, name in self.ROUTES:
            if route_method != method:
                continue
            match = pattern.fullmatch(path)
            if match:
                return getattr(self, name), tuple(urllib.parse.unquote(group) for group in match.groups())
        return None

    def do_GET(self) -> None:  # noqa: N802
        self._dispatch("GET")

    def do_HEAD(self) -> None:  # noqa: N802
        self._dispatch("GET")

    def do_POST(self) -> None:  # noqa: N802
        self._dispatch("POST")

    # -- static and docs -------------------------------------------------------------------

    def get_static(self, db: sqlite3.Connection, name: str) -> None:
        # Subfolders are allowed (static/fonts/*.woff2); ".." and symlinks
        # out of the static folder are not, whatever the route regex let in.
        path = STATIC_DIR / name
        content_type = STATIC_TYPES.get(path.suffix.lower())
        static_root = STATIC_DIR.resolve()
        if (content_type is None or ".." in name.split("/") or not path.is_file()
                or static_root not in path.resolve().parents):
            raise HttpError(404, "нет такого файла")
        self._send(200, path.read_bytes(), content_type, {"Cache-Control": "public, max-age=600"})

    def get_favicon(self, db: sqlite3.Connection) -> None:
        # The tab icon: browsers ask for /favicon.ico even with a <link rel="icon">.
        self.get_static(db, "favicon.ico")

    def get_docs(self, db: sqlite3.Connection) -> None:
        self._page("Как это работает", render("docs", base_url=self.portal.base_url))

    # -- the player's install bundle ---------------------------------------------------------
    #
    # <data>/downloads/*.zip is whatever the operator copies there (the public
    # preview bundle: loader, Lua host, CLI, launcher). The page lists them
    # newest first with size, date and SHA-256, and the file route streams one.

    def _downloads(self) -> Path:
        return self.portal.data_dir / "downloads"

    def get_download(self, db: sqlite3.Connection) -> None:
        rows = []
        folder = self._downloads()
        if folder.is_dir():
            bundles = [path for path in folder.iterdir() if path.suffix.lower() in (".exe", ".zip")]
            # the setup wizard first, then the zip, newest first inside each group
            for path in sorted(bundles, key=lambda item: (item.suffix.lower() != ".exe", -item.stat().st_mtime)):
                stat = path.stat()
                label = "установщик" if path.suffix.lower() == ".exe" else "архив для ручной установки"
                rows.append(f"<tr><td><a href='/download/{esc(path.name)}'>{esc(path.name)}</a><div class='muted small'>{label}</div></td>"
                            f"<td>{stat.st_size / 1048576:.1f} МБ</td>"
                            f"<td>{time.strftime('%d.%m.%Y', time.localtime(stat.st_mtime))}</td>"
                            f"<td><code>{download_digest(path)}</code></td></tr>")
        files = ("<div class='table-wrap'><table class='downloads'><thead><tr><th>Файл</th><th>Размер</th><th>Дата</th>"
                 "<th>SHA-256</th></tr></thead><tbody>" + "".join(rows) + "</tbody></table></div>") if rows             else "<p class='muted'>Архив ещё не выложен.</p>"
        self._page("Установка", render("download", files=files, base_url=self.portal.base_url,
                                       client_build=self.portal.client_build or "11.20.0.887"),
                   description="Как поставить загрузчик модов BlitzForge и кнопку «Установить в игру».")

    def get_download_file(self, db: sqlite3.Connection, name: str) -> None:
        folder = self._downloads()
        path = folder / name
        if not folder.is_dir() or not path.is_file() or path.resolve().parent != folder.resolve():
            raise HttpError(404, "нет такого файла")
        content_type = "application/zip" if name.lower().endswith(".zip") else "application/vnd.microsoft.portable-executable"
        self._send(200, path.read_bytes(), content_type,
                   {"Content-Disposition": f'attachment; filename="{name}"', "Cache-Control": "public, max-age=3600"})

    # -- catalogue pages ---------------------------------------------------------------------

    def get_catalog(self, db: sqlite3.Connection) -> None:
        q = self.query.get("q", "").strip()
        type_filter = self.query.get("type", "")
        tier = self.query.get("tier", "")
        verified_only = self.query.get("verified") == "1"
        sort = self.query.get("sort", "updated")
        items = self.portal.catalogue(db, q=q, type_filter=type_filter, tier=tier if tier in TIER_NAMES else "",
                                      verified_only=verified_only, sort=sort)
        count = len(items)
        forms = {"one": "мод", "few": "мода", "many": "модов"}
        if count % 10 == 1 and count % 100 != 11:
            word = forms["one"]
        elif 2 <= count % 10 <= 4 and not 12 <= count % 100 <= 14:
            word = forms["few"]
        else:
            word = forms["many"]
        count_text = f"{count} {word}" if count else "пока ни одного мода: опубликуйте первый из кабинета"
        if q and not count:
            count_text = "ничего не найдено"
        body = render("home", q=q, count_text=count_text,
                      p_lua=str(type_filter == "lua").lower(), p_native=str(type_filter == "native").lower(),
                      p_content=str(type_filter == "content").lower(), p_safe=str(tier == "SAFE").lower(),
                      p_verified=str(verified_only).lower(),
                      sort_options="".join(f"<option value='{key}' {'selected' if sort == key else ''}>{esc(label)}</option>"
                                           for key, label in SORTS.items()),
                      cards="".join(card_html(item) for item in items)
                      or "<li class='empty'>По этому запросу модов нет. Попробуйте другое слово или снимите фильтры.</li>")
        self._page("Каталог", body)

    @staticmethod
    def _badges(item: dict[str, Any]) -> str:
        badges = []
        if item.get("verified"):
            badges.append("<span class='verified'>проверен</span>")
        badges.append(tier_chip(item.get("tier", "SAFE")))
        return " ".join(badges)

    def _mod_or_404(self, db: sqlite3.Connection, mod_id: str) -> tuple[sqlite3.Row, dict[str, Any]]:
        mod = db.execute("SELECT * FROM mods WHERE id=?", (mod_id,)).fetchone()
        if mod is None:
            raise HttpError(404, "мод не найден")
        summary = self.portal.mod_summary(db, mod)
        if summary is None:
            raise HttpError(404, "у мода нет опубликованных версий")
        return mod, summary

    def get_mod(self, db: sqlite3.Connection, mod_id: str) -> None:
        mod, item = self._mod_or_404(db, mod_id)
        record = item["record"]
        release = item["release"]
        releases = db.execute("SELECT * FROM releases WHERE mod_id=? AND status='published' ORDER BY id DESC", (mod_id,)).fetchall()
        changelog = "".join(
            f"<li><div class='vh'><b>{esc(r['version'])}</b><span>{esc((r['published_at'] or '')[:10])}</span>"
            f"<span>{downloads_text(r['downloads'])}</span><code>{esc(r['sha256'][:12])}</code></div>"
            f"<p>{esc(r['notes']) if r['notes'] else '<span class=muted>Без описания изменений.</span>'}</p></li>"
            for r in releases)
        compat_rows = db.execute("SELECT c.*, u.display_name FROM compat c JOIN users u ON u.id=c.declared_by "
                                 "WHERE c.mod_id=? AND c.version=? ORDER BY c.client_build", (mod_id, release["version"])).fetchall()
        compat_labels = {"works": "работает", "broken": "сломан", "untested": "не проверялся"}
        compat = "<br>".join(f"{esc(c['client_build'])}: {compat_labels.get(c['status'], esc(c['status']))} <span class='muted'>({esc(c['display_name'])})</span>"
                             for c in compat_rows) or "<span class='muted'>не заявлена</span>"
        comments = db.execute("SELECT c.*, u.display_name FROM comments c JOIN users u ON u.id=c.user_id "
                              "WHERE c.mod_id=? AND c.hidden=0 ORDER BY c.id DESC LIMIT 100", (mod_id,)).fetchall()
        def comment_controls(c: sqlite3.Row) -> str:
            if self.user is None:
                return ""
            own = c["user_id"] == self.user["id"]
            if not own and ROLE_RANK[self.user["role"]] < ROLE_RANK["moderator"]:
                return ""
            csrf_field = f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
            edit = (f"<details class='inline'><summary class='btn btn-sm btn-ghost'>Изменить</summary>"
                    f"<form method='post' action='/mods/{esc(mod_id)}/comments/{c['id']}/edit'>{csrf_field}"
                    f"<textarea name='text' rows='3' required maxlength='2000'>{esc(c['text'])}</textarea>"
                    f"<button class='btn btn-sm' type='submit'>Сохранить</button></form></details>") if own else ""
            delete = (f"<form class='inline' method='post' action='/mods/{esc(mod_id)}/comments/{c['id']}/delete'>{csrf_field}"
                      f"<button class='btn btn-sm btn-ghost' type='submit'>{'Удалить' if own else 'Скрыть'}</button></form>")
            return f"<div class='comment-actions'>{edit}{delete}</div>"
        comments_html = "".join(f"<li><div class='who'>{esc(c['display_name'])}, {esc(c['created_at'][:10])}</div>"
                                f"<div>{esc(c['text'])}</div>{comment_controls(c)}</li>" for c in comments) \
            or "<li class='muted'>Отзывов пока нет. Первый может быть вашим.</li>"
        forms = {"rating_form": "", "comment_form": "", "report_form": "", "compat_form": ""}
        if self.user is not None:
            mine = db.execute("SELECT stars FROM ratings WHERE user_id=? AND mod_id=?", (self.user["id"], mod_id)).fetchone()
            chosen = mine["stars"] if mine else 0
            forms["rating_form"] = (f"<form class='inline' method='post' action='/mods/{esc(mod_id)}/rate' style='margin-bottom:12px'>"
                                    f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'><span class='muted small'>Ваша оценка</span>"
                                    "<span class='rating-pick'>" +
                                    "".join(f"<input type='radio' id='star{n}' name='stars' value='{n}' {'checked' if chosen == n else ''}>"
                                            f"<label for='star{n}' title='{n} из 5'>{n}</label>" for n in range(1, 6)) +
                                    "</span><noscript><button class='btn btn-sm' type='submit'>Оценить</button></noscript></form>")
            forms["comment_form"] = (f"<form method='post' action='/mods/{esc(mod_id)}/comments' style='margin-top:16px'>"
                                     f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
                                     "<label class='field'><span>Отзыв</span><textarea name='text' rows='3' required maxlength='2000' placeholder='Что понравилось, что сломалось, на каком клиенте'></textarea></label>"
                                     "<button class='btn btn-sm' type='submit'>Отправить отзыв</button></form>")
            forms["report_form"] = (f"<form class='inline' method='post' action='/mods/{esc(mod_id)}/report' style='margin-top:14px'>"
                                    f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
                                    "<input type='text' name='reason' placeholder='Пожаловаться модераторам: что не так?' required maxlength='500' aria-label='Причина жалобы'>"
                                    "<button class='btn btn-sm btn-ghost' type='submit'>Пожаловаться</button></form>")
            if mod["owner_id"] == self.user["id"] or ROLE_RANK[self.user["role"]] >= ROLE_RANK["moderator"]:
                forms["compat_form"] = (f"<form class='inline' method='post' action='/mods/{esc(mod_id)}/compat' style='margin-top:14px'>"
                                        f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'><input type='hidden' name='version' value='{esc(release['version'])}'>"
                                        f"<input type='text' name='client_build' value='{esc(self.portal.client_build)}' aria-label='Build клиента'>"
                                        "<select name='status' aria-label='Статус'><option value='works'>работает</option><option value='broken'>сломан</option><option value='untested'>не проверялся</option></select>"
                                        "<button class='btn btn-sm' type='submit'>Заявить совместимость</button></form>")
        else:
            forms["comment_form"] = "<p class='muted'><a href='/login'>Войдите</a>, чтобы оценить мод или оставить отзыв.</p>"
        signature = record.get("signature")
        signature_text = "нет: пакет без подписи"
        signature_link = ""
        if signature:
            signed_by = "портал" if signature.get("signed_by") == "portal" else "разработчик"
            signature_text = f"ключ {esc(signature.get('key_id', ''))} ({signed_by})"
            signature_link = f"<a href='/api/v1/releases/{esc(mod_id)}/{esc(release['version'])}/signature'>Подпись .sig</a>"
        scan_report = record.get("scan") or {}
        scan_html = "<p class='muted'>Эта версия не проходила статическую проверку.</p>"
        if scan_report:
            summary = scan_report.get("summary", {})
            risk = scan_report.get("risk", "low")
            risk_words = {"low": "низкий риск", "medium": "есть предупреждения", "high": "есть блокирующие находки"}  # noqa: E501
            scan_html = (f"<div class='scan {esc(risk)}'>{risk_words.get(risk, esc(risk))}: блокирующих {summary.get('block', 0)}, "
                         f"предупреждений {summary.get('warn', 0)}, проверено файлов {scan_report.get('files_scanned', 0)}")
            warnings = [f for f in scan_report.get("findings", []) if f.get("severity") in ("block", "warn")]
            if warnings:
                scan_html += "<ul>" + "".join(f"<li>{esc(f['code'])}: {esc(f['message'])} <span class='muted'>({esc(f['where'])})</span></li>"
                                              for f in warnings[:20]) + "</ul>"
            scan_html += "</div>"
        tier_name = record.get("permission_tier", "SAFE")
        body = render("mod", id=mod_id, name=item["name"], badges=self._badges(item), owner_id=item["owner_id"],
                      initials=initials_of(item["name"], mod_id),
                      developer=item["developer"], type_label=TYPE_LABELS.get(item["type"], item["type"]),
                      rating_html=rating_html(item["rating"], item["votes"]), downloads=item["downloads"],
                      description=mod["description"] or mod["summary"] or "Автор пока не добавил описание.",
                      version=release["version"], published_at=(release["published_at"] or "")[:10], sha256=release["sha256"],
                      size=format_size(release["size"]), signature=signature_text, signature_link=signature_link,
                      client_builds=", ".join(record.get("client", {}).get("builds", [])) or "не ограничен",
                      risk_label=f"{tier_name}: {scan.RISK_LABELS.get(tier_name, '')}",
                      permissions=permission_html(record), scan=scan_html,
                      dependencies=deps_html(record.get("dependencies", {})), optional=deps_html(record.get("optional_dependencies", {})),
                      incompatibilities=deps_html(record.get("incompatibilities", {})), compat=compat,
                      changelog=changelog, comments=comments_html, **forms)
        self._page(item["name"], body, description=(mod["summary"] or item["name"]))

    def get_install(self, db: sqlite3.Connection, spec: str) -> None:
        mod_id, _, version = spec.partition("@")
        mod, item = self._mod_or_404(db, mod_id)
        release = db.execute("SELECT * FROM releases WHERE mod_id=? AND version=? AND status='published'",
                             (mod_id, version or item["version"])).fetchone()
        if release is None:
            raise HttpError(404, "такой версии нет")
        record = json.loads(release["record_json"])
        catalog_url = f"{self.portal.base_url}/api/v1"
        scheme_url = f"wotbmod://install/{mod_id}@{release['version']}?source={urllib.parse.quote(catalog_url, safe='')}"
        signature_step = ""
        if record.get("signature"):
            signature_step = (f" и <a href='/api/v1/releases/{esc(mod_id)}/{esc(release['version'])}/signature'>"
                              f"{esc(record['artifact']['file'])}.sig</a> в ту же папку")
        tier_name = record.get("permission_tier", "SAFE")
        body = render("install", id=mod_id, name=item["name"], version=release["version"], scheme_url=scheme_url,
                      initials=initials_of(item["name"], mod_id), developer=item["developer"],
                      sha256=release["sha256"], permissions=permission_html(record),
                      risk_label=f"{tier_name}: {scan.RISK_LABELS.get(tier_name, '')}",
                      dependencies=deps_html(record.get("dependencies", {})),
                      client_builds=", ".join(record.get("client", {}).get("builds", [])) or "не ограничен",
                      artifact_name=record["artifact"]["file"], signature_step=signature_step, catalog_url=catalog_url)
        self._page(f"Установка {item['name']}", body)

    def get_developer(self, db: sqlite3.Connection, user_id: str) -> None:
        user = db.execute("SELECT * FROM users WHERE id=?", (int(user_id) if user_id.isdigit() else -1,)).fetchone()
        if user is None:
            raise HttpError(404, "разработчик не найден")
        cards = []
        for mod in db.execute("SELECT * FROM mods WHERE owner_id=? AND listed=1", (user["id"],)).fetchall():
            item = self.portal.mod_summary(db, mod)
            if item:
                cards.append(card_html(item))
        badges = "<span class='verified'>проверенный разработчик</span>" \
            if ROLE_RANK[user["role"]] >= ROLE_RANK["verified_developer"] else ""
        count = len(cards)  # rows, one per published mod
        self._page(user["display_name"], render("developer", display_name=user["display_name"], badges=badges,
                                                initials=initials_of(user["display_name"]), created_at=user["created_at"][:10],
                                                mods_count=f"{count} опубликованных модов" if count != 1 else "1 опубликованный мод",
                                                cards="".join(cards) or "<li class='empty'>Опубликованных модов пока нет.</li>"))

    # -- community actions ------------------------------------------------------------------

    def post_rate(self, db: sqlite3.Connection, mod_id: str) -> None:
        user = self._require_user()
        fields, _ = self._form()
        self._check_csrf(fields)
        self._mod_or_404(db, mod_id)
        stars = int(fields.get("stars", "0") or 0)
        if not 1 <= stars <= 5:
            raise HttpError(400, "оценка от 1 до 5")
        db.execute("INSERT INTO ratings(user_id, mod_id, stars, created_at) VALUES (?,?,?,?) "
                   "ON CONFLICT(user_id, mod_id) DO UPDATE SET stars=excluded.stars, created_at=excluded.created_at",
                   (user["id"], mod_id, stars, now_iso()))
        self._done(f"/mods/{mod_id}", {"ok": True, "stars": stars}, "Оценка сохранена")

    def post_comment(self, db: sqlite3.Connection, mod_id: str) -> None:
        user = self._require_user()
        fields, _ = self._form()
        self._check_csrf(fields)
        self._mod_or_404(db, mod_id)
        text = fields.get("text", "").strip()
        if not 1 <= len(text) <= 2000:
            raise HttpError(400, "отзыв: от 1 до 2000 символов")
        cursor = db.execute("INSERT INTO comments(mod_id, user_id, text, created_at) VALUES (?,?,?,?)",
                            (mod_id, user["id"], text, now_iso()))
        self._done(f"/mods/{mod_id}", {"ok": True, "id": cursor.lastrowid}, "Отзыв добавлен")

    def _own_comment(self, db: sqlite3.Connection, mod_id: str, comment_id: str, *, moderator_ok: bool) -> sqlite3.Row:
        user = self._require_user()
        comment = db.execute("SELECT * FROM comments WHERE id=? AND mod_id=?", (int(comment_id), mod_id)).fetchone()
        if comment is None:
            raise HttpError(404, "отзыв не найден")
        if comment["user_id"] != user["id"] and not (moderator_ok and ROLE_RANK[user["role"]] >= ROLE_RANK["moderator"]):
            raise HttpError(403, "не ваш отзыв")
        return comment

    def post_comment_edit(self, db: sqlite3.Connection, mod_id: str, comment_id: str) -> None:
        fields, _ = self._form()
        if not self.route_path.startswith("/api/"):
            self._check_csrf(fields)
        comment = self._own_comment(db, mod_id, comment_id, moderator_ok=False)
        text = fields.get("text", "").strip()
        if not 1 <= len(text) <= 2000:
            raise HttpError(400, "отзыв: от 1 до 2000 символов")
        db.execute("UPDATE comments SET text=? WHERE id=?", (text, comment["id"]))
        self._done(f"/mods/{mod_id}", {"ok": True, "id": comment["id"]}, "Отзыв изменён")

    def post_comment_delete(self, db: sqlite3.Connection, mod_id: str, comment_id: str) -> None:
        # The author deletes; a moderator hides (the row stays for the audit trail).
        fields, _ = self._form()
        if not self.route_path.startswith("/api/"):
            self._check_csrf(fields)
        comment = self._own_comment(db, mod_id, comment_id, moderator_ok=True)
        if comment["user_id"] == self.user["id"]:
            db.execute("DELETE FROM comments WHERE id=?", (comment["id"],))
            message = "Отзыв удалён"
        else:
            db.execute("UPDATE comments SET hidden=1 WHERE id=?", (comment["id"],))
            self.portal.audit(db, self.user["id"], "comment.hide", f"{mod_id}#{comment['id']}")
            message = "Отзыв скрыт"
        self._done(f"/mods/{mod_id}", {"ok": True, "id": comment["id"]}, message)

    def post_notice(self, db: sqlite3.Connection) -> None:
        # The site-wide announcement (client patch, maintenance): shown on every
        # page and carried in index.json so the CLI can print it too.
        admin = self._require_user("administrator")
        fields, _ = self._form()
        if not self.route_path.startswith("/api/"):
            self._check_csrf(fields)
        text = fields.get("text", "").strip()
        if len(text) > NOTICE_MAX:
            raise HttpError(400, f"объявление: не длиннее {NOTICE_MAX} символов")
        self.portal.set_setting(db, "notice", text)
        self.portal.audit(db, admin["id"], "site.notice", text[:200] or "(cleared)")
        self._done("/moderation", {"ok": True, "notice": text}, "Объявление обновлено" if text else "Объявление снято")

    def post_report(self, db: sqlite3.Connection, mod_id: str) -> None:
        user = self._require_user()
        fields, _ = self._form()
        self._check_csrf(fields)
        self._mod_or_404(db, mod_id)
        reason = fields.get("reason", "").strip()
        if not 1 <= len(reason) <= 500:
            raise HttpError(400, "причина: от 1 до 500 символов")
        target_kind = fields.get("target_kind", "mod")
        target_id = fields.get("target_id", mod_id)
        if target_kind not in ("mod", "comment", "release"):
            raise HttpError(400, "target_kind: mod|comment|release")
        cursor = db.execute("INSERT INTO reports(target_kind, target_id, reporter_id, reason, created_at) VALUES (?,?,?,?,?)",
                            (target_kind, target_id, user["id"], reason, now_iso()))
        self._done(f"/mods/{mod_id}", {"ok": True, "id": cursor.lastrowid}, "Жалоба отправлена модераторам")

    def post_compat(self, db: sqlite3.Connection, mod_id: str) -> None:
        user = self._require_user("developer")
        fields, _ = self._form()
        self._check_csrf(fields)
        mod, item = self._mod_or_404(db, mod_id)
        if mod["owner_id"] != user["id"] and ROLE_RANK[user["role"]] < ROLE_RANK["moderator"]:
            raise HttpError(403, "совместимость заявляет автор мода или модератор")
        version = fields.get("version", item["version"])
        client_build = fields.get("client_build", "").strip()
        status = fields.get("status", "works")
        if wotbmod.CLIENT_BUILD_RE.fullmatch(client_build) is None:
            raise HttpError(400, "client_build: например 11.20.0.887")
        if status not in ("works", "broken", "untested"):
            raise HttpError(400, "status: works|broken|untested")
        if db.execute("SELECT 1 FROM releases WHERE mod_id=? AND version=?", (mod_id, version)).fetchone() is None:
            raise HttpError(404, "такой версии нет")
        db.execute("INSERT INTO compat(mod_id, version, client_build, status, declared_by, created_at) VALUES (?,?,?,?,?,?) "
                   "ON CONFLICT(mod_id, version, client_build) DO UPDATE SET status=excluded.status, "
                   "declared_by=excluded.declared_by, created_at=excluded.created_at",
                   (mod_id, version, client_build, status, user["id"], now_iso()))
        self._done(f"/mods/{mod_id}", {"ok": True, "version": version, "client_build": client_build, "status": status},
                   "Совместимость заявлена")

    def _done(self, location: str, payload: dict[str, Any], flash: str) -> None:
        if self.route_path.startswith("/api/") or self.api_token_auth or \
                "application/json" in self.headers.get("Accept", ""):
            self._json(200, payload)
        else:
            self._redirect(location, self._flash_cookie(flash))

    # -- auth (HTML) ---------------------------------------------------------------------------

    def get_register(self, db: sqlite3.Connection) -> None:
        self._page("Регистрация", render("register"))

    def post_register(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        user_id = self.portal.register(db, fields.get("email", ""), fields.get("display_name", ""), fields.get("password", ""),
                                       self.client_address[0], fields.get("website", ""))
        token, csrf = self.portal.open_session(db, user_id)
        if self.route_path.startswith("/api/"):
            self._json(200, {"ok": True, "user_id": user_id, "session": token, "csrf": csrf}, self._session_cookie(token))
        else:
            self._redirect("/account", {**self._session_cookie(token),
                                        **self._flash_cookie("Аккаунт создан; подтвердите почту по ссылке из письма")})

    def get_login(self, db: sqlite3.Connection) -> None:
        self._page("Вход", render("login"))

    def post_login(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        user = self.portal.login(db, fields.get("email", ""), fields.get("password", ""), self.client_address[0])
        token, csrf = self.portal.open_session(db, user["id"])
        if self.route_path.startswith("/api/"):
            self._json(200, {"ok": True, "user_id": user["id"], "role": user["role"], "session": token, "csrf": csrf},
                       self._session_cookie(token))
        else:
            self._redirect("/", self._session_cookie(token))

    def post_logout(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        if self.session is not None:
            if not self.route_path.startswith("/api/"):
                self._check_csrf(fields)
            db.execute("DELETE FROM sessions WHERE token=?", (self.session["token"],))
        if self.route_path.startswith("/api/"):
            self._json(200, {"ok": True}, self._session_cookie("", clear=True))
        else:
            self._redirect("/", self._session_cookie("", clear=True))

    def post_refresh(self, db: sqlite3.Connection) -> None:
        if self.session is None:
            raise HttpError(401, "нет сессии")
        db.execute("DELETE FROM sessions WHERE token=?", (self.session["token"],))
        token, _csrf = self.portal.open_session(db, self.session["user_id"])
        self._json(200, {"ok": True, "session": token}, self._session_cookie(token))

    def get_verify(self, db: sqlite3.Connection) -> None:
        user_id = self.portal.consume_email_token(db, self.query.get("token", ""), "verify")
        db.execute("UPDATE users SET email_verified=1 WHERE id=?", (user_id,))
        self._redirect("/account", self._flash_cookie("Почта подтверждена"))

    def post_verify_api(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        user_id = self.portal.consume_email_token(db, fields.get("token", ""), "verify")
        db.execute("UPDATE users SET email_verified=1 WHERE id=?", (user_id,))
        self._json(200, {"ok": True, "user_id": user_id})

    def get_reset(self, db: sqlite3.Connection) -> None:
        self._page("Сброс пароля", render("reset_request"))

    def post_reset(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        user = db.execute("SELECT * FROM users WHERE email=?", (fields.get("email", "").strip().lower(),)).fetchone()
        if user is not None:
            token = self.portal.issue_email_token(db, user["id"], "reset")
            self.portal.send_mail(user["email"], "Сброс пароля BlitzForge",
                                  f"Ссылка для нового пароля:\n{self.portal.base_url}/reset/{token}\n")
        if self.route_path.startswith("/api/"):
            self._json(200, {"ok": True})
        else:
            self._redirect("/login", self._flash_cookie("Если почта известна, письмо отправлено"))

    def get_reset_confirm(self, db: sqlite3.Connection, token: str) -> None:
        self._page("Новый пароль", render("reset_confirm", token=token))

    def post_reset_confirm(self, db: sqlite3.Connection, token: str) -> None:
        fields, _ = self._form()
        self._reset_password(db, token, fields.get("password", ""))
        self._redirect("/login", self._flash_cookie("Пароль изменён; войдите"))

    def post_reset_confirm_api(self, db: sqlite3.Connection) -> None:
        fields, _ = self._form()
        self._reset_password(db, fields.get("token", ""), fields.get("password", ""))
        self._json(200, {"ok": True})

    def _reset_password(self, db: sqlite3.Connection, token: str, password: str) -> None:
        if len(password) < 8:
            raise HttpError(400, "пароль: минимум 8 символов")
        user_id = self.portal.consume_email_token(db, token, "reset")
        db.execute("UPDATE users SET password_hash=? WHERE id=?", (hash_password(password), user_id))
        db.execute("DELETE FROM sessions WHERE user_id=?", (user_id,))

    # -- account -------------------------------------------------------------------------------

    def get_account(self, db: sqlite3.Connection) -> None:
        user = self._require_user()
        application = db.execute("SELECT * FROM developer_applications WHERE user_id=? ORDER BY id DESC", (user["id"],)).fetchone()
        if ROLE_RANK[user["role"]] >= ROLE_RANK["developer"]:
            application_html = "<p>Вы разработчик: публикуйте моды в <a href='/dashboard'>кабинете</a>.</p>"
        elif application is not None and application["status"] == "pending":
            application_html = "<p class='muted'>Заявка на рассмотрении у модераторов. Ответ придёт в этот раздел.</p>"
        else:
            application_html = (f"<form method='post' action='/account/apply'><input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
                                "<label class='field'><span>Расскажите о себе и о модах, которые хотите публиковать</span>"
                                "<textarea name='text' rows='4' required maxlength='2000'></textarea></label>"
                                "<button class='btn' type='submit'>Подать заявку</button></form>")
            if application is not None and application["status"] == "rejected":
                application_html = "<p class='notice warn'>Предыдущая заявка отклонена. Можно подать новую с подробностями.</p>" + application_html
        verify_hint = "" if user["email_verified"] else \
            "<p class='notice warn'>Почта не подтверждена: откройте ссылку из письма. При локальном запуске письмо лежит в папке outbox.</p>"
        self._page("Аккаунт", render("account", email=user["email"], display_name=user["display_name"],
                                     role_label=ROLE_LABELS.get(user["role"], user["role"]),
                                     verified="подтверждена" if user["email_verified"] else "не подтверждена",
                                     verify_hint=verify_hint, application=application_html, csrf=self.csrf))

    def post_password(self, db: sqlite3.Connection) -> None:
        user = self._require_user()
        fields, _ = self._form()
        self._check_csrf(fields)
        if not check_password(fields.get("current", ""), user["password_hash"]):
            raise HttpError(403, "текущий пароль неверен")
        if len(fields.get("password", "")) < 8:
            raise HttpError(400, "пароль: минимум 8 символов")
        db.execute("UPDATE users SET password_hash=? WHERE id=?", (hash_password(fields["password"]), user["id"]))
        self._done("/account", {"ok": True}, "Пароль изменён")

    def post_apply(self, db: sqlite3.Connection) -> None:
        user = self._require_user()
        fields, _ = self._form()
        self._check_csrf(fields)
        if not user["email_verified"]:
            raise HttpError(403, "сначала подтвердите почту")
        if ROLE_RANK[user["role"]] >= ROLE_RANK["developer"]:
            raise HttpError(400, "вы уже разработчик")
        text = fields.get("text", "").strip()
        if not 1 <= len(text) <= 2000:
            raise HttpError(400, "текст заявки: от 1 до 2000 символов")
        if db.execute("SELECT 1 FROM developer_applications WHERE user_id=? AND status='pending'", (user["id"],)).fetchone():
            raise HttpError(409, "заявка уже на рассмотрении")
        cursor = db.execute("INSERT INTO developer_applications(user_id, text, created_at) VALUES (?,?,?)",
                            (user["id"], text, now_iso()))
        self._done("/account", {"ok": True, "application_id": cursor.lastrowid}, "Заявка отправлена")

    # -- dashboard -------------------------------------------------------------------------------

    def get_dashboard(self, db: sqlite3.Connection) -> None:
        user = self._require_user()
        gate = ""
        if ROLE_RANK[user["role"]] < ROLE_RANK["developer"]:
            gate = "<p class='notice'>Публиковать могут разработчики: <a href='/account'>подайте заявку</a> в аккаунте.</p>"
        rows = []
        for mod in db.execute("SELECT * FROM mods WHERE owner_id=? ORDER BY updated_at DESC", (user["id"],)).fetchall():
            releases = db.execute("SELECT * FROM releases WHERE mod_id=? ORDER BY id DESC", (mod["id"],)).fetchall()
            release_rows = []
            for release in releases:
                action = "unpublish" if release["status"] == "published" else "publish"
                label = "Снять" if action == "unpublish" else "Опубликовать"
                status_label = {"published": "опубликована", "draft": "черновик", "unpublished": "снята"}.get(release["status"], release["status"])
                release_rows.append(
                    f"<li><span class='grow'><b>{esc(release['version'])}</b> <span class='status {esc(release['status'])}'>{status_label}</span> "
                    f"<span class='sub'>{release['downloads']} загрузок, <code>{esc(release['sha256'][:12])}</code></span></span>"
                    f"<form method='post' action='/dashboard/releases/{release['id']}/{action}'>"
                    f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'><button class='btn btn-sm' type='submit'>{label}</button></form></li>")
            verified = "<span class='verified'>проверен</span>" if mod["verified"] else ""
            rows.append(f"<h3 style='margin:18px 0 8px'><a href='/mods/{esc(mod['id'])}'>{esc(mod['name'])}</a> {verified} "
                        f"<span class='muted small'><code>{esc(mod['id'])}</code></span></h3>"
                        f"<ul class='rows'>{''.join(release_rows) or '<li class=muted>Версий пока нет: загрузите первую ниже.</li>'}</ul>")
        tokens = db.execute("SELECT * FROM api_tokens WHERE user_id=? ORDER BY created_at", (user["id"],)).fetchall()
        tokens_html = "<ul class='rows'>" + ("".join(
            f"<li><span class='grow'>{esc(t['name'])}</span><span class='sub'>создан {esc(t['created_at'][:10])}, "
            f"{'использован ' + esc(t['last_used'][:10]) if t['last_used'] else 'ещё не использован'}</span></li>" for t in tokens)
            or "<li class='muted'>Токенов нет.</li>") + "</ul>"
        self._page("Кабинет", render("dashboard", display_name=user["display_name"], role_label=ROLE_LABELS.get(user["role"], user["role"]),
                                     developer_gate=gate, mods="".join(rows) or "<p class='muted'>Модов пока нет. Создайте первый ниже.</p>",
                                     csrf=self.csrf, tokens=tokens_html, base_url=self.portal.base_url))

    def post_create_mod(self, db: sqlite3.Connection) -> None:
        user = self._require_user("developer")
        fields, _ = self._form()
        self._check_csrf(fields)
        mod_id = fields.get("id", "").strip()
        name = fields.get("name", "").strip()
        if MOD_ID_RE.fullmatch(mod_id) is None:
            raise HttpError(400, "id: буквы, цифры, точки, дефисы, до 95 символов")
        if not 1 <= len(name) <= 127:
            raise HttpError(400, "название: от 1 до 127 символов")
        if db.execute("SELECT 1 FROM mods WHERE id=?", (mod_id,)).fetchone():
            raise HttpError(409, "такой id уже занят")
        db.execute("INSERT INTO mods(id, owner_id, name, summary, description, created_at, updated_at) VALUES (?,?,?,?,?,?,?)",
                   (mod_id, user["id"], name, fields.get("summary", "").strip()[:200], fields.get("description", "").strip()[:4000],
                    now_iso(), now_iso()))
        self.portal.audit(db, user["id"], "mod.create", mod_id)
        self._done("/dashboard", {"ok": True, "id": mod_id}, f"Мод {mod_id} создан")

    def post_upload(self, db: sqlite3.Connection) -> None:
        user = self._require_user("developer")
        fields, files = self._form()
        self._check_csrf(fields)
        if "artifact" not in files:
            raise HttpError(400, "нужен файл .wotbmod")
        signature = files.get("signature", (None, None))[1]
        release = self.portal.store_release(db, user, files["artifact"][1], signature, notes=fields.get("notes", "").strip(),
                                            publish=fields.get("publish") == "1")
        self._done("/dashboard", {"ok": True, "id": release["mod_id"], "version": release["version"], "status": release["status"]},
                   f"{release['mod_id']} {release['version']}: {release['status']}")

    def post_release_action(self, db: sqlite3.Connection, release_id: str, action: str) -> None:
        user = self._require_user("developer")
        fields, _ = self._form()
        self._check_csrf(fields)
        if action == "publish":
            self.portal.publish_release(db, user, int(release_id))
        else:
            self.portal.unpublish_release(db, user, int(release_id))
        self._done("/dashboard", {"ok": True, "release_id": int(release_id), "status": "published" if action == "publish" else "unpublished"},
                   "Готово")

    def post_create_token(self, db: sqlite3.Connection) -> None:
        user = self._require_user("developer")
        fields, _ = self._form()
        self._check_csrf(fields)
        name = fields.get("name", "").strip()[:40] or "token"
        token = "wmt_" + secrets.token_urlsafe(32)
        db.execute("INSERT INTO api_tokens(token_hash, user_id, name, created_at) VALUES (?,?,?,?)",
                   (token_hash(token), user["id"], name, now_iso()))
        if self.route_path.startswith("/api/") or "application/json" in self.headers.get("Accept", ""):
            self._json(200, {"ok": True, "token": token, "name": name})
        else:
            self._page("Токен создан", render("message", title="Токен создан", message="Скопируйте его сейчас: он показывается один раз.",
                                              extra=f"<div class='token mono'>{esc(token)}</div><p class='small muted' style='margin-top:10px'>"
                                                    f"<code>set WOTBMOD_PORTAL_TOKEN={esc(token)}</code></p>"))

    # -- moderation --------------------------------------------------------------------------------

    def get_moderation(self, db: sqlite3.Connection) -> None:
        user = self._require_user("moderator")
        csrf = f"<input type='hidden' name='csrf' value='{esc(self.csrf)}'>"
        applications = db.execute("SELECT a.*, u.display_name, u.email FROM developer_applications a JOIN users u ON u.id=a.user_id "
                                  "WHERE a.status='pending' ORDER BY a.id").fetchall()
        applications_html = "<ul class='rows'>" + ("".join(
            f"<li><span class='grow'><b>{esc(a['display_name'])}</b> <span class='sub'>{esc(a['email'])}</span><br>{esc(a['text'])}</span>"
            f"<form method='post' action='/moderation/applications/{a['id']}/approve'>{csrf}<button class='btn btn-sm' type='submit'>Одобрить</button></form>"
            f"<form method='post' action='/moderation/applications/{a['id']}/reject'>{csrf}<button class='btn btn-sm btn-ghost' type='submit'>Отклонить</button></form></li>"
            for a in applications) or "<li class='muted'>Заявок нет.</li>") + "</ul>"
        reports = db.execute("SELECT r.*, u.display_name FROM reports r JOIN users u ON u.id=r.reporter_id WHERE r.status='open' ORDER BY r.id").fetchall()
        reports_html = "<ul class='rows'>" + ("".join(
            f"<li><span class='grow'><b>{esc(r['target_kind'])} {esc(r['target_id'])}</b><br>{esc(r['reason'])} <span class='sub'>от {esc(r['display_name'])}</span></span>"
            f"<form method='post' action='/moderation/reports/{r['id']}/resolve'>{csrf}<button class='btn btn-sm' type='submit'>Решено</button></form>"
            f"<form method='post' action='/moderation/reports/{r['id']}/dismiss'>{csrf}<button class='btn btn-sm btn-ghost' type='submit'>Отклонить</button></form></li>"
            for r in reports) or "<li class='muted'>Открытых жалоб нет.</li>") + "</ul>"
        mods = db.execute("SELECT m.*, u.display_name FROM mods m JOIN users u ON u.id=m.owner_id ORDER BY m.updated_at DESC").fetchall()
        mods_html = "<ul class='rows'>" + ("".join(
            f"<li><span class='grow'><a href='/mods/{esc(m['id'])}'>{esc(m['id'])}</a> <span class='sub'>{esc(m['display_name'])}, "
            f"{'проверен' if m['verified'] else 'не проверен'}, {'в каталоге' if m['listed'] else 'скрыт'}</span></span>"
            f"<form method='post' action='/moderation/mods/{esc(m['id'])}/{'unverify' if m['verified'] else 'verify'}'>{csrf}"
            f"<button class='btn btn-sm' type='submit'>{'Снять проверку' if m['verified'] else 'Пометить проверенным'}</button></form>"
            f"<form method='post' action='/moderation/mods/{esc(m['id'])}/{'unlist' if m['listed'] else 'list'}'>{csrf}"
            f"<button class='btn btn-sm btn-ghost' type='submit'>{'Скрыть' if m['listed'] else 'Вернуть'}</button></form></li>"
            for m in mods) or "<li class='muted'>Модов нет.</li>") + "</ul>"
        users_html = ""
        if user["role"] == "administrator":
            def options_for(current: str) -> str:
                return "".join(f"<option value='{role}'{' selected' if role == current else ''}>{esc(label)}</option>"
                               for role, label in ROLE_LABELS.items())
            rows = "".join(
                f"<li><span class='grow'><b>{esc(u['display_name'])}</b> <span class='sub'>{esc(u['email'])}, {esc(ROLE_LABELS.get(u['role'], u['role']))}</span></span>"
                f"<form class='inline' method='post' action='/moderation/users/{u['id']}/role'>{csrf}<select name='role' aria-label='Роль'>{options_for(u['role'])}</select>"
                f"<button class='btn btn-sm' type='submit'>Назначить</button></form>"
                + (f"<form class='inline' method='post' action='/moderation/users/{u['id']}/delete'>{csrf}"
                   f"<button class='btn btn-sm btn-ghost' type='submit'>Удалить (почта не подтверждена)</button></form>"
                   if not u['email_verified'] else "") + "</li>" for u in db.execute("SELECT * FROM users ORDER BY id").fetchall())
            users_html = f"<section class='section'><h2>Роли</h2><ul class='rows'>{rows}</ul></section>"
        notice_html = ""
        if user["role"] == "administrator":
            notice_html = (f"<section class='section'><h2>Объявление на сайте</h2>"
                           f"<form method='post' action='/moderation/notice'>{csrf}"
                           f"<label class='field'><span>Текст (пусто - снять). Показывается на каждой странице и в index.json</span>"
                           f"<textarea name='text' rows='2' maxlength='{NOTICE_MAX}'>{esc(self.portal.get_setting(db, 'notice'))}</textarea></label>"
                           f"<button class='btn btn-sm' type='submit'>Сохранить</button></form></section>")
        self._page("Модерация", render("moderation", csrf=self.csrf, applications=applications_html, reports=reports_html,
                                       mods=mods_html, users=users_html + notice_html))

    def post_application(self, db: sqlite3.Connection, application_id: str, action: str) -> None:
        user = self._require_user("moderator")
        fields, _ = self._form()
        self._check_csrf(fields)
        application = db.execute("SELECT * FROM developer_applications WHERE id=?", (int(application_id),)).fetchone()
        if application is None or application["status"] != "pending":
            raise HttpError(404, "заявка не найдена")
        status = "approved" if action == "approve" else "rejected"
        db.execute("UPDATE developer_applications SET status=?, reviewed_by=? WHERE id=?", (status, user["id"], application["id"]))
        if status == "approved":
            db.execute("UPDATE users SET role='developer' WHERE id=? AND role='user'", (application["user_id"],))
        self.portal.audit(db, user["id"], f"application.{status}", str(application["user_id"]))
        self.portal.notify_user(db, application["user_id"],
                                "BlitzForge: заявка разработчика " + ("одобрена" if status == "approved" else "отклонена"),
                                "Ваша заявка на роль разработчика одобрена: можно публиковать моды из кабинета."
                                if status == "approved" else
                                "Ваша заявка на роль разработчика отклонена. Можно подать новую, описав планируемый мод подробнее.")
        self._done("/moderation", {"ok": True, "status": status}, f"Заявка: {status}")

    def post_mod_flag(self, db: sqlite3.Connection, mod_id: str, action: str) -> None:
        user = self._require_user("moderator")
        fields, _ = self._form()
        self._check_csrf(fields)
        if db.execute("SELECT 1 FROM mods WHERE id=?", (mod_id,)).fetchone() is None:
            raise HttpError(404, "мод не найден")
        column, value = {"verify": ("verified", 1), "unverify": ("verified", 0), "list": ("listed", 1), "unlist": ("listed", 0)}[action]
        db.execute(f"UPDATE mods SET {column}=?, updated_at=? WHERE id=?", (value, now_iso(), mod_id))
        if action == "verify":
            owner_id = db.execute("SELECT owner_id FROM mods WHERE id=?", (mod_id,)).fetchone()[0]
            db.execute("UPDATE users SET role='verified_developer' WHERE id=? AND role='developer'", (owner_id,))
        self.portal.audit(db, user["id"], f"mod.{action}", mod_id)
        owner_id = db.execute("SELECT owner_id FROM mods WHERE id=?", (mod_id,)).fetchone()[0]
        subject, body = {
            "verify": ("BlitzForge: мод {id} проверен", "Модератор пометил ваш мод {id} проверенным: его релизы подписывает портал."),
            "unverify": ("BlitzForge: с мода {id} снята отметка проверенного", "Модератор снял отметку проверенного с вашего мода {id}."),
            "unlist": ("BlitzForge: мод {id} скрыт из каталога", "Модератор скрыл ваш мод {id} из каталога. Релизы остаются доступны по прямой ссылке."),
            "list": ("BlitzForge: мод {id} снова в каталоге", "Модератор вернул ваш мод {id} в каталог."),
        }[action]
        self.portal.notify_user(db, owner_id, subject.format(id=mod_id), body.format(id=mod_id))
        self._done("/moderation", {"ok": True, "id": mod_id, column: bool(value)}, f"{mod_id}: {action}")

    def post_report_action(self, db: sqlite3.Connection, report_id: str, action: str) -> None:
        user = self._require_user("moderator")
        fields, _ = self._form()
        self._check_csrf(fields)
        status = "resolved" if action == "resolve" else "dismissed"
        updated = db.execute("UPDATE reports SET status=?, resolved_by=? WHERE id=? AND status='open'",
                             (status, user["id"], int(report_id))).rowcount
        if not updated:
            raise HttpError(404, "жалоба не найдена")
        self._done("/moderation", {"ok": True, "status": status}, f"Жалоба: {status}")

    def post_moderation_unpublish(self, db: sqlite3.Connection, release_id: str) -> None:
        user = self._require_user("moderator")
        fields, _ = self._form()
        self._check_csrf(fields)
        self.portal.unpublish_release(db, user, int(release_id))
        release = db.execute("SELECT r.mod_id, r.version, m.owner_id FROM releases r JOIN mods m ON m.id=r.mod_id WHERE r.id=?",
                             (int(release_id),)).fetchone()
        if release is not None and release["owner_id"] != user["id"]:
            self.portal.notify_user(db, release["owner_id"], f"BlitzForge: релиз {release['mod_id']} {release['version']} снят",
                                    f"Модератор снял релиз {release['mod_id']} {release['version']}. Установщики игроков перестанут его предлагать.")
        self._done("/moderation", {"ok": True, "release_id": int(release_id), "status": "unpublished"}, "Релиз снят")

    def post_role(self, db: sqlite3.Connection, user_id: str) -> None:
        admin = self._require_user("administrator")
        fields, _ = self._form()
        if not self.route_path.startswith("/api/"):
            self._check_csrf(fields)
        role = fields.get("role", "")
        if role not in ROLES:
            raise HttpError(400, "роль: " + "|".join(ROLES))
        if int(user_id) == admin["id"] and role != "administrator":
            raise HttpError(400, "администратор не может снять роль с себя")
        if not db.execute("UPDATE users SET role=? WHERE id=?", (role, int(user_id))).rowcount:
            raise HttpError(404, "пользователь не найден")
        self.portal.audit(db, admin["id"], "user.role", f"{user_id}={role}")
        self._done("/moderation", {"ok": True, "user_id": int(user_id), "role": role}, f"Роль назначена: {role}")

    def post_user_delete(self, db: sqlite3.Connection, user_id: str) -> None:
        # Spam cleanup: an administrator removes an account that never confirmed
        # its address. Verified accounts own mods, comments and ratings and are
        # not deleted here; that stays a manual, audited decision.
        admin = self._require_user("administrator")
        fields, _ = self._form()
        if not self.route_path.startswith("/api/"):
            self._check_csrf(fields)
        target = db.execute("SELECT * FROM users WHERE id=?", (int(user_id),)).fetchone()
        if target is None:
            raise HttpError(404, "пользователь не найден")
        if target["id"] == admin["id"]:
            raise HttpError(400, "администратор не может удалить себя")
        if target["email_verified"]:
            raise HttpError(409, "удаляются только аккаунты с неподтверждённой почтой")
        for table in ("sessions", "email_tokens", "api_tokens", "developer_applications"):
            db.execute(f"DELETE FROM {table} WHERE user_id=?", (target["id"],))
        db.execute("DELETE FROM users WHERE id=?", (target["id"],))
        self.portal.audit(db, admin["id"], "user.delete", f"{target['id']}:{target['email']}")
        self._done("/moderation", {"ok": True, "user_id": target["id"]}, f"Аккаунт удалён: {target['email']}")

    def post_takedown(self, db: sqlite3.Connection, mod_id: str) -> None:
        user = self._require_user("moderator")
        fields, _ = self._form()
        self._check_csrf(fields)
        result = self.portal.takedown(db, user, mod_id, fields.get("reason", "").strip() or "moderator takedown")
        self._done("/moderation", result, f"{mod_id}: снят из каталога")

    def api_audit(self, db: sqlite3.Connection, mod_id: str | None = None) -> None:
        self._require_user("moderator")
        if mod_id is not None and db.execute("SELECT 1 FROM mods WHERE id=?", (mod_id,)).fetchone() is None:
            raise HttpError(404, "mod not found")
        self._json(200, {"ok": True, "id": mod_id, "audit": self.portal.audit_rows(db, mod_id)})

    def api_mod_audit(self, db: sqlite3.Connection, mod_id: str) -> None:
        self.api_audit(db, mod_id)

    def post_crash(self, db: sqlite3.Connection, mod_id: str) -> None:
        """A player's `wotbmod report-crash`: what the loader's crash-loop guard recorded."""
        fields, _ = self._form()
        if db.execute("SELECT 1 FROM mods WHERE id=?", (mod_id,)).fetchone() is None:
            raise HttpError(404, "mod not found")
        version = fields.get("version", "").strip()
        client_build = fields.get("client_build", "").strip()
        try:
            count = int(fields.get("count", "1"))
        except ValueError as exc:
            raise HttpError(400, "count must be an integer") from exc
        if wotbmod._parse_semver(version) is None or wotbmod.CLIENT_BUILD_RE.fullmatch(client_build) is None or not 1 <= count <= 1000:
            raise HttpError(400, "version, client_build and count (1..1000) are required")
        ip = self.client_address[0]
        attempts = self.portal.login_attempts.setdefault("crash:" + ip, [])
        cutoff = time.time() - LOGIN_WINDOW
        attempts[:] = [stamp for stamp in attempts if stamp > cutoff]
        if len(attempts) >= LOGIN_LIMIT:
            raise HttpError(429, "too many crash reports from this address; try later")
        attempts.append(time.time())
        db.execute("INSERT INTO crashes(mod_id, version, client_build, count, reporter_id, client_ip, at) VALUES (?,?,?,?,?,?,?)",
                   (mod_id, version, client_build, count, self.user["id"] if self.user else None, ip, now_iso()))
        self._json(200, {"ok": True, "id": mod_id, "version": version, "client_build": client_build, "count": count})

    def api_compat(self, db: sqlite3.Connection) -> None:
        self._json(200, {"ok": True, "client_build": self.portal.client_build, "mods": self.portal.compat_overview(db)})

    def get_compat(self, db: sqlite3.Connection) -> None:
        labels = {"works": "работает", "broken": "сломан", "untested": "не проверялся"}
        rows = []
        for item in self.portal.compat_overview(db):
            builds = sorted(set(item["compat"]) | set(item["crashes"]))
            cells = []
            for build in builds:
                declared = item["compat"].get(build)
                crashed = item["crashes"].get(build)
                text = f"{labels.get(declared['status'], esc(declared['status']))} <span class='muted'>({esc(declared['by'])})</span>" if declared else "<span class='muted'>не заявлено</span>"
                if crashed:
                    text += f", крэшей {crashed['crashes']} в {crashed['reports']} отчётах"
                cells.append(f"<div><b>{esc(build)}</b>: {text}</div>")
            rows.append(f"<li><span class='grow'><a href='/mods/{esc(item['mod_id'])}'>{esc(item['mod_id'])}</a> <span class='sub'>{esc(item['version'])}</span></span>"
                        f"<span class='grow'>{''.join(cells)}</span></li>")
        body = render("compat", client_build=self.portal.client_build,
                      rows="<ul class='rows'>" + ("".join(rows) or "<li class='muted'>Пока нет ни заявлений, ни отчётов.</li>") + "</ul>")
        self._page("Совместимость и крэши", body)

    # -- JSON API ----------------------------------------------------------------------------------

    def api_index(self, db: sqlite3.Connection) -> None:
        self._json(200, self.portal.index(db), {"Cache-Control": "no-cache"})

    def api_legacy_mods(self, db: sqlite3.Connection) -> None:
        self._json(200, self.portal.legacy_mods(db), {"Cache-Control": "no-cache"})

    def api_mods(self, db: sqlite3.Connection) -> None:
        items = self.portal.catalogue(db, q=self.query.get("q", ""), type_filter=self.query.get("type", ""),
                                      tier=self.query.get("tier", "") if self.query.get("tier") in TIER_NAMES else "",
                                      verified_only=self.query.get("verified") == "1", sort=self.query.get("sort", "updated"))
        self._json(200, {"ok": True, "mods": [{key: item[key] for key in
                                                ("id", "name", "summary", "developer", "owner_id", "verified", "version", "type",
                                                 "tier", "rating", "votes", "downloads", "updated_at")} for item in items]})

    def api_mod(self, db: sqlite3.Connection, mod_id: str) -> None:
        mod, item = self._mod_or_404(db, mod_id)
        versions = {r["version"]: self.portal.public_record(r) for r in
                    db.execute("SELECT * FROM releases WHERE mod_id=? AND status='published'", (mod_id,)).fetchall()}
        compat = [dict(row) for row in db.execute("SELECT mod_id, version, client_build, status, created_at FROM compat WHERE mod_id=?",
                                                  (mod_id,)).fetchall()]
        self._json(200, {"ok": True, **{key: item[key] for key in
                                        ("id", "name", "summary", "description", "developer", "owner_id", "verified", "version",
                                         "type", "tier", "rating", "votes", "downloads", "updated_at")},
                         "latest": item["version"], "versions": versions, "compat": compat})

    def api_release(self, db: sqlite3.Connection, mod_id: str, version: str) -> None:
        release = db.execute("SELECT * FROM releases WHERE mod_id=? AND version=? AND status='published'", (mod_id, version)).fetchone()
        if release is None:
            raise HttpError(404, "release not found")
        self._json(200, {"ok": True, **self.portal.public_record(release)})

    def api_download(self, db: sqlite3.Connection, mod_id: str, version: str, what: str) -> None:
        release = db.execute("SELECT * FROM releases WHERE mod_id=? AND version=? AND status='published'", (mod_id, version)).fetchone()
        if release is None:
            raise HttpError(404, "release not found")
        # `artifact.sig` is what `wotbmod install <artifact url>` asks for next
        # to the artifact; `signature` is the name the index records.
        if what in ("signature", "artifact.sig"):
            if not release["signature_path"]:
                raise HttpError(404, "release is unsigned")
            path = self.portal.artifacts / release["signature_path"]
            self._send(200, path.read_bytes(), "text/plain; charset=utf-8",
                       {"Content-Disposition": f'attachment; filename="{path.name}"'})
            return
        path = self.portal.artifacts / release["artifact_path"]
        kind = "update" if self.query.get("update") == "1" else "download"
        db.execute("INSERT INTO downloads(release_id, kind, at, client_build) VALUES (?,?,?,?)",
                   (release["id"], kind, now_iso(), self.query.get("client")))
        db.execute(f"UPDATE releases SET {'updates' if kind == 'update' else 'downloads'}={'updates' if kind == 'update' else 'downloads'}+1 WHERE id=?",
                   (release["id"],))
        self._send(200, path.read_bytes(), "application/octet-stream",
                   {"Content-Disposition": f'attachment; filename="{path.name}"', "X-Package-SHA256": release["sha256"]})

    def api_stats(self, db: sqlite3.Connection, mod_id: str) -> None:
        if db.execute("SELECT 1 FROM mods WHERE id=?", (mod_id,)).fetchone() is None:
            raise HttpError(404, "mod not found")
        rows = db.execute("SELECT version, downloads, updates FROM releases WHERE mod_id=? ORDER BY id", (mod_id,)).fetchall()
        self._json(200, {"ok": True, "id": mod_id,
                         "versions": {r["version"]: {"downloads": r["downloads"], "updates": r["updates"]} for r in rows},
                         "downloads": sum(r["downloads"] for r in rows), "updates": sum(r["updates"] for r in rows)})

    def api_publish(self, db: sqlite3.Connection) -> None:
        """`wotbmod publish --to <base>/api/v1`: multipart record + artifact (+ signature)."""
        user = self._require_user("developer")
        fields, files = self._form()
        if "artifact" not in files:
            raise HttpError(400, "artifact file is required")
        record = None
        if fields.get("record"):
            try:
                record = json.loads(fields["record"])
            except ValueError as exc:
                raise HttpError(400, "record is not JSON") from exc
            if not isinstance(record, dict) or record.get("schema") != packages.RELEASE_SCHEMA:
                raise HttpError(400, "record has an unknown schema")
        signature = files.get("signature", (None, None))[1]
        release = self.portal.store_release(db, user, files["artifact"][1], signature,
                                            notes=str((record or {}).get("notes") or fields.get("notes", "")),
                                            publish=fields.get("publish", "1") != "0", record=record,
                                            filename=files["artifact"][0])
        self._json(200, {"ok": True, "id": release["mod_id"], "version": release["version"], "status": release["status"],
                         "sha256": release["sha256"], "url": f"{self.portal.base_url}/mods/{release['mod_id']}",
                         "review": "verified" if db.execute("SELECT verified FROM mods WHERE id=?", (release["mod_id"],)).fetchone()[0] else "unverified"})

    def api_apply(self, db: sqlite3.Connection) -> None:
        self.api_token_auth = True  # JSON callers authenticate with a session or a token; no HTML form CSRF
        self.post_apply(db)


def _routes() -> list[tuple[str, re.Pattern[str], str]]:
    table = [
        ("GET", r"/", "get_catalog"),
        ("GET", r"/static/((?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+)", "get_static"),
        ("GET", r"/favicon\.ico", "get_favicon"),
        ("GET", r"/docs", "get_docs"),
        ("GET", r"/download", "get_download"),
        ("GET", r"/download/([A-Za-z0-9._-]+\.(?:zip|exe))", "get_download_file"),
        ("GET", r"/mods/([^/]+)", "get_mod"),
        ("GET", r"/install/([^/]+)", "get_install"),
        ("GET", r"/developers/([^/]+)", "get_developer"),
        ("POST", r"/mods/([^/]+)/rate", "post_rate"),
        ("POST", r"/mods/([^/]+)/comments", "post_comment"),
        ("POST", r"/mods/([^/]+)/comments/(\d+)/edit", "post_comment_edit"),
        ("POST", r"/mods/([^/]+)/comments/(\d+)/delete", "post_comment_delete"),
        ("POST", r"/moderation/notice", "post_notice"),
        ("POST", r"/mods/([^/]+)/report", "post_report"),
        ("POST", r"/mods/([^/]+)/compat", "post_compat"),
        ("GET", r"/register", "get_register"), ("POST", r"/register", "post_register"),
        ("GET", r"/login", "get_login"), ("POST", r"/login", "post_login"),
        ("POST", r"/logout", "post_logout"),
        ("GET", r"/verify", "get_verify"),
        ("GET", r"/reset", "get_reset"), ("POST", r"/reset", "post_reset"),
        ("GET", r"/reset/([^/]+)", "get_reset_confirm"), ("POST", r"/reset/([^/]+)", "post_reset_confirm"),
        ("GET", r"/account", "get_account"),
        ("POST", r"/account/password", "post_password"),
        ("POST", r"/account/apply", "post_apply"),
        ("GET", r"/dashboard", "get_dashboard"),
        ("POST", r"/dashboard/mods", "post_create_mod"),
        ("POST", r"/dashboard/upload", "post_upload"),
        ("POST", r"/dashboard/releases/(\d+)/(publish|unpublish)", "post_release_action"),
        ("POST", r"/dashboard/tokens", "post_create_token"),
        ("GET", r"/moderation", "get_moderation"),
        ("POST", r"/moderation/applications/(\d+)/(approve|reject)", "post_application"),
        ("POST", r"/moderation/mods/([^/]+)/(verify|unverify|list|unlist)", "post_mod_flag"),
        ("POST", r"/moderation/reports/(\d+)/(resolve|dismiss)", "post_report_action"),
        ("POST", r"/moderation/releases/(\d+)/unpublish", "post_moderation_unpublish"),
        ("POST", r"/moderation/users/(\d+)/role", "post_role"),
        ("POST", r"/moderation/users/(\d+)/delete", "post_user_delete"),
        ("POST", r"/moderation/mods/([^/]+)/takedown", "post_takedown"),
        ("GET", r"/compat", "get_compat"),
        # JSON API
        ("GET", r"/api/v1/index\.json", "api_index"),
        ("GET", r"/api/mods", "api_legacy_mods"),
        ("GET", r"/api/v1/mods", "api_mods"),
        ("GET", r"/api/v1/mods/([^/]+)", "api_mod"),
        ("GET", r"/api/v1/mods/([^/]+)/releases/([^/]+)", "api_release"),
        ("GET", r"/api/v1/releases/([^/]+)/([^/]+)/(artifact|signature|artifact\.sig)", "api_download"),
        ("GET", r"/api/v1/stats/([^/]+)", "api_stats"),
        ("POST", r"/api/v1/releases", "api_publish"),
        ("POST", r"/api/v1/auth/register", "post_register"),
        ("POST", r"/api/v1/auth/login", "post_login"),
        ("POST", r"/api/v1/auth/logout", "post_logout"),
        ("POST", r"/api/v1/auth/refresh", "post_refresh"),
        ("POST", r"/api/v1/auth/verify", "post_verify_api"),
        ("POST", r"/api/v1/auth/password-reset/request", "post_reset"),
        ("POST", r"/api/v1/auth/password-reset/confirm", "post_reset_confirm_api"),
        ("POST", r"/api/v1/developer/apply", "api_apply"),
        ("POST", r"/api/v1/mods/([^/]+)/compat", "post_compat"),
        ("POST", r"/api/v1/mods/([^/]+)/rating", "post_rate"),
        ("POST", r"/api/v1/mods/([^/]+)/comments", "post_comment"),
        ("POST", r"/api/v1/mods/([^/]+)/comments/(\d+)/edit", "post_comment_edit"),
        ("POST", r"/api/v1/mods/([^/]+)/comments/(\d+)/delete", "post_comment_delete"),
        ("POST", r"/api/v1/moderation/notice", "post_notice"),
        ("POST", r"/api/v1/mods/([^/]+)/reports", "post_report"),
        ("POST", r"/api/v1/moderation/applications/(\d+)/(approve|reject)", "post_application"),
        ("POST", r"/api/v1/moderation/mods/([^/]+)/(verify|unverify|list|unlist)", "post_mod_flag"),
        ("POST", r"/api/v1/moderation/reports/(\d+)/(resolve|dismiss)", "post_report_action"),
        ("POST", r"/api/v1/moderation/releases/(\d+)/unpublish", "post_moderation_unpublish"),
        ("POST", r"/api/v1/moderation/users/(\d+)/role", "post_role"),
        ("POST", r"/api/v1/moderation/users/(\d+)/delete", "post_user_delete"),
        ("POST", r"/api/v1/moderation/mods/([^/]+)/takedown", "post_takedown"),
        ("GET", r"/api/v1/audit", "api_audit"),
        ("GET", r"/api/v1/mods/([^/]+)/audit", "api_mod_audit"),
        ("POST", r"/api/v1/mods/([^/]+)/crashes", "post_crash"),
        ("GET", r"/api/v1/compat", "api_compat"),
    ]
    return [(method, re.compile(pattern), name) for method, pattern, name in table]


Handler.ROUTES = _routes()


class PortalServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], portal: Portal, *, quiet: bool = False) -> None:
        handler = type("BoundHandler", (Handler,), {"portal": portal})
        super().__init__(address, handler)
        self.portal = portal
        self.quiet = quiet


def make_server(portal: Portal, host: str = "127.0.0.1", port: int = 0, *, quiet: bool = False) -> PortalServer:
    return PortalServer((host, port), portal, quiet=quiet)


# --- command line -------------------------------------------------------------------------------

DEFAULT_CONFIG = {
    "host": "127.0.0.1",
    "port": 8080,
    "base_url": "http://127.0.0.1:8080",
    "data_dir": "data",
    "signing_key": "",
    "key_id": "",
    "smtp": "",
    "smtp_user": "",
    "smtp_password": "",
    "mail_from": "",
    "client_build": "",
    "admin_email": "",
    "admin_password": "",
    "quiet": False,
}


def load_config(path: Path | None) -> dict[str, Any]:
    """config.json next to the portal (see config.example.json); missing keys take defaults.

    Relative paths inside the file (`data_dir`, `signing_key`) are resolved from the
    file's own folder, so the same config works from any working directory.
    """
    config = dict(DEFAULT_CONFIG)
    if path is None:
        return config
    if not path.is_file():
        raise SystemExit(f"config not found: {path} (copy config.example.json to config.json)")
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        raise SystemExit(f"config is not valid JSON: {path}: {exc}") from exc
    if not isinstance(loaded, dict):
        raise SystemExit("config must be a JSON object")
    unknown = set(loaded) - set(DEFAULT_CONFIG)
    if unknown:
        raise SystemExit(f"config has unknown keys: {', '.join(sorted(unknown))}")
    config.update(loaded)
    base = path.resolve().parent
    for key in ("data_dir", "signing_key"):
        value = config.get(key)
        if value and not Path(value).is_absolute():
            config[key] = str(base / value)
    return config


def portal_from_config(config: dict[str, Any]) -> Portal:
    signing_key = Path(config["signing_key"]) if config.get("signing_key") else None
    if signing_key is not None and not signing_key.is_file():
        raise SystemExit(f"signing key not found: {signing_key} (run: wotbmod keygen --out {signing_key} --key-id <id>)")
    portal = Portal(Path(config["data_dir"]).expanduser(), config["base_url"], signing_key=signing_key,
                    key_id=config.get("key_id") or None, smtp=config.get("smtp") or None,
                    client_build=config.get("client_build") or None,
                    smtp_user=config.get("smtp_user") or None,
                    smtp_password=config.get("smtp_password") or None,
                    mail_from=config.get("mail_from") or None)
    if config.get("admin_email") and config.get("admin_password"):
        ensure_admin(portal, config["admin_email"], config["admin_password"], "admin")
    return portal


def ensure_admin(portal: Portal, email_address: str, password: str, name: str) -> str:
    """Create the administrator, or make sure the existing account with that mail is one."""
    db = portal.connect()
    try:
        existing = db.execute("SELECT id, role FROM users WHERE email=?", (email_address.lower(),)).fetchone()
        if existing:
            if existing["role"] != "administrator":
                db.execute("UPDATE users SET role='administrator', email_verified=1 WHERE id=?", (existing["id"],))
                return "promoted"
            return "present"
        db.execute("INSERT INTO users(email, display_name, password_hash, role, email_verified, created_at) VALUES (?,?,?,?,?,?)",
                   (email_address.lower(), name, hash_password(password), "administrator", 1, now_iso()))
        return "created"
    finally:
        db.close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="portal_server", description="BlitzForge mod portal: catalogue, publishing, install links")
    parser.add_argument("--config", help="config.json (default: <portal>/config.json when it exists)")
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--data", help="data directory (overrides config data_dir)")
        p.add_argument("--base-url", help="public address of the site (overrides config)")

    init = sub.add_parser("init", help="create the database and folders")
    common(init)
    admin = sub.add_parser("create-admin", help="create (or promote) an administrator")
    common(admin)
    admin.add_argument("--email", required=True)
    admin.add_argument("--password", required=True)
    admin.add_argument("--name", default="admin")
    serve = sub.add_parser("serve", help="run the portal")
    common(serve)
    serve.add_argument("--host")
    serve.add_argument("--port", type=int)
    serve.add_argument("--signing-key", help="portal P-256 private key file (wotbmod keygen); signs verified developers' releases")
    serve.add_argument("--key-id")
    serve.add_argument("--smtp", help="host:port for real mail; default writes <data>/outbox/*.eml")
    serve.add_argument("--client-build", help="client build the catalogue is for (default: the SDK's)")
    serve.add_argument("--open", action="store_true", help="open the site in the default browser once it is up")
    serve.add_argument("--quiet", action="store_true", help="no request log lines")
    args = parser.parse_args(argv)

    config_path = Path(args.config).expanduser() if args.config else (PORTAL_ROOT / "config.json")
    config = load_config(config_path if (args.config or config_path.is_file()) else None)
    for key, value in (("data_dir", args.data), ("base_url", args.base_url), ("host", getattr(args, "host", None)),
                       ("port", getattr(args, "port", None)), ("signing_key", getattr(args, "signing_key", None)),
                       ("key_id", getattr(args, "key_id", None)), ("smtp", getattr(args, "smtp", None)),
                       ("client_build", getattr(args, "client_build", None))):
        if value not in (None, ""):
            config[key] = value
    if getattr(args, "quiet", False):
        config["quiet"] = True

    portal = portal_from_config(config)
    if args.command == "init":
        print(f"database: {portal.db_path}")
        print(f"artifacts: {portal.artifacts}")
        print(f"outbox: {portal.outbox}")
        return 0
    if args.command == "create-admin":
        state = ensure_admin(portal, args.email, args.password, args.name)
        print({"created": f"created administrator {args.email}", "promoted": f"promoted {args.email} to administrator",
               "present": f"{args.email} is already an administrator"}[state])
        return 0
    server = make_server(portal, config["host"], int(config["port"]), quiet=bool(config.get("quiet")))
    address = f"http://{config['host']}:{server.server_address[1]}"
    print(f"BlitzForge portal: {address}  (public address {portal.base_url}, data {portal.data_dir})")
    if portal.signing_key is None:
        print("note: no signing key configured; verified developers' releases keep their own signatures")
    if args.open:
        import webbrowser
        threading.Timer(0.6, lambda: webbrowser.open(address)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
