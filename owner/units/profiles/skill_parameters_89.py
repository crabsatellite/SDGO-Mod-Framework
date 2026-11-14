"""The supported host's primitive kind-79 skill-parameter record layout.

Record validation does not allocate an ID, authorize installation, resolve a
skill's other dependencies, or establish its gameplay effect.
"""
from __future__ import annotations
import hashlib
import math
import struct

PROFILE_ID='native-type79-89-v1'
RECORD_KIND=79
RECORD_SIZE=89
SKILL_REFERENCE_OFFSET=284
RATE_OFFSETS=(65,69,73,77,81,85)


def validate_record(raw,identity=None):
    if not isinstance(raw,bytes) or len(raw)!=RECORD_SIZE:
        raise ValueError('skill parameter record requires exactly 89 bytes')
    key=struct.unpack_from('<I',raw)[0]
    if not 1<=key<=9_999_999 or raw[4] or (identity is not None and (type(identity)is not int or key!=identity)):
        raise ValueError('skill parameter record identity/security differs')
    if any(not math.isfinite(struct.unpack_from('<f',raw,offset)[0])for offset in RATE_OFFSETS):
        raise ValueError('skill parameter record contains a non-finite rate')
    return {'kind':RECORD_KIND,'id':key,'profile':PROFILE_ID,'bytes':RECORD_SIZE,
            'sha256':hashlib.sha256(raw).hexdigest(),'native_actor_accepted':False}
