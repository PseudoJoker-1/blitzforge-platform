"""Keep the published hook-symbol table honest.

resolve_symbol is the only way a mod names a native target, so a bad row here
aims someone else's detour at the wrong address. Seven ways it can go wrong,
all blocked below:

  * a name pointing at a data anchor (vtable/singleton) - not executable, the
    caller gets E_CLIENT_MISMATCH and reads it as a broken build;
  * a name pointing at CRT boilerplate - a hook on operator new fires for the
    whole process, not just the mod's target;
  * a name for an anchor re_anchors.md never actually names;
  * a name mapped to an RVA constant no source declares;
  * a name the binding list advertises that resolve_symbol does not publish -
    a mod reads that name back and gets E_NOT_SUPPORTED;
  * a name the binding list advertises against one anchor while resolve_symbol
    hands out another - the mod is told it is hooking a function it is not;
  * two names for one address disagreeing on the permission they cost - the
    expensive spelling becomes optional.
"""
from pathlib import Path
import re
import sys

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS / "reanchor"))

from anchors import load_anchors  # noqa: E402

SOURCE = TOOLS / "mod_api" / "loader" / "v3_native_bindings.cpp"
text = SOURCE.read_text(encoding="utf-8")

RUNTIME = TOOLS / "mod_api" / "src" / "v3" / "runtime_services.cpp"
runtime_text = RUNTIME.read_text(encoding="utf-8")

start = text.find("WotbModV3NativeBindings_ResolveHookSymbol(")
assert start >= 0, "ResolveHookSymbol is missing"
# Bound the parse by the array literal, not by the function body. The table
# ends at the first "};" after its opening and no entry contains that pair,
# so this holds however the surrounding function is indented; looking for a
# closing brace in column 0 would only work while every nested brace in the
# function happens to be indented.
table = text.index("kHookSymbols[] = {", start)
body = text[table:text.index("};", table)]

published = re.findall(r'\{"([^"]+)",\s*k(\w+)Rva\}', body)
assert published, "no published symbols were parsed - did the table change shape?"

anchors = {a.id: a.rva for a in load_anchors(TOOLS)}

DATA_ANCHORS = {
    "DefaultEntityVtable", "DefaultHybridEventVtable", "DefaultSceneVtable",
    "DefaultSoundGroup", "DefaultSoundSystemProxyVtable",
    "DefaultSoundSystemSingleton", "DefaultSoundSystemVtable",
    "DefaultUiControlVtable", "DefaultUiPackageLoaderVtable",
    "DefaultUiPackageVtable", "DefaultWwiseEventVtable", "GameCameraVtable",
    "TracerManagerVtable",
}
CRT_ANCHORS = {
    "DefaultOperatorNew", "DefaultOperatorDelete", "DefaultFastNameCtor",
    "DefaultRefCountedRetain", "DefaultRefCountedRelease",
}
UNNAMED_ANCHORS = {
    "AimTargetSet", "AimTargetClear", "ObservedStatus",
    "ObservedPayloadGetter", "ResolveAvatarVehicle", "DefaultSceneLoadFromFile",
}

names = [name for name, _ in published]

duplicates = sorted(n for n in set(names) if names.count(n) > 1)
assert not duplicates, f"duplicate symbol name published: {duplicates}"

published_anchor = dict(published)

# The binding list is how a mod discovers what exists, and it is written
# twice: AddAllBindingsUnchecked on the fingerprint-mismatch path, and the
# LogBindingStatus calls on the matching path. Both are cross-checked, because
# a name added to only one of them is advertised on only one kind of client.
#
# AddUncheckedBinding(id, kind, rva) and LogBindingStatus(id, rva, ok, kind)
# order their arguments differently, hence two patterns.
BINDING_SITES = (
    re.compile(r'Add\w*Binding\w*\(\s*"([^"]+)",\s*"[^"]*",\s*k(\w+)Rva'),
    re.compile(r'LogBindingStatus\(\s*"([^"]+)",\s*k(\w+)Rva'),
)

advertised = {}
for index, pattern in enumerate(BINDING_SITES):
    found = pattern.findall(text)
    assert found, (
        f"binding-site pattern {index} matched nothing - did the binding list "
        "change shape? It must stay cross-checkable against resolve_symbol."
    )
    for name, anchor_id in found:
        advertised.setdefault(name, {}).setdefault(anchor_id, set()).add(index)

# The two paths must advertise the same names, or a mod discovers a target on
# a mismatched client that a matching client never mentions, or the reverse.
by_path = [
    {name for name, entry in advertised.items()
     if any(index in sites for sites in entry.values())}
    for index in range(len(BINDING_SITES))
]
assert by_path[0] == by_path[1], (
    "the mismatch path (AddAllBindingsUnchecked) and the matching path "
    "(LogBindingStatus) advertise different names: only-unchecked="
    f"{sorted(by_path[0] - by_path[1])}, only-checked="
    f"{sorted(by_path[1] - by_path[0])}"
)

split = sorted(name for name, entry in advertised.items() if len(entry) > 1)
assert not split, (
    f"the binding list advertises {split} against more than one anchor; "
    "the two sites disagree about which function that name is"
)

# `::vtable` entries are the exception - they are data, deliberately not
# hookable, and are excluded from the published table on purpose.
advertised_hookable = {
    name: next(iter(entry))
    for name, entry in advertised.items()
    if not name.endswith("::vtable")
}

missing = sorted(set(advertised_hookable) - set(names))
assert not missing, (
    f"the binding list advertises {missing}, but resolve_symbol does not "
    "publish them; a mod reading those names back cannot hook them"
)

# Agreement on which anchor, not just on the name. Retargeting a binding-list
# entry while leaving resolve_symbol alone would advertise one function and
# hand out another, and every check above would still pass.
retargeted = sorted(
    (name, anchor_id, published_anchor[name])
    for name, anchor_id in advertised_hookable.items()
    if published_anchor[name] != anchor_id
)
assert not retargeted, (
    "the binding list and resolve_symbol disagree about which anchor a name "
    f"means (name, advertised, published): {retargeted}"
)

for name, anchor_id in published:
    assert anchor_id in anchors, (
        f"'{name}' maps to k{anchor_id}Rva, which no source declares"
    )
    assert anchor_id not in DATA_ANCHORS, (
        f"'{name}' maps to data anchor {anchor_id}; it is not executable"
    )
    assert anchor_id not in CRT_ANCHORS, (
        f"'{name}' maps to CRT boilerplate {anchor_id}; hooking it is a trap"
    )
    assert anchor_id not in UNNAMED_ANCHORS, (
        f"'{name}' maps to {anchor_id}, which re_anchors.md does not name. "
        "Document the real symbol name before publishing it."
    )

for required in ("Camera::setFOV", "DAVA::Camera::SetFovY", "Client::LeaveToHangar"):
    assert required in names, f"'{required}' was published before and must keep working"

# Names that share an anchor are the same function, so they must cost the same
# permission. runtime_services.cpp lists only the canonical spelling of each
# managed target and covers every alias by comparing resolved addresses; if
# that ever regresses to a plain string compare, each alias below turns into a
# free way to skip the gameplay-tweak grant.
managed_start = runtime_text.index("kManagedHookTargets[] = {")
managed = dict(
    re.findall(
        r'\{"([^"]+)",\s*"([^"]+)"\}',
        runtime_text[managed_start:runtime_text.index("};", managed_start)],
    )
)
assert managed, "kManagedHookTargets did not parse - did the table change shape?"

ADDRESS_KEYED = "ResolveReviewedSymbol(target.canonical_symbol)"

groups = {}
for name, anchor_id in published:
    groups.setdefault(anchor_id, []).append(name)

alias_count = 0
for anchor_id, group in sorted(groups.items()):
    costs = {managed[name] for name in group if name in managed}
    assert len(costs) <= 1, (
        f"anchor {anchor_id} is published as {sorted(group)}, and those names "
        f"are listed with different permissions {sorted(costs)}; one address "
        "cannot cost two different grants"
    )
    aliases = sorted(set(group) - set(managed))
    if costs and aliases:
        alias_count += len(aliases)
        assert ADDRESS_KEYED in runtime_text, (
            f"{aliases} resolve to the same address as a target requiring "
            f"{sorted(costs)[0]}, but runtime_services.cpp no longer keys the "
            "permission on the resolved address; those spellings are now a "
            "free bypass"
        )

distinct = {anchor_id for _, anchor_id in published}
assert len(distinct) == 35, f"expected 35 published anchors, found {len(distinct)}"

print(
    f"hook symbol table: {len(names)} names -> {len(distinct)} anchors; "
    f"{len(DATA_ANCHORS)} data + {len(CRT_ANCHORS)} CRT + "
    f"{len(UNNAMED_ANCHORS)} unnamed correctly withheld; "
    f"{len(advertised_hookable)} advertised names agree on their anchor; "
    f"{alias_count} managed alias(es) priced by address"
)
