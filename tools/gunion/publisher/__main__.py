"""Command line entry for the roster publisher stages."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .common import Config


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('families', 'resources', 'programs', 'assemble'))
    parser.add_argument('--config', type=Path, required=True, help='private JSON with delivery, plan, baseline and output locations')
    args = parser.parse_args(argv)
    config = Config(args.config)
    if args.stage == 'families':
        from .families import build
        result = build(config)
        print(json.dumps({'stage': 'families', **result['counts'], 'output': str(config.output / 'families.json')}, ensure_ascii=False))
        return 0
    if args.stage == 'resources':
        from .resources import build
        result = build(config)
        print(json.dumps({'stage': 'resources', **result['counts'], 'output': str(config.output / 'resources.json')}, ensure_ascii=False))
        return 0
    if args.stage == 'programs':
        from .programs import build
        result = build(config)
        print(json.dumps({'stage': 'programs', **result['counts'], 'output': str(config.output / 'programs.json')}, ensure_ascii=False))
        return 0
    from .assemble import build
    result = build(config)
    print(json.dumps({'stage': 'assemble', **result['counts'], 'output': str(config.output / 'publication.json')}, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
