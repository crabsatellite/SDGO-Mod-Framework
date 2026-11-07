"""Stage runtime contracts from each Mod's authoritative Manager declaration."""
from pathlib import Path
import argparse
import json
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manager.configuration import render


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--manifest',type=Path,required=True);parser.add_argument('--contract',type=Path,required=True);args=parser.parse_args()
    raw=render(args.manifest.read_bytes(),json.loads(args.contract.read_text('utf-8-sig')))
    if args.manifest.read_bytes()!=raw:args.manifest.write_bytes(raw)
