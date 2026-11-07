"""Mirror verified framework refs using the maintainer's configured Git identity."""
from pathlib import Path
import json,os,re,subprocess,sys,tempfile
TOKENS=('RUNTIME_GIT_MIRROR_TOKEN','RUNTIME_GITHUB_MIRROR_TOKEN')


def reason(stderr,env):
    """Keep Git's rejection lines but drop mirror addresses, paths and credentials: the target list is private."""
    text=stderr.decode(errors='replace')
    for name in TOKENS:
        if env.get(name):text=text.replace(env[name],'***')
    text=re.sub(r'[A-Za-z][A-Za-z0-9+.-]*://\S+|\S+@[^\s:]+:\S*','<mirror>',re.sub(r"'[^'\n]*'","'<mirror>'",text))
    lines=[l.strip()[:200]for l in text.splitlines()if l.strip().startswith(('!','error:','fatal:','remote:'))]
    return' | '.join(lines[-6:])or'no Git diagnostic'


def main():
    # Hosted mirrors answer in their own language; Windows runners default stdout to a legacy code page.
    sys.stdout.reconfigure(encoding='utf-8',errors='replace')
    targets=json.loads(os.environ.get('RUNTIME_GIT_MIRROR_TARGETS')or'[]')or[{'url':os.environ['RUNTIME_GIT_MIRROR_URL'],'branches':['master','runtime/stable','runtime/beta']}]
    for target in targets:
        if not target['url'].startswith('https://')or'@'in target['url']or not set(target['branches'])<={'master','runtime/stable','runtime/beta'}:raise ValueError('Use credential-free HTTPS mirrors and declared runtime refs')
    env=dict(os.environ)
    with tempfile.TemporaryDirectory()as directory:
        token=env.get('RUNTIME_GIT_MIRROR_TOKEN')
        if token:
            ask=Path(directory)/'ask.py';ask.write_text("import os,sys\nprint('git' if 'username' in sys.argv[1].lower() else os.environ[os.environ['RUNTIME_MIRROR_CREDENTIAL']])\n",encoding='utf-8')
            # Git executes askpass through its own shell on both Windows and Linux.
            helper=Path(directory)/'ask.sh';helper.write_text('#!/bin/sh\nexec "'+sys.executable.replace('\\','/')+'" "'+ask.as_posix()+'" "$@"\n',encoding='utf-8');helper.chmod(0o700)
            env.update(GIT_ASKPASS=str(helper),GIT_TERMINAL_PROMPT='0',GCM_INTERACTIVE='Never')
        def git(*args):
            prefix=['-c','http.https://github.com/.extraheader=']if any(a.startswith('https://github.com/')for a in args)else[]
            r=subprocess.run(['git',*prefix,*args],env=env,capture_output=True)
            if r.returncode:raise RuntimeError(args[0]+' exited '+str(r.returncode)+': '+reason(r.stderr,env))
            return r.stdout.decode().strip()
        published={};failed=[]
        for number,target in enumerate(targets):
            env['RUNTIME_MIRROR_CREDENTIAL']='RUNTIME_GITHUB_MIRROR_TOKEN'if target['url'].startswith('https://github.com/')else'RUNTIME_GIT_MIRROR_TOKEN'
            # Each ref is independent, so one rejected ref does not keep the others stale.
            for branch in target['branches']:
                try:
                    ref='refs/heads/'+branch;rows=git('ls-remote','origin',ref).split()
                    if not rows:continue
                    commit=rows[0];git('fetch','--no-tags','origin',ref);git('push',target['url'],commit+':'+ref)
                    if git('ls-remote',target['url'],ref).split()[:1]!=[commit]:raise ValueError('readback differs')
                    published[str(number)+':'+branch]=commit
                except(RuntimeError,ValueError)as error:failed.append('target '+str(number)+' '+branch+': '+str(error))
        print('Verified mirrored refs: '+(', '.join(published)or'none'),flush=True)
        for line in failed:print('::error title=Runtime mirror ref not updated::'+line,flush=True)
        if failed:sys.exit(1)


if __name__=='__main__':main()
