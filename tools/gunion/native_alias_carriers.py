"""Bounded native resource carriers and in-place alias prefix preparation.

Only declared fixed string fields are writable. OBS, scripts, runtime roots
and scanner candidates require their own consumer contracts.
"""
from dataclasses import dataclass, asdict
import hashlib
from pathlib import PurePosixPath
import re
import struct

from .minecraft_fields import WEAPON_TEXT
from .sprites import parse_sprite
from .zfx_dependencies import parse_dependencies, union_native_zfx_source_profile

_PATH = re.compile(r'(gdrs|mdrs|anrs|txrs|fxrs|sprs|sdrs|scp)/[a-z0-9_.-]+\.[a-z0-9]+')


def normalize(path):
    path = path.replace('\\', '/').lower()
    if not _PATH.fullmatch(path) or '..' in path.split('/'):
        raise ValueError('bounded source resource path required')
    return path


@dataclass(frozen=True)
class Carrier:
    offset: int
    capacity: int
    encoding: str
    literal: str
    path: str
    role: str
    contract: str

    def as_dict(self, raw):
        return dict(**asdict(self), fieldSha256=hashlib.sha256(raw[self.offset:self.offset+self.capacity]).hexdigest())


# When a caller supplies a list here, literals outside the bounded path grammar
# are recorded as opaque (offset, role, literal) and skipped instead of refused.
OPAQUE_LITERALS = None


def field(raw, offset, capacity, encoding, role, contract, root=None):
    if offset < 0 or capacity <= 0 or offset + capacity > len(raw):
        raise ValueError('resource carrier outside source')
    data = raw[offset:offset+capacity]
    step = 2 if encoding == 'utf-16-le' else 1
    end = next((i for i in range(0,capacity,step) if data[i:i+step] == b'\0'*step), None)
    if end is None:
        raise ValueError('unterminated resource carrier')
    literal = data[:end].decode(encoding)
    if not literal:
        return None
    try:
        path = normalize((root+'/' if root else '')+literal)
    except ValueError:
        if OPAQUE_LITERALS is None:
            raise
        OPAQUE_LITERALS.append((offset, role, literal))
        return None
    return Carrier(offset,capacity,encoding,literal,path,role,contract)


def fixed_records(raw, kind):
    if kind == 'source-unit-record':
        assert len(raw) == 4976
        fields = [(612,60,'unit-model'),(4654,100,'unit-portrait-shop'),
                  (4754,100,'unit-portrait-capsule'),(4854,100,'unit-portrait-detail'),
                  (0xF4C,100,'unit-sword-texture')]
    elif kind == 'source-weapon-record':
        assert len(raw) == 1871
        names = ['muzzleEffectPath','impactEffectPath','soundPath','projectileModelPath',
                 'weaponHudTexturePath','weaponHudSelectedTexturePath','projectileTexturePath','secondarySoundPath']
        fields = [(*WEAPON_TEXT[name], 'weapon-'+name) for name in names]
    elif kind in ('source-skill-record','skill-definition'):
        assert len(raw) == 882
        fields = [(308,60,'skill-effect')]
    elif kind == 'source-funnel-record':
        assert len(raw) == 732
        fields = [(6+i*100,100,'fang-model-slot-'+str(i)) for i in range(6)]
    else:
        raise ValueError('unsupported native record carrier kind: '+kind)
    return [c for offset,size,role in fields
            if (c:=field(raw,offset,size,'utf-16-le',role,'fixed-native-grid-field')) is not None]


def model_materials(raw):
    if len(raw)<25 or raw[4:8] != b'ZOA\0':
        raise ValueError('native model header')
    material,names,bones,meshes = struct.unpack_from('<iiii',raw,9)
    start = 25+(names+bones)*256
    if min(material,names,bones,meshes)<0 or start+material*580>len(raw):
        raise ValueError('native model header count/extent')
    return [c for i in range(material)
            if (c:=field(raw,start+i*580+68,512,'utf-16-le','model-material-'+str(i),'native-C31B70-leading-material-table')) is not None]


def effect_carriers(raw,path):
    parsed = parse_dependencies(raw,union_native_zfx_source_profile(path,hashlib.sha256(raw).hexdigest()))
    result = []
    for reference in parsed['references']:
        if reference['kind'] != 'texture':
            continue
        carrier=reference['carrier']
        c=field(raw,carrier['offset'],carrier['length'],'ascii','effect-texture','native-ZFX-texture-first-NUL','txrs')
        if c: result.append(c)
    for mesh in parsed['trailer']['meshCarriers']:
        carrier=mesh['modelCarrier']
        c=field(raw,carrier['offset'],carrier['length'],'ascii','effect-model','native-ZFX-mesh-trailer','mdrs')
        if c: result.append(c)
    return result


def sprite_carriers(raw,path):
    parsed=parse_sprite(raw,path)
    result=[]
    for frame in parsed['frames']:
        carrier=frame['fieldProvenance']['texturePath']
        c=field(raw,carrier['offset'],carrier['size'],'utf-16-le','sprite-texture','native-SPR-first-texture-only')
        if c: result.append(c)
    return result


def obs_carriers(raw,offset):
    """Read fixed native LoadKudiz/CFrameEventSys strings at a geometry-bound
    start. No sentinel search or event payload/string scanning is performed.
    Absolute skip offsets, resource types, event records and names are retained.
    """
    if len(raw)<4 or struct.unpack_from('<I',raw)[0]!=0xC6E9:
        raise ValueError('unadmitted native OBS file version')
    pos=offset;result=[];actions=[];skips=[];event_spans=[];opaque=[]
    def need(start,size):
        if start<0 or size<0 or start+size>len(raw):raise ValueError('truncated native OBS')
    def count(limit):
        nonlocal pos
        need(pos,4);value=struct.unpack_from('<i',raw,pos)[0];pos+=4
        if value<0 or value>limit:raise ValueError('native OBS count outside admitted budget')
        return value
    def text(start,size):
        need(start,size);data=raw[start:start+size]
        end=next((i for i in range(0,size,2)if data[i:i+2]==b'\0\0'),None)
        if end is None:raise ValueError('unterminated native OBS text')
        return data[:end].decode('utf-16-le')
    material_count=count(4096)
    need(pos,material_count*324)
    for index in range(material_count):
        for stage in range(4):
            c=field(raw,pos+stage*64,64,'utf-16-le','obs-material-'+str(index)+'-stage-'+str(stage),'native-LoadKudiz-four-fixed-texture-stages')
            if c:result.append(c)
        pos+=324
    animation_count=count(4096)
    for index in range(animation_count):
        need(pos,4);skip_at=pos;end_offset=struct.unpack_from('<i',raw,pos)[0];pos+=4
        name=text(pos,128).rstrip(' ');pos+=128
        if name!='Global_Script':
            literal=text(pos,128)
            if literal:
                normalized=literal.replace('\\','/').lower()
                if normalized.endswith('.ani'):
                    root=None if '/'in normalized else'anrs'
                    c=field(raw,pos,128,'utf-16-le','obs-animation:'+name,'native-LoadKudiz-action-animation',root)
                    if c:result.append(c)
                else:opaque.append(dict(action=name,offset=pos,capacity=128,literal=literal,reason='non-file-animation-token'))
            pos+=128
        need(pos,1);has_events=raw[pos];pos+=1
        if has_events not in (0,1):raise ValueError('unadmitted native OBS has-events flag')
        resources=groups=events=0
        if has_events:
            resources=count(1024)
            for resource in range(resources):
                need(pos,68);kind=struct.unpack_from('<I',raw,pos)[0];literal=text(pos+4,64)
                if literal:
                    if kind in (0x20003,0x30000,0x10000):
                        c=field(raw,pos+4,64,'utf-16-le','obs-resource:'+name+':'+str(resource),'native-CFrameEventSys-typed-resource-table')
                        if c:result.append(c)
                    else:opaque.append(dict(action=name,offset=pos+4,capacity=64,resourceType=kind,literal=literal,reason='unadmitted-resource-type'))
                pos+=68
            groups=count(65536)
            for group in range(groups):
                need(pos,4);frame=struct.unpack_from('<i',raw,pos)[0];pos+=4
                event_count=count(65536);need(pos,event_count*172)
                event_spans.append(dict(offset=pos,bytes=event_count*172,sha256=hashlib.sha256(raw[pos:pos+event_count*172]).hexdigest(),frame=frame))
                pos+=event_count*172;events+=event_count
        decoded_end=pos
        if not 0<=end_offset<=len(raw):raise ValueError('native OBS absolute skip outside model')
        if name=='Global_Script':
            if end_offset<pos:raise ValueError('native OBS global skip precedes decoded payload')
            pos=end_offset
        skips.append(dict(offset=skip_at,sourceValue=end_offset,usedForGlobal=name=='Global_Script'))
        actions.append(dict(index=index,name=name,decodedEnd=decoded_end,storedEnd=end_offset,resources=resources,frameGroups=groups,events=events))
    return result,dict(kudizOffset=offset,parsedEnd=pos,sourceBytes=len(raw),trailingBytes=len(raw)-pos,
        materialOverrides=material_count,actions=actions,absoluteSkipFields=skips,unchangedEventSpans=event_spans,opaque=opaque)


def rewrite_prefixes(raw,carriers,aliases):
    """Prepare caller-assigned names; no allocation/publication authority.

    Prefix writes retain every original byte after the new terminator.
    Overlap, stale fields, directory/extension changes and overflow refuse.
    """
    output=bytearray(raw);spans=[];seen={}
    for c in carriers:
        if c.path not in aliases:
            continue
        alias=normalize(aliases[c.path])
        if PurePosixPath(alias).parent != PurePosixPath(c.path).parent or PurePosixPath(alias).suffix != PurePosixPath(c.path).suffix:
            raise ValueError('alias resource type differs')
        root=None if '/' in c.literal or '\\' in c.literal else c.path.split('/')[0]
        actual=field(raw,c.offset,c.capacity,c.encoding,c.role,c.contract,root)
        if actual != c:
            raise ValueError('stale source carrier')
        target=PurePosixPath(alias).name if root else alias.replace('/', '\\' if '\\' in c.literal else '/')
        value=target.encode(c.encoding)+(b'\0\0' if c.encoding=='utf-16-le' else b'\0')
        if len(value)>c.capacity:
            raise ValueError('alias exceeds carrier capacity')
        extent=(c.offset,c.offset+c.capacity)
        if extent in seen:
            if seen[extent] != value:raise ValueError('conflicting duplicate resource carrier')
            continue
        if any(max(extent[0],a)<min(extent[1],b) for a,b in seen):
            raise ValueError('overlapping resource carriers')
        seen[extent]=value
        output[c.offset:c.offset+len(value)]=value
        spans.append(dict(offset=c.offset,capacity=c.capacity,overwrittenBytes=len(value),source=c.path,target=alias,
                          sourceFieldSha256=hashlib.sha256(raw[c.offset:c.offset+c.capacity]).hexdigest(),contract=c.contract))
    return bytes(output),spans
