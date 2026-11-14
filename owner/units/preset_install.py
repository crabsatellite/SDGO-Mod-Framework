"""Transactional data-only preset installation beside an existing owner catalog."""
from __future__ import annotations
import hashlib
import configparser
import json
from pathlib import Path
import os
import shutil
import uuid
from .batch_server import load_publication
from .indexes import catalog_records
from .preset_publication import json_bytes
from manager import storage

RECEIPT='preset-install.json'
ALLOWED={'catalog.bin','definitions.json','resources.zdx','resources.zpk','resource-overlay.ini',
         'skill-atlases.bin','stage-rules.bin','blueprint-availability.json','blueprint-availability.bin'}

def sha(raw):return hashlib.sha256(raw).hexdigest()


def _overlay(folder):
    """Open the complete native archive, including declared split parts."""
    from owner.native_archive import OverlayReader,part_paths,MAX_INDEX_BYTES
    config=folder/'resource-overlay.ini'
    if not config.is_file() or config.stat().st_size>64*1024:raise ValueError('机体资源归档配置缺失或超过限制')
    parser=configparser.ConfigParser(interpolation=None,strict=True)
    try:
        parser.read_string(config.read_text('utf-8-sig'));section=parser['runtime']
        if section.getint('schema')!=1 or section.get('index')!='resources.zdx' or section.get('archive')!='resources.zpk':raise ValueError()
        parts=section.getint('parts',fallback=1);chunks=section.getint('part_chunks',fallback=0)
        if not 1<=parts<=32 or (parts>1 and chunks<1) or chunks<0:raise ValueError()
    except (KeyError,ValueError,configparser.Error,UnicodeError):raise ValueError('机体资源归档配置无效')from None
    paths=[config,folder/'resources.zdx',*part_paths(folder/'resources.zpk',parts)]
    for path in paths:
        if not path.is_file() or path.is_symlink() or getattr(path.stat(),'st_file_attributes',0)&0x400:
            raise ValueError('机体资源归档分卷缺失或包含链接：'+path.name)
    index=paths[1]
    if index.stat().st_size>MAX_INDEX_BYTES:raise ValueError('机体资源归档索引超过限制')
    raw=index.read_bytes()
    if sha(raw)!=section.get('index_sha256'):raise ValueError('机体资源归档索引哈希不一致')
    return OverlayReader(raw,folder/'resources.zpk',parts,chunks)

def verify(client):
    folder=Path(client)/'sdgo/mods/unit_catalog/presets';path=folder/RECEIPT
    if not path.is_file():return None
    value=json.loads(path.read_bytes())
    if value.get('schema')!='sdgo.unit-preset-install/1' or not isinstance(value.get('files'),dict):raise ValueError('默认预设安装回执无效')
    if folder.is_symlink() or any(not entry.is_file() or entry.name not in set(value['files'])|{RECEIPT} for entry in folder.iterdir()):
        raise ValueError('默认预设目录有未登记内容，已保留')
    for name,digest in value['files'].items():
        if name not in ALLOWED:raise ValueError('默认预设回执包含非数据文件')
        target=storage.safe_child(folder,name)
        if not target.is_file() or target.is_symlink() or sha(target.read_bytes())!=digest:
            raise ValueError('默认预设内容被外部修改，已保留：'+name)
    return value

def install(client,publication,fingerprint,*,dry_run=False):
    """Never replace root owner files, programmes, settings or player data."""
    client=Path(client).resolve();publication=load_publication(publication)
    module=client/'sdgo/mods/unit_catalog';source=publication.root/'package/client/sdgo/mods/unit_catalog'
    if not (client/'GOnline.exe').is_file() or not (module/'unit_catalog.dll').is_file():raise ValueError('请先安装配套客户端框架')
    if sha((module/'unit_catalog.dll').read_bytes())!=sha((source/'unit_catalog.dll').read_bytes()):
        raise ValueError('客户端机体程序与默认预设的配套版本不同，请先正常更新框架')
    data={path.name:path.read_bytes() for path in source.iterdir() if path.is_file() and path.name in ALLOWED}
    if not ALLOWED<=set(data):raise ValueError('默认预设的配套数据不完整')
    incoming=catalog_records(data['catalog.bin'])
    primary=module/'catalog.bin'
    if primary.is_file():
        overlap=set(incoming)&set(catalog_records(primary.read_bytes()))
        if overlap:raise ValueError('默认预设与已有机体目录编号冲突，已保留已有内容：'+str(min(overlap)))
    # The broker keeps both archives independently; no native virtual path may
    # shadow a resource already supplied by the owner's primary bundle.
    if (module/'resources.zdx').is_file():
        with _overlay(module) as original,_overlay(source) as candidate:
            overlap=set(original.entries)&set(candidate.entries)
        if overlap:raise ValueError('默认预设资源与已有目录重名：'+sorted(overlap)[0])
    from manager.operations import unmapped_image
    with unmapped_image(client/'GOnline.exe'):
        prior=verify(client);target=module/'presets'
        if target.exists() and prior is None:raise ValueError('默认预设目录已有未登记内容，拒绝覆盖')
        if prior and prior.get('owner')!=publication.owner:raise ValueError('默认预设目录属于另一个所有者')
        if dry_run:return {'ready':True,'changes_applied':False}
        marker={'schema':'sdgo.unit-preset-install/1','owner':publication.owner,'revision':publication.revision,
                'fingerprint':fingerprint,'files':{name:sha(raw) for name,raw in data.items()},
                'registered_roots':[family['root_unit_id'] for family in publication.families],
                'creates_shop_products':False,'creates_gacha_slots':False,'grants_inventory_units':False}
        if prior==marker:return {'installed':True,'changed':False,'revision':publication.revision}
        token=uuid.uuid4().hex;prepared=module/('.presets-prepared-'+token);rollback=module/('.presets-rollback-'+token)
        for path in (target,prepared,rollback):
            if not path.resolve().is_relative_to(module.resolve()) or path.is_symlink():raise ValueError('预设安装路径离开模块目录')
        prepared.mkdir()
        for name,raw in data.items():storage.atomic(prepared/name,raw)
        storage.atomic(prepared/RECEIPT,json_bytes(marker))
        ini=module/'mod.ini';original_ini=ini.read_bytes()
        try:
            if target.exists():os.replace(target,rollback)
            os.replace(prepared,target)
            storage.atomic(ini,storage.patch_ini(original_ini,[('mod','enabled','1')]))
            verify(client)
        except BaseException:
            # A failed new directory remains recoverable outside the scanned
            # presets name. Restore the exact pre-existing data and settings.
            if target.exists():os.replace(target,module/('.presets-failed-'+token))
            if rollback.exists():os.replace(rollback,target)
            storage.atomic(ini,original_ini)
            raise
        # Only the verified prior generated data lives here. Authored primary
        # content was never moved; failed installations keep their recovery
        # directory above. Do not accumulate old generated copies per update.
        if rollback.exists():
            checked=rollback.resolve()
            if checked.parent!=module.resolve() or rollback.is_symlink():
                raise ValueError('预设回滚目录离开模块目录，已保留')
            shutil.rmtree(checked)
        return {'installed':True,'changed':True,'revision':publication.revision,
                'rollback_data':None,'acquisition_configured':False}
