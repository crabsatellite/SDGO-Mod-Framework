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


# Direct CLI entry points start with tools/ on sys.path. Candidate overlays
# also import repository packages, independently of the caller's directory.
if str(Path(__file__).resolve().parents[1]) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import game_test_unit_scenario as unit_scenarios
from tools import game_test_capsule_package as capsule_packages
from tools import game_test_operator_scenario as operator_scenarios
SCHEMA_VERSION = 2
OWNED_BUILD_ROOT=Path(__file__).resolve().parents[1]/'build'
OWNED_PROCESS_EXIT_TIMEOUT = 30
if str(OWNED_BUILD_ROOT.parent) not in sys.path:
    sys.path.insert(0, str(OWNED_BUILD_ROOT.parent))
HOST_KEY = "GOnline-54BE1568-00C97000"
GAME_EXE = "GOnline.exe"
STARTUP_EVENT_NAME = "9YOU SDGO"
TEST_MOD = "game_test_client"
TARGET_MOD = "match_ai"
BATTLE_AI_MOD = "battle_ai"
MISSION_BEHAVIOR_MOD = "mission_behavior"
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
OPERATOR_PLAN_FIELDS = ("operator_flow", *operator_scenarios.PATH_FIELDS)
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
# Room-system switching: one client plays an OC room, a G改 1.0 room and a
# G改 2.0 room back to back. The G改 2.0 round uses the plain title (the default
# system); each round checks the system the client and Battle applied.
ROOM_SWITCH_SYSTEMS = ("oc", "g1", "g2")
ROOM_SWITCH_SUFFIXES = {"oc": " !oc", "g1": " !g1", "g2": ""}
ROOM_SWITCH_STEPS = ("room", "start", "ai_spawn", "system", "return")
# OC special (--oc-special): the native player probe settings sealed into the
# copied observer. BattlePlayerSpecial itself is always written (0 otherwise).
OC_SPECIAL_EXPECTED_AI = 7
OC_SPECIAL_OBSERVER_SETTINGS = {
    "BattlePlayerCombat": "1",
    "BattlePlayerCombatContinuous": "1",
    "BattlePlayerSecondary": "1",
    "BattlePlayerWeaponSlot": "-1",
}


def room_switch_titles(base: str) -> tuple[str, ...]:
    """The sealed per-round room titles of a room-switch run, in play order."""
    return tuple(base + ROOM_SWITCH_SUFFIXES[system] for system in ROOM_SWITCH_SYSTEMS)
MISSION_SUITE = "native-mission-smoke"
SUPPORTED_MISSION_MODES = (5, 8)
MISSION_FLOW_CASES = (
    "mission.room",
    "mission.start",
    "mission.ai_spawn",
    "mission.combat",
)
MISSION_SCOPE = "bounded_native_smoke"
MISSION_BEHAVIOR_SUITE = "native-mission-behavior"
MISSION_BEHAVIOR_SCOPE = "sealed_profile_behavior"
MISSION_BEHAVIOR_PROFILES = ("oc", "gco")
MISSION_BEHAVIOR_TARGETS = {
    88006: 5,  # M1 Easy: client Boss AI binding and native map-special behavior
    88009: 5,  # M2 Easy: server mission table behavior
}
MISSION_BEHAVIOR_CASES = MISSION_FLOW_CASES + (
    "mission.behavior",
)
MISSION_BEHAVIOR_FIELDS = (
    "mission_behavior_profile",
    "mission_game_mode_id",
)
MISSION_AI_COUNT = 3
MISSION_AI_LEVEL = 2
MISSION_EXISTING_ACCOUNT_STATES = frozenset(("registered", "existing_account"))
MISSION_CLAIM_FIELDS = (
    "mission_clear_claim",
    "mission_result_claim",
    "mission_active_exit_claim",
)
PAGE_GROUPS = ('hangar', 'preview_guard', 'storage_gift')
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
RUNTIME_OUTPUTS = frozenset({"sdgo/mods/capsule_catalog/runtime-receipt.ini"})
NATIVE_BATTLE_AI_POLICY_ERROR = "battle_ai: Lua reload rejected; previous policy retained:"
ALNUM = string.ascii_letters + string.digits


class GameTestError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        temporary.write_text(data, encoding="utf-8", newline="\n")
        for attempt in range(20):
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                if attempt == 19: raise
                time.sleep(.05)
    finally:
        temporary.unlink(missing_ok=True)


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
            or (parent == "sdgo" and lowered in {"gametest", "local-state"})
        ):
            ignored.add(name)
    return ignored


def git_source_ref(root: Path) -> dict:
    result = {"commit": None, "dirty": None}
    # Git is read-only here. Bound its scheduling priority as well as its
    # timeout on busy native-test hosts, where child processes otherwise start
    # at normal priority even when the runner itself has been prioritized.
    process_options = {}
    if os.name == "nt":
        process_options["creationflags"] = subprocess.ABOVE_NORMAL_PRIORITY_CLASS
    try:
        result["commit"] = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
            **process_options,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return result
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
            **process_options,
        ).stdout
        result["dirty"] = bool(status.strip())
    except (OSError, subprocess.SubprocessError):
        pass  # preserve the verified commit; unknown cleanliness stays unknown
    return result


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


def replace_ini(temporary: Path, path: Path, timeout: float = 5.0) -> None:
    deadline=time.monotonic()+timeout
    while True:
        try:
            os.replace(temporary,path)
            return
        except PermissionError as error:
            if getattr(error,'winerror',None) not in (5,32) or time.monotonic()>=deadline:
                raise
            time.sleep(min(.1,max(0,deadline-time.monotonic())))


def write_ini(path: Path, parser: configparser.ConfigParser) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        parser.write(stream, space_around_delimiters=False)
    replace_ini(temporary, path)


def copy_file_if_missing(source: Path, destination: Path) -> None:
    if destination.is_file():
        return
    if not source.is_file():
        raise GameTestError(f"normal client lacks {destination.name} and stage lacks fallback: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def manifest_configuration(path: Path) -> configparser.ConfigParser:
    try:
        parser=read_ini(path)
    except GameTestError:
        raw=path.read_bytes()
        decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
        match=re.search(r'(?ims)^\s*\[mod\]\s*\n(.*?)(?=^\s*\[|\Z)',decoded)
        if not match:raise GameTestError(f'manifest lacks [mod]: {path}') from None
        parser=ini_parser();parser.read_string('[mod]\n'+match[1])
    return parser


def manifest_mod(path: Path):
    parser=manifest_configuration(path)
    if not parser.has_section('mod'):raise GameTestError(f'manifest lacks [mod]: {path}')
    return parser['mod']


def retained_manifest_enabled(path: Path) -> bool:
    return manifest_mod(path).getint('enabled',fallback=1)!=0


def manifest_enabled(path: Path) -> bool:
    return retained_manifest_enabled(path)


def manifest_requires(path: Path) -> set[str]:
    return {item.strip() for item in manifest_mod(path).get('requires','').split(',') if item.strip()}


def shared_manifest_requires(manifest: Path, source) -> set[str]:
    names = manifest_requires(manifest)
    fragment = manifest.parent / 'symbols.fragment.ini'
    if fragment.is_file():
        local = read_ini(fragment)
        try:
            matching_host = (local['host']['image'].casefold() == source['host']['image'].casefold() and
                all(int(local['host'][key], 0) == int(source['host'][key], 0)
                    for key in ('timestamp', 'size_of_image')))
        except (KeyError, ValueError):
            matching_host = False
        if not matching_host:
            raise GameTestError('Mod symbol fragment does not match the selected host: ' + manifest.parent.name)
        # Core verifies this Mod's sidecar. Its aliases stay local and cannot
        # satisfy another Mod's dependencies.
        names -= set(local.sections()) - {'host'}
    return names


def merge_required_symbols(
    stage_symbols: Path,
    copied_symbols: Path,
    manifests: list[Path],
    *,
    preserve_existing: bool = False,
    allow_missing: bool = False,
    include_all: bool = False,
) -> None:

    source = read_ini(stage_symbols)
    destination = read_ini(copied_symbols) if copied_symbols.is_file() else ini_parser()
    required = {"host"}
    if include_all:
        # Multi-host Mods resolve their host-specific aliases dynamically.
        # Keep the complete matching staged map while preserving local aliases.
        required.update(source.sections())
    for manifest in manifests:
        required.update(shared_manifest_requires(manifest, source))
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
    disabled_mods: tuple[str, ...] = (),
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
    for identifier in disabled_mods:
        if not re.fullmatch(r'[a-z][a-z0-9_]{0,63}', identifier):
            raise GameTestError('invalid explicitly disabled test Mod ID')
        if identifier in (TEST_MOD,TARGET_MOD) or identifier in extra_mods:
            raise GameTestError('test Mod cannot be both required and explicitly disabled: '+identifier)
        manifest=root/'sdgo/mods'/identifier/'mod.ini'
        if not manifest.is_file() or read_ini(manifest).get('mod','id',fallback='')!=identifier:
            raise GameTestError('explicitly disabled test Mod is not installed: '+identifier)
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
    for manifest in updated.values():required.update(shared_manifest_requires(manifest, source))
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
    disabled_mods: tuple[str, ...] = (),
    *,
    fps_mod_enabled: bool | None = None,
    bind_staged_loader: bool = False,
    skip_staged_mods: tuple[str, ...] = (),
) -> None:
    validate_observer_stage(
        root,stage,extra_mods,disabled_mods,
        fps_mod_enabled=fps_mod_enabled)
    stage_client = stage / "client"
    observer_source = stage_client / "sdgo" / "mods" / TEST_MOD
    if not (observer_source / f"{TEST_MOD}.dll").is_file() or not (observer_source / "mod.ini").is_file():
        raise GameTestError(f"stage lacks native observer Mod: {observer_source}")

    staged_loader = stage_client / "d3d9.dll"
    if bind_staged_loader:
        if not staged_loader.is_file():
            raise GameTestError(f"stage lacks framework loader: {staged_loader}")
        shutil.copy2(staged_loader, root / "d3d9.dll")
    else:
        copy_file_if_missing(staged_loader, root / "d3d9.dll")
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
            sidecar=staged_target_dll.with_name('symbols.fragment.ini')
            if sidecar.is_file():shutil.copy2(sidecar,target_dll.with_name(sidecar.name))

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
        if identifier in skip_staged_mods:continue
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
            if payload.is_file() and (payload.suffix.lower() in ('.dll','.lua') or payload.name=='symbols.fragment.ini'):
                shutil.copy2(payload,destination/payload.name)
    if disabled_mods:
        settings=read_ini(root/'sdgo/sdgo.ini')
        if not settings.has_section('mods'):settings.add_section('mods')
        for identifier in disabled_mods:
            manifest=root/'sdgo/mods'/identifier/'mod.ini'
            current=read_ini(manifest)
            current['mod']['enabled']='0'
            write_ini(manifest,current)
            settings['mods'][identifier]='0'
        write_ini(root/'sdgo/sdgo.ini',settings)

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


REVIEW_FOOTAGE_SCREEN_RES = 12   # the native 1920x1080 battle resolution
# Quiet sessions that play several battles keep the hidden device's battle
# resets small: the template's desktop-sized level resets a 2389x1344 back
# buffer at every battle start, and the second such reset can stall the
# driver's TestCooperativeLevel on the graphics worker.
SEVERAL_BATTLES_SCREEN_RES = 0


def configure_muted_audio(root: Path, *, review_footage: bool = False, battle_screen_res: int | None = None) -> dict:
    path=root/'config.xml'
    try:
        tree=ET.parse(path)
        video=tree.getroot().find('Video')
        if video is None:video=ET.SubElement(tree.getroot(),'Video')
        screen=video.find('ScreenMode')
        if screen is None:screen=ET.SubElement(video,'ScreenMode',{'width':'1024','height':'768','bpp':'32'})
        screen.set('mode','window');screen.set('windowFull','0')
        if review_footage or battle_screen_res is not None:
            # Review footage keeps the template's post-processing and plays battles at 1920x1080.
            resolution=video.find('GameScreenRes')
            if resolution is None:resolution=ET.SubElement(video,'GameScreenRes')
            resolution.set('level',str(REVIEW_FOOTAGE_SCREEN_RES if review_footage else battle_screen_res))
        if not review_footage:
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


def validate_muted_audio(root: Path, *, review_footage: bool = False) -> None:
    try:
        document=ET.parse(root/'config.xml').getroot()
        screen=document.find('Video/ScreenMode')
        if screen is None or screen.get('mode')!='window' or screen.get('windowFull')!='0':
            raise ValueError('background tests must use a windowed device and may not change the display mode')
        if not review_footage and any(document.find('Video/PostEffect/'+key) is None or
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


def allow_named_desktop_fallback(metadata: dict) -> bool:
    if metadata.get('suite') == FPS_SUITE:
        return True
    plan = metadata.get('plan', {})
    supported_session=(metadata.get('suite')=='pvp-ai-normal-flow' or
                       (metadata.get('suite')=='native-friends' and plan.get('friends_only') is True))
    if (not supported_session or
        plan.get('quiet') is not True or plan.get('network_policy') != 'direct'):
        return False
    try:
        return ipaddress.IPv4Address(plan.get('target_host', '')).is_loopback
    except ipaddress.AddressValueError:
        return False


def stop_owned_entry(process) -> bool:
    if process is None or process.poll() is not None:
        return False
    try:
        process.kill()
        process.wait(timeout=10)
        return False
    except (TimeoutError, subprocess.TimeoutExpired):
        # Termination can be asynchronous while native COM/graphics threads
        # finish. A second bounded wait accepts only a signaled process.
        process.wait(timeout=30)
        if process.poll() is None:
            raise
        return True
    except OSError:
        process.wait(timeout=10)
        if process.poll() is None:
            raise
        return True


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
        validate_muted_audio(Path(self.metadata['client']['root']),
                             review_footage=bool(self.metadata['plan'].get('review_footage')))
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
            allow_named_desktop_fallback=allow_named_desktop_fallback(self.metadata))
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
            not allow_named_desktop_fallback(self.metadata)):
            raise GameTestError('named-desktop fallback requires FPS, local direct PvP, or a local direct coordinated session')
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
        proof.update(audio_driver='native_miles_waveout',audio_drivers_opened=opened,audio_drivers_muted=opened,
                     native_audio_open_failures=guard.getint('audio','open_failed',fallback=0))
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
        {"Schema", "Scope", "CatalogId", "Revision"} | ({'Query'} if assertions.get('shop','Scope',fallback='') == 'StockVisualAssertions' and assertions.has_option('shop','Query') else set()),
        "shop assertions",
    )
    visual_only = shop["Scope"] == "StockVisualAssertions"
    if 'Query' in shop and (not shop['Query'] or len(shop['Query'].encode('utf-16le'))>72 or any(c in shop['Query'] for c in '\r\n\0')):
        raise GameTestError('shop visual query must be bounded native input text')
    if shop["Schema"] != "1" or shop["Scope"] not in ("StockAssertions", "StockVisualAssertions"):
        raise GameTestError("shop assertions require [shop] Schema=1 and Scope=StockAssertions")
    _shop_key("shop CatalogId", shop["CatalogId"])
    revision = _shop_key("shop Revision", shop["Revision"])

    product_keys = {
        "ProductId", "ItemId", "TemplateId", "Category", "Price",
        "Currency", "Validity", "Enabled", "Name", "Label",
    }
    if visual_only:
        product_keys.add("DisplayName")
    products = 0
    product_ids: set[int] = set()
    item_ids: set[int] = set()
    for section in assertions.sections():
        if section == "shop":
            continue
        if not section.startswith("product:"):
            raise GameTestError("shop assertions contain an unsupported section")
        _shop_key("shop product key", section[len("product:"):])
        tooltip_keys={'TooltipDescriptionHex','TooltipUsageHex','TooltipUnit'}
        present_tooltip={k for k in tooltip_keys if assertions.has_option(section,k)}
        if present_tooltip and (not visual_only or present_tooltip!=tooltip_keys):
            raise GameTestError('tooltip visual assertions require the complete sealed visual scope')
        art_keys={k for k in ('ArtRankLabel','PreviewName','CaptureHover') if visual_only and assertions.has_option(section,k)}
        product = _exact_ini_keys(assertions, section, product_keys | present_tooltip | art_keys | ({'Query'} if visual_only and assertions.has_option(section,'Query') else set()), "shop assertions")
        if 'CaptureHover' in product and (product['CaptureHover'] not in ('0','1') or (product['CaptureHover']=='0' and present_tooltip)):
            raise GameTestError('hover expectations require native hover capture')
        if 'PreviewName' in product:
            value=product['PreviewName']
            if not value or len(value.encode('utf-16le'))>100 or any(c in value for c in '\r\n\0'):
                raise GameTestError('shop preview name must be bounded display text')
        if 'ArtRankLabel' in product:
            value=product['ArtRankLabel']
            if len(value.encode('utf-16le'))>64 or any(c in value for c in '\r\n\0'):
                raise GameTestError('shop artwork label must be bounded display text')
        if present_tooltip:
            if product['TooltipUnit'] not in ('0','1'):
                raise GameTestError('tooltip kind must be native unit or item')
            for key in ('TooltipDescriptionHex','TooltipUsageHex'):
                raw=product[key]
                if len(raw)>16384 or len(raw)%4 or not re.fullmatch('[0-9a-fA-F]*',raw):
                    raise GameTestError('tooltip expected text must be bounded UTF-16LE hex')
                try: decoded=bytes.fromhex(raw).decode('utf-16le')
                except UnicodeDecodeError as error: raise GameTestError('invalid tooltip expected text') from error
                if '\0' in decoded: raise GameTestError('tooltip expected text cannot contain NUL')
        if 'Query' in product and (not product['Query'] or len(product['Query'].encode('utf-16le'))>72 or any(c in product['Query'] for c in '\r\n\0')):
            raise GameTestError('product visual query must be bounded native input text')
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
        for field in (("Name", "Label", "DisplayName") if visual_only else ("Name", "Label")):
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

    result = {
        "shop_assertions_path": SHOP_ASSERTIONS_JOB_PATH,
        "shop_assertions_sha256": sha256(assertions_path),
        "shop_display_path": SHOP_DISPLAY_JOB_PATH,
        "shop_display_sha256": sha256(display_path),
        "shop_revision": revision,
    }
    if visual_only:
        result['shop_visual_only'] = True
    return result


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
    for relative in (SHOP_ASSERTIONS_RELATIVE, SHOP_DISPLAY_RELATIVE,
                     'sdgo/gametest/'+unit_scenarios.INPUT):
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
    if type(plan.get('shop_visual_only', False)) is not bool or plan.get('shop_visual_only', False) != installed.get('shop_visual_only', False):
        raise GameTestError('shop visual scope disagrees with sealed assertion inputs')
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


def validate_oc_allocate(value: str | None) -> str | None:
    """OC acceptance input: '<stat>:<points>[,<stat>:<points>...]' with stat 0..5 and 1..20 normal points."""
    if value is None:
        return None
    item = r'[0-5]:(?:[1-9]|1[0-9]|20)'
    if type(value) is not str or not re.fullmatch(item + r'(?:,' + item + r'){0,5}', value):
        raise GameTestError("oc_allocate must be '<stat 0-5>:<points 1-20>[,...]'")
    return value


def oc_allocated_stats(oc_allocate: str | None) -> set[str]:
    return {entry.split(':')[0] for entry in oc_allocate.split(',')} if oc_allocate is not None else set()


def oc_mobility_selected(oc_allocate: str | None) -> bool:
    """The battle checks the OC speed/agility gains when speed (0) or agility (4) is allocated."""
    return bool(oc_allocated_stats(oc_allocate) & {'0', '4'})


def oc_stats_selected(oc_allocate: str | None, oc_special: bool = False) -> bool:
    """The battle checks the local battle stat block when attack (2) or defense (3) is allocated, and
    observes it in the OC special battle."""
    return oc_allocate is not None and (oc_special or bool(oc_allocated_stats(oc_allocate) & {'2', '3'}))


def validate_oc_special(oc_special: bool, oc_allocate: str | None, *, room_switch: bool = False) -> bool:
    """OC special: one ordinary PvP battle in which the native player probe fires the play unit's
    special. The allocation must save special points (stat 5)."""
    if type(oc_special) is not bool:
        raise GameTestError('oc_special must be a boolean')
    if oc_special and (oc_allocate is None or '5' not in oc_allocated_stats(oc_allocate) or room_switch):
        raise GameTestError("oc_special fires the play unit's special in the --oc-allocate PvP battle and needs "
                            'special (stat 5) points; it is not combined with room_switch')
    return oc_special


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


def validate_mission_behavior(
    profile: str | None,
    mission_game_mode_id: int | None,
    mission_mode: int | None,
) -> tuple[str | None, int | None]:
    if profile is None and mission_game_mode_id is None:
        return None, None
    if (profile is None and type(mission_game_mode_id) is int and mission_game_mode_id > 0
            and mission_game_mode_id not in MISSION_BEHAVIOR_TARGETS):
        # Any other listed mission (e.g. one registered by the map import protocol): the observer
        # selects the exact GameModeID in the real create dialog; room mode follows the native rule.
        # Sealed behavior targets still need their profile.
        if mission_mode != (5 if mission_game_mode_id <= 0x313990 else 8):
            raise GameTestError("mission_game_mode_id does not belong to the requested mission_mode")
        return None, mission_game_mode_id
    if profile not in MISSION_BEHAVIOR_PROFILES:
        raise GameTestError("mission_behavior_profile must be one of: oc,gco")
    if (
        isinstance(mission_game_mode_id, bool)
        or not isinstance(mission_game_mode_id, int)
        or mission_game_mode_id not in MISSION_BEHAVIOR_TARGETS
    ):
        supported = ",".join(str(value) for value in MISSION_BEHAVIOR_TARGETS)
        raise GameTestError(
            "mission_game_mode_id must be one of the sealed native targets: "
            + supported
        )
    if mission_mode != MISSION_BEHAVIOR_TARGETS[mission_game_mode_id]:
        raise GameTestError(
            "mission_game_mode_id does not belong to the requested mission_mode"
        )
    return profile, mission_game_mode_id


def configure_mission_behavior_client(
    client: Path,
    profile: str,
) -> dict[str, str | int]:
    """Seal the selected production Mod profile without editing its manifest."""
    if profile not in MISSION_BEHAVIOR_PROFILES:
        raise GameTestError("mission behavior profile must be oc or gco")
    behavior_directory = client / "sdgo" / "mods" / MISSION_BEHAVIOR_MOD
    behavior_manifest = behavior_directory / "mod.ini"
    manifest = read_ini(behavior_manifest)
    if manifest.get("mod", "id", fallback="") != MISSION_BEHAVIOR_MOD:
        raise GameTestError("installed mission behavior Mod identity differs")

    behavior_config_path = behavior_directory / "config.ini"
    behavior_config = (
        read_ini(behavior_config_path)
        if behavior_config_path.is_file()
        else ini_parser()
    )
    if not behavior_config.has_section("settings"):
        behavior_config.add_section("settings")
    behavior_config["settings"]["Profile"] = profile

    profile_path = behavior_directory / "profile.bin"
    if profile == "oc" and not profile_path.is_file():
        raise GameTestError(
            "OC mission behavior GameTest requires the server owner's profile.bin"
        )
    write_ini(behavior_config_path, behavior_config)
    return {
        "profile": profile,
        "profile_data_present": int(profile_path.is_file()),
        "profile_data_sha256": sha256(profile_path) if profile_path.is_file() else "",
    }


def validate_ai_pool_level(value: int | None) -> int | None:
    if value is None:
        return None
    if type(value) is not int or not 1 <= value <= 5:
        raise GameTestError("ai_pool_level must be an integer between 1 and 5")
    return value


def validate_ai_difficulty(value: str | None) -> str | None:
    if value is None:
        return None
    if type(value) is not str or value not in ("Easy", "Medium", "Hard"):
        raise GameTestError("ai_difficulty must be one of: Easy, Medium, Hard")
    return value


def validate_ai_layout(value: str | None) -> str | None:
    if value is None:
        return None
    if type(value) is not str or value != "allied":
        raise GameTestError("ai_layout must be: allied")
    return value


def mission_room_title(
    title_nonce: str,
    respawns: str | None = None,
    difficulty: str | None = None,
) -> str:
    command = (
        f"!ai {difficulty.lower()}" if difficulty is not None
        else f"!ai{MISSION_AI_COUNT} lv{MISSION_AI_LEVEL}"
    )
    title = f"{title_nonce} {command}" + (' inf' if respawns == 'infinite' else '')
    if len(title.encode('ascii')) > 20:
        raise GameTestError('sealed mission AI title exceeds the native twenty-byte limit')
    return title


def required_flow_cases(mode_sequence: str, bootstrap_only: bool = False,
                        battleship_only: bool = False, gift_requests_only: bool = False,
                        deck_only: bool = False, pages_only: bool = False,
                        collection_draw: bool = False,
                         play_unit_uid: int | None = None,
                         mission_mode: int | None = None,
                         shop_catalog: bool = False, friends_only: bool = False, room_titles_only: bool = False,
                         room_style_preview: str | None = None,
                         same_room_rematch: bool = False, shop_visual_only: bool = False,
                         page_group: str | None = None, hangar_confirm_gb: bool = False,
                         hangar_sort: bool = False,
                         mission_respawns: str | None = None,
                         oc_allocate: str | None = None,
                         oc_room: bool = False,
                         g1_room: bool = False,
                         room_switch: bool = False,
                         mission_behavior_profile: str | None = None,
                         mission_game_mode_id: int | None = None,
                         oc_panel: bool = False, oc_challenge: bool = False,
                         oc_special: bool = False) -> tuple[str, ...]:
    if mission_respawns is not None and (mission_respawns not in ('player','infinite') or mission_mode is None):
        raise GameTestError('mission_respawns requires a native mission and player or infinite policy')
    oc_allocate = validate_oc_allocate(oc_allocate)
    oc_special = validate_oc_special(oc_special, oc_allocate, room_switch=room_switch is True)
    if type(room_switch) is not bool or (room_switch and (oc_allocate is None or oc_room or g1_room or same_room_rematch)):
        raise GameTestError('room_switch plays the --oc-allocate PvP battle once per room system (not with --oc-room or --g1-room)')
    if oc_allocate is not None and (play_unit_uid is None or mission_mode is not None or any((
            bootstrap_only, battleship_only, gift_requests_only, deck_only, pages_only, friends_only,
            room_titles_only, collection_draw, same_room_rematch, room_style_preview is not None))):
        raise GameTestError('oc_allocate requires an ordinary PvP battle with an existing owned play unit')
    if type(oc_panel) is not bool or type(oc_challenge) is not bool:
        raise GameTestError('oc_panel and oc_challenge must be booleans')
    if oc_challenge and not (oc_panel or oc_allocate is not None):
        raise GameTestError('oc_challenge runs inside the OC panel check')
    if oc_panel and (play_unit_uid is None or not (oc_allocate is not None or (pages_only and page_group == 'hangar'))):
        raise GameTestError('oc_panel runs after the OC PvP battle or the read-only hangar pages with an existing owned play unit')
    if page_group is not None and (page_group not in PAGE_GROUPS or not pages_only or shop_catalog or collection_draw):
        raise GameTestError('page_group requires bounded hangar pages without shop or collection scope')
    if type(hangar_confirm_gb) is not bool:
        raise GameTestError('hangar_confirm_gb must be a boolean')
    if hangar_confirm_gb and (page_group != 'hangar' or not pages_only or oc_panel):
        raise GameTestError('hangar_confirm_gb requires the sealed hangar page group without OC actions')
    mission_mode = validate_mission_mode(mission_mode)
    mission_behavior_profile, mission_game_mode_id = validate_mission_behavior(
        mission_behavior_profile, mission_game_mode_id, mission_mode
    )
    play_unit_uid = validate_play_unit_uid(play_unit_uid)
    if type(same_room_rematch) is not bool:raise GameTestError('same_room_rematch must be a boolean')
    if same_room_rematch and (mission_mode is not None or any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only,friends_only,collection_draw,shop_catalog,room_titles_only,room_style_preview))):
        raise GameTestError('same-room rematch requires a complete native PvP scenario')
    if sum((bool(bootstrap_only),bool(battleship_only),bool(gift_requests_only),bool(deck_only),bool(pages_only),bool(friends_only),bool(room_titles_only))) > 1:
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
    if shop_visual_only and not (shop_catalog and pages_only):
        raise GameTestError('shop visual scope requires sealed stock assertions and pages-only')
    if room_style_preview is not None:
        if room_style_preview not in ('pvp', 'mission') or not room_titles_only:
            raise GameTestError('room-style preview requires the room-title scope and pvp or mission')
        if mission_mode is not None or collection_draw or shop_catalog:
            raise GameTestError('room-style preview cannot contain combat or shop operations')
        selection = ('setup.active_unit',) if play_unit_uid is not None else ()
        return FLOW_CASES[:3] + selection + (
            f'room_titles.{room_style_preview}.create', f'room_style.{room_style_preview}.preview')
    if room_titles_only:
        if mission_mode is not None or collection_draw or shop_catalog:
            raise GameTestError('room-title input scope cannot contain mission combat or shop operations')
        selection=('setup.active_unit',) if play_unit_uid is not None else ()
        return FLOW_CASES[:3]+selection+tuple('room_titles.'+x for x in ('pvp.create','pvp.rename','pvp.leave','grid.create','grid.rename','grid.leave','mission.create','mission.rename','mission.leave','team.create','team.native_view','team.leave','start.create','start.rename','start'))
    selection = ('setup.active_unit',) if play_unit_uid is not None else ()
    if oc_allocate is not None:
        # Allocate first, then equip: the equip reply is the owned record every
        # native copy (profile, room pilot) is built from, so it must already
        # carry the new OC payload before the battle.
        # G改 1.0 rooms equip the play unit's G改 1.0 loadout once it is the
        # active unit, so the refreshed record carries it into the room.
        selection = ('oc.allocate',) + selection + (('g1.loadout',) if g1_room or room_switch else ()) + (
            ('oc.create',) if oc_room or g1_room or room_switch else ())
    if mission_mode is not None:
        if mode_sequence != "1":
            raise GameTestError("--mission-mode cannot be combined with a PvP mode sequence")
        if any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only,collection_draw)):
            raise GameTestError("--mission-mode cannot be combined with pages or account preparation scopes")
        if mission_behavior_profile is not None:
            if mission_respawns is not None:
                raise GameTestError(
                    "mission behavior scenarios cannot add the respawn fixture"
                )
            return FLOW_CASES[:3] + selection + MISSION_BEHAVIOR_CASES
        # An imported mission (exact GameModeID without a sealed behavior profile) is followed to its
        # natural end: phase events, cutscenes, the native result and the return to the wait room.
        imported = (('mission.import_flow',) if mission_game_mode_id is not None and mission_respawns is None
                    else ())
        return FLOW_CASES[:3] + selection + MISSION_FLOW_CASES + imported + (('mission.respawn',) if mission_respawns else ())
    modes = parse_mode_sequence(mode_sequence)
    if pages_only:
        if page_group == 'preview_guard':
            return FLOW_CASES[:1] + ('framework.special_target_guard',) + FLOW_CASES[1:3] + selection + ('pages.hangar.preview_guard',)
        if page_group == 'storage_gift':
            # One-shot storage claim, then the gift origin of a gifted and an
            # ordinary unit on the native unit-information page.
            return FLOW_CASES[:3] + selection + ('pages.storage.read','pages.storage.claim','pages.hangar.gift_origin','pages.hangar.gift_plain')
        if page_group == 'hangar':
            hangar_cases = (
                ('pages.hangar.read',) + (('pages.hangar.sort',) if hangar_sort else ()) +
                (('pages.hangar.confirm_gb',) if hangar_confirm_gb else ())
                if hangar_confirm_gb or hangar_sort else
                ('pages.hangar.read','pages.hangar.preview','pages.hangar.cancel')
            )
            return FLOW_CASES[:3] + selection + hangar_cases + (
                ('oc.panel',) if oc_panel else ()) + (('g1.page',) if g1_room and oc_panel else ())
        if shop_visual_only:
            return FLOW_CASES[:2] + ('flow.shop_ready',) + selection + ('pages.shop.catalog',)
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
    if same_room_rematch:
        cases=[]
        for mode in modes:
            name='pvp' if len(modes)==1 else f'pvp.mode{mode}'
            cases.extend(f'{name}.{step}' for step in ('room','start','ai_spawn','waiting'))
            cases.extend(f'{name}.rematch.{step}' for step in MODE_FLOW_STEPS)
        return prefix+tuple(cases)
    # The OC panel check runs last, after the settled return to the lobby.
    panel = (('oc.panel',) if (oc_allocate is not None or oc_panel) else ()) + (('g1.page',) if g1_room else ())
    if room_switch:
        if modes != (1,):
            raise GameTestError('room_switch plays three ordinary mode 1 battles')
        return prefix + tuple(f'pvp.{system}.{step}' for system in ROOM_SWITCH_SYSTEMS
                              for step in ROOM_SWITCH_STEPS) + panel
    if len(modes) == 1:
        # OC battle stats, speed/agility and the special: the battle unit exists
        # after the AI spawn check; the special waits inside the battle.
        stats = ('oc.stats',) if oc_stats_selected(oc_allocate, oc_special) else ()
        mobility = ('oc.mobility',) if oc_mobility_selected(oc_allocate) else ()
        special = ('oc.special',) if oc_special else ()
        return prefix + FLOW_CASES[3:6] + stats + mobility + special + FLOW_CASES[6:] + panel
    if oc_special:
        raise GameTestError('oc_special plays one ordinary PvP battle')
    return prefix + tuple(
        f"pvp.mode{mode}.{step}" for mode in modes for step in MODE_FLOW_STEPS
    ) + panel


def required_cases_for_plan(plan: dict) -> tuple[str, ...]:
    operator_fields = {field for field in OPERATOR_PLAN_FIELDS if field in plan}
    if operator_fields and operator_fields != set(OPERATOR_PLAN_FIELDS):
        raise GameTestError("operator flow plan is missing sealed input fields")
    if operator_fields:
        try:
            operator_scenarios.validate(plan["operator_flow"], plan)
        except ValueError as error:
            raise GameTestError(str(error)) from error
    if 'unit_scenario' in plan:
        if operator_fields:
            raise GameTestError("operator flow cannot be combined with a native unit scenario")
        try:return unit_scenarios.cases(unit_scenarios.validate(plan['unit_scenario'],plan))
        except ValueError as error:raise GameTestError(str(error)) from error
    shop_fields = {field for field in SHOP_PLAN_FIELDS if field in plan}
    if shop_fields and shop_fields != set(SHOP_PLAN_FIELDS):
        raise GameTestError("shop GameTest plan is missing sealed input fields")
    shop_catalog = bool(shop_fields)
    if "mission_mode" in plan:
        behavior_profile, _ = validate_mission_behavior(
            plan.get("mission_behavior_profile"),
            plan.get("mission_game_mode_id"),
            plan.get("mission_mode"),
        )
        forbidden = {
            "mode_sequence", "expected_ai", "ai_pool_level", "ai_difficulty",
            "ai_layout",
            "bootstrap_only",
            "battleship_only", "gift_requests_only", "deck_only",
            "pages_only", "hangar_confirm_gb", "hangar_sort", "friends_only", "room_titles_only", "collection_draw", "random_loadout", "same_room_rematch",
            *SHOP_PLAN_FIELDS,
        }
        if forbidden.intersection(plan):
            raise GameTestError("mission smoke plan cannot contain PvP, pages, or preparation scope")
        required = {
            "mission_mode", "mission_scope", "mission_ai_count",
            "mission_ai_level", *MISSION_CLAIM_FIELDS,
        }
        if behavior_profile is not None:
            required.update(MISSION_BEHAVIOR_FIELDS)
        if not required.issubset(plan):
            raise GameTestError("mission smoke plan is missing sealed scope fields")
        expected_scope = (
            MISSION_BEHAVIOR_SCOPE
            if behavior_profile is not None
            else MISSION_SCOPE
        )
        if (
            plan["mission_scope"] != expected_scope
            or type(plan["mission_ai_count"]) is not int
            or plan["mission_ai_count"] != MISSION_AI_COUNT
            or type(plan["mission_ai_level"]) is not int
            or plan["mission_ai_level"] != MISSION_AI_LEVEL
            or any(type(plan[field]) is not bool or plan[field] for field in MISSION_CLAIM_FIELDS)
        ):
            raise GameTestError("mission smoke plan changes its bounded native scope")
    required = required_flow_cases(
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
        same_room_rematch=plan.get("same_room_rematch",False),
        friends_only=plan.get("friends_only",False),
        room_titles_only=plan.get("room_titles_only",False),
        room_style_preview=plan.get("room_style_preview"),
        shop_visual_only=plan.get('shop_visual_only', False),
        page_group=plan.get('page_group'),
        hangar_confirm_gb=plan.get('hangar_confirm_gb', False),
        hangar_sort=plan.get('hangar_sort', False),
        mission_respawns=plan.get('mission_respawns'),
        oc_allocate=plan.get('oc_allocate'),
        oc_room=plan.get('oc_room', False),
        g1_room=plan.get('g1_room', False),
        room_switch=plan.get('room_switch', False),
        mission_behavior_profile=plan.get("mission_behavior_profile"),
        mission_game_mode_id=plan.get("mission_game_mode_id"),
        oc_panel=plan.get('oc_panel', False),
        oc_challenge=plan.get('oc_challenge', False),
        oc_special=plan.get('oc_special', False),
    )
    if operator_fields:
        try:
            return operator_scenarios.cases(required)
        except ValueError as error:
            raise GameTestError(str(error)) from error
    return required


def metadata_suite_for_plan(plan: dict) -> str:
    if plan.get('friends_only'):
        return 'native-friends'
    if plan.get("room_style_preview"):
        return "native-room-style-preview"
    if plan.get("room_titles_only"):
        return "native-room-titles"
    if "mission_mode" in plan:
        return (
            MISSION_BEHAVIOR_SUITE
            if "mission_behavior_profile" in plan
            else MISSION_SUITE
        )
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
    # A Windows short-name alias and its long spelling own the same account.
    # Canonicalize before deriving the lock identity, just as profile loading does.
    path = path.expanduser().resolve()
    return path.with_name(path.name + ".run.lock")


def ai_room_command(expected_ai: int, allies: int, policy: str | None = None, teams: str | None = None) -> str:
    if teams not in (None,'coop','versus'):
        raise GameTestError('AI human team policy must be coop or versus')
    if teams=='coop':
        if policy=='enemy':
            ai_room_command(expected_ai,allies,policy)
            return '!ai coop'
        if policy=='fill':
            ai_room_command(expected_ai,allies,policy)
            return '!ai coop fill'
        if policy is None:
            return f'!ai{expected_ai-allies} coop'+(f' a{allies}' if allies else '')
    if policy is None:return f'!ai e{expected_ai-allies} a{allies}'
    if policy not in ('fill','enemy'):
        raise GameTestError('AI population policy must be fill or enemy')
    expected_allies=3 if policy=='fill' else 0
    if expected_ai!=7 or allies!=expected_allies:
        raise GameTestError('automatic AI policies require seven bots with three allies for fill or zero for enemy in the single-host eight-player room')
    return '!ai '+policy


def planned_ai_room_command(plan: dict) -> str:
    """Preserve the sealed AI preset when adding another room command."""
    if plan.get('ai_layout') == 'allied':
        return '!ai1 a5 hard'
    if 'ai_pool_level' in plan:
        return f"!ai1 lv{plan['ai_pool_level']}"
    if 'ai_difficulty' in plan:
        return f"!ai {plan['ai_difficulty'].lower()}"
    count = plan['expected_ai']
    return ai_room_command(count, plan.get('ai_allies', count // 2),
                           plan.get('ai_policy'), plan.get('ai_teams'))


def random_loadout_room_title(nonce: str, plan: dict) -> str:
    title = f"{nonce} {planned_ai_room_command(plan)} !rand"
    # A reused observer adds a fresh nonce nibble. Use the equivalent native
    # enemy-count alias when necessary, retaining ally counts and the nonce.
    presets = ('ai_policy', 'ai_teams', 'ai_layout', 'ai_pool_level', 'ai_difficulty')
    allies=plan.get('ai_allies',plan['expected_ai']//2)
    enemies=plan['expected_ai']-allies
    if (len(title.encode('utf-8')) > 20 and enemies > 0 and
            not any(plan.get(k) for k in presets)):
        command=f'!ai{enemies}'+(f' a{allies}'if allies else'')
        return f"{nonce} {command} !rand"
    return title


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
    ai_allies: int | None = None,
    random_teams: bool = False,
    ai_policy: str | None = None,
    ai_teams: str | None = None,
    ai_pool_level: int | None = None,
    ai_difficulty: str | None = None,
    ai_layout: str | None = None,
    mission_respawns: str | None = None,
    mission_behavior_profile: str | None = None,
    mission_game_mode_id: int | None = None,
    mission_ai_difficulty: str | None = None,
    review_footage: bool = False,
) -> dict:
    if mission_respawns is not None and (mission_mode is None or mission_respawns not in ('player','infinite')):
        raise GameTestError('mission_respawns requires a native mission and player or infinite policy')
    if not 1 <= login_port <= 65535:
        raise GameTestError("login_port must be between 1 and 65535")
    if retry_count < 1 or retry_count > 2:
        raise GameTestError("retry_count must be 1 or 2")
    if expect_first_fail and retry_count != 2:
        raise GameTestError("expect_first_fail requires retry_count=2")
    ai_pool_level = validate_ai_pool_level(ai_pool_level)
    ai_difficulty = validate_ai_difficulty(ai_difficulty)
    ai_layout = validate_ai_layout(ai_layout)
    mission_ai_difficulty = validate_ai_difficulty(mission_ai_difficulty)
    if ai_pool_level is not None and ai_difficulty is not None:
        raise GameTestError("ai_pool_level and ai_difficulty cannot be combined")
    if any(value is not None for value in (ai_pool_level,ai_difficulty,ai_layout)) and (
            ai_allies is not None or random_teams or ai_policy is not None or ai_teams is not None):
        raise GameTestError('AI preset controls cannot be combined with custom population controls')
    mission_mode = validate_mission_mode(mission_mode)
    mission_behavior_profile, mission_game_mode_id = validate_mission_behavior(
        mission_behavior_profile, mission_game_mode_id, mission_mode
    )
    if mission_mode is None and mission_ai_difficulty is not None:
        raise GameTestError('mission_ai_difficulty requires --mission-mode')
    if mission_ai_difficulty is not None and mission_game_mode_id is not None:
        raise GameTestError('mission_ai_difficulty is limited to the plain mission smoke')
    if mission_mode is not None:
        if ai_allies is not None or random_teams or ai_policy is not None or ai_teams is not None:
            raise GameTestError('custom PvP AI teams cannot be used in a mission smoke plan')
        if ai_pool_level is not None or ai_difficulty is not None or ai_layout is not None:
            raise GameTestError("AI pool and difficulty controls are available only for ordinary PvP")
        if mode_sequence != "1":
            raise GameTestError("--mission-mode cannot be combined with a PvP mode sequence")
        if expected_ai != 3:
            raise GameTestError("--expected-ai is a PvP setting and cannot change a mission smoke plan")
        if mission_behavior_profile is not None and mission_respawns is not None:
            raise GameTestError(
                "mission behavior scenarios cannot add the respawn fixture"
            )
        modes = ()
    else:
        if mission_behavior_profile is not None or mission_game_mode_id is not None:
            raise GameTestError(
                "mission behavior scenarios require --mission-mode"
            )
        if expected_ai < 1 or expected_ai > 7:
            raise GameTestError("expected_ai must be between 1 and 7")
        if ai_pool_level is not None and expected_ai != 1:
            raise GameTestError("ai_pool_level requires expected_ai=1")
        if ai_layout is not None and (
            expected_ai != 6
            or ai_difficulty != "Hard"
            or ai_pool_level is not None
        ):
            raise GameTestError("ai_layout=allied requires ai_difficulty=Hard and expected_ai=6")
        if ai_difficulty is not None and ai_layout is None and expected_ai != 7:
            raise GameTestError("ai_difficulty requires expected_ai=7")
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
    if review_footage: plan["review_footage"] = True
    if mission_mode is not None:
        plan.update({
            "mission_mode": mission_mode,
            "mission_scope": (
                MISSION_BEHAVIOR_SCOPE
                if mission_behavior_profile is not None
                else MISSION_SCOPE
            ),
            "mission_ai_count": MISSION_AI_COUNT,
            "mission_ai_level": MISSION_AI_LEVEL,
            "mission_clear_claim": False,
            "mission_result_claim": False,
            "mission_active_exit_claim": False,
            "room_title": mission_room_title(
                title_nonce, mission_respawns, mission_ai_difficulty
            ),
        })
        if mission_ai_difficulty is not None:
            plan['mission_ai_difficulty'] = mission_ai_difficulty
        if mission_behavior_profile is not None:
            plan.update({
                "mission_behavior_profile": mission_behavior_profile,
                "mission_game_mode_id": mission_game_mode_id,
            })
        elif mission_game_mode_id is not None:
            # exact listed mission without a behavior scenario (e.g. an imported mission)
            plan["mission_game_mode_id"] = mission_game_mode_id
        if mission_respawns is not None: plan['mission_respawns'] = mission_respawns
    else:
        enemies = (expected_ai + 1) // 2
        allies = expected_ai - enemies
        if ai_allies is not None:
            if type(ai_allies) is not int or not 0<=ai_allies<expected_ai:
                raise GameTestError('ai_allies must leave at least one enemy in the single-host PvP scenario')
            allies=ai_allies;enemies=expected_ai-ai_allies
            plan['ai_allies']=allies
        if type(random_teams) is not bool:raise GameTestError('random_teams must be a boolean')
        if random_teams:plan['random_teams']=True
        command=ai_room_command(expected_ai,allies,ai_policy,ai_teams)
        if ai_layout=='allied':command='!ai1 a5 hard'
        elif ai_pool_level is not None:command=f'!ai1 lv{ai_pool_level}'
        elif ai_difficulty is not None:command=f'!ai {ai_difficulty.lower()}'
        if ai_policy is not None:plan['ai_policy']=ai_policy
        if ai_teams is not None:plan['ai_teams']=ai_teams
        plan.update({
            "mode_sequence": ",".join(str(mode) for mode in modes),
            "expected_ai": expected_ai,
            "room_title": f"{title_nonce} {command}",
        })
        if ai_pool_level is not None:
            plan["ai_pool_level"] = ai_pool_level
        if ai_difficulty is not None:
            plan["ai_difficulty"] = ai_difficulty
        if ai_layout is not None:
            plan["ai_layout"] = ai_layout
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
        if 'mission_ai_difficulty' in plan:
            run['run']['mission_ai_difficulty'] = plan['mission_ai_difficulty']
        if "mission_behavior_profile" in plan:
            run["run"].update({
                "mission_behavior_profile": plan["mission_behavior_profile"],
                "mission_game_mode_id": str(plan["mission_game_mode_id"]),
            })
        elif plan.get("mission_game_mode_id"):
            run["run"]["mission_game_mode_id"] = str(plan["mission_game_mode_id"])
        if 'mission_respawns' in plan: run['run']['mission_respawns'] = plan['mission_respawns']
    else:
        run["run"]["mode_sequence"] = plan["mode_sequence"]
        run["run"]["expected_ai"] = str(plan["expected_ai"])
        if 'ai_allies' in plan:run['run']['ai_allies']=str(plan['ai_allies'])
        if plan.get('random_teams'):run['run']['random_teams']='1'
        if 'ai_policy' in plan:run['run']['ai_policy']=plan['ai_policy']
        if 'ai_teams' in plan:run['run']['ai_teams']=plan['ai_teams']
        if "ai_pool_level" in plan:
            run["run"]["ai_pool_level"] = str(plan["ai_pool_level"])
        if "ai_difficulty" in plan:
            run["run"]["ai_difficulty"] = plan["ai_difficulty"]
        if "ai_layout" in plan:
            run["run"]["ai_layout"] = plan["ai_layout"]
    if 'play_unit_uid' in plan:
        run['run']['play_unit_uid'] = str(plan['play_unit_uid'])
    if 'oc_allocate' in plan:
        run['run']['oc_allocate'] = plan['oc_allocate']
    if plan.get('oc_room'):
        run['run']['oc_room'] = '1'
    if plan.get('g1_room'):
        run['run']['g1_room'] = '1'
    if plan.get('room_switch'):
        run['run']['room_switch'] = '1'
    if plan.get('oc_panel'):
        run['run']['oc_panel'] = '1'
    if plan.get('oc_challenge'):
        run['run']['oc_challenge'] = '1'
    if plan.get('oc_special'):
        run['run']['oc_special'] = '1'
    if 'unit_scenario_sha256' in plan:
        run['run']['unit_scenario_sha256'] = plan['unit_scenario_sha256']
    if 'operator_flow' in plan:
        for field, value in operator_scenarios.run_fields(plan).items():
            run['run'][field] = value
    if 'page_group' in plan:
        run['run']['page_group'] = plan['page_group']
    if plan.get('hangar_confirm_gb'):
        run['run']['hangar_confirm_gb'] = '1'
    if plan.get('hangar_sort'):
        run['run']['hangar_sort'] = '1'
    if plan.get('login_recovery'):
        run['run'].update({
            'login_recovery': '1',
            'login_recovery_min_retries': str(plan['login_recovery_min_retries']),
            'login_recovery_hold_after_completion':
                '1' if plan['login_recovery_hold_after_completion'] else '0',
        })
    if all(field in plan for field in SHOP_PLAN_FIELDS):
        for field in SHOP_PLAN_FIELDS:
            run['run'][field] = str(plan[field])
        if plan.get('shop_visual_only'):
            run['run']['shop_visual_only'] = '1'
    for flag in ('bootstrap_only','battleship_only','gift_requests_only','deck_only','pages_only','friends_only','room_titles_only','collection_draw','random_loadout','same_room_rematch'):
        if plan.get(flag):run['run'][flag] = '1'
    if 'room_style_preview' in plan:
        run['run']['room_style_preview'] = plan['room_style_preview']
    if 'network_policy' in plan:run['run']['network_policy']=plan['network_policy']
    if plan.get('network_policy') == ROUTED_LOOPBACK_POLICY:
        for field in ROUTE_PLAN_FIELDS:
            run['run'][field] = str(plan[field])
    if 'quiet' in plan:run['run']['quiet']='1' if plan['quiet'] else '0'
    if plan.get('review_footage'):run['run']['review_footage']='1'
    if plan.get('quiet'):
        run['run']['quiet_loopback']='1' if (
            plan.get('network_policy') == ROUTED_LOOPBACK_POLICY or
            (plan.get('network_policy')=='direct' and
             ipaddress.IPv4Address(plan['target_host']).is_loopback)
        ) else '0'
    write_ini(directory / "run.ini", run)


def read_public_plan(root: Path, *, historical_cases: bool = False) -> dict:
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
                or "ai_layout" in run
                or any(field.startswith("operator_") for field in run)
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
        page_group = run.get('page_group')
        if run.get('hangar_confirm_gb', '0') not in ('0', '1'):
            raise ValueError('hangar_confirm_gb must be canonical boolean')
        hangar_confirm_gb = run.get('hangar_confirm_gb', '0') == '1'
        if hangar_confirm_gb and (not pages_only or page_group != 'hangar'):
            raise ValueError('hangar_confirm_gb requires the sealed hangar page group')
        if run.get('hangar_sort', '0') not in ('0', '1'):
            raise ValueError('hangar_sort must be canonical boolean')
        hangar_sort = run.get('hangar_sort', '0') == '1'
        if hangar_sort and (not pages_only or page_group != 'hangar'):
            raise ValueError('hangar_sort requires the sealed hangar page group')
        friends_only = run.get('friends_only', '0') == '1'
        room_titles_only = run.get('room_titles_only','0') == '1'
        room_style_preview = run.get('room_style_preview')
        if mission_plan and room_style_preview is not None:
            raise ValueError('room-style preview cannot be a mission combat plan')
        collection_draw = run.get('collection_draw', '0') == '1'
        same_room_rematch = run.get('same_room_rematch','0')=='1'
        recovery_fields = {
            'login_recovery',
            'login_recovery_min_retries',
            'login_recovery_hold_after_completion',
        }
        present_recovery_fields = recovery_fields.intersection(run.keys())
        login_recovery = bool(present_recovery_fields)
        login_recovery_min_retries = None
        login_recovery_hold_after_completion = False
        if login_recovery:
            if present_recovery_fields != recovery_fields:
                raise ValueError('login recovery plan must seal every recovery field')
            if run['login_recovery'] != '1':
                raise ValueError('login_recovery must be canonical enabled boolean')
            if run['login_recovery_min_retries'] not in ('0','1'):
                raise ValueError('login_recovery_min_retries must be canonical 0 or 1')
            if run['login_recovery_hold_after_completion'] not in ('0','1'):
                raise ValueError('login recovery hold must be canonical boolean')
            login_recovery_min_retries = int(run['login_recovery_min_retries'])
            login_recovery_hold_after_completion = run['login_recovery_hold_after_completion'] == '1'
            if not pages_only or page_group != 'hangar':
                raise ValueError('login recovery requires the read-only hangar page group')
            if hangar_confirm_gb or hangar_sort:
                raise ValueError('login recovery cannot submit a hangar transaction')
        unknown_operator_fields = {
            field for field in run if field.startswith("operator_")
        } - set(operator_scenarios.RUN_FIELDS)
        if unknown_operator_fields:
            raise ValueError("unknown operator flow run.ini fields are forbidden")
        operator_fields_present = {
            field for field in operator_scenarios.RUN_FIELDS if field in run
        }
        if operator_fields_present and operator_fields_present != set(operator_scenarios.RUN_FIELDS):
            raise ValueError("operator flow run.ini must contain every sealed field")
        operator_present = bool(operator_fields_present)
        preparation_fields = (
            'bootstrap_only', 'battleship_only', 'gift_requests_only',
            'deck_only', 'pages_only', 'hangar_confirm_gb', 'hangar_sort', 'friends_only', 'room_titles_only', 'collection_draw', 'random_loadout', 'same_room_rematch',
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
        } - set(SHOP_PLAN_FIELDS) - {'shop_visual_only'}
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
            if 'shop_visual_only' in run:
                if run['shop_visual_only'] != '1':
                    raise ValueError('invalid shop visual scope')
                shop_plan['shop_visual_only'] = True
        elif 'shop_visual_only' in run:
            raise ValueError('shop visual scope requires sealed stock inputs')

        mission_mode = None
        if mission_plan:
            if login_recovery:
                raise ValueError('login recovery cannot be a mission plan')
            behavior_fields = {
                field for field in MISSION_BEHAVIOR_FIELDS if field in run
            }
            if (behavior_fields and behavior_fields != set(MISSION_BEHAVIOR_FIELDS)
                    and behavior_fields != {"mission_game_mode_id"}):
                raise ValueError(
                    "mission behavior plan must seal both profile and GameModeID"
                )
            behavior_profile = None
            behavior_mission_id = None
            required_mission_fields = {
                'mission_mode', 'mission_scope', 'mission_ai_count',
                'mission_ai_level', *MISSION_CLAIM_FIELDS,
                'network_policy', 'quiet',
            }
            if not required_mission_fields.issubset(run.keys()):
                raise ValueError('mission smoke plan is missing sealed scope fields')
            if any(field in run for field in (
                'mode_sequence', 'expected_ai', 'ai_pool_level', 'ai_difficulty',
                'ai_layout',
                *preparation_fields,
            )):
                raise ValueError('mission smoke plan contains PvP, pages, or preparation scope')
            if run['mission_mode'] not in tuple(str(mode) for mode in SUPPORTED_MISSION_MODES):
                raise ValueError('invalid mission mode')
            mission_mode = validate_mission_mode(int(run['mission_mode']))
            if behavior_fields:
                raw_behavior_id = run["mission_game_mode_id"]
                if not re.fullmatch(r"[1-9][0-9]*", raw_behavior_id):
                    raise ValueError(
                        "mission_game_mode_id must be canonical non-zero decimal"
                    )
                try:
                    behavior_profile, behavior_mission_id = validate_mission_behavior(
                        run.get("mission_behavior_profile"),
                        int(raw_behavior_id),
                        mission_mode,
                    )
                except GameTestError as error:
                    raise ValueError(str(error)) from error
            expected_mission_scope = (
                MISSION_BEHAVIOR_SCOPE
                if behavior_profile is not None
                else MISSION_SCOPE
            )
            if run['mission_scope'] != expected_mission_scope:
                raise ValueError('invalid mission smoke scope')
            if (
                run['mission_ai_count'] != str(MISSION_AI_COUNT)
                or run['mission_ai_level'] != str(MISSION_AI_LEVEL)
            ):
                raise ValueError('mission AI count or level differs from the sealed native plan')
            if any(run[field] != '0' for field in MISSION_CLAIM_FIELDS):
                raise ValueError('mission smoke cannot claim clear, result, or active exit')
            mission_ai_difficulty = run.get('mission_ai_difficulty')
            if mission_ai_difficulty is not None:
                try:
                    mission_ai_difficulty = validate_ai_difficulty(mission_ai_difficulty)
                except GameTestError as error:
                    raise ValueError(str(error)) from error
                if behavior_mission_id is not None:
                    raise ValueError('mission AI difficulty is limited to the plain mission smoke')
            if run['retry_count'] != '1' or run['expect_first_fail'] != '0':
                raise ValueError('mission smoke requires one existing-account login attempt')
            canonical_run_id = str(uuid.UUID(run['id']))
            if run['id'] != canonical_run_id:
                raise ValueError('mission run id must use canonical lowercase UUID form')
            if not re.fullmatch(r'gt[0-9a-f]{4}', run['title_nonce']):
                raise ValueError('invalid mission title nonce')
            mission_respawns = run.get('mission_respawns')
            if mission_respawns is not None and mission_respawns not in ('player','infinite'):
                raise ValueError('invalid mission respawn policy')
            if behavior_profile is not None and mission_respawns is not None:
                raise ValueError(
                    "mission behavior plan cannot add the respawn fixture"
                )
            if run['room_title'] != mission_room_title(
                run['title_nonce'], mission_respawns, mission_ai_difficulty
            ):
                raise ValueError('mission room title differs from the sealed native plan')
            expected_cases = required_flow_cases(
                '1', play_unit_uid=play_unit_uid, mission_mode=mission_mode,
                mission_respawns=mission_respawns,
                mission_behavior_profile=behavior_profile,
                mission_game_mode_id=behavior_mission_id,
            )
            allowed = {
                'id', 'role', 'suite', 'normal_flow', 'endpoint',
                'target_server', 'target_channel', 'server_channel_selection',
                'title_nonce', 'room_title', 'auto_nickname', 'exit_tutorial',
                'first_unit_draw', 'acknowledge_game_windows', 'process_entry',
                'account_path', 'retry_count', 'expect_first_fail',
                'required_cases', 'mission_mode', 'mission_scope',
                'mission_ai_count', 'mission_ai_level', 'mission_respawns',
                'mission_ai_difficulty', *MISSION_CLAIM_FIELDS,
                *MISSION_BEHAVIOR_FIELDS,
                'play_unit_uid', 'network_policy',
                *ROUTE_PLAN_FIELDS, 'quiet', 'quiet_loopback', 'review_footage',
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
                'mission_scope', 'mission_ai_count', 'mission_ai_level', 'mission_respawns',
                *MISSION_CLAIM_FIELDS, *MISSION_BEHAVIOR_FIELDS,
            )):
                raise ValueError('mission fields require native-mission-smoke suite')
            mode_sequence = ",".join(
                str(mode) for mode in parse_mode_sequence(run["mode_sequence"])
            )
            ai_pool_level = None
            ai_difficulty = None
            ai_layout = None
            if 'ai_pool_level' in run:
                raw_ai_pool_level = run['ai_pool_level']
                if not re.fullmatch(r'[1-5]', raw_ai_pool_level):
                    raise ValueError('ai_pool_level must be canonical decimal from 1 through 5')
                if run['expected_ai'] != '1':
                    raise ValueError('ai_pool_level requires exactly one AI')
                if any(field in run for field in preparation_fields):
                    raise ValueError('ai_pool_level requires ordinary PvP without preparation scopes')
                ai_pool_level = int(raw_ai_pool_level)
                nonce = run['title_nonce']
                if not re.fullmatch(r'gt[0-9a-f]{4}', nonce):
                    raise ValueError('invalid AI pool title nonce')
                suffix = f' !ai1 lv{ai_pool_level}'
                direct_title = nonce + suffix
                reused_title = (
                    r'gt[0-9a-f]{4}' + re.escape(nonce[2:]) + re.escape(suffix)
                )
                if (
                    run['room_title'] != direct_title
                    and re.fullmatch(reused_title, run['room_title']) is None
                ):
                    raise ValueError('AI pool room command differs from the sealed native plan')
            if 'ai_layout' in run:
                ai_layout = run['ai_layout']
                if ai_layout != 'allied':
                    raise ValueError('ai_layout must use its canonical allied name')
                if ai_pool_level is not None:
                    raise ValueError('ai_layout cannot be combined with ai_pool_level')
                if run.get('ai_difficulty') != 'Hard' or run['expected_ai'] != '6':
                    raise ValueError('allied AI layout requires Hard difficulty and exactly six AI')
                if any(field in run for field in preparation_fields):
                    raise ValueError('ai_layout requires ordinary PvP without preparation scopes')
            if 'ai_difficulty' in run:
                raw_ai_difficulty = run['ai_difficulty']
                if raw_ai_difficulty not in ('Easy', 'Medium', 'Hard'):
                    raise ValueError('ai_difficulty must use its canonical name')
                if ai_pool_level is not None:
                    raise ValueError('ai_difficulty cannot be combined with ai_pool_level')
                required_ai_count = '6' if ai_layout == 'allied' else '7'
                if run['expected_ai'] != required_ai_count:
                    raise ValueError(
                        f'ai_difficulty requires exactly {required_ai_count} AI'
                    )
                if any(field in run for field in preparation_fields if field != 'random_loadout'):
                    raise ValueError('ai_difficulty requires ordinary PvP without preparation scopes')
                ai_difficulty = raw_ai_difficulty
                nonce = run['title_nonce']
                randomized = run.get('random_loadout') == '1'
                if not re.fullmatch(r'[0-9a-f]{4,5}' if randomized else r'gt[0-9a-f]{4}', nonce):
                    raise ValueError('invalid AI difficulty title nonce')
                suffix = (
                    ' !ai1 a5 hard'
                    if ai_layout == 'allied'
                    else f' !ai {ai_difficulty.lower()}'
                )
                if randomized:
                    suffix += ' !rand'
                direct_title = nonce + suffix
                reused_title = (
                    r'gt[0-9a-f]{4}' + re.escape(nonce[2:]) + re.escape(suffix)
                )
                if randomized:
                    reused_title = r'(?!)'
                try:
                    title_bytes = run['room_title'].encode('ascii')
                except UnicodeEncodeError as error:
                    raise ValueError('AI difficulty room command must be ASCII') from error
                if (
                    len(title_bytes) > 20
                    or (
                        run['room_title'] != direct_title
                        and re.fullmatch(reused_title, run['room_title']) is None
                    )
                ):
                    raise ValueError('AI difficulty room command differs from the sealed native plan')
            oc_allocate = run.get('oc_allocate')
            if oc_allocate is not None:
                try:
                    validate_oc_allocate(oc_allocate)
                except GameTestError as error:
                    raise ValueError(str(error)) from error
            expected_cases = required_flow_cases(
                mode_sequence, bootstrap_only, battleship_only,
                gift_requests_only, deck_only, pages_only, collection_draw,
                play_unit_uid, shop_catalog=shop_plan is not None,friends_only=friends_only, room_titles_only=room_titles_only,
                room_style_preview=room_style_preview,
                shop_visual_only=bool(shop_plan and shop_plan.get('shop_visual_only')),
                same_room_rematch=same_room_rematch,
                page_group=page_group,
                hangar_confirm_gb=hangar_confirm_gb,
                hangar_sort=hangar_sort,
                oc_allocate=oc_allocate,
                oc_room=run.get('oc_room') == '1',
                g1_room=run.get('g1_room') == '1',
                room_switch=run.get('room_switch') == '1',
                oc_panel=run.get('oc_panel') == '1',
                oc_challenge=run.get('oc_challenge') == '1',
                oc_special=run.get('oc_special') == '1',
            )
            if operator_present:
                expected_cases = operator_scenarios.cases(expected_cases)

        unit_binding={}
        if 'unit_scenario_sha256' in run:
            if operator_present:
                raise ValueError("operator flow cannot be combined with a native unit scenario")
            unit_context={'pages_only':pages_only,'play_unit_uid':play_unit_uid,
                'collection_draw':collection_draw,'bootstrap_only':bootstrap_only,
                'friends_only':friends_only,'room_titles_only':room_titles_only,
                'mission_mode':mission_mode,**(shop_plan or {})}
            unit_binding=unit_scenarios.read(root,run['unit_scenario_sha256'],unit_context,read_ini)
            expected_cases=unit_scenarios.cases(unit_binding['unit_scenario'])
        if historical_cases:
            recorded_cases = tuple(run["required_cases"].split(','))
            if (not recorded_cases or len(recorded_cases) > 256 or len(set(recorded_cases)) != len(recorded_cases) or
                any(not re.fullmatch(r'[a-z][a-z0-9_.]{0,127}', case) for case in recorded_cases)):
                raise ValueError('invalid historical native case contract')
            expected_cases = recorded_cases
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
                'mission_scope': (
                    MISSION_BEHAVIOR_SCOPE
                    if behavior_profile is not None
                    else MISSION_SCOPE
                ),
                'mission_ai_count': MISSION_AI_COUNT,
                'mission_ai_level': MISSION_AI_LEVEL,
                'mission_clear_claim': False,
                'mission_result_claim': False,
                'mission_active_exit_claim': False,
            })
            if mission_ai_difficulty is not None:
                plan['mission_ai_difficulty'] = mission_ai_difficulty
            if behavior_profile is not None:
                plan.update({
                    "mission_behavior_profile": behavior_profile,
                    "mission_game_mode_id": behavior_mission_id,
                })
            elif behavior_mission_id is not None:
                plan["mission_game_mode_id"] = behavior_mission_id
            if mission_respawns is not None: plan['mission_respawns'] = mission_respawns
        else:
            plan['mode_sequence'] = mode_sequence
            plan['expected_ai'] = int(run['expected_ai'])
            if 'ai_allies' in run:
                allies=int(run['ai_allies'])
                if not 0<=allies<plan['expected_ai']:raise ValueError('invalid custom AI ally count')
                plan['ai_allies']=allies
            if 'random_teams' in run:
                if run['random_teams']!='1':raise ValueError('invalid native random-team flag')
                plan['random_teams']=True
            if 'ai_policy' in run or 'ai_teams' in run:
                policy=run.get('ai_policy')
                teams=run.get('ai_teams')
                command=ai_room_command(plan['expected_ai'],plan.get('ai_allies',plan['expected_ai']//2),policy,teams)
                suffix=' !rand' if run.get('random_loadout')=='1' else ''
                if not run['room_title'].endswith(' '+command+suffix):raise ValueError('automatic AI policy differs from the sealed room command')
                if policy is not None:plan['ai_policy']=policy
                if teams is not None:plan['ai_teams']=teams
            if ai_pool_level is not None:
                plan['ai_pool_level'] = ai_pool_level
            if ai_difficulty is not None:
                plan['ai_difficulty'] = ai_difficulty
            if ai_layout is not None:
                plan['ai_layout'] = ai_layout
        if play_unit_uid is not None:
            plan['play_unit_uid'] = play_unit_uid
        if login_recovery:
            if plan['retry_count'] != 1 or plan['expect_first_fail']:
                raise ValueError('login recovery requires one native existing-account attempt')
            plan.update({
                'login_recovery': True,
                'login_recovery_min_retries': login_recovery_min_retries,
                'login_recovery_hold_after_completion': login_recovery_hold_after_completion,
            })
        if run.get('oc_allocate'):
            plan['oc_allocate'] = validate_oc_allocate(run['oc_allocate'])
        if run.get('oc_room') == '1':
            if not run.get('oc_allocate') or not run['room_title'].endswith(' !oc'):
                raise ValueError('OC room-system run lacks its sealed !oc room command')
            plan['oc_room'] = True
        if run.get('g1_room') == '1':
            if (run.get('oc_allocate') and not run['room_title'].endswith(' !g1')) or (
                    not run.get('oc_allocate') and run.get('oc_panel') != '1'):
                raise ValueError('G改 1.0 room-system run lacks its sealed !g1 room command')
            plan['g1_room'] = True
        if run.get('room_switch') == '1':
            if (not run.get('oc_allocate') or run.get('oc_room') == '1' or run.get('g1_room') == '1' or
                    run['room_title'].endswith((' !oc', ' !g1', ' !g2')) or
                    any(len(title.encode('utf-8')) > 20 for title in room_switch_titles(run['room_title']))):
                raise ValueError('room-switch run lacks its plain sealed base room title')
            plan['room_switch'] = True
        if run.get('oc_panel') == '1':
            plan['oc_panel'] = True
        if run.get('oc_challenge') == '1':
            plan['oc_challenge'] = True
        if run.get('oc_special', '0') not in ('0', '1'):
            raise ValueError('oc_special must be canonical boolean')
        if run.get('oc_special') == '1':
            # A reused client's earlier seal may predate the all-enemy layout.
            if (not run.get('oc_allocate') or '5' not in oc_allocated_stats(run['oc_allocate']) or
                    run.get('room_switch') == '1' or run.get('mode_sequence') != '1' or
                    (not historical_cases and (run.get('expected_ai') != str(OC_SPECIAL_EXPECTED_AI) or
                                               run.get('ai_allies') != '0'))):
                raise ValueError('OC special run lacks its sealed special allocation in one all-enemy ordinary battle')
            plan['oc_special'] = True
        plan.update(unit_binding)
        if shop_plan is not None:
            plan.update(shop_plan)
        if room_style_preview is not None:
            plan['room_style_preview'] = room_style_preview
        if page_group is not None:
            plan['page_group'] = page_group
        if hangar_confirm_gb:
            plan['hangar_confirm_gb'] = True
        if hangar_sort:
            plan['hangar_sort'] = True
        for flag, value in (
            ('bootstrap_only', bootstrap_only),
            ('battleship_only', battleship_only),
            ('gift_requests_only', gift_requests_only),
            ('deck_only', deck_only),
            ('pages_only', pages_only),
            ('friends_only',friends_only),
            ('room_titles_only',room_titles_only),
            ('collection_draw', collection_draw),
            ('same_room_rematch', same_room_rematch),
        ):
            if value:
                plan[flag] = True

        if not mission_plan and 'random_loadout' in run:
            if run['random_loadout'] not in ('0', '1'):
                raise ValueError('invalid random loadout policy')
            if run['random_loadout'] == '1':
                # Coordinated PvP uses the native actor command session. Its
                # random title is sealed here too; friends.session alone still
                # makes no autonomous battle or settlement acceptance claim.
                if any((bootstrap_only, battleship_only, gift_requests_only, deck_only, pages_only)):
                    raise ValueError('random scenario requires PvP')
                nonce = run['title_nonce']
                expected_ai = int(run["expected_ai"])
                allies=plan.get('ai_allies',expected_ai//2)
                if (
                    not re.fullmatch(r'[0-9a-f]{4,5}', nonce)
                    or run['room_title'] !=
                    random_loadout_room_title(nonce, plan)
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
        if 'review_footage' in run:
            if run['review_footage'] != '1' or not plan.get('quiet'):
                raise ValueError('review footage requires a quiet run')
            plan['review_footage'] = True
        if operator_present:
            plan.update(operator_scenarios.read(root, run, plan))
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
    if (
        "operator_flow" in plan
        and registration_state not in MISSION_EXISTING_ACCOUNT_STATES
    ):
        raise GameTestError(
            "operator flow requires a registered or explicitly imported existing account profile"
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


def unused_direct_executable(relative: Path, launcher_path: str = '') -> bool:
    return (not launcher_path and relative.parent==Path('.') and
            relative.suffix.casefold()=='.exe' and relative.name.casefold()!=GAME_EXE.casefold())


def collect_hashes(root: Path, launcher_path: str = '') -> tuple[dict, dict]:
    immutable: dict[str, str] = {}
    initial: dict[str, str] = {}

    def bind(path: Path, collection: dict) -> None:
        if not path.is_file():
            raise GameTestError(f"missing copied GameTest payload: {path}")
        if not is_relative_to(path.resolve(),root.resolve()):
            raise GameTestError('copied native resource escapes the owned client')
        collection[path.relative_to(root).as_posix()] = sha256(path)

    bind(root / GAME_EXE, immutable)
    if launcher_path:bind(root/launcher_path,immutable)
    for path in sorted(root.iterdir(), key=lambda item: item.name.lower()):
        if (path.is_file() and path.suffix.lower() in IMMUTABLE_ROOT_SUFFIXES and
                not unused_direct_executable(path.relative_to(root),launcher_path)):
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
    mod_files=(path for tree in ('mods','owner-mods','local-mods')for path in(root/'sdgo'/tree).rglob('*'))
    for path in sorted(mod_files, key=lambda item: item.as_posix().lower()):
        if path.relative_to(root).as_posix().lower() in RUNTIME_OUTPUTS:
            continue
        if path.is_file() and path.suffix.lower() in {".dll", ".lua", ".ini", ".tsv", ".json",
                                                      ".bin", ".gdt", ".zsd", ".spr", ".txr", ".uir", ".zpk", ".zdx", ".html"}:
            bind(path, immutable)
    for relative in (
        SHOP_ASSERTIONS_RELATIVE,
        SHOP_DISPLAY_RELATIVE,
        operator_scenarios.SHOP_RELATIVE,
        operator_scenarios.DISPLAY_RELATIVE,
    ):
        path = root / relative
        if path.exists():
            bind(path, immutable)
    # Imported native assets live beside the game, not only under sdgo/mods.
    # Bind the complete explicit resource trees before the process starts.
    for tree in ('gdrs','mdrs','anrs','txrs','sprs','fxrs','sdrs','scp','sol','trns','uirs'):
        folder=root/tree
        if not folder.exists():continue
        for path in sorted(folder.rglob('*'),key=lambda item:item.as_posix().lower()):
            if path.is_file():bind(path,immutable)
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
    ai_allies: int | None = None,
    random_teams: bool = False,
    ai_policy: str | None = None,
    ai_teams: str | None = None,
    ai_pool_level: int | None = None,
    ai_difficulty: str | None = None,
    ai_layout: str | None = None,
    play_unit_uid: int | None = None,
    mission_mode: int | None = None,
    mission_respawns: str | None = None,
    mission_behavior_profile: str | None = None,
    mission_game_mode_id: int | None = None,
    mission_ai_difficulty: str | None = None,
    launcher_path: str | None = None,
    account_profile: Path | None = None,
    create_test_account: bool = False,
    sfx_only: bool = False,
    bootstrap_only: bool = False,
    battleship_only: bool = False,
    gift_requests_only: bool = False,
    deck_only: bool = False,
    pages_only: bool = False,
    page_group: str | None = None,
    hangar_confirm_gb: bool = False,
    hangar_sort: bool = False,
    friends_only: bool = False,
    room_titles_only: bool = False,
    room_style_preview: str | None = None,
    collection_draw: bool = False,
    shop_assertions: Path | None = None,
    shop_display: Path | None = None,
    random_loadout: bool = False,
    same_room_rematch: bool = False,
    network_policy: str = 'direct',
    route_profile: Path | None = None,
    quiet: bool = True,
    review_footage: bool = False,
    asset_baseline: Path | None = None,
    extra_mods: tuple[str, ...] = (),
    disabled_mods: tuple[str, ...] = (),
    apsalus_roundtrip: bool = False,
    unit_transform_roundtrip: bool = False,
    unit_transform_steps: int | None = None,
    capture_native_frames: bool = False,
    fps_gameplay_cap: int | None = None,
    oc_allocate: str | None = None,
    oc_panel: bool = False,
    oc_challenge: bool = False,
    oc_room: bool = False,
    g1_room: bool = False,
    room_switch: bool = False,
    oc_special: bool = False,
    server_session: dict | None = None,
    rebind_existing_account: bool = False,
    fps_cap: int | None = None,
    fps_mod_enabled: bool = True,
    bind_staged_loader: bool = False,
    client_packages: tuple[Path, ...] = (),
    battle_map: int = 0,
    unit_package: Path | None = None,
    capsule_package: Path | None = None,
    unit_scenario: dict | None = None,
    login_recovery_min_retries: int | None = None,
    login_recovery_hold_after_completion: bool = False,
    operator_flow: dict | None = None,
    _owned_client_transfer: tuple[Path, str] | None = None,
) -> dict:
    client_template = client_template.resolve()
    capsule_binding=None
    if capsule_package is not None:
        try:capsule_binding=capsule_packages.validate(capsule_package,stage)
        except (OSError,ValueError) as error:raise GameTestError(str(error)) from error
    unit_payload=None
    if unit_package is not None:
        unit_package=Path(unit_package).resolve()
        if not unit_package.is_file() or not 0<unit_package.stat().st_size<=160*1024*1024:
            raise GameTestError('unit package is missing or exceeds the trusted installer limit')
        unit_payload=unit_package.read_bytes()
    for package in client_packages:
        path=Path(package)
        if not path.is_file()or path.stat().st_size>72*1024*1024:raise GameTestError('client package is missing or exceeds the import limit')
    if page_group is not None:
        if page_group not in PAGE_GROUPS or not pages_only:
            raise GameTestError('page_group requires --pages-only')
        sdk=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
        parsed=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',sdk)
        if not parsed or tuple(map(int,parsed.groups())) < (0,1,8):
            raise GameTestError('the sealed hangar page group requires native observer 0.1.8 or newer')
    if type(hangar_confirm_gb) is not bool:
        raise GameTestError('--hangar-confirm-gb must be a flag')
    if hangar_confirm_gb:
        if not pages_only or page_group != 'hangar' or create_test_account:
            raise GameTestError('--hangar-confirm-gb requires the sealed hangar page group and an existing test account')
        sdk=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
        parsed=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',sdk)
        if not parsed or tuple(map(int,parsed.groups())) < (0,1,20):
            raise GameTestError('--hangar-confirm-gb requires native GameTest client 0.1.20 or newer')
    if type(hangar_sort) is not bool:
        raise GameTestError('--hangar-sort must be a flag')
    if hangar_sort or hangar_confirm_gb:
        if hangar_sort and (not pages_only or page_group != 'hangar' or create_test_account):
            raise GameTestError('--hangar-sort requires the sealed hangar page group and an existing test account')
        sdk=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
        parsed=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',sdk)
        if not parsed or tuple(map(int,parsed.groups())) < (0,1,27):
            raise GameTestError('live hangar refresh checks require native GameTest client 0.1.27 or newer')
    if page_group == 'storage_gift':
        if create_test_account:
            raise GameTestError('the storage_gift page group requires an existing test account with gift_origin units')
        sdk=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
        parsed=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',sdk)
        if not parsed or tuple(map(int,parsed.groups())) < (0,1,31):
            raise GameTestError('the storage_gift page group requires native GameTest client 0.1.31 or newer')
    if login_recovery_min_retries is None:
        if login_recovery_hold_after_completion:
            raise GameTestError('login recovery hold requires a sealed recovery scenario')
    else:
        if (type(login_recovery_min_retries) is not int or
                login_recovery_min_retries not in (0,1)):
            raise GameTestError('login_recovery_min_retries must be integer 0 or 1')
        if type(login_recovery_hold_after_completion) is not bool:
            raise GameTestError('login_recovery_hold_after_completion must be boolean')
        if not pages_only or page_group != 'hangar' or create_test_account or mission_mode is not None:
            raise GameTestError('login recovery requires read-only hangar pages and an existing account')
        if hangar_confirm_gb or hangar_sort:
            raise GameTestError('login recovery cannot submit a hangar transaction')
        extra_mods=tuple(dict.fromkeys((*extra_mods,'login_recovery_client')))
    if type(same_room_rematch) is not bool or (same_room_rematch and (fps_cap is not None or mission_mode is not None or any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only)))):
        raise GameTestError('same-room rematch requires complete PvP without preparation-only scopes')
    if rebind_existing_account:
        raise GameTestError('existing-account rebinding requires --reuse-client')
    if (ai_allies is not None or random_teams or ai_policy is not None or ai_teams is not None) and (fps_cap is not None or mission_mode is not None or any((bootstrap_only,
            battleship_only,gift_requests_only,deck_only,pages_only))):
        raise GameTestError('custom AI team counts require a complete native PvP battle')
    ai_pool_level = validate_ai_pool_level(ai_pool_level)
    ai_difficulty = validate_ai_difficulty(ai_difficulty)
    ai_layout = validate_ai_layout(ai_layout)
    if ai_pool_level is not None and ai_difficulty is not None:
        raise GameTestError('--ai-pool-level and --ai-difficulty cannot be combined')
    if fps_gameplay_cap is not None:
        if type(fps_gameplay_cap) is not int or not 1<=fps_gameplay_cap<=1000:
            raise GameTestError('gameplay FPS cap must be an integer from 1 to 1000')
        if fps_cap is not None or any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only,collection_draw)) or mission_mode is not None:
            raise GameTestError('gameplay FPS checks require a complete native PvP battle')
        extra_mods=tuple(dict.fromkeys((*extra_mods,FPS_MOD)))
    if type(unit_transform_roundtrip) is not bool or type(capture_native_frames) is not bool:
        raise GameTestError('native unit observation flags must be booleans')
    if apsalus_roundtrip and unit_transform_roundtrip:
        raise GameTestError('choose one native transform roundtrip profile')
    if unit_transform_steps is not None:
        if type(unit_transform_steps)is not int or not 1<=unit_transform_steps<=16:
            raise GameTestError('native directed transform steps must be an integer from 1 to 16')
        if apsalus_roundtrip or unit_transform_roundtrip:
            raise GameTestError('choose one native transform path profile')
        if fps_cap is not None or mode_sequence!='1' or play_unit_uid is None or any((bootstrap_only,
                battleship_only,gift_requests_only,deck_only,pages_only,friends_only,room_titles_only)) or mission_mode is not None:
            raise GameTestError('native directed transforms require one complete ordinary battle and an existing owned unit UID')
    if (apsalus_roundtrip or unit_transform_roundtrip) and (mode_sequence != '1' or play_unit_uid is None or any((bootstrap_only,
            battleship_only,gift_requests_only,deck_only,pages_only,friends_only)) or mission_mode is not None):
        raise GameTestError('Native transform roundtrip requires one complete ordinary battle and an existing owned unit UID')
    oc_allocate = validate_oc_allocate(oc_allocate)
    if oc_allocate is not None and (mode_sequence != '1' or play_unit_uid is None or fps_cap is not None or
            any((bootstrap_only,battleship_only,gift_requests_only,deck_only,pages_only,friends_only,
                 room_titles_only,collection_draw,same_room_rematch)) or mission_mode is not None):
        raise GameTestError('--oc-allocate requires one complete ordinary PvP battle and an existing owned unit UID')
    if type(oc_panel) is not bool:
        raise GameTestError('--oc-panel must be a flag')
    if oc_panel and (play_unit_uid is None or fps_cap is not None or mission_mode is not None or
                     not (oc_allocate is not None or (pages_only and page_group == 'hangar'))):
        raise GameTestError('--oc-panel requires --play-unit-uid with either --oc-allocate or --pages-only --page-group hangar')
    if type(oc_challenge) is not bool or (oc_challenge and not (oc_panel or oc_allocate is not None)):
        raise GameTestError('--oc-challenge runs inside the OC panel check (--oc-panel or --oc-allocate)')
    if type(oc_room) is not bool or (oc_room and (oc_allocate is None or random_loadout)):
        raise GameTestError('--oc-room marks the --oc-allocate PvP room with the !oc room-system command')
    if type(g1_room) is not bool or (g1_room and not (
            (oc_allocate is not None and not random_loadout and not oc_room) or
            (pages_only and page_group == 'hangar' and oc_panel))):
        raise GameTestError('--g1-room marks the --oc-allocate PvP room with the !g1 room-system command (not with --oc-room), '
                            'or adds the G改 1.0 page check to the hangar pages with --oc-panel')
    if type(room_switch) is not bool or (room_switch and (oc_allocate is None or random_loadout or oc_room or g1_room)):
        raise GameTestError('--room-switch plays the --oc-allocate PvP battle in an OC, a G改 1.0 and a G改 2.0 room '
                            'back to back (not with --oc-room, --g1-room or --random-loadout)')
    if type(oc_special) is not bool or (oc_special and (
            oc_allocate is None or '5' not in oc_allocated_stats(oc_allocate) or room_switch or random_loadout or
            apsalus_roundtrip or unit_transform_roundtrip or unit_transform_steps is not None)):
        raise GameTestError('--oc-special fires the --play-unit-uid unit\'s special in the --oc-allocate PvP battle and needs '
                            'special points (stat 5); with --oc-room the room plays OC, without it a room-mode server plays the '
                            'current system (negative control); not with --room-switch, --random-loadout or transform probes')
    if oc_special:
        sdk=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
        parsed=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',sdk)
        if not parsed or tuple(map(int,parsed.groups())) < (0,1,33):
            raise GameTestError('--oc-special requires native GameTest client 0.1.33 or newer')
        # The special needs enemies in reach: seven enemy AI and no allied AI hunt the only human.
        if (ai_allies is None and expected_ai == 3 and not random_teams and
                all(value is None for value in (ai_policy, ai_teams, ai_pool_level, ai_difficulty, ai_layout))):
            expected_ai, ai_allies = OC_SPECIAL_EXPECTED_AI, 0
        if (expected_ai != OC_SPECIAL_EXPECTED_AI or ai_allies != 0 or random_teams or
                any(value is not None for value in (ai_policy, ai_teams, ai_pool_level, ai_difficulty, ai_layout))):
            raise GameTestError('--oc-special plays against seven enemy AI and no allied AI '
                                '(--expected-ai 7 --ai-allies 0, the default for this flag)')
    if review_footage and (not quiet or fps_cap is not None):
        raise GameTestError('review footage is a quiet-lane recording profile; it does not apply to visible or FPS runs')
    if capture_native_frames and (fps_cap is not None or any((bootstrap_only,
            battleship_only,gift_requests_only,deck_only,friends_only,room_titles_only))):
        raise GameTestError('native frame capture requires pages or a complete native battle')
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
                pages_only, room_titles_only, collection_draw, random_loadout,
            ))
            or play_unit_uid is not None
            or mission_mode is not None
            or shop_assertions is not None
            or shop_display is not None
            or mode_sequence != "1"
            or retry_count is not None
            or expect_first_fail is not None
            or expected_ai != 3
            or ai_pool_level is not None
            or ai_difficulty is not None
            or ai_layout is not None
            or mission_ai_difficulty is not None
            or room_style_preview is not None
            or operator_flow is not None
        ):
            raise GameTestError("native FPS tests cannot request an authenticated account flow")
    elif fps_mod_enabled is not True:
        raise GameTestError("fps_mod_enabled is valid only with fps_cap")
    shop_binding = None
    if not fps_only:
        play_unit_uid = validate_play_unit_uid(play_unit_uid)
        mission_mode = validate_mission_mode(mission_mode)
        mission_behavior_profile, mission_game_mode_id = validate_mission_behavior(
            mission_behavior_profile, mission_game_mode_id, mission_mode
        )
        if mission_behavior_profile is not None:
            extra_mods=tuple(dict.fromkeys((
                *extra_mods,
                MISSION_BEHAVIOR_MOD,
                BATTLE_AI_MOD,
            )))
        if play_unit_uid is not None and create_test_account:
            raise GameTestError('--play-unit-uid requires an existing account and cannot create a test account')
        if mission_mode is not None and create_test_account:
            raise GameTestError('--mission-mode requires an existing account and cannot create a test account')
        if operator_flow is not None and create_test_account:
            raise GameTestError('operator flow requires a reused registered account and cannot create a test account')
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
            shop_binding is not None,friends_only=friends_only, room_titles_only=room_titles_only,
            room_style_preview=room_style_preview,
            page_group=page_group,
            hangar_confirm_gb=hangar_confirm_gb,
            hangar_sort=hangar_sort,
            mission_respawns=mission_respawns,
            mission_behavior_profile=mission_behavior_profile,
            mission_game_mode_id=mission_game_mode_id,
        )
        if mission_mode is not None:
            if random_loadout:
                raise GameTestError('--mission-mode cannot be combined with the PvP random-loadout scope')
            if expected_ai != 3:
                raise GameTestError('--expected-ai is a PvP setting and cannot change a mission smoke plan')
        if ai_pool_level is not None:
            if expected_ai != 1:
                raise GameTestError('--ai-pool-level requires --expected-ai 1')
            if mission_mode is not None or any((
                bootstrap_only, battleship_only, gift_requests_only, deck_only,
                pages_only, collection_draw, random_loadout, room_titles_only, room_style_preview is not None,
            )):
                raise GameTestError('--ai-pool-level requires an ordinary PvP scenario')
        if ai_layout is not None:
            if (
                expected_ai != 6
                or ai_difficulty != 'Hard'
                or ai_pool_level is not None
            ):
                raise GameTestError(
                    '--ai-layout allied requires --ai-difficulty Hard and --expected-ai 6'
                )
            if mission_mode is not None or fps_gameplay_cap is not None or any((
                bootstrap_only, battleship_only, gift_requests_only, deck_only,
                pages_only, collection_draw, random_loadout, room_titles_only, room_style_preview is not None,
            )):
                raise GameTestError('--ai-layout allied requires an ordinary PvP scenario')
        if ai_difficulty is not None:
            if ai_layout is None and expected_ai != 7:
                raise GameTestError('--ai-difficulty requires --expected-ai 7')
            if mission_mode is not None or any((
                bootstrap_only, battleship_only, gift_requests_only, deck_only,
                pages_only, collection_draw, room_titles_only, room_style_preview is not None,
            )):
                raise GameTestError('--ai-difficulty requires an ordinary PvP scenario')

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
    if bind_staged_loader and not (stage / "client" / "d3d9.dll").is_file():
        raise GameTestError(f"stage lacks framework loader: {stage / 'client' / 'd3d9.dll'}")
    if run_dir.exists():
        raise GameTestError(f"run directory already exists: {run_dir}")
    if is_relative_to(run_dir, client_template):
        raise GameTestError("run directory cannot be inside the client template")
    validate_observer_stage(
        _owned_client_transfer[0]/'client' if _owned_client_transfer else client_template,
        stage,
        extra_mods,
        disabled_mods,
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
            if login_recovery_min_retries is not None and (
                    registration_state not in MISSION_EXISTING_ACCOUNT_STATES or
                    resolved_retry_count != 1 or resolved_expect_first_fail):
                raise GameTestError('login recovery requires one native attempt with a registered existing account')
            if play_unit_uid is not None and pending_first_login:
                raise GameTestError('--play-unit-uid requires an already registered account profile')
            if mission_mode is not None:
                if registration_state not in MISSION_EXISTING_ACCOUNT_STATES:
                    raise GameTestError(
                        '--mission-mode requires a registered or explicitly imported existing account profile'
                    )
                if resolved_retry_count != 1 or resolved_expect_first_fail:
                    raise GameTestError('--mission-mode forbids registration retry behavior')
            if operator_flow is not None:
                if registration_state not in MISSION_EXISTING_ACCOUNT_STATES:
                    raise GameTestError(
                        'operator flow requires a registered or explicitly imported existing account profile'
                    )
                if resolved_retry_count != 1 or resolved_expect_first_fail:
                    raise GameTestError('operator flow forbids registration retry behavior')

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
            ai_allies,
            random_teams,
            ai_policy,
            ai_teams,
            ai_pool_level,
            ai_difficulty,
            ai_layout,
            mission_respawns,
            mission_behavior_profile,
            mission_game_mode_id,
            mission_ai_difficulty,
            review_footage=review_footage,
        )
        if oc_allocate is not None:
            plan['oc_allocate'] = oc_allocate
        if oc_panel:
            plan['oc_panel'] = True
        if oc_challenge:
            plan['oc_challenge'] = True
        if server_session is not None:
            if (not isinstance(server_session,dict) or
                    not re.fullmatch(r'[0-9a-f]{4}' if random_loadout else r'gt[0-9a-f]{4}',str(server_session.get('nonce',''))) or
                    not re.fullmatch(r'[0-9a-f-]{36}',str(server_session.get('run_id',''))) or
                    not server_session.get('pids') or
                    any(type(p) is not int or p<=0 for p in server_session['pids']) or
                    any((bootstrap_only,pages_only,mission_mode is not None)) or
                    (random_loadout and not re.fullmatch(r'[0-9a-f]',str(server_session.get('room_suffix',''))))):
                raise GameTestError('Reusable server session requires a verified ordinary PvP observer binding')
            # Keep the server's existing nonce while adding this fresh run's
            # full random suffix: twenty native bytes, unique per client run.
            if not random_loadout:
                plan['room_title']=server_session['nonce']+plan['room_title'][2:]
                if len(plan['room_title'].encode('ascii'))>20:
                    raise GameTestError('Reusable native room title exceeds twenty bytes')
        if random_loadout and any((
            bootstrap_only, battleship_only, gift_requests_only, deck_only,
            pages_only, room_titles_only, mission_mode is not None,
        )):
            raise GameTestError('random loadout requires a real PvP scenario')
        if random_loadout:
            # Keep the real title within the native twenty-byte limit. The run
            # UUID still owns all receipts; this four-hex nonce has the same
            # sixteen random bits as the ordinary gt-prefixed nonce.
            plan['title_nonce']=(server_session['nonce']+server_session['room_suffix']) if server_session else secrets.token_hex(2)
            allies=plan.get('ai_allies',expected_ai//2)
            plan['room_title']=random_loadout_room_title(plan['title_nonce'], plan)
            plan['random_loadout']=True
        if same_room_rematch:plan['same_room_rematch']=True
        if oc_room:
            # Room systems: the !oc command makes the battle play OC on a server
            # whose OC World Mod runs in room mode.
            plan['room_title']+=' !oc'
            plan['oc_room']=True
        if g1_room:
            # G改 1.0: the play unit's G改 1.0 loadout replaces its custom skills.
            if oc_allocate is not None:
                plan['room_title']+=' !g1'
            plan['g1_room']=True
        if room_switch:
            # The base title is the G改 2.0 round; the OC and G改 1.0 rounds add
            # their command to it.
            plan['room_switch']=True
        if oc_special:
            # The native player probe fires the play unit's special in the battle.
            plan['oc_special']=True
        if any(len(title.encode('utf-8'))>20 for title in (
                room_switch_titles(plan['room_title']) if room_switch else (plan['room_title'],))):
            raise GameTestError('Native GameTest room title exceeds twenty bytes; use a shorter rule or separate scenarios')
        plan['network_policy']=network_policy
        if route_binding is not None:
            plan.update(route_binding)
        if shop_binding is not None:
            plan.update(shop_binding)
        plan['quiet']=quiet
        if room_style_preview is not None:
            plan['room_style_preview'] = room_style_preview
        for flag,value in (
            ('bootstrap_only',bootstrap_only),
            ('battleship_only',battleship_only),
            ('gift_requests_only',gift_requests_only),
            ('deck_only',deck_only),
            ('pages_only',pages_only),
            ('friends_only',friends_only),
            ('room_titles_only',room_titles_only),
            ('collection_draw',collection_draw),
        ):
            if value:plan[flag] = True
        if page_group is not None:
            plan['page_group'] = page_group
        if hangar_confirm_gb:
            plan['hangar_confirm_gb'] = True
        if hangar_sort:
            plan['hangar_sort'] = True
        if login_recovery_min_retries is not None:
            plan.update({
                'login_recovery': True,
                'login_recovery_min_retries': login_recovery_min_retries,
                'login_recovery_hold_after_completion': login_recovery_hold_after_completion,
            })

    if operator_flow is not None:
        if unit_scenario is not None:
            raise GameTestError('operator flow cannot be combined with a native unit scenario')
        try:
            plan.update(operator_scenarios.bind(operator_flow, plan))
        except ValueError as error:
            raise GameTestError(str(error)) from error
    if unit_scenario is not None:
        if isinstance(unit_scenario,dict) and unit_scenario.get('detail_only'):
            observer=manifest_mod(stage/'client/sdgo/mods/game_test_client/mod.ini').get('version','')
            match=re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(?:[-+].*)?',observer)
            if not match or tuple(map(int,match.groups()))<(0,1,34):
                raise GameTestError('detail-only encyclopedia inspection requires native observer 0.1.34 or newer')
        try:plan.update(unit_scenarios.bind(unit_scenario,plan))
        except ValueError as error:raise GameTestError(str(error)) from error
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
            def ignore_client_files(directory,names):
                ignored=copy_ignore(directory,names)
                if Path(directory).resolve()==client_template:
                    ignored.update(name for name in names if unused_direct_executable(Path(name),launcher_path or ''))
                return ignored
            shutil.copytree(client_template, client, ignore=ignore_client_files,copy_function=copy_client_file)
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
        overlay_observer(
            client, stage, extra_mods, disabled_mods,
            fps_mod_enabled=fps_mod_enabled if fps_only else None,
            bind_staged_loader=bind_staged_loader,
            skip_staged_mods=tuple(name for name,selected in
                (('unit_catalog',unit_payload is not None),('capsule_catalog',capsule_binding is not None)) if selected))
        for package in client_packages:
            from types import SimpleNamespace
            import base64
            from manager.packages import import_client
            host={'id':'candidate','role':'client','path':str(client)}
            package_workspace=SimpleNamespace(host=lambda identity:host,public=lambda:{})
            import_client(package_workspace,'candidate',base64.b64encode(Path(package).read_bytes()).decode('ascii'))
        if mission_behavior_profile is not None:
            configure_mission_behavior_client(
                client,
                mission_behavior_profile,
            )
        capsule_install=None
        if capsule_binding is not None:
            capsule_install=capsule_packages.install(capsule_binding,client)
        unit_install=None
        if unit_payload is not None:
            # Install authored data through the ordinary trusted installer
            # before any runtime inputs are sealed. Never mutate a sealed run.
            from owner.units.deployment import install_package
            unit_install=install_package(client,unit_payload)
        observer_manifest=client/'sdgo/mods'/TEST_MOD/'mod.ini'
        observer_config=read_ini(observer_manifest)
        if not observer_config.has_section('settings'):observer_config.add_section('settings')
        observer_config['settings']['ApsalusRoundtrip']='1' if apsalus_roundtrip else '0'
        observer_config['settings']['UnitTransformRoundtrip']='1' if unit_transform_roundtrip else '0'
        observer_config['settings']['UnitDirectedTransforms']='1' if unit_transform_steps is not None else '0'
        observer_config['settings']['UnitTransformSteps']=str(unit_transform_steps or 3)
        observer_config['settings']['FpsGameplayCap']=str(fps_gameplay_cap or 0)
        # OC special: the native player probe charges the real SP gauge with continuous primary fire
        # and the lock action on AI targets, then selects and fires the special slot.
        observer_config['settings']['BattlePlayerSpecial']='1' if oc_special else '0'
        if oc_special:
            observer_config['settings'].update(OC_SPECIAL_OBSERVER_SETTINGS)
        # PvP map number selected on the native room page; 0 keeps the quick-room random-map policy.
        if type(battle_map) is not int or not 0<=battle_map<=255:raise GameTestError('--battle-map must be a MapNo in 0..255')
        observer_config['settings']['BattleMap']=str(battle_map)
        for key,value in unit_scenarios.settings(plan.get('unit_scenario')).items():
            observer_config['settings'][key]=value
        if capture_native_frames:observer_config['settings']['CaptureNativeFrames']='1'
        write_ini(observer_manifest,observer_config)
        if 'unit_scenario' in plan:
            atomic_write(client/'sdgo/gametest'/unit_scenarios.INPUT,unit_scenarios.encoded(plan['unit_scenario']).decode('utf-8'))
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
        if 'operator_flow' in plan:
            try:
                installed_operator = operator_scenarios.install(client, plan, atomic_write)
            except ValueError as error:
                raise GameTestError(str(error)) from error
            if installed_operator != {
                field: plan[field] for field in OPERATOR_PLAN_FIELDS
            }:
                raise GameTestError('installed operator inputs differ from their sealed plan')
        recording = configure_sfx_only(client) if sfx_only else None
        if quiet:configure_muted_audio(client,review_footage=review_footage,
                                       battle_screen_res=SEVERAL_BATTLES_SCREEN_RES if room_switch else None)
        if fps_only:configure_fps_test(client, fps_cap)
        if room_titles_only:
            # Bind protocol diagnostics before collecting immutable hashes.
            # The runner must never toggle logging in a sealed client.
            settings_path = client / "sdgo" / "sdgo.ini"
            settings = read_ini(settings_path)
            if not settings.has_section("core"):
                settings.add_section("core")
            settings["core"]["log_level"] = "0"
            write_ini(settings_path, settings)
        if fps_gameplay_cap is not None:
            from manager.storage import atomic as atomic_bytes, patch_client_timer_fps
            config=client/'config.xml'
            atomic_bytes(config,patch_client_timer_fps(config.read_bytes(),fps_gameplay_cap))
        write_job(client, run_id, plan, profile, profile_path)

        entry = client / (launcher_path or GAME_EXE)
        if not entry.is_file() or not is_relative_to(entry.resolve(), client.resolve()):
            raise GameTestError(f"configured process entry is not inside the copied client: {entry}")
        immutable, initial = collect_hashes(client,launcher_path or '')
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
        if unit_payload is not None:
            metadata['unit_package']={'path':str(unit_package),'sha256':hashlib.sha256(unit_payload).hexdigest(),
                                      'bytes':len(unit_payload),'revision':unit_install['receipt']['revision']}
        if capsule_install is not None:metadata['capsule_package']=capsule_install
        if server_session is not None:
            metadata['server_session']=server_session
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
    rebind_existing_account=options.pop('rebind_existing_account',False)
    if type(rebind_existing_account) is not bool:
        raise GameTestError('existing-account rebind flag must be boolean')
    raw=run_dir.absolute()
    if raw.is_symlink() or (hasattr(raw,'is_junction') and raw.is_junction()):
        raise GameTestError('client reuse rejects linked run directories')
    run_dir=raw.resolve();owned=OWNED_BUILD_ROOT.resolve()
    if run_dir==owned or not is_relative_to(run_dir,owned):
        raise GameTestError('client reuse requires an owned build run, never the player client')
    if options.get('create_test_account'):
        raise GameTestError('client reuse cannot register another account')
    if options.get('bind_staged_loader') and not (stage.resolve()/'client'/'d3d9.dll').is_file():
        raise GameTestError('client reuse requires the explicitly bound staged loader')
    # Preflight the recycler before changing paths or accounts.
    try:__import__('send2trash')
    except ImportError as error:raise GameTestError('install Send2Trash before client reuse; no files changed') from error
    # File transfer does not execute the previous observer. Its original sealed
    # contract can predate new/reordered cases; the new preparation below is
    # still validated against the complete current contract before it can run.
    previous=validate_metadata(run_dir, for_reuse=True);verify_hashes(previous,for_reuse=True)
    client_root=run_dir/'client'
    fps_only=options.get('fps_cap') is not None
    if fps_only!=(previous.get('suite')==FPS_SUITE):
        raise GameTestError('reuse cannot switch between account-free FPS and authenticated suites')
    validate_observer_stage(
        client_root,stage.resolve(),options.get('extra_mods',()),
        options.get('disabled_mods',()),
        fps_mod_enabled=options.get('fps_mod_enabled',True) if fps_only else None)
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
        # Windows can recycle a completed game's PID into a protected system
        # process whose full path cannot be queried. Toolhelp still supplies
        # its native image name. A different name proves this is not our game;
        # an unknown name or a matching game name remains fail-closed.
        name=row.get('name')
        ambiguous_previous=(row['pid']==previous_pid and not value and
                            (not name or name.casefold()==image.name.casefold()))
        if (value and is_relative_to(Path(value).resolve(),run_dir)) or ambiguous_previous:
            raise GameTestError('the owned client may still be running; reuse made no changes')
    account_lock_path=None;target_account_lock_path=None;account_rebound=False
    if fps_only:
        if options.get('account_profile') is not None:
            raise GameTestError('account-free FPS reuse does not accept an account profile')
        if rebind_existing_account:
            raise GameTestError('account-free FPS reuse cannot rebind an account')
    else:
        account_lock_path=validate_private_account(Path(previous['client']['root']),previous['plan'])
        profile=account_lock_path.with_name(account_lock_path.name.removesuffix('.run.lock'))
        requested=Path(options.get('account_profile') or profile).expanduser().resolve()
        if requested!=profile:
            if not rebind_existing_account:
                raise GameTestError('reuse keeps the same persistent account profile; explicit existing-account rebinding is required')
            candidate=load_account_profile(requested)
            if (candidate.get('registration','state',fallback='') not in MISSION_EXISTING_ACCOUNT_STATES or
                    options.get('retry_count') not in (None,1) or options.get('expect_first_fail')):
                raise GameTestError('rebinding requires an already registered account without registration retries')
            profile=requested;account_rebound=True
            target_account_lock_path=account_profile_lock_path(profile)
        elif rebind_existing_account and options.get('account_profile') is None:
            raise GameTestError('existing-account rebinding requires an explicit account profile')
        options['account_profile']=profile
    if previous.get('assets') and options.get('asset_baseline') is None:
        options['asset_baseline']=Path(previous['assets']['baseline'])
    archive=owned/'gametest-current';archive.mkdir(exist_ok=True)
    if archive.is_symlink() or (hasattr(archive,'is_junction') and archive.is_junction()):
        raise GameTestError('client reuse rejects a linked receipt archive')
    lock,token=acquire_lock(run_dir);account_locks=[]
    holding=run_dir.parent/('.gametest-reusing-'+uuid.uuid4().hex)
    try:
        for path in sorted({p for p in (account_lock_path,target_account_lock_path) if p is not None},key=lambda p:str(p).casefold()):
            account_locks.append(acquire_exclusive_lock(path,'persistent account'))
        prior={'run_id':previous['run_id'],'source':previous.get('source'),'plan':previous['plan'],
               'immutable_hashes':previous['client']['immutable_hashes'],'execution':previous.get('execution')}
        prior['required_cases']=read_ini(client_root/'sdgo/gametest/run.ini')['run']['required_cases'].split(',')
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
        if account_rebound:metadata['reuse']['rebound_existing_account']=True
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
        for account_lock,account_token in reversed(account_locks):release_owned_lock(account_lock,account_token)


def validate_metadata(run_dir: Path, *, for_reuse: bool = False) -> dict:
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
    public_plan = read_public_plan(root, historical_cases=for_reuse)
    public_run = read_ini(root / "sdgo" / "gametest" / "run.ini")["run"]
    for field in ('unit_scenario','unit_scenario_sha256'):
        if (field in plan)!=(field in public_plan) or plan.get(field)!=public_plan.get(field):
            raise GameTestError('run.json cannot change the sealed native unit scenario')
    unknown_operator_fields = {
        field for field in plan if field.startswith("operator_")
    } - set(OPERATOR_PLAN_FIELDS)
    if unknown_operator_fields:
        raise GameTestError("run.json contains unknown operator flow fields")
    for field in OPERATOR_PLAN_FIELDS:
        if (
            (field in plan) != (field in public_plan)
            or (
                field in public_plan
                and (
                    type(plan[field]) is not type(public_plan[field])
                    or plan[field] != public_plan[field]
                )
            )
        ):
            raise GameTestError("run.json cannot change the sealed operator flow")
    if public_run.get("id") != run_id:
        raise GameTestError("run.json run_id disagrees with run.ini")
    fps_only = "fps_cap" in public_plan
    expected_suite = FPS_SUITE if fps_only else metadata_suite_for_plan(public_plan)
    if metadata.get("suite") != expected_suite:
        raise GameTestError("run.json suite disagrees with the sealed normal-flow plan")
    public_shop_fields = set()
    if plan.get('room_style_preview') != public_plan.get('room_style_preview'):
        raise GameTestError('run.json cannot change the sealed room-style preview scope')
    if plan.get('page_group') != public_plan.get('page_group'):
        raise GameTestError('run.json cannot change the sealed page group')
    if (type(plan.get('oc_special', False)) is not bool or
            plan.get('oc_special', False) != public_plan.get('oc_special', False)):
        raise GameTestError('run.json cannot change the sealed OC special battle')
    observer_manifest = root / 'sdgo' / 'mods' / TEST_MOD / 'mod.ini'
    if public_plan.get('oc_special') or observer_manifest.is_file():
        observer_settings = read_ini(observer_manifest)
        if observer_settings.get('settings', 'BattlePlayerSpecial', fallback='0') != (
                '1' if public_plan.get('oc_special') else '0') or (
                public_plan.get('oc_special') and any(
                    observer_settings.get('settings', key, fallback='') != value
                    for key, value in OC_SPECIAL_OBSERVER_SETTINGS.items())):
            raise GameTestError('the copied observer settings disagree with the sealed OC special battle')
    if fps_only:
        if set(plan) != set(public_plan):
            raise GameTestError("run.json adds fields outside the native FPS plan")
    else:
        if "mission_mode" in public_plan:
            forbidden = {
                "mode_sequence", "expected_ai", "ai_pool_level", "ai_difficulty",
                "ai_layout",
                "bootstrap_only",
                "battleship_only", "gift_requests_only", "deck_only",
                "pages_only", "hangar_confirm_gb", "hangar_sort", "friends_only", "room_titles_only", "collection_draw", "random_loadout",
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
            "mission_ai_level", "mission_respawns", "mission_ai_difficulty",
            *MISSION_CLAIM_FIELDS,
            *MISSION_BEHAVIOR_FIELDS,
        )):
            raise GameTestError("run.json cannot add mission scope to a non-mission plan")
        for flag in (
            'bootstrap_only','battleship_only','gift_requests_only','deck_only',
            'pages_only', 'hangar_confirm_gb', 'hangar_sort', 'friends_only','room_titles_only','collection_draw','random_loadout',
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
        if ('ai_pool_level' in plan) != ('ai_pool_level' in public_plan):
            raise GameTestError('run.json cannot add or remove the configured AI pool level')
        if 'ai_pool_level' in public_plan and (
            type(plan['ai_pool_level']) is not int
            or plan['ai_pool_level'] != public_plan['ai_pool_level']
        ):
            raise GameTestError('run.json cannot change the configured AI pool level')
        if ('ai_difficulty' in plan) != ('ai_difficulty' in public_plan):
            raise GameTestError('run.json cannot add or remove the configured AI difficulty')
        if 'ai_difficulty' in public_plan and (
            type(plan['ai_difficulty']) is not str
            or plan['ai_difficulty'] != public_plan['ai_difficulty']
        ):
            raise GameTestError('run.json cannot change the configured AI difficulty')
        if ('ai_layout' in plan) != ('ai_layout' in public_plan):
            raise GameTestError('run.json cannot add or remove the configured AI layout')
        if 'ai_layout' in public_plan and (
            type(plan['ai_layout']) is not str
            or plan['ai_layout'] != public_plan['ai_layout']
        ):
            raise GameTestError('run.json cannot change the configured AI layout')
        unknown_plan_shop_fields = {
            field for field in plan if field.startswith("shop_")
        } - set(SHOP_PLAN_FIELDS) - {'shop_visual_only'}
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


def verify_hashes(metadata: dict, *, for_reuse: bool = False) -> None:
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
    operator_fields = {field for field in OPERATOR_PLAN_FIELDS if field in plan}
    if operator_fields and operator_fields != set(OPERATOR_PLAN_FIELDS):
        raise GameTestError("operator flow plan is missing sealed input fields")
    if operator_fields:
        required_operator_hashes = {
            operator_scenarios.SHOP_RELATIVE.as_posix(): plan["operator_shop_sha256"],
            operator_scenarios.DISPLAY_RELATIVE.as_posix(): plan["operator_shop_display_sha256"],
        }
        for relative, expected in required_operator_hashes.items():
            if hashes.get(relative) != expected:
                raise GameTestError(
                    f"immutable payload map does not bind sealed operator input: {relative}"
                )
    for relative, expected in hashes.items():
        if not isinstance(relative, str) or not relative:
            raise GameTestError("invalid immutable payload path in run.json")
        relative_path = Path(relative)
        path = (root / relative_path).resolve()
        if relative_path.is_absolute() or not is_relative_to(path, root):
            raise GameTestError(f"immutable payload path escapes the client capsule: {relative}")
        # Old direct-mode capsules included desktop/bootstrap tools which the
        # original game never executes. Reuse migrates to the narrower sealed
        # input set without reading or restoring quarantined helper programs.
        if for_reuse and unused_direct_executable(relative_path,plan.get('launcher_path','')):
            continue
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
                        "name": str(entry.szExeFile),
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


def spawn_entry(metadata: dict, receipt_dir: Path, quiet_runtime=None, *, through_manager=False):
    client = metadata["client"]
    plan = metadata["plan"]
    entry = Path(client["entry"])
    game = Path(client["game"])
    arguments = [str(entry)]
    if entry.resolve() == game.resolve():
        arguments.extend([plan["target_host"], str(plan["login_port"])])
    environment = os.environ.copy()
    environment["SDGO_GAME_TEST_RUN"] = metadata["run_id"]
    environment.pop('SDGO_LAUNCH_PERMIT',None)
    environment.pop('SDGO_GAME_TEST_LAUNCH_GATE',None)
    if through_manager:environment['SDGO_GAME_TEST_LAUNCH_GATE']=metadata['run_id']
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


def stop_owned_entry_process(process, timeout: float = OWNED_PROCESS_EXIT_TIMEOUT) -> int:
    """Stop one held owned process; allow bounded pending kernel teardown."""
    if process is None or process.poll() is not None:
        return 0
    termination_error = None
    try:
        process.kill()
    except OSError as error:
        # A repeated termination can be rejected while the first is pending.
        # Accept it only after the held process handle confirms actual exit.
        termination_error = error
    for attempt in range(2):
        try:
            process.wait(timeout=timeout)
            if process.poll() is None:
                if termination_error is not None:
                    raise termination_error
                raise GameTestError('owned process exit was not confirmed after waiting')
            return attempt
        except subprocess.TimeoutExpired:
            if attempt:
                raise
    raise AssertionError('unreachable owned process wait state')


class NativeClientExitWaiter:
    """Hold the verified owned process identity while its original exit runs."""
    def __init__(self, pid):
        if os.name != 'nt':raise GameTestError('native client cleanup requires Windows')
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        self.kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
        self.kernel.OpenProcess.restype=wintypes.HANDLE
        self.kernel.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD]
        self.kernel.WaitForSingleObject.restype=wintypes.DWORD
        self.kernel.GetExitCodeProcess.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)]
        self.kernel.GetExitCodeProcess.restype=wintypes.BOOL
        self.kernel.CloseHandle.argtypes=[wintypes.HANDLE]
        self.kernel.CloseHandle.restype=wintypes.BOOL
        self.handle=self.kernel.OpenProcess(0x00100000|0x1000,False,pid)
        if not self.handle:raise ctypes.WinError(ctypes.get_last_error())

    def wait(self, timeout=30):
        result=self.kernel.WaitForSingleObject(self.handle,int(timeout*1000))
        if result==0x102:raise GameTestError('the original native client shutdown did not finish within its budget')
        if result!=0:raise ctypes.WinError(ctypes.get_last_error())
        code=wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(self.handle,ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        # The pinned original entry has two final-release return branches:
        # 00491862 returns 0; 004918C8 returns 1. Both still require the exact
        # native destruction receipt below; crash and other exit codes fail.
        if code.value not in (0,1):raise GameTestError(f'original native client shutdown exited with 0x{code.value:08X}')
        return code.value

    def close(self):
        if self.handle:
            handle=self.handle;self.handle=None
            if not self.kernel.CloseHandle(handle):raise ctypes.WinError(ctypes.get_last_error())


def native_record_lifecycle_proof(metadata):
    from tools.game_test_native_record_lifecycle import verify_receipt
    scope=metadata['plan']['unit_scenario']['native_record_lifecycle']
    expected=metadata['client']['immutable_hashes'].get('sdgo/mods/unit_catalog/catalog.bin')
    try:
        return verify_receipt(Path(metadata['client']['root']),scope['ids'],metadata['run_id'],
                              expected_catalog_sha256=expected)
    except (ValueError,OSError) as error:
        raise GameTestError('native record lifecycle verification failed: '+str(error)) from error


def wait_for_manager_game_process(entry_process, expected_game: Path, timeout: float) -> dict | None:
    """Observe a root-bound startup receipt instead of mistaking the gate for the game."""
    from manager import launch
    root=expected_game.parent.resolve();deadline=time.monotonic()+timeout
    handle=getattr(entry_process,'_process_handle',None)or getattr(entry_process,'_handle',None)
    if not handle:raise GameTestError('Manager startup observation requires the held entry process identity')
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetProcessTimes.argtypes=(wintypes.HANDLE,*([ctypes.POINTER(wintypes.FILETIME)]*4))
    times=[wintypes.FILETIME()for _ in range(4)]
    if not kernel.GetProcessTimes(handle,*[ctypes.byref(value)for value in times]):raise ctypes.WinError(ctypes.get_last_error())
    created=(times[0].dwHighDateTime<<32)|times[0].dwLowDateTime
    while time.monotonic()<deadline:
        for path in launch.folder(root).glob('*.result.json'):
            value=storage_json(path)
            if(value.get('origin_pid')!=entry_process.pid or value.get('origin_created')!=created or
               value.get('status')!='launched'or not re.fullmatch('[0-9a-f]{64}',str(value.get('request_sha256','')))):continue
            pid=value.get('pid');image=process_image_path(pid)if type(pid)is int else None
            if image and Path(image).resolve()==expected_game.resolve():
                return {'pid':pid,'image_path':image,'manager_startup':value}
        time.sleep(.1)
    return None


def storage_json(path):
    # A receipt can be replaced between existence and read; malformed JSON is
    # never a matching startup proof.
    try:return json.loads(Path(path).read_bytes())
    except(OSError,ValueError):return {}


def run_prepared(run_dir: Path, timeout: float = 600.0, launch_timeout: float = 30.0, *, on_process=None, on_poll=None, through_manager=False) -> dict:
    run_dir = run_dir.resolve()
    validate_metadata(run_dir)
    lock, token = acquire_lock(run_dir)
    try:
        # Preparation can move/reseal the capsule. Hold its ownership lock while
        # reading the final identity and hashing inputs, not only after hashing.
        metadata = validate_metadata(run_dir)
        verify_hashes(metadata)
        if metadata.get("execution") is not None:
            raise GameTestError("this capsule already has an execution receipt; prepare a fresh run")
        events = Path(metadata["client"]["events"])
        if events.exists():
            raise GameTestError(f"native event file already exists; prepare a fresh run: {events}")
    except BaseException:
        release_owned_lock(lock, token)
        raise
    account_lock = None
    account_token = None
    receipt_dir = run_dir / "receipts"
    entry_process = None
    game_process = None
    quiet_runtime = None
    native_exit_waiter = None
    manager_exit_waiter = None
    manager_child_exited = False
    handles = []
    started = utc_now()
    native_log = Path(metadata["client"]["root"]) / "sdgo/logs/sdgo.log"
    try:
        native_log_stat = native_log.stat()
        native_log_start = native_log_stat.st_size
        native_log_identity = {
            "device": native_log_stat.st_dev,
            "inode": native_log_stat.st_ino,
        }
    except OSError:
        native_log_start = 0
        native_log_identity = None
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
                entry_process, stdout, stderr = spawn_entry(metadata, receipt_dir,quiet_runtime,through_manager=through_manager)
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

            game_process = wait_for_manager_game_process(entry_process,Path(metadata['client']['game']),launch_timeout)if through_manager else wait_for_owned_game_process(
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
                # Direct native entry only. A launcher child's initial thread
                # is a different identity and is deliberately not inferred.
                "entry_initial_thread_id": getattr(entry_process, 'initial_thread_id', 0),
                "game_pid": game_process["pid"],
                "game_image_path": str(Path(game_process["image_path"]).resolve()),
                "status": "running",
                "timed_out": False,
                "error": None,
                "native_log_start": native_log_start,
                "native_log_identity": native_log_identity,
            }
            if through_manager:
                metadata['execution']['entry_initial_thread_id']=0
                metadata['execution']['manager_startup']=game_process['manager_startup']
            if metadata['plan'].get('unit_scenario',{}).get('native_record_lifecycle'):
                native_exit_waiter=NativeClientExitWaiter(game_process['pid'])
            if through_manager:manager_exit_waiter=NativeClientExitWaiter(game_process['pid'])
            if on_process:
                on_process(game_process['pid'], Path(game_process['image_path']))
            metadata["status"] = "running"
            atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))

            deadline = time.monotonic() + max(0.0, timeout)
            status = "running"
            while time.monotonic() < deadline:
                if on_poll:on_poll()
                if has_run_end(events):
                    if native_exit_waiter:
                        completed=[json.loads(line) for line in events.read_text(encoding='utf-8').splitlines()]
                        if completed[-1].get('status')=='pass':
                            metadata['execution']['native_record_lifecycle_exit_code']=native_exit_waiter.wait()
                            metadata['execution']['native_record_lifecycle']=native_record_lifecycle_proof(metadata)
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
            retries=stop_owned_entry_process(entry_process)
            if retries:
                metadata['execution']['exit_wait_retries']=retries
                metadata['execution']['entry_stop_wait_retried']=True
        if game_process and (not entry_process or game_process['pid'] != entry_process.pid):
            cleanup('verified child stop',lambda:terminate_verified_process(game_process["pid"], Path(metadata["client"]["game"])))
        if manager_exit_waiter:
            def confirm_manager_child_exit():
                nonlocal manager_child_exited
                metadata['execution']['manager_child_exit_code']=manager_exit_waiter.wait()
                manager_child_exited=True
            cleanup('Manager-launched child exit',confirm_manager_child_exit)
            cleanup('Manager-launched child observation handle close',manager_exit_waiter.close)
        cleanup('owned entry stop',stop_entry)
        if native_exit_waiter:cleanup('native exit observation handle close',native_exit_waiter.close)
        for handle in handles:
            cleanup('output stream close',handle.close)
        if entry_process and metadata.get('execution') is not None:
            cleanup('entry exit readback',lambda:metadata['execution'].update(entry_exit_code=entry_process.poll()))
        if quiet_runtime:
            cleanup('quiet runtime close',lambda:metadata.setdefault('execution',{}).update(quiet_cleanup=quiet_runtime.close()))
        owned_alive=True
        try:
            owned_alive=bool(entry_process and entry_process.poll() is None)
            if game_process and not manager_child_exited and (not entry_process or game_process['pid']!=entry_process.pid):
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


def native_battle_ai_policy_errors(metadata: dict) -> list[str]:
    execution = metadata.get("execution") or {}
    if execution.get("status") not in {"completed", "game_exited", "timeout", "cleanup_error"}:
        return []
    path = Path(metadata["client"]["root"]) / "sdgo/logs/sdgo.log"
    try:
        with path.open("rb") as stream:
            current_stat = os.fstat(stream.fileno())
            payload = stream.read()
    except OSError:
        return []
    current_identity = {"device": current_stat.st_dev, "inode": current_stat.st_ino}
    offset = execution.get("native_log_start", 0)
    if (
        execution.get("native_log_identity") != current_identity
        or type(offset) is not int
        or offset < 0
        or offset > len(payload)
    ):
        offset = 0
    lines = payload[offset:].decode("utf-8", errors="replace").splitlines()
    return [line for line in lines if NATIVE_BATTLE_AI_POLICY_ERROR in line][-16:]


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
    policy_errors = native_battle_ai_policy_errors(metadata)
    if policy_errors:
        native["battle_ai_policy_reload"] = {
            "count": len(policy_errors),
            "messages": policy_errors,
        }
        native["errors"].append(
            f"battle_ai rejected {len(policy_errors)} native Lua policy reload(s)"
        )
        native["status"] = "fail"
    if 'play_unit_uid' in metadata['plan']:
        native['requested_play_unit_uid'] = metadata['plan']['play_unit_uid']
    if 'mission_mode' in metadata['plan']:
        native['mission_mode'] = metadata['plan']['mission_mode']
        native['mission_scope'] = metadata['plan']['mission_scope']
        if "mission_behavior_profile" in metadata["plan"]:
            native["mission_behavior_profile"] = metadata["plan"][
                "mission_behavior_profile"
            ]
            native["mission_game_mode_id"] = metadata["plan"][
                "mission_game_mode_id"
            ]
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
    if metadata['plan'].get('unit_scenario',{}).get('native_record_lifecycle'):
        lifecycle={'status':'pending','observer_scope':'registration-and-native-grid-destruction-only',
                   'native_actor_lifecycle_accepted':False,'errors':[]}
        if execution and execution.get('status')=='completed':
            try:
                exit_code=execution.get('native_record_lifecycle_exit_code')
                if type(exit_code) is not int or exit_code not in (0,1):
                    raise GameTestError('original native process exit is unverified')
                proof=native_record_lifecycle_proof(metadata)
                if proof!=execution.get('native_record_lifecycle'):
                    raise GameTestError('native cleanup proof differs from execution readback')
                lifecycle.update(proof)
            except GameTestError as error:
                lifecycle.update(status='fail',errors=[str(error)])
        elif execution and execution.get('status') not in ('running',None):
            lifecycle.update(status='fail',errors=['the native record cleanup scope did not finish'])
        report['layers']['native_record_lifecycle']=lifecycle
    statuses = tuple(layer['status'] for layer in report['layers'].values())
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
    prepare_parser.add_argument(
        '--mission-ai-difficulty',
        choices=('Easy', 'Medium', 'Hard'),
        help='seal the plain mission room title with the native AI difficulty token',
    )
    prepare_parser.add_argument('--mission-respawns',choices=('player','infinite'),help='native mission repeated-death acceptance with the isolated server combat probe')
    prepare_parser.add_argument(
        "--mission-behavior-profile",
        choices=MISSION_BEHAVIOR_PROFILES,
        help=(
            "sealed OC or G改 mission behavior acceptance; requires "
            "--mission-mode and --mission-game-mode-id"
        ),
    )
    prepare_parser.add_argument(
        "--mission-game-mode-id",
        type=int,
        help="exact native GameModeID: a sealed mission behavior target, or an imported mission (map import range)",
    )
    prepare_parser.add_argument("--retry-count", type=int, choices=(1, 2))
    prepare_parser.add_argument("--expected-ai", type=int, default=3)
    prepare_parser.add_argument('--ai-allies',type=int,help='exact allied AI count for the single host; remaining --expected-ai are enemies')
    prepare_parser.add_argument('--random-teams',action='store_true',help='request original native random human teams before AI placement')
    prepare_parser.add_argument('--ai-policy',choices=('fill','enemy'),help='exercise an automatic population command; requires --expected-ai 7 and matching --ai-allies')
    prepare_parser.add_argument('--ai-teams',choices=('coop','versus'),help='exercise cooperative human grouping or preserve native room teams independently of AI counts')
    prepare_parser.add_argument('--same-room-rematch',action='store_true',help='finish a battle, retain the same native room and human host, and start a second full battle before leaving')
    prepare_parser.add_argument(
        "--ai-pool-level",
        type=int,
        help="for ordinary one-AI PvP, select native AI catalog level 1..5",
    )
    prepare_parser.add_argument(
        "--ai-difficulty",
        choices=("Easy", "Medium", "Hard"),
        help="for ordinary seven-AI PvP, select the native room AI difficulty",
    )
    prepare_parser.add_argument(
        "--ai-layout",
        choices=("allied",),
        help="internal Hard-six-AI layout: host with five allied AI and one enemy AI",
    )
    prepare_parser.add_argument('--play-unit-uid',type=int,
                                help='select an existing genuinely owned non-zero uint64 unit UID through native hangar UI before PvP, pages, or mission tests')
    prepare_parser.add_argument("--sfx-only", action="store_true",
                                help="mute BGM and enable effects only in the isolated client")
    prepare_parser.add_argument('--bootstrap-only', action='store_true',
                                help='test real login and onboarding only; does not claim PvP acceptance')
    prepare_parser.add_argument('--battleship-only', action='store_true',help='acquire the initial battleships through native tutorial business')
    prepare_parser.add_argument('--gift-requests-only', action='store_true',help='send configured native mail gift requests; server processing is verified separately')
    prepare_parser.add_argument('--deck-only', action='store_true',help='save and select a native battleship deck using four genuinely owned unit UIDs from the private account profile')
    prepare_parser.add_argument('--room-titles-only',action='store_true',help='test all live room-title inputs, reopen and full server-confirmed titles')
    prepare_parser.add_argument('--room-style-preview', choices=('pvp', 'mission'), help='capture the genuine legacy room after native creation; requires --room-titles-only and a preview Mod')
    prepare_parser.add_argument('--pages-only', action='store_true',help='visit the real hangar and verify Mod inventory, preview and cancel without exchanging units')
    prepare_parser.add_argument('--capture-native-frames',action='store_true',help='save native frames after each passing page case under the run visual folder for visual review')
    prepare_parser.add_argument('--page-group', choices=PAGE_GROUPS, help='with --pages-only, seal bounded native hangar, preview-failure, or storage claim and gift-origin checks')
    prepare_parser.add_argument('--hangar-confirm-gb', action='store_true',
                                help='task-owned test SQL only: seal the hangar GB preview and two original Confirm actions, then require the live hangar refresh')
    prepare_parser.add_argument('--hangar-sort', action='store_true',
        help='with --pages-only --page-group hangar, run the original 一键整理 and require the live hangar refresh')
    prepare_parser.add_argument('--friends-only',action='store_true',help='run the real multi-client native friend session')
    prepare_parser.add_argument('--collection-draw',action='store_true',help='explicit pages-only extension: purchase one collection draw with existing native funds and verify delivery')
    prepare_parser.add_argument('--shop-assertions',type=Path,
                                help='read-only StockAssertions INI copied into a pages-only GameTest capsule')
    prepare_parser.add_argument('--shop-display',type=Path,
                                help='matching read-only runtime display INI copied into a pages-only GameTest capsule')
    prepare_parser.add_argument(
        '--operator-flow',
        type=Path,
        help='sealed JSON for one imported-operator owner-catalog preview and explicitly authorized purchase',
    )
    prepare_parser.add_argument('--random-loadout',action='store_true',help='use the native !rand room command with the actually owned inventory in the PvP scenario')
    prepare_parser.add_argument('--network-policy',choices=('direct','preserve',ROUTED_LOOPBACK_POLICY),default='direct',
                                help='direct disables inherited routes; routed_loopback installs a sealed 127/8 profile in the copy; preserve tests owner routes')
    prepare_parser.add_argument('--route-profile',type=Path,
                                help='strict net_route INI used only with --network-policy routed_loopback')
    prepare_parser.add_argument('--visible',action='store_true',help='explicit developer opt-in to a visible client; default tests use an isolated desktop and muted audio')
    prepare_parser.add_argument('--review-footage',action='store_true',help='quiet review-footage profile: keep the template post-processing settings and play battles at 1920x1080 for SDGO_GAME_TEST_RECORD_DIR recordings')
    prepare_parser.add_argument('--asset-baseline',type=Path,help='reuse verified read-only native archives from a passed owned build run; never links original player files')
    prepare_parser.add_argument('--reuse-client',action='store_true',help='reuse the stopped owned run-dir client in place with a fresh run UUID and compact prior receipt; no full client copy')
    prepare_parser.add_argument('--rebind-existing-account',action='store_true',help='with --reuse-client and --account-profile, switch the stopped client to another already registered account')
    prepare_parser.add_argument('--include-mod',action='append',default=[],help='explicit staged client Mod to install before runtime hashes freeze')
    prepare_parser.add_argument('--client-package',type=Path,action='append',default=[],help='explicit client package imported into the owned copy before sealing')
    prepare_parser.add_argument('--battle-map',type=int,default=0,help='PvP MapNo chosen on the native room page (0 = quick-room random map)')
    prepare_parser.add_argument('--disable-mod',action='append',default=[],help='explicit client Mod switched off in the owned copy before sealing')
    prepare_parser.add_argument('--fps-gameplay-cap',type=int,help='verify native 60 Hz gameplay and the selected render cap during a full PvP battle')
    prepare_parser.add_argument('--oc-allocate',help="OC six-stat acceptance '<stat 0-5>:<points>': allocate normal OC points to the --play-unit-uid unit through the production client Mod before the PvP battle")
    prepare_parser.add_argument('--oc-panel',action='store_true',help='open the original factory upgrade page for the --play-unit-uid unit at the end (after --oc-allocate or the read-only hangar pages) and drive the OC client panel on it: refresh, normal-point reset, one HP point')
    prepare_parser.add_argument('--g1-room',action='store_true',help='room systems: add the !g1 command to the --oc-allocate PvP room title and equip the play unit\'s G改 1.0 loadout first (World OC Mod in room mode with a G改 1.0 rule set)')
    prepare_parser.add_argument('--room-switch',action='store_true',help='room systems: play the --oc-allocate PvP battle three times in one client session, in an !oc room, a !g1 room and a plain (G改 2.0) room, checking the system each battle applied (World OC Mod in room mode with a G改 1.0 rule set)')
    prepare_parser.add_argument('--oc-room',action='store_true',help='room systems: add the !oc command to the --oc-allocate PvP room title so the battle plays OC (World OC Mod in room mode); without it the room plays the current custom skills')
    prepare_parser.add_argument('--oc-challenge',action='store_true',help='inside the OC panel check, also press the OC challenge (can change the unit\'s OC level)')
    prepare_parser.add_argument('--oc-special',action='store_true',help='fire the --play-unit-uid unit\'s special in the --oc-allocate PvP battle (allocation with stat 5) through the native player probe and check the OC special damage (oc.special) and battle stats (oc.stats); with --oc-room the room plays OC, without it a room-mode server plays the current system as the negative control')
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
            operator_flow = None
            if args.operator_flow is not None:
                try:
                    operator_flow = operator_scenarios.load(args.operator_flow)
                except ValueError as error:
                    raise GameTestError(str(error)) from error
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
                rebind_existing_account=args.rebind_existing_account,
                ai_allies=args.ai_allies,
                random_teams=args.random_teams,
                ai_policy=args.ai_policy,
                ai_teams=args.ai_teams,
                same_room_rematch=args.same_room_rematch,
                ai_pool_level=args.ai_pool_level,
                ai_difficulty=args.ai_difficulty,
                ai_layout=args.ai_layout,
                play_unit_uid=args.play_unit_uid,
                mission_mode=args.mission_mode,
                mission_respawns=args.mission_respawns,
                mission_behavior_profile=args.mission_behavior_profile,
                mission_game_mode_id=args.mission_game_mode_id,
                mission_ai_difficulty=args.mission_ai_difficulty,
                launcher_path=args.launcher_path,
                account_profile=args.account_profile,
                create_test_account=args.create_test_account,
                sfx_only=args.sfx_only,
                review_footage=args.review_footage,
                bootstrap_only=args.bootstrap_only,
                battleship_only=args.battleship_only,
                gift_requests_only=args.gift_requests_only,
                deck_only=args.deck_only,
                pages_only=args.pages_only,
                page_group=args.page_group,
                hangar_confirm_gb=args.hangar_confirm_gb,
                hangar_sort=args.hangar_sort,
                capture_native_frames=args.capture_native_frames,
                friends_only=args.friends_only,
                room_titles_only=args.room_titles_only,
                room_style_preview=args.room_style_preview,
                collection_draw=args.collection_draw,
                shop_assertions=args.shop_assertions,
                shop_display=args.shop_display,
                operator_flow=operator_flow,
                random_loadout=args.random_loadout,
                network_policy=args.network_policy,
                route_profile=args.route_profile,
                quiet=not args.visible,
                asset_baseline=args.asset_baseline,
                fps_cap=args.fps_cap,
                fps_mod_enabled=not args.fps_mod_disabled,
                extra_mods=tuple(args.include_mod),
                client_packages=tuple(args.client_package),
                battle_map=args.battle_map,
                disabled_mods=tuple(args.disable_mod),
                fps_gameplay_cap=args.fps_gameplay_cap,
                oc_allocate=args.oc_allocate,
                oc_panel=args.oc_panel,
                oc_challenge=args.oc_challenge,
                oc_room=args.oc_room,
                g1_room=args.g1_room,
                room_switch=args.room_switch,
                oc_special=args.oc_special,
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
