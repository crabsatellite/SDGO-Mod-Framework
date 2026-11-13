"""Focused protocol tests for schema-v2 named blocks and schema-v3 mission controls."""
import copy
import base64
import hashlib
import json
import re
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from owner.maps import block as B, bundle, compiler, mission as M, mod_format, occlusion, package as P, server, trn_format
from tests.test_map_import import ids, mission_spec, record_templates, templates, write_package


def triangle_glb() -> bytes:
    positions = struct.pack('<9f', -50, 0, 0, 50, 0, 0, 0, 100, 0)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    blob = positions + indices
    doc = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1}]}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
             'min': [-50, 0, 0], 'max': [50, 100, 0]},
            {'bufferView': 1, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'},
        ],
        'bufferViews': [
            {'buffer': 0, 'byteOffset': 0, 'byteLength': len(positions)},
            {'buffer': 0, 'byteOffset': len(positions), 'byteLength': 6},
        ],
        'buffers': [{'byteLength': len(blob)}],
    }
    body = json.dumps(doc, separators=(',', ':')).encode('utf-8')
    body += b' ' * (-len(body) % 4)
    return (struct.pack('<4sII', b'glTF', 2, 28 + len(body) + len(blob))
            + struct.pack('<I4s', len(body), b'JSON') + body
            + struct.pack('<I4s', len(blob), b'BIN\0') + blob)


def block_template(identity=80577) -> B.Template:
    native = bytearray(B.RECORD_BYTES)
    struct.pack_into('<I', native, 0, identity)
    struct.pack_into('<i', native, 165, 5000)
    server_row = {
        'ObjectID': identity, 'ObjectName': 'template', 'HP': 5000, 'CollisionDestroy': 0,
        'ExplosionRadius': 0.0, 'ExplosionRadiusPower': 0.0, 'ExplosionTime': 0,
        'TeamKill': 0, 'Damage': 0, 'FragmentType': 0,
    }
    return B.Template(bytes(native), server_row)


def gate_package(root: Path, mission=None) -> P.Package:
    write_package(root, mission=mission)
    raw = triangle_glb()
    model = root / 'models/gate.glb'
    model.parent.mkdir(parents=True, exist_ok=True)
    model.write_bytes(raw)
    manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
    manifest['schema_version'] = 2
    manifest['objects'] = [{
        'id': 'odessa_gate', 'model': 'models/gate.glb', 'position': [2500, 100, 2500],
        'rotation_y': 0, 'scale': 1,
        'collision': [{'center': [0, 100, 0], 'half_extents': [50, 100, 20]}],
        'block': {'template': 80577, 'name': {'zh': '基地闸门', 'en': 'Fortress gate'}, 'hp': 1000000},
    }]
    manifest['files']['models/gate.glb'] = hashlib.sha256(raw).hexdigest()
    if mission is not None:
        mission_raw = json.dumps(mission, ensure_ascii=False).encode('utf-8')
        (root / 'missions/m.json').write_bytes(mission_raw)
        manifest['files']['missions/m.json'] = hashlib.sha256(mission_raw).hexdigest()
    (root / 'map.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
    return P.load(root)


class NamedBlockTests(unittest.TestCase):
    def test_initial_ground_boundary_markings_keep_future_radar_and_area_definitions(self):
        spec = mission_spec()
        spec['schema_version'] = 4
        spec['area'] = [-1000, 1000, 1000, -1000]
        spec['phases'][1]['enter']['area'] = [-1500, 1500, 1500, -1500]
        spec['boundary_markings'] = 'initial'
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            pkg.manifest['missions'] = ['missions/m.json']
            self.assertEqual(compiler.mission_areas(pkg), [(-1000, 1000, 1000, -1000), (-1500, 1500, 1500, -1500)])
            self.assertEqual(compiler.mission_areas(pkg, markings=True), [(-1000, 1000, 1000, -1000)])

    def test_transparent_gltf_preserves_alpha_with_a_shared_texture(self):
        from owner.maps import txr_format
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp))
            raw = triangle_glb()
            size = struct.unpack_from('<I', raw, 12)[0]
            doc = json.loads(raw[20:20 + size])
            blob = raw[28 + size:]
            doc['buffers'][0]['uri'] = 'data:application/octet-stream;base64,' + base64.b64encode(blob).decode()
            doc['images'] = [{'uri': 'data:image/png;base64,' + base64.b64encode(pkg.read('terrain/ground.png')).decode()}]
            doc['textures'] = [{'source': 0}]
            doc['materials'] = [
                {'alphaMode': 'OPAQUE', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}},
                {'alphaMode': 'BLEND', 'doubleSided': True, 'pbrMetallicRoughness': {
                    'baseColorTexture': {'index': 0}, 'baseColorFactor': [1, 1, 1, 0.25]}}]
            primitive = doc['meshes'][0]['primitives'][0]
            primitive['material'] = 0
            doc['meshes'][0]['primitives'].append({**primitive, 'material': 1})
            path = Path(tmp) / 'models/transparent.gltf'
            path.write_text(json.dumps(doc), encoding='utf-8')
            manifest_path = Path(tmp) / 'map.json'
            manifest = json.loads(manifest_path.read_bytes())
            manifest['files']['models/transparent.gltf'] = hashlib.sha256(path.read_bytes()).hexdigest()
            manifest['objects'][0]['model'] = 'models/transparent.gltf'
            manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
            pkg = P.load(tmp)
            built = compiler.compile_map(pkg, map_no=143, map_id=500, variant='mission',
                                         block_templates={80577: block_template()}, block_object_first=80600)
            model = mod_format.parse(next(raw for name, raw in built.resources.items() if name.endswith('.mod')))
            nodes = [n for n in model.nodes if n.mesh]
            self.assertEqual([n.flags for n in nodes], [0, mod_format.FLAG_TWO_SIDED | mod_format.FLAG_SCREEN_ALPHA])
            slots = [n.mesh.groups[0].material for n in nodes]
            self.assertNotEqual(*slots)
            self.assertEqual([model.kudiz_materials[i].diffuse[3] for i in slots], [1.0, 0.25])
            self.assertEqual(model.kudiz_materials[slots[0]].textures[0], model.kudiz_materials[slots[1]].textures[0])

    def test_schema_v2_is_strict_and_v1_refuses_block_semantics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_package(root)
            manifest = json.loads((root / 'map.json').read_text(encoding='utf-8'))
            manifest['objects'] = [{'id': 'gate', 'model': 'models/missing.glb', 'position': [0, 0, 0],
                                    'block': {'template': 80577}}]
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, '未知字段|schema_version=2'):
                P.load(root)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pkg = gate_package(root)
            manifest = dict(pkg.manifest)
            manifest['objects'] = [dict(manifest['objects'][0], mystery=True)]
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, '未知字段'):
                P.load(root)
            manifest['objects'][0].pop('mystery')
            manifest['objects'][0]['initially_hidden'] = 'yes'
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, '布尔值'):
                P.load(root)
            manifest['objects'][0]['initially_hidden'] = False
            manifest['objects'][0]['block'].pop('name')
            (root / 'map.json').write_text(json.dumps(manifest), encoding='utf-8')
            with self.assertRaisesRegex(P.MapPackageError, 'template、name'):
                P.load(root)

    def test_named_block_compiles_native_trn_server_and_publication_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp) / 'package')
            shared = {}
            kwargs = {'block_templates': {80577: block_template()}, 'block_object_first': 80600}
            pvp = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp', prefix='m143',
                                       shared=shared, **kwargs)
            mission = compiler.compile_map(pkg, map_no=144, map_id=500, variant='mission', prefix='m143',
                                           shared=shared, **kwargs)
            self.assertEqual([(kind, ident, len(raw)) for kind, ident, raw in pvp.rows],
                             [(B.CATALOG_KIND, 80600, B.RECORD_BYTES), (compiler.MAP_KIND, 100144, 844)])
            self.assertEqual([(kind, ident) for kind, ident, _raw in mission.rows],
                             [(compiler.MAP_KIND, 100145)])
            native = pvp.rows[0][2]
            self.assertEqual(struct.unpack_from('<I', native, 0)[0], 80600)
            self.assertEqual(native[5:105].decode('utf-16-le').split('\0')[0], '基地闸门')
            self.assertEqual(pvp.server['tbl_MapObjects'][0]['ObjectName'], 'odessa_gate')
            self.assertEqual(struct.unpack_from('<i', native, 165)[0], 1000000)
            self.assertEqual(native[105:165].decode('utf-16-le').split('\0')[0], 'mdrs\\m143o00.mod')

            trn = compiler.verify_trn(pvp.resources['trns/m143.trn'])
            self.assertEqual(len(trn.destroyable_blocks), 1)
            placed = trn.destroyable_blocks[0]
            self.assertEqual(placed.object_id, 80600)
            self.assertEqual(trn_format.wtext(placed.name), 'odessa_gate')
            self.assertEqual(pvp.server['tbl_MapData'][0]['BlockCnt'], 1)
            self.assertEqual(pvp.server['tbl_MapBlocks'][0]['BlockID'], 7001)
            self.assertEqual(pvp.server['tbl_MapBlocks'][0]['ObjectID'], 80600)
            self.assertEqual(pvp.server['tbl_MapObjects'][0]['HP'], 1000000)
            self.assertEqual(pvp.report['blocks'][0]['scene_kind'], 0x12)
            self.assertEqual(pvp.report['blocks'][0]['display_name'],
                             {'zh': '基地闸门', 'en': 'Fortress gate'})
            self.assertEqual(pvp.report['blocks'][0]['aabb'],
                             {'min': [2450.0, 100.0, 2480.0], 'max': [2550.0, 300.0, 2520.0]})

            output = Path(tmp) / 'out'
            bundle.build_publication([pvp, mission], output)
            plan = server.load_plan(output / 'server-plan.json')
            self.assertEqual(plan['schema'], bundle.PLAN_SCHEMA)
            self.assertEqual(len(plan['tables']['tbl_MapObjects']), 1)
            self.assertEqual(len(plan['tables']['tbl_MapBlocks']), 1)
            invalid = json.loads((output / 'server-plan.json').read_text(encoding='utf-8'))
            invalid['tables']['tbl_MapBlocks'][0]['MaxX'] = invalid['tables']['tbl_MapBlocks'][0]['MinX']
            invalid_path = output / 'invalid-aabb.json'
            invalid_path.write_text(json.dumps(invalid), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '最小范围'):
                server.load_plan(invalid_path)
            with zipfile.ZipFile(output / 'SDGO-MapImport-Client.zip') as archive:
                definitions = json.loads(archive.read('client/sdgo/mods/map_catalog/definitions.json'))
            self.assertEqual(definitions['schema_version'], 2)
            self.assertEqual(definitions['maps'][0]['blocks'][0]['definition_id'], 80600)

    def test_v2_server_plan_keeps_runtime_policy_and_paired_catalog_support(self):
        spec = mission_spec()
        spec['schema_version'] = 2
        spec['npcs']['ally']['attack'] = 2.0
        with tempfile.TemporaryDirectory() as tmp:
            pkg = P.load(write_package(Path(tmp), mission=spec))
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                      templates=record_templates())
            map_build = compiler.compile_map(pkg, map_no=143, map_id=500, variant='pvp')
            tables = copy.deepcopy(map_build.server)
            for table, rows in built.server.items():
                tables.setdefault(table, []).extend(copy.deepcopy(rows))
            plan = {'schema': 'sdgo.map-import-server-plan/2', 'tables': tables,
                    'runtime_policy': {'immediate_transition_missions': [ids().game_mode_first]}}
            path = Path(tmp) / 'plan-v2.json'
            path.write_text(json.dumps(plan), encoding='utf-8')
            loaded = server.load_plan(path)
            self.assertTrue(loaded['tables']['tbl_UnitNpc'])
            self.assertTrue(loaded['tables']['tbl_WeaponsRev'])
            self.assertEqual(loaded['tables']['tbl_MapData'][0]['BlockCnt'], 0)
            self.assertEqual(loaded['runtime_policy'],
                             {'immediate_transition_missions': [ids().game_mode_first]})

    def test_section_clear_grouping_is_opt_in_and_preserves_native_progress_rows(self):
        spec = mission_spec()
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            def compile_mission(value):
                return M.compile_mission(pkg, value, map_no=144, terrain_path='Trns\\m143x.trn',
                                         ids=ids(), templates=templates())
            old = compile_mission(spec)
            self.assertEqual(old.record_flags, {})
            spec['schema_version'] = 3
            spec['phases'][0]['section_clear'] = False
            with self.assertRaisesRegex(M.MissionError, 'schema_version=4'):
                compile_mission(spec)
            spec['schema_version'] = 4
            grouped = compile_mission(spec)
            self.assertEqual(grouped.server, old.server)
            self.assertEqual(grouped.rows, old.rows)
            self.assertEqual(grouped.record_flags, {ids().game_mode_first: 0x201})
            self.assertEqual(grouped.report['section_clear_phases'], [2])
            raw = next(raw for kind, ident, raw in grouped.rows if kind == M.GAME_MODE_KIND)
            container = bundle.native_catalog([(M.GAME_MODE_KIND, ids().game_mode_first, raw)], grouped.record_flags)
            self.assertEqual(struct.unpack_from('<I', container, 48)[0], 0x201)
            self.assertEqual(container[56:], raw)
            with self.assertRaisesRegex(ValueError, 'flags'):
                bundle.native_catalog([(M.GAME_MODE_KIND, ids().game_mode_first, raw)], {ids().game_mode_first: 0x10001})
            spec['phases'][0]['section_clear'] = 0
            with self.assertRaisesRegex(M.MissionError, '布尔'):
                compile_mission(spec)

    def test_hidden_native_block_persists_into_later_cinematics_and_ending(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        spec['phases'][1]['enter']['blocks'] = [{'block': 'odessa_gate', 'visible': False}]
        third = copy.deepcopy(spec['phases'][1])
        third['key'] = 'three'
        third['enter'] = {'cutscene': 'later'}
        spec['phases'].append(third)
        scene = {'length': 1, 'actors': {},
                 'camera': [{'t': 0, 'pos': [2500, 200, 2500], 'look': [2600, 200, 2500]}]}
        spec['cutscenes']['later'] = copy.deepcopy(scene)
        spec['cutscenes']['outro'] = copy.deepcopy(scene)
        spec['ending'] = 'outro'
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            groups = occlusion.object_occluders(pkg, 'mission', None)
            M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                              templates=templates(), occluders=groups)
            skipped_hide = copy.deepcopy(spec)
            skipped_hide['phases'][0]['next'] = 'three'
            with self.assertRaisesRegex(M.MissionError, 'cutscenes.later|cutscenes.outro'):
                M.compile_mission(pkg, skipped_hide, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates(), occluders=groups)

    def test_old_server_plan_schema_refuses_named_block_tables(self):
        plan = {
            'schema': 'sdgo.map-import-server-plan/2', 'tables': {
                'tbl_MapObjects': [block_template().server], 'tbl_MapBlocks': []},
            'runtime_policy': {'immediate_transition_missions': []},
        }
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'plan.json'
            path.write_text(json.dumps(plan), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, '旧版.*命名物件'):
                server.load_plan(path)


class MissionBlockTests(unittest.TestCase):
    def test_block_transition_precedes_the_entry_camera(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        spec['cutscenes']['gate'] = copy.deepcopy(spec['cutscenes']['intro'])
        spec['phases'][1]['enter'] = {'cutscene': 'gate', 'blocks': [{'block': 'odessa_gate', 'visible': False}]}
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(), templates=templates())
            fn = built.report['cutscenes']['gate']
            body = built.lua.split(f'function {fn}_LuaInit(m)', 1)[1].split('\nend', 1)[0]
            self.assertLess(body.index("SDGOMI.hide_block('SDGOM1:odessa_gate'"), body.index('return SDGOMI.init'))
            self.assertNotIn('function SDGOM1_E1(m)', built.lua)
            self.assertEqual(built.report['block_controls'], 1)

    def test_reused_camera_keeps_each_phase_block_state_separate(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        spec['phases'][1]['enter'] = {'cutscene': 'intro', 'blocks': [{'block': 'odessa_gate', 'visible': False}]}
        later = copy.deepcopy(spec['phases'][1]);later['key'] = 'reclose'
        later['enter']['blocks'][0]['visible'] = True
        spec['phases'].append(later)
        original = copy.deepcopy(spec)
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(), templates=templates())
            self.assertEqual(spec, original)
            bodies = [re.search(r'function '+re.escape(fn)+r'_LuaInit\(m\)(.*?)\bend\b', built.lua, re.S).group(1)
                      for fn in built.report['cutscenes'].values()]
            self.assertEqual(sum("SDGOMI.hide_block('SDGOM1:odessa_gate'" in body for body in bodies), 1)
            self.assertEqual(sum("SDGOMI.unhide_block('SDGOM1:odessa_gate'" in body for body in bodies), 1)
            opening = built.report['cutscenes']['intro']
            opening_body = re.search(r'function '+re.escape(opening)+r'_LuaInit\(m\)(.*?)\bend\b', built.lua, re.S).group(1)
            self.assertNotIn('hide_block', opening_body)

    def test_schema_v3_generates_native_block_attack_and_transition_open(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        spec['npcs']['ally']['block_attack'] = {'block': 'odessa_gate', 'weapon': 'second'}
        spec['phases'][1]['enter']['blocks'] = [{'block': 'odessa_gate', 'visible': False}]
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                      templates=templates())
            lua = built.lua
            self.assertIn("local target = GetBlockID('odessa_gate')", lua)
            self.assertIn('npc:SetAttackTarget(target)', lua)
            self.assertIn('npc:ChangeWeapon(UNIT_WEAPON_CHANGE_SECOND)', lua)
            self.assertIn('npc:SetAttackOnOff(true)', lua)
            self.assertIn("function SDGOM1_E1(m)\n  SDGOMI.hide_block('SDGOM1:odessa_gate', 'odessa_gate')", lua)
            self.assertNotIn('GameMode:HideBlock', lua)
            self.assertIn('SDGOMI.block_ids[key] = id\n  HideObject(id)', lua)
            self.assertIn('local id = GetBlockID(name)\n  if (id == nil or id == 0) and SDGOMI.block_ids ~= nil then id = SDGOMI.block_ids[key] end', lua)
            self.assertIn("return SDGOMI.block_missing('hide', key, name)", lua)
            self.assertIn("return SDGOMI.block_missing('unhide', key, name)", lua)
            self.assertIn("local message = 'SDGOMI '..action..' missing block '..key..' ('..name..')'", lua)
            self.assertIn('SDGOMI.last_block_error = message\n  print(message)', lua)
            self.assertIn('SDGOMI.block_ids[key] = id\n  UnHideObject(id)', lua)
            derived = [raw for kind, _ident, raw in built.rows if kind == M.NPC_TEMPLATE_KIND
                       and raw[109:209].decode('utf-16-le').split('\0')[0] == 'SDGOM1_B1']
            self.assertEqual(len(derived), 1)
            self.assertEqual(built.report['block_attacks'][0]['block'], 'odessa_gate')
            self.assertEqual(built.report['block_controls'], 1)

            older = copy.deepcopy(spec)
            older['schema_version'] = 2
            with self.assertRaisesRegex(M.MissionError, 'schema_version=3'):
                M.compile_mission(pkg, older, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates())
            unknown = copy.deepcopy(spec)
            unknown['unknown_gate_mode'] = True
            with self.assertRaisesRegex(M.MissionError, '未知字段'):
                M.compile_mission(pkg, unknown, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates())
            bad_action = copy.deepcopy(spec)
            bad_action['phases'][1]['enter']['blocks'][0]['fade'] = 1
            with self.assertRaisesRegex(M.MissionError, 'block 与 visible'):
                M.compile_mission(pkg, bad_action, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates())


    def test_initial_hidden_block_caches_id_for_later_unhide(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        spec['phases'][1]['enter']['blocks'] = [{'block': 'odessa_gate', 'visible': True}]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            gate_package(root, mission=spec)
            manifest_path = root / 'map.json'
            manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
            manifest['objects'][0]['initially_hidden'] = True
            manifest_path.write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf-8')
            pkg = P.load(root)
            built = M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                      templates=templates())
            lua = built.lua
            self.assertIn("SDGOMI.hide_block('SDGOM1:odessa_gate', 'odessa_gate')", lua)
            self.assertIn("function SDGOM1_E1(m)\n  SDGOMI.unhide_block('SDGOM1:odessa_gate', 'odessa_gate')", lua)
            self.assertLess(lua.index('SDGOMI.block_ids[key] = id\n  HideObject(id)'),
                            lua.index('and SDGOMI.block_ids ~= nil then id = SDGOMI.block_ids[key]'))

    def test_phase_hidden_block_is_excluded_only_from_that_cutscene(self):
        spec = mission_spec()
        spec['schema_version'] = 3
        gate_scene = copy.deepcopy(spec['cutscenes']['intro'])
        spec['cutscenes']['gate'] = gate_scene
        spec['cutscenes']['intro']['camera'] = [
            {'t': 0, 'pos': [1000, 300, -600], 'look': [1000, 50, 200]},
            {'t': 3, 'pos': [1200, 250, -400], 'look': [1000, 50, 200]},
        ]
        spec['phases'][1]['enter']['cutscene'] = 'gate'
        spec['phases'][1]['enter']['blocks'] = [{'block': 'odessa_gate', 'visible': False}]
        with tempfile.TemporaryDirectory() as tmp:
            pkg = gate_package(Path(tmp), mission=spec)
            gate = pkg.objects[0]
            gate.position = (0.0, 0.0, 0.0)
            gate.collision = [{'center': (0.0, 200.0, -200.0), 'half_extents': (400.0, 200.0, 30.0),
                               'rotation_y': 0.0}]
            grouped = occlusion.object_occluders(pkg, 'mission', None)
            M.compile_mission(pkg, spec, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                              templates=templates(), occluders=grouped)

            closed = copy.deepcopy(spec)
            closed['phases'][1]['enter'].pop('blocks')
            with self.assertRaisesRegex(M.MissionError, 'cutscenes.gate'):
                M.compile_mission(pkg, closed, map_no=144, terrain_path='Trns\\m143x.trn', ids=ids(),
                                  templates=templates(), occluders=grouped)


if __name__ == '__main__':
    unittest.main()
