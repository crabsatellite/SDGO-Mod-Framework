#!/usr/bin/env python3
"""Compile optional unit-project Ace metadata into a managed owner content overlay."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from owner.units.ace_pilots import catalog_from_projects,encode_catalog


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--projects',required=True,type=Path,help='UTF-8 JSON array of reviewed unit drafts or portable sdgo.unit-project manifests')
    parser.add_argument('--out',required=True,type=Path,help='new owner-pilots.json path')
    args=parser.parse_args(argv)
    if args.out.exists():raise FileExistsError('output already exists')
    projects=json.loads(args.projects.read_text(encoding='utf-8-sig'))
    if not isinstance(projects,list):raise ValueError('projects input must be a JSON array')
    data=encode_catalog(catalog_from_projects(projects))
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_bytes(data)
    if args.out.read_bytes()!=data:raise OSError('owner pilot overlay readback failed')
    print(args.out)


if __name__=='__main__':main()
