"""Catalog/archive-bound skill atlases for the native detail controls.

All SPR frame texture dependencies are typed and hash-bound. This compiler
does not replace a global atlas, register a skill, or prove battle HUD behavior.
"""
from __future__ import annotations

import hashlib
import re
import struct

from .blueprint_resources import sprite_frames
from .effect_resources import inspect_texture
from .profiles.skill_target_1132 import recover_source_record

SCHEMA='sdgo.unit-skill-atlases/1'
MAGIC=b'SDGOAT1\0'
MAX_BYTES=8*1024*1024


def _hash(raw):return hashlib.sha256(raw).hexdigest()


def _digest(value):
    if not isinstance(value,str) or not re.fullmatch('[0-9a-f]{64}',value):raise ValueError('skill atlas SHA-256 is invalid')
    return value


def _exact(value,keys,label):
    if not isinstance(value,dict) or set(value)!=set(keys):raise ValueError(label+' fields differ')


def _path(value,root,suffix):
    if (not isinstance(value,str) or len(value)>80 or
            not re.fullmatch(root+r'/[a-z0-9][a-z0-9_.-]*'+re.escape(suffix),value) or '..' in value):
        raise ValueError('skill atlas resource path is invalid')
    return value


def validate_manifest(value,records,resources,*,authoring=False):
    _exact(value,{'schema','owner','revision','sprites','skills'},'skill atlas manifest')
    if value['schema']!=SCHEMA:raise ValueError('unsupported skill atlas schema')
    if not isinstance(value['owner'],str) or not re.fullmatch('[a-z0-9][a-z0-9._-]{0,63}',value['owner']):
        raise ValueError('skill atlas owner is invalid')
    _digest(value['revision'])
    if not isinstance(value['sprites'],list) or not 1<=len(value['sprites'])<=128:raise ValueError('skill atlas sprite count budget')
    sprites={};total=0
    for row in value['sprites']:
        _exact(row,{'path','sha256','textures'},'skill atlas sprite')
        path=_path(row['path'],'sprs','.spr')
        if path=='sprs/0594.spr' or path in sprites:raise ValueError('skill atlas requires an independent, unique sprite path')
        raw=resources.get(path)
        if not isinstance(raw,bytes) or _hash(raw)!=_digest(row['sha256']):raise ValueError('skill atlas sprite bytes differ')
        parsed=sprite_frames(raw)
        if not isinstance(row['textures'],list) or not 1<=len(row['textures'])<=128:raise ValueError('skill atlas texture count budget')
        textures={}
        for entry in row['textures']:
            _exact(entry,{'path','sha256'},'skill atlas texture')
            texture=_path(entry['path'],'txrs','.txr')
            if texture in textures:raise ValueError('skill atlas texture repeats')
            data=resources.get(texture)
            if not isinstance(data,bytes) or _hash(data)!=_digest(entry['sha256']):raise ValueError('skill atlas texture bytes differ')
            inspect_texture(data);textures[texture]=entry['sha256'];total+=len(data)
        if {frame['texture'] for frame in parsed['frames']}!=set(textures):
            raise ValueError('skill atlas typed texture inventory differs from all SPR frames')
        total+=len(raw)
        if total>128*1024*1024:raise ValueError('skill atlas media byte budget')
        sprites[path]={'sha256':row['sha256'],'frame_count':len(parsed['frames'])}
    if not isinstance(value['skills'],list) or not 1<=len(value['skills'])<=4096:raise ValueError('skill atlas record count budget')
    result=[];seen=set()
    for row in value['skills']:
        _exact(row,{'id','record_sha256','sprite'},'skill atlas record')
        identity=row['id']
        if type(identity)is not int or not 1<=identity<=9999999 or identity in seen:raise ValueError('skill atlas record identity repeats or is invalid')
        seen.add(identity);raw=records.get(identity)
        if (not isinstance(raw,bytes) or len(raw)!=1132 or raw[4]!=0 or struct.unpack_from('<I',raw)[0]!=identity or
                _hash(raw)!=_digest(row['record_sha256'])):raise ValueError('skill atlas complete record identity/SHA-256 differs')
        recover_source_record(raw)
        if raw[1131]>3 and not authoring:raise ValueError('skill atlas does not admit unknown skill dispatch')
        path=row['sprite']
        if not isinstance(path,str) or path not in sprites:raise ValueError('skill atlas record references an undeclared sprite')
        frame=struct.unpack_from('<H',raw,426)[0]
        if frame>=sprites[path]['frame_count']:raise ValueError('skill icon frame exceeds the declared owner atlas')
        result.append({'id':identity,'record_sha256':row['record_sha256'],'sprite':path,'frame':frame,**sprites[path]})
    return result


def compile_bundle(manifests,catalog_sha256,index_sha256,archive_sha256,records,resources):
    if not isinstance(manifests,list) or not 1<=len(manifests)<=128:raise ValueError('skill atlas owner count budget')
    owners=set();identities=set();entries=[]
    for manifest in manifests:
        bindings=validate_manifest(manifest,records,resources)
        if manifest['owner'] in owners:raise ValueError('skill atlas owner repeats')
        owners.add(manifest['owner'])
        for binding in bindings:
            if binding['id'] in identities:raise ValueError('skill atlas owners share a skill')
            identities.add(binding['id']);entries.append((manifest['owner'],manifest['revision'],binding))
    if len(entries)>4096:raise ValueError('skill atlas aggregate record count budget')
    payload=bytearray()
    def text(value):
        data=value.encode('ascii');payload.extend(struct.pack('<I',len(data)));payload.extend(data)
    for owner,revision,row in entries:
        payload.extend(struct.pack('<I',row['id']));payload.extend(bytes.fromhex(row['record_sha256']))
        text(owner);text(revision);text(row['sprite'])
        payload.extend(struct.pack('<I',row['frame_count']))
    if len(payload)>MAX_BYTES:raise ValueError('skill atlas sidecar byte budget')
    return (struct.pack('<8s4I',MAGIC,1,128,len(entries),len(payload))+
            b''.join(bytes.fromhex(_digest(value)) for value in (catalog_sha256,index_sha256,archive_sha256))+
            bytes(8)+payload)
