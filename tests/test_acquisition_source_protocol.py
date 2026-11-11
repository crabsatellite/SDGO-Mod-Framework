import copy
import struct
import unittest

from owner.units import acquisition_sources as protocol, compiler


def blueprint_record(output_unit_id=90001):
    raw = bytearray(1197)
    struct.pack_into('<I', raw, 0, 200001)
    name = 'Accepted blueprint'.encode('utf-16-le')
    raw[5:5 + len(name)] = name
    struct.pack_into('<I', raw, 105, 3)
    sprite = 'sprs/recipe.spr'.encode('utf-16-le')
    raw[169:169 + len(sprite)] = sprite
    struct.pack_into('<I', raw, 229, output_unit_id)
    struct.pack_into('<7I', raw, 233, 13003, 16002, 15004, 0, 0, 0, 0)
    struct.pack_into('<7I', raw, 261, 7, 1, 1, 0, 0, 0, 0)
    preview = 'sprs/preview.spr'.encode('utf-16-le')
    raw[289:289 + len(preview)] = preview
    struct.pack_into('<II', raw, 1189, 0, 600)
    return bytes(raw)


def accepted_unit_catalog(*, family_enabled=True, recipe_enabled=True):
    unit = bytearray(4976)
    struct.pack_into('<I', unit, 0, 90001)
    recipe = blueprint_record()
    index = bytearray(614)
    struct.pack_into('<I', index, 0, 0x375BE2)
    index[5] = int(family_enabled)
    struct.pack_into('<II', index, 6, 90001, 200001)
    catalog = compiler.native_catalog([
        (2, 90001, bytes(unit)),
        (33, 200001, recipe),
        (85, 0x375BE2, bytes(index)),
    ])
    recipe_definition = {
        'id': 200001,
        'item_category_id': 1500001,
        'enabled': recipe_enabled,
    }
    definitions = {
        'protocol': 'sdgo.unit-family',
        'schema_version': 1,
        'revision': 'a' * 64,
        'families': [{
            'family_key': 'unit-import.fixture',
            'root_unit_id': 90001,
            'blueprints': [copy.deepcopy(recipe_definition)],
        }],
        'registrations': [{
            'key': 'unit:90001',
            'provider': 'unit_catalog',
            'kind': 'unit',
            'family_key': 'unit-import.fixture',
            'unit_id': 90001,
            'unit_index_id': 0x375BE2,
            'primary_blueprint_id': 200001,
            'enabled': family_enabled,
            'blueprints': [copy.deepcopy(recipe_definition)],
        }],
    }
    return definitions, catalog


class AcquisitionSourceProtocolTests(unittest.TestCase):
    def test_source_rows_are_canonical_and_hash_bound(self):
        document = {
            'schema_version': 1,
            'provider': 'operator_catalog',
            'revision': 'fixture',
            'blueprints': {
                '200002': [{'key': 'operator:2', 'text': '副官掉落：乙'}],
                '200001': [
                    {'key': 'operator:3', 'text': '副官掉落：丙'},
                    {'key': 'operator:1', 'text': '副官掉落：甲'},
                ],
            },
        }
        raw = protocol.encode_sources(document, protocol.OPERATOR_PROVIDER)
        magic, version, header, count, size, digest, provider = struct.unpack_from('<8s6I', raw)
        self.assertEqual((protocol.SOURCE_MAGIC, 1, 32, 3, len(raw)-32, protocol.OPERATOR_PROVIDER),
                         (magic, version, header, count, size, provider))
        self.assertEqual(protocol.fnv1a32(raw[32:]), digest)
        cursor = 32
        observed = []
        for _ in range(count):
            subject, recipe, key_bytes, units = struct.unpack_from('<4I', raw, cursor);cursor += 16
            key = raw[cursor:cursor+key_bytes].decode('ascii');cursor += key_bytes
            text = raw[cursor:cursor+units*2].decode('utf-16-le');cursor += units*2
            observed.append((subject, recipe, key, text))
        self.assertEqual([
            (2, 200001, 'operator:1', '副官掉落：甲'),
            (2, 200001, 'operator:3', '副官掉落：丙'),
            (2, 200002, 'operator:2', '副官掉落：乙'),
        ], observed)
        self.assertEqual(len(raw), cursor)

    def test_shop_rows_include_units_before_blueprints(self):
        document = {
            'schema_version': 1,
            'units': {'10001': [
                {'key': 'shop:unit:10001:rental', 'text': '商城租用'},
            ]},
            'blueprints': {'200001': [
                {'key': 'shop:blueprint:200001', 'text': '商城购买'},
            ]},
        }
        raw = protocol.encode_sources(document, protocol.SHOP_PROVIDER)
        self.assertEqual((1, 10001), struct.unpack_from('<2I', raw, 32))
        _, _, key_bytes, units = struct.unpack_from('<4I', raw, 32)
        second = 32 + 16 + key_bytes + units * 2
        self.assertEqual((2, 200001), struct.unpack_from('<2I', raw, second))

    def test_operator_provider_rejects_unit_subjects(self):
        document = {
            'schema_version': 1,
            'units': {'10001': [
                {'key': 'operator:unit:10001', 'text': '副官掉落：错误机体来源'},
            ]},
            'blueprints': {},
        }
        with self.assertRaisesRegex(ValueError, '只能发布设计图'):
            protocol.encode_sources(document, protocol.OPERATOR_PROVIDER)

    def test_conflicting_stable_key_is_rejected(self):
        document = {'schema_version': 1, 'blueprints': {'200001': [
            {'key': 'shop:one', 'text': '商城购买'},
            {'key': 'shop:one', 'text': '另一个来源'},
        ]}}
        with self.assertRaisesRegex(ValueError, '冲突'):
            protocol.encode_sources(document, protocol.SHOP_PROVIDER)

    def test_availability_rows_bind_catalog_payload_hash(self):
        document = {'schema_version': 1, 'blueprints': {
            '209001': {'state': 2, 'unit_id': 90001},
            '209000': {'state': 1, 'unit_id': 90001},
        }}
        raw = protocol.encode_availability(document, 0x12345678)
        header = struct.unpack_from('<8s6I', raw)
        self.assertEqual((protocol.AVAILABILITY_MAGIC, 1, 32, 2, 24, 0x12345678),
                         (header[0], header[1], header[2], header[3], header[4], header[6]))
        self.assertEqual(protocol.fnv1a32(raw[32:]), header[5])
        self.assertEqual((209000, 90001, 1), struct.unpack_from('<3I', raw, 32))
        self.assertEqual((209001, 90001, 0), struct.unpack_from('<3I', raw, 44))


    def test_accepted_catalog_migration_is_pure_complete_and_hash_bound(self):
        definitions, catalog = accepted_unit_catalog()
        document, binary = protocol.compile_blueprint_availability(definitions, catalog)
        self.assertEqual({
            'state': 1,
            'item_category_id': 1500001,
            'unit_id': 90001,
        }, document['blueprints']['200001'])
        header = struct.unpack_from('<8s6I', binary)
        self.assertEqual(struct.unpack_from('<I', catalog, 24)[0], header[6])
        self.assertEqual((200001, 90001, 1), struct.unpack_from('<3I', binary, 32))

        for family_enabled, recipe_enabled in ((False, True), (True, False)):
            with self.subTest(family_enabled=family_enabled,
                              recipe_enabled=recipe_enabled):
                disabled, accepted = accepted_unit_catalog(
                    family_enabled=family_enabled,
                    recipe_enabled=recipe_enabled)
                migrated, _ = protocol.compile_blueprint_availability(disabled, accepted)
                self.assertEqual(2, migrated['blueprints']['200001']['state'])

    def test_accepted_catalog_migration_rejects_incomplete_or_mismatched_authority(self):
        definitions, catalog = accepted_unit_catalog()
        cases = []
        mismatch = copy.deepcopy(definitions)
        mismatch['registrations'][0]['blueprints'][0]['enabled'] = False
        cases.append(('清单不一致', mismatch, catalog))
        invalid_bool = copy.deepcopy(definitions)
        invalid_bool['registrations'][0]['enabled'] = 1
        cases.append(('启用状态', invalid_bool, catalog))
        missing = copy.deepcopy(definitions)
        missing['registrations'] = []
        cases.append(('覆盖', missing, catalog))
        wrong_output = blueprint_record(90002)
        unit = bytearray(4976);struct.pack_into('<I', unit, 0, 90001)
        index = bytearray(614);struct.pack_into('<I', index, 0, 0x375BE2)
        index[5] = 1;struct.pack_into('<II', index, 6, 90001, 200001)
        mismatched_catalog = compiler.native_catalog([
            (2, 90001, bytes(unit)),
            (33, 200001, wrong_output),
            (85, 0x375BE2, bytes(index)),
        ])
        cases.append(('产物', definitions, mismatched_catalog))
        for message, candidate, raw in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                protocol.compile_blueprint_availability(candidate, raw)


if __name__ == '__main__':
    unittest.main()
