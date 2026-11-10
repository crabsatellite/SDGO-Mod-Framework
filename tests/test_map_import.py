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
