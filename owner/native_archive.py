"""Encode provider-scoped resources using the shipped ZDX/ZPK file format."""
from __future__ import annotations

import gzip
import hashlib
from pathlib import Path
import re
import struct
from typing import Callable


MAX_PARTS = 32
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


def part_paths(archive_path, parts: int) -> list[Path]:
    """Part 0 is the archive itself; part k is `resources.<k>.zpk` beside it."""
    archive_path=Path(archive_path)
    return [archive_path]+[archive_path.with_name(f'{archive_path.stem}.{k}{archive_path.suffix}') for k in range(1,parts)]


class _Parts:
    """Read chunks by global number from a single or split archive."""
    def __init__(self, paths, part_chunks, chunk_count):
        self.files=[];self.part_chunks=part_chunks or chunk_count
        try:
            for path in paths:self.files.append(open(path,'rb'))
            sizes=[]
            for file in self.files:
                file.seek(0,2);sizes.append(file.tell())
            full=self.part_chunks*(CHUNK_SIZE+8)
            if len(paths)==1:
                if sizes[0]!=chunk_count*(CHUNK_SIZE+8):raise ValueError('Native overlay archive size differs from its index')
            elif (any(size!=full for size in sizes[:-1]) or not 0<sizes[-1]<=full or sizes[-1]%(CHUNK_SIZE+8)
                  or (len(paths)-1)*self.part_chunks+sizes[-1]//(CHUNK_SIZE+8)!=chunk_count):
                raise ValueError('Native overlay archive parts differ from their index')
        except BaseException:
            self.close();raise

    def read(self, start, chunks):
        part=start//self.part_chunks
        if part!=(start+chunks-1)//self.part_chunks or part>=len(self.files):
            raise ValueError('Native overlay entry crosses an archive part')
        file=self.files[part];file.seek((start-part*self.part_chunks)*(CHUNK_SIZE+8))
        return b''.join(file.read(CHUNK_SIZE+8)[8:] for _ in range(chunks))

    def close(self):
        for file in self.files:file.close()


def _parse_index(index: bytes) -> tuple[int, dict[str, tuple[int, int, int]]]:
    if len(index)<24 or len(index)>MAX_INDEX_BYTES:
        raise ValueError('Native overlay index size is unsupported')
    version,chunk_size,chunk_count,_,count,_=struct.unpack_from('<6I',index,0)
    if version!=1 or chunk_size!=CHUNK_SIZE or count>MAX_ENTRIES:
        raise ValueError('Native overlay index header is unsupported')
    cursor=24;entries={}
    for _ in range(count):
        name=[]
        while True:
            if cursor+2>len(index):raise ValueError('Truncated native overlay index')
            character=struct.unpack_from('<H',index,cursor)[0]^0xAA;cursor+=2
            if character==0:break
            name.append(chr(character))
        if cursor+36>len(index):raise ValueError('Truncated native overlay index')
        zsize,start,chunks=struct.unpack_from('<III',index,cursor);cursor+=36
        path=''.join(name).replace('\\','/').lower()
        if path in entries or start+chunks>chunk_count or chunks*CHUNK_SIZE<zsize or zsize<12:
            raise ValueError('Native overlay entry is invalid: '+path)
        entries[path]=(zsize,start,chunks)
    if cursor!=len(index):raise ValueError('Native overlay index has trailing bytes')
    return chunk_count,entries


def _unpack(packed: bytes, path: str) -> bytes:
    if packed[:8]!=b'ZOAGZIP1':raise ValueError('Native overlay entry encoding is unsupported: '+path)
    raw=gzip.decompress(packed[12:])
    if len(raw)!=struct.unpack_from('<I',packed,8)[0]:raise ValueError('Native overlay entry size differs: '+path)
    return raw


class OverlayReader:
    """Read single entries of an existing overlay on demand; close() releases the archive files."""
    def __init__(self, index: bytes, archive_path, parts: int = 1, part_chunks: int = 0):
        if parts!=1 and (parts<2 or parts>MAX_PARTS or part_chunks<1):
            raise ValueError('Native overlay archive parts are invalid')
        chunk_count,self.entries=_parse_index(index)
        self._archive=_Parts(part_paths(archive_path,parts),part_chunks if parts>1 else 0,chunk_count)

    def names(self):
        return list(self.entries)

    def has(self, name: str) -> bool:
        return name.replace('\\','/').lower() in self.entries

    def read(self, name: str) -> bytes:
        path=name.replace('\\','/').lower()
        if path not in self.entries:raise ValueError('Native overlay resource is missing: '+path)
        zsize,start,chunks=self.entries[path]
        return _unpack(self._archive.read(start,chunks)[:zsize],path)

    def close(self):
        self._archive.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def read_archive(index: bytes, archive_path, parts: int = 1, part_chunks: int = 0) -> dict[str, bytes]:
    """Read every entry of an existing overlay (index bytes + archive file or parts) back to raw bytes."""
    with OverlayReader(index, archive_path, parts, part_chunks) as reader:
        return {path: reader.read(path) for path in reader.names()}


def build_archive_file(resources, allowed: Callable[[str], bool], archive_path, part_chunks: int = 0) -> tuple[bytes, dict]:
    """Stream a large overlay to disk: `resources` maps names to byte loaders.

    With `part_chunks`, the archive is split into files of at most that many chunks
    (`resources.zpk`, `resources.1.zpk`, ...) so each stays below a per-file size limit.
    Index chunk numbers stay global; a part that cannot fit the next entry is padded to
    exactly `part_chunks` chunks, so no entry crosses a part, and chunk links inside a
    part are part-relative. `archive_sha256` covers all parts as one stream.
    """
    if len(resources)>MAX_ENTRIES:
        raise ValueError('Native overlay exceeds 100000 resources')
    if part_chunks<0 or part_chunks*(CHUNK_SIZE+8)>MAX_ARCHIVE_BYTES:
        raise ValueError('Native overlay part size is unsupported')
    archive_path=Path(archive_path)
    for stale in archive_path.parent.glob(f'{archive_path.stem}.*{archive_path.suffix}'):
        if re.fullmatch(re.escape(archive_path.stem)+r'\.[0-9]+'+re.escape(archive_path.suffix),stale.name):stale.unlink()
    entries=[];seen=set();chunk_count=0;digest=hashlib.sha256();written=[archive_path];local=0
    output=open(archive_path,'wb')
    try:
        for supplied_name in sorted(resources):
            name=supplied_name.replace('\\','/').lower()
            _validate_name(name,seen,allowed)
            seen.add(name)
            loader=resources[supplied_name]
            raw=loader() if callable(loader) else loader
            packed=_pack(name,raw)
            count=(len(packed)+CHUNK_SIZE-1)//CHUNK_SIZE
            if part_chunks:
                if count>part_chunks:raise ValueError('Native overlay resource exceeds one archive part: '+name)
                if local+count>part_chunks:
                    padding=b'\0'*(CHUNK_SIZE+8)
                    for _ in range(part_chunks-local):
                        output.write(padding);digest.update(padding);chunk_count+=1
                    output.close()
                    if len(written)>=MAX_PARTS:raise ValueError('Native overlay needs more than 32 archive parts')
                    written.append(part_paths(archive_path,len(written)+1)[-1]);output=open(written[-1],'wb');local=0
            start=chunk_count
            if (start+count)*(CHUNK_SIZE+8)>MAX_ARCHIVE_BYTES:
                raise ValueError('Native overlay archive exceeds its size limit')
            for index in range(count):
                chunk=(struct.pack('<II',local+1,max(0,local-1))+
                       packed[index*CHUNK_SIZE:(index+1)*CHUNK_SIZE].ljust(CHUNK_SIZE,b'\0'))
                output.write(chunk);digest.update(chunk);chunk_count+=1;local+=1
            entries.append({'path':name,'zsize':len(packed),'start_chunk':start,'chunks':count,
                            'sha256':hashlib.sha256(raw).hexdigest()})
    finally:
        output.close()
    inventory={'chunk_size':CHUNK_SIZE,'entries':entries,'archive_sha256':digest.hexdigest(),
               'archive_bytes':chunk_count*(CHUNK_SIZE+8)}
    if len(written)>1:
        inventory.update(part_chunks=part_chunks,
                         parts=[{'name':path.name,'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()} for path in written])
    return _index(entries,chunk_count),inventory
