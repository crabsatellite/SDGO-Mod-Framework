"""Bound a Manager-owned command and its descendants to its operation lifetime."""
from __future__ import annotations
import os
import signal
import subprocess
import threading
import time
from contextlib import ExitStack


class WindowsJob:
    def __init__(self,process):
        import ctypes as c
        from ctypes import wintypes as w
        class Basic(c.Structure):
            _fields_=[('process_time',c.c_int64),('job_time',c.c_int64),('flags',w.DWORD),
                      ('minimum',c.c_size_t),('maximum',c.c_size_t),('active',w.DWORD),
                      ('affinity',c.c_size_t),('priority',w.DWORD),('scheduling',w.DWORD)]
        class Io(c.Structure):
            _fields_=[(n,c.c_uint64)for n in ('reads','writes','other','read_bytes','write_bytes','other_bytes')]
        class Limits(c.Structure):
            _fields_=[('basic',Basic),('io',Io),('process_memory',c.c_size_t),('job_memory',c.c_size_t),
                      ('peak_process',c.c_size_t),('peak_job',c.c_size_t)]
        self.kernel=c.WinDLL('kernel32',use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes=(c.c_void_p,w.LPCWSTR);self.kernel.CreateJobObjectW.restype=w.HANDLE
        self.kernel.SetInformationJobObject.argtypes=(w.HANDLE,c.c_int,c.c_void_p,w.DWORD)
        self.kernel.AssignProcessToJobObject.argtypes=(w.HANDLE,w.HANDLE)
        self.kernel.CloseHandle.argtypes=(w.HANDLE,)
        self.kernel.TerminateJobObject.argtypes=(w.HANDLE,w.UINT)
        self.kernel.QueryInformationJobObject.argtypes=(w.HANDLE,c.c_int,c.c_void_p,w.DWORD,c.POINTER(w.DWORD))
        self.handle=self.kernel.CreateJobObjectW(None,None)
        if not self.handle:raise c.WinError(c.get_last_error())
        try:
            limits=Limits();limits.basic.flags=0x2000  # KILL_ON_JOB_CLOSE.
            if not self.kernel.SetInformationJobObject(self.handle,9,c.byref(limits),c.sizeof(limits)):raise c.WinError(c.get_last_error())
            if not self.kernel.AssignProcessToJobObject(self.handle,w.HANDLE(int(process._handle))):raise c.WinError(c.get_last_error())
            # The new process is still suspended: no helper can escape before
            # assignment. Resume only this newly created, owned process.
            native=c.WinDLL('ntdll');native.NtResumeProcess.argtypes=(w.HANDLE,);native.NtResumeProcess.restype=c.c_long
            status=native.NtResumeProcess(w.HANDLE(int(process._handle)))
            if status!=0:raise OSError('Could not resume owned command: '+hex(status&0xffffffff))
        except Exception:self.close();raise

    def close(self):
        if self.handle:
            import ctypes as c
            from ctypes import wintypes as w
            class Accounting(c.Structure):
                _fields_=[('user',c.c_int64),('kernel',c.c_int64),('period_user',c.c_int64),('period_kernel',c.c_int64),
                          ('faults',w.DWORD),('total',w.DWORD),('active',w.DWORD),('terminated',w.DWORD)]
            # Closing a kill-on-close job initiates asynchronous termination.
            # Reap the complete owned tree before releasing redirected files.
            self.kernel.TerminateJobObject(self.handle,1)
            deadline=time.monotonic()+15
            try:
                while True:
                    state=Accounting()
                    if not self.kernel.QueryInformationJobObject(self.handle,1,c.byref(state),c.sizeof(state),None):
                        raise c.WinError(c.get_last_error())
                    if state.active==0:break
                    if time.monotonic()>=deadline:raise TimeoutError('Owned command descendants have not finished closing')
                    time.sleep(.01)
            finally:self.kernel.CloseHandle(self.handle);self.handle=None


def run(args,*,cwd,env,timeout=900,stop=None,input=None,output=None,progress=None):
    with ExitStack()as files:
        destination=files.enter_context(open(output,'wb'))if output is not None else subprocess.PIPE
        return _run(args,cwd=cwd,env=env,timeout=timeout,stop=stop,input=input,stdout=destination,progress=progress)


def _run(args,*,cwd,env,timeout,stop,input,stdout,progress=None):
    windows=os.name=='nt'
    process=subprocess.Popen(args,cwd=cwd,env=env,stdin=subprocess.PIPE if input is not None else None,stdout=stdout,stderr=subprocess.PIPE,
        creationflags=(subprocess.CREATE_NO_WINDOW|0x00000004)if windows else 0,
        start_new_session=not windows)
    job=None
    def terminate():
        if job:job.close()
        elif not windows:
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        if process.poll()is None:process.kill()
    try:
        if windows:job=WindowsJob(process)
        deadline=None if timeout is None else time.monotonic()+timeout
        if progress:
            outputs=[];errors=[];failures=[];threads=[]
            def reader(stream,parts,notify=None):
                try:
                    while True:
                        block=stream.read1(64*1024)
                        if not block:break
                        parts.append(block)
                        if notify:notify(block)
                except Exception as error:failures.append(error)
            def writer():
                try:process.stdin.write(input);process.stdin.close()
                except BrokenPipeError:pass
                except Exception as error:failures.append(error)
            threads.append(threading.Thread(target=reader,args=(process.stderr,errors,progress),daemon=True))
            if stdout==subprocess.PIPE:threads.append(threading.Thread(target=reader,args=(process.stdout,outputs),daemon=True))
            if input is not None:threads.append(threading.Thread(target=writer,daemon=True))
            for thread in threads:thread.start()
            while process.poll()is None:
                if stop is not None and stop.is_set():raise InterruptedError('Manager operation cancelled')
                if deadline is not None and time.monotonic()>=deadline:raise subprocess.TimeoutExpired(args,timeout)
                if failures:raise failures[0]
                time.sleep(.05)
            terminate()
            for thread in threads:thread.join(timeout=5)
            if any(thread.is_alive()for thread in threads):raise TimeoutError('Owned progress streams did not close')
            if failures:raise failures[0]
            return subprocess.CompletedProcess(args,process.returncode,b''.join(outputs)if stdout==subprocess.PIPE else None,b''.join(errors))
        observed=0
        def report(stderr):
            nonlocal observed
            if progress:
                data=stderr or b'';progress(data[observed:]);observed=len(data)
        while True:
            if stop is not None and stop.is_set():
                terminate();raise InterruptedError('Manager operation cancelled')
            remaining=None if deadline is None else deadline-time.monotonic()
            if remaining is not None and remaining<=0:
                terminate();raise subprocess.TimeoutExpired(args,timeout)
            try:
                try:out,err=process.communicate(input=input,timeout=.25 if remaining is None else min(.25,remaining))
                finally:input=None
                report(err)
                return subprocess.CompletedProcess(args,process.returncode,out,err)
            except subprocess.TimeoutExpired as waiting:report(waiting.stderr)
    finally:
        terminate()
        if progress:
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:pass
            for stream in(process.stdin,process.stdout,process.stderr):
                if stream:
                    try:stream.close()
                    except OSError:pass
        else:
            try:process.communicate(timeout=5)
            except subprocess.TimeoutExpired:pass

