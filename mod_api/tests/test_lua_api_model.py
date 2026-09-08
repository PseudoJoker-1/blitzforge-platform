import json
import pathlib
import re
import sys
import unittest


MOD_API = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MOD_API / "tools"))

from generate_lua_bindings import (  # noqa: E402
    CALLBACK_RELEASE_INPUTS,
    COUNT_PARAMETER_PREFIX,
    CUSTOM_SLOTS,
    GENERIC_RELEASE_INPUTS,
    NATIVE_ONLY,
    NON_ELEMENT_TYPES,
    PERMISSIONS,
    RETAIN_INPUTS,
    SIZE_LIMITS,
    Generator,
    base_type,
    clean_type,
    constant_tables,
    excluded_constants,
    is_handle,
    is_input_array_field_pair,
    is_output_array_pair,
    symbol,
)
from lua_api_model import HeaderModel, INTERFACES  # noqa: E402
from wotbmod import REGISTERED_PERMISSIONS, UNSAFE_PERMISSIONS  # noqa: E402


EXPECTED_SLOT_COUNTS = {
    "archive": 10,
    "async": 20,
    "audio": 49,
    "bigworld_rpc": 3,
    "camera": 14,
    "capabilities": 5,
    "catalog": 3,
    "client": 5,
    "content": 7,
    "core": 8,
    "device": 3,
    "devtools": 15,
    "diagnostics": 8,
    "entity_public": 8,
    "events": 9,
    "gameplay_camera": 30,
    "gameplay_hangar": 11,
    "gameplay_hud": 34,
    "gameplay_replay": 9,
    "handles": 4,
    "hooks": 16,
    "http": 13,
    "input": 12,
    "intermod": 11,
    "lifecycle": 13,
    "loaders": 7,
    "manifest": 15,
    "permissions": 4,
    "projectile": 14,
    "render": 19,
    "render_native": 3,
    "resources": 11,
    "scene": 28,
    "settings": 19,
    "storage": 14,
    "ui": 74,
    "unsafe_native": 1,
    "vehicle_visual": 35,
    "vfs": 18,
    "yaml": 11,
    # Post-RC1 contract, 2026-08-16: five new interfaces published alongside
    # the frozen tables rather than widening them. Declaration-only for now -
    # every slot answers NOT_SUPPORTED and the runtime registers each
    # interface UNAVAILABLE, so no wotb.* table appears until its backend
    # lands. These counts move again when it does, and that is the point of
    # pinning them.
    "ui_read": 5,
    "camera_state": 3,
    "audio_intercept": 4,
    "scene_enumerate": 2,
    "tracer": 2,
    "ges": 10,
    # API 1.1, 2026-09-08.
    "session_cluster": 3,
}


# Every constant the generator publishes, per wotb.* table. Pinned exactly:
# the failure this guards against is a constant quietly disappearing, and a
# disappeared constant reads as nil in Lua, which turns every comparison
# against it into a comparison that never fires and never errors. An interface
# absent from this map publishes none - handles, unsafe_native, render_native,
# archive and devtools declare no constants of their own.
EXPECTED_CONSTANT_COUNTS = {
    "async": 6,
    "audio": 10,
    "bigworld_rpc": 2,
    "camera": 10,
    "capabilities": 6,
    "catalog": 4,
    "client": 5,
    "content": 6,
    "core": 6,
    "device": 5,
    "diagnostics": 14,
    "entity_public": 17,
    "events": 127,
    "gameplay_camera": 10,
    "gameplay_hangar": 9,
    "gameplay_hud": 19,
    "gameplay_replay": 5,
    "hooks": 15,
    "http": 8,
    "input": 21,
    "intermod": 3,
    "lifecycle": 16,
    "loaders": 6,
    "manifest": 10,
    "permissions": 8,
    "projectile": 30,
    "render": 28,
    "resources": 16,
    "scene": 11,
    "settings": 18,
    "storage": 5,
    "ui": 57,
    "vehicle_visual": 14,
    "vfs": 9,
    "yaml": 8,
    # Post-RC1 contract, 2026-08-16.
    "ui_read": 10,
    "camera_state": 11,
    "audio_intercept": 3,
    "scene_enumerate": 10,
    "tracer": 13,
    "ges": 15,
    # API 1.1, 2026-09-08: AUTO plus the four change statuses.
    "session_cluster": 5,
}

# Every slot the emitter turns into a counted output array, and the element
# type it allocates. Pinned exactly, because this classification is invisible
# from the outside: a binding that decides a slot is an array passes nullptr for
# the parameter it mistook for the array, drops whatever that parameter really
# was, and hands the script a table. It compiles, it is type-correct, and it is
# wrong. settings.get_bool shipped that way - `const char* key, uint32_t*
# out_value` read as an array of chars, so the key never reached the client.
#
# Every entry here pairs a real element type with a count the ABI spells
# `inout_count`, which is the only shape the emitter's two-pass probe fits.
EXPECTED_OUTPUT_ARRAY_SLOTS = {
    "gameplay_camera.get_zoom_steps": "float",
    "hooks.enumerate": "WotbModV3HookInfo",
    "hooks.get_chain": "WotbModV3HookInfo",
    "hooks.get_conflicts": "WotbModV3HookConflictInfo",
    "input.find_conflicts": "WotbModV3InputConflict",
    "input.get_bindings": "WotbModV3InputBinding",
    "intermod.enumerate_interfaces": "WotbModV3ExportedInterfaceInfo",
    "vfs.get_conflicts": "WotbModV3VfsConflict",
    "session_cluster.enumerate": "WotbModV3ClusterInfo",
    "vfs.get_providers": "WotbModV3VfsProvider",
    "vfs.list": "WotbModV3VfsListEntry",
    # Post-RC1 contract, 2026-08-16. The scene walk is here rather than in
    # UNMARSHALLED_OUTPUT_BUFFERS because its buffer is a real parameter pair
    # instead of a pointer buried in a request struct - which is exactly the
    # change that made it safe to publish to Lua at all.
    "scene_enumerate.walk_active_scene": "WotbModV3SceneNodeRecord",
}

# Every struct FIELD the emitter reads as a counted array, and the element type
# and count field it is paired with. Pinned exactly, for the reason the shape
# needs pinning at all: the field and its count are two independent values, and
# a reader that takes the count from the script while allocating for the array
# hands the client a length it never allocated. That shipped -
# WotbModV3SettingPreset.values was one arena element next to a value_count the
# script chose freely, so `{values = {v}, value_count = 512}` walked 511
# elements past the end of a 1-element block inside SettingsRegisterPreset.
#
# A new entry here is a new struct the ABI grew, and it is only correct if the
# emitted reader allocates N and reports N. A *missing* entry is the regression.
EXPECTED_INPUT_ARRAY_FIELDS = {
    "WotbModV3InputActionDesc.default_bindings": (
        "WotbModV3InputBinding", "default_binding_count",
    ),
    "WotbModV3SettingPreset.values": (
        "WotbModV3SettingPresetValue", "value_count",
    ),
    "WotbModV3VehicleSkinPack.assets": (
        "WotbModV3VehicleSkinAsset", "asset_count",
    ),
}

# Struct fields that really are a pointer to exactly one element even though a
# uint32_t is declared next to them. Empty, and the emptiness is the point: no
# such field exists in the current headers, so any const pointer-to-struct with
# a uint32_t after it is a counted array and is read as one. A header that grows
# a genuine single-element pointer beside an unrelated number lands here, by
# hand, after somebody has confirmed the client does not walk it - which is
# exactly the review this list exists to force.
SINGLE_ELEMENT_STRUCT_POINTERS = frozenset()

# The mirror shape, and the more dangerous one: a NON-const pointer to elements,
# beside a capacity. The caller does not fill that array - it lends the client a
# buffer and the client WRITES into it, so a capacity larger than the allocation
# is a heap overflow rather than an over-read.
#
# This host has no way to marshal one. Honouring it means allocating the
# capacity, calling, and pushing the written prefix back to Lua - a nested
# output array, which is a feature and not a fix, and nothing like the counted
# input arrays the reader knows how to build. Until that feature exists, the
# only safe state is the one asserted below: nothing a script can call reads a
# struct that contains one. The single element the reader would otherwise
# allocate for `nodes` is a 1-record buffer handed to a client told it has
# `node_capacity` records to write.
#
# {"Struct.field": "capacity field"}, and the test proves unreachability, not
# good intentions: publishing scene V2 through INTERFACES turns this list red.
# Empty, and it stayed empty by changing the ABI rather than by adding a
# marshalling feature. scene_v2's first draft put `WotbModV3SceneNodeRecord*
# nodes` + `node_capacity` inside WotbModV3SceneWalkRequest and this list
# caught it before the interface was published to Lua. The buffer moved out of
# the struct and became an ordinary (out_nodes, inout_count) parameter pair -
# the shape vfs.list and hooks.enumerate already use - so the emitter's
# two-pass probe allocates exactly what it reports and there is nothing left
# to marshal by hand.
UNMARSHALLED_OUTPUT_BUFFERS = {}

# {"NAME", WOTBMOD_V3_MACRO}, as the hand-written tables spell it.
_NAMED_CONSTANT_RE = re.compile(r'\{"(?P<name>[A-Z0-9_]+)",\s*(?P<macro>WOTBMOD_V3_\w+)\}')

_BINDING_RE = re.compile(r"^int (?P<name>Bind_\w+)\(lua_State\* state\) noexcept \{$")

_READER_RE = re.compile(r"^bool (?P<name>Read_\w+)\(lua_State\* state,.*\{$")

def _binding_bodies(source):
    """Every generated Bind_* function, by name, as its own block of text."""
    bodies = {}
    name = None
    lines = []
    for line in source.splitlines():
        match = _BINDING_RE.match(line)
        if match:
            name = match.group("name")
            lines = [line]
            continue
        if name is None:
            continue
        lines.append(line)
        if line == "}":
            bodies[name] = "\n".join(lines)
            name = None
    return bodies


def _emitted_output_arrays(source):
    """{Bind_* name: element type} for every binding emitted as an array.

    emit_output_array_slot is the only emitter that writes this bound check,
    and the sizeof it divides by names the element type it is about to
    allocate. Reading the emitted C++ rather than re-running the classifier
    means a future rewrite of the decision ladder is still measured by what it
    ships.
    """
    arrays = {}
    for name, body in _binding_bodies(source).items():
        if "client returned an unreasonable array size" not in body:
            continue
        element = re.search(r"SIZE_MAX / sizeof\((?P<element>[^)]+)\)\)\)", body)
        arrays[name] = element.group("element")
    return arrays


def _struct_readers(source):
    """Every generated Read_* function, by name, as its own block of text."""
    readers = {}
    name = None
    lines = []
    for line in source.splitlines():
        match = _READER_RE.match(line)
        if match:
            name = match.group("name")
            lines = [line]
            continue
        if name is None:
            continue
        lines.append(line)
        if line == "}":
            readers[name] = "\n".join(lines)
            name = None
    return readers


def _hand_written_constants(path, table):
    text = path.read_text(encoding="utf-8")
    start = text.index(table)
    return {
        match.group("name"): match.group("macro")
        for match in _NAMED_CONSTANT_RE.finditer(text[start : text.index("};", start)])
    }


class LuaApiModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = HeaderModel(MOD_API / "include" / "wotbmod")

    def test_catalogue_matches_every_current_interface_id(self):
        interface_ids = (MOD_API / "include" / "wotbmod" / "interface_ids.h").read_text(encoding="utf-8")
        declared = {
            line.split()[1]
            for line in interface_ids.splitlines()
            if line.startswith("#define WOTBMOD_V3_IFACE_") and "VERSION" not in line
        }
        catalogued = {interface.interface_macro for interface in INTERFACES}
        self.assertEqual(declared, catalogued)
        self.assertEqual(47, len(INTERFACES))

    def test_structural_slot_inventory_is_stable(self):
        actual = {
            interface.lua_name: len(self.model.flatten_slots(interface))
            for interface in INTERFACES
        }
        self.assertEqual(EXPECTED_SLOT_COUNTS, actual)
        self.assertEqual(622, sum(actual.values()))

    def test_typedef_function_fields_and_embedded_prefixes_are_counted(self):
        slots = {
            (slot.interface.lua_name, slot.field_path)
            for slot in self.model.all_slots()
        }
        self.assertIn(("core", "get_game_directory"), slots)
        self.assertIn(("lifecycle", "get_install_path"), slots)
        self.assertIn(("devtools", "inspect_ui"), slots)
        self.assertIn(("ui", "v2.control_create"), slots)
        self.assertIn(("ui", "control_get_snapshot"), slots)
        self.assertIn(("vehicle_visual", "v1.set_part_visible"), slots)
        self.assertIn(("vehicle_visual", "skin_pack_release"), slots)

    def test_every_current_slot_has_a_real_lua_policy(self):
        generator = Generator(self.model)
        source = generator.generate()
        self.assertEqual(NATIVE_ONLY, set(generator.unsupported))
        self.assertEqual(569, 622 - len(CUSTOM_SLOTS) - len(generator.unsupported))
        self.assertIn(
            "Read_WotbModV3UiStylePatch(state, 2, &patch, "
            "WOTBMOD_V3_UI_VERSION, &arena)",
            source,
        )
        self.assertIn(
            "out_snapshot.api_version = WOTBMOD_V3_UI_VERSION_3",
            source,
        )
        self.assertNotIn("callback bridge is not generated yet", source)
        self.assertNotIn("pointer shape", source)
        self.assertNotIn("counted input-array bridge", source)
        self.assertNotIn("counted output-array bridge", source)

    def test_generated_handle_ownership_policy_is_exhaustive(self):
        generator = Generator(self.model)
        source = generator.generate()
        slots = {
            f"{interface.lua_name}.{slot.name}": slot
            for interface in INTERFACES
            for slot in self.model.flatten_slots(interface)
        }
        for policy in (RETAIN_INPUTS, GENERIC_RELEASE_INPUTS,
                       CALLBACK_RELEASE_INPUTS):
            for key, parameter_name in policy.items():
                self.assertIn(key, slots)
                self.assertIn(
                    parameter_name,
                    {parameter.name for parameter in slots[key].function.parameters},
                    key,
                )

        for key, slot in slots.items():
            if key in CUSTOM_SLOTS or key in NATIVE_ONLY:
                continue
            has_callback = any(
                base_type(parameter.c_type) in self.model.function_types
                for parameter in slot.function.parameters
            )
            owned_outputs = [
                (index, parameter.name)
                for index, parameter in enumerate(slot.function.parameters)
                if clean_type(parameter.c_type).endswith("*")
                and parameter.name.startswith("out_")
                and is_handle(base_type(parameter.c_type))
            ]
            if has_callback:
                continue
            self.assertLessEqual(len(owned_outputs), 1, key)
            for index, output in owned_outputs:
                # Ask the emitter's own classifier rather than restating half
                # of it here. Restating it is how this test came to expect the
                # array witness for intermod.mod_get_dependency, whose
                # `WotbModV3Handle* out_dependency, uint32_t* out_optional` is
                # two scalar outputs and never was an array.
                is_array = is_output_array_pair(
                    list(slot.function.parameters), index
                )
                witness = (
                    "RecordHandle(static_cast<WotbModV3Handle>(items[recorded]))"
                    if is_array
                    else f"RecordHandle(static_cast<WotbModV3Handle>({output}))"
                )
                self.assertIn(
                    witness,
                    source,
                    key,
                )

    def test_a_counted_output_array_is_never_an_array_of_char_or_void(self):
        # The class of bug this closes: two adjacent pointers, `T*` then
        # `uint32_t*`, are not evidence of an array. Where T is char the
        # parameter is a string, where T is void it is opaque, and where the
        # uint32_t* is spelled `out_` rather than `inout_` it is a scalar
        # result with no capacity for a probe call to discover. Any of the
        # three, read as an array, produces a binding that silently passes
        # nullptr for a real argument.
        source = Generator(self.model).generate()
        emitted = _emitted_output_arrays(source)
        slots = {
            f"Bind_{symbol(slot)}": slot for slot in self.model.all_slots()
        }

        actual = {}
        for function, element in emitted.items():
            self.assertNotIn(element, NON_ELEMENT_TYPES, function)
            slot = slots[function]
            key = f"{slot.interface.lua_name}.{slot.name}"
            params = list(slot.function.parameters)
            index = next(
                i for i in range(len(params) - 1)
                if is_output_array_pair(params, i)
            )
            # The emitted element type is the one the headers declare, and the
            # count it is paired with really is in-out.
            self.assertEqual(element, base_type(params[index].c_type), key)
            self.assertTrue(
                params[index + 1].name.startswith(COUNT_PARAMETER_PREFIX), key
            )
            actual[key] = element
        self.assertEqual(EXPECTED_OUTPUT_ARRAY_SLOTS, actual)

        # Same claim stated over every slot, not only the ones that survived:
        # nothing in the current headers can be classified as an array of char
        # or void, whatever the rest of the ladder decides to do with it.
        for slot in self.model.all_slots():
            params = list(slot.function.parameters)
            for index in range(len(params) - 1):
                if is_output_array_pair(params, index):
                    self.assertNotIn(
                        base_type(params[index].c_type),
                        NON_ELEMENT_TYPES,
                        f"{slot.interface.lua_name}.{slot.name}",
                    )

    def test_a_counted_array_field_is_never_read_as_one_element(self):
        # The out-of-bounds read this closes, in the emitted C++ that shipped:
        #
        #     WotbModV3SettingPresetValue* item = static_cast<...>(
        #         ArenaAllocate(arena, sizeof(WotbModV3SettingPresetValue)));
        #     ... out->values = item;
        #     ... out->value_count = static_cast<uint32_t>(v);   // the script's
        #
        # One element allocated, any count at all believed. The client walks
        # value_count of them (src/v3/data_services.cpp:10754, and the same
        # shape in InputRegisterAction and the skin-pack registrar), so a
        # sandboxed script chose how far past a one-element heap block the
        # game read - which is the one thing a script may never be able to do.
        #
        # Asserted against the emitted source rather than by re-running the
        # classifier, for the same reason the output-array test above is: the
        # thing that has to be true is what ships, not what a helper believes.
        source = Generator(self.model).generate()
        readers = _struct_readers(source)
        found = {}
        for name in sorted(self.model.struct_bodies):
            if "ApiV" in name:
                continue
            fields = self.model.data_fields(name)
            for index, field in enumerate(fields):
                key = f"{name}.{field.name}"
                pointee = base_type(field.c_type)
                follower = fields[index + 1] if index + 1 < len(fields) else None
                points_at_elements = (
                    clean_type(field.c_type).endswith("*")
                    and pointee not in NON_ELEMENT_TYPES
                    and pointee in self.model.struct_bodies
                    and not field.array_extent
                    and not field.union_members
                )
                counted = follower is not None and clean_type(
                    follower.c_type
                ) == "uint32_t"
                if points_at_elements and counted:
                    if clean_type(field.c_type).startswith("const "):
                        # No third state: either the emitter pairs them, or the
                        # shape has been reviewed by hand and listed as a single
                        # element. Silently reading one element for a count the
                        # client will trust is what this whole test forbids.
                        self.assertTrue(
                            is_input_array_field_pair(fields, index)
                            or key in SINGLE_ELEMENT_STRUCT_POINTERS,
                            f"{key} points at elements and is followed by "
                            f"{follower.name}, but the emitter pairs neither",
                        )
                    else:
                        # A buffer the client writes into. Not marshalled at
                        # all, and only safe while no binding reads the struct
                        # that holds it.
                        self.assertEqual(
                            UNMARSHALLED_OUTPUT_BUFFERS.get(key),
                            follower.name,
                            f"{key} lends the client a buffer of "
                            f"{follower.name} elements and this host cannot "
                            f"build one",
                        )
                        self.assertNotIn(
                            f"Read_{name}(state,",
                            source,
                            f"{key} is reachable from Lua, and the reader "
                            f"allocates one element for a buffer the client "
                            f"writes {follower.name} of",
                        )
                if not is_input_array_field_pair(fields, index):
                    continue
                count = fields[index + 1]
                body = readers[f"Read_{name}"]
                length = f"{field.name}_length"
                found[key] = (pointee, count.name)

                # One element, ever again.
                self.assertNotIn(
                    f"ArenaAllocate(arena, sizeof({pointee}))", body, key
                )
                # N elements, and N is the Lua table's own length.
                self.assertIn(
                    f"{length} = static_cast<size_t>(lua_rawlen(state, -1));",
                    body,
                    key,
                )
                self.assertIn(
                    f"ArenaAllocate(arena, {length} * sizeof({pointee}))",
                    body,
                    key,
                )
                # The multiplication above cannot wrap: size_t is 32-bit in
                # this build, so N * sizeof(T) is exactly where a large N would
                # turn into a small allocation.
                self.assertIn(
                    f"if ({length} > UINT32_MAX || ({length} && "
                    f"{length} > SIZE_MAX / sizeof({pointee})))",
                    body,
                    key,
                )
                # A failed allocation is a value, not a short buffer.
                self.assertIn(
                    'lua_pushstring(state, "out of memory"); return false;',
                    body,
                    key,
                )
                # The count the client reads is the count this host filled.
                self.assertIn(
                    f"out->{count.name} = static_cast<uint32_t>({length});",
                    body,
                    key,
                )
                self.assertNotIn(
                    f"out->{count.name} = static_cast<uint32_t>(v);", body, key
                )
                # And a count the script supplies as well is only ever checked
                # against that length, never believed over it.
                self.assertIn(
                    f"if (v != static_cast<uint64_t>({length}))", body, key
                )
        self.assertEqual(EXPECTED_INPUT_ARRAY_FIELDS, found)

    def test_settings_scalar_accessors_take_a_key_and_return_one_value(self):
        # settings.get_bool shipped as an output array of chars: the key was
        # dropped, nullptr went to the client in its place, and Lua got a
        # table. Every sibling in the family is pinned next to it, because a
        # wrong get_bool is evidence that nobody had called any of them.
        source = Generator(self.model).generate()
        bodies = _binding_bodies(source)

        get_bool = bodies["Bind_settings_get_bool"]
        self.assertIn(
            "const char* key = nullptr; if (!CheckArgString(state, 1, &key))",
            get_bool,
        )
        self.assertIn(
            "->get_bool(ctx->mod, key, &out_value);",
            get_bool,
        )
        self.assertIn(
            "lua_pushinteger(state, static_cast<lua_Integer>(out_value));",
            get_bool,
        )
        # One value out, and none of the array machinery.
        self.assertEqual(1, get_bool.count("lua_push"))
        for array_only in (
            "client returned an unreasonable array size",
            "lua_createtable",
            "std::calloc",
            "nullptr, &count",
        ):
            self.assertNotIn(array_only, get_bool, array_only)

        for name, call, push in (
            ("get_int", "->get_int(ctx->mod, key, &out_value);",
             "lua_pushinteger(state, static_cast<lua_Integer>(out_value));"),
            ("get_float", "->get_float(ctx->mod, key, &out_value);",
             "lua_pushnumber(state, static_cast<lua_Number>(out_value));"),
            ("get_color", "->get_color(ctx->mod, key, &out_value);",
             "Push_WotbModV3Color(state, out_value);"),
        ):
            body = bodies[f"Bind_settings_{name}"]
            self.assertIn(
                "const char* key = nullptr; if (!CheckArgString(state, 1, &key))",
                body,
                name,
            )
            self.assertIn(call, body, name)
            self.assertIn(push, body, name)

        # get_string is the sized-string shape (`char* buffer, uint32_t*
        # inout_size`), claimed before the array rules are ever reached.
        get_string = bodies["Bind_settings_get_string"]
        self.assertIn(
            "PushSizedString(state, [&](char* buffer, uint32_t* size) { "
            "return static_cast<const WotbModV3SettingsApiV1*>(ctx->api)"
            "->get_string(ctx->mod, key, buffer, size); }",
            get_string,
        )

        # The setters take (key, value) and return nothing but the result.
        for name, call in (
            ("set_bool", "->set_bool(ctx->mod, key, value);"),
            ("set_int", "->set_int(ctx->mod, key, value);"),
            ("set_float", "->set_float(ctx->mod, key, value);"),
            ("set_string", "->set_string(ctx->mod, key, value);"),
            ("set_color", "->set_color(ctx->mod, key, &value);"),
        ):
            body = bodies[f"Bind_settings_{name}"]
            self.assertIn(
                "const char* key = nullptr; if (!CheckArgString(state, 1, &key))",
                body,
                name,
            )
            self.assertIn(call, body, name)
            self.assertEqual(0, body.count("lua_push"), name)

    def test_native_only_list_is_explicit_and_narrow(self):
        self.assertEqual(
            {
                "intermod.export_interface",
                "unsafe_native.create_address_hook",
            },
            NATIVE_ONLY,
        )

    def test_constant_inventory_is_stable_per_table(self):
        tables = constant_tables(self.model)
        actual = {
            interface: len(entries)
            for interface, entries in tables.items()
        }
        self.assertEqual(EXPECTED_CONSTANT_COUNTS, actual)
        self.assertEqual(611, sum(actual.values()))
        self.assertEqual(
            set(actual) | {
                "handles", "unsafe_native", "render_native", "archive", "devtools",
            },
            {interface.lua_name for interface in INTERFACES},
        )

    def test_hand_written_constant_spellings_survive_unchanged(self):
        # The spellings below are what shipped examples and every script
        # written so far already use. The assertion is against the C macro
        # rather than against a number: identical macro is a stronger claim
        # than identical value, and it keeps this test from becoming a second
        # place where somebody has to write the ABI's numbers down by hand.
        events = MOD_API / "loader" / "lua" / "lua_bind_events.cpp"
        ui = MOD_API / "loader" / "lua" / "lua_bind_ui.cpp"
        hand_written = {
            "events": {
                **_hand_written_constants(events, "kEventsConstants"),
                **_hand_written_constants(events, "kEventTopicConstants"),
            },
            "ui": _hand_written_constants(ui, "kUiConstants"),
        }
        self.assertEqual(76, len(hand_written["events"]))
        self.assertEqual(38, len(hand_written["ui"]))

        tables = constant_tables(self.model)
        for interface, expected in hand_written.items():
            generated = {
                lua_name: constant.c_name
                for lua_name, constant in tables[interface]
            }
            for lua_name, macro in expected.items():
                self.assertIn(lua_name, generated, f"wotb.{interface}.{lua_name}")
                self.assertEqual(macro, generated[lua_name], lua_name)

        # The 22 hard collisions: one concept, a topic string and a client
        # event type, kept apart by two disjoint prefixes.
        events_table = dict(tables["events"])
        for concept in ("BATTLE_STARTED", "SCENE_ACTIVATED", "UI_INPUT",
                        "SHOT_FIRED"):
            self.assertTrue(events_table[f"TOPIC_{concept}"].is_string)
            self.assertEqual("enum", events_table[f"TYPE_{concept}"].kind)

    def test_no_table_publishes_one_name_twice(self):
        # constant_tables raises on a collision, so reaching this point is
        # already the assertion; it is restated per table so a future change
        # that softens the exception still fails here.
        for interface, entries in constant_tables(self.model).items():
            names = [lua_name for lua_name, _ in entries]
            self.assertEqual(len(names), len(set(names)), interface)

    def test_constants_cannot_shadow_a_slot(self):
        # Constants and slots share one table per interface. They stay apart
        # because a constant is UPPER_SNAKE and a slot is lower_snake, which
        # is only a convention until something checks it.
        tables = constant_tables(self.model)
        for interface in INTERFACES:
            slots = {slot.name for slot in self.model.flatten_slots(interface)}
            for slot in slots:
                self.assertEqual(slot.lower(), slot, slot)
            for lua_name, _ in tables.get(interface.lua_name, ()):
                self.assertRegex(lua_name, r"^[A-Z][A-Z0-9_]*$")
                self.assertNotIn(lua_name, slots, interface.lua_name)

    def test_excluded_constant_categories_really_are_excluded(self):
        parsed = {constant.c_name for constant in self.model.constants}
        for never_parsed in (
            "WOTBMOD_V3_IFACE_CORE",          # interface id
            "WOTBMOD_V3_IFACE_VERSION_1",     # interface id and version macro
            "WOTBMOD_V3_UI_VERSION_3",        # version macro
            "WOTBMOD_V3_ABI_VERSION",
            "WOTBMOD_V3_CALL",                # plumbing
            "WOTBMOD_V3_EXPORT",
            "WOTBMOD_V3_EXTERN_C",
            "WOTBMOD_V3_ENTRY",
            "WOTBMOD_V3_ENTRY_NAME",
            "WOTBMOD_V3_INIT_STRUCT",         # function-like
            "WOTBMOD_V3_DEFINE_ENTRY",
        ):
            self.assertNotIn(never_parsed, parsed)

        published = {
            constant.c_name
            for entries in constant_tables(self.model).values()
            for _, constant in entries
        }
        excluded = excluded_constants(self.model)
        self.assertEqual(77, len(excluded))
        for name, reason in excluded.items():
            self.assertNotIn(name, published)
        for base_only in (
            "WOTBMOD_V3_OK",                  # WotbModV3Result: nil, message
            "WOTBMOD_V3_E_TIMEOUT",
            "WOTBMOD_V3_HANDLE_UI_CONTROL",   # WotbModV3HandleType
            "WOTBMOD_V3_CONTEXT_BATTLE",      # already wotb.context.BATTLE
            "WOTBMOD_V3_INVALID_HANDLE",
            "WOTBMOD_V3_MAX_PATH",
        ):
            self.assertIn(base_only, excluded, base_only)

        # Size caps are an opt-in list, not a side effect of a prefix rule.
        self.assertIn("WOTBMOD_V3_STORAGE_KEY_MAX", published)
        self.assertEqual(31, len(SIZE_LIMITS))
        for c_name in SIZE_LIMITS:
            self.assertIn(c_name, parsed)

    def test_constants_register_outside_the_capability_and_permission_fence(self):
        source = Generator(self.model).generate()
        self.assertIn(
            "void RegisterGeneratedConstants(lua_State* state) noexcept {",
            source,
        )
        constants = source[source.index("void RegisterGeneratedConstants("):]
        # No capability query and no permission fence: a constant is a fact
        # about the headers, not an operation on the client.
        self.assertNotIn("QueryGenerated", constants)
        self.assertNotIn("SetFuncsGuardedAll", constants)
        # storage would be dropped entirely by the RegisterGeneratedBindings
        # path, because all 14 of its slots are hand-written.
        self.assertIn('generated::OpenConstantTable(state, "storage");', constants)
        self.assertIn(
            'generated::SetConstant(state, "PATH_CACHE", '
            "WOTBMOD_V3_STORAGE_PATH_CACHE);",
            constants,
        )
        # Python never evaluates a C expression: every value is the ABI's own
        # name, handed to the C++ compiler.
        for line in constants.splitlines():
            if "generated::SetConstant(state," in line:
                self.assertRegex(line.strip(), r'^generated::SetConstant\(state, "[A-Z][A-Z0-9_]*", WOTBMOD_V3_[A-Z0-9_]+\);$')

    def test_generated_families_use_complete_inner_permission_fences(self):
        source = Generator(self.model).generate()
        self.assertEqual(
            ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
            PERMISSIONS["ui"],
        )
        self.assertEqual(
            ("camera.battle.read", "camera.hangar", "camera.replay",
             "gameplay.tweak.camera"),
            PERMISSIONS["camera"],
        )
        self.assertIn("SetFuncsGuardedAll(state, funcs_ui", source)
        self.assertIn(
            'permissions_camera[] = {"camera.battle.read", "camera.hangar", '
            '"camera.replay", "gameplay.tweak.camera"}',
            source,
        )

    def test_author_guide_tracks_the_structural_surface(self):
        guide = (MOD_API / "docs" / "LUA_MODS_RU.md").read_text(
            encoding="utf-8"
        )
        # The guide's headline numbers come from the model, not from memory:
        # every slot the headers declare, minus the two native-only ones, in
        # the interface count the model has. A header change that moves them
        # fails here until the guide says the new numbers.
        total = sum(len(self.model.flatten_slots(interface))
                    for interface in INTERFACES)
        available = total - len(NATIVE_ONLY)
        self.assertIn(f"{total} слотов в {len(INTERFACES)} интерфейсах", guide)
        self.assertIn(f"{available} из {total} слотов", guide)
        self.assertIn("on_frame(frame_index, delta_seconds)", guide)
        self.assertIn(r"mods\lua", guide)
        for interface in INTERFACES:
            self.assertIn(f"`{interface.lua_name}`", guide)
        for native_only in NATIVE_ONLY:
            self.assertIn(f"`{native_only}`", guide)

    def test_typed_gameplay_events_and_public_player_views_are_wired(self):
        event_source = (
            MOD_API / "loader" / "lua" / "lua_bind_events.cpp"
        ).read_text(encoding="utf-8")
        player_source = (
            MOD_API / "loader" / "lua" / "lua_bindings.cpp"
        ).read_text(encoding="utf-8")
        runtime_source = (
            MOD_API / "src" / "wotb_mod_runtime.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn('lua_setfield(state, -2, "data")', event_source)
        for constant in (
            "TOPIC_DAMAGE_RECEIVED",
            "TOPIC_LOCAL_VEHICLE_CREATED",
            "TOPIC_LOCAL_VEHICLE_DESTROYED",
            "TOPIC_SNIPER_ENTERED",
            "TOPIC_SNIPER_EXITED",
            "TOPIC_PUBLIC_ENTITY_ADDED",
            "TOPIC_PROJECTILE_CREATED",
            "TOPIC_RPC_OBSERVED",
        ):
            self.assertIn(constant, event_source)

        self.assertIn("function players.snapshot()", player_source)
        self.assertIn('enemy_scope = "currently_visible_only"', player_source)
        self.assertIn("function players.unknown_team()", player_source)
        self.assertIn("entity_api.enumerate_visible", player_source)

        for topic in (
            "WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED",
            "WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED",
            "WOTBMOD_V3_EVENT_DAMAGE_RECEIVED",
            "WOTBMOD_V3_EVENT_SNIPER_ENTERED",
            "WOTBMOD_V3_EVENT_SNIPER_EXITED",
        ):
            self.assertIn(topic, runtime_source)
        self.assertIn("RequiredClientEventPayloadSize", runtime_source)
        self.assertIn(
            "payloadSize < RequiredClientEventPayloadSize(type)",
            runtime_source,
        )
        self.assertIn("hasTypedPayload &&", runtime_source)

    def test_live_vehicle_optional_fields_use_fingerprint_gated_layout(self):
        loader_source = (
            MOD_API / "loader" / "wotb_mod_loader.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("kVehicleMaxHealthOffset = 0x11Cu", loader_source)
        self.assertIn("reinterpret_cast<const int16_t*>", loader_source)
        self.assertNotIn("kVehiclePlayerNameOffset", loader_source)
        self.assertIn(
            "kVehiclePrimaryVtableRva = 0x033056A4u",
            loader_source,
        )
        self.assertIn("kVehicleObjectEntityIdOffset = 0x1Cu", loader_source)
        self.assertIn("kVehicleObjectTeamOffset = 0xB0u", loader_source)
        self.assertIn("if (!g_nativeFingerprintVerified", loader_source)
        self.assertIn("actualVtable != expectedVtable", loader_source)
        self.assertIn("vehicleEntityId != entityId", loader_source)
        self.assertIn("nativeTeam < 1u || nativeTeam > 2u", loader_source)
        self.assertIn("static_cast<uint32_t>(nativeTeam);", loader_source)

    def test_loader_derives_hangar_and_catalog_context_from_dava(self):
        loader_source = (
            MOD_API / "loader" / "wotb_mod_loader.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(
            'InspectUiHierarchy(rootResource, "Hangar")',
            loader_source,
        )
        self.assertIn(
            "hangar == UiDescendantVisibility::kHidden",
            loader_source,
        )
        self.assertIn(
            'InspectUiHierarchy(rootResource, "ModCatalogScreen")',
            loader_source,
        )
        self.assertIn('"ModCatalogStateMarker"', loader_source)
        self.assertIn("g_cachedCatalogMarkerResource", loader_source)
        self.assertIn("InspectRetainedUiInputMarker", loader_source)
        self.assertIn("g_uiResourceRegistryChanged", loader_source)
        self.assertIn(
            "ResetUiContextProbeResources();",
            loader_source,
        )
        self.assertIn(
            "rootResource && fullProbe && markerUnavailable",
            loader_source,
        )
        self.assertIn("WOTBMOD_UI_CONTROL_INPUT_ENABLED", loader_source)
        self.assertIn("g_liveResourceBackend.ui_get_parent", loader_source)
        self.assertIn("InspectUiEffectiveVisibility", loader_source)
        self.assertIn("WOTBMOD_V3_CONTEXT_MOD_SCREEN", loader_source)
        self.assertIn("g_battleContextActive", loader_source)
        self.assertIn("ApplyUiDerivedContext();", loader_source)
        resources_source = (
            MOD_API / "src" / "wotb_mod_dava_resources.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("UiControlHasFastName", resources_source)
        self.assertIn("? root->native_object", resources_source)

        session_source = (
            MOD_API / "examples" / "lua_session_stats" / "main.lua"
        ).read_text(encoding="utf-8")
        self.assertNotIn("BattleButtonWrapper", session_source)
        self.assertNotIn("ModCatalogScreen", session_source)
        self.assertNotIn("probe_hangar_surface", session_source)
        self.assertNotIn("hangar_latched", session_source)
        self.assertNotIn("rebind_active_screen", session_source)
        self.assertIn("BATTLE_RESULT_GRACE_SECONDS", session_source)
        self.assertIn("on_battle_ended", session_source)
        self.assertIn("on_battle_left", session_source)

    def test_win32_ui_capture_uses_one_logical_coordinate_space(self):
        bindings_source = (
            MOD_API / "loader" / "v3_native_bindings.cpp"
        ).read_text(encoding="utf-8")
        capture_start = bindings_source.index("bool ResolveUiCapturePosition(")
        capture_end = bindings_source.index("bool ReadUiClientCursor(")
        capture_source = bindings_source[capture_start:capture_end]
        self.assertNotIn("logical_scale != 1.0f", capture_source)
        self.assertNotIn("ShouldCaptureClientHostUiInput(raw_x, raw_y)", capture_source)
        self.assertEqual(2, capture_source.count("ShouldCaptureClientHostUiInput"))

        loader_source = (
            MOD_API / "loader" / "wotb_mod_loader.cpp"
        ).read_text(encoding="utf-8")
        detour_start = loader_source.index("UiSystemInputDetour(")
        detour_end = loader_source.index("static bool TryReadAmmoShellId(")
        detour_source = loader_source[detour_start:detour_end]
        self.assertIn("ShouldCaptureClientHostUiInput", detour_source)
        self.assertIn("!consumeForApiUi && g_originalUiSystemInput", detour_source)
        self.assertIn("apiPointerGesture = overApiUi", detour_source)
        self.assertIn("apiPointerGesture = false", detour_source)
        self.assertNotIn("IsV3NativeClientUiControl", detour_source)
        self.assertNotIn("inputDpi", detour_source)

    def test_ui_capture_snapshot_refreshes_after_mod_frames(self):
        runtime_source = (
            MOD_API / "src" / "wotb_mod_runtime.cpp"
        ).read_text(encoding="utf-8")
        dispatch_start = runtime_source.index(
            "WotbModRuntime_DispatchFrame("
        )
        dispatch_end = runtime_source.index(
            'extern "C" void WOTBMOD_CALL WotbModRuntime_Shutdown()',
            dispatch_start,
        )
        dispatch_source = runtime_source[dispatch_start:dispatch_end]
        self.assertLess(
            dispatch_source.rindex("CallFrame(record, &frame)"),
            dispatch_source.rindex("RefreshClientHostUiCaptureSnapshot()"),
        )

    def test_release_manifest_requests_every_non_unsafe_permission(self):
        manifest = json.loads(
            (MOD_API / "examples" / "lua_host" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual("wotbmod.lua_host", manifest["id"])
        self.assertEqual(
            "bin/windows-x86/wotbmod_lua_host.dll",
            manifest["entrypoints"]["windows-x86"],
        )
        self.assertEqual(
            REGISTERED_PERMISSIONS - UNSAFE_PERMISSIONS,
            set(manifest["permissions"]),
        )
        self.assertEqual(51, len(manifest["permissions"]))

    def test_ui_framework_example_is_installable_and_distributed(self):
        example = MOD_API / "examples" / "lua_ui_framework"
        manifest = json.loads(
            (example / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual("example.lua_ui_framework", manifest["id"])
        self.assertEqual("main.lua", manifest["entrypoint"])
        self.assertEqual(
            {
                "core",
                "events.public",
                "ui.create",
                "ui.modify.own",
                "ui.modify.game",
                "battle.ui",
                "input",
            },
            set(manifest["permissions"]),
        )

        source = (example / "main.lua").read_text(encoding="utf-8")
        self.assertIn("wotb.events.TOPIC_UI_SCREEN_CHANGED", source)
        self.assertIn("wotb.events.subscribe", source)
        self.assertIn("wotb.context.should_show", source)
        self.assertIn("wotb.context.MOD_SCREEN", source)
        self.assertIn("local SAFE_LEFT = 128", source)
        self.assertIn("wotb.ui.create", source)
        self.assertIn("wotb.ui.EVENT_CLICK", source)
        self.assertIn(":set_text(", source)
        self.assertIn("template:clone()", source)
        self.assertIn(":push_style(", source)
        self.assertIn("CONTROL_TEXT_INPUT", source)
        self.assertIn("CONTROL_LIST", source)
        self.assertNotIn("TEMPLATE_URI", source)
        self.assertNotIn(".yaml", source.lower())
        for control_id in (
            "OpenButton",
            "Window",
            "CloseButton",
            "ToggleButton",
            "ActionButton",
            "ResetButton",
        ):
            self.assertIn(control_id, source)
        self.assertIn('create_slider("Volume"', source)
        self.assertIn('create_slider("Scale"', source)

        packager = (
            MOD_API / "tools" / "build_lua_host_package.ps1"
        ).read_text(encoding="utf-8")
        preview_packager = (
            MOD_API / "tools" / "build_public_preview.ps1"
        ).read_text(encoding="utf-8")
        self.assertIn("lua_ui_framework", packager)
        self.assertIn("example.lua_ui_framework", packager)
        self.assertNotIn("lua_ui_framework.yaml", packager)

        functional_examples = {
            "lua_ally_tracker": {
                "id": "example.lua_ally_tracker",
                "permissions": {
                    "core",
                    "events.public",
                    "entity.public.visible",
                    "game.entity.public",
                    "ui.create",
                    "ui.modify.own",
                    "ui.modify.game",
                    "battle.ui",
                },
                "markers": (
                    "wotb.players.snapshot",
                    "position_available",
                    "TOPIC_PUBLIC_ENTITY_UPDATED",
                ),
            },
            "lua_battle_telemetry": {
                "id": "example.lua_battle_telemetry",
                "permissions": {
                    "core",
                    "events.public",
                    "ui.create",
                    "ui.modify.own",
                    "ui.modify.game",
                    "battle.ui",
                },
                "markers": (
                    "TOPIC_DAMAGE_RECEIVED",
                    "TOPIC_RELOAD_STATE_CHANGED",
                    "TOPIC_LOCAL_SHELL_FIRED",
                    "TOPIC_SHOT_FIRED",
                ),
            },
            "lua_session_stats": {
                "id": "example.lua_session_stats",
                "permissions": {
                    "core",
                    "events.public",
                    "entity.public.visible",
                    "game.entity.public",
                    "ui.create",
                    "ui.modify.own",
                    "ui.modify.game",
                    "battle.ui",
                },
                "markers": (
                    "TOPIC_BATTLE_ENDED",
                    "TOPIC_DAMAGE_RECEIVED",
                    "TOPIC_LOCAL_SHELL_FIRED",
                    "wotb.players.snapshot",
                    "winner_team",
                    "SessionHistoryRow",
                ),
            },
        }
        for directory, expected in functional_examples.items():
            functional = MOD_API / "examples" / directory
            functional_manifest = json.loads(
                (functional / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(expected["id"], functional_manifest["id"])
            self.assertEqual(
                expected["permissions"],
                set(functional_manifest["permissions"]),
            )
            functional_source = (functional / "main.lua").read_text(
                encoding="utf-8"
            )
            self.assertIn("wotb.context.should_show", functional_source)
            self.assertIn("wotb.context.MOD_SCREEN", functional_source)
            for marker in expected["markers"]:
                self.assertIn(marker, functional_source)
            self.assertIn(directory, packager)
            self.assertIn(expected["id"], packager)
            self.assertIn(directory, preview_packager)

    def test_dava_workshop_is_typed_live_example_and_distributed(self):
        example = MOD_API / "examples" / "lua_dava_workshop"
        manifest = json.loads(
            (example / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual("example.lua_dava_workshop", manifest["id"])
        self.assertEqual("main.lua", manifest["entrypoint"])
        self.assertEqual(
            {
                "core",
                "events.public",
                "input",
                "ui.create",
                "ui.modify.own",
                "ui.modify.game",
                "battle.ui",
                "resources.mod",
                "gameplay.tweak.cosmetic",
                "camera.battle.read",
                "camera.hangar",
                "camera.replay",
                "gameplay.tweak.camera",
            },
            set(manifest["permissions"]),
        )

        source = (example / "main.lua").read_text(encoding="utf-8")
        for marker in (
            "wotb.dava.is_supported",
            "wotb.dava.create_material",
            "wotb.dava.material_set_property",
            "wotb.dava.create_mesh_consumer",
            "wotb.dava.create_mesh",
            "wotb.dava.mesh_hot_swap",
            "wotb.dava.create_stock_tracer",
            "wotb.loaders.load_dava_yaml",
            "wotb.loaders.open_dava_archive",
            "wotb.context.contains",
            "wotb.context.should_show",
            "wotb.input.KEY_F9",
            "wotb.input.KEY_F10",
        ):
            self.assertIn(marker, source)
        self.assertNotRegex(source, r"(?:native|provider)_(?:pointer|token)\s*=")
        self.assertTrue((example / "fixtures" / "workshop.yaml").is_file())
        self.assertTrue(
            (example / "fixtures" / "archive_payload.txt").is_file()
        )

        packager = (
            MOD_API / "tools" / "build_lua_host_package.ps1"
        ).read_text(encoding="utf-8")
        preview_packager = (
            MOD_API / "tools" / "build_public_preview.ps1"
        ).read_text(encoding="utf-8")
        for script in (packager, preview_packager):
            self.assertIn("lua_dava_workshop", script)
            self.assertIn("fixtures\\workshop.zip", script)
            self.assertIn("make_workshop_archive.py", script)

        archive_builder = (
            MOD_API / "tools" / "make_workshop_archive.py"
        ).read_text(encoding="utf-8")
        self.assertIn("with zipfile.ZipFile", archive_builder)
        self.assertIn("os.replace", archive_builder)


if __name__ == "__main__":
    unittest.main()
