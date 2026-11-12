"""Manager-owned, idempotent paired installation of bundled unit demo recipes."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import threading
import uuid
from . import storage

SERVICE='units.default-presets'

class DefaultPresets:
    def __init__(self,provider):
        self.provider=provider;self.stop_event=threading.Event();self.thread=None;self.lock=threading.RLock()
        self.status='waiting_configuration';self.error='';self.revision='';self.verified=None
    def state(self):
        with self.lock:return {'running':bool(self.thread and self.thread.is_alive()),'healthy':not self.error,
            'status':self.status,'error':self.error,'revision':self.revision,'grants_units':False,'configures_acquisition':False}
    def start(self):
        if self.thread and self.thread.is_alive():return self.state()
        self.stop_event.clear();self.thread=threading.Thread(target=self._run,name=SERVICE,daemon=True);self.thread.start();return self.state()
    def stop(self):
        self.stop_event.set()
        if self.thread and self.thread is not threading.current_thread():self.thread.join(timeout=120)
        return self.state()
    def _run(self):
        while not self.stop_event.is_set():
            try:self._ensure()
            except Exception as error:
                with self.lock:self.status='pending';self.error=str(error)[:1200]
            self.stop_event.wait(10)
    def _set(self,status,**values):
        with self.lock:self.status=status;self.error='';self.__dict__.update(values)
    def _ensure(self):
        from owner.units import preset_recipe,preset_publication,preset_install,batch_server,batch_registry
        from owner.units.assets import Baseline
        from owner.units.repository import Repository
        with self.provider.lock:c=dict(self.provider.config)
        if not c.get('client') or not c.get('server_ini'):
            self._set('waiting_configuration');return
        client=Path(c['client']).resolve();ini=Path(c['server_ini']).resolve();module=client/'sdgo/mods/unit_catalog'
        if not (module/'mod.ini').is_file() or not (module/'unit_catalog.dll').is_file():
            self._set('waiting_programmes');return
        current=storage.ini_values((module/'mod.ini').read_bytes())
        if tuple(int(n) for n in current.get('mod','version',fallback='0.0.0').split('.')[:3])<(0,6,0):
            self._set('waiting_programmes');return
        recipe=preset_recipe.load();recipe_raw=preset_publication.json_bytes(recipe)
        if hashlib.sha256((client/'GOnline.exe').read_bytes()).hexdigest()!=recipe['host_sha256']:
            self._set('unsupported_target');return
        primary=module/'catalog.bin'
        fingerprint=hashlib.sha256(recipe_raw+(primary.read_bytes() if primary.is_file() else b'')+
            (module/'unit_catalog.dll').read_bytes()+str(ini).casefold().encode()).hexdigest()
        receipt=preset_install.verify(client)
        if receipt and receipt.get('fingerprint')==fingerprint and self.verified==fingerprint:
            self._set('installed',revision=receipt['revision']);return
        root=self.provider._units_root()/'default-presets'
        output=root/fingerprint;publication=output/'publication'
        baseline=Baseline(client);baseline._loose_roots=[]
        if (receipt and receipt.get('fingerprint')==fingerprint and publication.is_dir() and
            batch_server.verify_installed(publication,ini,baseline)):
            # A Manager restart is not a catalogue update. Recheck actual SQL,
            # ledger and client/graph witnesses without requiring maintenance.
            self._set('installed',revision=receipt['revision'],verified=fingerprint);return
        if batch_server.running_services(ini):
            self._set('waiting_stopped_server');return
        # No service is stopped or restarted by this controller. Normal first
        # installation already selects a stopped, configured native group.
        self._set('preparing')
        root.mkdir(parents=True,exist_ok=True)
        if not publication.is_dir():
            prepared=root/('.preparing-'+uuid.uuid4().hex)
            preset_publication.build(prepared,baseline,Repository(server_ini=ini),module/'unit_catalog.dll',recipe)
            prepared.rename(output)
        loaded=batch_server.load_publication(publication)
        preset_install.install(client,publication,fingerprint,dry_run=True)
        review=batch_server.preflight(publication,ini)
        if not review['ready']:raise ValueError('默认演示预设尚不能登记：'+str(review['problems'][:1]))
        with self.provider.lock:
            if dict(self.provider.config)!=c:self._set('waiting_configuration');return
        self._set('registering')
        batch_server.apply(publication,ini,baseline=baseline)
        preset_install.install(client,publication,fingerprint)
        batch_registry.register(self.provider._units_root(),publication,ini)
        self._set('installed',revision=loaded.revision,verified=fingerprint)

def register(provider):
    controller=DefaultPresets(provider)
    provider.workspace.services.register(SERVICE,'默认演示机体预设',controller,controller.start)
    # An explicit saved opt-out remains an opt-out across upgrades. The unit
    # tool's default registration has no shop/gacha/inventory side effects.
    services=provider.workspace.settings.setdefault('services',{})
    if SERVICE not in services:
        services[SERVICE]={'enabled':True};provider.workspace.save_workspace()
    return controller
