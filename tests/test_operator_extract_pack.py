"""Synthetic coverage for execution-free operator pack extraction."""
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

from PIL import Image

from manager.engines.operators import assets, extract_pack, legacy_pack
from owner.collection_capsule.client import encode_gdt


def png():
    output = io.BytesIO()
    Image.new('RGBA', (2, 2), (20, 40, 60, 255)).save(output, 'PNG')
    return output.getvalue()


class OperatorExtractPackTests(unittest.TestCase):
    def fixture(self, root, *, missing_voice=False, missing_profile=False,
                invalid_texture=False, record_size=1671):
        root = Path(root)
        root.mkdir(parents=True)
        (root / 'GPackData.zdx').write_bytes(struct.pack('<6I', 1, 512, 0, 0, 0, 0))
        (root / 'GPackData.zpk').write_bytes(b'')
        rows = []
        for identity, name in ((60001, '目标模板'), (60003, '香港副官')):
            row = bytearray(record_size)
            struct.pack_into('<I', row, 0, identity)
            assets.put_wchar(row, 5, 100, name)
            if identity == 60003:
                assets.put_wchar(row, 105, 50, 'gdrs\\0043.gdt')
                assets.put_wchar(row, 205, 54, 'sprs\\3782.spr')
                assets.put_wchar(row, 933, 50, 'sdrs\\0313.zsd')
                if missing_profile:
                    assets.put_wchar(row, 313, 54, 'sprs\\9998.spr')
                    assets.put_wchar(row, 933, 50, 'sdrs\\9999.zsd')
            rows.append(bytes(row))
        self.write(root, 'gdrs/0255.gdt', encode_gdt(record_size, rows, 0))

        speech = bytearray(710)
        assets.put_wchar(speech, 0, 50, '事件')
        assets.put_wchar(speech, 100, 200, '繁體字幕')
        assets.put_wchar(speech, 502, 50, 'sdrs\\0314.zsd')
        assets.put_wchar(speech, 602, 50, 'sdrs\\0315.zsd')
        self.write(root, 'gdrs/0043.gdt', encode_gdt(710, [bytes(speech)], 0))
        self.write(root, 'sprs/3782.spr', assets.sprite_binary('txrs\\0201.txr', 2, 2))
        self.write(
            root,
            'txrs/0201.txr',
            b'not-a-texture' if invalid_texture else png(),
        )
        self.write(root, 'sdrs/0313.zsd', b'ID3-intro')
        self.write(root, 'sdrs/0314.zsd', b'ID3-first')
        if not missing_voice:
            self.write(root, 'sdrs/0315.zsd', b'ID3-second')
        return root

    @staticmethod
    def write(root, name, raw):
        path = Path(root).joinpath(*name.split('/'))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)

    def test_inspect_collects_both_voice_variants_and_sprite_texture(self):
        with tempfile.TemporaryDirectory() as directory:
            client = self.fixture(Path(directory) / 'client', record_size=1450)
            report = extract_pack.inspect_source(client, 60003)
            self.assertEqual(report['source_name'], '香港副官')
            self.assertEqual(report['operator_record_size'], 1450)
            self.assertEqual(
                [event['voice'] for event in report['voice_events']],
                ['sdrs/0314.zsd', 'sdrs/0315.zsd'],
            )
            self.assertEqual(report['voice_files'], 3)
            self.assertIn('txrs/0201.txr', report['files'])
            self.assertEqual(report['missing_dependencies'], [])
            self.assertEqual(report['package_profile'], report['profile'])
            self.assertEqual(report['native_fallback_dependencies'], [])
            self.assertEqual(report['unavailable_profile_roles'], [])
            self.assertEqual(report['operator_voice_slot_count'], 39)

    def test_extract_requires_explicit_external_new_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = self.fixture(root / 'client')
            before = hashlib.sha256((client / 'gdrs/0043.gdt').read_bytes()).hexdigest()
            output = root / 'share.zip'
            report = extract_pack.extract_pack(
                client,
                60003,
                output,
                target_template=60001,
                pack_id='fixture.hk.operator',
            )
            self.assertTrue(output.is_file())
            self.assertEqual(report['target_template'], 60001)
            self.assertEqual(
                hashlib.sha256((client / 'gdrs/0043.gdt').read_bytes()).hexdigest(),
                before,
            )
            parsed = legacy_pack.Pack(output.read_bytes(), assets.Assets(client))
            self.assertEqual(parsed.profile, {
                'dialogue': 'gdrs/0043.gdt',
                'sprites': ['sprs/3782.spr', '', '', '', '', '', ''],
                'intro_voice': 'sdrs/0313.zsd',
            })
            self.assertEqual(parsed.preferred_template, 60001)
            self.assertEqual(parsed.describe()['base_dependencies'], [])
            with self.assertRaisesRegex(ValueError, '已存在'):
                extract_pack.extract_pack(
                    client, 60003, output, target_template=60001,
                    pack_id='fixture.hk.operator')
            with self.assertRaisesRegex(ValueError, '来源客户端'):
                extract_pack.extract_pack(
                    client, 60003, client / 'share.zip', target_template=60001,
                    pack_id='fixture.hk.operator')

    def test_missing_dependency_is_reported_and_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = self.fixture(root / 'client', missing_voice=True)
            report = extract_pack.inspect_source(client, 60003)
            self.assertEqual(report['missing_dependencies'], ['sdrs/0315.zsd'])
            with self.assertRaisesRegex(ValueError, '资源不完整'):
                extract_pack.extract_pack(
                    client,
                    60003,
                    root / 'share.zip',
                    target_template=60001,
                    pack_id='fixture.hk.operator',
                )
            self.assertFalse((root / 'share.zip').exists())

    def test_explicit_source_fallback_preserves_dangling_dialogue_voice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = self.fixture(root / 'client', missing_voice=True)
            output = root / 'share.zip'
            report = extract_pack.extract_pack(
                client,
                60003,
                output,
                target_template=60001,
                pack_id='fixture.hk.operator',
                allow_source_fallbacks=True,
            )
            self.assertEqual(
                report['native_fallback_dependencies'],
                ['sdrs/0315.zsd'],
            )
            self.assertEqual(report['unavailable_profile_roles'], [])
            parsed = legacy_pack.Pack(output.read_bytes(), assets.Assets(client))
            self.assertEqual(parsed.describe()['base_dependencies'], [])
            self.assertEqual(parsed.describe()['suppressed_audio_dependencies'], ['sdrs/0315.zsd'])

    def test_explicit_source_fallback_blanks_unavailable_profile_roles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = self.fixture(root / 'client', missing_profile=True)
            output = root / 'share.zip'
            report = extract_pack.extract_pack(
                client,
                60003,
                output,
                target_template=60001,
                pack_id='fixture.hk.operator',
                allow_source_fallbacks=True,
            )
            self.assertEqual(report['profile']['sprites'][1], 'sprs/9998.spr')
            self.assertEqual(report['profile']['intro_voice'], 'sdrs/9999.zsd')
            self.assertEqual(report['package_profile']['sprites'][1], '')
            self.assertEqual(report['package_profile']['intro_voice'], '')
            self.assertEqual(report['unavailable_profile_roles'], [
                {'role': 'sprite', 'index': 1, 'path': 'sprs/9998.spr'},
                {'role': 'intro_voice', 'path': 'sdrs/9999.zsd'},
            ])
            parsed = legacy_pack.Pack(output.read_bytes(), assets.Assets(client))
            self.assertEqual(parsed.profile, report['package_profile'])

    def test_invalid_dependency_is_reported_and_never_downgraded_to_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = self.fixture(root / 'client', invalid_texture=True)
            report = extract_pack.inspect_source(client, 60003)
            self.assertIn('txrs/0201.txr', report['invalid_dependencies'])
            with self.assertRaisesRegex(ValueError, '格式不支持'):
                extract_pack.extract_pack(
                    client,
                    60003,
                    root / 'share.zip',
                    target_template=60001,
                    pack_id='fixture.hk.operator',
                    allow_source_fallbacks=True,
                )
            self.assertFalse((root / 'share.zip').exists())


if __name__ == '__main__':
    unittest.main()
