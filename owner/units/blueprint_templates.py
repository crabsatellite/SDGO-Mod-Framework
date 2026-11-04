"""Source-bound client presentation templates for native blueprints.

This module validates complete kind-33 recipe and kind-34 blueprint-item
records.  The records are client presentation inputs only: they never become
server definitions, replace the existing baseline/server donors, or authorize
native registration. Version 1 validates direct record references; version 2
also validates the prepared SPR/TXR closure. Target runtime behavior remains a
separate check, and validation alone never authorizes a package installation.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import PurePosixPath
import re
import struct
from typing import Any, Mapping

from . import blueprint_codec, codec


SCHEMA = "sdgo.unit-blueprint-client-templates/1"
RESOURCE_CLOSURE_SCHEMA = "sdgo.unit-blueprint-client-templates/2"
BINDING_FIELDS = frozenset({"manifest_path", "manifest_sha256"})
RECIPE_ROLE = "client_recipe_template"
ITEM_ROLE = "client_item_template"
ROLES = frozenset({RECIPE_ROLE, ITEM_ROLE})
DISPLAY_NAME_MODES = frozenset({"preserve-template", "explicit"})
MAX_RESOURCE_BYTES = 64 * 1024 * 1024

_SHA256 = re.compile(r"[0-9a-f]{64}")
_OWNER = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
_MEMBER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/-]{0,239}")
_RESOURCE = re.compile(
    rf"(?:{'|'.join(codec.RESOURCE_ROOTS)})/"
    r"(?:[a-z0-9][a-z0-9_.-]*/)*[a-z0-9][a-z0-9_.-]*"
)

_KIND_BY_ROLE = {RECIPE_ROLE: 33, ITEM_ROLE: 34}
_SIZE_BY_ROLE = {RECIPE_ROLE: 1197, ITEM_ROLE: 614}
_RECIPE_SPRITE_OFFSETS = frozenset(
    {
        109,
        169,
        289,
        349,
        409,
        469,
        529,
        589,
        649,
        709,
        769,
        829,
        889,
        949,
        1009,
        1069,
        1129,
    }
)
_APPROVED_REWRITE_SPANS = {
    RECIPE_ROLE: frozenset((offset, 60) for offset in _RECIPE_SPRITE_OFFSETS),
    ITEM_ROLE: frozenset({(114, 28)}),
}


def _exact_dict(
    value: Any, allowed: set[str], required: set[str], label: str
) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{label} must be an object")
    unknown = set(value) - allowed
    missing = required - set(value)
    if unknown:
        raise ValueError(f"{label} has unknown fields: {sorted(unknown)}")
    if missing:
        raise ValueError(f"{label} is missing fields: {sorted(missing)}")
    return value


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if (
        isinstance(value, bool)
        or not isinstance(value, int)
        or not minimum <= value <= maximum
    ):
        raise ValueError(f"{label} must be in {minimum}..{maximum}")
    return value


def _hash(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ValueError(f"{label} must be a lowercase SHA-256")
    return value


def _owner(value: Any) -> str:
    if not isinstance(value, str) or not _OWNER.fullmatch(value):
        raise ValueError("blueprint template owner is invalid")
    return value


def _member(value: Any, label: str) -> str:
    if (
        not isinstance(value, str)
        or "\\" in value
        or not _MEMBER.fullmatch(value)
    ):
        raise ValueError(f"{label} must be a bounded archive member")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError(f"{label} must be a relative archive member")
    if path.as_posix() != value:
        raise ValueError(f"{label} must be canonical")
    return value


def _resource_path(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a native resource path")
    normalized = value.replace("\\", "/").lower()
    if (
        len(normalized) > 240
        or not _RESOURCE.fullmatch(normalized)
        or any(part in ("", ".", "..") for part in normalized.split("/"))
    ):
        raise ValueError(f"{label} must be a native resource path")
    return normalized


def _native_resource(value: str, size: int, label: str) -> bytes:
    normalized = _resource_path(value, label)
    encoded = normalized.replace("/", "\\").encode("utf-16-le")
    if len(encoded) + 2 > size:
        raise ValueError(f"{label} exceeds its native carrier")
    return encoded.ljust(size, b"\0")


def _native_text(raw: bytes, offset: int, size: int, label: str) -> str:
    field = raw[offset : offset + size]
    if len(field) != size or size % 2:
        raise ValueError(f"{label} is truncated")
    end = None
    for index in range(0, size, 2):
        if field[index : index + 2] == b"\0\0":
            end = index
            break
    if end is None:
        raise ValueError(f"{label} is not NUL terminated")
    try:
        value = field[:end].decode("utf-16-le")
    except UnicodeDecodeError:
        raise ValueError(f"{label} is not valid UTF-16LE") from None
    if not value or any(ord(character) < 32 for character in value):
        raise ValueError(f"{label} is not displayable text")
    return value


def _display_name(value: Any, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-16-le")) + 2 > 100
        or any(ord(character) < 32 for character in value)
    ):
        raise ValueError(f"{label} does not fit the native item name")
    return value


def _blob(
    files: Mapping[str, bytes],
    member: str,
    size: int | None,
    expected_hash: str,
    label: str,
) -> bytes:
    if member not in files:
        raise ValueError(f"{label} is missing: {member}")
    raw = files[member]
    if not isinstance(raw, (bytes, bytearray)):
        raise ValueError(f"{label} must be bytes")
    raw = bytes(raw)
    if size is not None and len(raw) != size:
        raise ValueError(f"{label} must be {size} bytes")
    if size is None and not 0 < len(raw) <= MAX_RESOURCE_BYTES:
        raise ValueError(f"{label} has an invalid resource size")
    if hashlib.sha256(raw).hexdigest() != expected_hash:
        raise ValueError(f"{label} SHA-256 mismatch")
    return raw


def _record(
    raw: bytes, role: str, source_record_id: int, label: str
) -> dict[str, Any]:
    if role == RECIPE_ROLE:
        decoded = blueprint_codec.decode(raw)
        if decoded["id"] != source_record_id:
            raise ValueError(f"{label} recipe identity mismatch")
        return {
            "id": decoded["id"],
            "display_name": decoded["name"],
            "reference_id": None,
            "resource_refs": decoded["resource_refs"],
        }

    embedded = struct.unpack_from("<I", raw, 0)[0]
    if embedded != source_record_id:
        raise ValueError(f"{label} item identity mismatch")
    if raw[4] != 0:
        raise ValueError(f"{label} item security/release byte must be zero")
    if embedded // 100000 != 15:
        raise ValueError(f"{label} is not a class-15 blueprint item")
    reference = struct.unpack_from("<I", raw, 105)[0]
    if not 1 <= reference <= codec.MAX_NATIVE_ID:
        raise ValueError(f"{label} item recipe reference is invalid")
    return {
        "id": embedded,
        "display_name": _native_text(raw, 5, 100, f"{label} item name"),
        "reference_id": reference,
        "resource_refs": codec.scan_resource_references(raw, ".bin"),
    }


def _rewrite_spans(
    value: Any,
    *,
    role: str,
    source: bytes,
    prepared: bytes,
) -> tuple[list[dict[str, Any]], set[str]]:
    if not isinstance(value, list) or len(value) > 32:
        raise ValueError("template rewrite_spans must be a bounded list")
    approved = _APPROVED_REWRITE_SPANS[role]
    occupied: set[int] = set()
    normalized: list[dict[str, Any]] = []
    resources: set[str] = set()
    for index, raw in enumerate(value):
        span = _exact_dict(
            raw,
            {"offset", "bytes", "source_value", "prepared_value"},
            {"offset", "bytes", "source_value", "prepared_value"},
            f"rewrite span {index}",
        )
        offset = _integer(span["offset"], "rewrite offset", 0, len(source) - 1)
        size = _integer(span["bytes"], "rewrite size", 2, len(source))
        if (offset, size) not in approved:
            raise ValueError(
                f"rewrite span {offset}..{offset + size} is not approved "
                f"for {role}"
            )
        positions = set(range(offset, offset + size))
        if occupied & positions:
            raise ValueError("template rewrite spans overlap")
        occupied.update(positions)
        source_value = _resource_path(
            span["source_value"], f"rewrite span {index} source"
        )
        prepared_value = _resource_path(
            span["prepared_value"], f"rewrite span {index} prepared"
        )
        if source_value == prepared_value:
            raise ValueError("template rewrite span does not change its resource")
        if source[offset : offset + size] != _native_resource(
            source_value, size, f"rewrite span {index} source"
        ):
            raise ValueError(f"rewrite span {index} source bytes mismatch")
        if prepared[offset : offset + size] != _native_resource(
            prepared_value, size, f"rewrite span {index} prepared"
        ):
            raise ValueError(f"rewrite span {index} prepared bytes mismatch")
        resources.update((source_value, prepared_value))
        normalized.append(
            {
                "offset": offset,
                "bytes": size,
                "source_value": source_value,
                "prepared_value": prepared_value,
            }
        )
    normalized.sort(key=lambda span: span["offset"])
    for offset, (before, after) in enumerate(zip(source, prepared)):
        if offset not in occupied and before != after:
            raise ValueError(
                "prepared template changes bytes outside approved rewrite spans"
            )
    return normalized, resources


def _template(
    value: Any,
    *,
    files: Mapping[str, bytes],
    used_members: set[str],
) -> dict[str, Any]:
    common = {
        "role",
        "kind",
        "bytes",
        "source_record_id",
        "source_sha256",
        "prepared_sha256",
        "source_path",
        "prepared_path",
        "rewrite_spans",
        "preserve_outside_rewrite_spans",
    }
    template = _exact_dict(
        value,
        common | {"display_name_mode", "display_name"},
        common,
        "blueprint client template",
    )
    role = template["role"]
    if role not in ROLES:
        raise ValueError("unsupported blueprint client template role")
    if template["kind"] != _KIND_BY_ROLE[role]:
        raise ValueError(f"{role} has the wrong typed record kind")
    if template["bytes"] != _SIZE_BY_ROLE[role]:
        raise ValueError(f"{role} has the wrong typed record size")
    if template["preserve_outside_rewrite_spans"] is not True:
        raise ValueError(f"{role} must preserve bytes outside rewrite spans")

    source_record_id = _integer(
        template["source_record_id"], f"{role} source_record_id", 1, 9_999_999
    )
    source_hash = _hash(template["source_sha256"], f"{role} source")
    prepared_hash = _hash(template["prepared_sha256"], f"{role} prepared")
    source_path = _member(template["source_path"], f"{role} source_path")
    prepared_path = _member(template["prepared_path"], f"{role} prepared_path")
    if (
        PurePosixPath(source_path).suffix.lower() != ".bin"
        or PurePosixPath(prepared_path).suffix.lower() != ".bin"
    ):
        raise ValueError(f"{role} source and prepared members must be .bin")
    if source_path == prepared_path:
        raise ValueError(f"{role} source and prepared members must be distinct")
    for member in (source_path, prepared_path):
        if member in used_members:
            raise ValueError(f"blueprint template member is reused: {member}")
        used_members.add(member)

    size = _SIZE_BY_ROLE[role]
    source = _blob(files, source_path, size, source_hash, f"{role} source")
    prepared = _blob(
        files, prepared_path, size, prepared_hash, f"{role} prepared"
    )
    source_record = _record(
        source, role, source_record_id, f"{role} source"
    )
    prepared_record = _record(
        prepared, role, source_record_id, f"{role} prepared"
    )
    spans, rewritten_resources = _rewrite_spans(
        template["rewrite_spans"],
        role=role,
        source=source,
        prepared=prepared,
    )

    mode = template.get("display_name_mode")
    explicit_name = template.get("display_name")
    if role == ITEM_ROLE:
        if mode not in DISPLAY_NAME_MODES:
            raise ValueError("client item template display_name_mode is invalid")
        if mode == "preserve-template":
            if explicit_name is not None:
                raise ValueError(
                    "preserve-template item naming cannot supply display_name"
                )
            display_name = prepared_record["display_name"]
        else:
            display_name = _display_name(
                explicit_name, "explicit blueprint item display_name"
            )
    elif mode is not None or explicit_name is not None:
        raise ValueError("recipe template cannot define item naming fields")
    else:
        display_name = prepared_record["display_name"]

    resource_refs = set(source_record["resource_refs"])
    resource_refs.update(prepared_record["resource_refs"])
    resource_refs.update(rewritten_resources)
    return {
        "role": role,
        "kind": _KIND_BY_ROLE[role],
        "bytes": size,
        "source_record_id": source_record_id,
        "source": {"path": source_path, "sha256": source_hash},
        "prepared": {"path": prepared_path, "sha256": prepared_hash},
        "rewrite_spans": spans,
        "preserve_outside_rewrite_spans": True,
        "display_name_mode": mode,
        "display_name": display_name,
        "recipe_reference_id": prepared_record["reference_id"],
        "resource_refs": sorted(resource_refs),
        "source_resource_refs": sorted(source_record["resource_refs"]),
        "prepared_resource_refs": sorted(prepared_record["resource_refs"]),
        "client_presentation_only": True,
        "server_definition_eligible": False,
    }


def validate_manifest(
    value: Any, *, files: Mapping[str, bytes]
) -> dict[str, Any]:
    """Validate a complete recipe/item client-template binding.

    ``files`` is the caller's already-bounded package member map.  Every source
    and prepared record and every resource path directly referenced by either
    record or an approved rewrite span must be declared, present, and
    SHA-bound.  An absent project member is always rejected.

    This validator does not parse the resource payloads.  In particular, it
    does not follow SPR-to-TXR or other format-specific dependency edges.
    Callers must run a semantic resource-closure validator before treating the
    package as complete or eligible for native runtime registration.
    """
    schema = value.get('schema') if isinstance(value,Mapping) else None
    optional = {'resource_dependencies'} if schema == RESOURCE_CLOSURE_SCHEMA else set()
    manifest = _exact_dict(
        value,
        {"schema", "owner", "templates", "resources"} | optional,
        {"schema", "owner", "templates", "resources"},
        "blueprint client-template manifest",
    )
    if manifest["schema"] not in {SCHEMA,RESOURCE_CLOSURE_SCHEMA}:
        raise ValueError("unsupported blueprint client-template schema")
    owner = _owner(manifest["owner"])
    if not isinstance(files, Mapping):
        raise ValueError("blueprint template files must be a member map")

    resources = manifest["resources"]
    if not isinstance(resources, list) or len(resources) > 128:
        raise ValueError("blueprint template resources must be a bounded list")
    normalized_resources: dict[str, dict[str, str]] = {}
    resource_members: set[str] = set()
    for index, raw in enumerate(resources):
        entry = _exact_dict(
            raw,
            {"path", "member", "sha256"},
            {"path", "member", "sha256"},
            f"blueprint template resource {index}",
        )
        path = _resource_path(entry["path"], f"resource {index} path")
        member = _member(entry["member"], f"resource {index} member")
        if PurePosixPath(path).suffix.lower() != PurePosixPath(member).suffix.lower():
            raise ValueError(
                f"blueprint template resource {index} member suffix mismatch"
            )
        digest = _hash(entry["sha256"], f"resource {index}")
        if path in normalized_resources:
            raise ValueError(f"duplicate blueprint template resource: {path}")
        if member in resource_members:
            raise ValueError(
                f"blueprint template resource member is reused: {member}"
            )
        resource_members.add(member)
        _blob(files, member, None, digest, f"resource {path}")
        normalized_resources[path] = {
            "path": path,
            "member": member,
            "sha256": digest,
        }

    templates = manifest["templates"]
    if not isinstance(templates, list) or len(templates) != len(ROLES):
        raise ValueError("manifest needs one recipe and one item client template")
    used_members = set(resource_members)
    normalized_templates: dict[str, dict[str, Any]] = {}
    for raw in templates:
        entry = _template(raw, files=files, used_members=used_members)
        role = entry["role"]
        if role in normalized_templates:
            raise ValueError(f"duplicate blueprint client template role: {role}")
        normalized_templates[role] = entry
    if set(normalized_templates) != ROLES:
        raise ValueError("manifest needs one recipe and one item client template")

    recipe = normalized_templates[RECIPE_ROLE]
    item = normalized_templates[ITEM_ROLE]
    if item["recipe_reference_id"] != recipe["source_record_id"]:
        raise ValueError(
            "blueprint item template does not reference the recipe template"
        )
    required_resources = set(recipe["resource_refs"]) | set(
        item["resource_refs"]
    )
    declared_resources = set(normalized_resources)
    missing = sorted(required_resources - declared_resources)
    extra = sorted(declared_resources - required_resources)
    if missing:
        raise ValueError(
            f"blueprint template resources are missing: {missing}"
        )
    if extra:
        raise ValueError(
            f"blueprint template resources are unreferenced: {extra}"
        )

    prepared_refs = set(recipe['prepared_resource_refs']) | set(item['prepared_resource_refs'])
    runtime_resources = {path:normalized_resources[path] for path in sorted(prepared_refs)}
    dependency_resources = []
    closure = None
    if manifest['schema'] == RESOURCE_CLOSURE_SCHEMA:
        dependencies = manifest.get('resource_dependencies')
        if not isinstance(dependencies,list) or len(dependencies)>128:
            raise ValueError('blueprint resource_dependencies must be a bounded list')
        members = set(used_members)
        for index,raw in enumerate(dependencies):
            entry = _exact_dict(raw,{'path','member','sha256'},{'path','member','sha256'},'blueprint texture dependency')
            path = _resource_path(entry['path'],'texture dependency path')
            member = _member(entry['member'],'texture dependency member')
            digest = _hash(entry['sha256'],'texture dependency')
            if not path.startswith('txrs/') or not path.endswith('.txr') or PurePosixPath(member).suffix.lower()!='.txr':
                raise ValueError('blueprint dependency must be a TXR resource')
            if path in normalized_resources or path in runtime_resources or member in members:
                raise ValueError('blueprint dependency path/member is duplicated or reused')
            _blob(files,member,None,digest,'texture dependency '+path)
            normalized = {'path':path,'member':member,'sha256':digest}
            dependency_resources.append(normalized);runtime_resources[path]=normalized;members.add(member)
        from .blueprint_resources import inspect_closure
        closure = inspect_closure(sorted(prepared_refs),runtime_resources,lambda member:files[member])

    return {
        "schema": manifest['schema'],
        "owner": owner,
        "templates": copy.deepcopy(normalized_templates),
        "resources": [
            copy.deepcopy(normalized_resources[path])
            for path in sorted(normalized_resources)
        ],
        'resource_dependencies':sorted(dependency_resources,key=lambda row:row['path']),
        'runtime_resources':[copy.deepcopy(runtime_resources[path]) for path in sorted(runtime_resources)],
        'resource_closure':closure,
        "admission": {
            "typed_record_kinds": {
                RECIPE_ROLE: 33,
                ITEM_ROLE: 34,
            },
            "source_prepared_sha_bound": True,
            "outside_rewrite_spans_preserved": True,
            "direct_resource_refs_verified": True,
            "resource_closure_verified": closure is not None,
            "semantic_resource_closure_required": closure is None,
            "target_native_ui_acceptance_required": True,
            "item_template_name_preserved": (
                item["display_name_mode"] == "preserve-template"
            ),
            "client_presentation_only": True,
            "server_definition_eligible": False,
            "native_registration_authorized": False,
            "native_runtime_accepted": False,
        },
    }


def require_native_admission(validated_manifest):
    """Apply framework support policy to a freshly validated manifest.

    Producer-supplied flags are not accepted by the manifest schema. Resource
    closure here is derived by validate_manifest, never supplied by a package.
    This permits compilation, not a claim of acceptance for that owner's art.
    """
    from .blueprints import RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED
    if not RAW_TEMPLATE_NATIVE_REGISTRATION_SUPPORTED:
        raise ValueError('完整展示模板的原生编译尚未开放')
    if (validated_manifest['schema'] != RESOURCE_CLOSURE_SCHEMA or
            not validated_manifest['admission']['resource_closure_verified']):
        raise ValueError('完整展示模板发布需要版本2清单及完整的准备 SPR→TXR 资源闭包；版本1仅用于传输')


def normalize_binding(value: Any) -> dict[str, str]:
    """Normalize the small declaration-side pointer to a typed manifest.

    The manifest and every blob it names remain project members.  Keeping this
    pointer separate from the baseline/server donor IDs prevents an uploaded
    client record from silently becoming a server compatibility template.
    """
    binding = _exact_dict(
        value,
        set(BINDING_FIELDS),
        set(BINDING_FIELDS),
        "blueprint client-template binding",
    )
    manifest_path = _member(
        binding["manifest_path"], "blueprint client-template manifest_path"
    )
    if PurePosixPath(manifest_path).suffix.lower() != ".json":
        raise ValueError("blueprint client-template manifest must be JSON")
    return {
        "manifest_path": manifest_path,
        "manifest_sha256": _hash(
            binding["manifest_sha256"],
            "blueprint client-template manifest",
        ),
    }


def validate_binding(
    value: Any, *, files: Mapping[str, bytes]
) -> dict[str, Any]:
    """Resolve and validate one declaration's complete client-template input."""
    binding = normalize_binding(value)
    raw = _blob(
        files,
        binding["manifest_path"],
        None,
        binding["manifest_sha256"],
        "blueprint client-template manifest",
    )
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("blueprint client-template manifest exceeds 2 MiB")
    try:
        manifest_value = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeError, ValueError):
        raise ValueError(
            "blueprint client-template manifest is not valid UTF-8 JSON"
        ) from None
    manifest = validate_manifest(manifest_value, files=files)
    template_members: set[str] = set()
    for template in manifest["templates"].values():
        template_members.update(
            (template["source"]["path"], template["prepared"]["path"])
        )
    resource_members = {
        resource["member"] for resource in manifest["resources"]+manifest['resource_dependencies']
    }
    if binding["manifest_path"] in template_members | resource_members:
        raise ValueError(
            "blueprint client-template manifest cannot also be a payload member"
        )
    return {
        "binding": binding,
        "manifest": manifest,
        "members": sorted(
            {binding["manifest_path"]} | template_members | resource_members
        ),
        "template_members": sorted(template_members),
        "resource_members": sorted(resource_members),
    }


def validate_project_bindings(
    definitions: Any, *, files: Mapping[str, bytes]
) -> list[dict[str, Any]]:
    """Validate all declaration bindings in a project without enabling them."""
    if not isinstance(definitions, Mapping):
        return []
    declarations = definitions.get("blueprints", [])
    if not isinstance(declarations, list):
        return []
    results: list[dict[str, Any]] = []
    manifest_members: set[str] = set()
    template_members: set[str] = set()
    resource_map: dict[str, tuple[str, str]] = {}
    for index, declaration in enumerate(declarations):
        if not isinstance(declaration, Mapping):
            continue
        binding = declaration.get("client_templates")
        if binding is None:
            continue
        result = validate_binding(binding, files=files)
        manifest_path = result["binding"]["manifest_path"]
        if manifest_path in manifest_members:
            raise ValueError(
                "blueprint declarations cannot reuse a client-template manifest"
            )
        manifest_members.add(manifest_path)
        overlap = template_members & set(result["template_members"])
        if overlap:
            raise ValueError(
                "blueprint declarations cannot reuse typed template records: "
                + ", ".join(sorted(overlap))
            )
        template_members.update(result["template_members"])
        for resource in result["manifest"]["resources"]+result['manifest']['resource_dependencies']:
            key = resource["path"]
            witness = (resource["member"], resource["sha256"])
            if key in resource_map and resource_map[key] != witness:
                raise ValueError(
                    "blueprint declarations disagree about resource " + key
                )
            resource_map[key] = witness
        results.append(
            {
                "declaration_index": index,
                "blueprint_id": declaration.get("id"),
                **result,
            }
        )
    return results


def _changed_spans(
    before: bytes,
    after: bytes,
    fields: list[tuple[str, int, int]],
) -> list[dict[str, Any]]:
    if len(before) != len(after):
        raise ValueError("compiled blueprint template changed record size")
    allowed: dict[int, str] = {}
    for name, offset, size in fields:
        for position in range(offset, offset + size):
            if position in allowed:
                raise ValueError("compiled blueprint field spans overlap")
            allowed[position] = name
    changed = [index for index, pair in enumerate(zip(before, after)) if pair[0] != pair[1]]
    if any(position not in allowed for position in changed):
        raise ValueError(
            "compiled blueprint template changed bytes outside known fields"
        )
    spans: list[dict[str, Any]] = []
    cursor = 0
    while cursor < len(changed):
        start = changed[cursor]
        field = allowed[start]
        end = start + 1
        cursor += 1
        while (
            cursor < len(changed)
            and changed[cursor] == end
            and allowed[changed[cursor]] == field
        ):
            end += 1
            cursor += 1
        spans.append(
            {
                "field": field,
                "offset": start,
                "bytes": end - start,
                "before_sha256": hashlib.sha256(before[start:end]).hexdigest(),
                "after_sha256": hashlib.sha256(after[start:end]).hexdigest(),
            }
        )
    return spans


def compile_binding(
    declaration: Mapping[str, Any],
    *,
    root_unit_id: int,
    files: Mapping[str, bytes],
) -> dict[str, Any]:
    """Compile reviewed prepared records while retaining every unknown byte."""
    from . import blueprints

    normalized = blueprints.normalize(
        dict(declaration), root_unit_id=root_unit_id
    )
    compiling = copy.deepcopy(normalized)
    compiling["enabled"] = True
    blueprints.server_rows(compiling, root_unit_id=root_unit_id)
    resolved = validate_binding(compiling["client_templates"], files=files)
    manifest = resolved["manifest"]
    recipe_template = manifest["templates"][RECIPE_ROLE]
    item_template = manifest["templates"][ITEM_ROLE]

    prepared_recipe = bytes(files[recipe_template["prepared"]["path"]])
    decoded_recipe = blueprint_codec.decode(prepared_recipe)
    requested_sprite = compiling.get("recipe_sprite")
    if requested_sprite and requested_sprite != decoded_recipe.get("recipe_sprite"):
        raise ValueError(
            "bound client recipe template does not contain recipe_sprite"
        )
    recipe = bytearray(prepared_recipe)
    codec._put_utf16(recipe, 5, 100, compiling["name"], "blueprint name")
    struct.pack_into("<I", recipe, 0, compiling["id"])
    struct.pack_into("<I", recipe, blueprint_codec.COUNT_OFFSET, len(compiling["materials"]))
    struct.pack_into("<I", recipe, blueprint_codec.OUTPUT_OFFSET, root_unit_id)
    for slot in range(7):
        material = (
            compiling["materials"][slot]
            if slot < len(compiling["materials"])
            else None
        )
        struct.pack_into(
            "<I",
            recipe,
            blueprint_codec.UNIT_OFFSET + 4 * slot,
            material["unit_id"] if material else 0,
        )
        if material:
            struct.pack_into(
                "<I",
                recipe,
                blueprint_codec.LEVEL_OFFSET + 4 * slot,
                material["minimum_level"],
            )
    struct.pack_into(
        "<I",
        recipe,
        blueprint_codec.MINIMUM_PLAYER_OFFSET,
        compiling["minimum_player_level"],
    )
    struct.pack_into(
        "<I",
        recipe,
        blueprint_codec.REPAY_OFFSET,
        compiling["repay_point"],
    )
    recipe = bytes(recipe)
    decoded_compiled_recipe = blueprint_codec.decode(recipe)
    if (
        decoded_compiled_recipe["id"] != compiling["id"]
        or decoded_compiled_recipe["output_unit_id"] != root_unit_id
    ):
        raise ValueError("compiled client recipe identity mismatch")

    prepared_item = bytes(files[item_template["prepared"]["path"]])
    item = bytearray(prepared_item)
    struct.pack_into("<I", item, 0, compiling["item_category_id"])
    if item_template["display_name_mode"] == "explicit":
        codec._put_utf16(
            item,
            5,
            100,
            item_template["display_name"],
            "blueprint item name",
        )
    struct.pack_into("<I", item, 105, compiling["id"])
    item = bytes(item)
    decoded_compiled_item = _record(
        item,
        ITEM_ROLE,
        compiling["item_category_id"],
        "compiled client item",
    )
    if decoded_compiled_item["reference_id"] != compiling["id"]:
        raise ValueError("compiled client item recipe reference mismatch")

    recipe_fields = [
        ("identity", 0, 4),
        ("display_name", 5, 100),
        ("material_count", blueprint_codec.COUNT_OFFSET, 4),
        ("output_unit_id", blueprint_codec.OUTPUT_OFFSET, 4),
        ("material_unit_ids", blueprint_codec.UNIT_OFFSET, 28),
        ("material_minimum_levels", blueprint_codec.LEVEL_OFFSET, 28),
        ("minimum_player_level", blueprint_codec.MINIMUM_PLAYER_OFFSET, 4),
        ("repay_point", blueprint_codec.REPAY_OFFSET, 4),
    ]
    item_fields = [
        ("identity", 0, 4),
        ("display_name", 5, 100),
        ("recipe_reference_id", 105, 4),
    ]

    def receipt(
        role: str,
        prepared: bytes,
        compiled: bytes,
        fields: list[tuple[str, int, int]],
        target_id: int,
    ) -> dict[str, Any]:
        template = manifest["templates"][role]
        source = bytes(files[template["source"]["path"]])
        return {
            "role": role,
            "kind": template["kind"],
            "bytes": len(compiled),
            "source_record_id": template["source_record_id"],
            "target_record_id": target_id,
            "source_sha256": hashlib.sha256(source).hexdigest(),
            "prepared_sha256": hashlib.sha256(prepared).hexdigest(),
            "compiled_sha256": hashlib.sha256(compiled).hexdigest(),
            "compiled_changes": _changed_spans(prepared, compiled, fields),
            "unknown_bytes_preserved": True,
        }

    return {
        "recipe": recipe,
        "item": item,
        "resolved": resolved,
        'runtime_resources':copy.deepcopy(manifest['runtime_resources']),
        "receipt": {
            "schema": "sdgo.unit-blueprint-client-template-receipt/1",
            "blueprint_id": compiling["id"],
            "item_category_id": compiling["item_category_id"],
            "owner": manifest["owner"],
            "manifest": copy.deepcopy(resolved["binding"]),
            "records": [
                receipt(
                    RECIPE_ROLE,
                    prepared_recipe,
                    recipe,
                    recipe_fields,
                    compiling["id"],
                ),
                receipt(
                    ITEM_ROLE,
                    prepared_item,
                    item,
                    item_fields,
                    compiling["item_category_id"],
                ),
            ],
            "direct_resource_refs_verified": True,
            "resource_closure_verified": manifest['admission']['resource_closure_verified'],
            'resource_closure':copy.deepcopy(manifest['resource_closure']),
            'runtime_resources':copy.deepcopy(manifest['runtime_resources']),
            "native_registration_authorized": False,
            "native_runtime_accepted": False,
        },
    }
