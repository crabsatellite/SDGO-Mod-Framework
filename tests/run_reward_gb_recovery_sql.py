"""Native SQL recovery checks in an existing TEST-ONLY copy; always rolled back."""
from pathlib import Path
import argparse,copy,datetime,json,sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from manager.database import connect
from owner.rewards import gb_recovery as recovery


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--test-root',type=Path,required=True)
    p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--account',type=int,required=True,help='existing unused account in the local test copy')
    a=p.parse_args();test_root=a.test_root.resolve();ini=a.server_ini.resolve()
    if not (test_root/'TEST-ONLY.txt').is_file() or not ini.is_relative_to(test_root):
        raise RuntimeError('Use only the marked native test copy and its own INI')
    db=connect(ini,'DB StoreBilling',autocommit=True);db.timeout=30
    cursor=db.cursor();checks=[]
    def balance():return cursor.execute('SELECT f_GP FROM dbo.tbls_AccountPocket WHERE f_idx=?',a.account).fetchone()[0]
    def countlog():return cursor.execute('SELECT COUNT(*) FROM dbo.tbls_LogGPInOut20150526 WHERE f_idx=?',a.account).fetchone()[0]
    def record(row):return {'account':a.account,'gb':row[0],'after':row[1],'local_time':row[2].isoformat(timespec='milliseconds'),'table':'tbls_LogGPInOut20150526'}
    initial=balance();initial_count=countlog();outer=False
    try:
        paths=[Path(r[0]).resolve() for r in cursor.execute('SELECT physical_name FROM sys.database_files').fetchall()]
        if not paths or any(not path.is_relative_to(test_root) for path in paths):raise RuntimeError('Test SQL still points outside its copy')
        scope=str(cursor.execute('SELECT ScopeId FROM dbo.SdgoBattleRewardWalletMeta WHERE Singleton=1').fetchone()[0]).lower()
        receipt=cursor.execute('SELECT TOP 1 MatchId,PlanHash,AppliedUtc FROM dbo.SdgoBattleRewardMbReceipt WHERE AccountNumber=? ORDER BY AppliedUtc DESC',a.account).fetchone()
        if not receipt:raise RuntimeError('Use an existing test account with a normal historical reward receipt')
        stamp=receipt[2]
        if isinstance(stamp,str):stamp=datetime.datetime.fromisoformat(stamp)
        stamp+=datetime.timedelta(hours=9)
        normal=cursor.execute('SELECT f_GP,f_retGP,f_regdate FROM dbo.tbls_LogGPInOut20150526 WHERE f_idx=? AND f_from=2 AND f_etc=1 AND f_regdate BETWEEN ? AND ?',a.account,stamp-datetime.timedelta(seconds=20),stamp+datetime.timedelta(seconds=20)).fetchall()
        if len(normal)!=1:raise RuntimeError('Normal source is not unique in the test snapshot')
        reference=normal[0]
        sources=cursor.execute('SELECT TOP 2 f_GP,f_retGP,f_regdate FROM dbo.tbls_LogGPInOut20150526 WHERE f_idx=? AND f_from=2 AND f_etc=1 AND f_GP>=0 AND f_GP<? ORDER BY f_regdate DESC',a.account,reference[0]).fetchall()
        if len(sources)!=2:raise RuntimeError('Test copy needs two existing source records below its normal reference')
        samples=[{'source':record(reference),'event_multiplier':2,'match_id':receipt[0],'plan_hash':receipt[1].strip()}]
        def plan(items):return {'schema_version':1,'method':'healthy_average_gb','samples':samples,'sources':items,
            'entries':recovery.build_entries(samples,items),'expected_scope':scope,'expected_data_root':str(test_root)}
        first=[{'source':record(sources[0]),'event_multiplier':2}]
        cursor.execute('BEGIN TRANSACTION;');outer=True
        result=recovery.apply(db,plan(first));delta=result[0]['delta_gb']
        assert delta>0 and balance()==initial+delta and countlog()==initial_count+1
        checks.append('unchanged_native_wallet_procedure_credits_exact_delta')
        second=recovery.apply(db,plan(first))
        assert second[0]['already_applied'] and balance()==initial+delta and countlog()==initial_count+1
        checks.append('replay_does_not_duplicate_credit')
        other={'source':record(sources[1]),'event_multiplier':2}
        stale=copy.deepcopy(other);stale['source']['after']+=1
        before_failed=balance();before_failed_logs=countlog()
        try:recovery.apply(db,plan([other,stale]))
        except ValueError as error:
            assert 'uniquely verified' in str(error),error
        else:raise AssertionError('Stale native financial evidence was accepted')
        assert balance()==before_failed and countlog()==before_failed_logs
        checks.append('failed_later_entry_rolls_back_earlier_credit')
        wrong=plan(first);wrong['expected_data_root']=str(test_root/'not-the-database')
        try:recovery.apply(db,wrong)
        except ValueError as error:assert 'data scope' in str(error),error
        else:raise AssertionError('A foreign SQL data scope was accepted')
        checks.append('foreign_database_scope_rejected')
    finally:
        if outer:cursor.execute('IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;')
        assert balance()==initial and countlog()==initial_count
        cursor.close();db.close()
    checks.append('test_wallet_and_native_logs_restored_by_outer_rollback')
    print(json.dumps({'status':'pass','checks':checks,'new_accounts_created':0,'production_database_used':False},indent=2))


if __name__=='__main__':main()
