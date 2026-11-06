"""Preserve the unit record's content-bound model integrity carrier.

The stored text is four little-endian MD5 words, each written as eight upper
case hex characters. This derivation does not establish native playability.
"""
from __future__ import annotations

import hashlib
import struct

from .codec import UNIT_SIZE

OFFSET = 672
EXTENT = 64


def model_digest(model: bytes) -> bytes:
    if (not isinstance(model, bytes) or not 16 <= len(model) <= 64 * 1024 * 1024
            or model[4:8] != b'ZOA\0'):
        raise ValueError('model integrity requires a complete supported ZOA model')
    digest = hashlib.md5(model, usedforsecurity=False).digest()
    return ''.join(f'{word:08X}' for word in struct.unpack('<4I', digest)).encode('utf-16-le')


def bind_model_digest(record: bytes, prepared_model: bytes) -> bytes:
    """Compile the carrier from the authenticated final resource bytes."""
    if (not isinstance(record, bytes) or len(record) != UNIT_SIZE or record[4] != 0
            or not 1 <= struct.unpack_from('<I', record)[0] <= 9_999_999):
        raise ValueError('model integrity requires a complete native unit record')
    prepared = model_digest(prepared_model)
    return record[:OFFSET] + prepared + record[OFFSET + EXTENT:]


def rebind_model_digest(record: bytes, source_model: bytes, prepared_model: bytes) -> bytes:
    result = bind_model_digest(record, prepared_model)
    if record[OFFSET:OFFSET + EXTENT] != model_digest(source_model):
        raise ValueError('unit model integrity differs from the authenticated source model')
    return result
