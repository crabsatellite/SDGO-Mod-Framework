import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
import functools
from . import assets, model, server, presets, legacy_pack, native_resources, runtime


def _installed_upload(root, identity):
    asset_root = Path(root) / "sdgo/mods/operator_catalog/assets"
    loose = asset_root / f"{identity}.txr"
    if loose.is_file():
        return loose.read_bytes()
    bundle_root = asset_root / str(identity)
    receipt = native_resources.validate_bundle(
        identity,
        (bundle_root / native_resources.CONFIG_NAME).read_bytes(),
        (bundle_root / native_resources.INDEX_NAME).read_bytes(),
        (bundle_root / native_resources.ARCHIVE_NAME).read_bytes(),
        include_payload=True,
    )
    if receipt["layout"] != "upload":
        raise ValueError("已安装副官头像原生归档布局无效")
    for texture in native_resources.upload_texture_keys(identity):
        if texture in receipt["resources"]:
            return receipt["resources"][texture]
    raise ValueError("已安装副官头像原生归档缺少纹理")


class Workspace:
    def __init__(
        self, root, client, snapshot, module_binary=None, server_ini=None,
        server_root=None, package_root=None,
    ):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.assets = assets.Assets(client)
        self.snapshot = Path(snapshot)
        self.module_binary = Path(module_binary) if module_binary else None
        self.server_ini = Path(server_ini) if server_ini else None
        self.server_root = Path(server_root) if server_root else None
        self.package_root = Path(package_root) if package_root else None
        self.path = self.root / "operators.json"
        if self.path.exists():
            saved = model.load(self.path)
            model.validate(saved)
            if saved["client_index_sha256"] != self.assets.index_hash:
                raise ValueError("客户端数据包已变化，请在新的助手工作目录重新导入")
            self.baseline = copy.deepcopy(saved)
            native_ids = set(saved["native_operator_ids"])
            self.baseline["operators"] = [
                operator for operator in self.baseline["operators"]
                if operator["id"] in native_ids
            ]
        else:
            self.baseline = model.import_catalog(snapshot, self.assets)
            if presets.available(snapshot):
                self.baseline=presets.initialize(self.baseline,snapshot,self.assets)
            for op in self.baseline["operators"]:
                avatar=op["avatar"]
                if avatar["kind"]=="upload":
                    raw=_installed_upload(self.assets.root,op["id"])
                    if hashlib.sha256(raw).hexdigest()!=avatar["sha256"]:
                        raise ValueError("已安装副官头像与配置不匹配")
                    assets.validate_png(raw)
                    target=self.root/'uploads'/(avatar['sha256']+'.png')
                    target.parent.mkdir(exist_ok=True)
                    if not target.exists():target.write_bytes(raw)
                elif avatar['kind']=='pack':
                    sha=avatar['sha256'];source=self.assets.root/'sdgo/mods/operator_catalog/assets/imports'/(sha+'.opack')
                    raw=source.read_bytes()
                    if legacy_pack.digest(raw)!=sha:raise ValueError('已安装副官分享包与配置不匹配')
                    target=self.root/'packs'/(sha+'.opack');target.parent.mkdir(exist_ok=True)
                    if not target.exists():target.write_bytes(raw)
            model.save(self.path, self.baseline)
        self.read()

    def read(self):
        value = model.load(self.path)
        model.validate(value)
        if value["client_index_sha256"] != self.assets.index_hash:
            raise ValueError("客户端数据包已变化，请在新的助手工作目录重新导入")
        return value

    def check(self, expected):
        value = self.read()
        if expected != model.revision(value):
            raise ValueError("配置已在另一处改变，请刷新后重试")
        return value

    def commit(self, catalog):
        model.validate(catalog)
        old = self.path.read_bytes()
        backup = self.root / "history" / (hashlib.sha256(old).hexdigest() + ".json")
        if not backup.exists():
            backup.parent.mkdir(exist_ok=True)
            backup.write_bytes(old)
        model.save(self.path, catalog)
        model.save(self.root / "product-registrations.json", model.registrations(catalog))
        return self.public(catalog)

    def public(self, value=None):
        value = value or self.read()
        return {"schema_version": 1, "revision": model.revision(value), "operators": value["operators"],
                "drop_pools":value.get('drop_pools',{}),'imported_packs':value.get('imported_packs',{}),
                'registered_ability_extensions':value.get('registered_ability_extensions',[]),
                "items": value["items"], "native_operator_ids": value["native_operator_ids"],
                "gallery": [{"id": o["id"], "name": o["name"], "url": f"/api/operators/portrait/{o['id']}"}
                            for o in self.baseline["operators"] if o['id'] in self.baseline['native_operator_ids']],
                "capabilities": {"server_apply": getattr(self, "server_ini", None) is not None,
                                 "client_package": bool(getattr(self, "module_binary", None) and self.module_binary.exists()),
                                 "storefront_changes": False}}

    @functools.lru_cache(maxsize=8)
    def pack(self,sha):
        if not isinstance(sha,str) or len(sha)!=64 or any(c not in '0123456789abcdef' for c in sha):raise ValueError('副官包标识无效')
        raw=(self.root/'packs'/(sha+'.opack')).read_bytes()
        if legacy_pack.digest(raw)!=sha:raise ValueError('副官分享包校验失败')
        return legacy_pack.Pack(raw,self.assets)

    def preview_pack(self,raw,filename,expected):
        self.check(expected)
        source=legacy_pack.Pack(raw,self.assets,filename=filename)
        desc=source.describe()
        canonical=legacy_pack.package(source.files,source.profile,pack_id=(source.manifest or {}).get('pack_id','legacy-'+source.sha256[:24]),
            name=source.label,source_template=desc['source_template'],locale=(source.manifest or {}).get('resources',{}).get('locale',''),
            abilities=desc.get('abilities'),acquisition=desc.get('acquisition'),storefront=desc.get('storefront'),
            resources_metadata={k:v for k,v in (source.manifest or {}).get('resources',{}).items() if k not in ('profile','locale')})
        sha=legacy_pack.digest(canonical);path=self.root/'packs'/(sha+'.opack');path.parent.mkdir(exist_ok=True)
        if not path.exists():path.write_bytes(canonical)
        info=self.pack(sha).describe();info.update(source_archive_sha256=source.sha256)
        return {'revision':expected,'preview':info,'pack_sha256':sha,'portrait_url':'/api/operators/packs/'+sha+'/portrait'}

    def import_pack(self,sha,expected,*,name=None,template_id=None,source_template=None):
        catalog=self.check(expected);pack=self.pack(sha);desc=pack.describe()
        source_template=source_template or desc['source_template']
        if source_template not in pack.candidates:raise ValueError('副官资源模板不在包预览范围内')
        template_id=template_id or source_template
        updated,identity=model.create_operator(catalog,template_id)
        updated.setdefault('imported_packs',{})[sha]={
            'format':legacy_pack.FORMAT,'format_version':legacy_pack.FORMAT_VERSION,'name':desc['name'],
            'source_template':source_template,'voice_files':desc['voice_files'],'voice_events':len(desc['voice_events']),
            'distribution':desc['distribution'],'resources':desc['resources']}
        op=next(o for o in updated['operators'] if o['id']==identity)
        op.update(name=name or desc['name'],avatar={'kind':'pack','sha256':sha,'operator_id':source_template},drops=[])
        if desc.get('abilities') is not None:
            from . import capabilities
            op['abilities']=capabilities.normalize(desc['abilities'],updated.get('registered_ability_extensions',()))
        acquisition=desc.get('acquisition')
        if acquisition is not None:
            if not isinstance(acquisition,dict) or set(acquisition)-{'delivery','drops'}:raise ValueError('副官包获取规则无效')
            op.update(acquisition)
        if desc.get('storefront') is not None:op['storefront']=desc['storefront']
        result=self.commit(updated);result['selected_id']=identity;return result

    def share(self,identity):
        catalog=self.read();op=next((o for o in catalog['operators'] if o['id']==identity),None)
        if op is None or op['avatar']['kind']!='pack':raise ValueError('请选择由资源包导入的副官')
        p=self.pack(op['avatar']['sha256'])
        return legacy_pack.package(p.files,p.profile,pack_id=(p.manifest or {})['pack_id'],name=op['name'],
            source_template=op['avatar']['operator_id'],locale=(p.manifest or {}).get('resources',{}).get('locale',''),
            abilities=op.get('abilities'),acquisition={'delivery':op['delivery'],'drops':op['drops']},storefront=op.get('storefront'),
            resources_metadata={k:v for k,v in (p.manifest or {}).get('resources',{}).items() if k not in ('profile','locale')})

    def upload(self, data, expected):
        catalog = self.check(expected)
        width, height = assets.validate_png(data)
        sha = hashlib.sha256(data).hexdigest()
        path = self.root / "uploads" / (sha + ".png")
        path.parent.mkdir(exist_ok=True)
        if not path.exists():
            path.write_bytes(data)
        catalog["uploaded_assets"][sha] = {"width": width, "height": height}
        result = self.commit(catalog)
        result["uploaded_avatar"] = {"kind": "upload", "sha256": sha}
        return result

    def package(self):
        catalog = self.read()
        if not self.module_binary or not self.module_binary.is_file():
            raise ValueError("当前助手包缺少已编译的副官客户端模块")
        rows = self.assets.records("gdrs/0255.gdt", assets.OP_SIZE)
        files = {}
        prefix = "client/sdgo/mods/operator_catalog/"
        files[prefix + "operator_catalog.dll"] = self.module_binary.read_bytes()
        files[prefix + "mod.ini"] = (server.MODULE_DIR / "mod.ini").read_bytes()
        files[prefix + 'manager.json']=(server.MODULE_DIR/'manager.json').read_bytes()
        ini = ["[catalog]", "Version=2", "Revision=" + model.revision(catalog)]
        fixed_names=presets.fixed_name_ids(catalog,include_disabled=True)
        for op in catalog["operators"]:
            if not op["managed"]:
                continue
            avatar = op["avatar"]
            profile = avatar["operator_id"] if avatar["kind"] in ("native","pack") else op["template_id"]
            sprite = ""
            packed=None
            bundle_resources={}
            if avatar["kind"] == "upload":
                raw = (self.root / "uploads" / (avatar["sha256"] + ".png")).read_bytes()
                if hashlib.sha256(raw).hexdigest() != avatar["sha256"]:
                    raise ValueError("头像文件校验失败")
                assets.validate_png(raw)
                texture = native_resources.upload_texture_key(op["id"]).replace("/","\\")
                bundle_resources[texture.replace("\\","/")] = raw
                sprite = f"sdgo\\mods\\operator_catalog\\assets\\{op['id']}.spr"
                for index, (offset,capacity) in enumerate(assets.SPRITE_FIELDS):
                    original = assets.wchar(rows[profile],offset,capacity)
                    if original:
                        spr = self.assets.read(original)
                        height, width = struct.unpack_from("<II",spr,12)
                    else:
                        width=height=128
                    bundle_resources[
                        f"sdgo/mods/operator_catalog/assets/{op['id']}_{index}.spr"
                    ] = assets.sprite_binary(texture,width,height)
            elif avatar['kind']=='pack':
                sha=avatar['sha256'];pack=self.pack(sha)
                generated,packed=pack.compile(op['id'],profile)
                bundle_resources.update(generated)
                raw=(self.root/'packs'/(sha+'.opack')).read_bytes()
                if legacy_pack.digest(raw)!=sha:raise ValueError('副官分享包已被改动')
                files[prefix+'assets/imports/'+sha+'.opack']=raw
            if bundle_resources:
                bundle=native_resources.build_bundle(op['id'],bundle_resources)
                for name,data in bundle['files'].items():
                    files[prefix+f"assets/{op['id']}/"+name]=data
            ini += ["", f"[operator:{op['id']}]", f"Category={op['category_id']}",
                    'Enabled='+str(int(op['enabled'])),
                    f"Template={op['template_id']}", "Name=" + op["name"],
                    "NameHex=" + op["name"].encode("utf-16-le").hex(),
                    "FixedName=" + str(int(op['id'] in fixed_names)),
                    "MissionParts=" + str(int(op.get('abilities',{}).get('mission_parts')) if op.get('abilities',{}).get('mission_parts') is not None else 1 if op['id'] in fixed_names else -1),
                    f"AvatarProfile={profile}", "AvatarSprite=" + sprite]
            if packed is not None:
                ini += ['Dialogue='+packed['dialogue'],'IntroVoice='+packed['intro_voice']]
                ini += [f'Sprite{i}='+path for i,path in enumerate(packed['sprites'])]
        files[prefix + "display.ini"] = b"\xff\xfe" + ("\r\n".join(ini)+"\r\n").encode("utf-16-le")
        public_definitions={"schema_version":1,"revision":model.revision(catalog),
                            "client_index_sha256":catalog['client_index_sha256'],
                            "operators":[o for o in catalog['operators'] if o['managed']],
                            "uploaded_assets":catalog['uploaded_assets']}
        for field in ('drop_pools','default_pack_version','imported_packs'):
            if field in catalog:public_definitions[field]=copy.deepcopy(catalog[field])
        files[prefix+'definitions.json']=(json.dumps(public_definitions,ensure_ascii=False,indent=2)+'\n').encode()
        import configparser
        metadata=configparser.ConfigParser(interpolation=None)
        metadata.read_string(files[prefix+"mod.ini"].decode("utf-8-sig"))
        for name in metadata["mod"].get("files","").split(","):
            name=name.strip()
            if name and prefix+name not in files:
                resource=server.MODULE_DIR/name
                if not resource.resolve().is_relative_to(server.MODULE_DIR.resolve()):raise ValueError("模块资源路径越界")
                files[prefix+name]=resource.read_bytes()
        generated={name[len(prefix):]:{'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}
                   for name,raw in files.items() if name.startswith(prefix) and
                   name not in (prefix+'mod.ini',prefix+'manager.json') and not name.endswith('.dll')}
        files[prefix+'manifest.json']=(json.dumps({'schema_version':1,'module':'operator_catalog',
            'kind':'generated-client-resources','revision':model.revision(catalog),'files':generated},ensure_ascii=False,indent=2)+'\n').encode('utf-8')
        # Keep native INI lines small; generated content is declared in the
        # existing hashed resource-manifest contract used by owner delivery.
        declared={x.strip() for x in metadata['mod'].get('files','').split(',') if x.strip()}
        declared.update(('manager.json','manifest.json'))
        metadata['mod']['files']=','.join(sorted(declared))
        text=io.StringIO();metadata.write(text,space_around_delimiters=False);files[prefix+'mod.ini']=text.getvalue().encode('utf-8')
        fragment = server.MODULE_DIR / "symbols.fragment.ini"
        if not fragment.exists():
            raise ValueError("客户端模块缺少符号签名片段")
        files["symbols.fragment.ini"] = fragment.read_bytes()
        installer = Path(__file__).with_name("Install-Operator.ps1")
        files["Install-Operator.ps1"] = installer.read_bytes()
        files["registrations.json"] = (json.dumps(model.registrations(catalog),ensure_ascii=False,indent=2)+"\n").encode()
        manifest = {"module": "operator_catalog", "schema_version": 1,
                    "revision": model.revision(catalog), "storefront_changes": False,
                    "host": "GOnline-54BE1568-00C97000",
                    "files": {name:hashlib.sha256(data).hexdigest() for name,data in files.items()}}
        files["manifest.json"] = json.dumps(manifest,indent=2).encode()
        output = io.BytesIO()
        with zipfile.ZipFile(output,"w",zipfile.ZIP_DEFLATED) as z:
            for name,data in files.items():
                z.writestr(name,data)
        return output.getvalue()

    def _runtime_configuration(self):
        values = {
            "server_root": getattr(self, "server_root", None),
            "package_root": getattr(self, "package_root", None),
            "server_ini": getattr(self, "server_ini", None),
        }
        missing = [name for name, value in values.items() if value is None]
        return values, missing

    def _runtime_plan(self, catalog):
        values, missing = self._runtime_configuration()
        if missing:
            raise ValueError(
                "副官运行环境尚未配置：" + ",".join(missing)
            )
        return runtime.plan_runtime_install(
            values["server_root"], values["package_root"], catalog,
            values["server_ini"],
        )

    def runtime_plan(self, expected=None):
        catalog = self.check(expected) if expected is not None else self.read()
        values, missing = self._runtime_configuration()
        if missing:
            return {
                "schema_version": 1,
                "mode": "plan",
                "ready": False,
                "server_root": (
                    str(values["server_root"])
                    if values["server_root"] is not None else None
                ),
                "package_root": (
                    str(values["package_root"])
                    if values["package_root"] is not None else None
                ),
                "server_ini": (
                    str(values["server_ini"])
                    if values["server_ini"] is not None else None
                ),
                "startup_xml": None,
                "catalog_revision": model.revision(catalog),
                "database_channels": [],
                "selected_hosts": [],
                "skipped_hosts": [],
                "actions": [],
                "blockers": [
                    "operator runtime is not configured: "
                    + ", ".join(missing)
                ],
                "writes_performed": False,
            }
        return dict(self._runtime_plan(catalog))

    def runtime_status(self):
        catalog = self.read()
        catalog_revision = model.revision(catalog)
        values, missing = self._runtime_configuration()
        result = {
            "configured": not missing,
            "files_installed": False,
            "database_applied": False,
            "restart_required": False,
            "runtime_loaded": False,
            "running_effective": False,
            "catalog_revision": catalog_revision,
            "database_revision": None,
            "hosts": [],
            "reasons": [],
        }
        if missing:
            result["reasons"].append(
                "operator runtime is not configured: " + ", ".join(missing)
            )
            return result
        plan = self._runtime_plan(catalog)
        result["hosts"] = copy.deepcopy(plan.get("selected_hosts", []))
        readback = runtime.inspect_runtime_plan(plan)
        result["files_installed"] = readback["files_installed"]
        if not readback["files_installed"]:
            result["reasons"].extend(readback["reasons"])
        if plan._state:
            statuses = [
                host.get("module_status", {})
                for host in plan.get("selected_hosts", [])
            ]
            result["runtime_loaded"] = bool(statuses) and all(
                status.get("effective") for status in statuses
            )
            if not result["runtime_loaded"]:
                for host, status in zip(
                    plan.get("selected_hosts", []), statuses
                ):
                    for reason in status.get("reasons", ()):
                        result["reasons"].append(
                            f"{host.get('folder')}: {reason}"
                        )
        else:
            result["reasons"].extend(plan.get("blockers", ()))
        try:
            database = server.catalog_status(catalog, values["server_ini"])
        except Exception as error:
            result["reasons"].append(
                "operator catalog database status is unavailable: " + str(error)
            )
        else:
            result["database_applied"] = database["database_applied"]
            result["database_revision"] = database["applied_revision"]
            result["reasons"].extend(database["reasons"])
        result["restart_required"] = (
            result["files_installed"]
            and result["database_applied"]
            and not result["runtime_loaded"]
        )
        result["running_effective"] = (
            result["files_installed"]
            and result["database_applied"]
            and result["runtime_loaded"]
        )
        result["reasons"] = list(dict.fromkeys(result["reasons"]))
        return result

    def install_runtime(self, expected):
        catalog = self.check(expected)
        plan = self._runtime_plan(catalog)
        installed = runtime.install_runtime(plan)
        installed["runtime_status"] = self.runtime_status()
        return installed

    def apply(self, expected):
        catalog = self.check(expected)
        if getattr(self, "server_ini", None) is None:
            raise ValueError("此助手实例为离线配置模式，尚未连接服务端")
        evidence = None
        if (
            getattr(self, "server_root", None) is not None
            or getattr(self, "package_root", None) is not None
        ):
            plan = self._runtime_plan(catalog)
            evidence = runtime.installed_runtime_evidence(plan)
        return server.apply_catalog(
            catalog, self.server_ini, runtime_evidence=evidence
        )
