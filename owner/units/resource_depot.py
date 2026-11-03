"""Bounded, hash-pinned authoring resource transport; no runtime activation.

Metadata is paged. ZIP bytes are verified into private temporary files before
access, and requested blobs are streamed into a second verified temporary file.
Only two compressed chunks and four metadata pages are retained per reader.
The reader never extracts paths, applies SQL, registers IDs or executes programs.
"""
from __future__ import annotations
from collections import OrderedDict
from contextlib import contextmanager
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import zipfile

INDEX_SCHEMA='sdgo:authoring-resource-depot-index/1'
PAGE_SCHEMA='sdgo:authoring-resource-depot-page/1'
INDEX_LIMIT=64*1024
PAGE_LIMIT=1024*1024
LINE_LIMIT=64*1024
CHUNK_LIMIT=64*1024*1024
RAW_LIMIT=48*1024*1024
FILE_LIMIT=4096
BLOCK=1024*1024
SECTION_KEYS={'blobs':'sha256','logicalBindings':'logical_path','families':'sourceRootUnitId',
    'logicalPathConflicts':'logical_path','opaqueSourceGamePrograms':'logical_path'}

class DepotError(ValueError):pass

def _integer(value,minimum,maximum,label):
    if type(value) is not int or not minimum<=value<=maximum:raise DepotError(label+' is outside its integer budget')
    return value

def _hash(value):
    if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{64}',value):raise DepotError('invalid SHA-256')
    return value

def _pairs(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise DepotError('duplicate JSON key')
        result[key]=value
    return result

def _json(raw):
    try:
        def constant(_):raise DepotError('non-finite JSON value')
        return json.loads(raw.decode('utf-8'),object_pairs_hook=_pairs,parse_constant=constant)
    except (UnicodeError,json.JSONDecodeError,RecursionError) as error:raise DepotError('invalid bounded JSON') from error

def _relative(name):
    if not isinstance(name,str) or not 1<=len(name)<=240 or not re.fullmatch('[A-Za-z0-9_./-]+',name):
        raise DepotError('invalid relative resource path')
    path=PurePosixPath(name)
    if path.is_absolute() or path.as_posix()!=name or any(p in ('','.','..') or p.endswith('.') or
        re.fullmatch(r'(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\..*)?',p,re.I) for p in name.split('/')):
        raise DepotError('non-canonical resource path')
    return name

def _linked(path):return path.is_symlink() or bool(getattr(path,'is_junction',lambda:False)())

def _copy(source,target,expected_bytes,expected_hash):
    total=0;digest=hashlib.sha256()
    while True:
        block=source.read(min(BLOCK,expected_bytes-total+1))
        if not block:break
        total+=len(block)
        if total>expected_bytes:raise DepotError('resource exceeds its declared size')
        digest.update(block);target.write(block)
    if total!=expected_bytes or digest.hexdigest()!=expected_hash:raise DepotError('resource size or SHA-256 mismatch')
    target.seek(0)

class ResourceDepot:
    def __init__(self,root,*,index_sha256):
        self.root=Path(root).absolute();self.index_sha256=_hash(index_sha256)
        for part in (self.root,*self.root.parents):
            if _linked(part):raise DepotError('linked resource depot root')
        self.root=self.root.resolve();self._pages=OrderedDict();self._chunks=OrderedDict();self._closed=False
        raw=self._read('index.json',INDEX_LIMIT,self.index_sha256)
        index=_json(raw)
        if not isinstance(index,dict) or index.get('schema')!=INDEX_SCHEMA:raise DepotError('unsupported resource depot index')
        if index.get('authoringOnly') is not True:raise DepotError('resource depot must remain authoring-only')
        for flag in ('idsReserved','nativeActivationAuthorityIssued','nativeGameplayAccepted',
                     'nativeInstallerOrBundleSetConsumerAccepted','sqlAuthorityIssued','containsSqlOrOperationalScripts'):
            if index.get(flag) is not False:raise DepotError('resource transport cannot carry activation authority')
        self.revision=_hash(index.get('revision'))
        limits=index.get('limits')
        if limits!={'compressedBytesPerChunk':CHUNK_LIMIT,'rawBytesPerChunk':RAW_LIMIT,'filesPerChunk':FILE_LIMIT}:
            raise DepotError('unsupported resource chunk limits')
        if index.get('metadataPageByteLimit')!=PAGE_LIMIT:raise DepotError('unsupported metadata page budget')
        chunks=index.get('chunks');pages=index.get('metadataPages');counts=index.get('sectionCounts')
        if not isinstance(chunks,list) or not 1<=len(chunks)<=128 or not isinstance(pages,list) or not 1<=len(pages)<=128:
            raise DepotError('resource depot index count exceeds its budget')
        if not isinstance(counts,dict) or set(counts)!=set(SECTION_KEYS):raise DepotError('resource metadata section inventory differs')
        for count in counts.values():_integer(count,0,1000000,'section count')
        policy=index.get('programActivationPolicy')
        # A material-only addendum may omit the program section. Absence never
        # grants execution, and transported program declarations need the full
        # explicit deny-by-default policy.
        if policy is not None or counts['opaqueSourceGamePrograms']:
            if not isinstance(policy,dict) or policy.get('activationAllowed') is not False or policy.get('originalLuaJitDirectExecutionAllowed') is not False or policy.get('registrationRequired') is not True:
                raise DepotError('source programs need a separate registration authority')
        self.chunk_index={};self.page_index={key:[] for key in SECTION_KEYS};paths=set()
        for chunk in chunks:
            if not isinstance(chunk,dict):raise DepotError('invalid chunk declaration')
            ordinal=_integer(chunk.get('ordinal'),1,128,'chunk ordinal')
            path=_relative(chunk.get('path'));_hash(chunk.get('sha256'))
            if ordinal in self.chunk_index or path.casefold() in paths:raise DepotError('duplicate chunk identity')
            _integer(chunk.get('bytes'),1,CHUNK_LIMIT,'compressed chunk size')
            _integer(chunk.get('rawBytes'),0,RAW_LIMIT,'raw chunk size')
            _integer(chunk.get('files'),1,FILE_LIMIT,'chunk file count')
            self.chunk_index[ordinal]=copy.deepcopy(chunk);paths.add(path.casefold())
        if set(self.chunk_index)!=set(range(1,len(chunks)+1)):raise DepotError('chunk sequence is incomplete')
        for page in pages:
            if not isinstance(page,dict):raise DepotError('invalid metadata page')
            section=page.get('section');selector=page.get('selector')
            if section not in SECTION_KEYS or page.get('format')!='jsonl-header-and-rows/1' or not isinstance(selector,dict) or set(selector)!={'keyField','first','last'} or selector['keyField']!=SECTION_KEYS[section]:
                raise DepotError('unsupported metadata selector')
            _integer(page.get('ordinal'),0,127,'page ordinal');_integer(page.get('bytes'),1,PAGE_LIMIT,'page size')
            _integer(page.get('rows'),0,100000,'page rows');_integer(page.get('maxLineBytes'),1,LINE_LIMIT,'line size')
            _hash(page.get('sha256'));path=_relative(page.get('path'))
            if path.casefold() in paths:raise DepotError('duplicate metadata path')
            paths.add(path.casefold())
            first,last=selector['first'],selector['last']
            if section=='families':_integer(first,1,9999999,'first family');_integer(last,1,9999999,'last family')
            elif not isinstance(first,str) or not isinstance(last,str) or len(first)>240 or len(last)>240:raise DepotError('invalid string selector')
            if first>last:raise DepotError('inverted metadata range')
            self.page_index[section].append(copy.deepcopy(page))
        for section,entries in self.page_index.items():
            entries.sort(key=lambda row:row['ordinal'])
            if [p['ordinal'] for p in entries]!=list(range(len(entries))) or sum(p['rows'] for p in entries)!=counts[section]:
                raise DepotError('metadata page counts or ordinal sequence differ')
            if any(a['selector']['last']>=b['selector']['first'] for a,b in zip(entries,entries[1:])):
                raise DepotError('overlapping metadata selectors')
        self._index=index

    def _path(self,name):
        path=self.root
        for part in _relative(name).split('/'):
            path=path/part
            if _linked(path):raise DepotError('linked resource depot member')
        if not path.resolve().is_relative_to(self.root) or not path.is_file():raise DepotError('missing or escaped resource depot member')
        return path

    def _read(self,name,maximum,digest):
        path=self._path(name)
        if path.stat().st_size>maximum:raise DepotError('resource metadata exceeds its byte budget')
        with path.open('rb') as stream:raw=stream.read(maximum+1)
        if len(raw)>maximum or hashlib.sha256(raw).hexdigest()!=digest:raise DepotError('resource metadata SHA-256 mismatch')
        return raw

    def _check(self):
        if self._closed:raise DepotError('resource depot reader is closed')
        self._read('index.json',INDEX_LIMIT,self.index_sha256)

    @property
    def index(self):return copy.deepcopy(self._index)

    def _page(self,page):
        name=page['path']
        if name in self._pages:
            self._pages.move_to_end(name);return self._pages[name]
        raw=self._read(name,PAGE_LIMIT,page['sha256'])
        if len(raw)!=page['bytes']:raise DepotError('metadata page byte count differs')
        lines=raw.splitlines(keepends=True)
        if not lines or any(len(line)>page['maxLineBytes'] for line in lines):raise DepotError('metadata line exceeds its declared budget')
        header=_json(lines[0]);expected={'schema':PAGE_SCHEMA,'revision':self.revision,'section':page['section'],'ordinal':page['ordinal']}
        if header!=expected:raise DepotError('metadata header identity differs')
        records=[_json(line) for line in lines[1:]];field=page['selector']['keyField']
        if len(records)!=page['rows'] or any(not isinstance(r,dict) or field not in r for r in records):raise DepotError('metadata row inventory differs')
        keys=[r[field] for r in records]
        if any(type(k) is not type(page['selector']['first']) for k in keys) or any(a>=b for a,b in zip(keys,keys[1:])):
            raise DepotError('metadata keys are not strictly ordered')
        if keys and (keys[0]!=page['selector']['first'] or keys[-1]!=page['selector']['last']):raise DepotError('metadata selector does not match actual rows')
        self._pages[name]=records
        while len(self._pages)>4:self._pages.popitem(last=False)
        return records

    def lookup(self,section,key):
        self._check()
        if section not in SECTION_KEYS:raise DepotError('unknown metadata section')
        for page in self.page_index[section]:
            bounds=page['selector']
            if type(key) is not type(bounds['first']):raise DepotError('metadata lookup key type differs')
            if bounds['first']<=key<=bounds['last']:
                for row in self._page(page):
                    if row[bounds['keyField']]==key:return copy.deepcopy(row)
                return None
        return None

    def records(self,section):
        self._check()
        if section not in SECTION_KEYS:raise DepotError('unknown metadata section')
        for page in self.page_index[section]:
            for row in self._page(page):yield copy.deepcopy(row)

    def _chunk(self,ordinal):
        if ordinal in self._chunks:
            self._chunks.move_to_end(ordinal);return self._chunks[ordinal][1]
        chunk=self.chunk_index.get(ordinal)
        if chunk is None:raise DepotError('unknown resource chunk')
        path=self._path(chunk['path'])
        if path.stat().st_size!=chunk['bytes']:raise DepotError('compressed resource size differs')
        temporary=tempfile.TemporaryFile();archive=None
        try:
            with path.open('rb') as source:_copy(source,temporary,chunk['bytes'],chunk['sha256'])
            archive=zipfile.ZipFile(temporary);entries=archive.infolist()
            if len(entries)!=chunk['files']:raise DepotError('resource ZIP file count differs')
            seen=set();total=0
            for entry in entries:
                if not re.fullmatch(r'blobs/[0-9a-f]{64}\.bin',entry.filename) or entry.filename in seen or entry.is_dir() or stat.S_ISLNK(entry.external_attr>>16) or entry.flag_bits&1 or entry.compress_type not in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED):
                    raise DepotError('unsafe or duplicate resource ZIP member')
                _integer(entry.file_size,0,RAW_LIMIT,'resource member size');seen.add(entry.filename);total+=entry.file_size
            if total!=chunk['rawBytes'] or total>RAW_LIMIT:raise DepotError('raw resource ZIP size differs')
        except BaseException:
            if archive is not None:archive.close()
            temporary.close();raise
        self._chunks[ordinal]=(temporary,archive)
        while len(self._chunks)>2:
            _,(old,old_archive)=self._chunks.popitem(last=False);old_archive.close();old.close()
        return archive

    @contextmanager
    def open_blob(self,digest):
        digest=_hash(digest);row=self.lookup('blobs',digest)
        if row is None:raise DepotError('resource blob is not transported in this depot')
        size=_integer(row.get('bytes'),0,RAW_LIMIT,'blob size');ordinal=_integer(row.get('chunk'),1,128,'blob chunk')
        name='blobs/'+digest+'.bin'
        if row.get('entry')!=name:raise DepotError('blob identity and ZIP entry differ')
        archive=self._chunk(ordinal)
        try:entry=archive.getinfo(name)
        except KeyError as error:raise DepotError('declared blob is missing from its resource chunk') from error
        if entry.file_size!=size:raise DepotError('blob metadata size differs')
        with tempfile.TemporaryFile() as verified:
            try:
                with archive.open(entry) as source:_copy(source,verified,size,digest)
            except (zipfile.BadZipFile,RuntimeError,OSError) as error:raise DepotError('resource blob could not be authenticated') from error
            yield verified

    def close(self):
        for stream,archive in self._chunks.values():archive.close();stream.close()
        self._chunks.clear();self._pages.clear();self._closed=True
    def __enter__(self):return self
    def __exit__(self,*_):self.close()
