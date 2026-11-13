import io
import struct
import unittest

from PIL import Image

from owner.units.effect_resources import inspect_texture, scope_textures, texture_carriers


def effect(texture=b'2162.txr', *, version=5, emitter_type=0, model=b''):
    raw = bytearray(105)
    raw[:10] = b'ZOAEFFECT\0'
    raw[11] = version
    raw += struct.pack('<II', 0, 1)
    block = bytearray(550)
    struct.pack_into('<I', block, 0, emitter_type)
    block[4:4 + len(texture)] = texture
    raw += block + bytes(16)
    if version > 4:
        raw += bytes(5)
    if version > 1:
        raw += b'\0'
    if version > 2 and emitter_type == 2:
        raw += bytes(3) + model.ljust(32, b'\0')
    if version > 3:
        raw += b'\0'
    return bytes(raw)


def texture(color='red', fmt='JPEG'):
    output = io.BytesIO()
    Image.new('RGB', (8, 4), color).save(output, fmt)
    return output.getvalue()


class EffectResources(unittest.TestCase):
    def test_known_final_emitter_eof_preserves_exact_short_source(self):
        short=effect(emitter_type=1)[:-11]
        parsed=texture_carriers(short)
        self.assertEqual('native-final-emitter-subdivision-eof-v1',parsed['tail_profile'])
        self.assertEqual(len(short),parsed['parsed_bytes'])
        self.assertEqual([],parsed['models'])
        output=scope_textures('example',{'fxrs/example_short.zfx':short},lambda _:texture(),lambda _:None)
        rewritten=output['files'][output['manifest']['effects'][0]['target']]
        self.assertEqual(len(short),len(rewritten))
        self.assertEqual(parsed['tail_profile'],texture_carriers(rewritten)['tail_profile'])
        for bad in (short[:-1],short+b'\0',effect(emitter_type=2,model=b'unit.mod')[:-11]):
            with self.subTest(bytes=len(bad)),self.assertRaises(ValueError):texture_carriers(bad)

    def test_source_plus_leaf_is_scoped_without_changing_public_path_grammar(self):
        raw=effect(b'example+1.txr')
        self.assertEqual('txrs/example+1.txr',texture_carriers(raw)['textures'][0]['texture'])
        seen=[]
        def read(path):seen.append(path);return texture()
        output=scope_textures('example',{'fxrs/example_plus.zfx':raw},read,lambda _:None)
        self.assertEqual(['txrs/example+1.txr'],seen)
        self.assertTrue(all('+'not in path for path in output['files']))

    def test_typed_carrier_stops_at_first_nul_and_keeps_post_nul_evidence(self):
        raw = effect(b'2162.txr\0ignored.txr')
        carrier = texture_carriers(raw)['textures'][0]
        self.assertEqual('txrs/2162.txr', carrier['texture'])
        self.assertIn(b'ignored.txr', bytes.fromhex(carrier['source_hex']))

    def test_all_supported_versions_parse_to_exact_end(self):
        for version in range(1, 6):
            with self.subTest(version=version):
                raw = effect(version=version)
                self.assertEqual(len(raw), texture_carriers(raw)['parsed_bytes'])

    def test_scopes_same_logical_texture_variants_without_overwriting_baseline(self):
        raw = effect(b'4667.txr')
        original = texture('red')
        variant = texture('blue')
        baseline_reads = []
        def existing(path):
            baseline_reads.append(path)
            self.assertNotEqual('txrs/4667.txr', path)
            return None
        a = scope_textures('example', {'fxrs/a.zfx': raw}, lambda _: original, existing)
        b = scope_textures('example', {'fxrs/a.zfx': raw}, lambda _: variant, existing)
        self.assertNotEqual(a['manifest']['textures'][0]['target'], b['manifest']['textures'][0]['target'])
        self.assertNotEqual(a['manifest']['effects'][0]['target'], b['manifest']['effects'][0]['target'])
        self.assertEqual(original, a['files'][a['manifest']['textures'][0]['target']])
        self.assertTrue(baseline_reads)
        self.assertFalse(a['manifest']['runtime_registration_authorized'])

    def test_rewrites_only_typed_carrier_bytes_and_reparses(self):
        raw = effect(b'2162.txr\0literal-scanner-token')
        output = scope_textures('example', {'fxrs/a.zfx': raw}, lambda _: texture(), lambda _: None)
        binding = output['manifest']['effects'][0]
        patched = output['files'][binding['target']]
        edge = binding['texture_edges'][0]
        start = edge['offset']
        encoded = (edge['target'].split('/')[1] + '\0').encode('ascii')
        self.assertEqual(raw[:start], patched[:start])
        self.assertEqual(raw[start + len(encoded):], patched[start + len(encoded):])
        self.assertEqual(edge['target'], texture_carriers(patched)['textures'][0]['texture'])
        self.assertLessEqual(len(binding['target']), 29)

    def test_deduplicates_effects_and_textures_but_keeps_each_logical_binding(self):
        output = scope_textures('example', {'fxrs/a.zfx': effect(), 'fxrs/b.zfx': effect()},
                                lambda _: texture(), lambda _: None)
        self.assertEqual(2, len(output['files']))
        self.assertEqual(2, len(output['manifest']['effects']))
        self.assertEqual(1, len(output['manifest']['textures']))

    def test_rejects_destination_conflict_and_malformed_or_unclosed_effects(self):
        with self.assertRaisesRegex(ValueError, 'overwrite'):
            scope_textures('example', {'fxrs/a.zfx': effect()}, lambda _: texture(), lambda _: b'existing')
        for raw in (effect()[:-1], effect() + b'extra', effect(b'x' * 32),
                    effect(b'../outside.txr'), effect(version=6)):
            with self.subTest(raw=raw[-8:]), self.assertRaises(ValueError):
                texture_carriers(raw)
        with self.assertRaisesRegex(ValueError, 'model adapter'):
            scope_textures('example', {'fxrs/a.zfx': effect(emitter_type=2, model=b'unit.mod')},
                           lambda _: texture(), lambda _: None)

    def test_jpeg_texture_is_preserved_and_decoded(self):
        raw = texture()
        result = inspect_texture(raw)
        self.assertEqual(('JPEG', 8, 4, len(raw)),
                         (result['format'], result['width'], result['height'], result['bytes']))
        with self.assertRaises(Exception):
            inspect_texture(b'broken texture')


if __name__ == '__main__':
    unittest.main()
