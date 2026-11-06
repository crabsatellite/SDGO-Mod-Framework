"""Encode provider-scoped resources using the shipped ZDX/ZPK file format."""
from __future__ import annotations

import gzip
import hashlib
import re
import struct
from typing import Callable


MAX_ENTRIES = 100_000
MAX_RESOURCE_BYTES = 64 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_INDEX_BYTES = 64 * 1024 * 1024
CHUNK_SIZE = 512


def _validate_name(name: str, seen: set, allowed: Callable[[str], bool]) -> None:
    if (len(name)>240 or not re.fullmatch(r'[a-z0-9][a-z0-9_.-]*(?:/[a-z0-9][a-z0-9_.-]*)+',name)
            or any(part in ('.','..') or part.endswith('.') for part in name.split('/'))
            or name in seen or not allowed(name)):
        raise ValueError('Resource is outside the provider allowlist: '+name)


def _pack(name: str, raw: bytes) -> bytes:
    if not isinstance(raw,bytes) or len(raw)>MAX_RESOURCE_BYTES:
        raise ValueError('Native overlay resource size is unsupported: '+name)
    packed=b'ZOAGZIP1'+struct.pack('<I',len(raw))+gzip.compress(raw,mtime=0)
    if len(packed)>MAX_RESOURCE_BYTES:
        raise ValueError('Native overlay stored resource exceeds 64 MiB: '+name)
    return packed


def _index(entries: list, chunk_count: int) -> bytes:
    zdx=bytearray(struct.pack('<6I',1,CHUNK_SIZE,chunk_count,0,len(entries),0))
    for entry in entries:
        for character in entry['path'].replace('/','\\')+'\0':
            zdx.extend(struct.pack('<H',ord(character)^0xAA))
        zdx.extend(struct.pack('<III',entry['zsize'],entry['start_chunk'],entry['chunks'])+bytes(24))
    if len(zdx)>MAX_INDEX_BYTES:raise ValueError('Native overlay index exceeds 64 MiB')
    return bytes(zdx)


def build_archive(resources: dict[str, bytes], allowed: Callable[[str], bool]) -> tuple[bytes, bytes, dict]:
    """Keep format encoding shared; each provider supplies its own allowlist."""
    if len(resources)>MAX_ENTRIES:
        raise ValueError('Native overlay exceeds 100000 resources')
    entries=[];chunks=[];seen=set()
    for supplied_name,raw in sorted(resources.items()):
        name=supplied_name.replace('\\','/').lower()
        _validate_name(name,seen,allowed)
        seen.add(name)
        packed=_pack(name,raw)
        start=len(chunks);count=(len(packed)+CHUNK_SIZE-1)//CHUNK_SIZE
        if (start+count)*(CHUNK_SIZE+8)>MAX_ARCHIVE_BYTES:
            raise ValueError('Native overlay archive exceeds its size limit')
        for index in range(count):
            cursor=len(chunks)
            chunks.append(struct.pack('<II',cursor+1,max(0,cursor-1))+
                packed[index*CHUNK_SIZE:(index+1)*CHUNK_SIZE].ljust(CHUNK_SIZE,b'\0'))
        entries.append({'path':name,'zsize':len(packed),'start_chunk':start,'chunks':count,
                        'sha256':hashlib.sha256(raw).hexdigest()})
    return _index(entries,len(chunks)),b''.join(chunks),{'chunk_size':CHUNK_SIZE,'entries':entries}


def build_archive_file(resources, allowed: Callable[[str], bool], archive_path) -> tuple[bytes, dict]:
    """Stream a large overlay to disk: `resources` maps names to byte loaders."""
    if len(resources)>MAX_ENTRIES:
        raise ValueError('Native overlay exceeds 100000 resources')
    entries=[];seen=set();chunk_count=0;digest=hashlib.sha256()
    with open(archive_path,'wb') as output:
        for supplied_name in sorted(resources):
            name=supplied_name.replace('\\','/').lower()
            _validate_name(name,seen,allowed)
            seen.add(name)
            loader=resources[supplied_name]
            raw=loader() if callable(loader) else loader
            packed=_pack(name,raw)
            start=chunk_count;count=(len(packed)+CHUNK_SIZE-1)//CHUNK_SIZE
            if (start+count)*(CHUNK_SIZE+8)>MAX_ARCHIVE_BYTES:
                raise ValueError('Native overlay archive exceeds its size limit')
            for index in range(count):
                cursor=chunk_count
                chunk=(struct.pack('<II',cursor+1,max(0,cursor-1))+
                       packed[index*CHUNK_SIZE:(index+1)*CHUNK_SIZE].ljust(CHUNK_SIZE,b'\0'))
                output.write(chunk);digest.update(chunk);chunk_count+=1
            entries.append({'path':name,'zsize':len(packed),'start_chunk':start,'chunks':count,
                            'sha256':hashlib.sha256(raw).hexdigest()})
    return _index(entries,chunk_count),{'chunk_size':CHUNK_SIZE,'entries':entries,'archive_sha256':digest.hexdigest(),
                                        'archive_bytes':chunk_count*(CHUNK_SIZE+8)}
