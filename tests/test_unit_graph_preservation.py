import unittest
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch
from owner.units.graph_preservation import decode, read, require_no_unrelated
from owner.units.form_graph_runtime import compile_native_files
from owner.units import repository
from test_unit_form_graph_runtime import fixture,prepared,grade_rows

class PreservedGraphs(unittest.TestCase):
    def test_string_server_path_retains_complete_unrelated_owner_from_live_readback(self):
        value=fixture();candidate,authority=compile_native_files(prepared(value),value[2],grade_rows(value[2]))
        class Cursor:
            def execute(self,sql):
                if sql.endswith(' FROM tbl_Units'):
                    return [tuple(row[name] for name in repository.UNIT_COLUMNS) for row in value[2].values()]
                if sql.startswith('SELECT RecordID,FamilyKey,Revision,InstalledHash'):
                    return [(identity,row['family'],row['revision'],row['installed_hash']) for identity,row in value[3].items()]
                raise AssertionError('readback must not write: '+sql)
        connection=Mock();connection.cursor.return_value=Cursor()
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);graph=root/'sdgo/mods/unit_catalog_server/form-graphs.bin'
            auth=root/'sdgo/install-state/unit-form-authority.bin'
            graph.parent.mkdir(parents=True);auth.parent.mkdir(parents=True)
            graph.write_bytes(candidate);auth.write_bytes(authority)
            baseline=SimpleNamespace(root=root/'client',record=lambda kind,identity:value[1].get(identity))
            ini=root/'WorldServer.ini'
            with patch('owner.units.batch_server.battle_hosts',return_value=[{'path':root}]), \
                 patch('owner.units.repository._connect',return_value=connection) as connect:
                result=read(str(ini),SimpleNamespace(graph_requests=[{'owner':'new.family'}]),baseline)
            connect.assert_called_once_with(ini)
            self.assertEqual('example.family',result['requests'][0]['owner'])
            self.assertEqual('source-r1',result['requests'][0]['revision'])
            self.assertEqual(value[1],result['records'])
        connection.rollback.assert_called_once();connection.close.assert_called_once()

    def test_unrelated_graphs_require_client_witnesses_and_unpaired_state_is_refused(self):
        value=fixture();candidate,authority=compile_native_files(prepared(value),value[2],grade_rows(value[2]))
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);graph=root/'sdgo/mods/unit_catalog_server/form-graphs.bin'
            auth=root/'sdgo/install-state/unit-form-authority.bin'
            graph.parent.mkdir(parents=True);auth.parent.mkdir(parents=True)
            graph.write_bytes(candidate)
            publication=SimpleNamespace(graph_requests=[{'owner':'new.demo'}]);hosts=[{'path':root}]
            with self.assertRaisesRegex(ValueError,'不成对'):require_no_unrelated(hosts,publication)
            auth.write_bytes(authority)
            with self.assertRaisesRegex(ValueError,'其他所有者'):require_no_unrelated(hosts,publication)
            publication.graph_requests=[{'owner':'example.family'}]
            require_no_unrelated(hosts,publication)

    def test_roundtrip_preserves_owner_roots_and_all_client_server_witnesses(self):
        value=fixture();source=prepared(value);candidate,authority=compile_native_files(source,value[2],grade_rows(value[2]))
        owners=decode(candidate,authority)
        self.assertEqual(source['owners'],owners)
    def test_mismatched_candidate_authority_and_host_refuse(self):
        value=fixture();candidate,authority=compile_native_files(prepared(value),value[2],grade_rows(value[2]))
        for offset in (0,16,24,88,120,156,160,len(authority)-1):
            with self.subTest(offset=offset):
                corrupt=bytearray(authority);corrupt[offset]^=1
                with self.assertRaises(ValueError):decode(candidate,bytes(corrupt))
        corrupt=bytearray(candidate);corrupt[-1]^=1
        with self.assertRaises(ValueError):decode(bytes(corrupt),authority)
