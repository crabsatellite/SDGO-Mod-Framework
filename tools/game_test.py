"""Prepare and run an isolated normal-client SDGO GameTest.

The first preparation copies a real client. Later capsules can move and reuse
the stopped owned client, retaining compact receipts instead of another full
copy. Built versions of installed Mods and the native observer are overlaid
before running against an already-running backend. The player template is
never edited. The environment subcommand owns separate native service copies;
the lower-level prepare/run commands use an already-running backend.
"""
from __future__ import annotations

import argparse
import configparser
import ctypes
from ctypes import wintypes
import datetime as dt
import hashlib
import json
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import socket
import struct
import string
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
import importlib.util


SCHEMA_VERSION = 2
OWNED_BUILD_ROOT=Path(__file__).resolve().parents[1]/'build'
HOST_KEY = "GOnline-54BE1568-00C97000"
GAME_EXE = "GOnline.exe"
STARTUP_EVENT_NAME = "9YOU SDGO"
TEST_MOD = "game_test_client"
TARGET_MOD = "match_ai"
FPS_MOD = "fps_limit"
FPS_SUITE = "native-fps-limit"
FPS_CASES = (
    "framework.bootstrap",
    "fps.native_cap",
    "fps.native_cadence",
)
ROUTED_LOOPBACK_POLICY = "routed_loopback"
ROUTE_PROFILE_RELATIVE = Path("sdgo/mods/net_route/config.ini")
ROUTE_PLAN_FIELDS = (
    "route_profile_path",
    "route_profile_sha256",
    "route_profile_route_count",
    "logical_login_ipv4",
    "logical_login_port",
    "physical_login_ipv4",
    "physical_login_port",
)
SHOP_ASSERTIONS_JOB_PATH = "inputs/shop-assertions.ini"
SHOP_DISPLAY_JOB_PATH = "inputs/shop-runtime-display.ini"
SHOP_ASSERTIONS_RELATIVE = Path("sdgo/gametest") / SHOP_ASSERTIONS_JOB_PATH
SHOP_DISPLAY_RELATIVE = Path("sdgo/gametest") / SHOP_DISPLAY_JOB_PATH
SHOP_PLAN_FIELDS = (
    "shop_assertions_path",
    "shop_assertions_sha256",
    "shop_display_path",
    "shop_display_sha256",
    "shop_revision",
)
FLOW_CASES = (
    "framework.bootstrap",
    "flow.login",
    "flow.onboarding",
    "pvp.room",
    "pvp.start",
    "pvp.ai_spawn",
    "pvp.return",
)
SUPPORTED_PVP_MODES = (1, 3, 6, 7, 9)
MODE_FLOW_STEPS = ("room", "start", "ai_spawn", "return")
MISSION_SUITE = "native-mission-smoke"
SUPPORTED_MISSION_MODES = (5, 8)
MISSION_FLOW_CASES = (
    "mission.room",
    "mission.start",
    "mission.ai_spawn",
    "mission.combat",
)
MISSION_SCOPE = "bounded_native_smoke"
MISSION_AI_COUNT = 3
MISSION_AI_LEVEL = 2
MISSION_EXISTING_ACCOUNT_STATES = frozenset(("registered", "existing_account"))
MISSION_CLAIM_FIELDS = (
    "mission_clear_claim",
    "mission_result_claim",
    "mission_active_exit_claim",
)
IGNORED_DIRECTORIES = {
    ".git",
    ".sdgo-updater",
    ".svn",
    "__pycache__",
    "app_log",
    "logs",
    "screenshots",
    "zoa_log",
}
IGNORED_SUFFIXES = {".bak", ".dmp", ".log", ".tmp"}
IMMUTABLE_ROOT_SUFFIXES = {".dll", ".exe", ".zdx", ".zpk"}
ALNUM = string.ascii_letters + string.digits


class GameTestError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(data, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GameTestError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise GameTestError(f"{path} is not a JSON object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def copy_ignore(directory: str, names: list[str]) -> set[str]:
    ignored = set()
    parent = Path(directory).name.lower()
    for name in names:
        lowered = name.lower()
        if (
            lowered in IGNORED_DIRECTORIES
            or Path(lowered).suffix in IGNORED_SUFFIXES
            or "backup" in lowered
            or "澶囦唤" in name
            or (parent == "sdgo" and lowered == "gametest")
        ):
            ignored.add(name)
    return ignored


def git_source_ref(root: Path) -> dict:
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout
        return {"commit": commit, "dirty": bool(status.strip())}
    except (OSError, subprocess.SubprocessError):
        return {"commit": None, "dirty": None}


def ini_parser() -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    return parser


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = ini_parser()
    try:
        raw=path.read_bytes()
        if raw.startswith((b'\xff\xfe',b'\xfe\xff')):
            decoded=raw.decode('utf-16')
        else:
            try:decoded=raw.decode('utf-8-sig')
            except UnicodeDecodeError:decoded=raw.decode('cp936')
        parser.read_string(decoded)
    except (OSError, UnicodeError, configparser.Error) as error:
        raise GameTestError(f"invalid INI file {path}: {error}") from error
    return parser


def write_ini(path: Path, parser: configparser.ConfigParser) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        parser.write(stream, space_around_delimiters=False)
    os.replace(temporary, path)


def copy_file_if_missing(source: Path, destination: Path) -> None:
    if destination.is_file():
        return
    if not source.is_file():
        raise GameTestError(f"normal client lacks {destination.name} and stage lacks fallback: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def manifest_configuration(path: Path) -> configparser.ConfigParser:
    try:
        parser = read_ini(path)
    except GameTestError:
        # An unrelated diagnostic may have legacy duplicate settings sections.
        # Dependencies belong to its first native [mod] section; do not rewrite
        # or reinterpret that diagnostic's authored settings.
        raw=path.read_bytes()
        decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
        match=re.search(r'(?ims)^\s*\[mod\]\s*\n(.*?)(?=^\s*\[|\Z)',decoded)
        if not match:raise GameTestError(f'manifest lacks [mod]: {path}') from None
        parser=ini_parser();parser.read_string('[mod]\n'+match[1])
    return parser


def retained_manifest_enabled(path: Path) -> bool:
    # Native core never overrides a manifest's own off switch. Inactive
    # diagnostics may retain historical aliases and are not runtime consumers.
    return manifest_configuration(path).get('mod','enabled',fallback='1').strip()!='0'


def manifest_requires(path: Path) -> set[str]:
    parser=manifest_configuration(path)
    try:
        value = parser["mod"].get("requires", "")
    except KeyError as error:
        raise GameTestError(f"manifest lacks [mod]: {path}") from error
    return {item.strip() for item in value.split(",") if item.strip()}


def merge_required_symbols(
    stage_symbols: Path,
    copied_symbols: Path,
    manifests: list[Path],
    *,
    preserve_existing: bool = False,
    allow_missing: bool = False,
) -> None:
    source = read_ini(stage_symbols)
    destination = read_ini(copied_symbols) if copied_symbols.is_file() else ini_parser()
    required = {"host"}
    for manifest in manifests:
        required.update(manifest_requires(manifest))
    missing = sorted(section for section in required if not source.has_section(section) and
                     not (preserve_existing and destination.has_section(section)))
    if missing and not allow_missing:
        raise GameTestError(f"stage host symbols lack required sections: {', '.join(missing)}")
    for section in sorted(required):
        if not source.has_section(section):continue
        if destination.has_section(section):
            destination.remove_section(section)
        destination.add_section(section)
        for key, value in source.items(section, raw=True):
            destination.set(section, key, value)
    copied_symbols.parent.mkdir(parents=True, exist_ok=True)
    write_ini(copied_symbols, destination)


def merge_retained_addon_symbols(stage_symbols:Path,copied_symbols:Path,manifests:list[Path]) -> None:
    """Retain installed addons through their declared, matching-host fragments."""
    active=[]
    destination=read_ini(copied_symbols)
    for manifest in manifests:
        raw=manifest.read_bytes()
        decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
        match=re.search(r'(?ims)^\s*\[mod\]\s*\n(.*?)(?=^\s*\[|\Z)',decoded)
        if not match:raise GameTestError('retained addon lacks its native manifest')
        config=ini_parser();config.read_string('[mod]\n'+match[1])
        if config.getint('mod','enabled',fallback=1)==0:continue
        active.append(manifest)
        required=manifest_requires(manifest)
        if all(destination.has_section(key) for key in required):continue
        fragment=manifest.parent/'symbols.fragment.ini'
        if not fragment.is_file():continue
        source=read_ini(fragment)
        if not source.has_section('host') or dict(source['host'])!=dict(destination['host']):
            raise GameTestError('retained addon symbol fragment describes a different host')
        for key in required:
            if not destination.has_section(key) and source.has_section(key):destination[key]=dict(source[key])
    write_ini(copied_symbols,destination)
    merge_required_symbols(stage_symbols,copied_symbols,active,preserve_existing=True)


def retained_manifest_enabled(manifest:Path) -> bool:
    raw=manifest.read_bytes()
    decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
    match=re.search(r'(?ims)^\s*\[mod\]\s*\n(.*?)(?=^\s*\[|\Z)',decoded)
    if not match:raise GameTestError('retained addon lacks its native manifest')
    config=ini_parser();config.read_string('[mod]\n'+match[1])
    return config.getint('mod','enabled',fallback=1)!=0
def enable_test_configuration(root: Path, fps_mod_enabled: bool | None = None) -> None:
    manifest = root / "sdgo" / "mods" / TEST_MOD / "mod.ini"
    parser = read_ini(manifest)
    if not parser.has_section("mod") or parser["mod"].get("id") != TEST_MOD:
        raise GameTestError(f"invalid staged observer manifest: {manifest}")
    parser["mod"]["enabled"] = "1"
    write_ini(manifest, parser)

    settings_path = root / "sdgo" / "sdgo.ini"
    settings = read_ini(settings_path)
    for section in ("categories", "features", "mods"):
        if not settings.has_section(section):
            settings.add_section(section)
    settings["features"]["game-test"] = "1"
    settings["mods"][TEST_MOD] = "1"
    if fps_mod_enabled is None:
        settings["categories"]["development"] = "1"
        settings["categories"]["gameplay"] = "1"
        settings["features"]["pvp-ai"] = "1"
        settings["mods"][TARGET_MOD] = "1"
    else:
        # Opening a category for the observer/FPS Mod must not accidentally
        # activate unrelated Mods that the source client disabled globally.
        for category, required in (
            ("development", {TEST_MOD}),
            ("gameplay", {FPS_MOD} if fps_mod_enabled else set()),
        ):
            originally_enabled = settings.get("categories", category, fallback="1") != "0"
            if not originally_enabled:
                for candidate in (root / "sdgo" / "mods").glob("*/mod.ini"):
                    manifest = read_ini(candidate)
                    identifier = manifest.get("mod", "id", fallback="")
                    if (
                        identifier
                        and identifier not in required
                        and manifest.get("mod", "category", fallback="") == category
                    ):
                        settings["mods"][identifier] = "0"
            if required:
                settings["categories"][category] = "1"
        value = "1" if fps_mod_enabled else "0"
        settings["features"]["fps-limit"] = value
        settings["mods"][FPS_MOD] = value
    write_ini(settings_path, settings)


def validate_observer_stage(
    root: Path,
    stage: Path,
    extra_mods: tuple[str, ...] = (),
    *,
    fps_mod_enabled: bool | None = None,
) -> None:
    """Validate overlay dependencies before changing or moving an owned client."""
    client=stage/'client';mods=client/'sdgo/mods'
    observer=mods/TEST_MOD
    required_payloads = [
        observer/f'{TEST_MOD}.dll',
        observer/'mod.ini',
        client/'sdgo/core.dll',
    ]
    if fps_mod_enabled is None:
        required_payloads.extend((
            root/f'sdgo/mods/{TARGET_MOD}/mod.ini',
            root/f'sdgo/mods/{TARGET_MOD}/{TARGET_MOD}.dll',
        ))
    for required in required_payloads:
        if not required.is_file():raise GameTestError(f'stage or normal client lacks required payload: {required}')
    if read_ini(observer/'mod.ini').get('mod','id',fallback='')!=TEST_MOD:
        raise GameTestError('invalid staged observer manifest')
    updated={TEST_MOD:observer/'mod.ini'}
    for identifier in extra_mods:
        if not re.fullmatch(r'[a-z][a-z0-9_]{0,63}', identifier):
            raise GameTestError('invalid explicit test Mod ID')
        manifest=mods/identifier/'mod.ini'
        if not manifest.is_file() or not (manifest.parent/(identifier+'.dll')).is_file():
            raise GameTestError('explicit test Mod is not fully staged: '+identifier)
        cfg=read_ini(manifest)
        if cfg.get('mod','id',fallback='')!=identifier or HOST_KEY not in cfg.get('mod','hosts',fallback='').split(','):
            raise GameTestError('explicit test Mod identity/host mismatch: '+identifier)
        updated[identifier]=manifest
    for manifest in sorted(mods.glob('*/mod.ini')):
        identifier=manifest.parent.name
        if identifier==TEST_MOD:continue
        if fps_mod_enabled is not None and identifier==TARGET_MOD:continue
        if not (root/f'sdgo/mods/{identifier}/mod.ini').is_file():continue
        parser=read_ini(manifest)
        if identifier!=TARGET_MOD and parser.get('mod','category',fallback='')=='development':continue
        if parser.get('mod','id',fallback='')!=identifier:
            raise GameTestError(f'staged Mod identity mismatch: {identifier}')
        updated[identifier]=manifest
    if fps_mod_enabled is None:
        target=mods/TARGET_MOD/f'{TARGET_MOD}.dll'
        if target.is_file() and TARGET_MOD not in updated:
            raise GameTestError('staged production match_ai DLL requires its matching manifest')
        if not target.is_file():updated[TARGET_MOD]=root/f'sdgo/mods/{TARGET_MOD}/mod.ini'
    else:
        staged_fps=mods/FPS_MOD
        installed_fps=root/'sdgo/mods'/FPS_MOD
        staged_manifest=staged_fps/'mod.ini'
        staged_dll=staged_fps/f'{FPS_MOD}.dll'
        staged_partial=staged_manifest.exists()!=staged_dll.exists()
        if staged_partial:
            raise GameTestError('staged fps_limit requires a matching DLL and manifest')
        if staged_manifest.is_file():
            if read_ini(staged_manifest).get('mod','id',fallback='')!=FPS_MOD:
                raise GameTestError('fps_limit manifest has the wrong identity')
            updated[FPS_MOD]=staged_manifest
        else:
            installed_manifest=installed_fps/'mod.ini'
            installed_dll=installed_fps/f'{FPS_MOD}.dll'
            if not installed_manifest.is_file() or not installed_dll.is_file():
                raise GameTestError(
                    'native FPS GameTest requires installed fps_limit or a staged matching DLL and manifest'
                )
            if read_ini(installed_manifest).get('mod','id',fallback='')!=FPS_MOD:
                raise GameTestError('fps_limit manifest has the wrong identity')
    source=read_ini(client/f'sdgo/symbols/{HOST_KEY}.ini')
    required={'host'}
    for manifest in updated.values():required.update(manifest_requires(manifest))
    existing_path=root/f'sdgo/symbols/{HOST_KEY}.ini'
    existing=read_ini(existing_path) if existing_path.is_file() else ini_parser()
    missing=sorted(
        section for section in required
        if section not in source.sections()
        and not (
            fps_mod_enabled is not None
            and FPS_MOD not in updated
            and section in existing.sections()
        )
    )
    if missing:raise GameTestError(f"stage host symbols lack required sections: {', '.join(missing)}")
    if fps_mod_enabled is None:
        for manifest in (root/'sdgo/mods').glob('*/mod.ini'):
            if manifest.parent.name in updated or not retained_manifest_enabled(manifest):
                continue
            available=set(source.sections())|set(existing.sections())
            fragment=manifest.parent/'symbols.fragment.ini'
            if fragment.is_file():
                extra=read_ini(fragment)
                if not extra.has_section('host') or dict(extra['host'])!=dict(source['host']):
                    raise GameTestError('retained addon symbol fragment describes a different host')
                available.update(extra.sections())
            missing=sorted(manifest_requires(manifest)-available)
            if missing:raise GameTestError(f"retained Mod symbols lack required sections: {', '.join(missing)}")


def overlay_observer(
    root: Path,
    stage: Path,
    extra_mods: tuple[str, ...] = (),
    *,
    fps_mod_enabled: bool | None = None,
) -> None:
    validate_observer_stage(root,stage,extra_mods,fps_mod_enabled=fps_mod_enabled)
    stage_client = stage / "client"
    observer_source = stage_client / "sdgo" / "mods" / TEST_MOD
    if not (observer_source / f"{TEST_MOD}.dll").is_file() or not (observer_source / "mod.ini").is_file():
        raise GameTestError(f"stage lacks native observer Mod: {observer_source}")

    copy_file_if_missing(stage_client / "d3d9.dll", root / "d3d9.dll")
    # The current test Mod can require a newer manifest loader. Bind the
    # staged core in this isolated copy; leave the normal template untouched.
    staged_core=stage_client / "sdgo" / "core.dll"
    if not staged_core.is_file():
        raise GameTestError(f"stage lacks framework core: {staged_core}")
    shutil.copy2(staged_core,root / "sdgo" / "core.dll")
    copy_file_if_missing(stage_client / "sdgo" / "sdgo.ini", root / "sdgo" / "sdgo.ini")

    observer_target = root / "sdgo" / "mods" / TEST_MOD
    if observer_target.exists():
        shutil.rmtree(observer_target)
    shutil.copytree(observer_source, observer_target)

    target_manifest = root / "sdgo" / "mods" / TARGET_MOD / "mod.ini"
    target_dll = root / "sdgo" / "mods" / TARGET_MOD / f"{TARGET_MOD}.dll"
    if fps_mod_enabled is None:
        if not target_manifest.is_file() or not target_dll.is_file():
            raise GameTestError(
                "normal copied client lacks match_ai; install the normal compiled target before preparing GameTest"
            )
        staged_target_dll = (
            stage_client / "sdgo" / "mods" / TARGET_MOD / f"{TARGET_MOD}.dll"
        )
        if staged_target_dll.is_file():
            staged_target_manifest=staged_target_dll.with_name('mod.ini')
            if not staged_target_manifest.is_file():
                raise GameTestError('staged production match_ai DLL requires its matching manifest')
            previous_manifest=read_ini(target_manifest)
            current_manifest=read_ini(staged_target_manifest)
            if current_manifest.get('mod','id',fallback='')!=TARGET_MOD:
                raise GameTestError('staged production match_ai manifest has the wrong identity')
            current_manifest['mod']['enabled']='1'
            if previous_manifest.has_section('settings'):
                if not current_manifest.has_section('settings'):current_manifest.add_section('settings')
                for key,value in previous_manifest.items('settings',raw=True):
                    current_manifest['settings'][key]=value
            write_ini(target_manifest,current_manifest)
            shutil.copy2(staged_target_dll, target_dll)

    # Exercise the built master implementation of every already-installed Mod,
    # including its Lua policy, while keeping private owner settings local.
    # An unrelated installed addon without a staged counterpart is preserved.
    updated_manifests=[observer_target/'mod.ini']
    if fps_mod_enabled is None:
        updated_manifests.append(target_manifest)
    fps_manifest = None
    if fps_mod_enabled is not None:
        installed_fps = root / "sdgo" / "mods" / FPS_MOD
        staged_fps = stage_client / "sdgo" / "mods" / FPS_MOD
        installed_complete = (
            (installed_fps / "mod.ini").is_file()
            and (installed_fps / f"{FPS_MOD}.dll").is_file()
        )
        staged_complete = (
            (staged_fps / "mod.ini").is_file()
            and (staged_fps / f"{FPS_MOD}.dll").is_file()
        )
        staged_partial = (staged_fps / "mod.ini").exists() != (staged_fps / f"{FPS_MOD}.dll").exists()
        if staged_partial:
            raise GameTestError("staged fps_limit requires a matching DLL and manifest")
        if staged_complete:
            if installed_fps.exists():
                shutil.rmtree(installed_fps)
            shutil.copytree(staged_fps, installed_fps)
            installed_complete = True
        if not installed_complete:
            raise GameTestError(
                "native FPS GameTest requires installed fps_limit or a staged matching DLL and manifest"
            )
        fps_manifest = installed_fps / "mod.ini"
        fps_config = read_ini(fps_manifest)
        if fps_config.get("mod", "id", fallback="") != FPS_MOD:
            raise GameTestError("fps_limit manifest has the wrong identity")
        fps_config["mod"]["enabled"] = "1" if fps_mod_enabled else "0"
        write_ini(fps_manifest, fps_config)
    for identifier in extra_mods:
        if identifier in (TEST_MOD,TARGET_MOD) or (fps_mod_enabled is not None and identifier==FPS_MOD):
            continue
        source=stage_client/'sdgo/mods'/identifier
        destination=root/'sdgo/mods'/identifier
        destination.mkdir(parents=True,exist_ok=True)
        from tools.game_test_environment import no_reparse_tree
        no_reparse_tree(source)
        shutil.copytree(source,destination,dirs_exist_ok=True)
        current=read_ini(destination/'mod.ini');current['mod']['enabled']='1'
        write_ini(destination/'mod.ini',current)
        settings=read_ini(root/'sdgo/sdgo.ini')
        if not settings.has_section('mods'):settings.add_section('mods')
        settings['mods'][identifier]='1'
        write_ini(root/'sdgo/sdgo.ini',settings)
        updated_manifests.append(destination/'mod.ini')
    for staged_manifest in sorted((stage_client/'sdgo/mods').glob('*/mod.ini')):
        identifier=staged_manifest.parent.name
        if identifier in (TEST_MOD,TARGET_MOD) or (fps_mod_enabled is not None and identifier == FPS_MOD):continue
        destination=root/'sdgo/mods'/identifier
        installed_manifest=destination/'mod.ini'
        if not installed_manifest.is_file():continue
        current=read_ini(staged_manifest)
        if current.get('mod','category',fallback='')=='development':continue
        previous=read_ini(installed_manifest)
        if previous.has_option('mod','enabled'):current['mod']['enabled']=previous['mod']['enabled']
        if current.get('mod','id',fallback='')!=identifier:
            raise GameTestError(f'staged Mod identity mismatch: {identifier}')
        if previous.has_section('settings'):
            if not current.has_section('settings'):current.add_section('settings')
            for key,value in previous.items('settings',raw=True):current['settings'][key]=value
        write_ini(installed_manifest,current)
        updated_manifests.append(installed_manifest)
        for payload in staged_manifest.parent.iterdir():
            if payload.is_file() and payload.suffix.lower() in ('.dll','.lua'):
                shutil.copy2(payload,destination/payload.name)

    stage_symbols = stage_client / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
    copied_symbols = root / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
    if not stage_symbols.is_file():
        raise GameTestError(f"stage lacks host symbols: {stage_symbols}")
    merge_required_symbols(
        stage_symbols,
        copied_symbols,
        updated_manifests,
    )
    if fps_manifest is not None:
        merge_required_symbols(stage_symbols,copied_symbols,[fps_manifest],preserve_existing=True)
    retained_manifests=[manifest for manifest in (root/'sdgo/mods').glob('*/mod.ini')
                        if manifest not in updated_manifests and manifest != fps_manifest and retained_manifest_enabled(manifest)]
    if fps_mod_enabled is None:
        merge_retained_addon_symbols(stage_symbols,copied_symbols,retained_manifests)
    else:
        merge_required_symbols(
            stage_symbols,
            copied_symbols,
            retained_manifests,
            preserve_existing=True,
            allow_missing=True,
        )
    enable_test_configuration(root, fps_mod_enabled)


def configure_sfx_only(root: Path) -> dict:
    """Change only the prepared client's audio settings before hashing it."""
    path = root / "config.xml"
    try:
        tree = ET.parse(path)
        sound = tree.getroot().find("Sound")
        if sound is None:
            raise ValueError("missing Sound settings")
        bgm = sound.find("BGMVolume")
        effects = sound.find("EffectVolume")
        if bgm is None or effects is None:
            raise ValueError("missing BGMVolume or EffectVolume")
        previous_effects = int(effects.get("Value", "0"))
        bgm.set("Value", "0")
        bgm.set("use", "false")
        effects.set("use", "true")
        effects.set("Value", str(previous_effects if 0 < previous_effects <= 100 else 40))
        tree.write(path, encoding="utf-8", xml_declaration=True)
        return {"audio": "sfx_only", "bgm_volume": 0,
                "effect_volume": int(effects.get("Value")), "process_audio_only": True}
    except (OSError, ET.ParseError, ValueError) as error:
        raise GameTestError(f"cannot configure SFX-only test client: {error}") from error


def configure_muted_audio(root: Path) -> dict:
    path=root/'config.xml'
    try:
        tree=ET.parse(path)
        video=tree.getroot().find('Video')
        if video is None:video=ET.SubElement(tree.getroot(),'Video')
        screen=video.find('ScreenMode')
        if screen is None:screen=ET.SubElement(video,'ScreenMode',{'width':'1024','height':'768','bpp':'32'})
        screen.set('mode','window');screen.set('windowFull','0')
        effects=video.find('PostEffect')
        if effects is None:effects=ET.SubElement(video,'PostEffect')
        for key,level in (('MotionBlur','MotionBlurLevel'),('HeatHaze','HeatHazeLevel'),
                          ('Bloom','BloomLevel'),('EdgeLine','EdgeLineType'),('DOF','DOFLevel')):
            effect=effects.find(key)
            if effect is None:effect=ET.SubElement(effects,key)
            effect.set('Enable','false')
            # The original settings Apply recomputes Enable from these levels.
            # Enable=false alone is overwritten as soon as the native app starts.
            effect.set(level,'0')
        sound=tree.getroot().find('Sound')
        if sound is None:raise ValueError('missing Sound settings')
        for key in ('BGMVolume','EffectVolume','VoiceVolume'):
            setting=sound.find(key)
            if setting is None:raise ValueError(f'missing {key}')
            # Keep the legacy sound-driver objects initialized. Zero volume
            # silences the copy without disabling their native lifecycle.
            setting.set('Value','0');setting.set('use','true')
        tree.write(path,encoding='utf-8',xml_declaration=True)
        return {'audio':'muted','bgm_volume':0,'effect_volume':0,'voice_volume':0}
    except (OSError,ET.ParseError,ValueError) as error:
        raise GameTestError(f'cannot prepare a silent test client: {error}') from error


def configure_fps_test(root: Path, fps_cap: int) -> None:
    path = root / "config.xml"
    try:
        tree = ET.parse(path)
        document = tree.getroot()
        timer = document.find("Framework/Timer")
        if timer is None:
            raise ValueError("missing Framework/Timer settings")
        timer.set("VerticalSync", "false")
        timer.set("RenderSync", "false")
        timer.set("updatefps", str(fps_cap))

        # The copied client must stop at the login page without authenticating
        # any saved player identity from the source template.
        login = document.find("LoginData")
        if login is None:
            login = ET.SubElement(document, "LoginData")
        last_server = login.find("LastJoinServer")
        for child in list(login):
            if child is not last_server:
                login.remove(child)
        login.attrib.clear()
        ET.SubElement(login, "CheckLoginIdSave", {"use": "false", "id": ""})
        ET.SubElement(login, "CheckAutoLogin", {"use": "false", "id": "", "pass": ""})
        tree.write(path, encoding="utf-8", xml_declaration=True)
    except (OSError, ET.ParseError, ValueError) as error:
        raise GameTestError(f"cannot configure native FPS test client: {error}") from error


def validate_muted_audio(root: Path) -> None:
    try:
        document=ET.parse(root/'config.xml').getroot()
        screen=document.find('Video/ScreenMode')
        if screen is None or screen.get('mode')!='window' or screen.get('windowFull')!='0':
            raise ValueError('background tests must use a windowed device and may not change the display mode')
        if any(document.find('Video/PostEffect/'+key) is None or
               document.find('Video/PostEffect/'+key).get('Enable')!='false' or
               document.find('Video/PostEffect/'+key).get(level)!='0'
               for key,level in (('MotionBlur','MotionBlurLevel'),('HeatHaze','HeatHazeLevel'),
                                 ('Bloom','BloomLevel'),('EdgeLine','EdgeLineType'),('DOF','DOFLevel'))):
            raise ValueError('headless logic tests require the native no-postprocessing graphics profile')
        sound=document.find('Sound')
        if sound is None or any(sound.find(key) is None or
            sound.find(key).get('Value')!='0' or sound.find(key).get('use') not in ('true','false')
            for key in ('BGMVolume','EffectVolume','VoiceVolume')):
            raise ValueError('all three test audio channels must have zero volume')
    except (OSError,ET.ParseError,ValueError) as error:
        raise GameTestError(f'quiet test audio policy failed before launch: {error}') from error


class QuietRuntime:
    def __init__(self,metadata: dict,receipt_dir: Path):
        self.metadata=metadata;self.directory=receipt_dir;self.process=None;self.firewall=None;self.firewall_attempted=False
        self.proof_file_written=False
        self.guard_unavailable_since=None
        self.guard_proxy=0
        self.background_unavailable_since=None
        self.receipt={'status':'preparing','audio':'muted','background':True}

    def firewall_action(self,action: str) -> dict:
        # Argument-array invocation, no UAC/elevation or user-visible console.
        result=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-WindowStyle','Hidden',
            '-ExecutionPolicy','Bypass','-File',str(Path(__file__).with_name('game_test_firewall.ps1')),
            '-Action',action,'-Program',self.metadata['client']['game'],'-RunId',self.metadata['run_id'],
            '-RemoteIPv4',self.metadata['plan']['target_host']],capture_output=True,text=True,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=30)
        if result.returncode:raise GameTestError('cannot establish the owned silent firewall rule; client was not launched interactively')
        try:return json.loads(result.stdout)
        except json.JSONDecodeError as error:raise GameTestError('invalid quiet firewall readback') from error

    def start(self) -> None:
        validate_muted_audio(Path(self.metadata['client']['root']))
        # Scope stays on the declared server. Routed/non-IPv4 tests must supply
        # an explicit endpoint before a quiet run can open a native listener.
        try:ipaddress.IPv4Address(self.metadata['plan']['target_host'])
        except ipaddress.AddressValueError as error:raise GameTestError('quiet tests require an explicit IPv4 server endpoint') from error
        if (ipaddress.IPv4Address(self.metadata['plan']['target_host']).is_loopback and
            self.metadata['plan'].get('network_policy') in ('direct', ROUTED_LOOPBACK_POLICY)):
            # Local GameTest has no reason to listen on a public interface.
            # The native Mod constrains its own bind calls before game startup.
            self.firewall={'status':'loopback_only','system_policy_changed':False}
        else:
            self.firewall_attempted=True
            self.firewall=self.firewall_action('Install')
        self.receipt['firewall']=self.firewall
        self.save()

    def save(self):
        atomic_write(self.directory/'quiet-runtime.json',json.dumps(self.receipt,indent=2))

    def spawn(self,arguments: list[str],**kwargs):
        spec=importlib.util.spec_from_file_location('sdgo_game_test_windows',Path(__file__).with_name('game_test_windows.py'))
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        self.process=module.WindowsBackgroundProcess(arguments,cwd=kwargs['cwd'],env=kwargs['env'],
            stdout=kwargs['stdout'],stderr=kwargs['stderr'],run_id=self.metadata['run_id'],
            allow_named_desktop_fallback=self.metadata.get('suite')==FPS_SUITE)
        return self.process

    def verify(self,pid: int) -> dict | None:
        marker=Path(self.metadata['client']['root'])/'sdgo/gametest/quiet-sockets.ini'
        try:
            guard=read_ini(marker) if marker.is_file() else None
        except GameTestError as error:
            cause=error.__cause__
            # Native atomic replacement can briefly put the old file into
            # Windows delete-pending state. Retry only this I/O condition;
            # malformed content and policy failures remain hard failures.
            if not isinstance(cause,PermissionError) and getattr(cause,'winerror',None) not in (32,33):raise
            guard=None
            self.receipt['guard_read_retries']=self.receipt.get('guard_read_retries',0)+1
        matching=guard is not None and guard.get('run','id',fallback='')==self.metadata['run_id'] and guard.getint('policy','pid',fallback=0)==pid
        if matching:self.guard_proxy=guard.getint('graphics','proxy_hwnd',fallback=0)
        proxy=self.guard_proxy if guard is None else guard.getint('graphics','proxy_hwnd',fallback=0) if matching else 0
        proof=self.process.verify_background(pid,allowed_input_windows=(proxy,) if proxy else ())
        if self.receipt.get('desktop_probe')!=proof:
            self.receipt['desktop_probe']=proof
            self.save()
        if proof.get('error'):
            # A locked/secure desktop can deny observation. Keep the safety
            # stop, but do not turn missing evidence into a claim that the
            # owned game actually took the foreground.
            raise GameTestError(f"quiet desktop observation unavailable: {proof['error']}")
        if not proof.get('foreground_safe',False):raise GameTestError('owned game attempted to take the user foreground')
        if proof.get('verified') and proof.get('isolation_safe',True) is not True:
            raise GameTestError('quiet desktop isolation proof is internally inconsistent')
        if (proof.get('isolation_mode')=='private_desktop_with_hidden_default_proxy' and
            self.metadata.get('suite')!=FPS_SUITE):
            raise GameTestError('named-desktop fallback is restricted to the native FPS suite')
        if self.proof_file_written and (proof.get('input_desktop_unchanged') is False or
            proof.get('unexpected_input_window_count',0)>0 or
            (proof.get('input_desktop_safe') is False and proof.get('input_target_window_count',0)>0)):
            raise GameTestError('owned game window escaped the verified private-desktop policy')
        if guard is None and self.proof_file_written:
            now=time.monotonic()
            if self.guard_unavailable_since is None:self.guard_unavailable_since=now
            if now-self.guard_unavailable_since>2:
                raise GameTestError('native quiet runtime receipt stayed unreadable for more than two seconds')
        elif guard is not None:
            self.guard_unavailable_since=None
            if self.proof_file_written and not matching:
                raise GameTestError('native quiet runtime receipt changed its run or process identity')
        if not proof.get('verified',False):
            if self.proof_file_written:
                now=time.monotonic()
                if self.background_unavailable_since is None:self.background_unavailable_since=now
                if now-self.background_unavailable_since>2:
                    raise GameTestError('native background window context stayed unavailable for more than two seconds')
            return None
        self.background_unavailable_since=None
        if not matching:return None
        if (not proxy or guard.getint('graphics','active',fallback=0)!=1 or guard.getint('graphics','worker_ready',fallback=0)!=1 or
            guard.getint('graphics','created_devices',fallback=0)<1):return None
        if guard.getint('graphics','dispatch_failures',fallback=0):raise GameTestError('native background graphics dispatch failed')
        proof['graphics_backend']='native_d3d9_hal_hidden_proxy'
        proof['graphics_profile']='windowed_native_postfx_disabled'
        opened=guard.getint('audio','opened',fallback=0)
        if guard.getint('audio','active',fallback=0)!=1 or opened<1:return None
        if guard.getint('audio','muted',fallback=0)!=opened or guard.getint('audio','failed',fallback=0):
            raise GameTestError('native Miles audio drivers did not verify zero master volume')
        proof.update(audio_driver='native_miles_waveout',audio_drivers_opened=opened,audio_drivers_muted=opened)
        network_verified=self.firewall.get('status')=='installed'
        if self.firewall.get('status')=='loopback_only':
            if (guard.getint('policy','loopback_only',fallback=0)!=1 or
                guard.getint('policy','active',fallback=0)!=1):return None
            if guard.getint('sockets','escaped',fallback=0):raise GameTestError('native socket escaped the test loopback interface')
            endpoints=owned_udp_endpoints(pid)
            if any(not ipaddress.IPv4Address(row['local_ipv4']).is_loopback for row in endpoints):
                raise GameTestError('owned game has a non-loopback UDP listener')
            network_verified=True
            proof.update(network_policy='loopback_only',udp_endpoints=endpoints)
        proof={**proof,'audio_muted':True,'firewall_verified':network_verified,'pid':pid}
        if self.receipt.get('proof')!=proof:
            self.receipt.update(status='running',proof=proof);self.save()
        if not self.proof_file_written:
            parser=ini_parser();parser['run']={'id':self.metadata['run_id']}
            parser['quiet']={'pid':str(pid),'verified':'1','audio_muted':'1','firewall_verified':'1'}
            write_ini(Path(self.metadata['client']['root'])/'sdgo/gametest/quiet-verified.ini',parser)
            self.proof_file_written=True
        return proof

    def close(self) -> dict:
        errors=[]
        if self.process:
            try:self.process.close()
            except Exception as error:errors.append(type(error).__name__)
        if self.firewall_attempted:
            try:self.receipt['cleanup']=self.firewall_action('Remove')
            except Exception as error:errors.append(type(error).__name__)
        elif self.firewall:
            self.receipt['cleanup']={'status':'removed','system_policy_changed':False}
        self.receipt['status']='failed' if errors else 'closed'
        self.receipt['cleanup_errors']=errors;self.save();return self.receipt


def random_account() -> tuple[str, str]:
    return (
        "".join(secrets.choice(ALNUM) for _ in range(10)),
        "".join(secrets.choice(ALNUM) for _ in range(12)),
    )


def _route_port(field: str, value: str) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise GameTestError(f"{field} must be a decimal port")
    port = int(value)
    if not 1 <= port <= 65535:
        raise GameTestError(f"{field} must be between 1 and 65535")
    return port


def load_routed_loopback_profile(path: Path, target_host: str, login_port: int) -> dict:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise GameTestError(f"routed loopback profile does not exist: {path}")
    parser = read_ini(path)
    if parser.defaults() or parser.sections() != ["settings"]:
        raise GameTestError("routed loopback profile must contain only one [settings] section")
    settings = parser["settings"]
    try:
        count = int(settings["RouteCount"])
    except (KeyError, ValueError) as error:
        raise GameTestError("routed loopback profile requires RouteCount=1..32") from error
    if not re.fullmatch(r"[0-9]+", settings["RouteCount"]) or not 1 <= count <= 32:
        raise GameTestError("routed loopback profile requires RouteCount=1..32")
    expected_keys = {"LoginIPv4", "LoginPort", "RouteCount", "Trace"} | {
        f"Route{index}" for index in range(1, count + 1)
    }
    actual_keys = set(settings)
    if actual_keys != expected_keys:
        unknown = sorted(actual_keys - expected_keys)
        missing = sorted(expected_keys - actual_keys)
        details = []
        if unknown:
            details.append("unknown keys: " + ", ".join(unknown))
        if missing:
            details.append("missing keys: " + ", ".join(missing))
        raise GameTestError("routed loopback profile has an unsafe schema; " + "; ".join(details))
    if settings["Trace"] not in {"0", "1"}:
        raise GameTestError("routed loopback profile Trace must be 0 or 1")

    try:
        login_ip = str(ipaddress.IPv4Address(settings["LoginIPv4"]))
        requested_ip = str(ipaddress.IPv4Address(target_host))
    except ipaddress.AddressValueError as error:
        raise GameTestError("routed loopback login must use an IPv4 literal") from error
    configured_port = _route_port("LoginPort", settings["LoginPort"])
    if not ipaddress.IPv4Address(login_ip).is_loopback:
        raise GameTestError("routed loopback login must stay within IPv4 127/8")
    if login_ip != requested_ip or configured_port != login_port:
        raise GameTestError("routed loopback profile login disagrees with the requested logical endpoint")

    routes = []
    logicals: set[tuple[str, str, int]] = set()
    externals: set[tuple[str, str, int]] = set()
    for index in range(1, count + 1):
        field = f"Route{index}"
        cells = [cell.strip() for cell in settings[field].split(",")]
        if len(cells) != 5 or cells[0] not in {"tcp", "udp"}:
            raise GameTestError(
                f"{field} must be tcp|udp,logicalIPv4,port,externalIPv4,port"
            )
        protocol = cells[0]
        try:
            logical_ip = str(ipaddress.IPv4Address(cells[1]))
            external_ip = str(ipaddress.IPv4Address(cells[3]))
        except ipaddress.AddressValueError as error:
            raise GameTestError(f"{field} must use exact IPv4 literals; DNS and wildcards are forbidden") from error
        if not ipaddress.IPv4Address(logical_ip).is_loopback or not ipaddress.IPv4Address(
            external_ip
        ).is_loopback:
            raise GameTestError(f"{field} endpoints must stay within IPv4 127/8")
        logical_port = _route_port(f"{field} logical port", cells[2])
        external_port = _route_port(f"{field} external port", cells[4])
        logical = (protocol, logical_ip, logical_port)
        external = (protocol, external_ip, external_port)
        if logical in logicals:
            raise GameTestError(f"{field} duplicates a logical route")
        if external in externals:
            raise GameTestError(f"{field} creates an ambiguous reverse mapping")
        logicals.add(logical)
        externals.add(external)
        routes.append(
            {
                "protocol": protocol,
                "logical_ipv4": logical_ip,
                "logical_port": logical_port,
                "physical_ipv4": external_ip,
                "physical_port": external_port,
            }
        )

    login_routes = [
        route
        for route in routes
        if route["protocol"] == "tcp"
        and route["logical_ipv4"] == login_ip
        and route["logical_port"] == configured_port
    ]
    if len(login_routes) != 1:
        raise GameTestError(
            "routed loopback profile requires exactly one TCP route for the logical login endpoint"
        )
    physical = login_routes[0]
    return {
        "route_profile_path": ROUTE_PROFILE_RELATIVE.as_posix(),
        "route_profile_sha256": sha256(path),
        "route_profile_route_count": count,
        "logical_login_ipv4": login_ip,
        "logical_login_port": configured_port,
        "physical_login_ipv4": physical["physical_ipv4"],
        "physical_login_port": physical["physical_port"],
    }


def validate_routed_loopback_install(root: Path, plan: dict) -> dict:
    root = root.resolve()
    if plan.get("route_profile_path") != ROUTE_PROFILE_RELATIVE.as_posix():
        raise GameTestError("routed loopback profile path is not the fixed copied-client location")
    profile = (root / ROUTE_PROFILE_RELATIVE).resolve()
    if not is_relative_to(profile, root):
        raise GameTestError("routed loopback profile escapes the copied client")
    target_host = plan.get("target_host", plan.get("logical_login_ipv4"))
    login_port = plan.get("login_port", plan.get("logical_login_port"))
    if not isinstance(target_host, str) or isinstance(login_port, bool) or not isinstance(
        login_port, int
    ):
        raise GameTestError("routed loopback plan lacks its logical login endpoint")
    binding = load_routed_loopback_profile(profile, target_host, login_port)
    for field in ROUTE_PLAN_FIELDS:
        if plan.get(field) != binding[field]:
            raise GameTestError(f"routed loopback profile disagrees with the sealed plan for {field}")

    route_dir = profile.parent
    manifest = route_dir / "mod.ini"
    dll = route_dir / "net_route.dll"
    if not manifest.is_file() or not dll.is_file():
        raise GameTestError("copied client lacks the installed net_route Mod")
    mod = read_ini(manifest)
    if mod.get("mod", "id", fallback="") != "net_route" or mod.get(
        "mod", "enabled", fallback=""
    ) != "1":
        raise GameTestError("copied client net_route manifest is not explicitly enabled")
    global_settings = read_ini(root / "sdgo" / "sdgo.ini")
    required = (
        ("categories", "network", "1"),
        ("features", "frp-distribution", "1"),
        ("mods", "net_route", "1"),
        ("mods", "net_probe", "0"),
    )
    for section, key, expected in required:
        if global_settings.get(section, key, fallback="") != expected:
            raise GameTestError(f"copied client network gate {section}.{key} must equal {expected}")
    return binding


def configure_test_network(
    root: Path,
    policy: str,
    *,
    route_profile: Path | None = None,
    route_binding: dict | None = None,
) -> dict | None:
    if policy == 'preserve':
        if route_profile is not None or route_binding is not None:
            raise GameTestError("route_profile is valid only for routed_loopback")
        return None
    if policy == ROUTED_LOOPBACK_POLICY:
        if route_profile is None or route_binding is None:
            raise GameTestError("routed_loopback requires a validated route profile")
        root = root.resolve()
        route_dir = root / ROUTE_PROFILE_RELATIVE.parent
        if not route_dir.is_dir() or not is_relative_to(route_dir.resolve(), root):
            raise GameTestError("copied client net_route directory is missing or escapes the client")
        manifest = route_dir / "mod.ini"
        dll = route_dir / "net_route.dll"
        if not manifest.is_file() or not dll.is_file():
            raise GameTestError("routed_loopback requires net_route to be installed in the client template")
        config = read_ini(manifest)
        if config.get("mod", "id", fallback="") != "net_route":
            raise GameTestError("copied client has an invalid net_route manifest")
        shutil.copyfile(route_profile, root / ROUTE_PROFILE_RELATIVE)
        config["mod"]["enabled"] = "1"
        write_ini(manifest, config)
        path = root / "sdgo" / "sdgo.ini"
        settings = read_ini(path)
        for section in ("categories", "features", "mods"):
            if not settings.has_section(section):
                settings.add_section(section)
        settings["categories"]["network"] = "1"
        settings["features"]["frp-distribution"] = "1"
        settings["mods"]["net_route"] = "1"
        settings["mods"]["net_probe"] = "0"
        write_ini(path, settings)
        installed = validate_routed_loopback_install(root, route_binding)
        if installed != route_binding:
            raise GameTestError("copied routed loopback profile changed while it was being installed")
        return installed
    if policy != 'direct':raise GameTestError('unknown test network policy')
    if route_profile is not None or route_binding is not None:
        raise GameTestError("route_profile is valid only for routed_loopback")
    # Owner routing can override explicit command-line endpoints. Only the
    # copied translator is disabled; the source client is never changed.
    manifest = root/'sdgo/mods/net_route/mod.ini'
    if manifest.exists():
        config=read_ini(manifest);config['mod']['enabled']='0';write_ini(manifest,config)
    path=root/'sdgo/sdgo.ini';settings=read_ini(path)
    if not settings.has_section('mods'):settings.add_section('mods')
    settings['mods']['net_route']='0';write_ini(path,settings)
    return None


def _shop_key(field: str, value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
        raise GameTestError(f"{field} must be a 1..64 character stable identifier")
    return value


def _shop_integer(field: str, value: str, minimum: int, maximum: int) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise GameTestError(f"{field} must be canonical decimal")
    parsed = int(value)
    if not minimum <= parsed <= maximum or str(parsed) != value:
        raise GameTestError(f"{field} is outside its supported range")
    return parsed


def _exact_ini_keys(
    parser: configparser.ConfigParser,
    section: str,
    expected: set[str],
    label: str,
) -> configparser.SectionProxy:
    if not parser.has_section(section):
        raise GameTestError(f"{label} lacks [{section}]")
    actual = set(parser[section])
    if actual != expected:
        raise GameTestError(f"{label} has missing or unknown keys in [{section}]")
    return parser[section]


def load_shop_test_inputs(assertions_path: Path, display_path: Path) -> dict:
    assertions_path = assertions_path.expanduser().resolve()
    display_path = display_path.expanduser().resolve()
    if not assertions_path.is_file() or not display_path.is_file():
        raise GameTestError("shop assertions and runtime display must both be readable files")
    assertions = read_ini(assertions_path)
    display = read_ini(display_path)
    if assertions.defaults() or display.defaults():
        raise GameTestError("shop GameTest inputs cannot use INI defaults")

    shop = _exact_ini_keys(
        assertions,
        "shop",
        {"Schema", "Scope", "CatalogId", "Revision"},
        "shop assertions",
    )
    if shop["Schema"] != "1" or shop["Scope"] != "StockAssertions":
        raise GameTestError("shop assertions require [shop] Schema=1 and Scope=StockAssertions")
    _shop_key("shop CatalogId", shop["CatalogId"])
    revision = _shop_key("shop Revision", shop["Revision"])

    product_keys = {
        "ProductId", "ItemId", "TemplateId", "Category", "Price",
        "Currency", "Validity", "Enabled", "Name", "Label",
    }
    products = 0
    product_ids: set[int] = set()
    item_ids: set[int] = set()
    for section in assertions.sections():
        if section == "shop":
            continue
        if not section.startswith("product:"):
            raise GameTestError("shop assertions contain an unsupported section")
        _shop_key("shop product key", section[len("product:"):])
        product = _exact_ini_keys(assertions, section, product_keys, "shop assertions")
        product_id = _shop_integer(f"{section}.ProductId", product["ProductId"], 1, 2**31 - 1)
        template_id = _shop_integer(f"{section}.TemplateId", product["TemplateId"], 1, 2**31 - 1)
        if product_id != template_id:
            raise GameTestError("StockAssertions products must reference native stock IDs, not owner SKUs")
        item = product["ItemId"]
        if not re.fullmatch(r"[0-9]{6}", item):
            raise GameTestError(f"{section}.ItemId must be six decimal digits")
        item_id = int(item)
        _shop_integer(f"{section}.Category", product["Category"], 1, 2**31 - 1)
        _shop_integer(f"{section}.Price", product["Price"], 1, 2**31 - 1)
        _shop_integer(f"{section}.Validity", product["Validity"], 1, 1_000_000)
        if product["Currency"] not in {"GB", "MB"}:
            raise GameTestError(f"{section}.Currency must be GB or MB")
        if product["Enabled"] not in {"0", "1"}:
            raise GameTestError(f"{section}.Enabled must be 0 or 1")
        for field in ("Name", "Label"):
            value = product[field]
            if not value or len(value) > 50 or any(character in "\r\n\x00" for character in value):
                raise GameTestError(f"{section}.{field} is not bounded display text")
        if product_id in product_ids or item_id in item_ids:
            raise GameTestError("shop assertions contain duplicate product or item IDs")
        product_ids.add(product_id)
        item_ids.add(item_id)
        if product["Enabled"] == "1":
            products += 1
    if products == 0:
        raise GameTestError("shop assertions require at least one enabled stock product")

    display_shop = _exact_ini_keys(
        display, "shop", {"Revision"}, "shop runtime display"
    )
    display_revision = _shop_key("shop runtime Revision", display_shop["Revision"])
    if display_revision != revision:
        raise GameTestError("shop assertions and runtime display revisions do not match")
    option_keys = {"Category", "Validity", "Payment", "Price", "Text"}
    options = 0
    for section in display.sections():
        if section == "shop":
            continue
        if not section.startswith("option:"):
            raise GameTestError("shop runtime display contains an unsupported section")
        _shop_integer(
            "shop runtime option id",
            section[len("option:"):],
            1,
            2**31 - 1,
        )
        option = _exact_ini_keys(display, section, option_keys, "shop runtime display")
        _shop_integer(f"{section}.Category", option["Category"], 1, 2**31 - 1)
        _shop_integer(f"{section}.Validity", option["Validity"], 1, 1_000_000)
        _shop_integer(f"{section}.Payment", option["Payment"], 1, 2)
        _shop_integer(f"{section}.Price", option["Price"], 1, 2**31 - 1)
        text = option["Text"]
        if not text or len(text) > 50 or any(character in "\r\n\x00" for character in text):
            raise GameTestError(f"{section}.Text is not bounded display text")
        options += 1
    if options == 0:
        raise GameTestError("shop runtime display requires at least one expected option")

    return {
        "shop_assertions_path": SHOP_ASSERTIONS_JOB_PATH,
        "shop_assertions_sha256": sha256(assertions_path),
        "shop_display_path": SHOP_DISPLAY_JOB_PATH,
        "shop_display_sha256": sha256(display_path),
        "shop_revision": revision,
    }


def install_shop_test_inputs(
    root: Path,
    assertions_source: Path,
    display_source: Path,
    expected: dict,
) -> dict:
    root = root.resolve()
    assertions_target = (root / SHOP_ASSERTIONS_RELATIVE).resolve()
    display_target = (root / SHOP_DISPLAY_RELATIVE).resolve()
    if not is_relative_to(assertions_target, root) or not is_relative_to(display_target, root):
        raise GameTestError("shop GameTest input path escapes the copied client")
    assertions_target.parent.mkdir(parents=True, exist_ok=True)
    if assertions_target.exists() or display_target.exists():
        raise GameTestError("fresh copied client already contains shop GameTest inputs")
    shutil.copyfile(assertions_source, assertions_target)
    shutil.copyfile(display_source, display_target)
    installed = load_shop_test_inputs(assertions_target, display_target)
    if installed != expected:
        raise GameTestError("shop GameTest inputs changed while copied into the isolated client")
    return installed


def validate_shop_test_install(root: Path, plan: dict) -> dict:
    if {field for field in SHOP_PLAN_FIELDS if field in plan} != set(SHOP_PLAN_FIELDS):
        raise GameTestError("shop GameTest plan must contain all sealed input fields")
    if (
        plan["shop_assertions_path"] != SHOP_ASSERTIONS_JOB_PATH
        or plan["shop_display_path"] != SHOP_DISPLAY_JOB_PATH
    ):
        raise GameTestError("shop GameTest inputs do not use their fixed copied-client paths")
    root = Path(root).resolve()
    inputs = []
    for relative in (SHOP_ASSERTIONS_RELATIVE, SHOP_DISPLAY_RELATIVE):
        lexical = root / relative
        resolved = lexical.resolve()
        if (
            lexical.is_symlink()
            or (lexical.exists() and lexical.stat().st_nlink != 1)
            or not is_relative_to(resolved, root)
            or any(
                parent.is_symlink()
                or (hasattr(parent, "is_junction") and parent.is_junction())
                for parent in lexical.parents
                if parent != root and is_relative_to(parent, root)
            )
        ):
            raise GameTestError("shop GameTest input is linked or escapes the copied client")
        inputs.append(resolved)
    installed = load_shop_test_inputs(
        inputs[0],
        inputs[1],
    )
    for field in SHOP_PLAN_FIELDS:
        if type(plan[field]) is not type(installed[field]) or plan[field] != installed[field]:
            raise GameTestError(f"shop GameTest input disagrees with the sealed plan for {field}")
    return installed


def owned_tcp_peers(pid: int) -> list[dict]:
    if os.name != 'nt':return []
    api=ctypes.WinDLL('iphlpapi',use_last_error=True)
    get=api.GetExtendedTcpTable
    get.argtypes=[ctypes.c_void_p,ctypes.POINTER(wintypes.DWORD),wintypes.BOOL,wintypes.ULONG,ctypes.c_int,wintypes.ULONG]
    get.restype=wintypes.DWORD
    size=wintypes.DWORD(0)
    result=get(None,ctypes.byref(size),False,2,5,0)
    if result not in (0,122) or not 4<=size.value<=4*1024*1024:return []
    data=ctypes.create_string_buffer(size.value)
    if get(data,ctypes.byref(size),False,2,5,0):return []
    count=struct.unpack_from('<I',data.raw)[0]
    if count>(size.value-4)//24:return []
    peers=[]
    for i in range(count):
        state,local,local_port,remote,remote_port,owner=struct.unpack_from('<6I',data.raw,4+i*24)
        if owner==pid and state==5:
            peers.append({'local_ipv4':socket.inet_ntoa(struct.pack('<I',local)),
                'local_port':socket.ntohs(local_port&65535),'remote_ipv4':socket.inet_ntoa(struct.pack('<I',remote)),
                'remote_port':socket.ntohs(remote_port&65535)})
    return peers


def verify_login_peer(metadata: dict, pid: int) -> dict | None:
    plan = metadata["plan"]
    policy = plan.get("network_policy")
    if policy == "direct":
        physical_ip = plan["target_host"]
        physical_port = plan["login_port"]
    elif policy == ROUTED_LOOPBACK_POLICY:
        physical_ip = plan["physical_login_ipv4"]
        physical_port = plan["physical_login_port"]
    else:
        return None
    peers=owned_tcp_peers(pid)
    match=next((p for p in peers if p['remote_ipv4']==physical_ip and p['remote_port']==physical_port),None)
    if match is None:return None
    proof={
        'verified':True,
        'pid':pid,
        'network_policy':policy,
        'logical_ipv4':plan['target_host'],
        'logical_port':plan['login_port'],
        'physical_ipv4':physical_ip,
        'physical_port':physical_port,
        # Retain the established receipt names for direct-run consumers.
        'remote_ipv4':physical_ip,
        'remote_port':physical_port,
        'observed':peers,
    }
    if policy == ROUTED_LOOPBACK_POLICY:
        proof['route_profile_sha256'] = plan['route_profile_sha256']
    receipt=ini_parser();receipt['run']={'id':metadata['run_id']}
    receipt['network']={
        'verified':'1',
        'pid':str(pid),
        'policy':policy,
        'ipv4':physical_ip,
        'port':str(physical_port),
        'logical_ipv4':plan['target_host'],
        'logical_port':str(plan['login_port']),
        'physical_ipv4':physical_ip,
        'physical_port':str(physical_port),
    }
    if policy == ROUTED_LOOPBACK_POLICY:
        receipt['network']['route_profile_sha256'] = plan['route_profile_sha256']
    write_ini(Path(metadata['client']['root'])/'sdgo/gametest/network-verified.ini',receipt)
    return proof


def verify_direct_peer(metadata: dict, pid: int) -> dict | None:
    return verify_login_peer(metadata, pid)


def owned_udp_endpoints(pid: int) -> list[dict]:
    if os.name!='nt':raise GameTestError('quiet socket readback requires Windows')
    api=ctypes.WinDLL('iphlpapi',use_last_error=True)
    get=api.GetExtendedUdpTable
    get.argtypes=[ctypes.c_void_p,ctypes.POINTER(wintypes.DWORD),wintypes.BOOL,wintypes.ULONG,ctypes.c_int,wintypes.ULONG]
    get.restype=wintypes.DWORD
    size=wintypes.DWORD(0)
    result=get(None,ctypes.byref(size),False,2,1,0)
    if result not in (0,122) or not 4<=size.value<=4*1024*1024:
        raise GameTestError('cannot size the Windows process-owned UDP table')
    data=ctypes.create_string_buffer(size.value)
    if get(data,ctypes.byref(size),False,2,1,0):raise GameTestError('cannot read the Windows process-owned UDP table')
    count=struct.unpack_from('<I',data.raw)[0]
    if count>(size.value-4)//12:raise GameTestError('malformed Windows process-owned UDP table')
    rows=[]
    for i in range(count):
        local,port,owner=struct.unpack_from('<3I',data.raw,4+i*12)
        if owner==pid:rows.append({'local_ipv4':socket.inet_ntoa(struct.pack('<I',local)),
                                 'local_port':socket.ntohs(port&65535)})
    return rows


def default_account_profile() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise GameTestError(
            "LOCALAPPDATA is unavailable; pass --account-profile with the persistent test account"
        )
    return Path(local_app_data) / "SDGO-GameTest" / "account.ini"


def load_account_profile(path: Path) -> configparser.ConfigParser:
    if not path.is_file():
        raise GameTestError(
            f"persistent test account profile is missing: {path}; "
            "promote the already-registered account or explicitly use --create-test-account once"
        )
    parser = read_ini(path)
    try:
        username = parser["account"]["username"]
        password = parser["account"]["password"]
    except KeyError as error:
        raise GameTestError(f"persistent test account profile is invalid: {path}") from error
    validate_account_credentials(parser, username, password)
    return parser


def validate_account_credentials(parser, username: str, password: str) -> None:
    existing = parser.get('registration', 'state', fallback='') == 'existing_account'
    password_format = parser.get('account', 'password_format', fallback='plain')
    if existing:
        if not re.fullmatch(r'[A-Za-z0-9_]{1,14}', username):
            raise GameTestError('existing account username exceeds the native ASCII account field')
        if password_format == 'md5':
            if not re.fullmatch(r'[A-Fa-f0-9]{32}', password):
                raise GameTestError('existing account MD5 credential must contain exactly 32 hex characters')
        elif password_format == 'plain':
            if not password or len(password) > 64 or not password.isascii() or any(c in password for c in '\r\n\0'):
                raise GameTestError('existing account password does not fit the native account input')
        else:
            raise GameTestError('unknown existing account password format')
    else:
        if password_format != 'plain':
            raise GameTestError('imported MD5 credentials require an explicitly existing account profile')
        if not re.fullmatch(r'[A-Za-z0-9]{10}', username):
            raise GameTestError('persistent test account username must be 10 ASCII alphanumeric characters')
        if not re.fullmatch(r'[A-Za-z0-9]{12}', password):
            raise GameTestError('persistent test account password must be 12 ASCII alphanumeric characters')


def create_account_profile(path: Path) -> configparser.ConfigParser:
    if path.exists():
        raise GameTestError(f"refusing to overwrite persistent test account profile: {path}")
    username, password = random_account()
    parser = ini_parser()
    parser["account"] = {"username": username, "password": password}
    parser["registration"] = {"state": "pending_first_login"}
    path.parent.mkdir(parents=True, exist_ok=True)
    write_ini(path, parser)
    os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    return parser


def validate_public_value(label: str, value: str) -> str:
    if not value or len(value) > 128 or any(character in "\r\n\x00" for character in value):
        raise GameTestError(f"invalid {label}")
    return value


def parse_mode_sequence(value: str) -> tuple[int, ...]:
    value = validate_public_value("mode_sequence", value)
    if not re.fullmatch(r"[13679](?:,[13679])*", value):
        supported = ",".join(str(mode) for mode in SUPPORTED_PVP_MODES)
        raise GameTestError(
            "mode_sequence must contain comma-separated single-digit PvP modes "
            f"without spaces or leading zeros; supported modes: {supported}"
        )
    modes = tuple(int(item) for item in value.split(","))
    if len(set(modes)) != len(modes):
        raise GameTestError("mode_sequence must not contain duplicate modes")
    return modes


def validate_play_unit_uid(value: int | None) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or not 0 < value < 2**64:
        raise GameTestError("play_unit_uid must be a non-zero uint64 owned unit UID")
    return value


def validate_mission_mode(value: int | None) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value not in SUPPORTED_MISSION_MODES:
        supported = ",".join(str(mode) for mode in SUPPORTED_MISSION_MODES)
        raise GameTestError(f"mission_mode must be one of: {supported}")
    return value


def mission_room_title(title_nonce: str) -> str:
    return f"{title_nonce} !ai{MISSION_AI_COUNT} lv{MISSION_AI_LEVEL}"


def required_flow_cases(mode_sequence: str, bootstrap_only: bool = False,
                        battleship_only: bool = False, gift_requests_only: bool = False,
                        deck_only: bool = False, pages_only: bool = False,
                        collection_draw: bool = False,
                         play_unit_uid: int | None = None,
                         mission_mode: int | None = None,
                         shop_catalog: bool = False,
                        friends_only: bool = False) -> tuple[str, ...]:
    mission_mode = validate_mission_mode(mission_mode)
    play_unit_uid = validate_play_unit_uid(play_unit_uid)
    if sum((bool(bootstrap_only),bool(battleship_only),bool(gift_requests_only),bool(deck_only),bool(pages_only),bool(friends_only))) > 1:
        raise GameTestError('choose only one account preparation operation')
    if friends_only:
        if collection_draw or play_unit_uid is not None or mission_mode is not None or shop_catalog:
            raise GameTestError('friend sessions cannot add battle, mission or shop scope')
        return FLOW_CASES[:3]+('friends.session',)
    if play_unit_uid is not None and any((bootstrap_only,battleship_only,gift_requests_only,deck_only)):
        raise GameTestError('--play-unit-uid is supported only for PvP and pages scenarios, or mission smoke')
    if collection_draw and not pages_only:
        raise GameTestError('collection draw requires the explicit pages-only scenario')
    if shop_catalog and not pages_only:
        raise GameTestError('shop catalog assertions require the explicit pages-only scenario')
    selection = ('setup.active_unit',) if play_unit_uid is not None else ()
    if mission_mode is not None:
        if mode_sequence != "1":
            raise GameTestError("--mission-mode cannot be combined with a PvP mode sequence")
        if any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only,collection_draw)):
            raise GameTestError("--mission-mode cannot be combined with pages or account preparation scopes")
        return FLOW_CASES[:3] + selection + MISSION_FLOW_CASES
    modes = parse_mode_sequence(mode_sequence)
    if pages_only:
        shop_cases = ('pages.shop.catalog',) if shop_catalog else ()
        return FLOW_CASES[:3] + selection + ('pages.hangar.read','pages.hangar.preview','pages.hangar.cancel','catalog.operator','catalog.ai') + shop_cases + ('pages.storage.read','pages.storage.claim','catalog.capsule','pages.capsule.currency') + (
            ('pages.collection.preview','pages.collection.draw','pages.collection.delivery') if collection_draw else ())
    if bootstrap_only:
        return FLOW_CASES[:3]
    if gift_requests_only:
        return FLOW_CASES[:3] + ('setup.gift_requests',)
    if deck_only:
        return FLOW_CASES[:3] + ('flow.battleship','setup.deck')
    prefix = FLOW_CASES[:3] + selection
    if battleship_only or any(mode in (6,7) for mode in modes):
        prefix += ('flow.battleship',)
    if battleship_only:
        return prefix
    if len(modes) == 1:
        return prefix + FLOW_CASES[3:]
    return prefix + tuple(
        f"pvp.mode{mode}.{step}" for mode in modes for step in MODE_FLOW_STEPS
    )


def required_cases_for_plan(plan: dict) -> tuple[str, ...]:
    shop_fields = {field for field in SHOP_PLAN_FIELDS if field in plan}
    if shop_fields and shop_fields != set(SHOP_PLAN_FIELDS):
        raise GameTestError("shop GameTest plan is missing sealed input fields")
    shop_catalog = bool(shop_fields)
    if "mission_mode" in plan:
        forbidden = {
            "mode_sequence", "expected_ai", "bootstrap_only",
            "battleship_only", "gift_requests_only", "deck_only",
            "pages_only", "friends_only", "collection_draw", "random_loadout",
            *SHOP_PLAN_FIELDS,
        }
        if forbidden.intersection(plan):
            raise GameTestError("mission smoke plan cannot contain PvP, pages, or preparation scope")
        required = {
            "mission_mode", "mission_scope", "mission_ai_count",
            "mission_ai_level", *MISSION_CLAIM_FIELDS,
        }
        if not required.issubset(plan):
            raise GameTestError("mission smoke plan is missing sealed scope fields")
        if (
            plan["mission_scope"] != MISSION_SCOPE
            or type(plan["mission_ai_count"]) is not int
            or plan["mission_ai_count"] != MISSION_AI_COUNT
            or type(plan["mission_ai_level"]) is not int
            or plan["mission_ai_level"] != MISSION_AI_LEVEL
            or any(type(plan[field]) is not bool or plan[field] for field in MISSION_CLAIM_FIELDS)
        ):
            raise GameTestError("mission smoke plan changes its bounded native scope")
    return required_flow_cases(
        plan.get("mode_sequence", "1"),
        plan.get("bootstrap_only", False),
        plan.get("battleship_only", False),
        plan.get("gift_requests_only", False),
        plan.get("deck_only", False),
        plan.get("pages_only", False),
        plan.get("collection_draw", False),
        plan.get("play_unit_uid"),
        plan.get("mission_mode"),
        shop_catalog,
        friends_only=plan.get("friends_only",False),
    )


def metadata_suite_for_plan(plan: dict) -> str:
    if plan.get('friends_only'):
        return 'native-friends'
    if "mission_mode" in plan:
        return MISSION_SUITE
    if plan.get("bootstrap_only"):
        return "account-bootstrap"
    if plan.get("battleship_only"):
        return "battleship-acquisition"
    if plan.get("gift_requests_only"):
        return "gift-mail-requests"
    if plan.get("deck_only"):
        return "battleship-deck"
    if plan.get("pages_only"):
        return "native-pages"
    return "pvp-ai-normal-flow"


def account_profile_lock_path(path: Path) -> Path:
    return path.with_name(path.name + ".run.lock")


def make_plan(
    target_host: str,
    login_port: int,
    target_server: str,
    target_channel: str,
    mode_sequence: str,
    retry_count: int,
    expect_first_fail: bool,
    expected_ai: int,
    launcher_path: str | None,
    play_unit_uid: int | None = None,
    mission_mode: int | None = None,
) -> dict:
    if not 1 <= login_port <= 65535:
        raise GameTestError("login_port must be between 1 and 65535")
    if retry_count < 1 or retry_count > 2:
        raise GameTestError("retry_count must be 1 or 2")
    if expect_first_fail and retry_count != 2:
        raise GameTestError("expect_first_fail requires retry_count=2")
    mission_mode = validate_mission_mode(mission_mode)
    if mission_mode is not None:
        if mode_sequence != "1":
            raise GameTestError("--mission-mode cannot be combined with a PvP mode sequence")
        if expected_ai != 3:
            raise GameTestError("--expected-ai is a PvP setting and cannot change a mission smoke plan")
        modes = ()
    else:
        if expected_ai < 1 or expected_ai > 7:
            raise GameTestError("expected_ai must be between 1 and 7")
        modes = parse_mode_sequence(mode_sequence)
    title_nonce = "gt" + secrets.token_hex(2)
    plan = {
        "target_host": validate_public_value("target_host", target_host),
        "login_port": login_port,
        "target_server": validate_public_value("target_server", target_server),
        "target_channel": validate_public_value("target_channel", target_channel),
        "server_channel_selection": "native_automatic",
        "retry_count": retry_count,
        "expect_first_fail": bool(expect_first_fail),
        "title_nonce": title_nonce,
        "auto_nickname": "account_name",
        "exit_tutorial": True,
        "first_unit_draw": True,
        "acknowledge_game_windows": True,
        "launcher_path": launcher_path or "",
    }
    if mission_mode is not None:
        plan.update({
            "mission_mode": mission_mode,
            "mission_scope": MISSION_SCOPE,
            "mission_ai_count": MISSION_AI_COUNT,
            "mission_ai_level": MISSION_AI_LEVEL,
            "mission_clear_claim": False,
            "mission_result_claim": False,
            "mission_active_exit_claim": False,
            "room_title": mission_room_title(title_nonce),
        })
    else:
        enemies = (expected_ai + 1) // 2
        allies = expected_ai - enemies
        plan.update({
            "mode_sequence": ",".join(str(mode) for mode in modes),
            "expected_ai": expected_ai,
            "room_title": f"{title_nonce} !ai e{enemies} a{allies}",
        })
    play_unit_uid = validate_play_unit_uid(play_unit_uid)
    if play_unit_uid is not None:
        plan["play_unit_uid"] = play_unit_uid
    if launcher_path:
        relative = Path(launcher_path)
        if relative.is_absolute() or ".." in relative.parts:
            raise GameTestError("launcher_path must be a safe path relative to the copied client")
    return plan


def make_fps_plan(
    target_host: str,
    login_port: int,
    fps_cap: int,
    fps_mod_enabled: bool,
) -> dict:
    if type(fps_cap) is not int or not 1 <= fps_cap <= 1000:
        raise GameTestError("fps_cap must be between 1 and 1000")
    if type(fps_mod_enabled) is not bool:
        raise GameTestError("fps_mod_enabled must be boolean")
    if not 1 <= login_port <= 65535:
        raise GameTestError("login_port must be between 1 and 65535")
    try:
        host = str(ipaddress.IPv4Address(target_host))
    except ipaddress.AddressValueError as error:
        raise GameTestError("native FPS tests require an explicit loopback IPv4 endpoint") from error
    if not ipaddress.IPv4Address(host).is_loopback:
        raise GameTestError("native FPS tests require a loopback endpoint and never contact a game server")
    return {
        "target_host": host,
        "login_port": login_port,
        "network_policy": "direct",
        "quiet": True,
        "launcher_path": "",
        "fps_cap": fps_cap,
        "fps_mod_enabled": fps_mod_enabled,
    }


def required_plan_cases(plan: dict) -> tuple[str, ...]:
    if "fps_cap" in plan:
        return FPS_CASES
    return required_cases_for_plan(plan)


def write_job(
    root: Path,
    run_id: str,
    plan: dict,
    account_profile: configparser.ConfigParser | None,
    profile_path: Path | None,
) -> None:
    directory = root / "sdgo" / "gametest"
    directory.mkdir(parents=True, exist_ok=True)
    if "fps_cap" in plan:
        run = ini_parser()
        run["run"] = {
            "id": run_id,
            "role": "client",
            "suite": FPS_SUITE,
            "normal_flow": "0",
            "endpoint": f"{plan['target_host']}:{plan['login_port']}",
            "network_policy": "direct",
            "quiet": "1",
            "quiet_loopback": "1",
            "process_entry": GAME_EXE,
            "fps_cap": str(plan["fps_cap"]),
            "fps_mod_enabled": "1" if plan["fps_mod_enabled"] else "0",
            "required_cases": ",".join(FPS_CASES),
        }
        write_ini(directory / "run.ini", run)
        return
    if account_profile is None or profile_path is None:
        raise GameTestError("normal-flow GameTest requires a private account profile")
    private = directory / "private"
    private.mkdir()

    account = ini_parser()
    for section in account_profile.sections():
        account.add_section(section)
        for key, value in account_profile.items(section, raw=True):
            account.set(section, key, value)
    if not account.has_section("registration"):
        account.add_section("registration")
    account["registration"] = {
        **dict(account["registration"]),
        "retry_count": str(plan["retry_count"]),
        "expect_first_fail": "1" if plan["expect_first_fail"] else "0",
    }
    account["runner"] = {
        "lock_path": str(account_profile_lock_path(profile_path)),
    }
    account_path = private / "account.ini"
    write_ini(account_path, account)
    os.chmod(account_path, stat.S_IREAD | stat.S_IWRITE)

    run = ini_parser()
    run["run"] = {
        "id": run_id,
        "role": "client",
        "suite": "pvp-ai",
        "normal_flow": "1",
        "endpoint": f"{plan['target_host']}:{plan['login_port']}",
        "target_server": plan["target_server"],
        "target_channel": plan["target_channel"],
        "server_channel_selection": plan["server_channel_selection"],
        "title_nonce": plan["title_nonce"],
        "room_title": plan["room_title"],
        "auto_nickname": plan["auto_nickname"],
        "exit_tutorial": "1" if plan["exit_tutorial"] else "0",
        "first_unit_draw": "1" if plan["first_unit_draw"] else "0",
        "acknowledge_game_windows": "1" if plan["acknowledge_game_windows"] else "0",
        "process_entry": plan["launcher_path"] or GAME_EXE,
        "account_path": "private/account.ini",
        "retry_count": str(plan["retry_count"]),
        "expect_first_fail": "1" if plan["expect_first_fail"] else "0",
        "required_cases": ",".join(required_cases_for_plan(plan)),
    }
    if "mission_mode" in plan:
        run["run"].update({
            "mission_mode": str(plan["mission_mode"]),
            "mission_scope": plan["mission_scope"],
            "mission_ai_count": str(plan["mission_ai_count"]),
            "mission_ai_level": str(plan["mission_ai_level"]),
            "mission_clear_claim": "0",
            "mission_result_claim": "0",
            "mission_active_exit_claim": "0",
        })
    else:
        run["run"]["mode_sequence"] = plan["mode_sequence"]
        run["run"]["expected_ai"] = str(plan["expected_ai"])
    if 'play_unit_uid' in plan:
        run['run']['play_unit_uid'] = str(plan['play_unit_uid'])
    if all(field in plan for field in SHOP_PLAN_FIELDS):
        for field in SHOP_PLAN_FIELDS:
            run['run'][field] = str(plan[field])
    for flag in ('bootstrap_only','battleship_only','gift_requests_only','deck_only','pages_only','friends_only','collection_draw','random_loadout'):
        if plan.get(flag):run['run'][flag] = '1'
    if 'network_policy' in plan:run['run']['network_policy']=plan['network_policy']
    if plan.get('network_policy') == ROUTED_LOOPBACK_POLICY:
        for field in ROUTE_PLAN_FIELDS:
            run['run'][field] = str(plan[field])
    if 'quiet' in plan:run['run']['quiet']='1' if plan['quiet'] else '0'
    if plan.get('quiet'):
        run['run']['quiet_loopback']='1' if (
            plan.get('network_policy') == ROUTED_LOOPBACK_POLICY or
            (plan.get('network_policy')=='direct' and
             ipaddress.IPv4Address(plan['target_host']).is_loopback)
        ) else '0'
    write_ini(directory / "run.ini", run)


def read_public_plan(root: Path) -> dict:
    parser = read_ini(root / "sdgo" / "gametest" / "run.ini")
    try:
        run = parser["run"]
        if run.get("suite") == FPS_SUITE:
            if (
                run["role"] != "client"
                or run["normal_flow"] != "0"
                or run["process_entry"] != GAME_EXE
                or run["network_policy"] != "direct"
                or run["quiet"] != "1"
                or run["quiet_loopback"] != "1"
                or run["required_cases"] != ",".join(FPS_CASES)
                or "account_path" in run
            ):
                raise ValueError("run contract does not describe the native FPS suite")
            host, port = run["endpoint"].rsplit(":", 1)
            plan = make_fps_plan(
                host,
                int(port),
                int(run["fps_cap"]),
                {"0": False, "1": True}[run["fps_mod_enabled"]],
            )
            return plan
        suite = run["suite"]
        if suite != "pvp-ai":
            raise ValueError("unknown native provider suite")
        mission_plan = "mission_mode" in run

        bootstrap_only = run.get('bootstrap_only', '0') == '1'
        battleship_only = run.get('battleship_only', '0') == '1'
        gift_requests_only = run.get('gift_requests_only', '0') == '1'
        deck_only = run.get('deck_only', '0') == '1'
        pages_only = run.get('pages_only', '0') == '1'
        friends_only = run.get('friends_only', '0') == '1'
        collection_draw = run.get('collection_draw', '0') == '1'
        preparation_fields = (
            'bootstrap_only', 'battleship_only', 'gift_requests_only',
            'deck_only', 'pages_only', 'friends_only', 'collection_draw', 'random_loadout',
        )
        if any(run.get(flag, '0') not in ('0', '1') for flag in preparation_fields):
            raise ValueError('invalid preparation flag')

        play_unit_uid = None
        if 'play_unit_uid' in run:
            raw_play_unit_uid = run['play_unit_uid']
            if not re.fullmatch(r'[1-9][0-9]*', raw_play_unit_uid):
                raise ValueError('play_unit_uid must be canonical non-zero decimal')
            play_unit_uid = int(raw_play_unit_uid)
            if play_unit_uid >= 2**64:
                raise ValueError('play_unit_uid exceeds uint64')

        unknown_shop_fields = {
            field for field in run if field.startswith("shop_")
        } - set(SHOP_PLAN_FIELDS)
        if unknown_shop_fields:
            raise ValueError('unknown shop GameTest fields are forbidden')
        shop_fields_present = {field for field in SHOP_PLAN_FIELDS if field in run}
        shop_plan = None
        if shop_fields_present:
            if shop_fields_present != set(SHOP_PLAN_FIELDS):
                raise ValueError('shop GameTest inputs must provide every sealed field')
            if not pages_only:
                raise ValueError('shop GameTest inputs require pages_only')
            if (
                run['shop_assertions_path'] != SHOP_ASSERTIONS_JOB_PATH
                or run['shop_display_path'] != SHOP_DISPLAY_JOB_PATH
            ):
                raise ValueError('shop GameTest inputs must use fixed copied-client paths')
            if not re.fullmatch(r'[0-9a-f]{64}', run['shop_assertions_sha256']):
                raise ValueError('invalid shop assertions hash')
            if not re.fullmatch(r'[0-9a-f]{64}', run['shop_display_sha256']):
                raise ValueError('invalid shop display hash')
            if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', run['shop_revision']):
                raise ValueError('invalid shop revision')
            shop_plan = {field: run[field] for field in SHOP_PLAN_FIELDS}

        mission_mode = None
        if mission_plan:
            required_mission_fields = {
                'mission_mode', 'mission_scope', 'mission_ai_count',
                'mission_ai_level', *MISSION_CLAIM_FIELDS,
                'network_policy', 'quiet',
            }
            if not required_mission_fields.issubset(run.keys()):
                raise ValueError('mission smoke plan is missing sealed scope fields')
            if any(field in run for field in ('mode_sequence', 'expected_ai', *preparation_fields)):
                raise ValueError('mission smoke plan contains PvP, pages, or preparation scope')
            if run['mission_mode'] not in tuple(str(mode) for mode in SUPPORTED_MISSION_MODES):
                raise ValueError('invalid mission mode')
            mission_mode = validate_mission_mode(int(run['mission_mode']))
            if run['mission_scope'] != MISSION_SCOPE:
                raise ValueError('invalid mission smoke scope')
            if (
                run['mission_ai_count'] != str(MISSION_AI_COUNT)
                or run['mission_ai_level'] != str(MISSION_AI_LEVEL)
            ):
                raise ValueError('mission AI count or level differs from the sealed native plan')
            if any(run[field] != '0' for field in MISSION_CLAIM_FIELDS):
                raise ValueError('mission smoke cannot claim clear, result, or active exit')
            if run['retry_count'] != '1' or run['expect_first_fail'] != '0':
                raise ValueError('mission smoke requires one existing-account login attempt')
            canonical_run_id = str(uuid.UUID(run['id']))
            if run['id'] != canonical_run_id:
                raise ValueError('mission run id must use canonical lowercase UUID form')
            if not re.fullmatch(r'gt[0-9a-f]{4}', run['title_nonce']):
                raise ValueError('invalid mission title nonce')
            if run['room_title'] != mission_room_title(run['title_nonce']):
                raise ValueError('mission room title differs from the sealed native plan')
            expected_cases = required_flow_cases(
                '1', play_unit_uid=play_unit_uid, mission_mode=mission_mode,
            )
            allowed = {
                'id', 'role', 'suite', 'normal_flow', 'endpoint',
                'target_server', 'target_channel', 'server_channel_selection',
                'title_nonce', 'room_title', 'auto_nickname', 'exit_tutorial',
                'first_unit_draw', 'acknowledge_game_windows', 'process_entry',
                'account_path', 'retry_count', 'expect_first_fail',
                'required_cases', 'mission_mode', 'mission_scope',
                'mission_ai_count', 'mission_ai_level', *MISSION_CLAIM_FIELDS,
                'play_unit_uid', 'network_policy',
                *ROUTE_PLAN_FIELDS, 'quiet', 'quiet_loopback',
            }
            unexpected = set(run.keys()) - allowed
            if unexpected:
                raise ValueError(
                    'mission smoke run.ini contains unsealed fields: ' +
                    ', '.join(sorted(unexpected))
                )
            mode_sequence = None
        else:
            if any(field in run for field in (
                'mission_scope', 'mission_ai_count', 'mission_ai_level',
                *MISSION_CLAIM_FIELDS,
            )):
                raise ValueError('mission fields require native-mission-smoke suite')
            mode_sequence = ",".join(
                str(mode) for mode in parse_mode_sequence(run["mode_sequence"])
            )
            expected_cases = required_flow_cases(
                mode_sequence, bootstrap_only, battleship_only,
                gift_requests_only, deck_only, pages_only, collection_draw,
                play_unit_uid, shop_catalog=shop_plan is not None,friends_only=friends_only,
            )

        if (
            run["role"] != "client"
            or run["normal_flow"] != "1"
            or run["server_channel_selection"] != "native_automatic"
            or run["account_path"] != "private/account.ini"
            or run["required_cases"] != ",".join(expected_cases)
            or run["auto_nickname"] != "account_name"
            or run["exit_tutorial"] != "1"
            or run["first_unit_draw"] != "1"
            or run["acknowledge_game_windows"] != "1"
        ):
            raise ValueError("run contract does not describe the required normal client flow")

        host, port = run["endpoint"].rsplit(":", 1)
        if mission_plan and (
            not re.fullmatch(r'[1-9][0-9]*', port)
            or not 1 <= int(port) <= 65535
            or str(int(port)) != port
        ):
            raise ValueError('mission endpoint port must be canonical and in range')
        process_entry = run["process_entry"]
        plan = {
            "target_host": host,
            "login_port": int(port),
            "target_server": run["target_server"],
            "target_channel": run["target_channel"],
            "server_channel_selection": run["server_channel_selection"],
            "retry_count": int(run["retry_count"]),
            "expect_first_fail": run["expect_first_fail"] == "1",
            "title_nonce": run["title_nonce"],
            "room_title": run["room_title"],
            "auto_nickname": run["auto_nickname"],
            "exit_tutorial": run["exit_tutorial"] == "1",
            "first_unit_draw": run["first_unit_draw"] == "1",
            "acknowledge_game_windows": run["acknowledge_game_windows"] == "1",
            "launcher_path": "" if process_entry == GAME_EXE else process_entry,
        }
        if mission_plan:
            plan.update({
                'mission_mode': mission_mode,
                'mission_scope': MISSION_SCOPE,
                'mission_ai_count': MISSION_AI_COUNT,
                'mission_ai_level': MISSION_AI_LEVEL,
                'mission_clear_claim': False,
                'mission_result_claim': False,
                'mission_active_exit_claim': False,
            })
        else:
            plan['mode_sequence'] = mode_sequence
            plan['expected_ai'] = int(run['expected_ai'])
        if play_unit_uid is not None:
            plan['play_unit_uid'] = play_unit_uid
        if shop_plan is not None:
            plan.update(shop_plan)
        for flag, value in (
            ('bootstrap_only', bootstrap_only),
            ('battleship_only', battleship_only),
            ('gift_requests_only', gift_requests_only),
            ('deck_only', deck_only),
            ('pages_only', pages_only),
            ('friends_only',friends_only),
            ('collection_draw', collection_draw),
        ):
            if value:
                plan[flag] = True

        if not mission_plan and 'random_loadout' in run:
            if run['random_loadout'] not in ('0', '1'):
                raise ValueError('invalid random loadout policy')
            if run['random_loadout'] == '1':
                if any((bootstrap_only, battleship_only, gift_requests_only, deck_only, pages_only,friends_only)):
                    raise ValueError('random scenario requires PvP')
                nonce = run['title_nonce']
                expected_ai = int(run["expected_ai"])
                if (
                    not re.fullmatch(r'[0-9a-f]{4}', nonce)
                    or run['room_title'] !=
                    f'{nonce} !ai e{(expected_ai + 1) // 2} a{expected_ai // 2} !rand'
                ):
                    raise ValueError('random native room command differs')
                plan['random_loadout'] = True

        if 'network_policy' in run:
            if run['network_policy'] not in ('direct', 'preserve', ROUTED_LOOPBACK_POLICY):
                raise ValueError('invalid network policy')
            plan['network_policy'] = run['network_policy']
        route_fields_present = {field for field in ROUTE_PLAN_FIELDS if field in run}
        if plan.get('network_policy') == ROUTED_LOOPBACK_POLICY:
            if route_fields_present != set(ROUTE_PLAN_FIELDS):
                raise ValueError('routed loopback plan is missing sealed route fields')
            if run['route_profile_path'] != ROUTE_PROFILE_RELATIVE.as_posix():
                raise ValueError('invalid routed loopback profile path')
            route_hash = run['route_profile_sha256']
            if not re.fullmatch(r'[0-9a-f]{64}', route_hash):
                raise ValueError('invalid routed loopback profile hash')
            route_count = int(run['route_profile_route_count'])
            logical_ip = str(ipaddress.IPv4Address(run['logical_login_ipv4']))
            logical_port = int(run['logical_login_port'])
            physical_ip = str(ipaddress.IPv4Address(run['physical_login_ipv4']))
            physical_port = int(run['physical_login_port'])
            if not 1 <= route_count <= 32 or not 1 <= logical_port <= 65535 or not 1 <= physical_port <= 65535:
                raise ValueError('invalid routed loopback count or port')
            if not ipaddress.IPv4Address(logical_ip).is_loopback or not ipaddress.IPv4Address(physical_ip).is_loopback:
                raise ValueError('routed loopback endpoints must stay within IPv4 127/8')
            if logical_ip != host or logical_port != int(port):
                raise ValueError('routed logical login disagrees with endpoint')
            plan.update({
                'route_profile_path': run['route_profile_path'],
                'route_profile_sha256': route_hash,
                'route_profile_route_count': route_count,
                'logical_login_ipv4': logical_ip,
                'logical_login_port': logical_port,
                'physical_login_ipv4': physical_ip,
                'physical_login_port': physical_port,
            })
        elif route_fields_present:
            raise ValueError('route fields require routed_loopback')
        if 'quiet' in run:
            if run['quiet'] not in ('0', '1'):
                raise ValueError('invalid quiet policy')
            plan['quiet'] = run['quiet'] == '1'
            expected_quiet_loopback = (
                plan.get('network_policy') == ROUTED_LOOPBACK_POLICY or
                (plan.get('network_policy') == 'direct' and
                 ipaddress.IPv4Address(host).is_loopback)
            )
            if run.get('quiet_loopback', '') != ('1' if expected_quiet_loopback else '0'):
                raise ValueError('quiet loopback policy disagrees with the network plan')
        elif 'quiet_loopback' in run:
            raise ValueError('quiet_loopback requires a quiet run')
        return plan
    except (KeyError, ValueError) as error:
        raise GameTestError(f"invalid normal-flow run.ini: {error}") from error


def validate_deck_profile(parser: configparser.ConfigParser) -> None:
    try:
        values = [parser['deck'][f'unit_uid{i}'] for i in range(4)]
        if any(not re.fullmatch(r'[0-9]+',value) or not 0 < int(value) < 2**64 for value in values):
            raise ValueError('invalid unit UID')
        if len({int(value) for value in values}) != 4:
            raise ValueError('duplicate unit UID')
    except (KeyError,ValueError) as error:
        raise GameTestError('deck preparation requires four distinct real unit UIDs in the private deck profile') from error


def validate_private_account(root: Path, plan: dict) -> Path:
    path = root / "sdgo" / "gametest" / "private" / "account.ini"
    parser = read_ini(path)
    try:
        username = parser["account"]["username"]
        password = parser["account"]["password"]
        retry_count = int(parser["registration"]["retry_count"])
        expect_first_fail = parser["registration"]["expect_first_fail"] == "1"
        raw_lock_path = parser["runner"]["lock_path"]
    except (KeyError, ValueError) as error:
        raise GameTestError(f"invalid private GameTest account file: {error}") from error
    validate_account_credentials(parser, username, password)
    if plan.get('deck_only'):
        validate_deck_profile(parser)
    registration_state = parser.get('registration', 'state', fallback='')
    if registration_state in MISSION_EXISTING_ACCOUNT_STATES and (
        retry_count != 1 or expect_first_fail
    ):
        raise GameTestError('registered accounts cannot use registration retries')
    if (
        "mission_mode" in plan
        and registration_state not in MISSION_EXISTING_ACCOUNT_STATES
    ):
        raise GameTestError(
            "mission smoke requires a registered or explicitly imported existing account profile"
        )
    if retry_count != plan["retry_count"] or expect_first_fail != plan["expect_first_fail"]:
        raise GameTestError("private GameTest registration plan disagrees with run.ini")
    lock_path = Path(raw_lock_path)
    if not lock_path.is_absolute() or not lock_path.name.endswith(".run.lock"):
        raise GameTestError("private GameTest account lock path is invalid")
    return lock_path.resolve()


def validate_fps_prepared_client(root: Path, plan: dict) -> None:
    try:
        document = ET.parse(root / "config.xml").getroot()
        timer = document.find("Framework/Timer")
        if (
            timer is None
            or timer.get("updatefps") != str(plan["fps_cap"])
            or timer.get("VerticalSync") != "false"
            or timer.get("RenderSync") != "false"
        ):
            raise ValueError("Timer settings disagree with the FPS plan")
        login = document.find("LoginData")
        saved = login.find("CheckLoginIdSave") if login is not None else None
        automatic = login.find("CheckAutoLogin") if login is not None else None
        if (
            saved is None
            or saved.get("use") != "false"
            or saved.get("id", "") != ""
            or automatic is None
            or automatic.get("use") != "false"
            or automatic.get("id", "") != ""
            or automatic.get("pass", "") != ""
        ):
            raise ValueError("saved login identity was not removed from the copied client")
        manifest = read_ini(root / "sdgo" / "mods" / FPS_MOD / "mod.ini")
        expected = "1" if plan["fps_mod_enabled"] else "0"
        settings = read_ini(root / "sdgo" / "sdgo.ini")
        if (
            manifest.get("mod", "id", fallback="") != FPS_MOD
            or manifest.get("mod", "enabled", fallback="") != expected
            or settings.get("features", "fps-limit", fallback="") != expected
            or settings.get("mods", FPS_MOD, fallback="") != expected
        ):
            raise ValueError("fps_limit activation disagrees with the FPS plan")
    except (OSError, ET.ParseError, KeyError, ValueError, GameTestError) as error:
        raise GameTestError(f"invalid prepared native FPS client: {error}") from error


def collect_hashes(root: Path) -> tuple[dict, dict]:
    immutable: dict[str, str] = {}
    initial: dict[str, str] = {}

    def bind(path: Path, collection: dict) -> None:
        if not path.is_file():
            raise GameTestError(f"missing copied GameTest payload: {path}")
        collection[path.relative_to(root).as_posix()] = sha256(path)

    bind(root / GAME_EXE, immutable)
    for path in sorted(root.iterdir(), key=lambda item: item.name.lower()):
        if path.is_file() and path.suffix.lower() in IMMUTABLE_ROOT_SUFFIXES:
            bind(path, immutable)
    for path in (
        root / "sdgo" / "core.dll",
        root / "sdgo" / "sdgo.ini",
        root / "sdgo" / "gametest" / "run.ini",
    ):
        bind(path, immutable)
    symbols = root / "sdgo" / "symbols"
    for path in sorted(symbols.iterdir(), key=lambda item: item.name.lower()):
        if path.is_file() and path.suffix.lower() == ".ini":
            bind(path, immutable)
    mods = root / "sdgo" / "mods"
    for path in sorted(mods.rglob("*"), key=lambda item: item.as_posix().lower()):
        if path.is_file() and path.suffix.lower() in {".dll", ".lua", ".ini", ".tsv", ".json",
                                                      ".bin", ".gdt", ".zsd", ".spr", ".txr", ".uir", ".zpk", ".zdx"}:
            bind(path, immutable)
    for relative in (SHOP_ASSERTIONS_RELATIVE, SHOP_DISPLAY_RELATIVE):
        path = root / relative
        if path.exists():
            bind(path, immutable)
    for name in ("config.xml", "閰嶇疆鏂囦欢.ini"):
        path = root / name
        if path.is_file():
            bind(path, initial)
    return immutable, initial


def _remove_owned_preparation(path: Path, token: str) -> str:
    if not path.is_absolute() or path.name != f".gametest-preparing-{token}":
        return f"retained unverified preparation path: {path}"
    if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
        return f"retained linked preparation path: {path}"
    marker = path / ".gametest-preparing.json"
    if marker.is_symlink():
        return f"retained preparation with linked marker: {path}"
    try:
        data = read_json(marker)
        target = Path(data["target"]).resolve()
        resolved = path.resolve(strict=True)
    except (GameTestError, KeyError, OSError):
        return f"retained preparation with unverifiable marker: {path}"
    if data.get("token") != token or resolved.parent != target.parent:
        return f"retained preparation outside its verified target parent: {path}"
    return f"retained owned preparation for recoverable Recycle Bin cleanup: {path}"


def rename_prepared_directory(source: Path, target: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + max(0.0, timeout)
    delay = 0.05
    while True:
        if target.exists():
            raise GameTestError(f"refusing to overwrite existing run directory: {target}")
        try:
            os.replace(source, target)
            return
        except PermissionError as error:
            if getattr(error, "winerror", None) not in {5, 32}:
                raise GameTestError(f"cannot finalize GameTest directory: {error}") from error
            if time.monotonic() >= deadline:
                raise GameTestError(
                    f"cannot finalize GameTest directory after {timeout:g}s of Windows file-lock retries: {error}"
                ) from error
            time.sleep(min(delay, max(0.0, deadline - time.monotonic())))
            delay = min(delay * 2.0, 0.5)
        except OSError as error:
            raise GameTestError(f"cannot finalize GameTest directory: {error}") from error


def prepare(
    client_template: Path,
    stage: Path,
    run_dir: Path,
    *,
    target_host: str = "127.0.0.1",
    login_port: int = 5001,
    target_server: str = "1",
    target_channel: str = "1",
    mode_sequence: str = "1",
    retry_count: int | None = None,
    expect_first_fail: bool | None = None,
    expected_ai: int = 3,
    play_unit_uid: int | None = None,
    mission_mode: int | None = None,
    launcher_path: str | None = None,
    account_profile: Path | None = None,
    create_test_account: bool = False,
    sfx_only: bool = False,
    bootstrap_only: bool = False,
    battleship_only: bool = False,
    gift_requests_only: bool = False,
    deck_only: bool = False,
    pages_only: bool = False,
    friends_only: bool = False,
    collection_draw: bool = False,
    shop_assertions: Path | None = None,
    shop_display: Path | None = None,
    random_loadout: bool = False,
    network_policy: str = 'direct',
    route_profile: Path | None = None,
    quiet: bool = True,
    asset_baseline: Path | None = None,
    extra_mods: tuple[str, ...] = (),
    apsalus_roundtrip: bool = False,
    fps_cap: int | None = None,
    fps_mod_enabled: bool = True,
    _owned_client_transfer: tuple[Path, str] | None = None,
) -> dict:
    client_template = client_template.resolve()
    if apsalus_roundtrip and (mode_sequence != '1' or play_unit_uid is None or any((bootstrap_only,
            battleship_only,gift_requests_only,deck_only,pages_only,friends_only)) or mission_mode is not None):
        raise GameTestError('Apsalus roundtrip requires one complete ordinary battle and an existing owned unit UID')
    fps_only = fps_cap is not None
    if fps_only:
        if not quiet or sfx_only:
            raise GameTestError("native FPS tests are always background and silent")
        if network_policy != "direct":
            raise GameTestError("native FPS tests require the quiet loopback network policy")
        if account_profile is not None or create_test_account:
            raise GameTestError("native FPS tests do not read or create an account profile")
        if launcher_path:
            raise GameTestError("native FPS tests start only the copied GOnline.exe")
        if (
            any((
                bootstrap_only, battleship_only, gift_requests_only, deck_only,
                pages_only, friends_only, collection_draw, random_loadout,
            ))
            or play_unit_uid is not None
            or mission_mode is not None
            or shop_assertions is not None
            or shop_display is not None
            or mode_sequence != "1"
            or retry_count is not None
            or expect_first_fail is not None
            or expected_ai != 3
        ):
            raise GameTestError("native FPS tests cannot request an authenticated account flow")
    elif fps_mod_enabled is not True:
        raise GameTestError("fps_mod_enabled is valid only with fps_cap")
    shop_binding = None
    if not fps_only:
        play_unit_uid = validate_play_unit_uid(play_unit_uid)
        mission_mode = validate_mission_mode(mission_mode)
        if play_unit_uid is not None and create_test_account:
            raise GameTestError('--play-unit-uid requires an existing account and cannot create a test account')
        if mission_mode is not None and create_test_account:
            raise GameTestError('--mission-mode requires an existing account and cannot create a test account')
        if (shop_assertions is None) != (shop_display is None):
            raise GameTestError('--shop-assertions and --shop-display must be provided together')
        if shop_assertions is not None:
            if not pages_only:
                raise GameTestError('shop catalog assertions require --pages-only')
            shop_assertions = shop_assertions.expanduser().resolve()
            shop_display = shop_display.expanduser().resolve()
            shop_binding = load_shop_test_inputs(shop_assertions, shop_display)
        required_flow_cases(
            mode_sequence, bootstrap_only, battleship_only, gift_requests_only,
            deck_only, pages_only, collection_draw, play_unit_uid, mission_mode,
            shop_binding is not None, friends_only=friends_only,
        )
        if mission_mode is not None:
            if random_loadout:
                raise GameTestError('--mission-mode cannot be combined with the PvP random-loadout scope')
            if expected_ai != 3:
                raise GameTestError('--expected-ai is a PvP setting and cannot change a mission smoke plan')
    if quiet and sfx_only:raise GameTestError('SFX capture requires explicit --visible; ordinary tests are background and silent')
    if quiet:
        try:ipaddress.IPv4Address(target_host)
        except ipaddress.AddressValueError as error:raise GameTestError('quiet tests require an explicit IPv4 server endpoint') from error
    if network_policy not in ('direct','preserve',ROUTED_LOOPBACK_POLICY):raise GameTestError('invalid network policy')
    if network_policy in ('direct', ROUTED_LOOPBACK_POLICY):
        try:target_host=str(ipaddress.IPv4Address(target_host))
        except ipaddress.AddressValueError as error:raise GameTestError('direct and routed tests require an explicit IPv4 login endpoint') from error
    route_binding = None
    if network_policy == ROUTED_LOOPBACK_POLICY:
        if not ipaddress.IPv4Address(target_host).is_loopback:
            raise GameTestError('routed_loopback logical login must stay within IPv4 127/8')
        if route_profile is None:
            raise GameTestError('routed_loopback requires --route-profile')
        route_profile = route_profile.expanduser().resolve()
        route_binding = load_routed_loopback_profile(route_profile, target_host, login_port)
    elif route_profile is not None:
        raise GameTestError('--route-profile requires --network-policy routed_loopback')
    stage = stage.resolve()
    run_dir = run_dir.resolve()
    shared_assets={}
    asset_owner=None
    if asset_baseline is not None:
        asset_baseline=asset_baseline.resolve()
        if not is_relative_to(asset_baseline,OWNED_BUILD_ROOT.resolve()):
            raise GameTestError('shared assets must come from an owned local GameTest baseline')
        baseline=validate_metadata(asset_baseline)
        if read_json(asset_baseline/'receipts/report.json').get('status')!='pass':
            raise GameTestError('shared assets require a previously passed owned baseline')
        asset_owner=baseline['run_id']
        baseline_pid=(baseline.get('execution') or {}).get('game_pid')
        if baseline_pid and process_image_path(baseline_pid)==baseline['client']['game']:
            raise GameTestError('the archive baseline is still in use by its owned game process')
        for name in ('GPackData.zpk','GPackData.zdx'):
            owned=Path(baseline['client']['root'])/name
            expected=baseline['client']['immutable_hashes'].get(name)
            if not expected or sha256(owned)!=expected or sha256(client_template/name)!=expected:
                raise GameTestError('owned immutable archive baseline does not match this normal client')
            os.chmod(owned,stat.S_IREAD)
            shared_assets[name]=owned
    if not client_template.is_dir() or not (client_template / GAME_EXE).is_file():
        raise GameTestError(f"client template lacks {GAME_EXE}: {client_template}")
    if not (stage / "client").is_dir():
        raise GameTestError(f"stage lacks client tree: {stage / 'client'}")
    if run_dir.exists():
        raise GameTestError(f"run directory already exists: {run_dir}")
    if is_relative_to(run_dir, client_template):
        raise GameTestError("run directory cannot be inside the client template")
    validate_observer_stage(
        _owned_client_transfer[0]/'client' if _owned_client_transfer else client_template,
        stage,
        extra_mods,
        fps_mod_enabled=fps_mod_enabled if fps_only else None,
    )
    profile_path = None
    profile = None
    if fps_only:
        plan = make_fps_plan(target_host,login_port,fps_cap,fps_mod_enabled)
    else:
        profile_path = (account_profile or default_account_profile()).expanduser().resolve()
        if create_test_account:
            resolved_retry_count = 2 if retry_count is None else retry_count
            resolved_expect_first_fail = True if expect_first_fail is None else expect_first_fail
            if resolved_retry_count != 2 or not resolved_expect_first_fail:
                raise GameTestError(
                    "--create-test-account requires retry_count=2 and expect_first_fail=true"
                )
            profile = create_account_profile(profile_path)
        else:
            profile = load_account_profile(profile_path)
            registration_state = profile.get(
                "registration", "state", fallback=""
            ).strip()
            pending_first_login = registration_state == "pending_first_login"
            default_retry_count = 2 if pending_first_login else 1
            resolved_retry_count = default_retry_count if retry_count is None else retry_count
            resolved_expect_first_fail = False if expect_first_fail is None else expect_first_fail
            if registration_state in MISSION_EXISTING_ACCOUNT_STATES and (
                resolved_retry_count != 1 or resolved_expect_first_fail
            ):
                raise GameTestError('registered accounts cannot use registration retries')
            if play_unit_uid is not None and pending_first_login:
                raise GameTestError('--play-unit-uid requires an already registered account profile')
            if mission_mode is not None:
                if registration_state not in MISSION_EXISTING_ACCOUNT_STATES:
                    raise GameTestError(
                        '--mission-mode requires a registered or explicitly imported existing account profile'
                    )
                if resolved_retry_count != 1 or resolved_expect_first_fail:
                    raise GameTestError('--mission-mode forbids registration retry behavior')

        if deck_only:
            validate_deck_profile(profile)
        plan = make_plan(
            target_host,
            login_port,
            target_server,
            target_channel,
            mode_sequence,
            resolved_retry_count,
            resolved_expect_first_fail,
            expected_ai,
            launcher_path,
            play_unit_uid,
            mission_mode,
        )
        if random_loadout and any((
            bootstrap_only, battleship_only, gift_requests_only, deck_only,
            pages_only, friends_only, mission_mode is not None,
        )):
            raise GameTestError('random loadout requires a real PvP scenario')
        if random_loadout:
            # Keep the real title within the native twenty-byte limit. The run
            # UUID still owns all receipts; this four-hex nonce has the same
            # sixteen random bits as the ordinary gt-prefixed nonce.
            plan['title_nonce']=secrets.token_hex(2)
            plan['room_title']=f"{plan['title_nonce']} !ai e{(expected_ai+1)//2} a{expected_ai//2} !rand"
            plan['random_loadout']=True
        plan['network_policy']=network_policy
        if route_binding is not None:
            plan.update(route_binding)
        if shop_binding is not None:
            plan.update(shop_binding)
        plan['quiet']=quiet
        for flag,value in (
            ('bootstrap_only',bootstrap_only),
            ('battleship_only',battleship_only),
            ('gift_requests_only',gift_requests_only),
            ('deck_only',deck_only),
            ('pages_only',pages_only),
            ('friends_only',friends_only),
            ('collection_draw',collection_draw),
        ):
            if value:plan[flag] = True
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    preparing = run_dir.parent / f".gametest-preparing-{token}"
    preparing.mkdir()
    atomic_write(
        preparing / ".gametest-preparing.json",
        json.dumps({"token": token, "target": str(run_dir)}, indent=2),
    )
    run_id = str(uuid.uuid4())
    client = preparing / "client"
    try:
        def copy_client_file(source,destination):
            original=Path(source)
            if original.parent.resolve()==client_template and original.name in shared_assets:
                os.link(shared_assets[original.name],destination)
                return str(destination)
            return shutil.copy2(source,destination)
        if _owned_client_transfer is None:
            shutil.copytree(client_template, client, ignore=copy_ignore,copy_function=copy_client_file)
        else:
            holding, reuse_token = _owned_client_transfer
            marker=read_json(holding/'.gametest-reusing.json')
            if (holding.parent!=run_dir.parent or not holding.name.startswith('.gametest-reusing-') or
                marker.get('token')!=reuse_token or marker.get('target')!=str(run_dir) or
                not is_relative_to(holding,OWNED_BUILD_ROOT.resolve()) or
                (holding/'client').is_symlink() or
                (hasattr(holding/'client','is_junction') and (holding/'client').is_junction()) or
                (holding/'client').resolve().parent!=holding.resolve()):
                raise GameTestError('owned client transfer marker or scope differs')
            os.replace(holding/'client',client)
            recycle_owned_artifact(client/'sdgo/gametest',preparing)
            recycle_owned_artifact(client/'sdgo/mods'/TEST_MOD,preparing)
        overlay_observer(client, stage, extra_mods, fps_mod_enabled=fps_mod_enabled if fps_only else None)
        observer_manifest=client/'sdgo/mods'/TEST_MOD/'mod.ini'
        observer_config=read_ini(observer_manifest)
        if not observer_config.has_section('settings'):observer_config.add_section('settings')
        observer_config['settings']['ApsalusRoundtrip']='1' if apsalus_roundtrip else '0'
        write_ini(observer_manifest,observer_config)
        installed_route = configure_test_network(
            client,
            network_policy,
            route_profile=route_profile,
            route_binding=route_binding,
        )
        if installed_route is not None and installed_route != route_binding:
            raise GameTestError('installed route binding differs from the validated source profile')
        if shop_binding is not None:
            installed_shop = install_shop_test_inputs(
                client,
                shop_assertions,
                shop_display,
                shop_binding,
            )
            if installed_shop != shop_binding:
                raise GameTestError('installed shop inputs differ from their sealed source bytes')
        recording = configure_sfx_only(client) if sfx_only else None
        if quiet:configure_muted_audio(client)
        if fps_only:configure_fps_test(client, fps_cap)
        write_job(client, run_id, plan, profile, profile_path)

        entry = client / (launcher_path or GAME_EXE)
        if not entry.is_file() or not is_relative_to(entry.resolve(), client.resolve()):
            raise GameTestError(f"configured process entry is not inside the copied client: {entry}")
        immutable, initial = collect_hashes(client)
        source_root = Path(__file__).resolve().parents[1]
        metadata = {
            "schema": SCHEMA_VERSION,
            "run_id": run_id,
            "suite": FPS_SUITE if fps_only else metadata_suite_for_plan(plan),
            "status": "prepared",
            "created_utc": utc_now(),
            "source": {
                **git_source_ref(source_root),
                "runner_sha256": sha256(Path(__file__).resolve()),
            },
            "template": str(client_template),
            "stage": str(stage),
            "plan": plan,
            "client": {
                "root": str((run_dir / "client").resolve()),
                "entry": str((run_dir / "client" / (launcher_path or GAME_EXE)).resolve()),
                "game": str((run_dir / "client" / GAME_EXE).resolve()),
                "host_key": HOST_KEY,
                "events": str(
                    (run_dir / "client" / "sdgo" / "gametest" / "events.jsonl").resolve()
                ),
                "stdout": str((run_dir / "receipts" / "client.stdout.log").resolve()),
                "stderr": str((run_dir / "receipts" / "client.stderr.log").resolve()),
                "immutable_hashes": immutable,
                "initial_inputs": initial,
            },
            "execution": None,
        }
        if shared_assets:
            metadata['assets']={'mode':'owned_readonly_hardlinks','baseline_run_id':asset_owner,
                                'baseline':str(asset_baseline),'files':sorted(shared_assets)}
        if recording is not None:
            metadata["recording"] = recording
        atomic_write(preparing / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
        rename_prepared_directory(preparing, run_dir)
        (run_dir / ".gametest-preparing.json").unlink()
        return read_json(run_dir / "run.json")
    except GameTestError as error:
        cleanup = _remove_owned_preparation(preparing, token)
        raise GameTestError(f"{error}; {cleanup}") from error
    except BaseException:
        _remove_owned_preparation(preparing, token)
        raise


def recycle_owned_artifact(path: Path, owner_root: Path) -> None:
    """Use the OS Recycle Bin; never fall back to permanent removal."""
    if not path.exists():return
    if path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction()):
        raise GameTestError('refusing to recycle a linked GameTest artifact')
    resolved=path.resolve();owner=owner_root.resolve()
    if resolved==owner or not is_relative_to(resolved,owner):
        raise GameTestError('recycle target escapes its owned GameTest scope')
    try:
        from send2trash import send2trash
    except ImportError as error:
        raise GameTestError('client reuse requires Send2Trash; install it before reuse; no permanent-delete fallback') from error
    send2trash(str(resolved))
    if resolved.exists():raise GameTestError('owned artifact was not moved to the Recycle Bin')


def reuse_prepared(client_template: Path, stage: Path, run_dir: Path, **options) -> dict:
    """Move one stopped owned client into a fresh capsule, keeping its file IDs."""
    raw=run_dir.absolute()
    if raw.is_symlink() or (hasattr(raw,'is_junction') and raw.is_junction()):
        raise GameTestError('client reuse rejects linked run directories')
    run_dir=raw.resolve();owned=OWNED_BUILD_ROOT.resolve()
    if run_dir==owned or not is_relative_to(run_dir,owned):
        raise GameTestError('client reuse requires an owned build run, never the player client')
    if options.get('create_test_account'):
        raise GameTestError('client reuse cannot register another account')
    # Preflight the recycler before changing paths or accounts.
    try:__import__('send2trash')
    except ImportError as error:raise GameTestError('install Send2Trash before client reuse; no files changed') from error
    previous=validate_metadata(run_dir);verify_hashes(previous)
    client_root=run_dir/'client'
    if options.get('extra_mods'):validate_observer_stage(client_root,stage.resolve(),options['extra_mods'])
    else:validate_observer_stage(client_root,stage.resolve())
    for directory,dirs,files in os.walk(client_root,followlinks=False):
        for name in dirs+files:
            path=Path(directory)/name
            if path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction()):
                raise GameTestError('client reuse rejects linked client files/directories')
            if path.is_file() and path.stat().st_nlink>1 and path.relative_to(client_root).as_posix() not in ('GPackData.zpk','GPackData.zdx'):
                raise GameTestError('client reuse rejects shared mutable runtime files')
    for other in owned.rglob('run.json'):
        if other.parent==run_dir:continue
        data=read_json(other);baseline=data.get('assets',{}).get('baseline')
        if baseline and Path(baseline).resolve()==run_dir:
            raise GameTestError('this run is an archive baseline referenced by another capsule')
    image=Path(previous['client']['game']).resolve()
    previous_pid=(previous.get('execution') or {}).get('game_pid')
    for row in process_snapshot():
        value=row.get('image_path')
        if (value and is_relative_to(Path(value).resolve(),run_dir)) or (row['pid']==previous_pid and not value):
            raise GameTestError('the owned client may still be running; reuse made no changes')
    account_lock_path=validate_private_account(Path(previous['client']['root']),previous['plan'])
    profile=account_lock_path.with_name(account_lock_path.name.removesuffix('.run.lock'))
    if options.get('account_profile') and Path(options['account_profile']).resolve()!=profile:
        raise GameTestError('reuse keeps the same persistent account profile')
    options['account_profile']=profile
    if previous.get('assets') and options.get('asset_baseline') is None:
        options['asset_baseline']=Path(previous['assets']['baseline'])
    archive=owned/'gametest-current';archive.mkdir(exist_ok=True)
    if archive.is_symlink() or (hasattr(archive,'is_junction') and archive.is_junction()):
        raise GameTestError('client reuse rejects a linked receipt archive')
    lock,token=acquire_lock(run_dir);account_lock=None;account_token=None
    holding=run_dir.parent/('.gametest-reusing-'+uuid.uuid4().hex)
    try:
        account_lock,account_token=acquire_exclusive_lock(account_lock_path,'persistent account')
        prior={'run_id':previous['run_id'],'source':previous.get('source'),'plan':previous['plan'],
               'immutable_hashes':previous['client']['immutable_hashes'],'execution':previous.get('execution')}
        proofs=archive/'proof-binaries';proofs.mkdir(exist_ok=True)
        if proofs.is_symlink() or (hasattr(proofs,'is_junction') and proofs.is_junction()):
            raise GameTestError('client reuse rejects a linked binary proof archive')
        prior['binaries']={}
        for identifier in (TEST_MOD,TARGET_MOD,'room_random_client'):
            payload=client_root/f'sdgo/mods/{identifier}/{identifier}.dll'
            if not payload.is_file():continue
            digest=sha256(payload);target=proofs/(digest+'.dll')
            if target.exists() and (target.is_symlink() or sha256(target)!=digest):
                raise GameTestError('existing binary proof is linked or has changed')
            if not target.exists():shutil.copy2(payload,target)
            prior['binaries'][identifier]={'sha256':digest,'path':target.relative_to(archive).as_posix()}
        report=run_dir/'receipts/report.json'
        if report.exists():prior['report']=read_json(report)
        events=Path(previous['client']['events'])
        if events.exists():prior['events']=events.read_text(encoding='utf-8')
        old=archive/'previous-run.json'
        recycle_owned_artifact(old,archive)
        atomic_write(old,json.dumps(prior,indent=2)+'\n')
        rename_prepared_directory(run_dir,holding)
        atomic_write(holding/'.gametest-reusing.json',json.dumps({'token':token,'target':str(run_dir)}))
        release_owned_lock(holding/lock.name,token)
        metadata=prepare(client_template,stage,run_dir,
                         _owned_client_transfer=(holding,token),**options)
        metadata['reuse']={'previous_run_id':previous['run_id'],'mode':'move_owned_client_no_copy'}
        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
        recycle_owned_artifact(holding,owned)
        return metadata
    except BaseException:
        # A preflight failure before the transfer leaves a complete old capsule.
        # Restore its path; a transferred/partially updated client stays in the
        # token-marked preparation for recovery, never gets deleted.
        if holding.is_dir() and (holding/'client').is_dir() and not run_dir.exists():
            release_owned_lock(holding/lock.name,token)
            rename_prepared_directory(holding,run_dir)
        raise
    finally:
        release_owned_lock(run_dir/lock.name,token)
        if account_lock is not None:release_owned_lock(account_lock,account_token)


def validate_metadata(run_dir: Path) -> dict:
    if run_dir.is_symlink() or (hasattr(run_dir,'is_junction') and run_dir.is_junction()):
        raise GameTestError('run directory is linked rather than an owned capsule')
    run_dir = run_dir.resolve()
    metadata = read_json(run_dir / "run.json")
    if metadata.get("schema") != SCHEMA_VERSION:
        raise GameTestError("unsupported run.json schema")
    run_id = str(metadata.get("run_id", ""))
    try:
        parsed = uuid.UUID(run_id)
    except ValueError as error:
        raise GameTestError("invalid run_id in run.json") from error
    if run_id != str(parsed):
        raise GameTestError("run_id in run.json must use canonical lowercase UUID form")
    client = metadata.get("client")
    if not isinstance(client, dict):
        raise GameTestError("run.json lacks client metadata")
    raw_root=run_dir/'client'
    if raw_root.is_symlink() or (hasattr(raw_root,'is_junction') and raw_root.is_junction()):
        raise GameTestError('client root is linked rather than an owned copy')
    root = raw_root.resolve()
    if root.parent!=run_dir:
        raise GameTestError('client root escapes this run directory')
    if Path(client.get("root", "")).resolve() != root:
        raise GameTestError("client root escapes this run directory")
    if Path(client.get("game", "")).resolve() != root / GAME_EXE:
        raise GameTestError("game path does not match this run directory")
    entry = Path(client.get("entry", "")).resolve()
    if not is_relative_to(entry, root):
        raise GameTestError("process entry escapes this run directory")
    if Path(client.get("events", "")).resolve() != root / "sdgo" / "gametest" / "events.jsonl":
        raise GameTestError("native event path does not match this run directory")
    if client.get("host_key") != HOST_KEY:
        raise GameTestError("run.json changes the expected client host")

    plan = metadata.get("plan")
    if not isinstance(plan, dict):
        raise GameTestError("run.json lacks normal-flow plan")
    public_plan = read_public_plan(root)
    public_run = read_ini(root / "sdgo" / "gametest" / "run.ini")["run"]
    if public_run.get("id") != run_id:
        raise GameTestError("run.json run_id disagrees with run.ini")
    fps_only = "fps_cap" in public_plan
    expected_suite = FPS_SUITE if fps_only else metadata_suite_for_plan(public_plan)
    if metadata.get("suite") != expected_suite:
        raise GameTestError("run.json suite disagrees with the sealed normal-flow plan")
    public_shop_fields = set()
    if fps_only:
        if set(plan) != set(public_plan):
            raise GameTestError("run.json adds fields outside the native FPS plan")
    else:
        if "mission_mode" in public_plan:
            forbidden = {
                "mode_sequence", "expected_ai", "bootstrap_only",
                "battleship_only", "gift_requests_only", "deck_only",
                "pages_only", "friends_only", "collection_draw", "random_loadout",
            }
            if forbidden.intersection(plan):
                raise GameTestError("run.json adds PvP, pages, or preparation scope to a mission smoke")
            if set(plan) != set(public_plan):
                raise GameTestError("run.json mission plan fields disagree with the sealed public plan")
            for key, value in public_plan.items():
                if type(plan[key]) is not type(value) or plan[key] != value:
                    raise GameTestError(f"run.json mission plan disagrees with run.ini for {key}")
        elif any(field in plan for field in (
            "mission_mode", "mission_scope", "mission_ai_count",
            "mission_ai_level", *MISSION_CLAIM_FIELDS,
        )):
            raise GameTestError("run.json cannot add mission scope to a non-mission plan")
        for flag in (
            'bootstrap_only','battleship_only','gift_requests_only','deck_only',
            'pages_only','friends_only','collection_draw','random_loadout',
        ):
            if (
                type(plan.get(flag, False)) is not bool
                or plan.get(flag, False) != public_plan.get(flag, False)
            ):
                raise GameTestError('run.json cannot change the required bootstrap or battle scope')
        if 'play_unit_uid' in plan:
            validate_play_unit_uid(plan['play_unit_uid'])
        if ('play_unit_uid' in plan) != ('play_unit_uid' in public_plan):
            raise GameTestError('run.json cannot add or remove the configured owned unit UID')
        unknown_plan_shop_fields = {
            field for field in plan if field.startswith("shop_")
        } - set(SHOP_PLAN_FIELDS)
        if unknown_plan_shop_fields:
            raise GameTestError('run.json contains unknown shop GameTest fields')
        plan_shop_fields = {field for field in SHOP_PLAN_FIELDS if field in plan}
        public_shop_fields = {field for field in SHOP_PLAN_FIELDS if field in public_plan}
        if plan_shop_fields not in (set(), set(SHOP_PLAN_FIELDS)):
            raise GameTestError('run.json has a partial shop GameTest input plan')
        if plan_shop_fields != public_shop_fields:
            raise GameTestError('run.json cannot add or remove shop GameTest inputs')
    for key, value in public_plan.items():
        if plan.get(key) != value:
            raise GameTestError(f"run.json plan disagrees with run.ini for {key}")
    if not fps_only:
        if public_shop_fields:
            if not plan.get('pages_only'):
                raise GameTestError('shop GameTest inputs require the native pages suite')
            validate_shop_test_install(root, plan)
        if plan.get('network_policy') == ROUTED_LOOPBACK_POLICY:
            validate_routed_loopback_install(root, plan)
    expected_launcher = plan.get("launcher_path", "")
    expected_entry = (root / (expected_launcher or GAME_EXE)).resolve()
    if entry != expected_entry or not is_relative_to(expected_entry, root):
        raise GameTestError("run.json entry disagrees with its normal-flow plan")
    if fps_only:
        if (root / "sdgo" / "gametest" / "private" / "account.ini").exists():
            raise GameTestError("native FPS GameTest unexpectedly contains a private account")
        validate_fps_prepared_client(root, plan)
    else:
        validate_private_account(root, plan)
    return metadata


def verify_hashes(metadata: dict) -> None:
    root = Path(metadata["client"]["root"]).resolve()
    hashes = metadata["client"].get("immutable_hashes")
    if not isinstance(hashes, dict) or not hashes:
        raise GameTestError("client has no immutable payload hashes")
    plan = metadata.get("plan")
    if not isinstance(plan, dict):
        raise GameTestError("run.json lacks normal-flow plan")
    shop_fields = {field for field in SHOP_PLAN_FIELDS if field in plan}
    if shop_fields and shop_fields != set(SHOP_PLAN_FIELDS):
        raise GameTestError("shop GameTest plan is missing sealed input fields")
    if shop_fields:
        required_shop_hashes = {
            SHOP_ASSERTIONS_RELATIVE.as_posix(): plan["shop_assertions_sha256"],
            SHOP_DISPLAY_RELATIVE.as_posix(): plan["shop_display_sha256"],
        }
        for relative, expected in required_shop_hashes.items():
            if hashes.get(relative) != expected:
                raise GameTestError(
                    f"immutable payload map does not bind sealed shop input: {relative}"
                )
    for relative, expected in hashes.items():
        if not isinstance(relative, str) or not relative:
            raise GameTestError("invalid immutable payload path in run.json")
        relative_path = Path(relative)
        path = (root / relative_path).resolve()
        if relative_path.is_absolute() or not is_relative_to(path, root):
            raise GameTestError(f"immutable payload path escapes the client capsule: {relative}")
        if (
            not isinstance(expected, str)
            or len(expected) != 64
            or any(character not in "0123456789abcdef" for character in expected)
        ):
            raise GameTestError(f"invalid immutable payload hash for {relative}")
        if not path.is_file() or sha256(path) != expected:
            raise GameTestError(f"immutable client payload changed since preparation: {path}")


def acquire_exclusive_lock(path: Path, label: str) -> tuple[Path, str]:
    token = uuid.uuid4().hex
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump({"token": token, "pid": os.getpid(), "created_utc": utc_now()}, stream)
    except FileExistsError as error:
        raise GameTestError(f"{label} lock already exists and was preserved: {path}") from error
    return path, token


def acquire_lock(run_dir: Path) -> tuple[Path, str]:
    return acquire_exclusive_lock(run_dir / ".run.lock", "run")


def acquire_account_lock(root: Path, plan: dict) -> tuple[Path, str]:
    lock_path = validate_private_account(root, plan)
    return acquire_exclusive_lock(lock_path, "persistent account")


def release_owned_lock(lock: Path, token: str) -> None:
    try:
        if read_json(lock).get("token") == token:
            lock.unlink()
    except (GameTestError, FileNotFoundError):
        pass


class NamedStartupEvent:
    def __init__(self, name: str = STARTUP_EVENT_NAME):
        self.name = name
        self.handle = None

    def __enter__(self):
        if os.name == "nt":
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CreateEventW.argtypes = [
                wintypes.LPVOID,
                wintypes.BOOL,
                wintypes.BOOL,
                wintypes.LPCWSTR,
            ]
            kernel32.CreateEventW.restype = wintypes.HANDLE
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            self.handle = kernel32.CreateEventW(None, False, False, self.name)
            if not self.handle:
                raise OSError(ctypes.get_last_error(), f"cannot create startup event {self.name!r}")
        return self

    def __exit__(self, *_exc):
        if self.handle:
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            kernel32.CloseHandle(self.handle)
            self.handle = None


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


def process_image_path(pid: int) -> str | None:
    if os.name != "nt":
        return None
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    process = kernel32.OpenProcess(0x1000, False, pid)
    if not process:
        return None
    try:
        capacity = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(capacity.value)
        if not kernel32.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(capacity)):
            return None
        return buffer.value
    finally:
        kernel32.CloseHandle(process)


def process_snapshot() -> list[dict]:
    if os.name != "nt":
        return []
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    invalid = ctypes.c_void_p(-1).value
    if snapshot == invalid:
        return []
    rows = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                pid = int(entry.th32ProcessID)
                rows.append(
                    {
                        "pid": pid,
                        "parent_pid": int(entry.th32ParentProcessID),
                        "image_path": process_image_path(pid),
                    }
                )
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
    finally:
        kernel32.CloseHandle(snapshot)
    return rows


def select_owned_game_process(rows: list[dict], entry_pid: int, expected_game: Path) -> dict | None:
    owned = {entry_pid}
    changed = True
    while changed:
        changed = False
        for row in rows:
            if row.get("parent_pid") in owned and row.get("pid") not in owned:
                owned.add(row["pid"])
                changed = True
    expected = expected_game.resolve()
    candidates = []
    for row in rows:
        image = row.get("image_path")
        if row.get("pid") in owned and image and Path(image).resolve() == expected:
            candidates.append(row)
    return min(candidates, key=lambda row: row["pid"]) if candidates else None


def wait_for_owned_game_process(entry_process, entry_path: Path, expected_game: Path, timeout: float) -> dict | None:
    if entry_path.resolve() == expected_game.resolve():
        return {
            "pid": entry_process.pid,
            "parent_pid": os.getpid(),
            "image_path": str(expected_game.resolve()),
        }
    deadline = time.monotonic() + max(0.0, timeout)
    while True:
        selected = select_owned_game_process(process_snapshot(), entry_process.pid, expected_game)
        if selected:
            return selected
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.1)


def terminate_verified_process(pid: int, expected_path: Path) -> bool:
    current = process_image_path(pid)
    if not current or Path(current).resolve() != expected_path.resolve() or os.name != "nt":
        return False
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    process = kernel32.OpenProcess(0x0001 | 0x1000, False, pid)
    if not process:
        return False
    try:
        return bool(kernel32.TerminateProcess(process, 0))
    finally:
        kernel32.CloseHandle(process)


def has_run_end(path: Path) -> bool:
    if not path.is_file():
        return False
    for raw in path.read_bytes().splitlines():
        try:
            event = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if isinstance(event, dict) and event.get("event") == "run_end":
            return True
    return False


def spawn_entry(metadata: dict, receipt_dir: Path, quiet_runtime=None):
    client = metadata["client"]
    plan = metadata["plan"]
    entry = Path(client["entry"])
    game = Path(client["game"])
    arguments = [str(entry)]
    if entry.resolve() == game.resolve():
        arguments.extend([plan["target_host"], str(plan["login_port"])])
    environment = os.environ.copy()
    environment["SDGO_GAME_TEST_RUN"] = metadata["run_id"]
    environment["SDGO_GAME_TEST_ROLE"] = "client"
    environment["SDGO_GAME_TEST_PLAN"] = str(
        Path(client["root"]) / "sdgo" / "gametest" / "run.ini"
    )
    if "fps_cap" not in plan:
        environment["SDGO_GAME_TEST_ACCOUNT"] = str(
            Path(client["root"]) / "sdgo" / "gametest" / "private" / "account.ini"
        )
    else:
        environment.pop("SDGO_GAME_TEST_ACCOUNT", None)
    environment["__COMPAT_LAYER"] = "RunAsInvoker"
    stdout = (receipt_dir / "client.stdout.log").open("wb")
    stderr = (receipt_dir / "client.stderr.log").open("wb")
    try:
        launch=quiet_runtime.spawn if quiet_runtime else subprocess.Popen
        process = launch(
            arguments,
            cwd=client["root"],
            env=environment,
            shell=False,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            startupinfo=startup_info(),
        )
    except BaseException:
        stdout.close()
        stderr.close()
        raise
    return process, stdout, stderr


def startup_info():
    if os.name != "nt":
        return None
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 0  # SW_HIDE; quiet runs additionally use a separate desktop.
    return info


def run_prepared(run_dir: Path, timeout: float = 600.0, launch_timeout: float = 30.0, *, on_process=None, on_poll=None) -> dict:
    run_dir = run_dir.resolve()
    metadata = validate_metadata(run_dir)
    verify_hashes(metadata)
    if metadata.get("execution") is not None:
        raise GameTestError("this capsule already has an execution receipt; prepare a fresh run")
    events = Path(metadata["client"]["events"])
    if events.exists():
        raise GameTestError(f"native event file already exists; prepare a fresh run: {events}")

    lock, token = acquire_lock(run_dir)
    account_lock = None
    account_token = None
    receipt_dir = run_dir / "receipts"
    entry_process = None
    game_process = None
    quiet_runtime = None
    handles = []
    started = utc_now()
    try:
        receipt_dir.mkdir(exist_ok=True)
        if "fps_cap" not in metadata["plan"]:
            account_lock, account_token = acquire_account_lock(
                Path(metadata["client"]["root"]), metadata["plan"]
            )
        if metadata['plan'].get('quiet'):
            quiet_runtime=QuietRuntime(metadata,receipt_dir)
            quiet_runtime.start()
        with NamedStartupEvent():
            try:
                entry_process, stdout, stderr = spawn_entry(metadata, receipt_dir,quiet_runtime)
                handles.extend((stdout, stderr))
                if on_process:
                    on_process(entry_process.pid, Path(metadata['client']['entry']))
            except OSError as error:
                metadata["execution"] = {
                    "started_utc": started,
                    "finished_utc": utc_now(),
                    "entry_pid": None,
                    "game_pid": None,
                    "game_image_path": None,
                    "status": "launch_error",
                    "timed_out": False,
                    "error": f"{type(error).__name__}: {error}",
                }
                atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
                return build_report(run_dir)

            game_process = wait_for_owned_game_process(
                entry_process,
                Path(metadata["client"]["entry"]),
                Path(metadata["client"]["game"]),
                launch_timeout,
            )
            if not game_process:
                metadata["execution"] = {
                    "started_utc": started,
                    "finished_utc": utc_now(),
                    "entry_pid": entry_process.pid,
                    "game_pid": None,
                    "game_image_path": None,
                    "status": "launch_timeout",
                    "timed_out": True,
                    "error": "copied GOnline.exe was not observed as the configured entry or its descendant",
                }
                atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
                return build_report(run_dir)

            metadata["execution"] = {
                "started_utc": started,
                "finished_utc": None,
                "entry_pid": entry_process.pid,
                "game_pid": game_process["pid"],
                "game_image_path": str(Path(game_process["image_path"]).resolve()),
                "status": "running",
                "timed_out": False,
                "error": None,
            }
            if on_process:
                on_process(game_process['pid'], Path(game_process['image_path']))
            metadata["status"] = "running"
            atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))

            deadline = time.monotonic() + max(0.0, timeout)
            status = "running"
            while time.monotonic() < deadline:
                if on_poll:on_poll()
                if has_run_end(events):
                    status = "completed"
                    break
                if quiet_runtime:
                    if process_image_path(game_process['pid']) is None:
                        status='game_exited';break
                    proof=quiet_runtime.verify(game_process['pid'])
                    if proof and not metadata['execution'].get('quiet_verified'):
                        metadata['execution']['quiet_verified']=proof
                        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
                if ("fps_cap" not in metadata["plan"] and
                    metadata['plan'].get('network_policy') in ('direct', ROUTED_LOOPBACK_POLICY) and
                    not metadata['execution'].get('network_verified')):
                    proof=verify_login_peer(metadata,game_process['pid'])
                    if proof:
                        metadata['execution']['network_verified']=proof
                        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
                if process_image_path(game_process["pid"]) is None:
                    status = "game_exited"
                    break
                time.sleep(0.1)
            if status == "running":
                status = "timeout"
            metadata["execution"]["status"] = status
            metadata["execution"]["timed_out"] = status == "timeout"
            metadata["execution"]["finished_utc"] = utc_now()
            metadata["status"] = "executed"
            atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
    except (GameTestError,OSError) as error:
        if quiet_runtime is None:raise
        execution=metadata.get('execution') or {}
        metadata['execution']=execution
        execution.update(status='launch_error',error=str(error),finished_utc=utc_now())
        metadata['status']='executed'
        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
    finally:
        if not isinstance(metadata.get('execution'),dict):metadata['execution']={}
        cleanup_errors=[]
        def cleanup(label,action):
            try:action()
            except Exception as error:cleanup_errors.append(f'{label}: {type(error).__name__}: {error}')
        def stop_entry():
            if entry_process and entry_process.poll() is None:
                try:entry_process.kill()
                except OSError:
                    # Windows can reject a second termination while the first
                    # exit is still pending. Accept only a confirmed exit.
                    entry_process.wait(timeout=10)
                    if entry_process.poll() is None:raise
                    return
                entry_process.wait(timeout=10)
        if game_process:
            if entry_process and game_process["pid"] == entry_process.pid:
                cleanup('owned game stop',stop_entry)
            else:
                cleanup('verified child stop',lambda:terminate_verified_process(game_process["pid"], Path(metadata["client"]["game"])))
        cleanup('owned entry stop',stop_entry)
        for handle in handles:
            cleanup('output stream close',handle.close)
        if entry_process and metadata.get('execution') is not None:
            cleanup('entry exit readback',lambda:metadata['execution'].update(entry_exit_code=entry_process.poll()))
        if quiet_runtime:
            cleanup('quiet runtime close',lambda:metadata.setdefault('execution',{}).update(quiet_cleanup=quiet_runtime.close()))
        owned_alive=True
        try:
            owned_alive=bool(entry_process and entry_process.poll() is None)
            if game_process and (not entry_process or game_process['pid']!=entry_process.pid):
                owned_alive=owned_alive or any(row['pid']==game_process['pid'] for row in process_snapshot())
        except Exception as error:cleanup_errors.append(f'owned exit readback: {type(error).__name__}: {error}')
        if not owned_alive:
            if account_lock is not None and account_token is not None:
                cleanup('account lock release',lambda:release_owned_lock(account_lock,account_token))
            cleanup('run lock release',lambda:release_owned_lock(lock,token))
        else:
            cleanup_errors.append('owned process exit is unconfirmed; account and run locks retained')
        if cleanup_errors:
            metadata.setdefault('execution',{}).update(status='cleanup_error',cleanup_errors=cleanup_errors,
                                                       finished_utc=utc_now())
            metadata['status']='executed'
        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
    return build_report(run_dir)


def evaluate_events(
    path: Path,
    run_id: str,
    expected_pid: int | None,
    expected_image: Path,
    required_cases: tuple[str, ...],
) -> dict:
    errors: list[str] = []
    pending: list[str] = []
    events: list[dict] = []
    if not path.is_file():
        pending.append(f"native event log has not been created: {path}")
    else:
        for line_number, raw in enumerate(path.read_bytes().splitlines(), 1):
            try:
                event = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                errors.append(f"line {line_number}: malformed or truncated JSON: {error}")
                continue
            if not isinstance(event, dict):
                errors.append(f"line {line_number}: event is not an object")
                continue
            required_fields = (
                "run_id",
                "role",
                "event",
                "case",
                "status",
                "message",
                "host_key",
                "pid",
                "image_path",
                "duration_ms",
            )
            missing = [field for field in required_fields if field not in event]
            if missing:
                errors.append(f"line {line_number}: missing fields {', '.join(missing)}")
                continue
            if event["run_id"] != run_id or event["role"] != "client":
                errors.append(f"line {line_number}: foreign run_id or role")
                continue
            if (
                not isinstance(event["event"], str)
                or not isinstance(event["case"], str)
                or not isinstance(event["status"], str)
                or not isinstance(event["message"], str)
                or event["host_key"] != HOST_KEY
                or not isinstance(event["pid"], int)
                or event["pid"] <= 0
                or not isinstance(event["image_path"], str)
                or not isinstance(event["duration_ms"], (int, float))
                or event["duration_ms"] < 0
            ):
                errors.append(f"line {line_number}: invalid native event fields")
                continue
            try:
                image = Path(event["image_path"]).resolve()
            except OSError:
                errors.append(f"line {line_number}: invalid native image path")
                continue
            if image != expected_image.resolve():
                errors.append(f"line {line_number}: event came from a different executable: {image}")
                continue
            if expected_pid is not None and event["pid"] != expected_pid:
                errors.append(
                    f"line {line_number}: native pid {event['pid']} does not match owned pid {expected_pid}"
                )
                continue
            events.append(event)

    allowed = {
        "run_begin": {"running"},
        "observation": {"info"},
        "case_begin": {"running"},
        "assert": {"pass", "fail"},
        "case_end": {"pass", "fail", "skip"},
        "run_end": {"pass", "fail"},
    }
    for event in events:
        if event["event"] not in allowed or event["status"] not in allowed[event["event"]]:
            errors.append(f"invalid {event['event']!r} event status {event['status']!r}")
    if events:
        identity = {(event["pid"], event["image_path"], event["host_key"]) for event in events}
        if len(identity) != 1:
            errors.append("native process identity changed during the run")

    begins = [event for event in events if event["event"] == "run_begin"]
    ends = [event for event in events if event["event"] == "run_end"]
    if len(begins) > 1:
        errors.append(f"expected at most one run_begin, found {len(begins)}")
    if len(ends) > 1:
        errors.append(f"expected at most one run_end, found {len(ends)}")
    if events and events[0]["event"] != "run_begin":
        errors.append("run_begin is not the first native event")
    if ends and events[-1]["event"] != "run_end":
        errors.append("run_end is not the last native event")

    cases: dict[str, dict] = {}
    order: list[str] = []
    for event in events:
        if event["event"] == "assert" and event["status"] == "fail":
            errors.append(f"failed assertion: {event['case'] or event['message'] or 'unnamed'}")
        if event["event"] != "case_end":
            continue
        case_id = event["case"]
        if not case_id:
            errors.append("case_end has no case id")
            continue
        if case_id in cases:
            errors.append(f"duplicate case_end: {case_id}")
            continue
        cases[case_id] = {
            "status": event["status"],
            "message": event["message"],
            "duration_ms": event["duration_ms"],
        }
        order.append(case_id)
        if event["status"] != "pass":
            errors.append(f"case {case_id} status is {event['status']!r}")

    observed_server = None
    observed_channel = None
    selection_event_count = 0
    for event in events:
        if "server_id" not in event and "channel_id" not in event:
            continue
        invalid_selection_fields = []
        for field in ("server_id", "channel_id"):
            if field not in event:
                continue
            value = event[field]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                invalid_selection_fields.append(field)
        if invalid_selection_fields:
            errors.append(
                "invalid native server/channel observation fields: "
                + ", ".join(invalid_selection_fields)
            )
            continue
        selection_event_count += 1
        if "server_id" in event:
            observed_server = event["server_id"]
        if "channel_id" in event:
            observed_channel = event["channel_id"]

    missing_cases = [case_id for case_id in required_cases if case_id not in cases]
    if missing_cases:
        pending.append(f"normal flow is incomplete; missing cases: {', '.join(missing_cases)}")
    present_required = [case_id for case_id in order if case_id in required_cases]
    expected_present = [case_id for case_id in required_cases if case_id in cases]
    if present_required != expected_present:
        errors.append("normal-flow cases completed out of order")
    if not begins:
        pending.append("native run_begin has not been observed")
    if not ends:
        pending.append("native run_end has not been observed")
    elif ends[0]["status"] != "pass":
        errors.append(f"run_end status is {ends[0]['status']!r}")
    elif missing_cases:
        errors.append("native run_end claimed pass before the required normal flow completed")

    status = "fail" if errors else ("pending" if pending else "pass")
    if observed_server is not None and observed_channel is not None:
        selection_status = "observed"
    elif observed_server is not None or observed_channel is not None:
        selection_status = "partial"
    else:
        selection_status = "unobserved"
    return {
        "status": status,
        "event_count": len(events),
        "cases": cases,
        "required_cases": list(required_cases),
        "server_channel_selection": {
            "policy": "native_automatic",
            "source": "native_events",
            "status": selection_status,
            "observed_server_id": observed_server,
            "observed_channel_id": observed_channel,
            "observation_event_count": selection_event_count,
        },
        "errors": errors,
        "pending": pending,
        "path": str(path),
    }


def junit_report(report: dict) -> str:
    root = ET.Element("testsuites")
    native = report["layers"]["native"]
    suite = ET.SubElement(root, "testsuite", name="sdgo-gametest-normal-client")
    required_cases = tuple(native["required_cases"])
    failures = 0
    skipped = 0
    for case_id in required_cases:
        test = ET.SubElement(suite, "testcase", name=case_id, classname="client.normal-flow")
        case = native["cases"].get(case_id)
        if case is None:
            skipped += 1
            ET.SubElement(test, "skipped", message="normal flow has not reached this case")
        elif case["status"] != "pass":
            failures += 1
            message = case.get("message") or f"status={case['status']!r}"
            ET.SubElement(test, "failure", message=message).text = message
        elif isinstance(case.get("duration_ms"), (int, float)):
            test.set("time", f"{case['duration_ms'] / 1000.0:.6f}")
    extra_tests = 0
    if report.get("status") == "fail" and failures == 0:
        messages = []
        for layer_name in ("source", "launch", "native"):
            layer = report["layers"].get(layer_name, {})
            for error in layer.get("errors", []):
                messages.append(f"{layer_name}: {error}")
        message = "; ".join(messages) or "GameTest failed outside an individual native case"
        test = ET.SubElement(
            suite,
            "testcase",
            name="framework.infrastructure",
            classname="client.infrastructure",
        )
        ET.SubElement(test, "failure", message=message).text = message
        failures += 1
        extra_tests = 1
    suite.set("tests", str(len(required_cases) + extra_tests))
    suite.set("failures", str(failures))
    suite.set("errors", "0")
    suite.set("skipped", str(skipped))
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True)


def build_report(run_dir: Path) -> dict:
    run_dir = run_dir.resolve()
    metadata = validate_metadata(run_dir)
    source_errors = []
    try:
        verify_hashes(metadata)
    except GameTestError as error:
        source_errors.append(str(error))
    execution = metadata.get("execution")
    launch_errors = []
    launch_pending = []
    if execution is None:
        launch_pending.append("normal copied client has not been launched")
    elif execution.get("status") in {"launch_error", "launch_timeout"}:
        launch_errors.append(execution.get("error") or execution["status"])
    elif execution.get("game_pid") is None or Path(execution.get("game_image_path", "")).resolve() != Path(
        metadata["client"]["game"]
    ).resolve():
        launch_errors.append("execution is not bound to the copied GOnline.exe")
    else:
        execution_status = execution.get("status")
        if execution_status == "running":
            launch_pending.append("normal copied client is still running")
        elif execution_status != "completed":
            launch_errors.append(
                execution.get("error")
                or {
                    "game_exited": "normal copied client exited before GameTest completed",
                    "timeout": "normal copied client timed out before GameTest completed",
                    "cleanup_error": 'owned runtime cleanup failed: '+'; '.join(execution.get('cleanup_errors',[])),
                }.get(execution_status, f"unexpected execution status: {execution_status!r}")
            )
            if execution_status=='game_exited':
                native_log=Path(metadata['client']['root'])/'sdgo/logs/sdgo.log'
                if native_log.is_file():
                    faults=[line for line in native_log.read_text(encoding='utf-8',errors='replace').splitlines()
                            if 'game_test_client: native exception ' in line]
                    if faults:launch_errors.append(faults[-1])
    launch_status = "fail" if launch_errors else ("pending" if launch_pending else "pass")
    if (execution is not None and execution.get('status')=='completed' and
        "fps_cap" not in metadata["plan"] and
        metadata['plan'].get('network_policy') in ('direct', ROUTED_LOOPBACK_POLICY)):
        proof=execution.get('network_verified',{})
        policy=metadata['plan'].get('network_policy')
        expected_ip=(metadata['plan']['physical_login_ipv4'] if policy==ROUTED_LOOPBACK_POLICY
                     else metadata['plan']['target_host'])
        expected_port=(metadata['plan']['physical_login_port'] if policy==ROUTED_LOOPBACK_POLICY
                       else metadata['plan']['login_port'])
        if (proof.get('verified') is not True or proof.get('pid')!=execution.get('game_pid') or
            proof.get('remote_ipv4')!=expected_ip or proof.get('remote_port')!=expected_port):
            launch_errors.append('the actual native login TCP peer was not verified against the requested endpoint')
            launch_status='fail'
        elif policy==ROUTED_LOOPBACK_POLICY and (
            proof.get('network_policy')!=ROUTED_LOOPBACK_POLICY or
            proof.get('logical_ipv4')!=metadata['plan']['target_host'] or
            proof.get('logical_port')!=metadata['plan']['login_port'] or
            proof.get('physical_ipv4')!=expected_ip or
            proof.get('physical_port')!=expected_port or
            proof.get('route_profile_sha256')!=metadata['plan']['route_profile_sha256']
        ):
            launch_errors.append('routed login evidence does not preserve the sealed logical and physical endpoints')
            launch_status='fail'
    if execution is not None and execution.get('status')=='completed' and metadata['plan'].get('quiet'):
        proof=execution.get('quiet_verified',{})
        cleanup=execution.get('quiet_cleanup',{})
        if (proof.get('verified') is not True or proof.get('foreground_safe') is not True or
            proof.get('pid')!=execution.get('game_pid') or proof.get('audio_muted') is not True or
            proof.get('firewall_verified') is not True or cleanup.get('status')!='closed' or
            cleanup.get('cleanup',{}).get('status')!='removed'):
            launch_errors.append('quiet desktop, muted audio, scoped firewall and cleanup evidence are incomplete')
            launch_status='fail'

    required_cases = required_plan_cases(metadata["plan"])
    native = evaluate_events(
        Path(metadata["client"]["events"]),
        metadata["run_id"],
        execution.get("game_pid") if execution else None,
        Path(metadata["client"]["game"]),
        required_cases,
    )
    if 'play_unit_uid' in metadata['plan']:
        native['requested_play_unit_uid'] = metadata['plan']['play_unit_uid']
    if 'mission_mode' in metadata['plan']:
        native['mission_mode'] = metadata['plan']['mission_mode']
        native['mission_scope'] = metadata['plan']['mission_scope']
        native['mission_claims'] = {
            'clear': False,
            'result': False,
            'active_exit': False,
        }
    if (
        execution is not None
        and execution.get("status") == "completed"
        and native["status"] == "pending"
    ):
        launch_errors.append("execution completed without the required native GameTest evidence")
        launch_status = "fail"
    report = {
        "schema": SCHEMA_VERSION,
        "run_id": metadata["run_id"],
        "suite": metadata["suite"],
        "generated_utc": utc_now(),
        "layers": {
            "source": {
                "status": "bound" if not source_errors else "fail",
                "source_ref": metadata.get("source"),
                "errors": source_errors,
            },
            "launch": {
                "status": launch_status,
                "errors": launch_errors,
                "pending": launch_pending,
                "execution": execution,
                "stdout": metadata["client"]["stdout"],
                "stderr": metadata["client"]["stderr"],
            },
            "native": native,
        },
    }
    statuses = (report["layers"]["source"]["status"], launch_status, native["status"])
    report["status"] = (
        "fail"
        if "fail" in statuses
        else ("pass" if all(value in {"bound", "pass"} for value in statuses) else "pending")
    )
    receipt_dir = run_dir / "receipts"
    receipt_dir.mkdir(exist_ok=True)
    atomic_write(receipt_dir / "report.json", json.dumps(report, indent=2, sort_keys=True))
    atomic_write(receipt_dir / "junit.xml", junit_report(report))
    return report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)

    prepare_parser = subcommands.add_parser("prepare", help="copy a normal client and add GameTest")
    prepare_parser.add_argument("--client-template", type=Path, required=True)
    prepare_parser.add_argument(
        "--stage",
        type=Path,
        required=True,
        help="built client stage; overlays the observer and a staged production match_ai.dll when present",
    )
    prepare_parser.add_argument("--run-dir", type=Path, required=True)
    prepare_parser.add_argument("--target-host", default="127.0.0.1")
    prepare_parser.add_argument("--login-port", type=int, default=5001)
    prepare_parser.add_argument(
        "--target-server",
        default="1",
        help=(
            "deprecated compatibility value, recorded only; the normal client uses its "
            "native automatic server selection"
        ),
    )
    prepare_parser.add_argument(
        "--target-channel",
        default="1",
        help=(
            "deprecated compatibility value, recorded only; the normal client uses its "
            "native automatic channel selection"
        ),
    )
    battle_scope = prepare_parser.add_mutually_exclusive_group()
    battle_scope.add_argument("--mode-sequence", default="1")
    battle_scope.add_argument(
        "--mission-mode",
        type=int,
        choices=SUPPORTED_MISSION_MODES,
        default=None,
        help=(
            "bounded native mission smoke for mode 5 or 8: real room, start, "
            "AI spawn and combat only; no clear, result, or active-exit claim"
        ),
    )
    prepare_parser.add_argument("--retry-count", type=int, choices=(1, 2))
    prepare_parser.add_argument("--expected-ai", type=int, default=3)
    prepare_parser.add_argument('--play-unit-uid',type=int,
                                help='select an existing genuinely owned non-zero uint64 unit UID through native hangar UI before PvP, pages, or mission tests')
    prepare_parser.add_argument("--sfx-only", action="store_true",
                                help="mute BGM and enable effects only in the isolated client")
    prepare_parser.add_argument('--bootstrap-only', action='store_true',
                                help='test real login and onboarding only; does not claim PvP acceptance')
    prepare_parser.add_argument('--battleship-only', action='store_true',help='acquire the initial battleships through native tutorial business')
    prepare_parser.add_argument('--gift-requests-only', action='store_true',help='send configured native mail gift requests; server processing is verified separately')
    prepare_parser.add_argument('--deck-only', action='store_true',help='save and select a native battleship deck using four genuinely owned unit UIDs from the private account profile')
    prepare_parser.add_argument('--pages-only', action='store_true',help='visit the real hangar and verify Mod inventory, preview and cancel without exchanging units')
    prepare_parser.add_argument('--friends-only',action='store_true',help='run the real multi-client native friend session')
    prepare_parser.add_argument('--collection-draw',action='store_true',help='explicit pages-only extension: purchase one collection draw with existing native funds and verify delivery')
    prepare_parser.add_argument('--shop-assertions',type=Path,
                                help='read-only StockAssertions INI copied into a pages-only GameTest capsule')
    prepare_parser.add_argument('--shop-display',type=Path,
                                help='matching read-only runtime display INI copied into a pages-only GameTest capsule')
    prepare_parser.add_argument('--random-loadout',action='store_true',help='use the native !rand room command with the actually owned inventory in the PvP scenario')
    prepare_parser.add_argument('--network-policy',choices=('direct','preserve',ROUTED_LOOPBACK_POLICY),default='direct',
                                help='direct disables inherited routes; routed_loopback installs a sealed 127/8 profile in the copy; preserve tests owner routes')
    prepare_parser.add_argument('--route-profile',type=Path,
                                help='strict net_route INI used only with --network-policy routed_loopback')
    prepare_parser.add_argument('--visible',action='store_true',help='explicit developer opt-in to a visible client; default tests use an isolated desktop and muted audio')
    prepare_parser.add_argument('--asset-baseline',type=Path,help='reuse verified read-only native archives from a passed owned build run; never links original player files')
    prepare_parser.add_argument('--reuse-client',action='store_true',help='reuse the stopped owned run-dir client in place with a fresh run UUID and compact prior receipt; no full client copy')
    prepare_parser.add_argument('--include-mod',action='append',default=[],help='explicit staged client Mod to install before runtime hashes freeze')
    prepare_parser.add_argument(
        "--fps-cap",
        type=int,
        help="prepare the unauthenticated native FPS cap/cadence suite at 1..1000 FPS",
    )
    prepare_parser.add_argument(
        "--fps-mod-disabled",
        action="store_true",
        help="for --fps-cap, disable fps_limit and verify the native 60 FPS baseline",
    )
    prepare_parser.add_argument(
        "--expect-first-fail",
        action=argparse.BooleanOptionalAction,
        default=None,
    )
    prepare_parser.add_argument(
        "--account-profile",
        type=Path,
        help=r"persistent account profile; default %%LOCALAPPDATA%%\SDGO-GameTest\account.ini",
    )
    prepare_parser.add_argument(
        "--create-test-account",
        action="store_true",
        help="explicit one-time authorization to create a missing persistent test account profile",
    )
    prepare_parser.add_argument(
        "--launcher-path",
        help="optional path inside the copied client; default starts GOnline.exe directly",
    )

    run_parser = subcommands.add_parser("run", help="run the copied normal client")
    run_parser.add_argument("--run-dir", type=Path, required=True)
    run_parser.add_argument("--timeout", type=float, default=600.0)
    run_parser.add_argument("--launch-timeout", type=float, default=30.0)

    report_parser = subcommands.add_parser("report", help="rebuild receipts from native evidence")
    report_parser.add_argument("--run-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    effective = sys.argv[1:] if argv is None else argv
    if effective and effective[0] == 'environment':
        if str(Path(__file__).resolve().parents[1]) not in sys.path:
            sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
        from tools.game_test_environment import main as environment_main
        return environment_main(effective[1:])
    args = parse_args(argv)
    try:
        if args.command == "prepare":
            preparation = reuse_prepared if args.reuse_client else prepare
            result = preparation(
                args.client_template,
                args.stage,
                args.run_dir,
                target_host=args.target_host,
                login_port=args.login_port,
                target_server=args.target_server,
                target_channel=args.target_channel,
                mode_sequence=args.mode_sequence,
                retry_count=args.retry_count,
                expect_first_fail=args.expect_first_fail,
                expected_ai=args.expected_ai,
                play_unit_uid=args.play_unit_uid,
                mission_mode=args.mission_mode,
                launcher_path=args.launcher_path,
                account_profile=args.account_profile,
                create_test_account=args.create_test_account,
                sfx_only=args.sfx_only,
                bootstrap_only=args.bootstrap_only,
                battleship_only=args.battleship_only,
                gift_requests_only=args.gift_requests_only,
                deck_only=args.deck_only,
                pages_only=args.pages_only,
                friends_only=args.friends_only,
                collection_draw=args.collection_draw,
                shop_assertions=args.shop_assertions,
                shop_display=args.shop_display,
                random_loadout=args.random_loadout,
                network_policy=args.network_policy,
                route_profile=args.route_profile,
                quiet=not args.visible,
                asset_baseline=args.asset_baseline,
                fps_cap=args.fps_cap,
                fps_mod_enabled=not args.fps_mod_disabled,
                extra_mods=tuple(args.include_mod),
            )
        elif args.command == "run":
            result = run_prepared(args.run_dir, args.timeout, args.launch_timeout)
        else:
            result = build_report(args.run_dir)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if args.command == "prepare" or result.get("status") == "pass" else 1
    except GameTestError as error:
        print(f"game_test: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
