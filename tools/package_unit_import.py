"""Package the unit importer tools and trusted adapter, never owner or game data."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries',type=Path,default=root/'build/Release')
    parser.add_argument('--manager',type=Path)
    parser.add_argument('--out',type=Path,default=root/'dist/SDGO-UnitImport-0.2.0-beta.zip')
    parser.add_argument('--directory-only',action='store_true')
    args=parser.parse_args()
    names=['docs/unit-import.md','docs/unit-import.zh-CN.md',
        'docs/unit-import-validation.zh-CN.md','LICENSE']
    files={name:(root/name).read_bytes() for name in names}
    if not args.directory_only:
        manager=args.manager or args.binaries.parent/'manager/SDGO Mod Manager.exe'
        files['SDGO Mod Manager.exe']=manager.read_bytes()
    files['README.zh-CN.txt']=('SDGO 机体导入 0.2.0 Beta · Mod Manager\n\n打开“SDGO Mod Manager.exe”，选择左侧“机体导入”。\n'
        '可继续使用原机体制作工作区，无需系统 Python。RAR 解析需要本机 7-Zip。\n'
        '先解析补齐并选择发布图片，再准备独立客户端调用实际商城与扭蛋 UI，目视核对后确认导入。\n'
        '通用工具包不含游戏 EXE/资源、机体数据、账号信息、数据库凭据或实际宿主配置。\n'
        '详细使用和验证边界见 docs/unit-import.zh-CN.md。\n').encode('utf-8')
    revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    manifest={'module':'unit-import-tools','schema_version':1,'version':'0.2.0-beta','entry':'SDGO Mod Manager.exe','source_commit':revision,
        'source_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip()),
        'files':{name:hashlib.sha256(raw).hexdigest() for name,raw in files.items()}}
    files['toolkit-manifest.json']=json.dumps(manifest,ensure_ascii=False,indent=2).encode()
    if args.directory_only:
        if args.out.exists():raise ValueError('输出目录必须是新的')
        args.out.mkdir(parents=True)
        for name,raw in files.items():
            path=args.out/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
        print(json.dumps({'directory':str(args.out.resolve()),'files':len(files)},ensure_ascii=False));return
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.out,'x',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name,raw in sorted(files.items()):z.writestr(name,raw)
    with zipfile.ZipFile(args.out) as z:
        assert z.testzip() is None and set(z.namelist())==set(files)
        for name,digest in manifest['files'].items():assert hashlib.sha256(z.read(name)).hexdigest()==digest
    print(json.dumps({'zip':str(args.out.resolve()),'files':len(files),'sha256':hashlib.sha256(args.out.read_bytes()).hexdigest()},ensure_ascii=False))


if __name__=='__main__':main()
