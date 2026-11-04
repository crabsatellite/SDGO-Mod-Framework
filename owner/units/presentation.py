"""Presentation generators shared by authoring previews and publication."""
from __future__ import annotations
import base64
from collections import OrderedDict
import hashlib
import io
from pathlib import Path
from .assets import normalize_png

GENERATOR_VERSION = 4
BLUEPRINT_PREVIEW_SIZE = (252, 101)
BLUEPRINT_FRAME_SAMPLE_COUNT = 48
_BLUEPRINT_FRAME_CACHE = OrderedDict()


def _png(image):
    stream=io.BytesIO();image.save(stream,'PNG');return stream.getvalue()


def _alpha_resize(image,size,*,cover):
    """Resize RGBA through premultiplied color so transparent edges stay clean."""
    from PIL import Image
    width,height=size
    scale=(max if cover else min)(width/image.width,height/image.height)
    resized=image.convert('RGBa').resize(
        (max(1,round(image.width*scale)),max(1,round(image.height*scale))),
        Image.Resampling.LANCZOS).convert('RGBA')
    if cover:
        left=max(0,(resized.width-width)//2);top=max(0,(resized.height-height)//2)
        return resized.crop((left,top,left+width,top+height))
    result=Image.new('RGBA',size)
    result.alpha_composite(
        resized,((width-resized.width)//2,(height-resized.height)//2))
    return result


def _native_blueprint_frame(client):
    """Recover the fixed 252x101 outline shared by native blueprint previews.

    Kind-0x21 records store their large preview SPR at +289. The shipped
    sprites already contain the thin outline and corner cut over a transparent
    interior. Intersecting many unrelated native previews retains that outline
    while excluding every donor unit.
    """
    from collections import deque
    from PIL import Image
    from types import SimpleNamespace
    from manager.engines.capsules.capsule_assets import Assets,sprite_frame
    from manager.engines.capsules.capsule_catalog import ClientData,gdt_records
    reader=ClientData(client)
    raw_gdt=reader.read_resource('gdrs/0029.gdt')
    size,rows=gdt_records(raw_gdt)
    if size!=1197:raise ValueError('原生设计图预览表布局不受支持')
    resources={};paths=[];seen=set()
    def resource(path):
        key=path.replace('\\','/').lower()
        if key not in resources:resources[key]=reader.read_resource(key)
        return resources[key]
    for row in rows:
        if len(row)!=1197 or row[4]!=0:continue
        path=row[289:349].decode('utf-16-le').split('\0')[0].replace('\\','/').lower()
        if not path or path in seen:continue
        if not path.startswith('sprs/') or not path.endswith('.spr'):
            raise ValueError('原生设计图预览引用不是SPR')
        spr=resource(path);frame=sprite_frame(spr)
        if (frame['width'],frame['height'])!=BLUEPRINT_PREVIEW_SIZE:continue
        texture=frame['texture'].replace('\\','/').lower()
        resource(texture)
        seen.add(path);paths.append(path)
        if len(paths)==BLUEPRINT_FRAME_SAMPLE_COUNT:break
    if len(paths)<BLUEPRINT_FRAME_SAMPLE_COUNT:
        raise ValueError('原生设计图预览样本不足，无法可靠恢复显示边框')
    digest=hashlib.sha256(raw_gdt)
    for path in sorted(resources):
        raw=resources[path]
        digest.update(path.encode('ascii'));digest.update(len(raw).to_bytes(8,'little'));digest.update(raw)
    key=(str(Path(client).resolve()).lower(),digest.hexdigest())
    cached=_BLUEPRINT_FRAME_CACHE.get(key)
    if cached is not None:
        _BLUEPRINT_FRAME_CACHE.move_to_end(key)
        return cached.copy()
    renderer=object.__new__(Assets)
    renderer.reader=SimpleNamespace(read_resource=resource)
    samples=[renderer.sprite(path).convert('RGBA') for path in paths]
    fixed=set()
    for y in range(BLUEPRINT_PREVIEW_SIZE[1]):
        for x in range(BLUEPRINT_PREVIEW_SIZE[0]):
            pixel=samples[0].getpixel((x,y))
            if pixel[3] and all(sample.getpixel((x,y))==pixel for sample in samples[1:]):
                fixed.add((x,y))
    if not fixed:raise ValueError('原生设计图预览没有可恢复的固定边框')
    width,height=BLUEPRINT_PREVIEW_SIZE
    pending=deque((x,y) for x,y in fixed if x in (0,width-1) or y in (0,height-1))
    connected=set(pending)
    while pending:
        x,y=pending.popleft()
        for near_y in range(max(0,y-1),min(height,y+2)):
            for near_x in range(max(0,x-1),min(width,x+2)):
                point=(near_x,near_y)
                if point in fixed and point not in connected:
                    connected.add(point);pending.append(point)
    if connected!=fixed:
        raise ValueError('原生设计图固定像素含有与边框无关的内容，拒绝复制模板机体')
    result=Image.new('RGBA',BLUEPRINT_PREVIEW_SIZE)
    for x,y in connected:result.putpixel((x,y),samples[0].getpixel((x,y)))
    _BLUEPRINT_FRAME_CACHE[key]=result
    _BLUEPRINT_FRAME_CACHE.move_to_end(key)
    while len(_BLUEPRINT_FRAME_CACHE)>4:_BLUEPRINT_FRAME_CACHE.popitem(last=False)
    return result.copy()


def _clear_native_blueprint_frame_cache():
    _BLUEPRINT_FRAME_CACHE.clear()


def capsule_unit(engine,family):
    from manager.engines.capsules.capsule_catalog import grade
    root=family['root_unit_id'];form=next(r for r in family['forms'] if r['id']==root)
    sha=family['presentation']['capsule']['asset_sha']
    return {'id':root,'name':family['name'],'rank':form['server']['UnitGradeNumber'],
            'subgrade':form['server']['UnitGradeNumber2'],'grade':grade(form['server']),
            'capsule_sprites':[],'unit_import':{'image_sha256':sha,
                'image_png':base64.b64encode(engine.blob(sha)).decode('ascii')}}


def capsule_card(engine,family,variant=0):
    """The existing capsule compiler's frame/crop/rare-silhouette renderer."""
    from manager.engines.capsules.capsule_assets import Assets
    unit=capsule_unit(engine,family)
    if engine.baseline.root is None:raise ValueError('生成扭蛋卡面需要配套客户端的原生卡框资源')
    assets=Assets(engine.baseline.root,{}, {'units':[unit]})
    return _png(assets.card(unit['id'],variant))


def capsule_source(engine,family):
    """S/SR custom lists use the game's own candidate controls and rank frame."""
    return unit_images(engine,family)['custom']


def unit_images(engine,family):
    """Read the three original unit sprites at their actual consumer sizes.

    A selected full-body image is not three interchangeable native images.
    Existing package sprites stay separate. Explicit PNG uploads can replace
    their own channel using its verified native canvas.
    """
    from PIL import Image,ImageOps
    from types import SimpleNamespace
    from manager.engines.capsules.capsule_assets import Assets
    root=family['root_unit_id'];form=next(r for r in family['forms'] if r['id']==root)
    raw=engine.blob(form['source_sha'])
    files={f['path'].replace('\\','/').lower():f['sha'] for f in family['files']}
    def read(path):
        name=path.replace('\\','/').lower()
        return engine.blob(files[name]) if name in files else engine.baseline.read(name)
    renderer=object.__new__(Assets)
    renderer.reader=SimpleNamespace(read_resource=read)
    def source(role):
        sha=family['presentation'][role]['asset_sha']
        entry=next((a for a in family['assets'] if a['sha']==sha),{})
        return entry,engine.blob(sha)
    outputs={}
    for key,offset,role,canvas in (('unit_primary',4654,'shop',(256,256)),('shop',4754,'shop',(256,256)),('custom',4854,'capsule',(92,92))):
        entry,selected=source(role)
        path=raw[offset:offset+100].decode('utf-16-le').split('\0')[0]
        native=None
        if path:
            try:native=_png(renderer.sprite(path))
            except (OSError,ValueError,KeyError):pass
        mode=family['presentation'][role].get('source_mode','selected' if entry.get('source')=='upload' else 'native')
        if native is not None and mode=='native':
            outputs[key]=native
        else:
            image=Image.open(io.BytesIO(normalize_png(selected))).convert('RGBA')
            if native is not None:canvas=Image.open(io.BytesIO(native)).size
            fitted=ImageOps.contain(image,canvas,Image.Resampling.LANCZOS)
            result=Image.new('RGBA',canvas);result.alpha_composite(fitted,((canvas[0]-fitted.width)//2,(canvas[1]-fitted.height)//2))
            outputs[key]=_png(result)
    return outputs


def shop_image(engine,family,variant=0):
    images=unit_images(engine,family)
    # The ordinary eight-row shop resolves kind 0x55 +14, not unit +4754.
    # Keep its preview identical to the owned icon written by the compiler.
    return images['unit_primary'] if variant else _index_images(images)['index_owned']


def _index_images(images):
    from PIL import Image,ImageOps
    colored=Image.open(io.BytesIO(images['unit_primary'])).convert('RGBA')
    colored=ImageOps.contain(colored,(122,122),Image.Resampling.LANCZOS)
    icon=Image.new('RGBA',(122,122))
    icon.alpha_composite(colored,((122-colored.width)//2,(122-colored.height)//2))
    gray=ImageOps.grayscale(icon).convert('RGBA')
    gray.putalpha(icon.getchannel('A'))
    return {'index_owned':_png(icon),'index_missing':_png(gray)}


def publication_images(engine,family):
    images=unit_images(engine,family)
    unit=capsule_unit(engine,family)
    # Fixed cards share the capsule module's renderer; S custom cards receive
    # their frame in the original native custom selector, using unit+0x12F6.
    images['capsule_fixed']=capsule_card(engine,family) if unit['rank']<=3 else None
    images.update(_index_images(images))
    if any(not row.get('client_templates') for row in family.get('blueprints',[])):
        images.update(blueprint_images(engine,family))
    return images


def blueprint_images(engine,family):
    """Reuse capsule artwork inside the actual native blueprint presentation."""
    from manager.engines.capsules.capsule_assets import Assets
    unit=capsule_unit(engine,family)
    if engine.baseline.root is None:
        raise ValueError('生成设计图图片需要配套客户端的原生显示资源')
    renderer=Assets(engine.baseline.root,{}, {'units':[unit]})
    head=renderer._head_crop(unit['id'],0)
    preview=_alpha_resize(head,BLUEPRINT_PREVIEW_SIZE,cover=True)
    preview.alpha_composite(_native_blueprint_frame(str(engine.baseline.root)))
    return {'blueprint_preview':_png(preview),
            'blueprint_card':_png(_alpha_resize(head,(183,87),cover=False))}


def contract(engine,family):
    unit=capsule_unit(engine,family)
    return {'schema_version':GENERATOR_VERSION,'shop':{'generator':'native-shop-owned-index-icon','index_kind':85,'index_icon_offset':14,
        'normal_shop_canvas':[122,122],'alternate_list_offset':4654,
        'shop_2d_offset':4754,'model_renderer':'CServiceSecretStore::SetNomalItemView_Unit',
        'product_economics_owner':'shop_catalog'},
        'capsule':{'generator':'native-capsule-Assets.card' if unit['rank']<=3 else 'native-custom-candidate-controls',
            'kind':'fixed' if unit['rank']<=3 else 'custom','portrait_offset':4854,
            'rank':unit['rank'],'subgrade':unit['subgrade'],
            'runtime_frame':'native-0078C970-00C1EF40' if unit['rank']==4 else None},
        'thumbnail_is_game_screenshot':False}
