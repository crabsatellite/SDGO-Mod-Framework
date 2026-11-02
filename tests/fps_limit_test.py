"""Run the real-core FPS DLL fixtures in independent native host processes."""
from pathlib import Path
import argparse,concurrent.futures,configparser,os,shutil,subprocess,uuid

ROOT=Path(__file__).resolve().parents[1]
MODES=('160','120','60','min','max','zero','over','negative','disabled','badbytes','wrongkind')

def run_case(mode,binaries,work):
    folder=work/mode;mod=folder/'sdgo/mods/fps_limit';mod.mkdir(parents=True)
    for name,target in (('fps_limit_runtime_test.exe',folder/'fps_limit_runtime_test.exe'),
                        ('core.dll',folder/'sdgo/core.dll'),('fps_limit.dll',mod/'fps_limit.dll')):
        shutil.copy2(binaries/name,target)
    manifest=(ROOT/'mods/fps_limit/mod.ini').read_text(encoding='utf-8')
    cfg=configparser.ConfigParser(interpolation=None);cfg.read_string(manifest)
    manifest=manifest.replace('hosts='+cfg['mod']['hosts'],'hosts=*')
    if mode=='160':manifest=manifest.replace('LogFps=0','LogFps=1')
    (mod/'mod.ini').write_text(manifest,encoding='utf-8')
    (folder/'sdgo/sdgo.ini').write_text('[core]\nlog_level=0\n'+('[mods]\nfps_limit=0\n' if mode=='disabled' else ''),encoding='utf-8')
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0)|getattr(subprocess,'NORMAL_PRIORITY_CLASS',0)
    result=subprocess.run([str(folder/'fps_limit_runtime_test.exe'),mode],cwd=folder,
        capture_output=True,timeout=120,creationflags=flags)
    output=(result.stdout+result.stderr).decode('utf-8',errors='replace')
    if result.returncode or 'ALL FPS LIMIT RUNTIME CHECKS PASSED' not in output:
        raise RuntimeError(mode+': '+output)
    log=(folder/'sdgo/logs/sdgo.log').read_text(encoding='utf-8')
    active=mode not in ('disabled','badbytes','wrongkind')
    loaded='loaded Configured FPS limit '+cfg['mod']['version']
    if (loaded in log)!=active:raise RuntimeError(mode+': unexpected production Mod load state')
    if active:
        effective={'min':1,'max':1000,'zero':60,'over':60,'negative':60}.get(mode,int(mode) if mode.isdigit() else 60)
        if f'applied native FPS timer: render_cap={effective} update_cap=60' not in log:
            raise RuntimeError(mode+': native 60 Hz / independent render policy missing')
        if log.count('applied native FPS timer:')!=2:
            raise RuntimeError(mode+': native Idle wrote unchanged timer state')
    return mode

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--build-dir',type=Path,default=ROOT/'build')
    parser.add_argument('--config',default='Release');args=parser.parse_args()
    work=args.build_dir.resolve()/'fps_limit_runtime_run'/uuid.uuid4().hex
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        results=list(executor.map(lambda mode:run_case(mode,args.build_dir/args.config,work),MODES))
    if work.resolve().parent!=(args.build_dir.resolve()/'fps_limit_runtime_run'):
        raise RuntimeError('Native fixture cleanup escaped the selected build directory')
    for path in work.rglob('*'):
        if path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction()):
            raise RuntimeError('Linked native fixture cleanup target')
    shutil.rmtree(work)
    print('ALL FPS LIMIT RUNTIME CASES PASSED: '+', '.join(results),flush=True)

if __name__=='__main__':main()
