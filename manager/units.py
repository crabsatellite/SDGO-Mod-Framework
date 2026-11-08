"""Unit-family authoring behind the Manager's existing session and lifetime."""
from __future__ import annotations

import base64
import binascii
import copy
import hashlib
from pathlib import Path
import re
import threading
import time

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
    id, title = 'units', '机体导入（Beta）'
    fields = {'workspace': '机体制作工作区（可沿用已有目录）', 'client': '配套客户端目录',
              'server_ini': 'WorldServer INI（留空只生成离线发布物）',
              'snapshot': '原生定义快照目录', 'module_binary': '机体客户端 DLL（用于导出）',
              'archive_executable': '7-Zip 程序（可选）'}
    developer_fields = Provider.developer_fields | {'archive_executable'}

    def __init__(self, workspace):
        super().__init__(workspace)
        # The batch job keeps its own lock: provider requests hold self.lock, and
        # progress must stay readable while the job runs in its own thread.
        self._batch_lock = threading.Lock()
        self._batch = {'status': 'idle'}
        self._batch_thread = None

    def batch_state(self):
        with self._batch_lock:
            return {'batch': copy.deepcopy(self._batch), 'server_configured': bool(self.config.get('server_ini')),
                    'changes_applied': False}

    @staticmethod
    def _batch_folder(body):
        if not isinstance(body, dict) or set(body) - {'publication'}:
            raise ValueError('批量发布请求无效')
        folder = body.get('publication')
        if not isinstance(folder, str) or not folder.strip() or len(folder) > 1024 or '\0' in folder:
            raise ValueError('请选择批量发布目录')
        return folder.strip()

    def _batch_server_ini(self):
        ini = self.config.get('server_ini')
        if not ini:
            raise ValueError('请先在机体导入模块配置 WorldServer INI；批量阵容只能应用到已配置的服务器')
        return ini

    def start_batch(self, folder, wait=120.0):
        """Run the batch apply in its own thread; return once the stopped-server gate decided."""
        from owner.units import batch_server
        ini = self._batch_server_ini()
        with self._batch_lock:
            if self._batch.get('status') in ('starting', 'running'):
                raise ValueError('批量阵容正在应用，请等待当前任务结束')
            self._batch = {'status': 'starting', 'stage': 'gate', 'done': 0, 'total': 0, 'detail': '',
                           'started_at': time.time()}
        decided = threading.Event()

        def progress(event):
            with self._batch_lock:
                self._batch.update(event, status='running')
            if event.get('stage') != 'gate':
                decided.set()

        def run():
            try:
                result = batch_server.apply(folder, ini, progress)
            except ValueError as error:
                outcome = {'status': 'failed', 'error': str(error)[:1200]}
            except BaseException:
                import traceback
                try:
                    self.workspace.root.mkdir(parents=True, exist_ok=True)
                    with (self.workspace.root / 'manager-errors.log').open('a', encoding='utf-8') as stream:
                        stream.write(traceback.format_exc() + '\n')
                except OSError:
                    pass
                outcome = {'status': 'failed', 'error': '批量阵容应用未完成，请查看本机 Manager 日志；已提交的机体族可直接重新应用'}
            else:
                outcome = {'status': 'completed', 'result': result}
                try:  # make the applied families available to the capsule and shop editors
                    from owner.units import batch_registry
                    outcome['registered'] = batch_registry.register(self._units_root(), folder, ini)['families']
                except (ValueError, OSError) as error:
                    outcome['registration_error'] = str(error)[:400]
            with self._batch_lock:
                self._batch.update(outcome, finished_at=time.time())
            decided.set()

        # Not a daemon: closing Manager waits for the current family transaction.
        thread = threading.Thread(target=run, name='units-batch-apply')
        self._batch_thread = thread
        thread.start()
        decided.wait(wait)
        state = self.batch_state()
        if state['batch'].get('status') == 'failed' and state['batch'].get('stage') in ('gate', 'check'):
            raise ValueError(state['batch']['error'])
        return state

    def _units_root(self):
        return Path(self.config.get('workspace') or self.workspace.root / 'data/units')

    def batch_rows(self, ini=None):
        """Registered batch-roster families for the configured database (Manager project row shape)."""
        from owner.units import batch_registry
        ini = ini or self.config.get('server_ini')
        rows = batch_registry.rows(self._units_root(), ini) if ini else []
        for row in rows:
            query = f'unit={row["unit_id"]}&revision={row["revision"]}'
            row.update(shop_image_sha256=None, shop_source_sha256=None, capsule_image_sha256=None,
                       shop_image_url=f'/api/providers/units/api/units/batch/image?{query}&role=shop',
                       capsule_image_url=f'/api/providers/units/api/units/batch/image?{query}&role=capsule')
        return rows

    def batch_image(self, row, role='capsule'):
        from owner.units import batch_registry
        # Baseline-identical sprites are not shipped by the publisher; the matching client's archive holds them.
        client = self.config.get('client')
        return batch_registry.sprite_png(row, {'shop': 'shop_sprite', 'capsule': 'capsule_sprite'}[role],
                                         baseline_client=client if client and Path(client).is_dir() else None)

    def register_batch(self, folder):
        from owner.units import batch_registry
        return {'registered': batch_registry.register(self._units_root(), folder, self._batch_server_ini()),
                'publications': batch_registry.load(self._units_root()), 'changes_applied': False,
                'message': '批量阵容已登记：扭蛋机、活动机和商城可以选用这些机体；未改变任何机台、商品或游戏服务器'}

    def _batch_request(self, method, path, body):
        from owner.units import batch_server, batch_registry
        from urllib.parse import urlsplit, parse_qs
        split = urlsplit(path)
        if method == 'GET' and split.path == '/api/units/batch/image':
            query = parse_qs(split.query)
            unit, revision, role = (query.get(name, [''])[0] for name in ('unit', 'revision', 'role'))
            row = next((r for r in self.batch_rows() if str(r['unit_id']) == unit and r['revision'] == revision), None)
            if row is None or role not in ('shop', 'capsule'):
                raise ValueError('批量机体图片不存在或版本已变化')
            return Binary(self.batch_image(row, role), 'image/png')
        if method == 'GET' and path == '/api/units/batch':
            state = self.batch_state()
            state['publications'] = batch_registry.load(self._units_root())
            return state
        if method == 'POST' and path == '/api/units/batch/register':
            return self.register_batch(self._batch_folder(body))
        if method == 'POST' and path == '/api/units/batch/preflight':
            return {'preflight': batch_server.preflight(self._batch_folder(body), self._batch_server_ini()), 'changes_applied': False}
        if method == 'POST' and path == '/api/units/batch/readback':
            return {'readback': batch_server.readback(self._batch_folder(body), self._batch_server_ini()), 'changes_applied': False}
        if method == 'POST' and path == '/api/units/batch/apply':
            return self.start_batch(self._batch_folder(body))
        return None

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
            root = Path(__file__).resolve().parents[1]
            candidates=[root/'unit-import/unit_catalog.dll',root/'build/Release/unit_catalog.dll']
            if c.get('client'):candidates.append(Path(c['client'])/'sdgo/mods/unit_catalog/unit_catalog.dll')
            binary = c.get('module_binary') or next((str(p) for p in candidates if p.is_file()),None)
            self.engine = Workspace(c.get('workspace') or self.workspace.root/'data/units',
                c.get('client'), c.get('snapshot'), c.get('server_ini'), binary,
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
                server_result=draft.get('published_server_result',draft.get('server_result',{}))
                from .operations import activation
                active=activation(self.config['server_ini'],server_result.get('activation_id'))if self.config.get('server_ini')else{'status':'offline'}
                result.append({'key': 'unit:'+str(root), 'provider': 'unit_catalog', 'kind': 'unit','activation':active,
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
            # A Manager project owns its own family keys; a batch family never shadows one.
            owned = {row['family_key'] for row in result} | {row['unit_id'] for row in result}
            result += [row for row in self.batch_rows()
                       if row['family_key'] not in owned and row['unit_id'] not in owned]
            return result

    def for_server(self, ini):
        # A definition in another server must never silently become purchasable here.
        if not ini or not self.config.get('server_ini'):
            return []
        with self.lock:
            from owner.units.repository import database_target_key
            target = database_target_key(Path(ini))
            return [r for r in self.registrations() if r['server_applied'] and r['server_target'] == target and r['activation']['status']in('active','legacy')]

    def acquisition_preview(self):
        from owner.units.acquisition import route_roots
        from owner.units.acquisition_host import read_configuration,activity_grants
        from owner.units.model import revision
        from .engines.capsules.capsule_catalog import acquisition
        capsule=self.workspace.providers['capsules']
        ini=self.config.get('server_ini')
        if not ini or not capsule.config.get('catalog'):
            return {'available':False,'changes_applied':False,'message':'请先配置本机体宿主与扭蛋目录，才能检查获取闭环'}
        rows=self.for_server(ini)
        if not rows:return {'available':False,'changes_applied':False,'message':'请先确认并应用机体定义，再检查其获取途径'}
        from owner.units.repository import database_target_key
        if not capsule.config.get('server_ini') or database_target_key(Path(ini))!=database_target_key(Path(capsule.config['server_ini'])):
            raise ValueError('获取途径和机体导入必须使用同一宿主数据库')
        app=capsule.get_engine()
        with app.store.lock:catalog,_=app.store.read()
        configuration=read_configuration(ini);tables=configuration['tables']
        grants=activity_grants(self.config['client'],tables)
        reach=acquisition(None,catalog,table_reader=lambda db,name:tables[(db,name)],source_grants=grants)
        result=route_roots(rows,reach,tables[('GOnlineGame','tbl_MixTreeFile')],family_revision=revision(self.get_engine().read()))
        result.update(available=True,configuration_sha256=configuration['configuration_sha256'],
                      captured_at=configuration['captured_at'],catalog_revision=revision(catalog))
        result['limitations']=list(dict.fromkeys([*result['limitations'],*reach.get('limitations',[])]))
        return result

    def project_library(self):
        from owner.units.project import normalize_metadata,shared_media
        from owner.units.model import content_revision
        engine=self.get_engine();items=[]
        with engine.lock:
            for draft in engine.read()['drafts'].values():
                if draft['status']=='disabled':continue
                host_applied=bool(draft.get('published') and draft.get('published_server_result',draft.get('server_result',{})).get('applied'))
                items.append({'project_id':draft['id'],'family_key':draft['family_key'],'name':draft['name'],
                    'root_unit_id':draft.get('root_unit_id'),'metadata':normalize_metadata(draft.get('metadata',{})),
                    'revision':content_revision(draft),'usable_for_authoring':True,'native_ready':False,
                    'host_definition_applied':host_applied,'requires_runtime_validation':True,
                    'media':{role:{**entry,'image_url':'/api/providers/units/api/units/assets/'+entry['asset_sha']}
                             for role,entry in shared_media(draft).items()}})
        return {'protocol':'sdgo.unit-project-library','schema_version':1,'items':items,
                'changes_applied':False,'grants_units':False}

    def request(self, method, path, body, headers):
        if path == '/api/units/batch' or path.startswith('/api/units/batch/'):
            result = self._batch_request(method, path, body)
            if result is None: raise ValueError('机体操作未登记')
            return result
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
                if path == '/api/units/acquisition': return self.acquisition_preview()
                if path == '/api/units/projects': return self.project_library()
                project_export=re.fullmatch(r'/api/units/drafts/([0-9a-f]{32})/project',path)
                if project_export:
                    from owner.units.project import export_project
                    with engine.lock:
                        raw=export_project(engine,engine._get(engine.read(),project_export[1]))
                    return Binary(raw,'application/zip','SDGO-Unit-Project.zip')
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
            if method=='POST' and path=='/api/units/projects':return engine.create_project(body,expected)
            if method=='POST' and path=='/api/units/preview-cleanup':
                from owner.units.native_preview import cleanup
                engine.check(expected)
                return cleanup(engine)
            if method == 'POST' and path == '/api/units/inspect':
                return engine.inspect(*decode_upload(body), expected)
            match = re.fullmatch(r'/api/units/drafts/([0-9a-f]{32})(?:/(asset|definitions|native-content|native-server-definition|confirm|drop-missing-skills|native-preview|launch-preview))?', path)
            if match:
                identity, action = match.groups()
                if method == 'PUT' and action is None: return engine.update(identity, body, expected)
                if method == 'DELETE' and action is None: return engine.disable(identity, expected)
                if method == 'POST' and action == 'asset':
                    return engine.upload_asset(identity, *decode_upload(body), body.get('role'), expected)
                if method == 'POST' and action == 'definitions':
                    return engine.upload_definitions(identity, *decode_upload(body), expected)
                if method == 'POST' and action == 'native-content':
                    return engine.upload_native_content(identity,*decode_upload(body),expected,body.get('replace_manifest'))
                if method == 'POST' and action == 'native-server-definition':
                    return engine.edit_native_server_definition(identity,body,expected)
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


def sync_capsule_units(provider, expected, unit_ids=None):
    """Explicitly add approved definitions/images; never change machine economics.

    unit_ids limits the synchronization to the listed roots (a batch roster can hold hundreds
    of families while only a few go into capsule machines); None synchronizes every root.
    """
    from .engines.capsules.capsule_catalog import grade
    if unit_ids is not None and (not isinstance(unit_ids, list) or not unit_ids
                                 or any(type(uid) is not int for uid in unit_ids) or len(unit_ids) > 4096):
        raise ValueError('同步机体清单无效')
    source = provider.workspace.providers['units']
    engine = provider.get_engine()
    with engine.store.lock, source.lock:
        rows = source.for_server(provider.config.get('server_ini'))
        if unit_ids is not None:
            wanted = set(unit_ids)
            missing = wanted - {row['unit_id'] for row in rows}
            if missing:
                raise ValueError('这些机体尚未登记到同一服务器：' + '、'.join(map(str, sorted(missing)[:10])))
            rows = [row for row in rows if row['unit_id'] in wanted]
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
            if row.get('batch'):
                png = source.batch_image(row, 'capsule')
            elif row['rank']==4:
                from owner.units.presentation import capsule_source
                published=next(d['published'] for d in source.get_engine().read()['drafts'].values()
                               if d.get('published',{}).get('root_unit_id')==uid)
                png=capsule_source(source.get_engine(),published)
            else:
                png = source.get_engine().blob(row['capsule_image_sha256'])
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


def import_project_media(provider,body):
    """Copy reviewed author media into the portable capsule authoring catalog."""
    from owner.units import model
    from owner.units.project import shared_media,normalize_metadata
    if not isinstance(body,dict) or set(body)-{'project_id','role','asset_sha','source_revision','expected_revision'}:
        raise ValueError('素材引用请求无效')
    identity=body.get('project_id')
    if not isinstance(identity,str) or not re.fullmatch('[0-9a-f]{32}',identity):raise ValueError('请选择资料项目')
    source=provider.workspace.providers['units'].get_engine();app=provider.get_engine()
    with app.store.lock,source.lock:
        draft=source._get(source.read(),identity)
        if draft['status']=='disabled':raise ValueError('该资料项目已停用')
        if body.get('source_revision')!=model.content_revision(draft):raise ValueError('项目资料已变化，请刷新素材库后重选')
        role=body.get('role');entry=shared_media(draft).get(role)
        if not entry or entry['asset_sha']!=body.get('asset_sha'):raise ValueError('项目素材已变化，请刷新后重选')
        raw=source.blob(entry['asset_sha'])
        if role=='activity_poster':
            from owner.collection_capsule.client import normalize_activity_cover
            raw=normalize_activity_cover(raw)
        catalog,previous=app.store.read();app.store._expect(catalog,body.get('expected_revision'))
        candidate=copy.deepcopy(catalog)
        key=draft['family_key']+':'+role
        value={'key':key,'project_key':draft['family_key'],'project_revision':model.content_revision(draft),
            'name':draft['name'],'role':role,'unit_id':draft.get('root_unit_id') or draft.get('metadata',{}).get('declared_unit_id'),
            'asset_sha256':hashlib.sha256(raw).hexdigest(),'source_asset_sha256':entry['asset_sha'],
            'original_sha256':entry['source_sha'],
            'png_base64':base64.b64encode(raw).decode('ascii'),'metadata':normalize_metadata(draft.get('metadata',{}))}
        target=candidate.setdefault('project_assets',[])
        existing=next((row for row in target if row['key']==key),None)
        if existing is None:target.append(value)
        else:target[target.index(existing)]=value
        for draft_machine in candidate.get('activity_drafts',[]):
            if draft_machine.get('artwork_key')==key:
                draft_machine['artwork_sha256']=value['asset_sha256']
                draft_machine['project_revision']=value['project_revision']
                draft_machine['artwork_review_required']=True
        for machine in candidate.get('activity_machines',[]):
            if machine.get('artwork_key')==key:
                machine['artwork_sha256']=value['asset_sha256']
                machine['revision']=model.revision({k:v for k,v in machine.items() if k!='revision'})
        from .engines.capsules.capsule_catalog import validate
        validate(candidate)
        if candidate!=catalog:app.store._save(candidate,previous)
    return {'catalog_revision':model.revision(candidate),'project_asset_key':key,
        'changes_applied':False,'native_units_registered':False,
        'message':'资料与图片已复制到扭蛋制作项目；尚未改变奖池、价格或上架状态'}


def _poster_asset(key,project_key,project_revision,name,unit_id,raw,metadata,source_sha=None):
    from owner.units.project import normalize_metadata
    from owner.collection_capsule.client import normalize_activity_cover
    normalized=normalize_activity_cover(raw)
    return {'key':key,'project_key':project_key,'project_revision':project_revision,'name':name,'role':'activity_poster',
            'unit_id':unit_id,'asset_sha256':hashlib.sha256(normalized).hexdigest(),
            'source_asset_sha256':source_sha or hashlib.sha256(raw).hexdigest(),'original_sha256':hashlib.sha256(raw).hexdigest(),
            'png_base64':base64.b64encode(normalized).decode('ascii'),'metadata':normalize_metadata(metadata or {})}


def _put_poster(candidate,value):
    """Store a poster asset and repoint every draft/machine that uses its key."""
    from owner.units import model
    target=candidate.setdefault('project_assets',[])
    existing=next((row for row in target if row['key']==value['key']),None)
    if existing is None:target.append(value)
    else:target[target.index(existing)]=value
    for draft_machine in candidate.get('activity_drafts',[]):
        if draft_machine.get('artwork_key')==value['key']:
            draft_machine['artwork_sha256']=value['asset_sha256'];draft_machine['artwork_review_required']=True
    for machine in candidate.get('activity_machines',[]):
        if machine.get('artwork_key')==value['key']:
            machine['artwork_sha256']=value['asset_sha256']
            machine['revision']=model.revision({k:v for k,v in machine.items() if k!='revision'})


def store_activity_poster(provider,body):
    """Upload or replace the poster of an activity draft or machine; pool, price and number stay unchanged."""
    from owner.units import model
    from .engines.capsules.capsule_catalog import validate
    if not isinstance(body,dict) or set(body)!={'expected_revision','id','filename','data'}:
        raise ValueError('活动海报上传请求无效')
    _,raw=decode_upload(body)
    app=provider.get_engine()
    with app.store.lock:
        catalog,previous=app.store.read();app.store._expect(catalog,body['expected_revision'])
        row=next((r for r in catalog.get('activity_machines',[])+catalog.get('activity_drafts',[]) if r['id']==body['id']),None)
        if row is None:raise ValueError('活动机台不存在')
        key=row.get('artwork_key') or row['project_key']+':activity_poster'
        old=next((r for r in catalog.get('project_assets',[]) if r['key']==key),{})
        candidate=copy.deepcopy(catalog)
        value=_poster_asset(key,row['project_key'],old.get('project_revision') or row.get('project_revision',''),
                            old.get('name') or row.get('name',''),row['unit_id'],raw,old.get('metadata'))
        value['uploaded']=True
        _put_poster(candidate,value)
        validate(candidate)
        if candidate!=catalog:app.store._save(candidate,previous)
    return {'catalog_revision':model.revision(candidate),'project_asset_key':key,'changes_applied':False,
            'message':'活动海报已更换；编译并应用后游戏内才会显示新海报'}


def create_project_activity_draft(provider,body):
    """Reviewable dedicated machine; disabled until paired native application.

    The root is a Manager unit project (project_id) or a registered batch-roster family (family_key).
    A batch family without a poster gets a generated stand-in poster that the owner replaces later.
    """
    from owner.units import model
    from owner.units.project import normalize_metadata
    from owner.units.activity_patterns import build_pattern
    from owner.units.acquisition_host import read_configuration,activity_grants
    from owner.units.repository import database_target_key
    from .unit_activity import resolve_family
    if not isinstance(body,dict) or set(body)-{'project_id','family_key','source_revision','expected_revision','pattern_id'}:
        raise ValueError('活动机台草稿请求无效')
    if bool(body.get('project_id'))==bool(body.get('family_key')):
        raise ValueError('请选择一个机体项目或批量阵容机体')
    units=provider.workspace.providers['units'];engine=units.get_engine()
    ini=units.config.get('server_ini');capsule_ini=provider.config.get('server_ini')
    if not ini or not capsule_ini or database_target_key(Path(ini))!=database_target_key(Path(capsule_ini)):
        raise ValueError('活动机台与机体资料必须配置同一宿主；先生成草稿，不会更改宿主')
    app=provider.get_engine()
    with app.store.lock,engine.lock:
        if body.get('project_id'):
            source=engine._get(engine.read(),body.get('project_id'))
            if source['status']=='disabled' or model.content_revision(source)!=body.get('source_revision'):
                raise ValueError('机体项目已变化或停用，请刷新后重选')
            if not source.get('published') or not source.get('published_server_result',source.get('server_result',{})).get('applied'):
                raise ValueError('先完成机体的宿主登记，再生成以该机体为奖品的活动草稿；图片仍可提前复用')
            family=resolve_family(units,source['family_key'],None,ini)
        else:
            family=resolve_family(units,body['family_key'],None,ini)
            if family['revision']!=body.get('source_revision'):
                raise ValueError('批量阵容已变化，请刷新后重选')
        root=family['root_unit_id']
        if family['rank']!=4:raise ValueError('S级以下请检查设计图闭环或使用普通扭蛋机')
        catalog,previous=app.store.read();app.store._expect(catalog,body.get('expected_revision'))
        configuration=read_configuration(ini);tables=configuration['tables']
        listed={r['witness']['machine_id'] for r in activity_grants(units.config['client'],tables)}
        configuration_units=list(engine.repository.catalog['units'].values())
        preset=build_pattern(body.get('pattern_id','gp_rare'),
            {'unit_id':root,'rank':4,'subgrade':family['subgrade']},family['category_id'],
            units=configuration_units,categories=tables[('GOnlineGame','tbl_ItemCategory')],
            prices=tables[('GOnlineGame','tbl_ItemMachinePrice')],pool=tables[('GOnlineGame','tbl_ItemMachineList')],
            listed_machines=listed,preferences=catalog.get('activity_preferences'),
            excluded_machine_ids={r['id'] for r in catalog.get('activity_machines',[])+catalog.get('activity_drafts',[])})
        balance=preset['balance']
        candidate=copy.deepcopy(catalog)
        drafts=candidate.setdefault('activity_drafts',[])
        existing=next((r for r in drafts if r['project_key']==family['family_key'] and r['unit_id']==root),None)
        occupied={r['MachineID'] for r in tables[('GOnlineGame','tbl_ItemMachinePrice')]}|{r['id'] for r in drafts}
        from owner.units.activity_ids import allocate_activity_id
        authored_ids={r['id'] for r in drafts+candidate.get('activity_machines',[])}
        occupied.update(authored_ids)
        machine_id=existing['id'] if existing else allocate_activity_id(occupied,listed=listed|authored_ids)
        poster_key=family['family_key']+':activity_poster'
        artwork=next((r for r in candidate.get('project_assets',[]) if r['key']==poster_key),None)
        if not artwork and body.get('family_key'):
            from owner.units.activity_poster import placeholder
            row=next(r for r in units.batch_rows(ini) if r['family_key']==family['family_key'])
            artwork=_poster_asset(poster_key,family['family_key'],family['revision'],family['name'],root,
                                  placeholder(units.batch_image(row,'capsule'),family['name']),{})
            artwork['placeholder']=True
            _put_poster(candidate,artwork)
        if not artwork:raise ValueError('请先将该项目的活动海报存入扭蛋制作素材')
        draft={'schema_version':1,'id':machine_id,'project_key':family['family_key'],
            'project_revision':family['revision'],'unit_id':root,'unit_category_id':family['category_id'],
            'name':family['name']+'活动扭蛋机','enabled':False,'status':'draft',
            'artwork_key':poster_key,'artwork_sha256':artwork['asset_sha256'],
            'metadata':normalize_metadata(family.get('metadata',{})),
            'currency_type':preset['currency_type'],'price':preset['price'],
            'pattern_id':preset['pattern_id'],'pool':preset['pool'],'template_machine_id':preset['template_machine_id'],
            'artwork_review_required':True,
            'balance':balance,'host_configuration_sha256':configuration['configuration_sha256'],
            'changes_applied':False}
        if existing:drafts[drafts.index(existing)]=draft
        else:drafts.append(draft)
        from .engines.capsules.capsule_catalog import validate
        validate(candidate)
        if candidate!=catalog:app.store._save(candidate,previous)
    return {'catalog_revision':model.revision(candidate),'activity_draft':draft,'changes_applied':False,
        'message':'独立活动机草稿已生成：所选价格与奖池模板、项目海报和资料已绑定；尚未编译安装或上架'}
