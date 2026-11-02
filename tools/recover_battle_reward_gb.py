"""Preview or apply a locally prepared, evidence-bound GB compensation plan."""
from pathlib import Path
import argparse,json,sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from owner.rewards import gb_recovery
from owner.rewards.deployment import _connect_billing


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',type=Path,required=True)
    p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--apply',action='store_true')
    args=p.parse_args()
    plan=json.loads(args.plan.read_text(encoding='utf-8-sig'))
    entries=gb_recovery.verify_plan(plan)
    totals={}
    for row in entries:totals[row['source']['account']]=totals.get(row['source']['account'],0)+row['delta_gb']
    result={'method':plan['method'],'entries':len(entries),'average_samples':len(plan['samples']),'planned_gb_by_account':totals,'writes_performed':False}
    if args.apply:
        db=_connect_billing(args.server_ini);db.autocommit=True;db.timeout=30
        try:receipts=gb_recovery.apply(db,plan)
        finally:db.close()
        result.update(writes_performed=True,new_entries=sum(not r['already_applied'] for r in receipts),
            already_applied=sum(r['already_applied'] for r in receipts),
            newly_credited_gb=sum(r['delta_gb'] for r in receipts if not r['already_applied']))
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
