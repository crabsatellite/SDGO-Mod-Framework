"""The editor's final compiled records must feed the real batch graph consumer."""
from pathlib import Path
import hashlib
import io
import json
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from owner.units import batch_server,form_publication
from owner.units.compiler import native_catalog
from owner.units.workspace import Workspace
from unit_import_support import Baseline,Repository,archive,complete,png,unit,native_dll


class FormPublicationTests(unittest.TestCase):
    def fixture(self,targets):
        records={identity:unit(identity,target) for identity,target in targets.items()}
        family={'family_key':'example.directed','root_unit_id':min(targets),
            'behavior_profile':'native-directed-forms-v1','forms':[{'id':n} for n in targets]}
        plan={'family_key':family['family_key'],'revision':'old','root_unit_id':min(targets),
            'item_template_id':1110001,'units':[{'UnitNumber':n} for n in targets],'weapons':[],
            'item':{'CatalogueID':1119001,'RefIndex':min(targets),'Name':'Example'},'names':[]}
        return family,plan,native_catalog([(2,n,raw) for n,raw in records.items()])

    def test_portable_plan_preserves_every_node_edge_root_and_final_hash(self):
        targets={26001:26002,26002:26003,26003:26002,26004:26003}
        family,plan,catalog=self.fixture(targets)
        members=form_publication.build([family],[plan],catalog,'a'*64)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for name,raw in members.items():
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(raw)
            publication=batch_server.load_publication(root)
        request,=publication.graph_requests
        self.assertEqual([26001],request['roots'])
        self.assertEqual(set(targets),{row['id'] for row in request['records']})
        for row in request['records']:
            raw=publication.records[2][row['id']]
            self.assertEqual(hashlib.sha256(raw).hexdigest(),row['client_sha256'])
            self.assertEqual(targets[row['id']],struct.unpack_from('<I',raw,316)[0])
        self.assertNotIn('native_registration_authorized',request)

    def test_external_target_is_refused_without_fabricating_authority(self):
        family,plan,catalog=self.fixture({26001:26002,26002:26003})
        with self.assertRaisesRegex(ValueError,'全部形态'):
            form_publication.build([family],[plan],catalog,'a'*64)

    def test_editor_confirm_exports_same_catalog_for_client_and_server(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);binary=root/'fixture.dll';binary.write_bytes(native_dll(directed=True))
            workspace=Workspace(root/'workspace',baseline=Baseline(),repository=Repository(),module_binary=binary)
            try:
                with zipfile.ZipFile(io.BytesIO(archive())) as source:
                    files={name:source.read(name) for name in source.namelist() if name!='gdrs/unit.bin'}
                for identity,target in {26001:26002,26002:26003,26003:26004,26004:26001}.items():
                    files[f'gdrs/{identity}.bin']=unit(identity,target)
                output=io.BytesIO()
                with zipfile.ZipFile(output,'w') as archive_out:
                    for name,raw in files.items():archive_out.writestr(name,raw)
                state=workspace.inspect('example.zip',output.getvalue(),workspace.public()['revision'])
                identity=state['draft']['id']
                state=workspace.update(identity,{'root_unit_id':26001,'behavior_profile':'native-directed-forms-v1'},state['revision'])
                state=complete(workspace,state);draft=state['draft']
                binary.write_bytes(native_dll())
                refused=workspace.public(identity)['draft']
                self.assertFalse(refused['can_confirm'])
                self.assertTrue(any(row['code']=='runtime_directed' for row in refused['requirements']))
                with self.assertRaisesRegex(ValueError,'有向多形态能力'):
                    workspace.confirm(identity,{'preview_revision':draft['content_revision'],'shop':True,'capsule':True},state['revision'])
                binary.write_bytes(native_dll(directed=True))
                with patch('owner.units.presentation.capsule_card',return_value=png()):
                    state=workspace.confirm(identity,{'preview_revision':draft['content_revision'],'shop':True,'capsule':True},state['revision'])
                publication=batch_server.load_publication(state['draft']['release']['server_publication'])
                with zipfile.ZipFile(io.BytesIO(workspace.export(identity))) as exported:
                    catalog=exported.read('client/sdgo/mods/unit_catalog/catalog.bin')
                    self.assertFalse(any('server-plan' in name or 'server-publication' in name or 'form-graph-requests' in name for name in exported.namelist()))
                with zipfile.ZipFile(io.BytesIO(workspace.export_server(identity))) as exported:
                    self.assertEqual(catalog,exported.read(batch_server.CATALOG))
                    request=json.loads(exported.read('form-graph-requests.json'))['requests'][0]
                    self.assertEqual(4,len(request['records']))
                    self.assertEqual(publication.revision,state['draft']['release']['revision'])
                    self.assertTrue(all(row['client_sha256']==hashlib.sha256(publication.records[2][row['id']]).hexdigest() for row in request['records']))
                path=Path(state['draft']['release']['server_zip']);path.write_bytes(b'modified')
                with self.assertRaisesRegex(ValueError,'被修改'):workspace.export_server(identity)
            finally:workspace.close()


if __name__=='__main__':unittest.main()
