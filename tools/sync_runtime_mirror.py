"""Mirror verified framework refs using the maintainer's configured Git identity."""
from pathlib import Path
import json,os,subprocess,sys,tempfile


def main():
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
            if r.returncode:raise RuntimeError('Runtime mirror Git operation failed (exit '+str(r.returncode)+')')
            return r.stdout.decode().strip()
        published={}
        for number,target in enumerate(targets):
            env['RUNTIME_MIRROR_CREDENTIAL']='RUNTIME_GITHUB_MIRROR_TOKEN'if target['url'].startswith('https://github.com/')else'RUNTIME_GIT_MIRROR_TOKEN'
            for branch in target['branches']:
                ref='refs/heads/'+branch;rows=git('ls-remote','origin',ref).split()
                if not rows:continue
                commit=rows[0];git('fetch','--no-tags','origin',ref);git('push',target['url'],commit+':'+ref)
                if git('ls-remote',target['url'],ref).split()[0]!=commit:raise ValueError('Runtime mirror readback differs')
                published[str(number)+':'+branch]=commit
        print('Verified mirrored refs: '+', '.join(published),flush=True)


if __name__=='__main__':main()
