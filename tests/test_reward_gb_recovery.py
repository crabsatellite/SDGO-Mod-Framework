from copy import deepcopy
from pathlib import Path
import sys,unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from owner.rewards import gb_recovery as recovery


def source(gb=3000,stamp='2026-11-02T21:37:07.860',after=100000):
    return {'account':42,'gb':gb,'after':after,'local_time':stamp,'table':'tbls_LogGPInOut20150526'}


class AverageGbRecoveryTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform=='win32','Native LiteSQL uses Windows paths')
    def test_legacy_database_paths_use_instance_data_directory(self):
        directory=Path('E:/selected/LiteSQL/MSSQL/Data')
        self.assertEqual(recovery.resolve_database_files([r'.\..\Data\GDStoreBilling.mdf'],directory),
                         [directory/'GDStoreBilling.mdf'])
        for raw in (r'..\elsewhere\file.mdf',r'..\Data\..\file.mdf'):
            with self.assertRaises(ValueError):recovery.resolve_database_files([raw],directory)
        with self.assertRaises(ValueError):recovery.resolve_database_files([r'.\..\Data\file.mdf'])

    def test_exact_average_normalizes_event_and_floors_once(self):
        samples=[{'source':source(11611),'event_multiplier':2},
            {'source':source(4916,'2026-11-01T18:50:24.230'),'event_multiplier':2}]
        entries=recovery.build_entries(samples,[{'source':source(3000,'2026-11-02T21:38:00.000'),'event_multiplier':2},
            {'source':source(1680,'2026-11-02T22:07:13.460'),'event_multiplier':1}])
        self.assertEqual(entries[0]['target_gb'],8263)
        self.assertEqual(entries[0]['delta_gb'],5263)
        self.assertEqual(entries[1]['target_gb'],4131)
        self.assertEqual(entries[1]['delta_gb'],2451)

    def test_larger_original_reward_is_never_removed(self):
        samples=[{'source':source(22000),'event_multiplier':2}]
        entries=recovery.build_entries(samples,[{'source':source(25920,'2026-11-02T21:16:28.747'),'event_multiplier':2}])
        self.assertEqual(entries[0]['delta_gb'],0)

    def test_duplicate_evidence_and_invalid_native_values_are_rejected(self):
        sample={'source':source(),'event_multiplier':2}
        with self.assertRaises(ValueError):recovery.average([sample,sample])
        with self.assertRaises(ValueError):recovery.build_entries([sample],[sample,sample])
        for bad in (True,-1,65536):
            with self.subTest(value=bad),self.assertRaises(ValueError):recovery.transaction(source(bad))
        malicious=source();malicious['table']='tbls_LogGPInOut;DROP TABLE x'
        with self.assertRaises(ValueError):recovery.transaction(malicious)

    def test_source_key_does_not_depend_on_the_requested_compensation(self):
        a=[{'source':source(6000),'event_multiplier':2}]
        b=[{'source':source(7000),'event_multiplier':2}]
        candidate=[{'source':source(3000,'2026-11-02T21:41:05.487'),'event_multiplier':2}]
        self.assertEqual(recovery.build_entries(a,candidate)[0]['source_key'],recovery.build_entries(b,candidate)[0]['source_key'])

    def test_modified_plan_cannot_override_the_average(self):
        samples=[{'source':source(6000),'event_multiplier':2}]
        sources=[{'source':source(3000,'2026-11-02T21:41:05.487'),'event_multiplier':2}]
        plan={'schema_version':1,'method':'healthy_average_gb','samples':samples,'sources':sources,
            'entries':recovery.build_entries(samples,sources),'expected_scope':'12345678-1234-1234-1234-123456789abc','expected_data_root':'E:/test'}
        recovery.verify_plan(plan)
        changed=deepcopy(plan);changed['entries'][0]['delta_gb']+=1
        with self.assertRaises(ValueError):recovery.verify_plan(changed)


if __name__=='__main__':unittest.main()
