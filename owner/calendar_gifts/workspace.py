"""Local policy revisions, separate private lists, and a public-rule export."""
from __future__ import annotations
import datetime as dt
from pathlib import Path
from manager import storage
from . import model


class Workspace:
    def __init__(self,root):
        self.root=Path(root).resolve();self.root.mkdir(parents=True,exist_ok=True)
        self.policy_file=self.root/'calendar-rules.json'
        self.people_file=self.root/'birthdays.private.json'
        self.state_file=self.root/'calendar-state.private.json'
        self.legacy_file=self.root/'calendar-legacy.private.json'

    def policy(self):return model.validate(storage.read_json(self.policy_file,model.defaults()))
    def people(self):return model.validate_private(storage.read_json(self.people_file,model.private_defaults()))
    def today(self,policy=None):
        p=policy or self.policy()
        return dt.datetime.now(dt.timezone(dt.timedelta(minutes=p['utc_offset_minutes']))).date()

    def public(self):
        return {'policy':self.policy(),'private':self.people(),
                'policy_revision':storage.revision(self.policy_file.read_bytes() if self.policy_file.exists() else b''),
                'private_revision':storage.revision(self.people_file.read_bytes() if self.people_file.exists() else b''),
                'today':self.today().isoformat(),'calendar_years':[2025,2040],
                'grades':[{'value':list(g),'title':name} for g,name in model.GRADE_NAMES.items()]}

    def save(self,kind,value,revision):
        if kind not in ('policy','private'):raise ValueError('配置类型无效')
        target=self.policy_file if kind=='policy' else self.people_file
        checked=model.validate(value) if kind=='policy' else model.validate_private(value)
        with storage.LOCK:
            raw=target.read_bytes() if target.exists() else b''
            if revision!=storage.revision(raw):raise storage.Conflict('配置已在另一处改变，请刷新后再保存')
            before=self.policy() if kind=='policy' else None
            # Re-enabling starts today; downtime catch-up never backfills years
            # before this server opted in. Retained SQL receipts remain final.
            if kind=='policy' and checked['enabled'] and not before['enabled']:
                state=storage.read_json(self.state_file,{})
                state['not_before']=self.today(checked).isoformat()
                storage.atomic(self.state_file,storage.json_bytes(state))
            storage.atomic(target,storage.json_bytes(checked))
        return self.public()

    def export(self):
        # Private birthdays/groups, activation dates and delivery receipts are
        # deliberately outside this export boundary.
        value=self.policy();value['enabled']=False
        value['events']=[event for event in value['events'] if event['group']=='all']
        return storage.json_bytes(value)

    def window(self,policy):
        today=self.today(policy)
        with storage.LOCK:
            state=storage.read_json(self.state_file,{})
            if not state.get('not_before'):
                state['not_before']=today.isoformat();storage.atomic(self.state_file,storage.json_bytes(state))
        start=max(dt.date.fromisoformat(state['not_before']),today-dt.timedelta(days=policy['catchup_days']))
        return start,today

    def legacy(self):
        value=storage.read_json(self.legacy_file,{'birthday_done':{}})
        return model.legacy_birthdays(value,self.today())

    def import_legacy(self,path):
        source=Path(path)
        if source.stat().st_size>2*1024*1024:raise ValueError('旧生日回执文件过大')
        rows=model.legacy_birthdays(storage.read_json(source),self.today())
        with storage.LOCK:
            previous=self.legacy()
            # The old script retains the most recent birthday date per account.
            # Keep an already imported later completion when importing an older copy.
            done={str(r['account_id']):r['date'] for r in previous}
            for row in rows:done[str(row['account_id'])]=max(done.get(str(row['account_id']),''),row['date'])
            storage.atomic(self.legacy_file,storage.json_bytes({'birthday_done':done}))
        return len(rows)
