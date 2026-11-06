"""Stage 3: owner-scoped native programs for every weapon callback of the roster.

Weapon records name their ranged, special-ranged and projectile-initializer
callbacks by global function name. Names the selected baseline client already
defines are reused as they are. Names only the recovered plain-text scripts
define are compiled with the official Lua 5.0 compiler, scoped under the owner
prefix (exports, imports between scoped modules, mutable scratch globals,
callback-name arguments and aliased resource constants) and registered through
the framework's closure-publication program transport. Modules whose root does
anything but publish closures, and names no script defines, are reported open.
"""
from __future__ import annotations

from pathlib import Path, PurePosixPath
import re
import struct
import tempfile

from owner.units.assets import Baseline
from owner.units.native_programs import CALLBACK_ROLES, CALLBACK_SCHEMA, compile_bundle, inspect_chunk, validate_manifest
from tools.gunion.lua50_compile import Lua50CompileError, compile_source, discover_toolchain, global_function_bindings, parse_chunk
from tools.gunion.lua50_scope import Rewrite, constant_uses, lower, prototypes
from tools.gunion.luajit import LuaJITFormatError, parse_dump
from tools.gunion.luajit_lower import LoweringError, native_lua50_chunk

from .common import Config, WEAPON_BYTES, dump_json, load_json, normalize_path, sha256, wide_field, write_wide

SCHEMA = 'sdgo.gunion-roster-programs/1'
_IDENTIFIER = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
_TEXT_FUNCTION = re.compile(r'^\s*function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', re.M)
_RESOURCE = re.compile(r'^(gdrs|mdrs|anrs|txrs|fxrs|sprs|sdrs|scp)[\\/][A-Za-z0-9_.\-\\/]+\.[A-Za-z0-9]+$')


def _callback_fields(raw: bytes) -> dict[str, str]:
    return {role: wide_field(raw, offset, 100) for role, (_, offset) in CALLBACK_ROLES.items()}


def _root_exports(chunk) -> list[str] | None:
    """Return export names when the root only publishes closures, else None."""
    root = chunk.root
    code = root.code
    if not code or code[-1].name != 'RETURN':
        return None
    names = []
    pc = 0
    while pc + 1 < len(code):
        closure, publication = code[pc], code[pc + 1]
        if closure.name != 'CLOSURE' or publication.name != 'SETGLOBAL' or closure.a != publication.a:
            return None
        constant = root.constants[publication.bx]
        if constant.kind != 'string':
            return None
        names.append(constant.value.decode('ascii', 'replace'))
        pc += 2
    if pc != len(code) - 1:
        return None
    return names


def build(config: Config) -> dict:
    families = load_json(config.output / 'families.json')
    resources = load_json(config.output / 'resources.json')
    owner = config.owner
    prefix = owner + '_'
    toolchain = discover_toolchain(config.lua_toolchain_root)
    baseline = Baseline(config.baseline_client)
    baseline._loose_roots = []
    baseline_defined: set[str] = set()
    baseline_unparsed = 0
    for path in baseline.entries:
        if not path.startswith('scp/'):
            continue
        try:
            chunk = parse_chunk(baseline.read(path))
        except Exception:
            baseline_unparsed += 1
            continue
        for binding in global_function_bindings(chunk.root):
            name = binding.name if isinstance(binding.name, str) else binding.name.decode('ascii', 'replace')
            baseline_defined.add(name)

    # Alias map from the resource stage: logical path -> shipped target (unambiguous only).
    alias_targets: dict[str, set[str]] = {}
    for target, row in resources['resources'].items():
        alias_targets.setdefault(row['source_path'], set()).add(target)
    alias_map = {path: next(iter(targets)) for path, targets in alias_targets.items() if len(targets) == 1 and next(iter(targets)) != path}
    ambiguous_aliases = sorted(path for path, targets in alias_targets.items() if len(targets) > 1)

    # Earlier bounded work already lowered a few LuaJIT modules and validated them
    # against finite VM traces; their pre-scoping derived bytes are reused as a
    # check that this lowering reproduces exactly the same instruction carrier.
    derived_references: dict[str, str] = {}
    for manifest_relative in config.prescoped_program_manifests:
        manifest = load_json(config.delivery / manifest_relative)
        for program in manifest.get('modules', []):
            if program.get('sourceScript') and program.get('derivedReferenceSha256'):
                derived_references[PurePosixPath(program['sourceScript']).name] = program['derivedReferenceSha256']
    prescoped_exports: dict[str, str] = {}

    # Recovered scripts: plain text modules compile with the official Lua 5.0
    # compiler; LuaJIT dumps are lowered by the source-attributed carrier.
    scp_dir = config.native / 'resources' / 'scp'
    text_modules: dict[str, bytes] = {}
    luajit_modules: dict[str, bytes] = {}
    declared_by: dict[str, str] = {}
    for path in sorted(scp_dir.iterdir()):
        raw = path.read_bytes()
        if raw[:3] == b'\x1bLJ':
            luajit_modules[path.name] = raw
            try:
                dump = parse_dump(raw)
            except LuaJITFormatError:
                continue
            root = dump.prototypes[dump.root_prototype_id]
            for instruction in root.instructions:
                if instruction.opcode_name == 'GSET':
                    constant = root.gc_constant_for_operand(instruction.d)
                    if constant is not None and constant.kind == 'string':
                        declared_by.setdefault(constant.value.decode('ascii', 'replace'), path.name)
            continue
        if raw[:4] == b'\x1bLua':
            continue
        text_modules[path.name] = raw
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            text = raw.decode('gbk', errors='replace')
        for match in _TEXT_FUNCTION.finditer(text):
            declared_by.setdefault(match.group(1), path.name)

    # Needed callback names across the prepared weapon records.
    weapon_records: dict[int, tuple[str, Path, bytes]] = {}
    # A weapon shared by several families has one prepared member per family; the
    # resource stage members stay untouched and the rewritten record is written once
    # under programs/weapons so this stage can be rerun without a resources rerun.
    needed: dict[str, list[tuple[int, str]]] = {}
    for family in families['families']:
        receipt = next(row for row in resources['families'] if row['family_key'] == family['family_key'])
        for weapon in family['weapons']:
            member = config.output / receipt['records']['10:%d' % weapon['id']]['member']
            raw = member.read_bytes()
            if weapon['id'] in weapon_records:
                if weapon_records[weapon['id']][2] != raw:
                    raise ValueError('families prepare different bytes for weapon %d' % weapon['id'])
                continue
            weapon_records[weapon['id']] = (family['family_key'], member, raw)
            for role, name in _callback_fields(raw).items():
                if name:
                    if name.startswith(prefix):
                        name = name[len(prefix):]
                    needed.setdefault(name, []).append((weapon['id'], role))
    reuse_baseline = {name for name in needed if name in baseline_defined}
    to_scope = {name for name in needed if name not in baseline_defined}
    undefined = sorted(name for name in to_scope if name not in declared_by)

    # Module closure: modules defining needed names, plus modules defining scoped helpers they read.
    toolchain_dir = config.output / 'programs' / 'compiled'
    toolchain_dir.mkdir(parents=True, exist_ok=True)
    compiled: dict[str, object] = {}
    compile_errors: dict[str, str] = {}
    root_shape_errors: dict[str, str] = {}
    module_exports: dict[str, list[str]] = {}

    lowering_matches: dict[str, bool] = {}
    adaptations_applied: dict[str, list] = {}
    adapted_dir = config.output / 'programs' / 'adapted-sources'

    def adapted_source(name: str) -> Path:
        """Apply reviewed textual adaptations (Lua 5.1 syntax to 5.0) before compiling."""
        rules = config.script_adaptations.get(name)
        if not rules:
            return scp_dir / name
        text = text_modules[name]
        applied = []
        for rule in rules:
            find, replace = rule['find'].encode('utf-8'), rule['replace'].encode('utf-8')
            count = text.count(find)
            if count != int(rule.get('count', 1)):
                raise ValueError('script adaptation for %s matched %d times, expected %s' % (name, count, rule.get('count', 1)))
            text = text.replace(find, replace)
            applied.append({'find': rule['find'], 'replace': rule['replace'], 'count': count})
        adapted_dir.mkdir(parents=True, exist_ok=True)
        target = adapted_dir / name
        target.write_bytes(text)
        adaptations_applied[name] = applied
        return target

    def compile_module(name: str):
        if name in compiled or name in compile_errors or name in root_shape_errors:
            return
        try:
            if name in luajit_modules:
                lowered, _info = native_lua50_chunk(parse_dump(luajit_modules[name]))
                (toolchain_dir / (name + '.luac')).write_bytes(lowered)
                if name in derived_references:
                    lowering_matches[name] = sha256(lowered) == derived_references[name]
                chunk = parse_chunk(lowered)
            else:
                chunk, _receipt = compile_source(adapted_source(name), toolchain_dir / (name + '.luac'), toolchain=toolchain)
        except (Lua50CompileError, LoweringError, LuaJITFormatError, ValueError) as error:
            compile_errors[name] = str(error)[:300]
            return
        exports = _root_exports(chunk)
        if exports is None:
            root_shape_errors[name] = 'root executes statements other than closure publications'
            return
        compiled[name] = chunk
        module_exports[name] = exports

    pending = sorted({declared_by[name] for name in to_scope if name in declared_by})
    closure: list[str] = []
    while pending:
        name = pending.pop(0)
        if name in closure:
            continue
        compile_module(name)
        closure.append(name)
        chunk = compiled.get(name)
        if chunk is None:
            continue
        for prototype in prototypes(chunk.root):
            uses = constant_uses(prototype)
            for index, constant in enumerate(prototype.constants):
                if constant.kind != 'string' or not uses[index]:
                    continue
                literal = constant.value.decode('ascii', 'replace')
                operations = {use['opcode'] for use in uses[index]}
                if 'GETGLOBAL' in operations and literal not in baseline_defined and literal in declared_by and declared_by[literal] not in closure:
                    pending.append(declared_by[literal])
    scoped_modules = [name for name in closure if name in compiled]
    # Every global a scoped module publishes is renamed under the owner prefix, including
    # names the baseline also defines: the module's own closure stays isolated, while weapon
    # callbacks that name a baseline function keep binding the original mechanism.
    scoped_exports: dict[str, str] = {}
    for name in scoped_modules:
        for export in module_exports[name]:
            scoped_exports[export] = prefix + export
    # Mutable scratch globals: SETGLOBAL outside root closure publications, across scoped modules.
    mutable: set[str] = set()
    for name in scoped_modules:
        chunk = compiled[name]
        for prototype in prototypes(chunk.root):
            uses = constant_uses(prototype)
            for index, constant in enumerate(prototype.constants):
                if constant.kind != 'string':
                    continue
                literal = constant.value.decode('ascii', 'replace')
                operations = {use['opcode'] for use in uses[index]}
                if 'SETGLOBAL' in operations and not (prototype.path == () and literal in module_exports[name]):
                    if literal not in baseline_defined:
                        mutable.add(literal)
    scoped_names = {**scoped_exports, **{name: prefix + name for name in mutable}}

    programs: list[dict] = []
    files: dict[str, bytes] = {}
    open_items: list[dict] = []
    proofs: dict[str, dict] = {}
    for name in scoped_modules:
        chunk = compiled[name]
        raw = (toolchain_dir / (name + '.luac')).read_bytes()
        rewrites: list[Rewrite] = []
        module_open: list[str] = []
        for prototype in prototypes(chunk.root):
            uses = constant_uses(prototype)
            for index, constant in enumerate(prototype.constants):
                if constant.kind != 'string' or not uses[index]:
                    continue
                literal = constant.value.decode('latin-1')
                operations = {use['opcode'] for use in uses[index]}
                target = None
                role = None
                if prototype.path == () and operations == {'SETGLOBAL'} and literal in module_exports[name] and literal in scoped_exports:
                    role, target = 'global-export', scoped_exports[literal]
                elif operations == {'GETGLOBAL'} and literal in scoped_names:
                    role, target = 'global-import', scoped_names[literal]
                elif 'SETGLOBAL' in operations and literal in mutable and operations <= {'GETGLOBAL', 'SETGLOBAL'}:
                    role, target = 'mutable-global', scoped_names[literal]
                elif operations == {'LOADK'}:
                    if literal in scoped_exports:
                        role, target = 'callback-name', scoped_exports[literal]
                    elif _RESOURCE.match(literal):
                        logical = normalize_path(literal)
                        if logical in alias_map:
                            alias = alias_map[logical]
                            target = alias.replace('/', '\\') if '\\' in literal else alias
                            role = 'resource-path'
                        elif logical in ambiguous_aliases:
                            module_open.append('ambiguous alias for resource constant ' + literal)
                elif literal in scoped_names or (literal in mutable):
                    module_open.append('constant %r has mixed uses %s' % (literal, sorted(operations)))
                if role is None:
                    continue
                consumer = b'SpecialScriptFuncStr' if role == 'callback-name' else None
                rewrites.append(Rewrite(prototype=prototype.path, constant=index, source=constant.value, target=target.encode('latin-1'),
                                        role=role, authority='roster publisher: owner-scoped %s' % role, consumer_api=consumer))
        try:
            scoped, proof = lower(raw, rewrites)
        except ValueError as error:
            open_items.append({'module': name, 'reason': 'scoping refused: ' + str(error)[:200]})
            continue
        try:
            exports = inspect_chunk(scoped)
        except ValueError as error:
            open_items.append({'module': name, 'reason': 'program transport refused: ' + str(error)[:200]})
            continue
        for note in module_open:
            open_items.append({'module': name, 'reason': note})
        stem = PurePosixPath(name).stem
        resource = 'scp/%s%s.scp' % (prefix, stem)
        files[resource] = scoped
        source_raw = text_modules.get(name) or luajit_modules[name]
        proofs[resource] = {'source': 'scp/' + name, 'source_kind': 'luajit-dump' if name in luajit_modules else 'plain-text',
                            'source_sha256': sha256(source_raw), 'compiled_sha256': sha256(raw),
                            'source_adaptations': adaptations_applied.get(name, []),
                            'lowering_matches_earlier_reference': lowering_matches.get(name),
                            'scoped_sha256': sha256(scoped), 'carriers': proof['carriers'], 'exports': exports}
        programs.append({'resource': resource, 'member': resource, 'sha256': sha256(scoped), 'exports': exports})
    # Two recovered modules may publish the same global; the first compiled module wins
    # and the duplicate module is withheld as an open item rather than registered twice.
    seen_exports: dict[str, str] = {}
    kept = []
    for program in programs:
        duplicates = [export for export in program['exports'] if export in seen_exports]
        if duplicates:
            open_items.append({'module': program['resource'], 'reason': 'exports already published by %s: %s' % (
                seen_exports[duplicates[0]], duplicates)})
            files.pop(program['resource'], None)
            continue
        for export in program['exports']:
            seen_exports[export] = program['resource']
        kept.append(program)
    programs = kept
    export_map = {export: export for program in programs for export in program['exports']}

    # Rewrite weapon callback fields and bind callbacks.
    weapons_final: dict[int, bytes] = {}
    callbacks: list[dict] = []
    callback_open: list[dict] = []
    substitutions_applied: list[dict] = []
    rewritten_dir = config.output / 'programs' / 'weapons'
    if rewritten_dir.is_dir():
        for stale in rewritten_dir.glob('10-*.bin'):
            stale.unlink()
    for identity, (family_key, member, raw) in weapon_records.items():
        output = bytearray(raw)
        for role, (_, offset) in CALLBACK_ROLES.items():
            name = wide_field(raw, offset, 100)
            if not name:
                continue
            if name in config.callback_substitutions:
                replacement = config.callback_substitutions[name]
                substitutions_applied.append({'weapon_id': identity, 'family': family_key, 'role': role,
                                              'name': name, 'replacement': replacement})
                write_wide(output, offset, 50, replacement)
                name = replacement
                if not name:
                    continue
            plain = name[len(prefix):] if name.startswith(prefix) else name
            if plain in scoped_exports and scoped_exports[plain] in export_map:
                bound = scoped_exports[plain]
                if bound != name:
                    write_wide(output, offset, 50, bound)
            elif plain in baseline_defined:
                if plain != name:
                    write_wide(output, offset, 50, plain)
                continue
            else:
                callback_open.append({'weapon_id': identity, 'family': family_key, 'role': role, 'name': name,
                                      'reason': 'no baseline, pre-scoped or compiled program defines this callback'})
                continue
            callbacks.append({'weapon_id': identity, 'role': role, 'export': bound})
        final = bytes(output)
        weapons_final[identity] = final
        if final != raw:
            rewritten_dir.mkdir(parents=True, exist_ok=True)
            (rewritten_dir / ('10-%d.bin' % identity)).write_bytes(final)
    for row in callbacks:
        row['weapon_sha256'] = sha256(weapons_final[row['weapon_id']])
    declaration = {'schema': CALLBACK_SCHEMA, 'owner': owner, 'revision': '', 'vm_abi': 'lua50-x86-double64',
                   'phase': 'after-original-root-loader', 'root_profile': 'closure-publications-only-v1',
                   'programs': programs, 'weapon_callbacks': callbacks}
    declaration['revision'] = sha256(dump_json(config.output / 'programs' / 'declaration.draft.json', {k: v for k, v in declaration.items() if k != 'revision'}))
    validate_manifest(declaration, files, weapons_final)
    programs_dir = config.output / 'programs'
    for resource, raw in files.items():
        target = programs_dir / resource
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
    dump_json(programs_dir / 'declaration.json', declaration)
    counts = {'weapons': len(weapon_records), 'callback_names': len(needed), 'reused_baseline_names': len(reuse_baseline),
              'scoped_names': len(scoped_exports), 'mutable_globals': len(mutable), 'undefined_names': len(undefined),
              'text_modules': len(text_modules), 'luajit_modules': len(luajit_modules),
              'lowering_reference_matches': lowering_matches,
              'modules_in_closure': len(closure), 'modules_compiled': len(scoped_modules),
              'compile_errors': len(compile_errors), 'root_shape_errors': len(root_shape_errors),
              'programs': len(programs), 'callbacks_bound': len(callbacks),
              'callbacks_open': len(callback_open), 'open_items': len(open_items), 'ambiguous_aliases': len(ambiguous_aliases),
              'baseline_scripts_unparsed': baseline_unparsed}
    receipt = {'schema': SCHEMA, 'owner': owner, 'counts': counts, 'toolchain': toolchain.json(),
               'undefined_callback_names': undefined, 'reused_baseline_names': sorted(reuse_baseline),
               'callback_substitutions': substitutions_applied,
               'compile_errors': compile_errors, 'root_shape_errors': root_shape_errors, 'open_items': open_items,
               'callback_open': callback_open, 'mutable_globals': sorted(mutable), 'proofs': proofs,
               'declaration_sha256': sha256((programs_dir / 'declaration.json').read_bytes()),
               # Weapon callback fields are rewritten inside the resource-stage record members, so a
               # later resources rerun invalidates this stage; assemble checks these hashes.
               'families_sha256': sha256((config.output / 'families.json').read_bytes()),
               'resources_sha256': sha256((config.output / 'resources.json').read_bytes()),
               'weapon_records': {str(identity): sha256(raw) for identity, raw in weapons_final.items()},
               'native_acceptance_claimed': False}
    dump_json(config.output / 'programs.json', receipt)
    return receipt
