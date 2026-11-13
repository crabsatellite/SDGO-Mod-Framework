"""Prepare owner-scoped ZFX texture closures without changing shared resources.

Only typed emitter texture carriers are rewritten. Serialized bytes after a
carrier's first NUL never create an extra dependency. Mesh dependencies and
unrecognized layouts require a separate adapter; this module grants no native
record, script, server or actor admission.
"""
from __future__ import annotations

import hashlib
import io
import re
import struct

PROFILE = 'native-zfx-emitter-texture-v1'
MAX_EFFECT_BYTES = 16 * 1024 * 1024
MAX_TEXTURE_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024
MAX_EFFECTS = 128
MAX_EMITTERS = 4096
MAX_TEXTURE_PIXELS = 16_777_216
_LEAF = re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.+-]*\.txr', re.I)
FINAL_EMITTER_EOF_PROFILE = 'native-final-emitter-subdivision-eof-v1'


def _hash(raw):
    return hashlib.sha256(raw).hexdigest()


def _string(raw):
    end = raw.find(b'\0')
    if end < 0:
        raise ValueError('ZFX texture/model carrier is not NUL terminated')
    try:
        return raw[:end].decode('ascii')
    except UnicodeError as error:
        raise ValueError('unsupported ZFX texture/model name encoding') from error


def texture_carriers(raw):
    """Parse the complete native v1..5 emitter layout, never scan for paths.

    The first-NUL texture contract is established by the native ZOA LoadCommon,
    ZoaCharToWChar and SetTexture consumers. A complete parse proves this narrow
    dependency inventory, not effect appearance or gameplay equivalence.
    """
    if not isinstance(raw, bytes) or not 109 <= len(raw) <= MAX_EFFECT_BYTES:
        raise ValueError('ZFX byte extent exceeds the supported profile')
    if raw[:10] != b'ZOAEFFECT\0' or not 1 <= raw[11] <= 5:
        raise ValueError('unsupported ZFX magic/version')
    version = raw[11]

    def require(offset, size):
        if offset < 0 or size < 0 or offset + size > len(raw):
            raise ValueError('truncated ZFX field/vector')

    def word(offset):
        require(offset, 4)
        return struct.unpack_from('<I', raw, offset)[0]

    def vector(offset, stride):
        count = word(offset)
        offset += 4
        if count > (len(raw) - offset) // stride:
            raise ValueError('ZFX vector exceeds remaining byte extent')
        return offset + count * stride

    offset = vector(105, 8)
    count = word(offset)
    offset += 4
    if count > MAX_EMITTERS:
        raise ValueError('ZFX emitter count exceeds its budget')
    carriers, meshes = [], []
    for index in range(count):
        require(offset, 550)
        emitter_type = word(offset)
        field = offset + 4
        literal = _string(raw[field:field + 32])
        if literal and (not _LEAF.fullmatch(literal) or '..' in literal):
            raise ValueError('unsupported ZFX texture leaf')
        carriers.append({'emitter': index, 'emitter_type': emitter_type,
                         'offset': field, 'length': 32, 'literal': literal,
                         'texture': 'txrs/' + literal.lower() if literal else None,
                         'source_hex': raw[field:field + 32].hex()})
        if emitter_type == 2:
            meshes.append(index)
        offset += 550
        for vector_index,stride in enumerate((8,16,8,4)):
            # The native v5 loader initializes these counters/flags before its
            # EOF reads. Only this exact final type-1 boundary is known; never
            # pad the source or infer omitted mesh trailers/partial integers.
            if (version==5 and index==count-1 and emitter_type==1 and not meshes and
                    vector_index==3 and offset==len(raw)):
                return {'profile':PROFILE,'tail_profile':FINAL_EMITTER_EOF_PROFILE,'version':version,
                        'textures':carriers,'models':[],'parsed_bytes':offset,
                        'native_defaults':{'final_subdivision_count':0,'version5_flag':0,
                                           'final_extra_count':0,'effect_tail_flags':[0,0]}}
            offset = vector(offset, stride)
        if version > 4:
            require(offset, 1)
            offset = vector(offset + 1, 16)
    if version > 1:
        require(offset, 1)
        offset += 1
    model_carriers = []
    if version > 2:
        for index in meshes:
            require(offset, 35)
            model_carriers.append({'emitter': index, 'offset': offset + 3,'flags':list(raw[offset:offset+3]),
                                   'literal': _string(raw[offset + 3:offset + 35])})
            offset += 35
    if version > 3:
        require(offset, 1)
        offset += 1
    if offset != len(raw):
        raise ValueError('ZFX has unparsed trailing bytes')
    return {'profile': PROFILE, 'version': version, 'textures': carriers,
            'models': model_carriers, 'parsed_bytes': offset}


def inspect_texture(raw):
    """Inspect effect TXRs, including the original JPEG variant, without recoding."""
    from PIL import Image
    if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_TEXTURE_BYTES:
        raise ValueError('effect texture byte budget')
    payload = raw
    if raw.startswith(b'ZOATEX1\0'):
        if len(raw) < 12:
            raise ValueError('truncated effect texture wrapper')
        payload = raw[12:]
    with Image.open(io.BytesIO(payload)) as image:
        if image.format not in {'PNG', 'DDS', 'TGA', 'JPEG'} or getattr(image, 'n_frames', 1) != 1:
            raise ValueError('unsupported effect texture format')
        width, height = image.size
        if not 0 < width <= 8192 or not 0 < height <= 8192 or width * height > MAX_TEXTURE_PIXELS:
            raise ValueError('effect texture pixel budget')
        image.load()
        return {'format': image.format, 'width': width, 'height': height,
                'sha256': _hash(raw), 'bytes': len(raw)}


def scope_textures(owner, effects, read_texture, destination_read):
    """Return portable files/receipt for a specific, explicitly selected variant.

    ``read_texture`` must select the source variant, rather than search overlays
    for any matching filename. ``destination_read`` independently checks existing
    paths and returns None for absent resources. It cannot authorize overwrites.
    The caller still owns project import, publication and native admission.
    """
    if not isinstance(owner, str) or not re.fullmatch('[a-z][a-z0-9_]{0,63}', owner):
        raise ValueError('invalid effect resource owner')
    if not isinstance(effects, dict) or not 1 <= len(effects) <= MAX_EFFECTS:
        raise ValueError('effect resource count budget')
    namespace = _hash(owner.encode('ascii'))[:8]
    files, textures, bindings = {}, {}, []
    total = 0

    def publish(path, data):
        nonlocal total
        old = files.get(path)
        if old is not None and old != data:
            raise ValueError('scoped effect resource alias collision')
        if old is None:
            existing = destination_read(path)
            if existing is not None and existing != data:
                raise ValueError('scoped effect resource would overwrite existing bytes')
            if total + len(data) > MAX_TOTAL_BYTES:
                raise ValueError('effect closure total byte budget')
            files[path] = data
            total += len(data)

    for source, raw in sorted(effects.items()):
        if not isinstance(source, str) or not re.fullmatch(r'fxrs/[a-z0-9_][a-z0-9_.-]*\.zfx', source) or '..' in source:
            raise ValueError('non-canonical source effect path')
        parsed = texture_carriers(raw)
        if any(row['literal'] for row in parsed['models']):
            raise ValueError('ZFX model dependency needs a separate scoped model adapter')
        patched = bytearray(raw)
        edges = []
        for carrier in parsed['textures']:
            texture = carrier['texture']
            if texture is None:
                continue
            if texture not in textures:
                data = read_texture(texture)
                details = inspect_texture(data)
                leaf = 'o' + namespace + details['sha256'][:16] + '.txr'
                target = 'txrs/' + leaf
                publish(target, data)
                textures[texture] = {'source': texture, 'target': target, **details}
            binding = textures[texture]
            encoded = (binding['target'].split('/')[1] + '\0').encode('ascii')
            if len(encoded) > carrier['length']:
                raise ValueError('scoped texture name exceeds native carrier')
            start = carrier['offset']
            # Keep all storage after the new NUL. It is not another path and
            # must not be scanned, normalized, or silently zeroed.
            patched[start:start + len(encoded)] = encoded
            edges.append({**carrier, 'target': binding['target'],
                          'target_sha256': binding['sha256']})
        output = bytes(patched)
        target = 'fxrs/o' + namespace + _hash(output)[:11] + '.zfx'
        publish(target, output)
        reparsed = texture_carriers(output)
        expected = [textures[row['texture']]['target'] if row['texture'] else None
                    for row in parsed['textures']]
        if [row['texture'] for row in reparsed['textures']] != expected:
            raise ValueError('scoped ZFX dependency readback differs')
        bindings.append({'source': source, 'source_sha256': _hash(raw),
                         'target': target, 'target_sha256': _hash(output),
                         'bytes': len(raw), 'texture_edges': edges})
    return {'files': files, 'manifest': {'schema': 'sdgo.effect-texture-namespace/1',
            'owner': owner, 'effects': bindings, 'textures': list(textures.values()),
            'runtime_registration_authorized': False, 'gameplay_accepted': False}}
