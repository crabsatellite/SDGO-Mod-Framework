"""Compile a neutral map package (sdgo.map-package v1) into native resources, map rows and server rows.

Usage:
    python tools/map_compile.py <package dir> --map-no 143 --map-id 500 [--mission-map-no 144]
                                [--client <client dir>] [--out <dir>]

With --client the compiler checks referenced native models/BGM exist and that no generated resource name
collides with the client's resources, and verifies the compiled .trn round-trips. Without --out it only
reports.
"""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.maps import compiler, package  # noqa: E402


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('package', type=Path)
    parser.add_argument('--map-no', type=int, required=True)
    parser.add_argument('--map-id', type=int, required=True)
    parser.add_argument('--mission-map-no', type=int, default=0)
    parser.add_argument('--client', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--publish', type=Path, help='write the client ZIP + server plan here')
    parser.add_argument('--program', type=Path, help='include map_catalog.dll/mod.ini/... from this stage folder')
    parser.add_argument('--mission', action='append', default=[], help='package-relative mission JSON to compile')
    parser.add_argument('--server-ini', type=Path, help='read-only NpcData lookup for mission NPC templates')
    parser.add_argument('--mission-group', type=int, default=0, help='catalogue group (default: next free)')
    args = parser.parse_args(argv)

    native_exists = None
    if args.client:
        from owner.units.assets import Baseline
        from owner.maps import pipeline
        baseline = Baseline(args.client)
        native_exists = pipeline.native_exists_for(args.client, baseline)

    pkg = package.load(args.package)
    if args.mission:
        pkg = dataclasses.replace(pkg, manifest={**pkg.manifest, 'missions': list(args.mission)})
    if args.client:
        pipeline.check_placements(pkg, baseline)
    shared: dict = {}
    builds = [compiler.compile_map(pkg, map_no=args.map_no, map_id=args.map_id, variant='pvp',
                                   native_exists=native_exists, shared=shared)]
    if args.mission_map_no:
        builds.append(compiler.compile_map(pkg, map_no=args.mission_map_no, map_id=args.map_id, variant='mission',
                                           prefix=builds[0].prefix, native_exists=native_exists, shared=shared))
    for build in builds:
        compiler.verify_trn(build.resources[build.report['trn'].replace('\\', '/').lower()])
        print(json.dumps(build.report, ensure_ascii=False))
    resources = builds[-1].resources
    print('resources', len(resources), 'bytes', sum(len(v) for v in resources.values()))
    missions = []
    if args.mission:
        if not args.client or not args.server_ini or not args.mission_map_no:
            raise SystemExit('--mission needs --client, --server-ini and --mission-map-no')
        missions = compile_missions(args, pkg, baseline, builds[-1])
    if args.publish:
        from owner.maps import bundle
        program = None
        if args.program:
            program = {name: (args.program / name).read_bytes() for name in
                       ('map_catalog.dll', 'mod.ini', 'manager.json', 'symbols.fragment.ini')}
        print(json.dumps(bundle.build_publication(builds, args.publish, program=program, missions=missions),
                         ensure_ascii=False))
    if args.out:
        out = args.out
        out.mkdir(parents=True, exist_ok=True)
        for name, raw in resources.items():
            target = out / 'resources' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
        rows = [{'kind': k, 'id': i, 'sha256': hashlib.sha256(b).hexdigest()} for b_ in builds for k, i, b in b_.rows]
        (out / 'rows.bin').write_bytes(b''.join(b for b_ in builds for _, _, b in b_.rows))
        server = {}
        for build in builds:
            for table, items in build.server.items():
                server.setdefault(table, [])
                for item in items:
                    if item not in server[table]:
                        server[table].append(item)
        (out / 'build.json').write_text(json.dumps({'reports': [b.report for b in builds], 'rows': rows, 'server': server,
                                                    'loose': sorted(builds[0].loose)}, ensure_ascii=False, indent=1),
                                        encoding='utf-8')
    return 0


def compile_missions(args, pkg, baseline, mission_build):
    """Compile mission JSON files with templates read from the target client and the server's NpcData."""
    from owner.maps import mission as M, pipeline
    templates = pipeline.mission_templates(baseline, args.server_ini)
    banner = M.banner_effect_factory(lambda path: baseline.read(path))
    cursor = pipeline.next_ids(baseline)
    occluders = pipeline.occluders(pkg, baseline)
    if args.mission_group:
        cursor['group'] = args.mission_group
    builds = []
    for index, relative in enumerate(args.mission):
        spec = json.loads(pkg.read(relative))
        build = M.compile_mission(pkg, spec, map_no=args.mission_map_no, terrain_path=mission_build.report['trn'],
                                  ids=pipeline.mission_ids(cursor, index), templates=templates, banner_effect=banner,
                                  occluders=occluders)
        for kind, ident, raw in build.rows:
            if baseline.any_id(ident):
                raise SystemExit(f'mission record id {ident} (kind {kind:#x}) already exists in the client')
        pipeline.advance(cursor, build)
        print(json.dumps(build.report, ensure_ascii=False))
        builds.append(build)
    return builds


if __name__ == '__main__':
    raise SystemExit(main())
