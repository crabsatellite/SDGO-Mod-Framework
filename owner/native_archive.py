"""Encode provider-scoped resources using the shipped ZDX/ZPK file format."""
from __future__ import annotations

import gzip
import hashlib
import re
import struct
from typing import Callable


def build_archive(resources: dict[str, bytes], allowed: Callable[[str], bool]) -> tuple[bytes, bytes, dict]:
    """Keep format encoding shared; each provider supplies its own allowlist."""
    if len(resources)>100_000:
        raise ValueError('Native overlay exceeds 100000 resources')
    chunk_size=512;entries=[];chunks=[];seen=set()
    for supplied_name,raw in sorted(resources.items()):
        name=supplied_name.replace('\\','/').lower()
        if (len(name)>240 or not re.fullmatch(r'[a-z0-9][a-z0-9_.-]*(?:/[a-z0-9][a-z0-9_.-]*)+',name)
                or any(part in ('.','..') or part.endswith('.') for part in name.split('/'))
                or name in seen or not allowed(name)):
            raise ValueError('Resource is outside the provider allowlist: '+name)
        if not isinstance(raw,bytes) or len(raw)>64*1024*1024:
            raise ValueError('Native overlay resource size is unsupported: '+name)
        seen.add(name)
        packed=b'ZOAGZIP1'+struct.pack('<I',len(raw))+gzip.compress(raw,mtime=0)
        if len(packed)>64*1024*1024:
            raise ValueError('Native overlay stored resource exceeds 64 MiB: '+name)
        start=len(chunks);count=(len(packed)+chunk_size-1)//chunk_size
        if (start+count)*(chunk_size+8)>512*1024*1024:
            raise ValueError('Native overlay archive exceeds 512 MiB')
        for index in range(count):
            cursor=len(chunks)
            chunks.append(struct.pack('<II',cursor+1,max(0,cursor-1))+
                packed[index*chunk_size:(index+1)*chunk_size].ljust(chunk_size,b'\0'))
        entries.append({'path':name,'zsize':len(packed),'start_chunk':start,'chunks':count,
                        'sha256':hashlib.sha256(raw).hexdigest()})
    zdx=bytearray(struct.pack('<6I',1,chunk_size,len(chunks),0,len(entries),0))
    for entry in entries:
        for character in entry['path'].replace('/','\\')+'\0':
            zdx.extend(struct.pack('<H',ord(character)^0xAA))
        zdx.extend(struct.pack('<III',entry['zsize'],entry['start_chunk'],entry['chunks'])+bytes(24))
    if len(zdx)>64*1024*1024:raise ValueError('Native overlay index exceeds 64 MiB')
    return bytes(zdx),b''.join(chunks),{'chunk_size':chunk_size,'entries':entries}
