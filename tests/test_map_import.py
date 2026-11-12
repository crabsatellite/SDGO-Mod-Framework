"""Map and mission import protocol: package validation, compile, checks, install and workspace (no client/SQL)."""
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PIL import Image  # noqa: E402

from owner.maps import bundle, compiler, deployment, mission as M, occlusion, package as P  # noqa: E402
from owner.maps.workspace import MapWorkspace, WorkspaceError  # noqa: E402


def png(image):
    out = io.BytesIO()
    image.save(out, format='PNG')
    return out.getvalue()


def spawn(x, z, y=100.0):
    return {'position': [x, y, z], 'facing': [0, 1]}


def write_package(root: Path, *, key='test.flat-plain', mission=None, extra_objects=()) -> Path:
    files = {
        'terrain/height.png': png(Image.new('L', (257, 257), 0)),
        'terrain/ground.png': png(Image.new('RGBA', (64, 64), (90, 120, 60, 255))),
        'images/preview.png': png(Image.new('RGB', (64, 64), (80, 80, 80))),
        'images/minimap.png': png(Image.new('RGB', (128, 128), (60, 90, 60))),
    }
    if mission is not None:
        files['missions/m.json'] = json.dumps(mission, ensure_ascii=False).encode('utf-8')
    manifest = {
        'schema': 'sdgo.map-package', 'schema_version': 1, 'map_key': key,
        'name': {'zh': '测试平原', 'en': 'Test Plain'}, 'modes': ['normal'], 'servers': ['beginner'],
        'terrain': {'half_extent': 4096, 'height': {'image': 'terrain/height.png', 'min': 0, 'max': 0},
                    'layers': [{'texture': 'terrain/ground.png', 'tiling': 32}],
                    'lighting': {'bake_shadows': False}},
        'objects': list(extra_objects),
        'spawns': {'team_a': [spawn(-1500, -300 + 120 * i) for i in range(6)],
                   'team_b': [spawn(1500, -300 + 120 * i) for i in range(6)],
                   'shared': [spawn(0, 0)]},
        'presentation': {'preview': 'images/preview.png', 'minimap': 'images/minimap.png',
                         'bgm': {'native': 'BGM\\0001.mp3'}},
    }
    for name, raw in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(raw)
    manifest['files'] = {name: hashlib.sha256(raw).hexdigest() for name, raw in files.items()}
    (root / 'map.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
    return root


def templates():
    def row(size, ident):
        raw = bytearray(size)
        struct.pack_into('<I', raw, 0, ident)
        return bytes(raw)
    npc_data = {500: {'NpcDataID': 500, 'NpcName': 'Base', 'UseUnitID': 1001, 'UseUnitHP': 3000, 'NpcPoint': 10,
                      'ObjectHpUp': 0, 'NpcType': 2, 'AddUnitEXP': 10, 'AddPoint': 10, 'BossFlag': 0,
                      'NpcUnitLevel': 1, 'ItemBuffNumber': 0, 'CreateUnitHp': 0, 'DestroyUnitHp': 0}}
    npc = bytearray(598)
    struct.pack_into('<I', npc, 0, 500)
    struct.pack_into('<I', npc, 105, 1001)
    return M.Templates(game_mode=row(1090, 88006), clear=row(391, 610001), routes={1: row(34, 195001)},
                       objective=row(634, 197001), catalogue=row(1309, 191001), speech=row(506, 3229001),
                       npc_templates={500: bytes(npc)}, npc_data=npc_data, spawn=row(152, 620001),
                       unit_labels=lambda unit: {'wait_01', 'win'})


def mission_spec():
    return {
        'schema': 'sdgo.map-mission', 'schema_version': 1, 'mission_key': 'test.flat-plain.drill',
        'name': {'zh': '演习'}, 'description': {'zh': '击破靶机'}, 'briefing': {'zh': ['击破全部靶机']},
        'difficulties': [{'level': 1, 'required_rank': 1, 'time_limit': 600, 'lives': 3}],
        'bgm': {'native': 'BGM\\0001.mp3'},
        'npcs': {'target': {'template': 500}, 'ally': {'template': 500, 'hp': 5000}},
        'lines': {'l1': {'portrait': 'sprs\\0001.spr', 'speaker': {'zh': '教官'}, 'text': {'zh': '开始演习。'}}},
        'phases': [
            {'key': 'one', 'objective': {'zh': '击破靶机'}, 'slots': {'ally': 1, 'enemy': 4},
             'spawns': [{'npc': 'ally', 'side': 'ally', 'positions': [[-200, 150, 0]], 'at': 1},
                        {'npc': 'target', 'positions': [[300, 150, 300], [-300, 150, 300]], 'at': 1, 'every': 2}],
             'goal': {'kill': [{'npc': 'target', 'count': 2}]}, 'fail': [{'time_limit': 300}]},
            {'key': 'two', 'objective': {'zh': '坚守'}, 'enter': {'lines': ['l1']},
             'spawns': [{'npc': 'target', 'positions': [[0, 150, 600]], 'at': 2}],
             'goal': {'survive': 30}}],
        'opening': 'intro',
        'cutscenes': {'intro': {'length': 3.0, 'actors': {'a': {'npc': 'target', 'at': [0, 50, 200]}},
                                'camera': [{'t': 0, 'pos': [0, 300, -600], 'look': [0, 50, 200]},
                                           {'t': 3, 'pos': [200, 250, -400], 'look': [0, 50, 200]}],
                                'events': [{'t': 0.5, 'line': 'l1'}, {'t': 1.0, 'actor': 'a', 'anim': 'win'}]}},
    }


def ids():
    return M.MissionIds(group=40, catalogue=191040, route_first=195129, objective_first=197128,
                        game_mode_first=88072, clear_first=610142, spawn_first=2910001, mode_start_first=2900031,
                        npc_first=97621, speech_first=3229324, lua_prefix='SDGOM1')


class PackageTests(unittest.TestCase):
    def test_description_length_reserves_the_bloom_parameter(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp))
            path = root / 'map.json'
            manifest = json.loads(path.read_text(encoding='utf-8'))
            manifest['description'] = {'zh': '图' * 99}
            path.write_text(json.dumps(manifest), encoding='utf-8')
            self.assertEqual(P.load(root).description['zh'], '图' * 99)
            manifest['description']['zh'] += '图'
            path.write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, 'description.zh'):
                P.load(root)

    def test_valid_package_loads_and_digest_is_enforced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp))
            pkg = P.load(root)
            self.assertEqual(pkg.map_key, 'test.flat-plain')
            self.assertEqual(len(pkg.spawns['team_a']), 6)
            (root / 'terrain/ground.png').write_bytes(png(Image.new('RGBA', (64, 64), (1, 2, 3, 255))))
            with self.assertRaises(P.MapPackageError):
                P.load(root).read('terrain/ground.png')

    def test_spawn_outside_terrain_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp))
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['spawns']['shared'] = [spawn(9000, 0)]
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaises(P.MapPackageError):
                P.load(root)


class CompileTests(unittest.TestCase):
    def test_map_description_preserves_bloom_threshold_and_radar_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = P.load(write_package(Path(tmp)))
            pkg.description['zh'] = '图' * 99
            for variant in ('pvp', 'mission'):
                row = compiler.map_row(pkg, 143, variant, 'Trns\\m143.trn', 'sprs\\m143p.spr',
                                       'Txrs\\m143r.txr', 'BGM\\0001.mp3')
                self.assertEqual(len(row), compiler.MAP_ROW_BYTES)
                self.assertEqual(row[0x20B:0x2D3].decode('utf-16-le'), '图' * 99 + '\0')
                self.assertAlmostEqual(struct.unpack_from('<f', row, 0x2D3)[0], 0.85)
                self.assertEqual(row[0x2D7:0x33B].decode('utf-16-le').rstrip('\0'), 'Txrs\\m143r.txr')
            pkg.description['zh'] += '图'
            with self.assertRaisesRegex(compiler.MapCompileError, 'description.zh'):
                compiler.map_row(pkg, 143, 'mission', 'Trns\\m143.trn', 'sprs\\m143p.spr',
                                 'Txrs\\m143r.txr', 'BGM\\0001.mp3')

    def test_radar_keeps_the_play_area_aspect_and_draws_the_borders(self):
        from owner.maps import txr_format
        with tempfile.TemporaryDirectory() as tmp:
            spec = mission_spec()
            spec['area'] = [-500, 1000, 500, -1000]
            root = write_package(Path(tmp), mission=spec)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['terrain']['play_area'] = {'half_x': 1024, 'half_z': 2048}
            manifest['missions'] = ['missions/m.json']
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            pkg = P.load(root)

            def radar(build, name):
                raw = build.resources[name]
                return Image.open(io.BytesIO(txr_format.parse(raw).image.to_bytes())).convert('RGB')
            shared = {}
            pvp = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp', prefix='m143', shared=shared)
            self.assertEqual(struct.unpack_from('<4i', pvp.rows[0][2], 0x33B), (64, 0, 128, 256))
            image = radar(pvp, 'txrs/m143r.txr')
            self.assertEqual(image.getpixel((10, 128)), (0, 0, 0))                 # letterbox outside the play area
            self.assertEqual(image.getpixel((64, 128)), compiler.RADAR_BORDER)     # play-area border
            self.assertNotIn('txrs/m143rx.txr', pvp.resources)
            mission = compiler.compile_map(pkg, map_no=144, map_id=500, variant='mission', prefix='m143', shared=shared)
            self.assertIn('Txrs\\m143rx.txr', mission.rows[0][2][0x2D7:0x33B].decode('utf-16-le'))
            image = radar(mission, 'txrs/m143rx.txr')
            # area x -500..500 of the play half 1024 -> columns 64 + (524/2048)*127 ~ 96 .. 159
            self.assertEqual(image.getpixel((96, 128)), compiler.RADAR_BORDER)
            self.assertNotEqual(image.getpixel((80, 128)), compiler.RADAR_BORDER)

    def test_movable_areas_set_every_players_terrain_rect(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = mission_spec()
            spec['area'] = [-2000, 1500, 2000, -1500]                    # applied when the opening starts
            spec['phases'][1]['enter'] = {'area': [-1000, 2000, 1000, 500]}
            pkg = P.load(write_package(Path(tmp), mission=spec))

            def compile_lua():
                build = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                          templates=templates())
                return build.lua.decode('utf-8') if isinstance(build.lua, bytes) else build.lua
            lua = compile_lua()
            self.assertRegex(lua, r"SDGOM1_C\d+ = \{id='SDGOM1_C\d+', frames=\d+, n=\d+, hide=1, area=\{-2000,1500,2000,-1500\}")
            self.assertIn('if c.area ~= nil then SDGOMI.area(m, c.area) end', lua)
            self.assertIn('function SDGOM1_E1(m)\n  SDGOMI.area(m, {-1000,2000,1000,500})', lua)
            self.assertIn('unit:SetTerrainRect(r[1], r[2], r[3], r[4])', lua)
            # the client's own player is reached through the document, as native single-player scripts do
            self.assertIn('local me = CastingUnit(p)', lua)
            self.assertIn('me:SetTerrainRect(r[1], r[2], r[3], r[4]); me:SetTerrainRectOnOff(true)', lua)
            self.assertIn('if w.rect ~= nil then SDGOMI.local_area(w.rect) end', lua)
            for bad in ([2000, 1500, -2000, -1500], [-2000, -1500, 2000, 1500], [-5000, 1500, 2000, -1500]):
                spec['area'] = bad
                with self.assertRaisesRegex(M.MissionError, 'area'):
                    compile_lua()
            spec['area'] = [-2000, 1500, 2000, -1500]
            spec.pop('opening')
            with self.assertRaisesRegex(M.MissionError, 'opening'):
                compile_lua()

    def test_jpeg_layer_and_model_textures_compile(self):
        def jpeg(colour):
            out = io.BytesIO()
            Image.new('RGB', (64, 64), colour).save(out, format='JPEG', quality=90)
            return out.getvalue()

        def triangle_glb(image):
            positions = struct.pack('<9f', -50, 0, 0, 50, 0, 0, 0, 100, 0)
            uvs = struct.pack('<6f', 0, 1, 1, 1, 0.5, 0)
            indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
            blob = positions + uvs + indices + image + b'\0' * (-len(image) % 4)
            views = [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 24},
                     {'buffer': 0, 'byteOffset': 60, 'byteLength': 6}, {'buffer': 0, 'byteOffset': 68, 'byteLength': len(image)}]
            doc = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
                   'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1}, 'indices': 2, 'material': 0}]}],
                   'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}],
                   'textures': [{'source': 0}], 'images': [{'bufferView': 3, 'mimeType': 'image/jpeg'}],
                   'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                                  'min': [-50, 0, 0], 'max': [50, 100, 0]},
                                 {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                                 {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
                   'bufferViews': views, 'buffers': [{'byteLength': len(blob)}]}
            body = json.dumps(doc).encode('utf-8')
            body += b' ' * (-len(body) % 4)
            return (struct.pack('<4sII', b'glTF', 2, 28 + len(body) + len(blob)) + struct.pack('<I4s', len(body), b'JSON')
                    + body + struct.pack('<I4s', len(blob), b'BIN\0') + blob)

        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp))
            files = {'terrain/ground.jpg': jpeg((90, 120, 60)), 'models/rock.glb': triangle_glb(jpeg((120, 110, 100)))}
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['terrain']['layers'] = [{'texture': 'terrain/ground.jpg', 'tiling': 32}]
            manifest['objects'] = [{'model': 'models/rock.glb', 'position': [2500, 0, 2500], 'rotation_y': 0}]
            for name, raw in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_bytes(raw)
                manifest['files'][name] = hashlib.sha256(raw).hexdigest()
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            build = compiler.compile_map(P.load(root), map_no=143, map_id=500, variant='pvp')
            self.assertEqual(build.report['models'], 1)
            self.assertGreaterEqual(build.report['textures'], 2)
            # height masks stay lossless: a JPEG mask is refused
            manifest['terrain']['layers'].append({'texture': 'terrain/ground.jpg', 'mask': 'terrain/ground.jpg', 'tiling': 8})
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaises(P.MapPackageError):
                P.load(root)

    def test_pvp_map_compiles_to_one_contiguous_row_and_server_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = P.load(write_package(Path(tmp)))
            build = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp')
            self.assertEqual([(k, i) for k, i, _ in build.rows], [(0x0F, 100001 + 143)])
            self.assertEqual(len(build.rows[0][2]), 0x34C)
            compiler.verify_trn(build.resources[build.report['trn'].replace('\\', '/').lower()])
            self.assertEqual(build.server['tbl_MapFile'][0]['MapNo'], 143)

    def test_mission_compile_links_spawn_mirrors_and_lays_out_objective_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = P.load(write_package(Path(tmp), mission=mission_spec()))
            built = M.compile_mission(pkg, json.loads(pkg.read('missions/m.json')), map_no=144,
                                      terrain_path='Trns\\m143x.trn', ids=ids(), templates=templates())
            rows = {(k, i): r for k, i, r in built.rows}
            game_mode = rows[(M.GAME_MODE_KIND, 88072)]
            spawns = sorted(i for k, i in rows if k == M.SPAWN_KIND)
            self.assertEqual(spawns, [r['MissionNpcID'] for r in built.server['tbl_MissionNpc']])
            enemy_first = struct.unpack_from('<I', game_mode, 1054)[0]
            ally_first = struct.unpack_from('<I', game_mode, 1058)[0]
            self.assertEqual(struct.unpack_from('<i', rows[(M.SPAWN_KIND, enemy_first)], 5)[0], 88072)
            self.assertEqual(rows[(M.SPAWN_KIND, enemy_first)][21], 2)
            self.assertEqual(rows[(M.SPAWN_KIND, ally_first)][21], 1)
            objective = rows[(M.OBJECTIVE_KIND, 197128)]
            self.assertEqual(objective[114:114 + 100].decode('utf-16-le').split('\0')[0], '击破全部靶机')
            for kind, _, raw in built.rows:
                self.assertEqual(len(raw), M.SIZES[kind])
            self.assertIn('function SDGOM1_C0_LuaInit', built.lua)
            self.assertIn('hide=1', built.lua)
            self.assertTrue(all(ord(c) < 128 for c in built.lua))

    def test_catalogue_artwork_replaces_the_template_images(self):
        spec = mission_spec()
        spec['presentation'] = {'room_poster': 'images/poster.png', 'loading': 'images/loading.png'}
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp), mission=spec)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            for name, size in (('images/poster.png', (250, 120)), ('images/loading.png', (1024, 1024))):
                raw = png(Image.new('RGBA', size, (200, 60, 200, 255)))
                (root / name).write_bytes(raw)
                manifest['files'][name] = hashlib.sha256(raw).hexdigest()
            (root / 'map.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
            pkg = P.load(root)
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                      templates=templates())
            catalogue = {(k, i): r for k, i, r in built.rows}[(M.CATALOGUE_KIND, 191040)]
            text = lambda offset: catalogue[offset:offset + 100].decode('utf-16-le').split('\0')[0]
            self.assertEqual(text(309), 'sprs\\sdgom1a2.spr')
            self.assertEqual(text(1209), 'Txrs\\sdgom1a4.txr')
            self.assertEqual(text(109), '')                       # not given: the template's value stays
            sprite = built.resources['sprs/sdgom1a2.spr']
            self.assertEqual(struct.unpack_from('<5I', sprite, 0)[3:], (120, 250))
            self.assertEqual(sprite[44:44 + 36].decode('utf-16-le').split('\0')[0], 'Txrs\\sdgom1a2.txr')
            self.assertIn('txrs/sdgom1a4.txr', built.resources)
            self.assertTrue(all(bundle.allowed_overlay(name) for name in built.resources))
            spec['presentation'] = {'room_poster': 'images/loading.png'}
            with self.assertRaises(M.MissionError):
                M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates())

    def test_missing_enemy_wave_is_refused(self):
        spec = mission_spec()
        for phase in spec['phases']:
            phase['spawns'] = [s for s in phase['spawns'] if s.get('side') == 'ally']
        spec['phases'] = spec['phases'][1:]
        with tempfile.TemporaryDirectory() as tmp:
            pkg = P.load(write_package(Path(tmp), mission=spec))
            with self.assertRaises(M.MissionError):
                M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates())


def record_templates():
    """templates() plus client unit/weapon records for NPC label and attack copies."""
    unit = bytearray(M.UNIT_RECORD_BYTES)
    struct.pack_into('<I', unit, 0, 1001)
    unit[5:5 + 12] = '原机体(X)'.encode('utf-16-le')[:12]
    for slot, weapon in zip(M.UNIT_WEAPON_SLOTS + (M.UNIT_SPECIAL_SLOT,), (7001, 7002, 0, 7002)):
        struct.pack_into('<I', unit, slot, weapon)
    weapons = {}
    server_weapons = {}
    for ident, damage in ((7001, 800), (7002, 5000)):
        raw = bytearray(M.WEAPON_RECORD_BYTES)
        struct.pack_into('<I', raw, 0, ident)
        struct.pack_into('<I', raw, M.WEAPON_DAMAGE, damage)
        weapons[ident] = bytes(raw)
        server = {name: 0 for name in M.SERVER_WEAPON_COLUMNS}
        server.update(WeaponNo=ident, WeaponName=f'weapon {ident}', DamageSize=damage)
        server_weapons[ident] = server
    server_unit = {name: 0 for name in M.SERVER_UNIT_NPC_COLUMNS}
    server_unit.update(UnitNumber=1001, CustomNumber=6, UnitModelName='Fixture',
                       WeaponNumber_1=7001, WeaponNumber_2=7002, WeaponNumber_3=None,
                       WeaponNumber_FatalAttack=7002)
    base = templates()
    return M.Templates(**{**base.__dict__, 'unit_record': lambda u: bytes(unit) if u == 1001 else None,
                          'weapon_record': weapons.get,
                          'server_unit_record': lambda u, template=None: dict(server_unit) if u == 1001 else None,
                          'server_weapon_record': lambda w: dict(server_weapons[w]) if w in server_weapons else None})


def compile_spec(spec, tpl=None):
    with tempfile.TemporaryDirectory() as tmp:
        pkg = P.load(write_package(Path(tmp), mission=spec))
        return M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                 templates=tpl or templates())


class MissionFeatureTests(unittest.TestCase):
    def test_derived_npcs_do_not_inherit_a_stock_missions_cutins(self):
        tpl = templates()
        original = bytearray(tpl.npc_templates[500])
        struct.pack_into('<II', original, 390, 70001, 70002)
        sprite = 'sprs\\1234.spr'.encode('utf-16-le')
        original[398:398 + len(sprite)] = sprite
        tpl.npc_templates[500] = bytes(original)
        spec = mission_spec()
        spec['npcs']['target']['hp'] = 9000
        built = compile_spec(spec, tpl)
        rows = [raw for kind, _, raw in built.rows if kind == M.NPC_TEMPLATE_KIND]
        self.assertEqual(len(rows), 2)
        for raw in rows:
            self.assertEqual(raw[390:598], bytes(208))
        self.assertEqual(tpl.npc_templates[500], bytes(original))

        spec['schema_version'] = 2
        spec['npcs']['target']['cutin'] = {'native_sprite': 'sprs\\5678.spr', 'line': 'l1'}
        tpl.native_exists = lambda name: name == 'sprs\\5678.spr'
        built = compile_spec(spec, tpl)
        target = next(raw for kind, _, raw in built.rows
                      if kind == M.NPC_TEMPLATE_KIND and struct.unpack_from('<I', raw, 229)[0] == 9000)
        self.assertEqual(struct.unpack_from('<II', target, 390), (ids().speech_first, 0))
        self.assertEqual(target[398:598].decode('utf-16-le').split('\0')[0], 'sprs\\5678.spr')

    def test_native_unit_cutin_is_bound_to_the_spawned_npc(self):
        spec = mission_spec()
        spec['schema_version'] = 2
        spec['npcs']['target']['cutin'] = {'native_sprite': 'sprs\\10763.spr', 'line': 'l1'}
        tpl = templates()
        tpl.native_exists = lambda name: name == 'sprs\\10763.spr'
        built = compile_spec(spec, tpl)
        row = next(raw for kind, _, raw in built.rows if kind == M.NPC_TEMPLATE_KIND)
        npc_id = struct.unpack_from('<I', row)[0]
        self.assertEqual(struct.unpack_from('<I', row, 390)[0], ids().speech_first)
        self.assertEqual(row[398:598].decode('utf-16-le').rstrip('\0'), 'sprs\\10763.spr')
        self.assertTrue(any(r['NpcID'] == npc_id for r in built.server['tbl_MissionNpc']))
        # The native sprite is referenced, never repackaged as a world-space text effect.
        self.assertFalse(any(n.endswith(('.zfx', '.spr', '.txr')) for n in built.resources))
        tpl.native_exists = lambda _: False
        with self.assertRaisesRegex(M.MissionError, 'native_sprite'):
            compile_spec(spec, tpl)
        for bad in ({'native_sprite': '../wrong.spr', 'line': 'l1'},
                    {'native_sprite': 'sprs\\10763.spr', 'line': 'missing'},
                    {'native_sprite': 'sprs\\10763.spr', 'line': 'l1', 'text': 'wrong'}):
            spec['npcs']['target']['cutin'] = bad
            with self.assertRaises(M.MissionError):
                compile_spec(spec, tpl)

    def test_authored_cutin_uses_the_native_strip_viewport(self):
        spec = mission_spec()
        spec['schema_version'] = 2
        spec['npcs']['target']['cutin'] = {'image': 'images/unit.png', 'line': 'l1'}
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp), mission=spec)
            image = png(Image.new('RGBA', (1024, 156), (35, 60, 90, 255)))
            (root / 'images').mkdir(exist_ok=True)
            (root / 'images/unit.png').write_bytes(image)
            manifest = json.loads((root / 'map.json').read_bytes())
            manifest['files']['images/unit.png'] = hashlib.sha256(image).hexdigest()
            (root / 'map.json').write_bytes(json.dumps(manifest).encode())
            built = M.compile_mission(P.load(root), spec, map_no=144, terrain_path='Trns\\m143x.trn',
                                      ids=ids(), templates=templates())
            sprites = [raw for name, raw in built.resources.items() if name.endswith('.spr')]
            self.assertEqual(len(sprites), 1)
            self.assertEqual(struct.unpack_from('<II', sprites[0], 12), (156, 1024))
            npc = next(raw for kind, _, raw in built.rows if kind == M.NPC_TEMPLATE_KIND)
            self.assertIn('sdgom1u', npc[398:598].decode('utf-16-le'))
            self.assertFalse(any(name.endswith('.zfx') for name in built.resources))

    def test_new_mission_features_cannot_be_silently_ignored_by_v1(self):
        spec = mission_spec()
        spec['npcs']['target']['cutin'] = {'native_sprite': 'sprs\\10763.spr', 'line': 'l1'}
        with self.assertRaisesRegex(M.MissionError, 'schema_version=2'):
            compile_spec(spec)
        spec['npcs']['target'].pop('cutin')
        spec['transition_delay'] = 'immediate'
        with self.assertRaisesRegex(M.MissionError, 'schema_version=2'):
            compile_spec(spec)

    def test_cutin_crop_uses_native_uvs_and_preserves_the_picture_aspect(self):
        from owner.maps import txr_format
        spec = mission_spec()
        spec['schema_version'] = 2
        spec['npcs']['target']['cutin'] = {'image': 'images/strip.png', 'line': 'l1',
                                         'crop': [0, 116, 1280, 312]}
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp), mission=spec)
            picture = Image.new('RGBA', (1280, 427), 'white')
            picture.paste((10, 40, 90, 255), (0, 116, 1280, 312))
            raw = png(picture)
            (root / 'images/strip.png').write_bytes(raw)
            manifest = json.loads((root / 'map.json').read_bytes())
            manifest['files']['images/strip.png'] = hashlib.sha256(raw).hexdigest()
            (root / 'map.json').write_bytes(json.dumps(manifest).encode())
            pkg = P.load(root)
            def build():
                return M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn',
                                         ids=ids(), templates=templates())
            result = build()
            sprite = next(r for n, r in result.resources.items() if n.endswith('.spr'))
            self.assertEqual(struct.unpack_from('<II', sprite, 12), (157, 1024))
            self.assertEqual(struct.unpack_from('<4f', sprite, 20), (0, 116/512, 1280/2048, 312/512))
            texture = next(r for n, r in result.resources.items() if n.endswith('.txr'))
            width, height, pixels = txr_format.decode_rgba(txr_format.parse(texture))
            image = Image.frombytes('RGBA', (width, height), pixels)
            self.assertEqual(image.getpixel((20, 10)), (255, 255, 255, 255))
            self.assertEqual(image.getpixel((20, 120)), (10, 40, 90, 255))
            for bad in ([0, -1, 1280, 312], [0, 116, 1281, 312], [0, 312, 1280, 116],
                        [False, 116, 1280, 312], [0, 0, 10, 427]):
                spec['npcs']['target']['cutin']['crop'] = bad
                with self.assertRaises(M.MissionError):
                    build()
    def test_drops_keep_native_drops_and_add_mission_blueprints(self):
        tpl = record_templates()
        # unit 1001 (the template NPC's unit) has two blueprints; unit 3003 is not in the mission
        tpl.blueprints = {1001: [(1520002, 'bp a'), (1540075, 'bp b')], 3003: [(1520099, 'other')]}
        spec = mission_spec()
        spec['drops'] = {'blueprint_per_mille': 3}
        built = compile_spec(spec, tpl)
        setting = built.server['tbl_BattleResultItemDropSetting']
        self.assertEqual(setting, [{'DropConditionID': 1_040_001, 'NormalMachineID': 101_001,
                                    'RareMachineID': 390_401, 'UniqueMachineID': 0}])
        dice = built.server['tbl_BattleResultItemDropDice']
        self.assertEqual([(d['CategoryID'], d['Prob'], d['SumProb']) for d in dice],
                         [(0, 994, 993), (1520002, 3, 996), (1540075, 3, 999)])
        self.assertEqual(sum(d['Prob'] for d in dice), 1000)
        self.assertEqual(built.server['tbl_MissionMode'][0]['DropConditionID'], 1_040_001)
        self.assertEqual(built.server['tbl_MissionRankBossData'][0]['DropConditionID'], 1_040_001)
        # explicit units, native drops only, no drops
        spec['drops'] = {'blueprints': [3003], 'normal': False}
        self.assertEqual([d['CategoryID'] for d in compile_spec(spec, tpl).server['tbl_BattleResultItemDropDice']],
                         [0, 1520099])
        spec['drops'] = {'blueprints': []}
        built = compile_spec(spec, tpl)
        self.assertNotIn('tbl_BattleResultItemDropDice', built.server)
        self.assertEqual(built.server['tbl_BattleResultItemDropSetting'][0]['RareMachineID'], 0)
        spec.pop('drops')
        built = compile_spec(spec, tpl)
        self.assertEqual(built.server['tbl_MissionMode'][0]['DropConditionID'], 0)
        self.assertNotIn('tbl_BattleResultItemDropSetting', built.server)
        for bad in ({'blueprint_per_mille': 0}, {'blueprint_per_mille': 300}, {'blueprints': 'all'}, {'chance': 1}):
            spec['drops'] = bad
            with self.assertRaises(M.MissionError):
                compile_spec(spec, tpl)

    def test_kill_any_goal_and_timed_waves_use_absolute_time(self):
        spec = mission_spec()
        spec['phases'][0]['goal'] = {'kill_any': 3}
        spec['phases'][0]['spawns'][1]['at'] = 6           # targets at 6 s and 8 s (every 2)
        built = compile_spec(spec)
        goals = [r for r in built.server['tbl_MissionClear'] if r['EventOrder'] == 1 and r['ClearType'] == 1]
        self.assertEqual([(r['UseWho'], r['UseHow'], r['UseObjectID'], r['UseObjectCount']) for r in goals],
                         [(2, 2, 0, 3)])
        first = sorted((r['NpcParty'], r['AfterTime'], r['AbsTime']) for r in built.server['tbl_MissionNpc']
                       if r['MissionPart'] == 1)
        # Native ScriptTime holds the opening and resets the phase clock; authored times start afterwards.
        self.assertEqual(first, [(1, 1, 0), (2, 0, 6), (2, 0, 8)])
        spec.pop('opening')
        first = sorted((r['NpcParty'], r['AfterTime'], r['AbsTime']) for r in compile_spec(spec).server['tbl_MissionNpc']
                       if r['MissionPart'] == 1)
        self.assertEqual(first, [(1, 1, 0), (2, 0, 6), (2, 0, 8)])     # without a cutscene: AfterTime for t < 2
        spec['opening'] = 'intro'
        spec['phases'][0]['goal'] = {'kill_any': 0}
        with self.assertRaisesRegex(M.MissionError, 'kill_any'):
            compile_spec(spec)

    def test_npc_label_and_attack_use_paired_client_server_copies(self):
        spec = mission_spec()
        spec['npcs']['boss'] = {'template': 500, 'hp': 20000, 'boss': True, 'label': '加亚',
                                'attack': 2.0, 'special_attack': 1.5}
        spec['phases'][0]['spawns'].append({'npc': 'boss', 'positions': [[0, 150, 900]], 'at': 1})
        built = compile_spec(spec, record_templates())
        rows = {(k, i): r for k, i, r in built.rows}
        units = [i for k, i in rows if k == M.UNIT_KIND]
        self.assertEqual(units, [M.UNIT_LABEL_FIRST + 40 * 100])
        copy = rows[(M.UNIT_KIND, units[0])]
        self.assertEqual(copy[5:105].decode('utf-16-le').split('\0')[0], '加亚')
        slots = [struct.unpack_from('<I', copy, s)[0] for s in M.UNIT_WEAPON_SLOTS + (M.UNIT_SPECIAL_SLOT,)]
        damage = {i: struct.unpack_from('<I', r, M.WEAPON_DAMAGE)[0] for (k, i), r in rows.items()
                  if k == M.WEAPON_KIND}
        # weapon 7002 is both a normal weapon (x2) and the special (x1.5): two copies; empty slot stays empty
        self.assertEqual([damage.get(w, w) for w in slots], [1600, 10000, 0, 7500])
        self.assertEqual(len(damage), 3)
        template = [r for (k, i), r in rows.items() if k == M.NPC_TEMPLATE_KIND][-1]
        self.assertEqual(struct.unpack_from('<I', template, 105)[0], units[0])
        server = [r for r in built.server['tbl_NpcData'] if r['UseUnitHP'] == 20000][0]
        self.assertEqual((server['UseUnitID'], server['BossFlag']), (units[0], 1))
        server_unit = built.server['tbl_UnitNpc'][0]
        self.assertEqual(server_unit['UnitNumber'], units[0])
        self.assertEqual([server_unit[name] or 0 for name in M.SERVER_UNIT_WEAPON_FIELDS], slots)
        self.assertEqual({row['WeaponNo']: row['DamageSize'] for row in built.server['tbl_WeaponsRev']}, damage)
        for kind, _, raw in built.rows:
            self.assertEqual(len(raw), M.SIZES[kind])
        for bad in ({'attack': 5}, {'attack': True}, {'special_attack': 0.1}, {'label': 'A(B)'}):
            spec['npcs']['boss'] = {'template': 500, **bad}
            with self.assertRaises(M.MissionError):
                compile_spec(spec, record_templates())

    def test_cutscene_camera_is_a_sampled_path(self):
        built = compile_spec(mission_spec())
        # 0.1 s samples of the camera keys (frame, position, look), interpolated by the runtime every frame
        self.assertIn('path={n=31,{1,0,300,-600,0,50,200},{6,3.66,299.09,-596.34,0,50,200}', built.lua)
        self.assertIn('{180,200,250,-400,0,50,200}}', built.lua)
        self.assertNotIn('cam=vector3', built.lua)          # no chained MoveCameraPos jumps
        self.assertIn('SDGOMI.camera(m, c, s)', built.lua)

    def test_held_actor_stays_in_place_every_frame(self):
        spec = mission_spec()
        spec['cutscenes']['intro']['actors']['a']['hold'] = True
        lua = compile_spec(spec).lua
        self.assertIn('a={tpl=500,at=vector3(0,50,200),dir=vector3(0,0,1),hold=1,enemy=0}', lua)
        self.assertIn('if a.enemy == 1 then team = OBJECT_TEAM_BLUE end', lua)   # players' side by default
        self.assertIn('if a.hold == 1 and s.actors[name] ~= nil then s.actors[name]:SetNowPos(a.at) end', lua)
        spec['cutscenes']['intro']['events'].append({'t': 1.5, 'actor': 'a', 'move_to': [0, 150, 400]})
        with self.assertRaisesRegex(M.MissionError, 'hold'):
            compile_spec(spec)

    def test_move_target_below_the_lift_is_refused(self):
        spec = mission_spec()
        spec['cutscenes']['intro']['events'].append({'t': 1.5, 'actor': 'a', 'move_to': [0, 20, 400]})
        with self.assertRaisesRegex(M.MissionError, '移动目标'):
            compile_spec(spec)
        spec['cutscenes']['intro']['events'][-1]['move_to'] = [0, 150, 400]
        self.assertIn('move=vector3(0,150,400)', compile_spec(spec).lua)

    def test_spawns_outside_the_phase_area_are_refused(self):
        spec = mission_spec()
        spec['area'] = [-2000, 1500, 2000, -1500]
        spec['phases'][1]['enter'] = {'area': [-1000, 2000, 1000, 500]}
        compile_spec(spec)                                    # phase 2 target at z 600 is inside its area
        spec['phases'][1]['spawns'][0]['positions'] = [[0, 150, 300]]
        with self.assertRaisesRegex(M.MissionError, r'phases\[1\]\.spawns\[0\]\.positions\[0\] 不在本阶段的活动范围内'):
            compile_spec(spec)

    def test_mission_terrain_has_a_respawn_pair_per_phase(self):
        # the server respawns a pilot of phase p at shared start point 2p-2 or 2p-1; the shipped terrains have
        # 12 shared points (six phases), so a longer mission needs its own pairs or a late respawn finds none
        with tempfile.TemporaryDirectory() as tmp:
            spec = mission_spec()
            first = spec['phases'][0]
            phases = [dict(first, key='p%d' % i, next=None) for i in range(1, 9)]
            for phase in phases:
                phase.pop('next')
            phases[5]['enter'] = {'sector': 'north'}
            phases[6]['enter'] = {'area': [-1000, 2000, -600, 1200]}
            spec['phases'] = phases
            spec['sectors'] = {'north': {'warp': [[-300, 120, 1800], [300, 120, 1800]], 'facing': [0, 1],
                                         'rect': [-1500, 2000, 1500, 1000]}}
            root = write_package(Path(tmp), mission=spec)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['missions'] = ['missions/m.json']
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            pkg = P.load(root)
            shared = {}
            pvp = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp', prefix='m143', shared=shared)
            mission = compiler.compile_map(pkg, map_no=144, map_id=500, variant='mission', prefix='m143', shared=shared)
            pvp_points = compiler.verify_trn(pvp.resources['trns/m143.trn']).start_points
            points = compiler.verify_trn(mission.resources['trns/m143x.trn']).start_points
            self.assertEqual([p.index for p in pvp_points if p.colour == 0], list(range(len(pkg.spawns['shared']))))
            shared_points = {p.index: p.position for p in points if p.colour == 0}
            self.assertEqual(sorted(shared_points), list(range(max(16, len(pkg.spawns['shared'])))))
            base = [s.position for s in pkg.spawns['shared'][:2]]
            base = base if len(base) == 2 else base * 2  # a single shared spawn serves both points of a pair
            for phase in range(5):                       # phases 1..5 respawn at the opening pair
                self.assertEqual([shared_points[2 * phase], shared_points[2 * phase + 1]], base)
            self.assertEqual([(x, z) for x, _, z in (shared_points[10], shared_points[11])], [(-300, 1800), (300, 1800)])
            for index in (12, 13):                       # phase 7 moves the pair into its new area
                x, _, z = shared_points[index]
                self.assertTrue(-1000 + 150 <= x <= -600 - 150 and 1200 + 150 <= z <= 2000 - 150, (x, z))
            self.assertEqual(shared_points[14], shared_points[12])

    def test_boundary_lines_are_painted_on_the_ground(self):
        from owner.maps import trn_format
        # a shared west border of two areas is one line; opposite-facing edges on the same z stay apart
        lines = compiler.boundary_lines([(-2000, 1500, 2000, -1500), (-2000, 3000, 2000, 1500)])
        self.assertIn(('z', -2000, -1500, 3000, 0.0), lines)
        self.assertIn(('x', 1500, -2000, 2000, 270.0), lines)
        self.assertIn(('x', 1500, -2000, 2000, 90.0), lines)
        self.assertEqual(len(lines), 6)
        with tempfile.TemporaryDirectory() as tmp:
            spec = mission_spec()
            spec['area'] = [-2000, 1500, 2000, -1500]
            root = write_package(Path(tmp), mission=spec)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['terrain']['play_area'] = {'half_x': 2048, 'half_z': 2048}
            manifest['missions'] = ['missions/m.json']
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            pkg = P.load(root)
            shared = {}
            pvp = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp', prefix='m143', shared=shared)
            decals = compiler.verify_trn(pvp.resources['trns/m143.trn']).decals
            # the play area edge, 4 x 4096 in segments of 1024 like the shipped maps
            self.assertEqual(len(decals), 16)
            west = [d for d in decals if d.tex_rotation == 0.0]
            self.assertEqual(sorted(d.transform.position for d in west),
                             [(-2048.0, 0.0, z) for z in (-1536.0, -512.0, 512.0, 1536.0)])
            d = west[0]
            self.assertEqual(trn_format.wtext(d.texture), compiler.BOUNDARY_BAND)
            self.assertEqual(compiler.BOUNDARY_BAND, 'Txrs' + chr(92) + '0806.txr')
            self.assertEqual((d.u_wrap, d.v_wrap, d.receive_shadow), (0, 1, 0))
            self.assertAlmostEqual(d.transform.scale[2] * compiler.DECAL_UNIT, 1024.0, 3)
            self.assertAlmostEqual(d.tex_scale[1], 16.0, 5)
            mission = compiler.compile_map(pkg, map_no=144, map_id=500, variant='mission', prefix='m143', shared=shared)
            decals = compiler.verify_trn(mission.resources['trns/m143x.trn']).decals
            # the mission area 4000 x 3000: 4 + 4 segments along x, 3 + 3 along z
            self.assertEqual(len(decals), 14)
            self.assertEqual(max(abs(d.transform.position[0]) for d in decals), 2000.0)
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp))
            data = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            data['presentation']['boundary_lines'] = False
            (root / 'map.json').write_text(json.dumps(data), encoding='utf-8')
            build = compiler.compile_map(P.load(root), map_no=143, map_id=500, variant='pvp', prefix='m143')
            self.assertEqual(compiler.verify_trn(build.resources['trns/m143.trn']).decals, [])
            data['presentation']['boundary_lines'] = 'yes'
            (root / 'map.json').write_text(json.dumps(data), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, 'boundary_lines'):
                P.load(root)

    def test_loading_screen_poster_or_random(self):
        spec = mission_spec()
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp), mission=spec)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            raw = png(Image.new('RGBA', (1024, 1024), (20, 60, 200, 255)))
            (root / 'images/loading.png').write_bytes(raw)
            manifest['files']['images/loading.png'] = hashlib.sha256(raw).hexdigest()
            (root / 'map.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
            pkg = P.load(root)

            def poster(presentation):
                spec['presentation'] = presentation
                built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                          templates=templates())
                row = {(k, i): r for k, i, r in built.rows}[(M.CATALOGUE_KIND, 191040)]
                return row[1209:1309].decode('utf-16-le').split('\0')[0], built.resources
            path, resources = poster({'loading': 'images/loading.png'})
            self.assertEqual(path, 'Txrs\\sdgom1a4.txr')
            self.assertIn('txrs/sdgom1a4.txr', resources)
            path, resources = poster({'loading': 'images/loading.png', 'loading_screen': 'random'})
            self.assertEqual(path, '')
            self.assertNotIn('txrs/sdgom1a4.txr', resources)
            for bad in ({'loading_screen': 'poster'}, {'loading_screen': 'banner'}):
                with self.assertRaisesRegex(M.MissionError, 'loading_screen'):
                    poster(bad)


class CheckTests(unittest.TestCase):
    def test_camera_and_placement_checks(self):
        box = ((0.0, 100.0, 0.0), ((1, 0, 0), (0, 1, 0), (0, 0, 1)), (100.0, 100.0, 100.0))
        flat = lambda p: 0.0  # noqa: E731
        self.assertIsNone(occlusion.shot_problem(flat, [box], (500, 150, -500), (500, 50, 500)))
        self.assertEqual(occlusion.shot_problem(flat, [box], (0, 150, -500), (0, 100, 500)), '视线被物件遮挡')
        self.assertEqual(occlusion.shot_problem(flat, [box], (0, 100, 0), (0, 100, 500)), '镜头位于物件内部')
        self.assertEqual(occlusion.shot_problem(flat, [], (0, 10, 0), (0, 100, 500)), '镜头低于地面')
        self.assertTrue(occlusion.blocked([box], (50, 150, 0)))
        self.assertFalse(occlusion.blocked([box], (300, 150, 0)))

    def test_cutscene_through_an_object_is_refused(self):
        wall = {'model': 'models/wall.glb', 'position': [0, 0, 0], 'rotation_y': 0,
                'collision': [{'center': [0, 200, -200], 'half_extents': [400, 200, 30]}]}
        with tempfile.TemporaryDirectory() as tmp:
            root = write_package(Path(tmp), mission=mission_spec())
            pkg = P.load(root)
            obj = P.MapObject((0.0, 0.0, 0.0), 0.0, (1.0, 1.0, 1.0), collision=wall['collision'])
            obj.model = 'models/wall.glb'
            pkg.objects.append(obj)
            boxes = occlusion.object_boxes(pkg, 'mission', None)
            with self.assertRaises(M.MissionError) as raised:
                M.compile_mission(pkg, json.loads(pkg.read('missions/m.json')), map_no=144,
                                  terrain_path='Trns\\m143x.trn', ids=ids(), templates=templates(), occluders=boxes)
            self.assertIn('cutscenes.intro', str(raised.exception))


class InstallTests(unittest.TestCase):
    def test_install_writes_content_and_removes_stale_mission_script(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            pkg = P.load(write_package(tmp / 'pkg', mission=mission_spec()))
            build = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp')
            mission_build = M.compile_mission(pkg, json.loads(pkg.read('missions/m.json')), map_no=144,
                                              terrain_path='Trns\\m143x.trn', ids=ids(), templates=templates())
            client = tmp / 'client'
            (client / 'sdgo/mods/map_catalog').mkdir(parents=True)
            (client / 'GOnline.exe').write_bytes(b'MZ')
            (client / 'sdgo/mods/map_catalog/mod.ini').write_text('[mod]\nid=map_catalog\n', encoding='utf-8')
            bundle.build_publication([build], tmp / 'pub1', missions=[mission_build])
            first = deployment.install_package(client, (tmp / 'pub1/SDGO-MapImport-Client.zip').read_bytes())
            self.assertTrue((client / 'sdgo/mods/map_catalog/missions.lua').is_file())
            self.assertEqual(first['removed'], [])
            bundle.build_publication([build], tmp / 'pub2')
            second = deployment.install_package(client, (tmp / 'pub2/SDGO-MapImport-Client.zip').read_bytes())
            self.assertFalse((client / 'sdgo/mods/map_catalog/missions.lua').exists())
            self.assertEqual(second['removed'], ['sdgo/mods/map_catalog/missions.lua'])
            self.assertTrue((client / 'sdgo/mods/map_catalog/mod.ini').is_file())
            with zipfile.ZipFile(tmp / 'pub2/SDGO-MapImport-Client.zip') as archive:
                self.assertNotIn('client/sdgo/mods/map_catalog/missions.lua', archive.namelist())


class ExampleMapTests(unittest.TestCase):
    def test_pinned_example_matches_the_committed_package(self):
        from manager import maps as manager_maps
        for key, spec in manager_maps.EXAMPLES.items():
            folder = ROOT / spec['path']
            raw = (folder / 'map.json').read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), spec['map_json'], key)
            self.assertRegex(spec['commit'], r'^[0-9a-f]{40}$')
            pkg = P.load(folder)
            for name in json.loads(raw)['files']:
                pkg.read(name)               # every listed file is present with its digest

    def test_example_download_is_verified_and_cached(self):
        from manager import maps as manager_maps
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = write_package(tmp / 'source')
            served = {}

            def fetch(url, limit):
                name = url.split('/example/', 1)[1]
                served[name] = served.get(name, 0) + 1
                return (source / name).read_bytes()

            spec = {'name': {'zh': '测试'}, 'description': '', 'path': 'example', 'commit': '0' * 40,
                    'map_json': hashlib.sha256((source / 'map.json').read_bytes()).hexdigest()}
            original = dict(manager_maps.EXAMPLES)
            manager_maps.EXAMPLES.clear()
            manager_maps.EXAMPLES['test'] = spec
            try:
                folder = manager_maps.fetch_example('test', tmp / 'cache', fetch=fetch)
                self.assertEqual(P.load(folder).map_key, 'test.flat-plain')
                manager_maps.fetch_example('test', tmp / 'cache', fetch=fetch)
                self.assertEqual(served['terrain/ground.png'], 1)        # unchanged files are not fetched again
                (source / 'terrain/ground.png').write_bytes(b'tampered')
                (folder / 'terrain/ground.png').unlink()
                with self.assertRaisesRegex(ValueError, '校验失败'):
                    manager_maps.fetch_example('test', tmp / 'cache', fetch=fetch)
                manifest = json.loads((source / 'map.json').read_text(encoding='utf-8'))
                manifest['files']['../escape.png'] = '0' * 64
                (source / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
                spec['map_json'] = hashlib.sha256((source / 'map.json').read_bytes()).hexdigest()
                with self.assertRaisesRegex(ValueError, '无效文件路径'):
                    manager_maps.fetch_example('test', tmp / 'cache', fetch=fetch)
                with self.assertRaisesRegex(ValueError, '没有这个示例地图'):
                    manager_maps.fetch_example('missing', tmp / 'cache', fetch=fetch)
            finally:
                manager_maps.EXAMPLES.clear()
                manager_maps.EXAMPLES.update(original)


class WorkspaceTests(unittest.TestCase):
    def test_register_edit_reorder_and_remove(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write_package(tmp / 'a', key='test.alpha')
            write_package(tmp / 'b', key='test.beta', mission=mission_spec())
            ws = MapWorkspace(tmp / 'ws')
            ws.add(tmp / 'a')
            entry = ws.add(tmp / 'b')
            self.assertEqual(entry['available_missions'], ['missions/m.json'])
            edited = ws.update('test.alpha', {'name_zh': '新名字', 'modes': ['normal', 'deathmatch'],
                                              'servers': ['expert']})
            self.assertEqual(edited['name']['zh'], '新名字')
            self.assertEqual(edited['modes'], ['normal', 'deathmatch'])
            ws.update('test.beta', {'position': 0})
            self.assertEqual([m['key'] for m in ws.load()], ['test.beta', 'test.alpha'])
            with self.assertRaises(WorkspaceError):
                ws.update('test.alpha', {'modes': ['bogus']})
            # re-registering keeps the owner's edits
            ws.add(tmp / 'a')
            self.assertEqual(ws.describe_entry(ws._entry(ws.load(), 'test.alpha'))['name']['zh'], '新名字')
            ws.remove('test.beta')
            self.assertEqual([m['key'] for m in ws.load()], ['test.alpha'])
            self.assertFalse((tmp / 'ws/packages/test.beta').exists())


if __name__ == '__main__':
    unittest.main()
