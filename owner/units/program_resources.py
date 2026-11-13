"""Read-only target checks for literal resources of admitted native programs."""
from __future__ import annotations

import configparser
import hashlib
import json
import re
import stat
from pathlib import Path

from owner.native_archive import OverlayReader, part_paths, MAX_ARCHIVE_BYTES, MAX_INDEX_BYTES, CHUNK_SIZE
from . import native_programs


def validate_publication(publication, baseline=None):
    root=Path(publication.root).absolute()
    module=root/'package/client/sdgo/mods/unit_catalog'
    def confined(path):
        # Resolving first would hide linked parents, including Windows junctions.
        for current in (path,*path.parents):
            if current.exists() or current.is_symlink():
                info=current.lstat()
                if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:
                    raise ValueError('native program publication contains a linked path: '+current.name)
            if current==root:break
        else:raise ValueError('native program publication path escaped')
        if not path.resolve().is_relative_to(root.resolve()):
            raise ValueError('native program publication path escaped')
    def read_bounded(path,maximum):
        confined(path)
        with path.open('rb')as stream:
            data=stream.read(maximum+1)
        if len(data)>maximum:raise ValueError('native program resource file byte budget: '+path.name)
        return data
    confined(module)
    program=module/'native-programs.bin'
    confined(program)
    if not program.is_file():return {'programs':0,'resources':{},'references':[],'actor_accepted':False}
    manifest_path=root/'package/manifest.json'
    if not manifest_path.is_file()or manifest_path.stat().st_size>64*1024*1024:
        raise ValueError('native programs require the sealed client package manifest')
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('native program package JSON key repeats')
            result[key]=value
        return result
    def invalid_constant(_):raise ValueError('native program package JSON constant is invalid')
    try:manifest=json.loads(read_bounded(manifest_path,8*1024*1024),object_pairs_hook=unique,parse_constant=invalid_constant)
    except (UnicodeError,json.JSONDecodeError)as error:raise ValueError('native program package manifest is invalid JSON')from error
    if not isinstance(manifest,dict):raise ValueError('native program package manifest is not an object')
    files=manifest.get('files',{})
    if not isinstance(files,dict):raise ValueError('native program package file hashes are missing')

    def sealed(path,maximum,*,retain=True):
        confined(path)
        if not path.is_file()or path.is_symlink()or path.resolve().parent!=module.resolve():
            raise ValueError('native program resource file is missing or linked: '+path.name)
        expected=files.get('client/sdgo/mods/unit_catalog/'+path.name)
        if not isinstance(expected,str)or not re.fullmatch('[0-9a-f]{64}',expected):
            raise ValueError('native program resource file is not hash-bound: '+path.name)
        if path.stat().st_size>maximum:raise ValueError('native program resource file byte budget: '+path.name)
        digest=hashlib.sha256();pieces=[];size=0
        with path.open('rb')as stream:
            while raw:=stream.read(1024*1024):
                size+=len(raw)
                if size>maximum:raise ValueError('native program resource file byte budget: '+path.name)
                digest.update(raw)
                if retain:pieces.append(raw)
        if digest.hexdigest()!=expected:raise ValueError('native program resource file SHA-256 differs: '+path.name)
        return b''.join(pieces)if retain else size
    program_raw=sealed(program,native_programs.MAX_BYTES+128)
    catalog=module/'catalog.bin';catalog_raw=sealed(catalog,64*1024*1024+32)
    catalog_sha256=hashlib.sha256(catalog_raw).hexdigest()
    reader=None
    overlay=module/'resource-overlay.ini'
    confined(overlay)
    if overlay.is_file():
        overlay_raw=sealed(overlay,64*1024)
        cfg=configparser.ConfigParser(interpolation=None,strict=True)
        try:cfg.read_string(overlay_raw.decode('utf-8-sig'))
        except (configparser.Error,UnicodeError)as error:raise ValueError('native program resource configuration is invalid')from error
        if not cfg.has_section('runtime'):raise ValueError('native program resource configuration lacks runtime')
        section=cfg['runtime']
        if section.get('index')!='resources.zdx'or section.get('archive')!='resources.zpk':
            raise ValueError('native program resource overlay uses unsupported archive paths')
        try:parts=section.getint('parts',fallback=1);part_chunks=section.getint('part_chunks',fallback=0)
        except ValueError as error:raise ValueError('native program resource archive part budget')from error
        if not 1<=parts<=32 or part_chunks<0 or part_chunks*(CHUNK_SIZE+8)>MAX_ARCHIVE_BYTES or (parts>1 and part_chunks<1):
            raise ValueError('native program resource archive part budget')
        index=module/'resources.zdx';index_raw=sealed(index,MAX_INDEX_BYTES)
        if hashlib.sha256(index_raw).hexdigest()!=section.get('index_sha256'):
            raise ValueError('native program resource index identity differs')
        archive=hashlib.sha256();total=0
        for path in part_paths(module/'resources.zpk',parts):
            total+=sealed(path,MAX_ARCHIVE_BYTES,retain=False)
            if total>MAX_ARCHIVE_BYTES:raise ValueError('native program resource archive byte budget')
            with path.open('rb')as stream:
                while raw:=stream.read(1024*1024):archive.update(raw)
        if archive.hexdigest()!=section.get('archive_sha256'):
            raise ValueError('native program resource archive identity differs')
        reader=OverlayReader(index_raw,module/'resources.zpk',parts,part_chunks)
    def read(path):
        if reader is not None and path in reader.entries:return reader.read(path)
        if baseline is not None:return baseline.read(path)
        raise FileNotFoundError(path)
    try:
        report=native_programs.validate_resource_dependencies(program_raw,publication.records.get(10,{}),read,catalog_sha256=catalog_sha256)
        return {**report,'catalog_sha256':catalog_sha256}
    finally:
        if reader is not None:reader.close()
