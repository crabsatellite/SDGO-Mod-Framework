import copy
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from owner.units import compiler, model
from owner.units.project import export_project
from owner.units.workspace import Workspace
from unit_import_support import Baseline, Repository, archive, complete, png, publisher, wchar


class TemplateTextBaseline(Baseline):
    """The item template carries another unit's 「性能」 and 「使用」 text, as stock categories do."""

    def record(self, kind, identity):
        raw = super().record(kind, identity)
        if kind == 'item' and identity == 1110001:
            raw = bytearray(raw)
            wchar(raw, 214, 'Template unit description')
            wchar(raw, 414, 'Buy Template Unit (Level: Lv7)')
            raw = bytes(raw)
        return raw


def text(raw, offset):
    return raw[offset:offset + 200].decode('utf-16-le').split('\0')[0]


class ItemTextModel(unittest.TestCase):
    def test_missing_text_is_empty_and_values_are_trimmed(self):
        self.assertEqual({'description': '', 'usage': ''}, model.item_text(None))
        self.assertEqual({'description': '双形态近战机体', 'usage': ''},
                         model.item_text({'description': '  双形态近战机体 '}))

    def test_native_capacity_counts_utf16_units(self):
        self.assertEqual(99, len(model.item_text({'usage': '甲' * 99})['usage']))
        with self.assertRaisesRegex(ValueError, '99'):
            model.item_text({'description': '甲' * 100})
        # A character outside the BMP takes two of the field's UTF-16 units.
        with self.assertRaisesRegex(ValueError, '99'):
            model.item_text({'description': '\U0001F680' * 50})

    def test_unknown_keys_control_characters_and_non_text_are_rejected(self):
        for value in ({'caption': 'x'}, {'description': 'a\nb'}, {'usage': 7}, ['description']):
            with self.assertRaises(ValueError):
                model.item_text(value)

    def test_definitions_file_carries_item_text(self):
        patch_value = model.definitions({'schema_version': 1, 'item_text': {'description': '说明'}})
        self.assertEqual({'description': '说明'}, patch_value['item_text'])


class ItemTextWorkspace(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.engines = []

    def tearDown(self):
        for engine in self.engines:
            engine.close()
        self.temp.cleanup()

    def engine(self, name):
        engine = Workspace(self.root / name, baseline=TemplateTextBaseline(), repository=Repository(),
                           publisher=publisher)
        self.engines.append(engine)
        return engine

    def test_empty_description_is_a_warning_that_does_not_block_confirmation(self):
        one = self.engine('one')
        state = complete(one, one.inspect('unit.zip', archive(), one.public()['revision']))
        warning = [row for row in state['draft']['requirements'] if row['code'] == 'item_text']
        self.assertEqual(['warning'], [row['level'] for row in warning])
        self.assertTrue(state['draft']['can_confirm'])
        state = one.update(state['draft']['id'], {'item_text': {'description': '作者说明'}}, state['revision'])
        self.assertFalse(any(row['code'] == 'item_text' for row in state['draft']['requirements']))

    def test_project_round_trip_keeps_text_and_omits_the_key_when_unused(self):
        one = self.engine('one')
        state = complete(one, one.inspect('unit.zip', archive(), one.public()['revision']))
        with zipfile.ZipFile(io.BytesIO(export_project(one, state['draft']))) as z:
            self.assertNotIn('item_text', json.loads(z.read('project.json'))['definitions'])
        value = {'description': '作者说明', 'usage': '所有军阶都可使用.'}
        state = one.update(state['draft']['id'], {'item_text': value}, state['revision'])
        raw = export_project(one, state['draft'])
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            self.assertEqual(value, json.loads(z.read('project.json'))['definitions']['item_text'])
        two = self.engine('two')
        imported = two.inspect('project.zip', raw, two.public()['revision'])
        self.assertEqual(value, imported['draft']['item_text'])

    def compile(self, item_text):
        engine = self.engine('compile-' + str(len(self.engines)))
        state = complete(engine, engine.inspect('unit.zip', archive(), engine.public()['revision']))
        family = copy.deepcopy(engine.read()['drafts'][state['draft']['id']])
        if item_text is not None:
            family['item_text'] = item_text
        family['status'] = 'confirmed'
        family['published'] = copy.deepcopy(family)
        state = engine.read()
        state['drafts'][family['id']] = family
        transaction = self.root / ('transaction-' + str(len(self.engines)))
        transaction.mkdir()
        images = {role: png() for role in ('unit_primary', 'shop', 'custom', 'capsule_fixed', 'index_owned',
                                           'index_missing')}
        with patch.object(compiler, '_adapter_files',
                          return_value=(b'MZ' + bytes(510), b'[mod]\nenabled=0\nfiles=\n', b'symbols', b'installer')), \
             patch('owner.units.presentation.publication_images', return_value=images), \
             patch('owner.units.presentation.contract', return_value={'schema': 3}):
            result = compiler.publish(engine, state, family['id'], transaction)
        with zipfile.ZipFile(result['client_zip']) as z:
            raw = z.read('client/sdgo/mods/unit_catalog/catalog.bin')
        records = {}
        cursor = 32
        for _ in range(struct.unpack_from('<I', raw, 16)[0]):
            kind, identity, size, *_ = struct.unpack_from('<6I', raw, cursor)
            cursor += 24
            records[(kind, identity)] = raw[cursor:cursor + size]
            cursor += size
        item = records[(34, family['category_id'])]
        return item, engine.baseline.record('item', 1110001)

    def test_compiled_item_category_carries_authored_text_not_the_template_text(self):
        item, template = self.compile({'description': '作者说明', 'usage': '所有军阶都可使用.'})
        self.assertEqual('作者说明', text(item, 214))
        self.assertEqual('所有军阶都可使用.', text(item, 414))
        self.assertEqual(bytes(200 - len('作者说明') * 2), item[214 + len('作者说明') * 2:414])
        # Everything outside the identity, name, root and the two text fields stays the template's.
        for start, end in ((4, 5), (109, 214)):
            self.assertEqual(template[start:end], item[start:end])

    def test_without_authored_text_both_fields_are_cleared(self):
        for value in (None, {'description': '', 'usage': ''}):
            item, template = self.compile(value)
            self.assertEqual('Template unit description', text(template, 214))
            self.assertEqual(bytes(400), item[214:614])


if __name__ == '__main__':
    unittest.main()
