"""The native core and Manager fingerprint the same declared input files."""
from __future__ import annotations
import configparser
import hashlib
import re
from pathlib import Path
from . import storage
from .dependencies import runtime_sections


def relative(name):
    if name=='root:config.xml':return name
    if not isinstance(name,str)or not name or '\\'in name or ':'in name or name.startswith('/')or any(p in('','.','..')for p in name.split('/')):
        raise ValueError('运行配置文件路径无效')
    if not re.fullmatch(r'[A-Za-z0-9_./-]+',name):raise ValueError('运行配置文件路径无效')
    return name


def files(manifest,contract):
    result={'mod.ini'}
    for name in manifest.get('mod','config_files',fallback='').split(','):
        if name.strip():result.add(relative(name.strip()))
    result.update(relative(n)for n in contract.get('configuration_files',[]))
    for field in contract.get('settings',[]):result.add('root:config.xml'if field['file']=='config.xml'else relative(field['file']))
    return sorted(result)


def fingerprint(directory,names):
    directory=Path(directory).resolve();parts=[]
    for name in sorted(set(names)):
        relative(name)
        path=storage.safe_child(directory.parents[2],'config.xml')if name=='root:config.xml'else storage.safe_child(directory,name)
        if path.is_file()and path.stat().st_size>16*1024*1024:raise ValueError('运行配置文件超过核验预算')
        digest=hashlib.sha256(path.read_bytes()).hexdigest()if path.is_file()else'-'
        parts.append(name+'\0'+digest+'\n')
    return hashlib.sha256(''.join(parts).encode('utf-8')).hexdigest()


def render(raw,contract):
    ini=configparser.ConfigParser(interpolation=None,strict=False);ini.optionxform=str
    ini.read_string(storage.decode_ini(raw)[0])
    for section in list(ini.sections()):
        if section.startswith('framework.'):ini.remove_section(section)
    for section,values in runtime_sections(contract).items():ini[section]=values
    ini['framework.configuration']={'files':','.join(files(ini,contract))}
    ini.optionxform=str
    import io
    out=io.StringIO();ini.write(out,space_around_delimiters=False)
    return out.getvalue().encode('utf-8')
