"""Bounded, execution-free adapters for operator artwork/voice packages.

Legacy files are inputs. Generated resources get private names and never
replace gdrs/0004.gdt or the player's original SPR/TXR/SDR files.
"""
from __future__ import annotations
import hashlib,io,json,math,re,stat,struct,zipfile,zlib
from pathlib import PurePosixPath
from . import assets

FORMAT='sdgo.operator-pack'
FORMAT_VERSION=1
LAYER_VERSIONS={'resources':1,'abilities':1,'acquisition':1,'storefront':1}
MAX_ARCHIVE=128*1024*1024
MAX_EXPANDED=256*1024*1024
MAX_FILE=64*1024*1024
ROOTS={'gdrs':'.gdt','sprs':'.spr','txrs':'.txr','sdrs':'.zsd'}
MANIFEST='operator-pack.json'

def digest(raw):return hashlib.sha256(raw).hexdigest()

def name(value):
    value=value.replace('\\','/')
    if (not value or len(value)>240 or value.startswith('/') or ':' in value or
        any(ord(c)<32 for c in value) or any(p in ('','..','.') for p in value.rstrip('/').split('/'))):
        raise ValueError('副官包包含不安全的资源路径')
    return value.rstrip('/').casefold()

def resource_name(value):
    value=name(value)
    parts=value.split('/')
    if len(parts)!=2 or parts[0] not in ROOTS or not re.fullmatch(r'[0-9]+'+re.escape(ROOTS[parts[0]]),parts[1]):
        raise ValueError('旧副官资源必须使用原生资源目录和数字文件名')
    return value

def unpack_resource(raw):
    if not raw.startswith(b'ZOAGZIP1'):return raw
    if len(raw)<12:raise ValueError('资源压缩头不完整')
    expected=struct.unpack_from('<I',raw,8)[0]
    if expected>MAX_FILE:raise ValueError('资源解压量过大')
    decoder=zlib.decompressobj(31)
    result=decoder.decompress(raw[12:],expected+1)
    if len(result)!=expected or not decoder.eof or decoder.unused_data:
        raise ValueError('资源压缩校验失败')
    return result

class Gdt:
    """Native F9F8 record codec; dialogue records do not have numeric IDs."""
    def __init__(self,raw,expected_size=None):
        raw=unpack_resource(raw)
        if len(raw)<14 or raw[:2]!=b'\xf9\xf8':raise ValueError('不支持的副官 GDT 格式')
        size,count,extra=struct.unpack_from('<III',raw,2)
        n=(size+count+extra)%65+192
        stride=((size+n-1)//n)*n
        if (not 1<=size<=65536 or not 0<count<=65536 or
            expected_size is not None and size!=expected_size or
            len(raw)!=14+n+count*stride or len(raw)>MAX_FILE):
            raise ValueError('副官 GDT 记录布局无效')
        key=raw[14:14+n]
        if sorted(key)!=list(range(n)):raise ValueError('副官 GDT 置换表无效')
        self.header=raw[:14];self.key=key;self.stride=stride;self.size=size;self.rows=[];self.blocks=[]
        for index in range(count):
            start=14+n+index*stride;decoded=bytearray()
            for offset in range(0,stride,n):
                block=raw[start+offset:start+offset+n]
                decoded.extend(block[key[j]]^key[(768+3*j)%n] for j in range(n))
            self.rows.append(bytearray(decoded[:size]));self.blocks.append(decoded)

    def encode(self):
        out=bytearray(self.header+self.key);n=len(self.key)
        for row,padded in zip(self.rows,self.blocks):
            if len(row)!=self.size:raise ValueError('GDT 记录长度被改变')
            decoded=bytearray(padded);decoded[:self.size]=row
            for offset in range(0,self.stride,n):
                block=bytearray(n)
                for j in range(n):block[self.key[j]]=decoded[offset+j]^self.key[(768+3*j)%n]
                out.extend(block)
        return bytes(out)

def sprite(raw,*,allow_compiled=False):
    raw=unpack_resource(raw)
    if len(raw)<12:raise ValueError('SPR 头不完整')
    version,count,kind=struct.unpack_from('<III',raw)
    if version not in (10000,11000) or not 1<=count<=1024 or kind!=20:
        raise ValueError('不支持的副官 SPR 版本或帧数')
    stride=164 if version==10000 else 172
    path_offset=32 if version==10000 else 40
    if len(raw)!=12+count*stride:raise ValueError('SPR 记录长度无效')
    frames=[]
    for index in range(count):
        start=12+index*stride;height,width=struct.unpack_from('<II',raw,start)
        uv=struct.unpack_from('<4f',raw,start+8)
        path=assets.wchar(raw,start+path_offset,assets.SPR_TEXTURE_WCHARS)
        # Native atlases may extend a little across their wrap boundary or
        # mirror an axis. Keep those coordinates unchanged.
        if (not 1<=width<=16384 or not 1<=height<=16384 or
            not all(math.isfinite(v) and -1<=v<=2 for v in uv) or
            not 0<abs(uv[2]-uv[0])<=1 or not 0<abs(uv[3]-uv[1])<=1):
            raise ValueError('SPR 尺寸或裁切范围无效')
        normalized=name(path)
        private=allow_compiled and (
            re.fullmatch(r'sdgo/mods/operator_catalog/assets/[0-9]+/[0-9]+\.txr',normalized)
            or re.fullmatch(r'sdgo/optex/[0-9]+/[0-9]+\.txr',normalized)
        )
        frames.append({'offset':start+path_offset,'texture':normalized if private else resource_name(path),
                       'width':width,'height':height,'uv':list(uv)})
    return raw,frames

def texture(raw):
    from PIL import Image
    raw=unpack_resource(raw)
    image_raw=raw[12:] if raw.startswith(b'ZOATEX1\0') else raw
    if len(image_raw)>MAX_FILE:raise ValueError('副官纹理过大')
    with Image.open(io.BytesIO(image_raw)) as image:
        if not 1<=image.width<=4096 or not 1<=image.height<=4096 or image.width*image.height>16777216:
            raise ValueError('副官纹理尺寸超出限制')
        image.verify()
    return image_raw

def voice(raw):
    # ZSD assets in replacement packs are native Miles-supported MP3/WAVE
    # streams. Preserve their bytes and do not run an external converter.
    data=unpack_resource(raw)
    if not data or len(data)>16*1024*1024 or not (data.startswith(b'ID3') or data.startswith(b'RIFF') and data[8:12]==b'WAVE' or
            len(data)>1 and data[0]==255 and data[1]&224==224):
        raise ValueError('副官语音不是支持的原生音频流')
    return raw

class Pack:
    def __init__(self,raw,base,*,filename=''):
        if not isinstance(raw,bytes) or not raw or len(raw)>MAX_ARCHIVE:raise ValueError('副官包须为不超过 128 MiB 的 ZIP')
        self.sha256=digest(raw);self.base=base;self.files={};self.ignored=[];self.manifest=None
        try:
            with zipfile.ZipFile(io.BytesIO(raw)) as z:
                entries=z.infolist()
                if len(entries)>4096 or sum(e.file_size for e in entries)>MAX_EXPANDED:raise ValueError('副官包文件数量或解压量过大')
                seen=set()
                for entry in entries:
                    key=name(entry.filename)
                    if key in seen:raise ValueError('副官包包含重复路径')
                    seen.add(key)
                    if stat.S_ISLNK(entry.external_attr>>16) or entry.flag_bits&1:raise ValueError('副官包不能包含链接或加密文件')
                    if entry.is_dir():continue
                    if entry.file_size>MAX_FILE:raise ValueError('副官包单文件过大')
                    if key==MANIFEST:
                        self.manifest=json.loads(z.read(entry).decode('utf-8-sig'));continue
                    if any(key.startswith(k+'/') for k in ROOTS):
                        key=resource_name(key);self.files[key]=z.read(entry)
                    else:self.ignored.append(entry.filename)
        except (zipfile.BadZipFile,UnicodeError,json.JSONDecodeError) as error:
            raise ValueError('副官包 ZIP 或清单无效') from error
        self.format='protocol' if self.manifest is not None else 'legacy-replacement'
        self.label=PurePosixPath(filename.replace('\\','/')).stem[:48] if filename else '导入副官'
        self.preferred_template=None;self.profile=None
        if self.manifest is not None:self.validate_manifest()
        if not self.files:raise ValueError('副官包没有可导入的原生资源')
        self.frames={};self.dependencies={};self.dialogues={};self.voice_events=[]
        for key,data in self.files.items():
            if key.startswith('sprs/'):
                _,frames=sprite(data);self.frames[key]=frames
                for frame in frames:self.dependencies[frame['texture']]=frame['texture'] in self.files
            elif key.startswith('txrs/'):texture(data)
            elif key.startswith('sdrs/'):voice(data)
            elif key.startswith('gdrs/'):
                table=Gdt(data,710);self.dialogues[key]=table
                for index,row in enumerate(table.rows):
                    # The 710-byte table stores two independent 50-wchar
                    # variants. Treating them as one 100-wchar path erases
                    # the second variant while rewriting the first.
                    for variant,offset in enumerate((502,602)):
                        path=assets.wchar(row,offset,50)
                        if path:
                            source=resource_name(path)
                            if not source.startswith('sdrs/'):raise ValueError('台词表包含非语音路径')
                            self.dependencies[source]=source in self.files
                            if source in self.files:
                                self.voice_events.append({'table':key,'row':index,'variant':variant,'event':assets.wchar(row,0,50),
                                                          'caption':assets.wchar(row,100,200),'voice':source})
        records=base.records('gdrs/0255.gdt',assets.OP_SIZE)
        self.candidates=[];matched=[]
        for identity,row in records.items():
            speech=name(assets.wchar(row,105,50)) if assets.wchar(row,105,50) else ''
            matches=[assets.wchar(row,o,c).replace('\\','/').casefold() in self.files for o,c in assets.SPRITE_FIELDS]
            if speech in self.dialogues or any(matches):matched.append((identity,speech in self.dialogues,sum(matches)))
        if matched:
            exact=[x for x in matched if x[1]]
            relevant=exact or matched
            strongest=max(x[2] for x in relevant)
            self.candidates=[x[0] for x in relevant if x[2]==strongest]
        if self.profile is not None:
            preferred=self.preferred_template or 60001
            if preferred not in records:raise ValueError('目标副官模板不存在')
            self.candidates=[preferred]
        if not self.candidates:raise ValueError('副官包未匹配到当前客户端的副官资源模板')
        if self.preferred_template is not None and self.preferred_template not in self.candidates:raise ValueError('协议资源模板与实际内容不一致')

    def validate_manifest(self):
        m=self.manifest
        if not isinstance(m,dict) or m.get('format')!=FORMAT or m.get('format_version')!=FORMAT_VERSION:
            raise ValueError('不支持的副官包协议版本')
        if set(m)-{'format','format_version','pack_id','name','layers','required_features','source_template','files','resources','distribution','abilities','acquisition','storefront'}:
            raise ValueError('副官包清单含未知的根字段')
        if not isinstance(m.get('pack_id'),str) or not re.fullmatch(r'[a-z0-9][a-z0-9_.-]{0,95}',m['pack_id']):raise ValueError('副官包标识无效')
        if m.get('layers',LAYER_VERSIONS)!=LAYER_VERSIONS:raise ValueError('副官包包含不支持的分层协议版本')
        if m.get('required_features',[]):raise ValueError('副官包要求尚未注册的扩展能力')
        if not isinstance(m.get('files'),dict) or set(m['files'])!=set(self.files):raise ValueError('副官包文件清单不完整')
        for path,spec in m['files'].items():
            if not isinstance(spec,dict) or spec.get('sha256')!=digest(self.files[path]) or spec.get('bytes')!=len(self.files[path]):
                raise ValueError('副官包资源校验失败：'+path)
        if not isinstance(m.get('name'),str) or not m['name'].strip() or len(m['name'].encode('utf-16-le'))>96:raise ValueError('副官包名称无效')
        self.label=m['name'];self.preferred_template=m.get('source_template')
        if self.preferred_template is not None and type(self.preferred_template)is not int:raise ValueError('资源模板编号无效')
        resources=m.get('resources',{})
        resource_fields={'profile','locale','source_label','source_index_sha256','source_operator_id','source_operator_record_size',
                         'source_missing_dependencies','unavailable_profile_roles','voice_policy'}
        if not isinstance(resources,dict) or set(resources)-resource_fields:raise ValueError('副官资源描述无效')
        if resources.get('voice_policy','provided_only' if resources.get('locale') else 'native_fallback') not in ('provided_only','native_fallback'):
            raise ValueError('副官语音缺失策略无效')
        profile=resources.get('profile')
        if profile is not None:
            if not isinstance(profile,dict) or set(profile)!={'dialogue','sprites','intro_voice'} or not isinstance(profile['sprites'],list) or len(profile['sprites'])!=7:
                raise ValueError('副官资源角色必须包含台词表、七个图像位置及出击语音')
            if any(not isinstance(p,str) for p in [profile['dialogue'],profile['intro_voice'],*profile['sprites']]):raise ValueError('资源角色路径无效')
            for p in [profile['dialogue'],profile['intro_voice'],*profile['sprites']]:
                if p and resource_name(p) not in self.files:raise ValueError('独立副官协议缺少角色资源：'+p)
            if profile['dialogue'] and not profile['dialogue'].startswith('gdrs/'):raise ValueError('台词表类型无效')
            if profile['intro_voice'] and not profile['intro_voice'].startswith('sdrs/'):raise ValueError('出击语音类型无效')
            if any(p and not p.startswith('sprs/') for p in profile['sprites']):raise ValueError('图像角色类型无效')
            self.profile=profile
        distribution=m.get('distribution',{'scope':'client-content','bundle_with_manager':False})
        if distribution!={'scope':'client-content','bundle_with_manager':False}:
            raise ValueError('副官素材包只能作为客户端内容，不进入 Manager 或框架程序包')

    def describe(self):
        resources=(self.manifest or {}).get('resources',{})
        isolated=resources.get('voice_policy','provided_only' if resources.get('locale') else 'native_fallback')=='provided_only'
        absent=sorted(k for k,v in self.dependencies.items() if not v)
        suppressed=[k for k in absent if isolated and k.startswith('sdrs/')]
        return {'format':self.format,'format_version':FORMAT_VERSION,'sha256':self.sha256,'name':self.label,
                'layers':LAYER_VERSIONS,'source_templates':self.candidates,'source_template':self.preferred_template or self.candidates[0],
                'files':{k:{'sha256':digest(v),'bytes':len(v)} for k,v in self.files.items()},
                'sprite_frames':{k:len(v) for k,v in self.frames.items()},'voice_files':sum(k.startswith('sdrs/') for k in self.files),
                'voice_events':self.voice_events,'base_dependencies':[k for k in absent if k not in suppressed],
                'suppressed_audio_dependencies':suppressed,
                'ignored_files':self.ignored,'distribution':{'scope':'client-content','bundle_with_manager':False},
                'resources':self.manifest.get('resources',{}) if self.manifest else {},
                'abilities':self.manifest.get('abilities') if self.manifest else None,
                'acquisition':self.manifest.get('acquisition') if self.manifest else None,
                'storefront':self.manifest.get('storefront') if self.manifest else None}

    def compile(self,operator_id,source_template):
        if type(operator_id)is not int or not 60001<=operator_id<=60999 or source_template not in self.candidates:
            raise ValueError('副官资源导出目标无效')
        # Short numeric target names fit the native 50-wchar fields.
        prefix=f'sdgo/mods/operator_catalog/assets/{operator_id}/'
        mapping={
            key:(f'sdgo/optex/{operator_id}/{i}.txr'
                 if key.startswith('txrs/') else
                 prefix+f'{i}{ROOTS[key.split("/")[0]]}')
            for i,key in enumerate(sorted(self.files))
        }
        out={}
        for key,data in self.files.items():
            if key.startswith('sprs/'):
                raw,frames=sprite(data);raw=bytearray(raw)
                for frame in frames:
                    if frame['texture'] in mapping:
                        assets.put_wchar(
                            raw,frame['offset'],assets.SPR_TEXTURE_WCHARS,
                            mapping[frame['texture']].replace('/','\\'),
                        )
                data=bytes(raw)
            elif key.startswith('gdrs/'):
                table=Gdt(data,710)
                resources=(self.manifest or {}).get('resources',{})
                isolated_audio=resources.get('voice_policy','provided_only' if resources.get('locale') else 'native_fallback')=='provided_only'
                for row in table.rows:
                    for offset in (502,602):
                        path=assets.wchar(row,offset,50)
                        if path:
                            source=resource_name(path)
                            if source in mapping:assets.put_wchar(row,offset,50,mapping[source].replace('/','\\'))
                            elif isolated_audio:assets.put_wchar(row,offset,50,'')
                    if isolated_audio and not assets.wchar(row,502,50) and not assets.wchar(row,602,50):row[501]=0
                data=table.encode()
            out[mapping[key]]=data
        row=self.base.records('gdrs/0255.gdt',assets.OP_SIZE)[source_template]
        inherited=[assets.wchar(row,o,c).replace('\\','/').casefold() for o,c in assets.SPRITE_FIELDS]
        paths=[p or inherited[i] for i,p in enumerate(self.profile['sprites'])] if self.profile else inherited
        speech=self.profile['dialogue'] if self.profile else assets.wchar(row,105,50).replace('\\','/').casefold()
        intro=self.profile['intro_voice'] if self.profile else assets.wchar(row,933,50).replace('\\','/').casefold()
        fields={'sprites':[mapping.get(p,p).replace('/','\\') for p in paths],
                'dialogue':mapping.get(speech,speech).replace('/','\\'),
                'intro_voice':mapping.get(intro,intro).replace('/','\\')}
        if any(len(p.encode('utf-16-le'))>=100 for p in [fields['dialogue'],fields['intro_voice'],*fields['sprites']]):
            raise ValueError('生成的资源路径超过原生字段容量')
        return out,fields

    def portrait(self,source_template=None):
        from PIL import Image
        def axis(value0,value1,size):
            start=round(value0*size);end=round(value1*size)
            length=abs(end-start)
            if not length:length=1
            origin=min(start,end)%size
            first=min(length,size-origin)
            parts=[(origin,first,0)]
            if first<length:parts.append((0,length-first,first))
            return length,parts,value1<value0
        row=self.base.records('gdrs/0255.gdt',assets.OP_SIZE)[source_template or self.preferred_template or self.candidates[0]]
        for index in (3,5,1,2):
            off,cap=assets.SPRITE_FIELDS[index]
            source=self.profile['sprites'][index] if self.profile else assets.wchar(row,off,cap).replace('\\','/').casefold()
            if source not in self.files:continue
            _,frames=sprite(self.files[source]);frame=frames[0]
            raw=self.files.get(frame['texture'])
            if raw is None:raw=self.base.read(frame['texture'])
            with Image.open(io.BytesIO(texture(raw))) as image:
                image.load();u0,v0,u1,v1=frame['uv']
                width,xparts,xflip=axis(u0,u1,image.width)
                height,yparts,yflip=axis(v0,v1,image.height)
                cropped=Image.new('RGBA',(width,height))
                for sy,sh,dy in yparts:
                    for sx,sw,dx in xparts:
                        piece=image.crop((sx,sy,sx+sw,sy+sh)).convert('RGBA')
                        cropped.paste(piece,(dx,dy))
                if xflip:cropped=cropped.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
                if yflip:cropped=cropped.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
                output=io.BytesIO();cropped.save(output,'PNG');return output.getvalue()
        return self.base.portrait(source_template or self.candidates[0])

def package(files,profile,*,pack_id,name,source_template=60001,locale='',abilities=None,acquisition=None,storefront=None,resources_metadata=None):
    """Build a client-content-only exchange package. No executable is admitted."""
    canonical={resource_name(k):v for k,v in files.items()}
    if len(canonical)!=len(files):raise ValueError('包中存在大小写资源冲突')
    manifest={'format':FORMAT,'format_version':FORMAT_VERSION,'pack_id':pack_id,'name':name,
              'layers':dict(LAYER_VERSIONS),'source_template':source_template,
              'distribution':{'scope':'client-content','bundle_with_manager':False},
              'resources':{**(resources_metadata or {}),'profile':profile,'locale':locale},
              'files':{k:{'bytes':len(v),'sha256':digest(v)} for k,v in sorted(canonical.items())}}
    for key,value in (('abilities',abilities),('acquisition',acquisition),('storefront',storefront)):
        if value is not None:manifest[key]=value
    output=io.BytesIO()
    with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED) as z:
        payload={MANIFEST:(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode('utf-8'),**canonical}
        for key,data in sorted(payload.items()):
            entry=zipfile.ZipInfo(key,(1980,1,1,0,0,0));entry.compress_type=zipfile.ZIP_DEFLATED
            entry.external_attr=0o100644<<16;z.writestr(entry,data)
    raw=output.getvalue()
    if len(raw)>MAX_ARCHIVE:raise ValueError('独立副官包超过 128 MiB')
    return raw
