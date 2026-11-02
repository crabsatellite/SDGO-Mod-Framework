"""Unit-family authoring behind the Manager's existing session and lifetime."""
from __future__ import annotations

import base64
import binascii
import copy
import hashlib
from pathlib import Path
import re

from .providers import Provider, Binary
from . import storage


def decode_upload(body):
    name, encoded = body.get('filename'), body.get('data')
    if not isinstance(name, str) or len(name) > 256 or not isinstance(encoded, str):
        raise ValueError('请选择有效文件')
    try:
        raw = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error):
        raise ValueError('上传数据不是有效的 Base64') from None
    if not 0 < len(raw) <= 32 * 1024 * 1024:
        raise ValueError('文件为空或超过 32 MiB')
    return name, raw


class Units(Provider):
    id, title = 'units', '机体导入'
    fields = {'workspace': '机体制作工作区（可沿用已有目录）', 'client': '配套客户端目录',
              'server_ini': 'WorldServer INI（留空只生成离线发布物）',
              'snapshot': '原生定义快照目录', 'module_binary': '机体客户端 DLL（用于导出）',
              'archive_executable': '7-Zip 程序（可选）'}
    developer_fields = Provider.developer_fields | {'archive_executable'}

    def _published_shop(self, engine, family):
        from owner.units.model import content_revision
        from owner.units.presentation import GENERATOR_VERSION,shop_image
        key=(id(engine),content_revision(family),GENERATOR_VERSION)
        cache=getattr(self,'_shop_previews',None)
        if cache is None:self._shop_previews=cache={}
        if key not in cache:
            raw=shop_image(engine,family)
            if len(cache)>=128:cache.pop(next(iter(cache)))
            cache[key]=(raw,hashlib.sha256(raw).hexdigest())
        return cache[key]

    def get_engine(self):
        if self.engine is None:
            from owner.units.workspace import Workspace
            c = self.config
            if not c.get('client'):
                raise ValueError('请先选择配套客户端目录')
            root = Path(__file__).resolve().parents[1]
            binary = c.get('module_binary') or next((str(p) for p in (
                root/'unit-import/unit_catalog.dll',root/'build/Release/unit_catalog.dll',
                Path(c['client'])/'sdgo/mods/unit_catalog/unit_catalog.dll') if p.is_file()), None)
            self.engine = Workspace(c.get('workspace') or self.workspace.root/'data/units',
                c['client'], c.get('snapshot'), c.get('server_ini'), binary,
                archive_executable=c.get('archive_executable'))
        return self.engine

    def registrations(self):
        """Only the last approved publication is visible to downstream editors."""
        if self.engine is None and not self.config.get('client'):
            return []
        engine = self.get_engine()
        with engine.lock:
            result = []
            for draft in engine.read()['drafts'].values():
                family = draft.get('published')
                if not family or draft['status'] == 'disabled':
                    continue
                root = family['root_unit_id']
                form = next(r for r in family['forms'] if r['id'] == root)
                from owner.units.model import content_revision
                _,shop_sha=self._published_shop(engine,family)
                result.append({'key': 'unit:'+str(root), 'provider': 'unit_catalog', 'kind': 'unit',
                    'unit_id': root, 'category_id': family['category_id'], 'family_key': family['family_key'],
                    'revision': content_revision(family), 'display_name': family['name'],
                    'forms': [r['id'] for r in family['forms']],
                    'rank': form['server']['UnitGradeNumber'], 'subgrade': form['server']['UnitGradeNumber2'],
                    'shop_sprite': f'sprs/u{root}o.spr', 'secret_store_sprite':f'sprs/u{root}s.spr', 'unit_primary_sprite':f'sprs/u{root}p.spr',
                    'capsule_sprite':f'sprs/u{root}{"c" if form["server"]["UnitGradeNumber"]<=3 else "i"}.spr',
                    'custom_candidate_sprite':f'sprs/u{root}i.spr',
                    'shop_image_sha256': shop_sha, 'shop_source_sha256':family['presentation']['shop']['asset_sha'],
                    'capsule_image_sha256': family['presentation']['capsule']['asset_sha'],
                    'server_applied': bool(draft.get('published_server_result', draft.get('server_result', {})).get('applied')),
                    'server_target': draft.get('published_server_result', draft.get('server_result', {})).get('server_target'),
                    'shop_image_url': f'/api/providers/units/api/units/published/{draft["id"]}/presentation/shop?revision={content_revision(family)}&image={shop_sha}',
                    'capsule_image_url': f'/api/providers/units/api/units/assets/{family["presentation"]["capsule"]["asset_sha"]}'})
            return result

    def for_server(self, ini):
        # A definition in another server must never silently become purchasable here.
        if not ini or not self.config.get('server_ini'):
            return []
        with self.lock:
            from owner.units.repository import database_target_key
            target = database_target_key(Path(ini))
            return [r for r in self.registrations() if r['server_applied'] and r['server_target'] == target]

    def request(self, method, path, body, headers):
        result=self._request(method,path,body,headers)
        if isinstance(result,dict) and 'draft' in result:
            from owner.units.native_preview import status
            result['draft']['native_preview']=status(self.get_engine(),result['draft']['id'])
            d=result['draft']
            try:
                from owner.units.presentation import contract
                d['presentation_contract']=contract(self.get_engine(),d)
            except (ValueError,KeyError,StopIteration):d['presentation_contract']=None
            d['presentation_outputs']={role:f'/api/units/drafts/{d["id"]}/presentation/{role}?revision={d["content_revision"]}'
                                       for role in ('shop','capsule')}
        return result

    def _request(self, method, path, body, headers):
        from owner.units.model import RevisionConflict
        engine = self.get_engine()
        try:
            if method == 'GET':
                from urllib.parse import urlsplit,parse_qs
                split=urlsplit(path)
                published=re.fullmatch(r'/api/units/published/([0-9a-f]{32})/presentation/shop',split.path)
                if published:
                    with engine.lock:
                        draft=engine._get(engine.read(),published[1]);family=draft.get('published')
                        if not family or draft['status']=='disabled':raise ValueError('已确认的商城图标不存在')
                        from owner.units.model import content_revision
                        query=parse_qs(split.query)
                        raw,digest=self._published_shop(engine,family)
                        if query.get('revision',[''])[0]!=content_revision(family) or query.get('image',[''])[0]!=digest:
                            raise ValueError('已确认的商城图标版本已变化，请刷新商品列表')
                        return Binary(raw,'image/png')
                presentation=re.fullmatch(r'/api/units/drafts/([0-9a-f]{32})/presentation/(shop|capsule)',split.path)
                if presentation:
                    from owner.units import presentation as generator
                    with engine.lock:
                        draft=engine._get(engine.read(),presentation[1])
                        from owner.units.model import content_revision
                        if parse_qs(split.query).get('revision',[''])[0]!=content_revision(draft):
                            raise ValueError('展示内容版本已变化，请保存并刷新生成结果')
                        raw=generator.shop_image(engine,draft) if presentation[2]=='shop' else (
                            generator.capsule_card(engine,draft) if generator.capsule_unit(engine,draft)['rank']<=3
                            else generator.capsule_source(engine,draft))
                        return Binary(raw,'image/png')
                if path == '/api/units': return engine.public()
                if path == '/api/units/registrations':
                    return {'provider': 'unit_catalog', 'items': self.registrations(),
                            'grants_units': False, 'creates_shop_products': False, 'creates_machine_slots': False}
                match = re.fullmatch(r'/api/units/assets/([0-9a-f]{64})', path)
                if match:
                    sha = match[1]
                    with engine.lock:
                        allowed = {a['sha'] for d in engine.read()['drafts'].values()
                                   for source in (d, d.get('published', {}))
                                   for a in source.get('assets', []) if a['kind'] == 'image'}
                        if sha not in allowed: raise ValueError('预览图片不存在')
                        return Binary(engine.blob(sha), 'image/png')
                match = re.fullmatch(r'/api/units/drafts/([0-9a-f]{32})(?:/(export|server-plan|native-preview))?', path)
                if match:
                    identity, action = match.groups()
                    if action == 'native-preview':
                        from owner.units.native_preview import status
                        return status(engine,identity)
                    if action == 'export': return Binary(engine.export(identity), 'application/zip', 'SDGO-UnitImport-Client.zip')
                    if action == 'server-plan':
                        with engine.lock:
                            draft = engine._get(engine.read(), identity)
                            if draft['status'] != 'confirmed': raise ValueError('请先确认当前版本')
                            return Binary(storage.json_bytes(engine.plan(draft)), 'application/json', 'unit-import-server-plan.json')
                    return engine.public(identity)
            expected = headers.get('X-Catalog-Revision', '')
            if method=='POST' and path=='/api/units/preview-cleanup':
                from owner.units.native_preview import cleanup
                engine.check(expected)
                return cleanup(engine)
            if method == 'POST' and path == '/api/units/inspect':
                return engine.inspect(*decode_upload(body), expected)
            match = re.fullmatch(r'/api/units/drafts/([0-9a-f]{32})(?:/(asset|definitions|confirm|drop-missing-skills|native-preview|launch-preview))?', path)
            if match:
                identity, action = match.groups()
                if method == 'PUT' and action is None: return engine.update(identity, body, expected)
                if method == 'DELETE' and action is None: return engine.disable(identity, expected)
                if method == 'POST' and action == 'asset':
                    return engine.upload_asset(identity, *decode_upload(body), body.get('role'), expected)
                if method == 'POST' and action == 'definitions':
                    return engine.upload_definitions(identity, *decode_upload(body), expected)
                if method == 'POST' and action == 'confirm':
                    from owner.units.native_preview import status
                    from owner.units import presentation as generator
                    engine.check(expected)
                    draft=engine._get(engine.read(),identity)
                    from owner.units.model import content_revision
                    if (body.get('preview_revision')!=content_revision(draft) or
                            body.get('shop') is not True or body.get('capsule') is not True):
                        raise ValueError('请重新核对当前版本的商城和扭蛋图标并逐项确认')
                    problems=[row for row in engine.requirements(draft) if row['level']!='warning']
                    if problems:raise ValueError('仍有必填或关键缺项：'+problems[0]['message'])
                    images={'shop':generator.shop_image(engine,draft),
                            'capsule':generator.capsule_card(engine,draft) if generator.capsule_unit(engine,draft)['rank']<=3
                                      else generator.capsule_source(engine,draft)}
                    # Reviewing generated icons is distinct from a native game
                    # run. Only genuine optional receipts may claim native UI.
                    body={key:value for key,value in body.items() if key not in ('_native_ui','_presentation_review')}
                    body['_presentation_review']={'method':'shared_asset_generators',
                        'generated_sha256':{role:hashlib.sha256(raw).hexdigest() for role,raw in images.items()}}
                    proof=status(engine,identity)
                    if proof.get('ready'):
                        body['_native_ui']={'method':'existing_game_ui_calls','content_revision':proof.get('content_revision'),
                                            'shop':True,'capsule':True}
                    return engine.confirm(identity, body, expected)
                if method == 'POST' and action == 'drop-missing-skills':
                    return engine.drop_missing_skills(identity, body.get('skill_ids'), expected)
                if method == 'POST' and action in ('native-preview','launch-preview'):
                    from owner.units.native_preview import prepare,launch,StartupEvent
                    engine.check(expected)
                    if action=='launch-preview':self.workspace.services.resource('units.startup_gate',StartupEvent)
                    return prepare(engine,identity) if action=='native-preview' else launch(engine,identity)
            raise ValueError('机体操作未登记')
        except RevisionConflict as error:
            raise storage.Conflict(str(error)) from None


def sync_capsule_units(provider, expected):
    """Explicitly add approved definitions/images; never change machine economics."""
    from .engines.capsules.capsule_catalog import grade
    source = provider.workspace.providers['units']
    engine = provider.get_engine()
    with engine.store.lock, source.lock:
        rows = source.for_server(provider.config.get('server_ini'))
        if not rows:
            raise ValueError('没有已应用到同一 WorldServer 的机体发布。请先确认机体导入并选择相同服务器 INI')
        catalog, raw = engine.store.read()
        engine.store._expect(catalog, expected)
        candidate = copy.deepcopy(catalog)
        by_id = {u['id']: u for u in candidate['units']}
        for row in rows:
            uid = row['unit_id']
            old = by_id.get(uid)
            if old and old.get('unit_import', {}).get('family_key') != row['family_key']:
                raise ValueError(f'扭蛋目录已有非本机体模块拥有的编号：{uid}')
            png = source.get_engine().blob(row['capsule_image_sha256'])
            if row['rank']==4:
                from owner.units.presentation import capsule_source
                published=next(d['published'] for d in source.get_engine().read()['drafts'].values()
                               if d.get('published',{}).get('root_unit_id')==uid)
                png=capsule_source(source.get_engine(),published)
            import hashlib
            image_sha=hashlib.sha256(png).hexdigest()
            unit = {'id': uid, 'name': row['display_name'], 'rank': row['rank'], 'subgrade': row['subgrade'],
                    'grade': grade({'UnitGradeNumber': row['rank'], 'UnitGradeNumber2': row['subgrade']}),
                    'eligible': True, 'transform_to': 0, 'capsule_sprites': [],
                    'unit_import': {'family_key': row['family_key'], 'revision': row['revision'],
                        'category_id': row['category_id'], 'image_sha256': image_sha,
                        'image_png': base64.b64encode(png).decode('ascii')}}
            if old:
                candidate['units'][candidate['units'].index(old)] = unit
            else: candidate['units'].append(unit)
        if candidate != catalog: engine.store._save(candidate, raw)
    return {**engine.bootstrap(), 'unit_import_count': len(rows),
            'message': '已登记机体与头像；机台槽位、价格和概率保持原配置。请在机台编辑器选择机体后编译并应用'}
