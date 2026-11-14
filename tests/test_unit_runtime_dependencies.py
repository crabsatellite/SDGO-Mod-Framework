"""Import chains require the matching native missing-animation recovery."""
import json
from pathlib import Path
import unittest
from manager import dependencies

ROOT=Path(__file__).resolve().parents[1]


class RuntimeDependenciesTests(unittest.TestCase):
    def setUp(self):
        self.catalog=json.loads((ROOT/'mods/unit_catalog/manager.json').read_bytes())
        self.recovery=json.loads((ROOT/'mods/transform_input_fix/manager.json').read_bytes())

    def node(self,identity,version,contract,**kwargs):
        return dict(id=identity,host='fixture-client',role='client',version=version,
            contract=contract,enabled=True,settings={},**kwargs)

    def test_required_only_when_form_chain_runtime_is_enabled(self):
        dependencies.validate(self.catalog);dependencies.validate(self.recovery)
        node=self.node('unit_catalog','0.7.0',self.catalog)
        node['settings']['form_chains']=True
        self.assertEqual('required',dependencies.issues([node])[0]['kind'])
        node['settings']['form_chains']=False
        self.assertEqual([],dependencies.issues([node]))

    def test_old_disabled_or_missing_capability_recovery_refuses_chain_enablement(self):
        node=self.node('unit_catalog','0.7.0',self.catalog);node['settings']['form_chains']=True
        peer=self.node('transform_input_fix','1.1.0',self.recovery)
        self.assertEqual([],dependencies.issues([node,peer]))
        for version,enabled,contract in (('1.0.0',True,self.recovery),('1.1.0',False,self.recovery),
                ('1.1.0',True,{'id':'transform_input_fix','settings':[]})):
            with self.subTest(version=version,enabled=enabled):
                changed=dict(peer,version=version,enabled=enabled,contract=contract)
                self.assertTrue(dependencies.issues([node,changed]))


if __name__=='__main__':unittest.main()
