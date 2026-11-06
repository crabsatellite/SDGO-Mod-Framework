"""Cross-process leases shared by every Manager workspace on this machine."""
from __future__ import annotations
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import tempfile
import threading

_local={};_guard=threading.Lock();_held=threading.local()


@contextmanager
def lease(identity):
    key=hashlib.sha256(str(identity).casefold().encode('utf-8')).hexdigest()
    with _guard:lock=_local.setdefault(key,threading.RLock())
    with lock:
        held=getattr(_held,'keys',set())
        if key in held:
            yield;return
        root=Path(os.environ.get('LOCALAPPDATA',tempfile.gettempdir()))/'SDGO/ManagerLocks'
        root.mkdir(parents=True,exist_ok=True);file=(root/(key+'.lock')).open('a+b')
        try:
            file.seek(0,2)
            if not file.tell():file.write(b'\0');file.flush()
            file.seek(0)
            try:
                if os.name=='nt':
                    import msvcrt
                    msvcrt.locking(file.fileno(),msvcrt.LK_NBLCK,1)
                else:
                    import fcntl
                    fcntl.flock(file.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
            except OSError:raise ValueError('另一项变更正在使用这个目标，请完成后重试')from None
            _held.keys=held|{key}
            try:yield
            finally:_held.keys=held
        finally:file.close()
