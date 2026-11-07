"""Read an installed client and emit a client-content-only operator pack."""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct

from . import assets, legacy_pack


OPERATOR_TABLE = 'gdrs/0255.gdt'
OPERATOR_MIN_RECORD_SIZE = 1033
DIALOGUE_RECORD_SIZE = 710
DIALOGUE_FIELDS = (502, 602)

# These are the 39 row/variant roles used by complete legacy operator voice
# replacements. The full dialogue dependency closure is still collected.
OPERATOR_VOICE_SLOTS = (
    (6, 0), (8, 0), (14, 0), (15, 0), (29, 0), (37, 0),
    (79, 0), (79, 1), (80, 0), (80, 1),
    (106, 0), (106, 1), (107, 0), (107, 1),
    (108, 0), (108, 1), (109, 0), (109, 1),
    (110, 0), (110, 1), (111, 0), (111, 1),
    (112, 0), (112, 1), (113, 0), (113, 1),
    (114, 0), (114, 1), (117, 0), (117, 1),
    (122, 0), (122, 1), (208, 0), (208, 1),
    (209, 0), (212, 0), (212, 1), (213, 0), (214, 0),
)


def _path(value):
    return legacy_pack.resource_name(value) if value else ''


def _exists(source, name):
    if not name:
        return False
    loose = source.root.joinpath(*name.split('/'))
    return loose.is_file() or name in source.entries


def _operator_rows(source):
    table = legacy_pack.Gdt(source.read(OPERATOR_TABLE))
    if table.size < OPERATOR_MIN_RECORD_SIZE:
        raise ValueError('副官总表记录缺少资源角色字段')
    rows = {}
    for row in table.rows:
        identity = struct.unpack_from('<I', row)[0]
        if identity in rows:
            raise ValueError('副官总表包含重复编号')
        rows[identity] = row
    return table.size, rows


def _collect(client, source_operator_id):
    if type(source_operator_id) is not int or not 60001 <= source_operator_id <= 60999:
        raise ValueError('来源副官编号无效')
    source = assets.Assets(client)
    record_size, rows = _operator_rows(source)
    if source_operator_id not in rows:
        raise ValueError('来源客户端没有指定副官')
    row = rows[source_operator_id]
    display_name = assets.wchar(row, 5, 50)
    dialogue = _path(assets.wchar(row, 105, 50))
    sprites = [_path(assets.wchar(row, offset, capacity))
               for offset, capacity in assets.SPRITE_FIELDS]
    intro_voice = _path(assets.wchar(row, 933, 50))
    profile = {'dialogue': dialogue, 'sprites': sprites, 'intro_voice': intro_voice}
    files, missing, invalid = {}, set(), {}
    dependency_roles = {}
    voice_events, voice_slots = [], []

    def add(name, validator, role):
        if not name:
            return None
        dependency_roles.setdefault(name, set()).add(role)
        if name in files:
            return files[name]
        if name in missing or name in invalid:
            return None
        if not _exists(source, name):
            missing.add(name)
            return None
        try:
            raw = source.read(name)
            validator(raw)
        except (ValueError, OSError) as error:
            invalid[name] = str(error)
            return None
        files[name] = raw
        return raw

    dialogue_table = None
    raw = add(
        dialogue,
        lambda value: legacy_pack.Gdt(value, DIALOGUE_RECORD_SIZE),
        'profile.dialogue',
    )
    if raw is not None:
        dialogue_table = legacy_pack.Gdt(raw, DIALOGUE_RECORD_SIZE)
        for index, speech in enumerate(dialogue_table.rows):
            for variant, offset in enumerate(DIALOGUE_FIELDS):
                voice = _path(assets.wchar(speech, offset, 50))
                if not voice:
                    continue
                if not voice.startswith('sdrs/'):
                    raise ValueError('副官台词表引用了非语音资源')
                voice_events.append({
                    'row': index,
                    'variant': variant,
                    'event': assets.wchar(speech, 0, 50),
                    'event_variant': assets.wchar(speech, 50, 50),
                    'caption': assets.wchar(speech, 100, 200),
                    'voice': voice,
                })
                add(voice, legacy_pack.voice, f'dialogue[{index}].voice[{variant}]')
        for index, variant in OPERATOR_VOICE_SLOTS:
            if index >= len(dialogue_table.rows):
                voice_slots.append({'row': index, 'variant': variant, 'missing_row': True})
                continue
            speech = dialogue_table.rows[index]
            voice = _path(assets.wchar(speech, DIALOGUE_FIELDS[variant], 50))
            voice_slots.append({
                'row': index,
                'variant': variant,
                'event': assets.wchar(speech, 0, 50),
                'event_variant': assets.wchar(speech, 50, 50),
                'caption': assets.wchar(speech, 100, 200),
                'voice': voice,
                'available': bool(voice and _exists(source, voice)),
            })

    add(intro_voice, legacy_pack.voice, 'profile.intro_voice')
    sprite_frames = {}
    for sprite_index, sprite_name in enumerate(sprites):
        raw = add(
            sprite_name,
            legacy_pack.sprite,
            f'profile.sprites[{sprite_index}]',
        )
        if raw is None:
            continue
        _, frames = legacy_pack.sprite(raw)
        sprite_frames[sprite_name] = len(frames)
        for frame_index, frame in enumerate(frames):
            add(
                frame['texture'],
                legacy_pack.texture,
                f'{sprite_name}.frames[{frame_index}].texture',
            )

    package_profile = {
        'dialogue': dialogue if dialogue in files else '',
        'sprites': [name if name in files else '' for name in sprites],
        'intro_voice': intro_voice if intro_voice in files else '',
    }
    unavailable_profile_roles = []
    if dialogue and dialogue not in files:
        unavailable_profile_roles.append({'role': 'dialogue', 'path': dialogue})
    for index, name in enumerate(sprites):
        if name and name not in files:
            unavailable_profile_roles.append({
                'role': 'sprite',
                'index': index,
                'path': name,
            })
    if intro_voice and intro_voice not in files:
        unavailable_profile_roles.append({
            'role': 'intro_voice',
            'path': intro_voice,
        })
    fallback_dependencies = sorted(
        name for name in missing
        if any(
            role.startswith('dialogue[') or '.frames[' in role
            for role in dependency_roles.get(name, ())
        )
    )
    profile_missing = sorted(
        name for name in missing
        if any(
            role.startswith('profile.')
            for role in dependency_roles.get(name, ())
        )
    )

    report = {
        'source_operator_id': source_operator_id,
        'source_name': display_name,
        'source_index_sha256': source.index_hash,
        'operator_record_size': record_size,
        'profile': profile,
        'package_profile': package_profile,
        'files': {
            name: {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
            for name, raw in sorted(files.items())
        },
        'missing_dependencies': sorted(missing),
        'profile_missing_dependencies': profile_missing,
        'native_fallback_dependencies': fallback_dependencies,
        'unavailable_profile_roles': unavailable_profile_roles,
        'invalid_dependencies': dict(sorted(invalid.items())),
        'dependency_roles': {
            name: sorted(roles)
            for name, roles in sorted(dependency_roles.items())
        },
        'sprite_frames': sprite_frames,
        'voice_events': voice_events,
        'operator_voice_slots': voice_slots,
        'operator_voice_slot_count': len(OPERATOR_VOICE_SLOTS),
        'operator_voice_slots_available': sum(
            bool(slot.get('available')) for slot in voice_slots),
        'voice_files': sum(name.startswith('sdrs/') for name in files),
        'distribution': {'scope': 'client-content', 'bundle_with_manager': False},
    }
    return source, files, report


def inspect_source(client, source_operator_id):
    """Return a JSON-safe dependency report without writing to the client."""
    return _collect(client, source_operator_id)[2]


def extract_pack(client, source_operator_id, output, *, target_template,
                 pack_id, name=None, locale='zh-HK', abilities=None,
                 acquisition=None, storefront=None,
                 allow_source_fallbacks=False):
    """Write one complete protocol pack to an explicit, new output file."""
    if type(target_template) is not int or not 60001 <= target_template <= 60999:
        raise ValueError('目标原生副官模板编号无效')
    source, files, report = _collect(client, source_operator_id)
    if type(allow_source_fallbacks) is not bool:
        raise ValueError('来源缺失资源策略无效')
    if report['missing_dependencies'] and not allow_source_fallbacks:
        raise ValueError('来源副官资源不完整：' + '、'.join(report['missing_dependencies'][:8]))
    if report['invalid_dependencies']:
        raise ValueError('来源副官资源格式不支持：'
                         + '、'.join(list(report['invalid_dependencies'])[:8]))
    if not report['package_profile']['dialogue']:
        raise ValueError('来源副官缺少可打包的台词表')
    target = Path(output)
    if target.exists():
        raise ValueError('副官分享包输出已存在')
    if not target.parent.is_dir():
        raise ValueError('副官分享包输出目录不存在')
    source_root = source.root.resolve()
    resolved_target = target.resolve()
    if resolved_target.is_relative_to(source_root):
        raise ValueError('副官分享包不能写入来源客户端')
    raw = legacy_pack.package(
        files,
        report['package_profile'],
        pack_id=pack_id,
        name=name or report['source_name'],
        source_template=target_template,
        locale=locale,
        abilities=abilities,
        acquisition=acquisition,
        storefront=storefront,
        resources_metadata={
            'source_label':report['source_name'],
            'source_index_sha256':report['source_index_sha256'],
            'source_operator_id':source_operator_id,
            'source_operator_record_size':report['operator_record_size'],
            'source_missing_dependencies':report['missing_dependencies'],
            'unavailable_profile_roles':report['unavailable_profile_roles'],
            'voice_policy':'provided_only',
        },
    )
    with target.open('xb') as handle:
        handle.write(raw)
    return {
        **report,
        'output': {
            'bytes': len(raw),
            'sha256': hashlib.sha256(raw).hexdigest(),
        },
        'target_template': target_template,
    }
