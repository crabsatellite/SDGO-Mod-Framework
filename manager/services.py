"""Manager-owned background capabilities; never start or stop game processes."""
import threading
import re
from pathlib import Path


class WalletService:
    def __init__(self):
        self.thread = None
        self.stop_event = threading.Event()
        self.error = ''
        self.healthy = False
        self.recovering = False
        self.reconnects = 0
        self.restart_handoff = threading.Event()

    def state(self):
        return {'running': bool(self.thread and self.thread.is_alive()), 'healthy': bool(self.thread and self.thread.is_alive() and self.healthy), 'error': self.error,
                'recovering':bool(self.thread and self.thread.is_alive() and self.recovering),
                'reconnects':self.reconnects,'ownership': 'manager_process', 'delivery': 'native_MB_ledger'}

    def prepare_restart(self):
        self.restart_handoff.set()

    def start(self, ini, runtime):
        if self.thread and self.thread.is_alive(): return self.state()
        if not ini or not Path(ini).is_file(): raise ValueError('请先选择包含 DB StoreBilling 的原生服务器 INI')
        runtime = Path(runtime)
        if runtime.is_symlink(): raise ValueError('运行目录不能是目录链接')
        # A recent heartbeat is a bounded readiness lease, not proof of a live
        # owner. The OS lock excludes simultaneous workers and allows a verified
        # update successor to resume immediately after the old owner releases it.
        runtime.mkdir(parents=True,exist_ok=True)
        lease=(runtime/'.manager-wallet.lock').open('a+b')
        try:
            import msvcrt
            lease.seek(0,2)
            if not lease.tell(): lease.write(b'\0');lease.flush()
            lease.seek(0)
            msvcrt.locking(lease.fileno(),msvcrt.LK_NBLCK,1)
        except Exception:
            lease.close()
            raise ValueError('另一个 Manager 或奖励入口拥有此工作区') from None
        self.stop_event.clear(); self.error = ''; self.healthy = False
        self.restart_handoff.clear();self.recovering=False;self.reconnects=0
        ready = threading.Event()
        def run():
            connection = worker = None
            scope=None;delay=1
            try:
                from .database import connect
                from owner.rewards.sql_repository import RewardWalletRepository
                from owner.rewards.worker import WalletWorker
                from owner.rewards.wallet_binding import publish
                while not self.stop_event.is_set():
                    try:
                        connection=connect(ini,'DB StoreBilling')
                        worker=WalletWorker(runtime,RewardWalletRepository(connection))
                        if scope is not None and worker.scope!=scope:
                            raise ValueError('Wallet scope changed during reconnection')
                        publish(runtime,worker.scope);scope=worker.scope
                        self.recovering=False
                        while not self.stop_event.is_set():
                            self.healthy=worker.poll()
                            self.error='' if self.healthy else '钱包暂未就绪：'+worker.last_error
                            ready.set()
                            if not self.healthy and re.search(r':(?:08[A-Z0-9]{3}|HYT0[01])(?:$|:)',worker.last_error):
                                raise ConnectionError(worker.last_error)
                            if self.healthy:delay=1
                            if self.stop_event.wait(1):break
                    except Exception as error:
                        self.healthy=False;self.recovering=True
                        code=re.search(r'\b(?:08[A-Z0-9]{3}|HYT0[01])\b',str(error))
                        self.error='钱包连接或运行时暂不可用，正在自动重连（'+type(error).__name__+((' / '+code.group(0)) if code else '')+'）'
                        ready.set();self.reconnects+=1
                    finally:
                        if connection:
                            try:connection.close()
                            except Exception:pass
                            connection=None
                    if self.stop_event.is_set():break
                    if self.stop_event.wait(delay):break
                    delay=min(delay*2,15)
            finally:
                was_healthy=self.healthy
                self.healthy = False
                self.recovering=False;ready.set()
                try:
                    if worker and not (self.restart_handoff.is_set() and was_healthy):
                        worker.last_error = 'wallet_worker_stopped'
                        worker.heartbeat(False)
                finally:
                    lease.close()
        self.thread = threading.Thread(target=run, name='sdgo-wallet', daemon=True)
        self.thread.start(); ready.wait(20)
        return self.state()

    def stop(self):
        self.stop_event.set()
        if self.thread: self.thread.join(timeout=20)
        return self.state()
