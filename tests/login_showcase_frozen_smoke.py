"""Exercise the packaged login-showcase Manager panel against a real read-only client baseline."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--client-template", type=Path, required=True)
    parser.add_argument("--manager-version", default="")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    source = args.client_template.resolve(strict=True)
    required = ("GOnline.exe", "GPackData.zdx", "GPackData.zpk")
    missing = [name for name in required if not (source / name).is_file()]
    if missing:
        raise RuntimeError("Client template is missing: " + ", ".join(missing))

    scratch = repo / "build"
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="login-showcase-manager-", dir=scratch) as temporary:
        root = Path(temporary).resolve()
        if root.parent != scratch.resolve() or not root.name.startswith("login-showcase-manager-"):
            raise RuntimeError("Frozen smoke directory escaped the task build root")
        client = root / "client"
        client.mkdir()
        for name in required:
            os.link(source / name, client / name)

        for identity in ("startup_crash_fix", "login_showcase"):
            target = client / "sdgo/mods" / identity
            target.mkdir(parents=True)
            for name in ("mod.ini", "manager.json"):
                shutil.copyfile(repo / "mods" / identity / name, target / name)
            binary = repo / "build/Release" / (identity + ".dll")
            if not binary.is_file():
                raise RuntimeError("Build the candidate " + binary.name + " before the frozen smoke")
            shutil.copyfile(binary, target / binary.name)
        login_ini = client / "sdgo/mods/login_showcase/mod.ini"
        with login_ini.open("a", encoding="utf-8") as stream:
            stream.write("\n; frozen smoke preservation marker\n[private]\nKeep=unchanged\n")

        state = root / "workspace"
        state.mkdir()
        (state / "manager.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "hosts": [
                        {
                            "id": "client",
                            "name": "read-only baseline fixture",
                            "path": str(client),
                            "role": "client",
                        }
                    ],
                    "modules": {},
                }
            ),
            encoding="utf-8",
        )
        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        process = subprocess.Popen(
            [str(args.exe.resolve()), "--workspace", str(state), "--no-browser"],
            cwd=root,
            env=environment,
        )
        base = token = None
        try:
            for _ in range(300):
                if process.poll() is not None:
                    raise RuntimeError("Frozen Manager exited before publishing its session")
                session = state / "manager-session.json"
                if session.is_file():
                    try:
                        base = json.loads(session.read_text(encoding="utf-8"))["url"].rstrip("/")
                        break
                    except (OSError, json.JSONDecodeError):
                        pass
                time.sleep(0.1)
            if not base:
                raise RuntimeError("Frozen Manager did not start")

            def call(path, body=None):
                headers = {
                    "Content-Type": "application/json",
                    "Origin": base,
                    "X-ModManager-Token": token or "",
                }
                request = urllib.request.Request(
                    base + path,
                    None if body is None else json.dumps(body).encode(),
                    headers,
                )
                with urllib.request.urlopen(request, timeout=90) as response:
                    return json.load(response)

            public = call("/api/state")
            token = public["session_token"]
            assert any(panel["id"] == "login_showcase" for panel in public["panels"])
            updates = call("/api/updates")
            if args.manager_version:
                assert updates["current_version"] == args.manager_version

            details = call("/api/settings?host=client&mod=login_showcase")
            assert details["contract"]["provider"] == "login_showcase"
            assert details["contract"]["provider_audience"] == "user"
            assert details["values"]["mode"] == "grades"
            assert details["values"]["grades"] == "SS,SR,SU"

            description = call("/api/providers/login_showcase/config")
            assert description["configured"] and description["embedded_configuration"]
            with urllib.request.urlopen(base + "/panels/login_showcase.js", timeout=30) as response:
                panel = response.read().decode("utf-8")
            assert all(text in panel for text in ("所有机体", "最近注册", "指定机体", "搜索机体名称"))
            with urllib.request.urlopen(base + "/app.js", timeout=30) as response:
                app = response.read().decode("utf-8")
            assert "provider_audience==='user'" in app

            catalog = call("/api/providers/login_showcase/api/login-showcase")
            assert catalog["catalog_count"] == len(catalog["catalog"]) > 0
            assert catalog["settings"]["grades"] == ["SS", "SR", "SU"]
            expected = [
                item
                for item in catalog["catalog"]
                if item["grade"] in {"SS", "SR", "SU"}
            ]
            assert catalog["selection"]["count"] == len(expected) > 0
            assert all(item["name"].strip() for item in catalog["catalog"])

            settings = dict(catalog["settings"])
            settings.update({"mode": "recent", "recent_count": 3})
            saved = call(
                "/api/providers/login_showcase/api/login-showcase",
                {
                    "host_id": "client",
                    "settings": settings,
                    "revisions": catalog["revisions"],
                    "catalog_revision": catalog["catalog_revision"],
                },
            )
            assert saved["saved"] and saved["activation"] == "restart_required"
            assert saved["selection"]["count"] == min(3, catalog["catalog_count"])
            text = login_ini.read_text(encoding="utf-8")
            assert "Mode=recent" in text and "RecentCount=3" in text
            assert "; frozen smoke preservation marker" in text and "Keep=unchanged" in text

            call("/api/exit", {})
            process.wait(timeout=30)
            assert process.returncode == 0
            print(
                "PASS frozen login-showcase Manager: packaged player panel, real baseline names, "
                "default SS/SR/SU pool, recent mode save and unknown INI preservation"
            )
        finally:
            if process.poll() is None:
                if base and token:
                    try:
                        call("/api/exit", {})
                    except (OSError, urllib.error.URLError, ValueError):
                        pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == "__main__":
    main()
