"""Native GP+GC transaction tests against the selected isolated SQL fixture."""
import copy,hashlib,os,re,unittest
from datetime import datetime,timezone
from pathlib import Path
from owner.rewards.sql_repository import RewardWalletRepository
import test_reward_repository as fixture

@unittest.skipUnless(os.environ.get('SDGO_REWARD_SQL_INTEGRATION')=='1','isolated native SQL required')
class GatheringSql(fixture.RewardRepositorySqlIntegrationTests):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        # Existing native wallets retain compatibility level 100. Reinstall
        # every owned procedure at that level rather than upgrading the database.
        cls.admin.cursor().execute('ALTER DATABASE ['+cls.database+'] SET COMPATIBILITY_LEVEL=100')
        RewardWalletRepository(cls.connection).install()
        source=cls._connect(cls.base['source_database'],autocommit=False)
        try:definition=source.cursor().execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_PocketGP_IO'))").fetchone()[0]
        finally:source.close()
        if not definition:raise RuntimeError('Native test GP procedure is missing')
        cursor=cls.connection.cursor()
        for name in set(re.findall(r'tbls_LogGPInOut[0-9]*',definition,re.I)):
            if not re.fullmatch(r'tbls_LogGPInOut[0-9]*',name,re.I):raise ValueError('Unexpected log table')
            cursor.execute('CREATE TABLE dbo.['+name+'](f_idx int,f_GP int,f_from smallint,f_etc smallint,f_retGP int)')
        cursor.execute(definition);cls.connection.commit()

    def setUp(self):
        super().setUp();c=self.connection.cursor()
        c.execute('DELETE dbo.SdgoBattleGatheringReceipt');c.execute('DELETE dbo.SdgoBattleGatheringPeriod')
        c.execute('UPDATE dbo.tbls_AccountPocket SET f_GP=2000 WHERE f_idx=900000001');self.connection.commit()

    def job(self,key='realm.one'):
        return {'match_id':key,'account_number':900000001,'expected_scope':self.scope,
                'source_hash':hashlib.sha256(key.encode()).hexdigest(),'policy_revision':'b'*64}
    def award(self,**changes):return {'id':'meetup','period':'week','period_key':'2026-11-02','gb':100000,'mb':30000,**changes}
    def balances(self):
        row=self.connection.cursor().execute('SELECT f_GP,f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=900000001').fetchone();self.connection.rollback();return tuple(row)

    def test_large_GP_bonus_uses_both_native_wallets_once_across_servers(self):
        first=self.repository.apply_gathering(self.job(),self.award())
        self.assertEqual(first,{'gb':100000,'mb':30000});self.assertEqual(self.balances(),(102000,31000))
        self.assertEqual(self.repository.current_gp(900000001),102000)
        self.assertEqual(self.repository.apply_gathering(self.job(),self.award()),first)
        self.assertEqual(self.repository.apply_gathering(self.job('other.realm'),self.award()),{'gb':0,'mb':0})
        self.assertEqual(self.balances(),(102000,31000))

    def test_period_identity_is_canonical_and_independent_of_session_dateformat(self):
        cursor=self.connection.cursor();cursor.execute('SET DATEFORMAT dmy');self.connection.commit()
        try:
            for value in ('2026-02-31','2026-1-02','2026/11/16','tomorrow'):
                with self.subTest(value=value),self.assertRaises(self.pyodbc.Error):
                    self.repository.apply_gathering(self.job(),self.award(period_key=value))
            self.assertEqual(self.balances(),(2000,1000))
            self.assertEqual(self.repository.apply_gathering(self.job(),self.award(period_key='2026-11-16')),
                             {'gb':100000,'mb':30000})
        finally:
            cursor.execute('SET DATEFORMAT mdy');self.connection.commit()

    def test_GC_failure_rolls_back_prior_GP_credit_and_period_claim(self):
        c=self.connection.cursor();c.execute('UPDATE dbo.tbls_AccountPocket SET f_GC=2147483640 WHERE f_idx=900000001');self.connection.commit()
        with self.assertRaises(self.pyodbc.Error):self.repository.apply_gathering(self.job(),self.award())
        self.assertEqual(self.balances(),(2000,2147483640))
        c.execute('UPDATE dbo.tbls_AccountPocket SET f_GC=1000 WHERE f_idx=900000001')
        c.execute("CREATE TRIGGER dbo.RejectGatheringFixture ON dbo.SdgoBattleGatheringReceipt AFTER INSERT AS THROW 51995,'fixture rollback',1;")
        self.connection.commit()
        try:
            with self.assertRaises(self.pyodbc.Error):self.repository.apply_gathering(self.job(),self.award())
            self.assertEqual(self.balances(),(2000,1000))
        finally:c.execute('DROP TRIGGER dbo.RejectGatheringFixture');self.connection.commit()
        self.assertEqual(self.repository.apply_gathering(self.job(),self.award()),{'gb':100000,'mb':30000})

    def test_new_account_can_claim_after_peers_claimed_and_period_resets(self):
        self.repository.apply_gathering(self.job(),self.award())
        self.connection.cursor().execute('INSERT dbo.tbls_AccountPocket VALUES(900000003,1000,0)');self.connection.commit()
        try:
            j=self.job('late.friend');j['account_number']=900000003
            self.assertEqual(self.repository.apply_gathering(j,self.award()),{'gb':100000,'mb':30000})
            self.assertEqual(self.repository.apply_gathering(self.job('next.week'),self.award(period_key='2026-11-09')),{'gb':100000,'mb':30000})
        finally:self.connection.cursor().execute('DELETE dbo.tbls_AccountPocket WHERE f_idx=900000003');self.connection.commit()

    def test_concurrent_realms_and_changed_retry_do_not_duplicate(self):
        from concurrent.futures import ThreadPoolExecutor
        def pay(i):
            con=self._connect(self.database,autocommit=False)
            try:return RewardWalletRepository(con).apply_gathering(self.job('realm.'+str(i)),self.award())
            finally:con.close()
        with ThreadPoolExecutor(max_workers=4) as pool:results=list(pool.map(pay,range(4)))
        self.assertEqual(sum(r['gb'] for r in results),100000);self.assertEqual(sum(r['mb'] for r in results),30000)
        j=self.job('realm.0');j['source_hash']='c'*64
        with self.assertRaises(self.pyodbc.Error):self.repository.apply_gathering(j,self.award())
        self.assertEqual(self.balances(),(102000,31000))

if __name__=='__main__':unittest.main()
