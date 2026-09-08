"""The SDK's generated and declarative surfaces stay in step with the code.

- docs/API_REFERENCE_RU.md is what tools/generate_api_reference.py renders
  from the headers today (a header change without a regenerate fails here);
- every shipped Lua example's manifest satisfies schemas/wotbmod-lua-manifest-
  v1.schema.json and asks only for registered permissions;
- every `wotbmod new --type lua` template names a shipped example, and the
  facade-first examples the quick start advertises exist;
- the VS Code snippets file is valid JSON with the fields the editor reads;
- the quick start links only to documents that exist.

Stdlib only, like the rest of tools/ and tests/: the JSON Schema subset used
by the two schemas (type, required, properties, pattern, items, enum,
uniqueItems, maxLength, additionalProperties as a schema) is checked by the
small validator below rather than by a library the SDK does not depend on.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import sys
import unittest

MOD_API = pathlib.Path(__file__).resolve().parents[1]
TOOLS = MOD_API / "tools"
sys.path.insert(0, str(TOOLS))


def load_tool(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # Registered before execution: the dataclasses in tools/*.py resolve
    # their annotations through sys.modules[<name>].
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def validate(instance, schema, path="$") -> list[str]:
    """A subset of JSON Schema, enough for the SDK's own schemas."""
    problems: list[str] = []
    expected = schema.get("type")
    if expected is not None:
        types = expected if isinstance(expected, list) else [expected]
        matched = False
        for kind in types:
            if kind == "object" and isinstance(instance, dict):
                matched = True
            elif kind == "array" and isinstance(instance, list):
                matched = True
            elif kind == "string" and isinstance(instance, str):
                matched = True
            elif kind == "integer" and isinstance(instance, int) and not isinstance(instance, bool):
                matched = True
            elif kind == "number" and isinstance(instance, (int, float)) and not isinstance(instance, bool):
                matched = True
            elif kind == "boolean" and isinstance(instance, bool):
                matched = True
        if not matched:
            problems.append(f"{path}: expected {expected}")
            return problems
    if "enum" in schema and instance not in schema["enum"]:
        problems.append(f"{path}: not one of {schema['enum']}")
    if isinstance(instance, str):
        if "pattern" in schema and re.search(schema["pattern"], instance) is None:
            problems.append(f"{path}: does not match {schema['pattern']}")
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            problems.append(f"{path}: longer than {schema['maxLength']}")
    if "not" in schema and not validate(instance, schema["not"], path):
        problems.append(f"{path}: matches a forbidden shape")
    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                problems.append(f"{path}: missing {key}")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        names = schema.get("propertyNames")
        for key, value in instance.items():
            if names is not None:
                problems.extend(validate(key, names, f"{path}.{key}<name>"))
            if key in properties:
                problems.extend(validate(value, properties[key], f"{path}.{key}"))
            elif additional is False:
                problems.append(f"{path}: unexpected {key}")
            elif isinstance(additional, dict):
                problems.extend(validate(value, additional, f"{path}.{key}"))
    if isinstance(instance, list):
        if "items" in schema:
            for index, item in enumerate(instance):
                problems.extend(validate(item, schema["items"], f"{path}[{index}]"))
        if schema.get("uniqueItems") and len(set(map(json.dumps, instance))) != len(instance):
            problems.append(f"{path}: duplicate items")
    return problems


class SdkDocsTests(unittest.TestCase):
    def test_api_reference_is_current(self) -> None:
        reference = load_tool("generate_api_reference")
        model = reference.HeaderModel(reference._default_include_dir())
        expected = reference.render(model)
        current = (MOD_API / "docs" / "API_REFERENCE_RU.md").read_text(encoding="utf-8")
        self.assertEqual(current.replace("\r\n", "\n"), expected,
                         "run: python tools/generate_api_reference.py")

    def test_lua_example_manifests_match_the_schema(self) -> None:
        schema = json.loads(
            (MOD_API / "schemas" / "wotbmod-lua-manifest-v1.schema.json").read_text(encoding="utf-8")
        )
        wotbmod = load_tool("wotbmod")
        examples = sorted((MOD_API / "examples").glob("lua_*/manifest.json"))
        self.assertGreaterEqual(len(examples), 5)
        for manifest_path in examples:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(validate(manifest, schema), [], str(manifest_path))
            for permission in manifest["permissions"]:
                self.assertIn(permission, wotbmod.REGISTERED_PERMISSIONS, str(manifest_path))

    def test_config_schema_accepts_the_documented_shape(self) -> None:
        schema = json.loads(
            (MOD_API / "schemas" / "wotbmod-config-schema-v1.schema.json").read_text(encoding="utf-8")
        )
        good = {
            "volume": {"type": "number", "default": 0.5, "min": 0, "max": 1},
            "mode": {"type": "string", "default": "compact", "values": ["compact", "full"]},
            "enabled": {"type": "boolean", "default": True},
        }
        self.assertEqual(validate(good, schema), [])
        bad = {"volume": {"type": "float", "default": 0.5}}
        self.assertNotEqual(validate(bad, schema), [])
        missing_default = {"volume": {"type": "number"}}
        self.assertNotEqual(validate(missing_default, schema), [])

    def test_lua_templates_are_shipped_examples(self) -> None:
        wotbmod = load_tool("wotbmod")
        for template, example in wotbmod.LUA_TEMPLATES.items():
            root = MOD_API / "examples" / example
            self.assertTrue((root / "manifest.json").is_file(), template)
            self.assertTrue((root / "main.lua").is_file(), template)
            self.assertTrue((root / "README_RU.md").is_file(), template)
        for facade_first in ("lua_facade_tour", "lua_facade_panel", "lua_facade_battle",
                             "lua_hud_tweaks", "lua_skin_switcher"):
            self.assertIn(facade_first, wotbmod.LUA_TEMPLATES.values())

    def test_snippets_are_valid(self) -> None:
        snippets = json.loads(
            (MOD_API / "sdk" / "vscode" / "wotbmod.code-snippets").read_text(encoding="utf-8")
        )
        self.assertGreaterEqual(len(snippets), 5)
        for name, snippet in snippets.items():
            self.assertIn("prefix", snippet, name)
            self.assertIn("body", snippet, name)
            self.assertIsInstance(snippet["body"], list, name)
            self.assertIn(snippet.get("scope"), ("lua", "json"), name)

    def test_quick_start_links_exist(self) -> None:
        text = (MOD_API / "docs" / "QUICKSTART_RU.md").read_text(encoding="utf-8")
        for target in re.findall(r"\]\(([A-Za-z0-9_./-]+\.md)\)", text):
            self.assertTrue((MOD_API / "docs" / target).is_file(), target)
        for template in ("hello", "tour", "panel", "battle", "hud", "vehicle"):
            self.assertIn(f"`{template}`", text)


if __name__ == "__main__":
    unittest.main()
