"""Regression tests for V3 package installation and mods.ini grants."""
from __future__ import annotations

import configparser
import tempfile
from pathlib import Path

import install
import modpack


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str.lower
    parser.read(path, encoding="utf-8")
    return parser


with tempfile.TemporaryDirectory(prefix="blitzforge-installer-test-") as root:
    temporary = Path(root)
    source = temporary / "source"
    payload = source / "files" / "mods" / "wotbmod.native_validation"
    payload.mkdir(parents=True)
    (source / "manifest.yaml").write_text(
        "\n".join((
            "id: native-validation",
            "version: 1.0.0",
            "type: native",
            "loader_id: wotbmod.native_validation",
            "loader_enabled: true",
            "loader_permission_tier: 3",
            "")),
        encoding="utf-8")
    (payload / "manifest.json").write_text("{}\n", encoding="utf-8")
    artifact = temporary / "native-validation.zip"
    modpack.build(source, artifact)
    metadata = modpack.read_metadata(artifact)
    assert metadata["loader"] == {
        "id": "wotbmod.native_validation",
        "enabled": True,
        "permission_tier": 3,
    }

    settings = temporary / "mods.ini"
    settings.write_text(
        "[mods]\nlegacy_mod=0\n\n[permissions]\nlegacy_mod=2\n",
        encoding="utf-8")
    install.configure_loader(metadata["loader"], settings)
    configured = read_ini(settings)
    assert configured.get("mods", "legacy_mod") == "0"
    assert configured.get("permissions", "legacy_mod") == "2"
    assert configured.get("mods", "wotbmod.native_validation") == "1"
    assert configured.get("permissions", "wotbmod.native_validation") == "3"

    install.remove_loader_config(metadata["loader"], settings)
    removed = read_ini(settings)
    assert removed.get("mods", "legacy_mod") == "0"
    assert removed.get("permissions", "legacy_mod") == "2"
    assert not removed.has_option("mods", "wotbmod.native_validation")
    assert not removed.has_option("permissions", "wotbmod.native_validation")

    installed = {"version": "1.2.3", "targets": []}
    for offered in ("1.2.3", "1.2.2", "1.1.99"):
        try:
            install.ensure_installable_version("night-mode", offered, installed)
        except SystemExit:
            pass
        else:
            raise AssertionError(f"installer accepted non-upgrade {offered}")
    install.ensure_installable_version("night-mode", "1.2.4", installed)

    bootstrap_source = temporary / "bootstrap-source"
    bootstrap_source.mkdir()
    proxy_source = bootstrap_source / "version.dll"
    loader_source = bootstrap_source / "wotb_mod_loader.dll"
    system_forwarder = bootstrap_source / "system-version.dll"
    proxy_source.write_bytes(b"wotb_mod proxy loaded:proxy-v1")
    loader_source.write_bytes(b"BlitzForge live loader starting:loader-v1")
    system_forwarder.write_bytes(b"system-v1")
    game_root = temporary / "game"
    install.ensure_native_bootstrap(
        game_root, proxy_source, loader_source, system_forwarder)
    assert (game_root / "version.dll").read_bytes() == (
        b"wotb_mod proxy loaded:proxy-v1")
    assert (game_root / "wotb_mod_loader.dll").read_bytes() == (
        b"BlitzForge live loader starting:loader-v1")
    assert (game_root / "vorig.dll").read_bytes() == b"system-v1"

    proxy_source.write_bytes(b"wotb_mod proxy loaded:proxy-v2")
    install.ensure_native_bootstrap(
        game_root, proxy_source, loader_source, system_forwarder)
    assert (game_root / "version.dll").read_bytes() == (
        b"wotb_mod proxy loaded:proxy-v2")

    (game_root / "version.dll").write_bytes(b"foreign-proxy")
    try:
        install.ensure_native_bootstrap(
            game_root, proxy_source, loader_source, system_forwarder)
    except SystemExit:
        pass
    else:
        raise AssertionError("installer overwrote a foreign VERSION proxy")

print("installer V3 loader config/version guard: all passing")
