#!/usr/bin/env python3
"""Add a neutral sdgo.map-mission JSON file to an existing map package.

This authoring helper only copies a local JSON file and updates map.json's SHA-256
manifest. It does not contact a client, server, database, or content provider.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath


def safe_name(value: str) -> str:
    path = PurePosixPath(value)
    if (not value or "\\" in value or path.is_absolute() or len(path.parts) != 1
            or path.name in (".", "..") or not path.name.endswith(".json")):
        raise ValueError("mission name must be one relative .json file name")
    return path.name


def load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain one JSON object")
    return value


def atomic_json(path: Path, value: dict) -> None:
    raw = (json.dumps(value, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(raw)
    os.replace(temporary, path)


def add_mission(package: Path, source: Path, name: str | None = None) -> Path:
    package = package.resolve()
    source = source.resolve()
    manifest_path = package / "map.json"
    manifest = load_json(manifest_path)
    if manifest.get("schema") != "sdgo.map-package" or not isinstance(manifest.get("files"), dict):
        raise ValueError("map.json is not an sdgo.map-package manifest")
    mission = load_json(source)
    if mission.get("schema") != "sdgo.map-mission" or not isinstance(mission.get("mission_key"), str):
        raise ValueError("mission source is not an sdgo.map-mission document")
    leaf = safe_name(name or source.name)
    relative = f"missions/{leaf}"
    target = package / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    raw = (json.dumps(mission, ensure_ascii=False, indent=1) + "\n").encode("utf-8")
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_bytes(raw)
    os.replace(temporary, target)
    manifest["files"][relative] = hashlib.sha256(raw).hexdigest()
    manifest["files"] = dict(sorted(manifest["files"].items()))
    atomic_json(manifest_path, manifest)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="map package folder containing map.json")
    parser.add_argument("mission", type=Path, help="local sdgo.map-mission JSON file")
    parser.add_argument("--name", help="destination file name under missions/ (default: source name)")
    args = parser.parse_args()
    print(add_mission(args.package, args.mission, args.name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
