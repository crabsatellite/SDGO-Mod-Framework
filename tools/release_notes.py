"""Prepare bilingual programme release notes and applicable animated WebP assets."""
from pathlib import Path,PurePosixPath
import argparse,configparser,hashlib,json,re,struct,zipfile

ROOT=Path(__file__).resolve().parents[1]


def version(value):
    match=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:-beta\.\d+)?',value)
    if not match:raise ValueError('Invalid release version')
    return tuple(map(int,match.groups()))


def animation(path):
    raw=path.read_bytes()
    if raw[:4]!=b'RIFF' or raw[8:12]!=b'WEBP' or len(raw)!=struct.unpack_from('<I',raw,4)[0]+8:
        raise ValueError('Demo must be a complete WebP: '+str(path))
    frames=0;duration=0;offset=12
    while offset+8<=len(raw):
        tag=raw[offset:offset+4];size=struct.unpack_from('<I',raw,offset+4)[0];data=offset+8
        if data+size>len(raw):raise ValueError('Truncated WebP chunk')
        if tag==b'ANMF':
            if size<16:raise ValueError('Invalid animated frame')
            frames+=1;duration+=int.from_bytes(raw[data+12:data+15],'little')
        offset=data+size+(size&1)
    if frames<2 or not 250<=duration<=45000:raise ValueError('Demo must be an animation lasting 0.25..45 seconds')
    return {'frames':frames,'duration_ms':duration,'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}


def package(asset):
    with zipfile.ZipFile(asset)as archive:
        manifest=json.loads(archive.read('release.json'));version(manifest['version'])
        if manifest['channel']not in('stable','beta'):raise ValueError('Unknown release channel')
        mods=set()
        for name in archive.namelist():
            if re.fullmatch(r'(client|server)/sdgo/mods/[a-z][a-z0-9_]*/mod.ini',name):
                config=configparser.ConfigParser(interpolation=None);config.read_string(archive.read(name).decode('utf-8-sig'))
                if config.get('mod','distribution',fallback='')!='public':raise ValueError('Release notes accept public programme packages only')
                mods.add(config['mod']['id'])
        return manifest,mods


def select_demos(root,registry,release_version,mods):
    if registry.get('schema_version')!=1 or not isinstance(registry.get('demos'),list):raise ValueError('Invalid demo registry')
    selected=[];ids=set();names=set()
    for demo in registry['demos']:
        identity=demo['id'];relative=PurePosixPath(demo['path'])
        if not re.fullmatch('[a-z][a-z0-9-]*',identity)or identity in ids:raise ValueError('Invalid or duplicate demo ID')
        ids.add(identity)
        if relative.is_absolute()or '..'in relative.parts or relative.parts[:1]!=('docs',)or relative.suffix!='.webp':raise ValueError('Demo must be a repository docs WebP')
        path=(root/relative).resolve()
        if not path.is_relative_to(root.resolve())or path.is_symlink():raise ValueError('Demo escapes the repository')
        if relative.name in names:raise ValueError('Duplicate release asset name')
        names.add(relative.name)
        for key in('title_zh','title_en','guide_zh','guide_en'):
            if not isinstance(demo.get(key),str)or not demo[key].strip():raise ValueError('Demo needs bilingual titles and guides')
        for key in('guide_zh','guide_en'):
            guide=(root/demo[key]).resolve()
            if not guide.is_relative_to(root.resolve())or not guide.is_file():raise ValueError('Demo guide is missing or outside the repository')
        if version(release_version)<version(demo['since'])or(demo['mods']and not set(demo['mods'])&mods):continue
        selected.append({**demo,'file':str(path),'asset':relative.name,**animation(path)})
    return selected


def render(root,manifest,mods,registry):
    label=manifest['version'];repo=manifest['repository'];commit=manifest['source_commit'];version(label)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',repo)or not re.fullmatch('[0-9a-f]{40}',commit):raise ValueError('Invalid public release identity')
    demos=select_demos(root,registry,label,mods);stable=manifest['channel']=='stable'
    fragment=root/'docs/releases'/('.'.join(map(str,version(label)))+'.md')
    if fragment.is_file():body=fragment.read_text(encoding='utf-8').strip()
    else:body=('## 中文\n\n本包更新 SDGO Mod Manager、框架与清单声明的公共 Mod 程序。玩家配置和自定义内容由服主配置入口维护。\n\n'
               '## English\n\nThis package updates SDGO Mod Manager, the framework and its declared public Mod programmes. Owner profiles maintain shared settings and custom content.')
    if not re.search(r'^## 中文\s*$',body,re.M)or not re.search(r'^## English\s*$',body,re.M):raise ValueError('Release notes need both Chinese and English sections')
    channel='正式版 / Stable'if stable else'测试通道 / Beta'
    notes=f'**SDGO Mod Manager {label} · {channel}**\n\n'+body+'\n\n'
    notes+=f'[中文功能目录](https://github.com/{repo}/blob/{commit}/README.zh-CN.md) · [English feature index](https://github.com/{repo}/blob/{commit}/README.md)\n'
    if demos:
        notes+='\n## 功能短片 / Short demonstrations\n\nWebP 动图展示操作与效果；具体设置及适用通道见功能指南。\nAnimated WebP clips show the workflow; consult each guide for settings and channels.\n'
        for demo in demos:
            notes+=f'\n### {demo["title_zh"]} / {demo["title_en"]}\n\n'
            notes+=f'![{demo["title_zh"]} / {demo["title_en"]}](https://github.com/{repo}/releases/download/v{label}/{demo["asset"]})\n\n'
            notes+=f'[中文说明](https://github.com/{repo}/blob/{commit}/{demo["guide_zh"]}) · [English guide](https://github.com/{repo}/blob/{commit}/{demo["guide_en"]})\n'
    return notes,demos


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--asset',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--media-list',type=Path,required=True)
    args=parser.parse_args();manifest,mods=package(args.asset)
    notes,demos=render(ROOT,manifest,mods,json.loads((ROOT/'docs/feature-demos.json').read_bytes()))
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(notes,encoding='utf-8')
    args.media_list.parent.mkdir(parents=True,exist_ok=True);args.media_list.write_text(json.dumps(demos,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'version':manifest['version'],'languages':['zh-CN','en'],'demos':len(demos),'output':str(args.output)},ensure_ascii=False))
