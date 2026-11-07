"""Check explicit framework SQL ownership and forbid private content in Releases."""
from pathlib import Path
import argparse,json,subprocess,zipfile

ROOT=Path(__file__).resolve().parents[1]
PRIVATE_NAMES={'server-profile.json','profile-source.json','client-source.json','shared-settings.json','owner-source.json','owner-release.json','player-package.json'}


def source(root=ROOT):
    root=Path(root);policy=json.loads((root/'distribution/artifact-ownership.json').read_text(encoding='utf-8'))
    rows=policy['framework_sql'];tracked=subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')
    sql={n for n in tracked if n.casefold().endswith('.sql')}
    if sql-set(rows):raise ValueError('SQL ownership is not declared: '+', '.join(sorted(sql-set(rows))))
    for name in sql:
        row=rows[name]
        if row.get('ownership')!='framework'or row.get('kind')not in('schema','query','migration','reference-data')or not row.get('purpose'):raise ValueError('Invalid generic SQL ownership: '+name)
    return len(sql)


def archive(path):
    with zipfile.ZipFile(path)as z:
        for name in z.namelist():
            if Path(name).name.casefold()in PRIVATE_NAMES or any(p in name.casefold().split('/')for p in('local-state','local-mods','private','content')):
                raise ValueError('Owner/local content is not a framework artifact: '+name)
    return True


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path);p.add_argument('--archive',type=Path);a=p.parse_args()
    if a.source:print('Declared generic SQL files:',source(a.source))
    if a.archive:archive(a.archive);print('Framework archive ownership verified')
