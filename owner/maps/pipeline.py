"""Shared compile helpers: target-client ID cursors, mission templates and native resource checks.

Every ID space continues right after the target client's own rows. Mission IDs advance by exactly what each
compiled mission used, so consecutive missions form one gap-free block per table.
"""
from __future__ import annotations

from pathlib import Path

from . import mission as M

MAP_FIRST_ID = 100001
SPAWN_FIRST = 2910001        # client spawn mirror rows == server MissionNpcID (no native rows in this range)
MODE_START_FIRST = 2900031   # server-only MissionModeStartID


def native_exists_for(client: Path, baseline):
    client = Path(client).resolve()

    def native_exists(path: str) -> bool:
        relative = path.replace('\\', '/')
        if relative.lower().startswith('bgm/'):       # BGM is streamed from loose files next to the client
            return (client / relative).is_file()
        try:
            return baseline.exists(relative.lower())
        except ValueError:
            return False
    return native_exists


def next_ids(baseline) -> dict:
    def last(path, size, limit=None):
        ids = [i for i in baseline._records(path, size) if limit is None or i <= limit]
        return max(ids)
    catalogue = last('gdrs/0056.gdt', 1309)
    return {
        'map_no': last('gdrs/0254.gdt', 844) - MAP_FIRST_ID + 1,
        'block_object': last('gdrs/0008.gdt', 513) + 1,
        'game_mode': last('gdrs/0021.gdt', 1090, M.MODE5_ID_LIMIT) + 1,
        'route': last('gdrs/0057.gdt', 34) + 1,
        'objective': last('gdrs/0080.gdt', 634) + 1,
        'group': catalogue - 191000 + 1,
        'clear': last('gdrs/0055.gdt', 391) + 1,
        'speech': last('gdrs/0119.gdt', 506) + 1,
        'npc': last('gdrs/0105.gdt', 598) + 1,
        'spawn': SPAWN_FIRST,
        'mode_start': MODE_START_FIRST,
    }


def mission_ids(cursor: dict, index: int) -> M.MissionIds:
    return M.MissionIds(group=cursor['group'], catalogue=191000 + cursor['group'], route_first=cursor['route'],
                        objective_first=cursor['objective'], game_mode_first=cursor['game_mode'],
                        clear_first=cursor['clear'], spawn_first=cursor['spawn'],
                        mode_start_first=cursor['mode_start'], npc_first=cursor['npc'],
                        speech_first=cursor['speech'], lua_prefix=f'SDGOM{index + 1}')


def advance(cursor: dict, built: M.MissionBuild) -> None:
    report = built.report
    modes = len(report['game_modes'])
    cursor['game_mode'] += modes
    cursor['route'] += modes
    cursor['objective'] += modes
    cursor['group'] += 1
    cursor['clear'] += report['clear_rows']
    cursor['spawn'] += report['spawn_rows']
    cursor['npc'] += report['npc_rows']
    cursor['speech'] += report['speech_rows']
    cursor['mode_start'] += report['phases'] * modes


def unit_labels(baseline, unit):
    from . import mod_anim
    record = baseline.record('unit', unit)
    if record is None:
        return set()
    model = record[612:672].decode('utf-16-le').split(chr(0))[0]
    mod = mod_anim.parse_mod(baseline.read(model.replace(chr(92), '/').lower()))
    return {label.rstrip() for label in mod.labels()}


def _server_unit_record(npc_units: dict[int, dict], player_units: dict[int, dict], unit: int,
                        template_unit: int | None = None) -> dict | None:
    if unit in npc_units:
        return dict(npc_units[unit])
    source = player_units.get(unit)
    template = npc_units.get(template_unit) if template_unit is not None else None
    if source is None or template is None:
        return None
    # CustomNumber is required by tbl_UnitNpc but is not part of BattleServer's packed UnitData.  Inherit the
    # selected NPC template's value; all shared native combat fields come from the requested player unit.
    result = {name: source[name] for name in M.SERVER_UNIT_NPC_COLUMNS
              if name not in ('UnitNumber', 'CustomNumber')}
    result.update(UnitNumber=unit, CustomNumber=template['CustomNumber'])
    return result


def mission_templates(baseline, server_ini) -> M.Templates:
    """Native rows the mission mirrors are cloned from, plus the server's NpcData (read-only)."""
    from owner.units import repository

    def records(path, size):
        return baseline._records(path, size)
    npc_templates = {}
    for table in ('gdrs/0020.gdt', 'gdrs/0105.gdt', 'gdrs/0128.gdt', 'gdrs/0162.gdt'):
        npc_templates.update(records(table, 598))
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()

        def exact_rows(table: str, columns: tuple[str, ...], key: str) -> dict[int, dict]:
            cursor.execute(f'SELECT {",".join(columns)} FROM dbo.{table}')
            observed = tuple(item[0] for item in cursor.description)
            if observed != columns:
                raise ValueError(f'{table} 的服务端列与任务协议不匹配')
            result = {}
            for values in cursor.fetchall():
                row = dict(zip(columns, values))
                ident = int(row[key])
                if ident in result:
                    raise ValueError(f'{table} 的 {key} {ident} 不唯一')
                result[ident] = row
            return result

        cursor.execute('SELECT * FROM dbo.tbl_NpcData')
        columns = [d[0] for d in cursor.description]
        npc_data = {}
        for row in cursor.fetchall():
            item = dict(zip(columns, row))
            if isinstance(item.get('NpcName'), str):
                item['NpcName'] = item['NpcName'].rstrip()
            ident = int(item['NpcDataID'])
            if ident in npc_data:
                raise ValueError(f'tbl_NpcData 的 NpcDataID {ident} 不唯一')
            npc_data[ident] = item
        npc_units = exact_rows('tbl_UnitNpc', M.SERVER_UNIT_NPC_COLUMNS, 'UnitNumber')
        player_units = exact_rows('tbl_Units', tuple(repository.UNIT_COLUMNS), 'UnitNumber')
        server_weapons = exact_rows('tbl_WeaponsRev', M.SERVER_WEAPON_COLUMNS, 'WeaponNo')

        # blueprint items (item category -> recipe) and the unit each recipe produces, for mission drops
        cursor.execute('SELECT i.CatalogueID, i.Name, m.OutUnitID FROM dbo.tbl_ItemCategory i '
                       'JOIN dbo.tbl_MixTreeFile m ON m.Idx = i.RefIndex')
        blueprints: dict[int, list[tuple[int, str]]] = {}
        for category, name, unit in cursor.fetchall():
            blueprints.setdefault(int(unit), []).append((int(category), str(name or '').rstrip()))
    finally:
        connection.rollback()
        connection.close()
    clears = records('gdrs/0055.gdt', 391)
    routes = records('gdrs/0057.gdt', 34)
    routes_by_level = {routes[i][9]:routes[i] for i in (195001,195002,195003) if i in routes}
    for _, row in sorted(routes.items()):
        if len(row) == 34 and 1 <= row[9] <= 9:
            routes_by_level.setdefault(row[9], row)
    return M.Templates(
        game_mode=records('gdrs/0021.gdt', 1090)[88006], clear=clears[min(clears)],
        routes=routes_by_level,
        objective=records('gdrs/0080.gdt', 634)[197001], catalogue=records('gdrs/0056.gdt', 1309)[191001],
        speech=records('gdrs/0119.gdt', 506)[3229322], npc_templates=npc_templates, npc_data=npc_data,
        spawn=records('gdrs/0053.gdt', 152)[620001],
        unit_exists=lambda unit: baseline.record('unit', unit) is not None,
        unit_labels=lambda unit: unit_labels(baseline, unit), unit_record=lambda unit: baseline.record('unit', unit),
        weapon_record=lambda weapon: baseline.record('weapon', weapon),
        server_unit_record=lambda unit, template=None: _server_unit_record(npc_units, player_units, unit, template),
        server_weapon_record=lambda weapon: server_weapons.get(weapon),
        blueprints=blueprints,
        native_exists=lambda name: baseline.exists(name.replace('\\', '/').lower()))



def block_templates(baseline, server_ini, template_ids) -> dict:
    """Exact client/server object templates requested by schema-v2 named blocks (read-only)."""
    from owner.units import repository
    from . import block as B
    requested = set(template_ids)
    if not requested:
        return {}
    native = baseline._records('gdrs/0008.gdt', B.RECORD_BYTES)
    connection = repository._connect(Path(server_ini))
    try:
        cursor = connection.cursor()
        cursor.execute('SELECT ' + ','.join(B.SERVER_OBJECT_COLUMNS) + ' FROM dbo.tbl_MapObjects')
        observed = tuple(item[0] for item in cursor.description)
        if observed != B.SERVER_OBJECT_COLUMNS:
            raise ValueError('tbl_MapObjects 的服务端列与命名物件协议不匹配')
        server = {}
        for values in cursor.fetchall():
            row = dict(zip(B.SERVER_OBJECT_COLUMNS, values))
            identity = int(row['ObjectID'])
            if identity in server:
                raise ValueError(f'tbl_MapObjects 的 ObjectID {identity} 不唯一')
            server[identity] = row
    finally:
        connection.rollback()
        connection.close()
    missing = sorted(identity for identity in requested if identity not in native or identity not in server)
    if missing:
        raise ValueError('命名物件模板在目标客户端或服务端不存在：' + ','.join(map(str, missing)))
    return {identity: B.Template(native=native[identity], server=server[identity]) for identity in requested}

def occluders(package, baseline):
    """Named collision groups of the mission variant, for state-aware cutscene checks."""
    from .occlusion import object_occluders
    return object_occluders(package, 'mission', lambda path: baseline.read(path))


def check_placements(package, baseline) -> None:
    """Refuse map spawn/sector/item points inside an object (either variant)."""
    from .occlusion import object_boxes, placement_problems
    from .package import MapPackageError
    problems = []
    for variant in ('pvp', 'mission'):
        boxes = object_boxes(package, variant, lambda path: baseline.read(path))
        problems += [f'{variant}: {p}' for p in placement_problems(package, variant, boxes)]
    if problems:
        raise MapPackageError('以下点位位于物件内部：' + '、'.join(sorted(set(problems))[:12]))
