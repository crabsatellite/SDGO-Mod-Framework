"""Presentation generators shared by authoring previews and publication."""
from __future__ import annotations
import base64
import io
from .assets import normalize_png


def _png(image):
    stream=io.BytesIO();image.save(stream,'PNG');return stream.getvalue()


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
    return unit_images(engine,family)['unit_primary' if variant else 'shop']


def publication_images(engine,family):
    images=unit_images(engine,family)
    unit=capsule_unit(engine,family)
    # Fixed cards share the capsule module's renderer; S custom cards receive
    # their frame in the original native custom selector, using unit+0x12F6.
    images['capsule_fixed']=capsule_card(engine,family) if unit['rank']<=3 else None
    from PIL import Image,ImageOps
    colored=Image.open(io.BytesIO(images['unit_primary'])).convert('RGBA')
    colored=ImageOps.contain(colored,(122,122),Image.Resampling.LANCZOS)
    icon=Image.new('RGBA',(122,122))
    icon.alpha_composite(colored,((122-colored.width)//2,(122-colored.height)//2))
    gray=ImageOps.grayscale(icon).convert('RGBA')
    gray.putalpha(icon.getchannel('A'))
    images['index_owned']=_png(icon)
    images['index_missing']=_png(gray)
    return images


def contract(engine,family):
    unit=capsule_unit(engine,family)
    return {'schema_version':2,'shop':{'generator':'native-unit-sprite-consumers','alternate_list_offset':4654,
        'shop_2d_offset':4754,'model_renderer':'CServiceSecretStore::SetNomalItemView_Unit',
        'product_economics_owner':'shop_catalog'},
        'capsule':{'generator':'native-capsule-Assets.card' if unit['rank']<=3 else 'native-custom-candidate-controls',
            'kind':'fixed' if unit['rank']<=3 else 'custom','portrait_offset':4854,
            'rank':unit['rank'],'subgrade':unit['subgrade'],
            'runtime_frame':'native-0078C970-00C1EF40' if unit['rank']==4 else None},
        'thumbnail_is_game_screenshot':False}
