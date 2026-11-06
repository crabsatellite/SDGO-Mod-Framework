"""Mirror verified framework refs using the maintainer's configured Git identity."""
from pathlib import Path
import os,subprocess,sys,tempfile


def main():
    target=os.environ['RUNTIME_GIT_MIRROR_URL'];env=dict(os.environ)
    if not target.startswith('https://')or'@'in target:raise ValueError('Use a credential-free HTTPS mirror URL')
    with tempfile.TemporaryDirectory()as directory:
        token=env.get('RUNTIME_GIT_MIRROR_TOKEN')
        if token:
            ask=Path(directory)/'ask.py';ask.write_text("import os,sys\nprint('git' if 'username' in sys.argv[1].lower() else os.environ['RUNTIME_GIT_MIRROR_TOKEN'])\n",encoding='utf-8')
            # Git executes askpass through its own shell on both Windows and Linux.
            helper=Path(directory)/'ask.sh';helper.write_text('#!/bin/sh\nexec "'+sys.executable.replace('\\','/')+'" "'+ask.as_posix()+'" "$@"\n',encoding='utf-8');helper.chmod(0o700)
            env.update(GIT_ASKPASS=str(helper),GIT_TERMINAL_PROMPT='0',GCM_INTERACTIVE='Never')
        def git(*args):
            r=subprocess.run(['git',*args],env=env,capture_output=True)
            if r.returncode:raise RuntimeError('Runtime mirror Git operation failed: '+r.stderr.decode(errors='replace'))
            return r.stdout.decode().strip()
        published={}
        for branch in ('master','runtime/stable','runtime/beta'):
            ref='refs/heads/'+branch;rows=git('ls-remote','origin',ref).split()
            if not rows:continue
            commit=rows[0];git('fetch','--no-tags','origin',ref);git('push',target,commit+':'+ref)
            if git('ls-remote',target,ref).split()[0]!=commit:raise ValueError('Runtime mirror readback differs')
            published[branch]=commit
        print('Verified mirrored refs: '+', '.join(published),flush=True)


if __name__=='__main__':main()
