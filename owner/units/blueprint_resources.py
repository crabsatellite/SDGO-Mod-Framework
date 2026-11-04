"""Bounded prepared-blueprint SPR/TXR closure, independent of native admission.

The second SPR string and unassigned frame fields remain opaque evidence.
Only the native first texture carrier creates a dependency edge. No resource
is rewritten, installed, registered, or executed by this inspector.
"""
from __future__ import annotations

import hashlib
import io
import math
from collections.abc import Mapping
import re
import struct

from .assets import _resource_path

PROFILE = 'native-blueprint-spr-txr-v1'
MAX_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 128 * 1024 * 1024
MAX_RESOURCES = 128
MAX_TEXTURE_PIXELS = 16_777_216
MAX_TOTAL_PIXELS = 67_108_864


def _hash(raw):
    return hashlib.sha256(raw).hexdigest()


def sprite_frames(raw):
    """Read exact native 10000/11000 records without scanning opaque strings."""
    if not isinstance(raw, bytes) or len(raw) < 12:
        raise ValueError('blueprint SPR is truncated')
    version, count = struct.unpack_from('<II', raw)
    if version not in (10000, 11000) or not 1 <= count <= 1000:
        raise ValueError('unsupported blueprint SPR version or frame count')
    stride = 164 if version == 10000 else 172
    if len(raw) != 8 + count * stride + 4:
        raise ValueError('blueprint SPR record extent differs')
    frames = []
    for index in range(count):
        offset = 8 + index * stride
        duration, height, width = struct.unpack_from('<III', raw, offset)
        uv = struct.unpack_from('<4f', raw, offset + 12)
        if not 0 < width <= 8192 or not 0 < height <= 8192:
            raise ValueError('invalid blueprint SPR dimensions')
        if (not all(math.isfinite(value) for value in uv)
                or not (-.02 <= uv[0] < uv[2] <= 1.02
                        and -.02 <= uv[1] < uv[3] <= 1.02)):
            raise ValueError('invalid blueprint SPR UV')
        carrier = offset + 36
        color, opacity = 0xffffffff, 255
        if version == 11000:
            color, opacity = struct.unpack_from('<II', raw, carrier)
            carrier += 8
        if opacity > 255:
            raise ValueError('unsupported blueprint SPR opacity')
        field = raw[carrier:carrier + 64]
        terminator = next((i for i in range(0, 64, 2) if field[i:i + 2] == b'\0\0'), None)
        if terminator is None:
            raise ValueError('blueprint SPR texture carrier lacks a terminator')
        try:
            texture = _resource_path(field[:terminator].decode('utf-16-le'))
        except (UnicodeError, ValueError) as error:
            raise ValueError('invalid blueprint SPR texture path') from error
        if not texture.startswith('txrs/') or not texture.endswith('.txr'):
            raise ValueError('blueprint SPR first carrier must reference TXR')
        frames.append({'index':index,'record_offset':offset,'width':width,'height':height,
            'duration_source_updates':duration,'uv':list(uv),'uv_bits':raw[offset+12:offset+28].hex(),
            'opaque_frame_fields':raw[offset+28:offset+36].hex(),'color':color,'opacity':opacity,
            'texture':texture,'texture_carrier_offset':carrier,
            'annotation_raw':raw[carrier+64:carrier+128].hex()})
    return {'version':version,'frames':frames,'playback_bits':raw[-4:].hex(),
            'opaque_fields_preserved':True,'annotation_is_texture_dependency':False}


def _texture(raw):
    from PIL import Image, UnidentifiedImageError
    payload = raw
    wrapped = raw.startswith(b'ZOATEX1\0')
    if wrapped:
        if len(raw) < 12:
            raise ValueError('blueprint TXR wrapper is truncated')
        payload = raw[12:]
    try:
        with Image.open(io.BytesIO(payload)) as image:
            if image.format not in {'PNG','DDS','TGA'} or getattr(image,'n_frames',1) != 1:
                raise ValueError('unsupported blueprint texture profile')
            width, height = image.size
            if not 0 < width <= 8192 or not 0 < height <= 8192 or width*height > MAX_TEXTURE_PIXELS:
                raise ValueError('blueprint texture exceeds the pixel budget')
            image.load()
            rgba = image.convert('RGBA')
            pixels = rgba.tobytes()
            return {'format':image.format,'width':width,'height':height,'wrapped':wrapped,
                    'rgba_sha256':_hash(pixels),'rgba_bytes':len(pixels)}
    except (UnidentifiedImageError, OSError, SyntaxError, Image.DecompressionBombError) as error:
        raise ValueError('invalid or truncated blueprint texture') from error


def inspect_closure(roots, bindings, read):
    """Verify a hash-pinned prepared sprite set and every frame's exact texture.

    ``bindings`` maps logical runtime paths to member/sha256 declarations.
    ``read(member)`` returns already bounded package or depot bytes. Evidence
    resources which are absent from the prepared record roots are not runtime
    inputs and must not be included in this map.
    """
    if (not isinstance(roots,(list,tuple,set)) or any(not isinstance(p,str) for p in roots)
            or not isinstance(bindings,Mapping)):
        raise ValueError('invalid blueprint resource closure inventory')
    roots = sorted(set(roots))
    if not roots or len(bindings) > MAX_RESOURCES or not callable(read):
        raise ValueError('invalid blueprint resource closure inventory')
    normalized = {}
    for path, binding in bindings.items():
        if _resource_path(path) != path or path in normalized:
            raise ValueError('non-canonical blueprint resource closure path')
        if (not isinstance(binding,Mapping) or not isinstance(binding.get('member'),str)
                or not isinstance(binding.get('sha256'),str)
                or not re.fullmatch('[0-9a-f]{64}',binding['sha256'])):
            raise ValueError('invalid blueprint closure hash binding')
        normalized[path] = binding
    resources, edges, textures = {}, [], {}
    total_bytes = total_pixels = 0

    def content(path):
        nonlocal total_bytes
        if path not in normalized:
            raise ValueError('missing blueprint resource dependency: '+path)
        binding = normalized[path]
        raw = read(binding['member'])
        if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_BYTES or _hash(raw) != binding['sha256']:
            raise ValueError('blueprint resource dependency size or SHA-256 differs: '+path)
        if path not in resources:total_bytes += len(raw)
        if total_bytes > MAX_TOTAL_BYTES:
            raise ValueError('blueprint closure exceeds the resource byte budget')
        resources[path] = {'path':path,'member':binding['member'],'sha256':binding['sha256'],'bytes':len(raw)}
        return raw

    sprites = []
    for path in roots:
        if not path.startswith('sprs/') or not path.endswith('.spr'):
            raise ValueError('prepared blueprint root must be an SPR: '+path)
        parsed = sprite_frames(content(path))
        sprites.append({'path':path,**parsed})
        for frame in parsed['frames']:
            texture = frame['texture']
            edges.append({'sprite':path,'frame':frame['index'],'texture':texture,
                          'carrier_offset':frame['texture_carrier_offset']})
            if texture not in textures:
                decoded = _texture(content(texture))
                total_pixels += decoded['width']*decoded['height']
                if total_pixels > MAX_TOTAL_PIXELS:
                    raise ValueError('blueprint closure exceeds the decoded pixel budget')
                textures[texture] = {'path':texture,**decoded}
    extra = set(normalized)-set(resources)
    if extra:
        raise ValueError('unreferenced blueprint dependency resources: '+', '.join(sorted(extra)))
    return {'schema':'sdgo.blueprint-resource-closure/1','profile':PROFILE,'roots':roots,
            'resources':[resources[p] for p in sorted(resources)],'sprites':sprites,
            'textures':[textures[p] for p in sorted(textures)],'edges':edges,
            'total_resource_bytes':total_bytes,'total_decoded_pixels':total_pixels,
            'resource_closure_verified':True,'resource_bytes_changed':False,
            'native_registration_authorized':False,'native_runtime_accepted':False}
