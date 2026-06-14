"""Desktop automation management command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
from datetime import datetime, timezone
from functools import partial
import http.server
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def serve_directory(path: Path, *, host: str, port: int) -> None:
    handler = partial(http.server.SimpleHTTPRequestHandler, directory=str(path))
    with http.server.ThreadingHTTPServer((host, port), handler) as server:
        server.serve_forever()


def _path_is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def desktop_publish_root_from_config(config: dict) -> Path:
    return Path(config["desktop_automation"]["artifact_root"]).expanduser().resolve() / "_published"


def desktop_serve_state_dir(publish_root: Path) -> Path:
    return publish_root / "_serve"


def desktop_serve_state_path(publish_root: Path, label: str) -> Path:
    safe_label = "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "-" for ch in label.strip())
    return desktop_serve_state_dir(publish_root) / f"{safe_label or 'desktop-proof'}.json"


def process_is_running(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _append_unique(items: list[str], value: str | None) -> None:
    value = (value or "").strip()
    if value and value not in items:
        items.append(value)


def _find_executable_path(
    name: str,
    *,
    which_fn: Callable[[str], str | None] = shutil.which,
    extra_paths: tuple[str, ...] = (),
) -> str | None:
    found = which_fn(name)
    if found:
        return found
    for candidate in extra_paths:
        path = Path(candidate)
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    return None


def _find_macos_skia_library(repo_root: Path) -> Path | None:
    skia_roots: list[Path] = []
    env_skia_dir = os.environ.get("SKIA_DIR")
    if env_skia_dir:
        skia_roots.append(Path(env_skia_dir).expanduser())
    skia_roots.append(repo_root / "external" / "skia-build")

    seen: set[Path] = set()
    for root in skia_roots:
        root = root.resolve() if root.exists() else root
        if root in seen:
            continue
        seen.add(root)
        for candidate in (
            root / "build" / "mac-gpu" / "lib" / "Release" / "libskia.a",
            root / "mac-gpu" / "lib" / "Release" / "libskia.a",
            root / "mac" / "lib" / "libskia.a",
            root / "lib" / "libskia.a",
            root / "libskia.a",
        ):
            if candidate.is_file():
                return candidate
    return None


def desktop_serve_candidate_hosts(
    bind_host: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    which_fn: Callable[[str], str | None] = shutil.which,
    environ: Mapping[str, str] = os.environ,
    hostname_fn: Callable[[], str] = socket.gethostname,
) -> list[str]:
    hosts: list[str] = []
    if bind_host in {"0.0.0.0", "::"}:
        _append_unique(hosts, "127.0.0.1")
        hostname = hostname_fn()
        _append_unique(hosts, hostname)
        if hostname and "." not in hostname:
            _append_unique(hosts, f"{hostname}.local")
        explicit_hosts = environ.get("PULP_DESKTOP_SERVE_HOSTS") or environ.get("PULP_DESKTOP_SERVE_PUBLIC_HOSTS")
        if explicit_hosts:
            for item in explicit_hosts.split(","):
                _append_unique(hosts, item)
        if which_fn("tailscale"):
            try:
                result = run_fn(["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=3, check=False)
                if result.returncode == 0:
                    for line in (result.stdout or "").splitlines():
                        _append_unique(hosts, line)
            except (OSError, subprocess.SubprocessError):
                pass
    else:
        _append_unique(hosts, bind_host)
    return hosts


def desktop_serve_candidate_urls(bind_host: str, port: int, **kwargs) -> list[str]:
    return [f"http://{host}:{port}/" for host in desktop_serve_candidate_hosts(bind_host, **kwargs)]


def find_available_desktop_serve_port(host: str, preferred_port: int, *, attempts: int = 100) -> int:
    start = max(1, int(preferred_port or 8765))
    for offset in range(max(1, attempts)):
        candidate = start + offset
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.bind((host, candidate))
            return candidate
        except OSError:
            continue
    raise RuntimeError(f"no free desktop proof serve port found starting at {start}")


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n")


def _read_text_tail(path: Path, *, max_chars: int = 4000) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return ""
    if len(text) <= max_chars:
        return text
    return text[-max_chars:]


def start_desktop_serve_process(
    serve_dir: Path,
    *,
    host: str,
    port: int,
    label: str,
    publish_root: Path,
    urls: list[str],
    popen_fn: Callable[..., subprocess.Popen] = subprocess.Popen,
    now_fn: Callable[[], float] = time.time,
    sleep_fn: Callable[[float], None] = time.sleep,
    startup_grace_seconds: float = 0.2,
) -> dict:
    state_path = desktop_serve_state_path(publish_root, label)
    log_dir = desktop_serve_state_dir(publish_root)
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = log_dir / f"{state_path.stem}.out"
    stderr_path = log_dir / f"{state_path.stem}.err"
    stdout_handle = stdout_path.open("a")
    stderr_handle = stderr_path.open("a")
    try:
        process = popen_fn(
            [
                sys.executable,
                "-u",
                "-m",
                "http.server",
                str(port),
                "--bind",
                host,
                "--directory",
                str(serve_dir),
            ],
            stdout=stdout_handle,
            stderr=stderr_handle,
            start_new_session=True,
        )
    finally:
        stdout_handle.close()
        stderr_handle.close()

    payload = {
        "kind": "desktop-proof-serve-process",
        "label": label,
        "pid": int(process.pid),
        "host": host,
        "port": port,
        "directory": str(serve_dir),
        "urls": urls,
        "state_path": str(state_path),
        "stdout": str(stdout_path),
        "stderr": str(stderr_path),
        "started_at_epoch": now_fn(),
        "command": [
            sys.executable,
            "-u",
            "-m",
            "http.server",
            str(port),
            "--bind",
            host,
            "--directory",
            str(serve_dir),
        ],
    }
    if startup_grace_seconds > 0:
        sleep_fn(startup_grace_seconds)
    exit_code = process.poll()
    if exit_code is not None:
        payload.update(
            {
                "status": "failed",
                "exit_code": int(exit_code),
                "error": "desktop proof server exited during startup",
                "stdout_tail": _read_text_tail(stdout_path),
                "stderr_tail": _read_text_tail(stderr_path),
            }
        )
    _write_json(state_path, payload)
    return payload


def persist_desktop_serve_urls(report_dir: Path, urls: list[str]) -> None:
    clean_urls = [str(url) for url in urls if str(url).strip()]
    if not clean_urls:
        return
    for filename in ("index.json", "review-package.json"):
        path = report_dir / filename
        if not path.exists():
            continue
        try:
            payload = json.loads(path.read_text())
        except (json.JSONDecodeError, OSError):
            continue
        if not isinstance(payload, dict):
            continue
        payload["serve_urls"] = clean_urls
        for run in payload.get("runs") or []:
            if not isinstance(run, dict):
                continue
            fallback = run.get("fallback")
            if isinstance(fallback, dict):
                fallback["serve_urls"] = clean_urls
        _write_json(path, payload)


def read_desktop_serve_state(publish_root: Path, label: str) -> dict | None:
    state_path = desktop_serve_state_path(publish_root, label)
    if not state_path.exists():
        return None
    try:
        return json.loads(state_path.read_text())
    except json.JSONDecodeError:
        return {"kind": "desktop-proof-serve-process", "label": label, "state_path": str(state_path), "invalid": True}


def stop_desktop_serve_process(
    publish_root: Path,
    label: str,
    *,
    is_running_fn: Callable[[int], bool] = process_is_running,
    kill_fn: Callable[[int, int], None] = os.kill,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> dict:
    state_path = desktop_serve_state_path(publish_root, label)
    state = read_desktop_serve_state(publish_root, label)
    if not state:
        return {"status": "missing", "label": label, "state_path": str(state_path)}
    pid = int(state.get("pid") or 0)
    was_running = is_running_fn(pid)
    if was_running:
        kill_fn(pid, signal.SIGTERM)
        for _ in range(10):
            sleep_fn(0.1)
            if not is_running_fn(pid):
                break
    stopped = not is_running_fn(pid)
    if stopped:
        state_path.unlink(missing_ok=True)
    return {
        "status": "stopped" if stopped else "still-running",
        "label": label,
        "pid": pid,
        "was_running": was_running,
        "state_path": str(state_path),
    }


VIDEO_PROOF_DEMO_SCENARIOS = (
    {
        "id": "standalone-interaction",
        "title": "Standalone app interaction",
        "platform": "mac",
        "status": "ready",
        "template": "standalone",
        "proves": "A Pulp standalone launches, accepts a click, and visibly changes state.",
        "prepare_command": (
            'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DSKIA_DIR="$(pwd)/external/skia-build" && '
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe standalone-interaction "
            "--source-mode exact-sha "
            "--command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' "
            "--prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DSKIA_DIR=\"$(pwd)/external/skia-build\" && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' "
            "--pulp-app-automation --capture-ui-snapshot --click-view-id bypass-toggle "
            "--duration 6 --video-fps 8 --video-recorder frame-sequence "
            "--label standalone-bypass-toggle --compose-video-proof"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor mac",
        "watch_for": [
            "app window is visible",
            "click marker lands on the intended control",
            "after state or diff proves the response",
        ],
    },
    {
        "id": "audio-inspector-demo",
        "title": "Audio inspector demo proof",
        "platform": "mac",
        "status": "ready",
        "template": "inspector-workflow",
        "proves": "The no-GPU audio inspector demo launches and produces a short Remotion-composed proof without requiring Skia.",
        "prepare_command": (
            "cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target pulp-audio-inspector-demo -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe audio-inspector-demo "
            "--source-mode exact-sha "
            "--command './build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo' "
            "--prepare-command 'cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target pulp-audio-inspector-demo -j$(sysctl -n hw.ncpu)' "
            "--duration 4 --video-fps 8 --video-title 'Audio inspector demo proof' "
            "--video-note 'The proof launches the no-GPU audio inspector demo and records a short validation clip with Remotion context.' "
            "--label audio-inspector-demo-proof --compose-video-proof"
        ),
        "doctor": (
            "python3 tools/local-ci/local_ci.py desktop video-doctor mac "
            "--recipe audio-inspector-demo"
        ),
        "watch_for": [
            "Terminal/app proof shows the demo process running",
            "Remotion title and notes explain this is an audio-inspector smoke proof",
            "local readiness should pass on machines with cmake and the in-tree demo source",
        ],
    },
    {
        "id": "reaper-plugin-editor",
        "title": "Plugin editor in REAPER",
        "platform": "mac",
        "status": "ready",
        "template": "plugin-host",
        "proves": "A real host loads a Pulp plugin and records host/editor context.",
        "prepare_command": (
            "cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target PulpSynth_CLAP -j$(sysctl -n hw.ncpu) && "
            'mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP" && '
            'ln -sfn "$(pwd)/build-video-nogpu/CLAP/PulpSynth.clap" '
            '"$HOME/Library/Audio/Plug-Ins/CLAP/PulpSynth.clap"'
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe reaper-plugin-editor --plugin PulpSynth --plugin-format clap "
            "--source-mode exact-sha "
            "--prepare-command 'cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target PulpSynth_CLAP -j$(sysctl -n hw.ncpu) && "
            "mkdir -p \"$HOME/Library/Audio/Plug-Ins/CLAP\" && "
            "ln -sfn \"$(pwd)/build-video-nogpu/CLAP/PulpSynth.clap\" "
            "\"$HOME/Library/Audio/Plug-Ins/CLAP/PulpSynth.clap\"' "
            "--label reaper-clap-pulpsynth --compose-video-proof"
        ),
        "doctor": (
            "python3 tools/local-ci/local_ci.py desktop video-doctor mac "
            "--recipe reaper-plugin-editor --plugin PulpSynth --plugin-format clap"
        ),
        "watch_for": [
            "REAPER chrome proves real host context",
            "plugin is inserted rather than only opening a blank project",
            "floating plugin editor is focused and captured rather than a blank project window",
        ],
    },
    {
        "id": "inspector-workflow",
        "title": "Developer inspector workflow",
        "platform": "mac",
        "status": "ready",
        "template": "inspector-workflow",
        "proves": "A developer build exposes inspector/audio-inspector state during a visible workflow.",
        "prepare_command": (
            "cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target pulp-audio-inspector-demo -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe inspector-workflow "
            "--source-mode exact-sha "
            "--command './build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo' "
            "--prepare-command 'cmake -S . -B build-video-nogpu -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_GPU=OFF && "
            "cmake --build build-video-nogpu --target pulp-audio-inspector-demo -j$(sysctl -n hw.ncpu)' "
            "--label inspector-open-and-select --compose-video-proof"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor mac",
        "watch_for": [
            "inspector or audio-inspector pane is visible",
            "selected node/probe/meter state is readable",
            "storyboard explains what the viewer should verify",
        ],
    },
    {
        "id": "component-zoom",
        "title": "Component zoom validation",
        "platform": "mac",
        "status": "ready",
        "template": "component-zoom",
        "proves": "The proof highlights one component so the reviewer does not hunt through the full window.",
        "prepare_command": (
            'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DSKIA_DIR="$(pwd)/external/skia-build" && '
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe component-zoom "
            "--source-mode exact-sha "
            "--command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' "
            "--prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DSKIA_DIR=\"$(pwd)/external/skia-build\" && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' "
            "--pulp-app-automation --capture-ui-snapshot --component-id bypass-toggle "
            "--click-view-id bypass-toggle --label component-bypass-toggle "
            "--duration 6 --video-fps 8 --video-recorder frame-sequence --compose-video-proof"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor mac",
        "watch_for": [
            "full-window context appears first",
            "focus box and zoom inset identify the component",
            "action marker aligns with the focused component",
        ],
    },
    {
        "id": "design-parity",
        "title": "Design/source parity",
        "platform": "mac",
        "status": "ready",
        "template": "design-parity",
        "proves": "Source material and the native render are shown side by side for visual review.",
        "command": (
            "python3 tools/local-ci/local_ci.py desktop compose-video /path/to/run/manifest.json "
            "--template design-parity --source-image planning/screenshots/reference.png "
            "--source-label 'Figma reference' --title 'Design parity proof' "
            "--small-video --small-video-budget-mb 10"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor mac",
        "watch_for": [
            "source image and native proof are both readable",
            "critical component/region is explained by notes or storyboard",
            "issue-ready and small fallback videos fit the intended budgets",
        ],
    },
    {
        "id": "ios-simulator",
        "title": "iOS Pulp HostApp interaction",
        "platform": "ios-simulator",
        "status": "ready",
        "template": "mobile-simulator",
        "proves": "A booted iOS Simulator can install and launch a Pulp HostApp, then produce a bounded MP4 proof clip.",
        "prepare_command": (
            "cmake -S . -B build-ios-sim-video-proof -G Xcode -DCMAKE_SYSTEM_NAME=iOS "
            "-DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 "
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 -DCMAKE_BUILD_TYPE=Release "
            "-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=ON && "
            "cmake --build build-ios-sim-video-proof --target PulpSineSynth_HostApp_Embed "
            "--config Release -- -sdk iphonesimulator"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py simulator video "
            "--app build-ios-sim-video-proof/AUv3/Release/PulpSineSynth.app "
            "--bundle-id com.pulp.examples.sinesynth.host "
            "--open-url https://example.com --action-label 'open validation URL' "
            "--label ios-pulpsinesynth-hostapp-proof --duration 6 --video-fps 8 "
            "--compose-video-proof --video-title 'Pulp iOS HostApp proof' "
            "--video-note 'Installs and launches the PulpSineSynth iOS HostApp, then records the simulator while simctl opens a validation URL.' "
            "--small-video"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py simulator video-doctor",
        "watch_for": [
            "device/runtime is identified",
            "PulpSineSynth HostApp install/launch context is visible",
            "open-url action is marked in the proof; coordinate taps need a future automation backend",
        ],
    },
    {
        "id": "android-emulator",
        "title": "Android emulator interaction",
        "platform": "android-emulator",
        "status": "partial",
        "template": "mobile-emulator",
        "proves": "An Android build responds visibly in an emulator proof.",
        "command": (
            "python3 tools/local-ci/local_ci.py android video "
            "--apk android/app/build/outputs/apk/debug/app-debug.apk --package com.pulp.demo "
            "--open-url pulp-demo://validate --action-label 'open validation deep link' "
            "--label android-emulator-proof --duration 8 "
            "--compose-video-proof --video-title 'Android emulator deep-link proof' "
            "--video-note 'The emulator opens the validation deep link during recording.' --small-video"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py android video-doctor",
        "watch_for": [
            "adb serial/model and screenrecord readiness are identified",
            "app launch or current emulator state is visible",
            "open-url action is marked in the proof; coordinate taps need a future automation backend",
        ],
    },
    {
        "id": "linux-xvfb-desktop",
        "title": "Linux/Xvfb desktop proof",
        "platform": "ubuntu",
        "status": "planned",
        "template": "standalone",
        "proves": "A Linux desktop target can record a bounded proof video from the Xvfb display.",
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video ubuntu "
            "--command './build/examples/ui-preview/pulp-ui-preview' "
            "--label linux-xvfb-video-proof --compose-video-proof"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor ubuntu",
        "watch_for": [
            "video-doctor currently fails backend.recorder until the Linux ffmpeg x11grab backend lands",
            "future proof should identify display, window bounds, and x11grab capture settings",
            "use still screenshots on linux-xvfb until video recording is implemented",
        ],
    },
    {
        "id": "windows-session-agent-desktop",
        "title": "Windows session-agent desktop proof",
        "platform": "windows",
        "status": "planned",
        "template": "standalone",
        "proves": "A Windows desktop target can record a bounded proof video from the interactive session.",
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video windows "
            "--command 'build\\\\examples\\\\ui-preview\\\\pulp-ui-preview.exe' "
            "--label windows-session-video-proof --compose-video-proof"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py desktop video-doctor windows",
        "watch_for": [
            "video-doctor currently fails backend.recorder until the Windows ffmpeg ddagrab/gdigrab backend lands",
            "future proof should identify session, display, and capture backend",
            "use still screenshots through the session agent until video recording is implemented",
        ],
    },
)


def _video_matrix_check(
    item: dict,
    *,
    repo_root: Path,
    design_parity_manifest: Path | None = None,
    design_parity_source_image: Path | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
) -> dict:
    checks: list[dict] = []

    def add(name: str, ok: bool, detail: str, *, required: bool = True, remediation: str | None = None) -> None:
        check = {"name": name, "ok": ok, "required": required, "detail": detail}
        if remediation and not ok:
            check["remediation"] = remediation
        checks.append(check)

    prepare_command = str(item.get("prepare_command") or "")
    if "cmake" in prepare_command:
        cmake_path = _find_executable_path(
            "cmake",
            which_fn=which_fn,
            extra_paths=(
                "/opt/homebrew/bin/cmake",
                "/usr/local/bin/cmake",
                "/Applications/CMake.app/Contents/bin/cmake",
            ),
        )
        add(
            "cmake",
            bool(cmake_path),
            cmake_path or "cmake not found on PATH or common macOS install paths",
            remediation="Install CMake or add it to PATH; Homebrew installs are expected at /opt/homebrew/bin/cmake or /usr/local/bin/cmake.",
        )
    if item["id"] in {"standalone-interaction", "component-zoom"}:
        skia_path = _find_macos_skia_library(repo_root)
        expected_path = repo_root / "external" / "skia-build" / "build" / "mac-gpu" / "lib" / "Release" / "libskia.a"
        add(
            "skia-build.libskia",
            bool(skia_path),
            str(skia_path) if skia_path else f"missing required Skia binary: {expected_path}",
            remediation=(
                "Run `python3 tools/scripts/fetch_skia_for_release.py darwin-arm64`, set SKIA_DIR to an existing "
                "Skia bundle, or choose audio-inspector-demo for a no-GPU proof."
            ),
        )
    if item["id"] in {"audio-inspector-demo", "inspector-workflow"}:
        source = repo_root / "examples" / "audio-inspector-demo"
        add(
            "audio-inspector-demo-source",
            source.is_dir(),
            str(source) if source.is_dir() else f"missing source directory: {source}",
            remediation="Run this check from a complete Pulp source checkout that includes examples/audio-inspector-demo.",
        )
    if item["id"] == "design-parity":
        manifest = design_parity_manifest
        source_image = design_parity_source_image or repo_root / "planning" / "screenshots" / "reference.png"
        add(
            "design-parity.run-manifest",
            bool(manifest and manifest.is_file()),
            str(manifest) if manifest and manifest.is_file() else "missing run manifest for design-parity composition",
            remediation="Pass --design-parity-manifest /path/to/run/manifest.json after recording or choosing a run bundle to compare.",
        )
        add(
            "design-parity.source-image",
            source_image.is_file(),
            str(source_image) if source_image.is_file() else f"missing source image: {source_image}",
            remediation="Pass --design-parity-source-image /path/to/source.png, export or provide planning/screenshots/reference.png, or choose another ready proof scenario.",
        )
    if item["id"] == "reaper-plugin-editor":
        reaper_path = Path("/Applications/REAPER.app/Contents/MacOS/REAPER")
        add(
            "reaper.app",
            reaper_path.is_file(),
            str(reaper_path) if reaper_path.is_file() else "REAPER not found at /Applications/REAPER.app",
            required=False,
            remediation="Install REAPER at /Applications/REAPER.app or skip the REAPER proof on this machine.",
        )
    if item["id"] == "ios-simulator":
        xcrun_path = which_fn("xcrun")
        add(
            "xcrun",
            bool(xcrun_path),
            xcrun_path or "xcrun not found on PATH",
            remediation="Install Xcode command line tools and run xcode-select so xcrun is available.",
        )
    if item["id"] == "android-emulator":
        adb_path = which_fn("adb")
        add(
            "adb",
            bool(adb_path),
            adb_path or "adb not found on PATH or under Android SDK",
            remediation="Install Android platform-tools or add adb to PATH before running Android emulator proofs.",
        )
    if item["id"] in {"linux-xvfb-desktop", "windows-session-agent-desktop"}:
        add(
            "backend.recorder",
            False,
            "recorder backend is planned for this platform",
            required=True,
            remediation="Use macOS desktop, iOS Simulator, Android emulator, or still screenshots until this platform recorder backend lands.",
        )

    failed_required = [check for check in checks if check["required"] and not check["ok"]]
    if failed_required:
        status = "blocked"
    elif item.get("status") == "planned":
        status = "planned"
    elif item.get("status") == "partial":
        status = "partial"
    else:
        status = "ready"
    return {"status": status, "checks": checks}


def _latest_published_design_parity_inputs(publish_root: Path) -> dict | None:
    if not publish_root.is_dir():
        return None
    for index_path in sorted(publish_root.glob("*/index.json"), reverse=True):
        try:
            index_payload = json.loads(index_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        report_dir = index_path.parent
        for run in index_payload.get("runs") or []:
            if not isinstance(run, dict):
                continue
            composition = run.get("video_proof_composition") if isinstance(run.get("video_proof_composition"), dict) else {}
            if composition.get("template") != "design-parity":
                continue
            artifacts = run.get("artifacts") if isinstance(run.get("artifacts"), dict) else {}
            manifest_rel = artifacts.get("manifest")
            source_rel = artifacts.get("video_source_image")
            if not isinstance(manifest_rel, str) or not isinstance(source_rel, str):
                continue
            manifest_path = report_dir / manifest_rel
            source_image_path = report_dir / source_rel
            if manifest_path.is_file() and source_image_path.is_file():
                return {
                    "manifest": manifest_path.resolve(),
                    "source_image": source_image_path.resolve(),
                    "report_dir": report_dir.resolve(),
                    "label": index_payload.get("label"),
                    "run_label": run.get("label"),
                }
    return None


def desktop_video_matrix_payload(
    *,
    target: str | None = None,
    scenario: str | None = None,
    status: str | None = None,
    check: bool = False,
    design_parity_manifest: str | Path | None = None,
    design_parity_source_image: str | Path | None = None,
    design_parity_publish_root: str | Path | None = None,
    repo_root: Path | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
) -> dict:
    root = repo_root or Path.cwd()
    design_parity_manifest_path = Path(design_parity_manifest).expanduser().resolve() if design_parity_manifest else None
    design_parity_source_image_path = Path(design_parity_source_image).expanduser().resolve() if design_parity_source_image else None
    design_parity_discovered: dict | None = None
    if check and design_parity_publish_root and (not design_parity_manifest_path or not design_parity_source_image_path):
        design_parity_discovered = _latest_published_design_parity_inputs(Path(design_parity_publish_root).expanduser().resolve())
        if design_parity_discovered:
            if not design_parity_manifest_path:
                design_parity_manifest_path = design_parity_discovered["manifest"]
            if not design_parity_source_image_path:
                default_source = (root / "planning" / "screenshots" / "reference.png").resolve()
                design_parity_source_image_path = default_source if default_source.is_file() else design_parity_discovered["source_image"]
    scenarios: list[dict] = []
    for item in VIDEO_PROOF_DEMO_SCENARIOS:
        if target and item["platform"] != target:
            continue
        if scenario and item["id"] != scenario:
            continue
        row = {key: value for key, value in item.items()}
        declared_status = str(row.get("status") or "")
        if row["id"] == "design-parity":
            manifest_for_command = design_parity_manifest_path or Path("/path/to/run/manifest.json")
            source_for_command = design_parity_source_image_path or (root / "planning" / "screenshots" / "reference.png").resolve()
            source_label_for_command = "Source reference" if design_parity_discovered else "Figma reference"
            row["command"] = (
                "python3 tools/local-ci/local_ci.py desktop compose-video "
                f"{shlex.quote(str(manifest_for_command))} "
                f"--template design-parity --source-image {shlex.quote(str(source_for_command))} "
                f"--source-label {shlex.quote(source_label_for_command)} --title 'Design parity proof' "
                "--small-video --small-video-budget-mb 10"
            )
            if design_parity_discovered and manifest_for_command == design_parity_discovered.get("manifest"):
                row["discovered_report"] = {
                    key: str(value) if isinstance(value, Path) else value
                    for key, value in design_parity_discovered.items()
                }
        if check:
            row["local_readiness"] = _video_matrix_check(
                row,
                repo_root=root,
                design_parity_manifest=design_parity_manifest_path,
                design_parity_source_image=design_parity_source_image_path,
                which_fn=which_fn,
            )
            row["declared_status"] = declared_status
            row["status"] = row["local_readiness"]["status"]
        status_value = row.get("local_readiness", {}).get("status") if check else row.get("status")
        if status and status_value != status:
            continue
        label = row["id"]
        report_placeholder = f"/path/to/published-reports/{label}"
        manifest_placeholder = "/path/to/run/manifest.json"
        row["publish_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop publish --manifest {manifest_placeholder} "
            f"--label {label}-review"
        )
        row["review_issue_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop review-issue {report_placeholder} "
            "--repo owner/repo --check-files"
        )
        row["review_status_command"] = (
            "python3 tools/local-ci/local_ci.py desktop review-status <issue-url> "
            f"--repo owner/repo --manifest {manifest_placeholder} --close-issue"
        )
        row["serve_background_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve {report_placeholder} "
            f"--host 0.0.0.0 --port 8765 --auto-port --background --label {label}-review --json"
        )
        row["serve_status_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve --status --label {label}-review --json"
        )
        row["serve_stop_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve --stop --label {label}-review --json"
        )
        row["published_cleanup_command"] = (
            "python3 tools/local-ci/local_ci.py desktop cleanup "
            "--published --older-than-days 14 --keep-last 3 --json"
        )
        row["review_workflow"] = [
            {"step": "prepare", "command": row.get("prepare_command") or "not required"},
            {"step": "doctor", "command": row["doctor"]},
            {"step": "record_or_compose", "command": row["command"]},
            {"step": "publish", "command": row["publish_command"]},
            {"step": "draft_issue", "command": row["review_issue_command"]},
            {"step": "serve_background", "command": row["serve_background_command"]},
            {"step": "check_server", "command": row["serve_status_command"]},
            {"step": "check_review", "command": row["review_status_command"]},
            {"step": "stop_server", "command": row["serve_stop_command"]},
            {"step": "cleanup_published_reports", "command": row["published_cleanup_command"]},
        ]
        scenarios.append(row)
    return {
        "kind": "desktop-video-proof-demo-matrix",
        "target": target or "all",
        "scenario": scenario or "all",
        "status": status or "all",
        "status_basis": "local_readiness" if check else "declared",
        "checked": check,
        "scenario_count": len(scenarios),
        "scenarios": scenarios,
    }


def desktop_video_matrix_lines(payload: dict) -> list[str]:
    lines = [
        "Desktop validation video proof demo matrix:",
        f"  target: {payload.get('target')}",
        f"  status: {payload.get('status')} ({payload.get('status_basis')})",
        f"  scenarios: {payload.get('scenario_count')}",
    ]
    if not payload.get("scenarios"):
        lines.append("  no scenarios matched the selected filters")
        if payload.get("status") != "all" and not payload.get("checked"):
            lines.append("  tip: add --check to filter by machine-local readiness instead of declared matrix status")
        return lines
    for item in payload.get("scenarios", []):
        lines.extend(
            [
                "",
                f"- {item['id']} [{item['status']}]",
                f"  title: {item['title']}",
                f"  platform: {item['platform']}",
                f"  template: {item['template']}",
            ]
        )
        if item.get("local_readiness"):
            lines.append(f"  local readiness: {item['local_readiness']['status']}")
        lines.extend(
            [
                f"  proves: {item['proves']}",
                f"  doctor: {item['doctor']}",
                f"  prepare: {item.get('prepare_command') or '(none)'}",
                f"  command: {item['command']}",
                f"  publish: {item['publish_command']}",
                f"  review issue: {item['review_issue_command']}",
                f"  review status: {item['review_status_command']}",
                f"  serve background: {item['serve_background_command']}",
                f"  serve status: {item['serve_status_command']}",
                f"  serve stop: {item['serve_stop_command']}",
                f"  cleanup published: {item['published_cleanup_command']}",
            ]
        )
        if item.get("local_readiness"):
            lines.append("  readiness checks:")
            for check in item["local_readiness"].get("checks", []):
                prefix = "PASS" if check.get("ok") else "FAIL"
                required = "required" if check.get("required") else "optional"
                lines.append(f"    - {prefix} {check['name']} ({required}): {check['detail']}")
                if check.get("remediation"):
                    lines.append(f"      remediation: {check['remediation']}")
        lines.append("  watch for:")
        lines.extend(f"    - {value}" for value in item.get("watch_for", []))
    return lines


def desktop_video_matrix_markdown(payload: dict) -> str:
    lines = [
        "# Desktop Validation Video Proof Demo Matrix",
        "",
        f"- Target: `{payload.get('target')}`",
        f"- Status filter: `{payload.get('status')}` (`{payload.get('status_basis')}`)",
        f"- Scenarios: `{payload.get('scenario_count')}`",
        "",
    ]
    if not payload.get("scenarios"):
        lines.append("_No scenarios matched the selected filters._")
        if payload.get("status") != "all" and not payload.get("checked"):
            lines.extend(
                [
                    "",
                    "Add `--check` to filter by machine-local readiness instead of the declared matrix status.",
                ]
            )
        return "\n".join(lines).rstrip() + "\n"
    for item in payload.get("scenarios", []):
        lines.extend(
            [
                f"## {item['title']}",
                "",
                f"- Scenario: `{item['id']}`",
                f"- Status: `{item['status']}`",
                f"- Platform: `{item['platform']}`",
                f"- Remotion template: `{item['template']}`",
            ]
        )
        if item.get("local_readiness"):
            lines.append(f"- Local readiness: `{item['local_readiness']['status']}`")
        lines.extend(
            [
                f"- Proves: {item['proves']}",
                f"- Doctor: `{item['doctor']}`",
                f"- Prepare: `{item.get('prepare_command') or 'none'}`",
                "",
                "Record or compose:",
                "",
                "```bash",
                item["command"],
                "```",
                "",
                "Publish, draft, and serve:",
                "",
                "```bash",
                item["publish_command"],
                item["review_issue_command"],
                item["serve_background_command"],
                item["serve_status_command"],
                item["review_status_command"],
                item["serve_stop_command"],
                item["published_cleanup_command"],
                "```",
            ]
        )
        if item.get("local_readiness"):
            lines.append("")
            lines.append("Readiness checks:")
            for check in item["local_readiness"].get("checks", []):
                prefix = "PASS" if check.get("ok") else "FAIL"
                required = "required" if check.get("required") else "optional"
                lines.append(f"- {prefix} `{check['name']}` ({required}): {check['detail']}")
                if check.get("remediation"):
                    lines.append(f"  - Remediation: {check['remediation']}")
            lines.append("")
        lines.extend(["", "Watch for:"])
        lines.extend(f"- {value}" for value in item.get("watch_for", []))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def cmd_desktop_video_matrix(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict] | None = None,
    print_fn: Callable[[str], None] = print,
) -> int:
    design_parity_publish_root = None
    if getattr(args, "check", False) and load_config_fn is not None:
        try:
            config = load_config_fn()
            design_parity_publish_root = desktop_publish_root_from_config(config)
        except Exception:
            design_parity_publish_root = None
    payload = desktop_video_matrix_payload(
        target=getattr(args, "target", None) or None,
        scenario=getattr(args, "scenario", None) or None,
        status=getattr(args, "status", None) or None,
        check=getattr(args, "check", False),
        design_parity_manifest=getattr(args, "design_parity_manifest", None) or None,
        design_parity_source_image=getattr(args, "design_parity_source_image", None) or None,
        design_parity_publish_root=design_parity_publish_root,
    )
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0
    if getattr(args, "markdown", False):
        print_fn(desktop_video_matrix_markdown(payload).rstrip())
        return 0
    _print_lines(desktop_video_matrix_lines(payload), print_fn=print_fn)
    return 0


def cmd_desktop_status(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_receipt_for_fn: Callable[[str], dict],
    desktop_capabilities_for_fn: Callable[[str, str, dict | None], list[str]],
    desktop_optional_capabilities_fn: Callable[[dict | None], list[str]],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_status_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    windows_tooling_detail_fn: Callable[..., str],
    windows_repo_checkout_detail_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    targets = desktop_cfg.get("targets", {})
    if args.target:
        if args.target not in targets:
            print_fn(f"\nError: unknown desktop target `{args.target}`")
            return 1
        target_names = [args.target]
    else:
        target_names = sorted(targets)

    target_payloads: list[dict] = []
    for name in target_names:
        target = targets[name]
        receipt = desktop_receipt_for_fn(name)
        capabilities = ", ".join(
            desktop_capabilities_for_fn(target["adapter"], target["capability_tier"], target.get("optional"))
        )
        optional_capabilities = desktop_optional_capabilities_fn(target.get("optional"))
        latest = desktop_run_manifests_fn(config, target_name=name)[:1]
        latest_manifest = latest[0] if latest else None
        latest_run = desktop_run_summary_fn(config, latest_manifest) if latest_manifest else None
        latest_proof_matches = desktop_proof_summaries_fn(config, target_name=name, limit=1)
        latest_proof = latest_proof_matches[0] if latest_proof_matches else None
        target_info = {
            "name": name,
            "enabled": target["enabled"],
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "capability_tier": target["capability_tier"],
            "capabilities": desktop_capabilities_for_fn(target["adapter"], target["capability_tier"], target.get("optional")),
            "capabilities_text": capabilities,
            "optional_features": normalize_desktop_optional_config_fn(target.get("optional")),
            "optional_capabilities": optional_capabilities,
            "installed": bool(receipt),
            "installed_at": receipt.get("installed_at", "?") if receipt else None,
            "contract": receipt.get("contract") if receipt else desktop_target_contract_fn(name, target),
            "remote_bootstrap_ready": receipt.get("remote_bootstrap_ready") if receipt else None,
            "remote_tooling_ready": receipt.get("remote_tooling_ready") if receipt else None,
            "remote_repo_checkout_ready": receipt.get("remote_repo_checkout_ready") if receipt else None,
            "tooling_probe": receipt.get("tooling_probe") if receipt else None,
            "repo_checkout_probe": receipt.get("repo_checkout_probe") if receipt else None,
            "latest_run": None,
            "latest_proof": latest_proof,
        }
        if latest_run:
            target_info["latest_run"] = {
                "label": latest_run["label"],
                "completed_at": latest_run["completed_at"],
                "interaction_mode": latest_run["interaction_mode"],
                "run_status": latest_run["run_status"],
                "source_mode": latest_run["source"]["mode"],
                "source_branch": latest_run["source"]["branch"],
                "source_sha": latest_run["source"]["sha"],
                "proof_scope": latest_run["proof_scope"],
                "host": latest_run["host"],
                "screenshot": latest_run["artifacts"]["screenshot"],
                "before_screenshot": latest_run["artifacts"]["before_screenshot"],
                "diff_screenshot": latest_run["artifacts"]["diff_screenshot"],
                "image_change": latest_run["artifacts"]["image_change"],
                "ui_snapshot": latest_run["artifacts"]["ui_snapshot"],
                "video": latest_run["artifacts"].get("video"),
                "video_composed": latest_run["artifacts"].get("video_composed"),
                "bundle_dir": latest_run["artifacts"]["bundle_dir"],
            }
        target_payloads.append(target_info)

    latest_publish_matches = desktop_publish_reports_fn(config, limit=1)
    latest_publish = latest_publish_matches[0] if latest_publish_matches else None
    if getattr(args, "json", False):
        payload = {
            "artifact_root": desktop_cfg["artifact_root"],
            "publish_mode": desktop_cfg["publish_mode"],
            "publish_branch": desktop_cfg["publish_branch"],
            "retention_days": desktop_cfg["retention_days"],
            "latest_publish": latest_publish,
            "targets": target_payloads,
        }
        print_fn(json.dumps(payload, indent=2))
        return 0

    _print_lines(
        desktop_status_lines_fn(
            desktop_cfg,
            target_payloads,
            latest_publish=latest_publish,
            short_sha_fn=short_sha_fn,
            windows_tooling_detail_fn=windows_tooling_detail_fn,
            windows_repo_checkout_detail_fn=windows_repo_checkout_detail_fn,
        ),
        print_fn=print_fn,
    )
    return 0


def cmd_desktop_config_show(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_config_show_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config["desktop_automation"]
    if getattr(args, "json", False):
        print_fn(json.dumps(desktop_cfg, indent=2))
        return 0

    _print_lines(desktop_config_show_lines_fn(desktop_cfg), print_fn=print_fn)
    return 0


def cmd_desktop_config_set(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    save_config_fn: Callable[[dict], None],
    config_path_fn: Callable[[], Path],
    normalize_publish_mode_fn: Callable[[str], str],
    parse_config_bool_fn: Callable[[str], bool],
    normalize_desktop_config_fn: Callable[[dict], dict],
    desktop_config_update_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    desktop_cfg = config.setdefault("desktop_automation", {})
    key = args.key
    raw_value = args.value
    payload_value = None
    try:
        if key == "artifact_root":
            desktop_cfg["artifact_root"] = raw_value
            payload_value = desktop_cfg["artifact_root"]
        elif key == "publish_mode":
            desktop_cfg["publish_mode"] = normalize_publish_mode_fn(raw_value)
            payload_value = desktop_cfg["publish_mode"]
        elif key == "publish_branch":
            desktop_cfg["publish_branch"] = raw_value
            payload_value = desktop_cfg["publish_branch"]
        elif key == "retention_days":
            retention_days = int(raw_value)
            if retention_days < 0:
                raise ValueError("retention_days must be >= 0")
            desktop_cfg["retention_days"] = retention_days
            payload_value = desktop_cfg["retention_days"]
        elif key.startswith("target."):
            parts = key.split(".")
            if len(parts) != 3:
                raise ValueError("Target desktop config keys must look like target.<name>.<field>.")
            _, target_name, field = parts
            target_cfg = desktop_cfg.setdefault("targets", {}).setdefault(target_name, {})
            optional_cfg = dict(target_cfg.get("optional", {}))
            if field in {"webview_driver", "debug_attach", "video_capture", "frame_stats"}:
                optional_cfg[field] = parse_config_bool_fn(raw_value)
            elif field in {"webdriver_url", "debugger_command"}:
                optional_cfg[field] = raw_value
            else:
                raise ValueError(
                    "Unsupported target desktop config field. Use one of: "
                    "target.<name>.webview_driver, target.<name>.webdriver_url, "
                    "target.<name>.debug_attach, target.<name>.debugger_command, "
                    "target.<name>.video_capture, target.<name>.frame_stats."
                )
            target_cfg["optional"] = optional_cfg
            payload_value = optional_cfg[field]
        else:
            raise ValueError(
                f"Unsupported desktop config key `{key}`. Use one of: artifact_root, publish_mode, publish_branch, retention_days, or target.<name>.<field>."
            )
        normalized = normalize_desktop_config_fn(config)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    save_config_fn(normalized)
    if key.startswith("target."):
        _, target_name, field = key.split(".")
        payload_value = normalized["desktop_automation"]["targets"][target_name]["optional"][field]
    else:
        payload_value = normalized["desktop_automation"][key]
    payload = {
        "key": key,
        "value": payload_value,
        "config_path": str(config_path_fn()),
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0

    _print_lines(desktop_config_update_lines_fn(payload), print_fn=print_fn)
    return 0


def cmd_desktop_config(
    args: argparse.Namespace,
    *,
    commands: Mapping[str, Callable[[argparse.Namespace], int]],
    print_fn: Callable[[str], None] = print,
) -> int:
    handler = commands.get(args.desktop_config_command)
    if handler is None:
        print_fn("Error: desktop config subcommand required (show, set)")
        return 1
    return handler(args)


def cmd_desktop_recent(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_recent_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not manifests:
        print_fn("No desktop automation runs found.")
        return 0
    manifests = manifests[: args.limit]
    if getattr(args, "json", False):
        print_fn(json.dumps({"runs": manifests}, indent=2))
        return 0

    run_summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    _print_lines(desktop_recent_lines_fn(run_summaries, short_sha_fn=short_sha_fn), print_fn=print_fn)
    return 0


def cmd_desktop_proof(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    desktop_proof_empty_line_fn: Callable[..., str],
    desktop_proof_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    try:
        proofs = desktop_proof_summaries_fn(
            config,
            target_name=args.target,
            action=args.action,
            source_mode=args.source_mode,
            sha=args.sha,
            branch=args.branch,
            limit=args.limit,
        )
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps({"proofs": proofs}, indent=2))
        return 0

    if not proofs:
        print_fn(
            desktop_proof_empty_line_fn(
                target=args.target,
                action=args.action,
                source_mode=args.source_mode,
                sha=args.sha,
                branch=args.branch,
                short_sha_fn=short_sha_fn,
            )
        )
        return 0

    _print_lines(desktop_proof_lines_fn(proofs, short_sha_fn=short_sha_fn), print_fn=print_fn)
    return 0


def cmd_desktop_publish(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    stage_desktop_publish_report_fn: Callable[..., dict],
    desktop_publish_lines_fn: Callable[[dict], list[str]],
    desktop_serve_candidate_urls_fn: Callable[[str, int], list[str]] = lambda _host, _port: [],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifest_paths = getattr(args, "manifest", None) or []
    manifests = []
    for manifest_arg in manifest_paths:
        manifest_path = Path(manifest_arg).expanduser()
        if not manifest_path.is_file():
            print_fn(f"Error: desktop run manifest not found: {manifest_path}")
            return 1
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print_fn(f"Error: could not read desktop run manifest: {exc}")
            return 1
        artifacts = manifest.setdefault("artifacts", {})
        artifacts.setdefault("bundle_dir", str(manifest_path.parent))
        manifests.append(manifest)
    if not manifests:
        manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not manifests:
        print_fn("No desktop automation runs found.")
        return 0

    if not manifest_paths:
        manifests = manifests[: args.limit]
    output_dir = Path(args.output).expanduser() if args.output else None
    serve_urls = desktop_serve_candidate_urls_fn("0.0.0.0", 8765)
    try:
        report = stage_desktop_publish_report_fn(
            config,
            manifests,
            output_dir=output_dir,
            label=args.label,
            serve_urls=serve_urls,
        )
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    if getattr(args, "json", False):
        print_fn(json.dumps(report, indent=2))
        return 0

    _print_lines(desktop_publish_lines_fn(report), print_fn=print_fn)
    return 0


def cmd_desktop_verdict(
    args: argparse.Namespace,
    *,
    now_iso_fn: Callable[[], str],
    atomic_write_text_fn: Callable[[Path, str], None],
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    manifest_path = Path(args.manifest).expanduser()
    if not manifest_path.is_file():
        print_fn(f"Error: desktop run manifest not found: {manifest_path}")
        return 1
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print_fn(f"Error: could not read desktop run manifest: {exc}")
        return 1
    status = "approved" if getattr(args, "approved", False) else "needs-work"
    reviewed_at = now_iso_fn()
    run_label = str(manifest.get("label") or manifest_path.parent.name)
    review = {
        "status": status,
        "reviewed_at": reviewed_at,
    }
    if args.notes:
        review["notes"] = args.notes
    if args.reviewer:
        review["reviewer"] = args.reviewer
    if args.issue_url:
        review["issue_url"] = args.issue_url
    if status == "approved":
        review["close_review_issue"] = True
    else:
        review["close_review_issue"] = False
        review["follow_up_required"] = True
    markdown_path = manifest_path.parent / "review-verdict.md"
    json_path = manifest_path.parent / "review-verdict.json"
    if status == "approved":
        summary_comment = f"Approved desktop video proof `{run_label}`."
        if args.issue_url:
            summary_comment += f" Review issue: {args.issue_url}."
        if args.notes:
            summary_comment += f" Notes: {args.notes}"
        follow_up = None
    else:
        summary_comment = f"Desktop video proof `{run_label}` needs another iteration."
        if args.notes:
            summary_comment += f" Requested change: {args.notes}"
        follow_up = {
            "kind": "same-issue-checklist",
            "text": f"- [ ] Re-record `{run_label}` after addressing: {args.notes or 'reviewer feedback'}",
        }
    close_result = None
    if getattr(args, "close_issue", False):
        if status != "approved":
            print_fn("Error: --close-issue requires --approved")
            return 1
        if not args.issue_url:
            print_fn("Error: --close-issue requires --issue-url")
            return 1
        close_argv = [
            "gh",
            "issue",
            "close",
            args.issue_url,
            "--comment",
            summary_comment,
            "--reason",
            getattr(args, "close_reason", "completed") or "completed",
        ]
        close_process = run_fn(close_argv, capture_output=True, text=True, check=False)
        close_result = {
            "command": close_argv,
            "returncode": int(close_process.returncode),
            "stdout": close_process.stdout,
            "stderr": close_process.stderr,
        }
        if close_process.returncode != 0:
            detail = (close_process.stderr or close_process.stdout or "gh issue close failed").strip()
            print_fn(f"Error: {detail}")
            return 1
    verdict_payload = {
        "kind": "desktop-video-proof-verdict",
        "manifest": str(manifest_path),
        "status": status,
        "reviewed_at": reviewed_at,
        "reviewer": args.reviewer or None,
        "issue_url": args.issue_url or None,
        "notes": args.notes or None,
        "close_review_issue": bool(review["close_review_issue"]),
        "follow_up_required": bool(review.get("follow_up_required", False)),
        "summary_comment": summary_comment,
        "follow_up": follow_up,
        "issue_close": close_result,
    }
    markdown_lines = [
        f"# Desktop Video Proof Verdict: {status}",
        "",
        f"- Manifest: `{manifest_path}`",
        f"- Run: `{run_label}`",
        f"- Reviewed at: `{reviewed_at}`",
    ]
    if args.reviewer:
        markdown_lines.append(f"- Reviewer: `{args.reviewer}`")
    if args.issue_url:
        markdown_lines.append(f"- Review issue: {args.issue_url}")
    if args.notes:
        markdown_lines.append(f"- Notes: {args.notes}")
    markdown_lines.extend(
        [
            "",
            "## Issue Comment",
            "",
            summary_comment,
        ]
    )
    if follow_up:
        markdown_lines.extend(["", "## Follow-up", "", follow_up["text"]])
    if status == "approved":
        markdown_lines.extend(["", "## Closeout", "", "Close the review issue after posting the summary comment."])
    else:
        markdown_lines.extend(["", "## Closeout", "", "Keep the review issue open until a replacement proof is recorded."])
    atomic_write_text_fn(markdown_path, "\n".join(markdown_lines) + "\n")
    atomic_write_text_fn(json_path, json.dumps(verdict_payload, indent=2) + "\n")
    review["verdict_markdown"] = str(markdown_path)
    review["verdict_json"] = str(json_path)
    review["summary_comment"] = summary_comment
    if follow_up:
        review["follow_up"] = follow_up
    if close_result:
        review["issue_close"] = close_result
    manifest["review"] = review
    atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")
    payload = {
        "manifest": str(manifest_path),
        "review": review,
        "verdict_markdown": str(markdown_path),
        "verdict_json": str(json_path),
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        print_fn(f"Desktop proof verdict recorded: {status}")
        print_fn(f"  manifest: {manifest_path}")
        print_fn(f"  verdict_markdown: {markdown_path}")
        print_fn(f"  verdict_json: {json_path}")
        if args.issue_url:
            print_fn(f"  issue_url: {args.issue_url}")
        if close_result:
            print_fn("  issue_close: closed")
    return 0


def _review_package_path(path_value: str) -> Path:
    path = Path(path_value).expanduser().resolve()
    if path.is_dir():
        path = path / "review-package.json"
    return path


def cmd_desktop_review_issue(
    args: argparse.Namespace,
    *,
    desktop_review_issue_draft_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    package_path = _review_package_path(args.path)
    if not package_path.exists():
        print_fn(f"Error: review package not found: {package_path}")
        return 1
    try:
        review_package = json.loads(package_path.read_text())
    except json.JSONDecodeError as exc:
        print_fn(f"Error: invalid review package JSON: {exc}")
        return 1
    try:
        draft = desktop_review_issue_draft_fn(
            review_package,
            package_path=package_path,
            title=args.title,
            repo=args.repo,
            check_files=getattr(args, "check_files", False),
        )
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1
    body_path = Path(args.body_output).expanduser().resolve() if args.body_output else Path(draft["body_file"])
    json_path = Path(args.json_output).expanduser().resolve() if args.json_output else Path(draft["json_file"])
    draft["body_file"] = str(body_path)
    draft["json_file"] = str(json_path)
    atomic_write_text_fn(body_path, draft["body"])
    create_result = None
    if getattr(args, "create", False):
        create_argv = ["gh", "issue", "create"]
        if args.repo:
            create_argv.extend(["--repo", args.repo])
        create_argv.extend(["--title", draft["title"], "--body-file", str(body_path)])
        for label in getattr(args, "label", []) or []:
            create_argv.extend(["--label", label])
        for assignee in getattr(args, "assignee", []) or []:
            create_argv.extend(["--assignee", assignee])
        process = run_fn(create_argv, capture_output=True, text=True, check=False)
        issue_url = ""
        if process.returncode == 0:
            issue_url = next((line.strip() for line in reversed((process.stdout or "").splitlines()) if line.strip()), "")
        create_result = {
            "command": create_argv,
            "returncode": int(process.returncode),
            "stdout": process.stdout,
            "stderr": process.stderr,
            "issue_url": issue_url or None,
        }
        draft["create_result"] = create_result
        if issue_url:
            draft["issue_url"] = issue_url
        if process.returncode != 0:
            atomic_write_text_fn(json_path, json.dumps(draft, indent=2) + "\n")
            detail = (process.stderr or process.stdout or "gh issue create failed").strip()
            print_fn(f"Error: {detail}")
            return 1
    atomic_write_text_fn(json_path, json.dumps(draft, indent=2) + "\n")
    if args.json:
        print_fn(json.dumps(draft, indent=2))
    else:
        print_fn("Desktop video review issue draft ready:")
        print_fn(f"  title: {draft['title']}")
        print_fn(f"  body_file: {draft['body_file']}")
        print_fn(f"  json_file: {draft['json_file']}")
        print_fn(f"  attachments: {len(draft.get('attachments') or [])}")
        print_fn(f"  fallback_links: {len(draft.get('fallback_links') or [])}")
        print_fn(f"  create_command: {draft['create_command']}")
        if create_result and create_result.get("issue_url"):
            print_fn(f"  issue_url: {create_result['issue_url']}")
    return 0


def _review_status_approval_comment(comments: list[dict], trigger: str = "looks good to me") -> dict | None:
    trigger_lower = trigger.lower()
    for comment in reversed(comments):
        body = str(comment.get("body") or "")
        if trigger_lower in body.lower():
            return comment
    return None


def cmd_desktop_review_status(
    args: argparse.Namespace,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
) -> int:
    argv = ["gh", "issue", "view", args.issue_url, "--json", "state,url,comments"]
    if getattr(args, "repo", None):
        argv.extend(["--repo", args.repo])
    process = run_fn(argv, capture_output=True, text=True, check=False)
    if process.returncode != 0:
        detail = (process.stderr or process.stdout or "gh issue view failed").strip()
        print_fn(f"Error: {detail}")
        return 1
    try:
        issue = json.loads(process.stdout or "{}")
    except json.JSONDecodeError as exc:
        print_fn(f"Error: invalid gh issue view JSON: {exc}")
        return 1
    comments = issue.get("comments") if isinstance(issue.get("comments"), list) else []
    approval_comment = _review_status_approval_comment(comments)
    issue_url = str(issue.get("url") or args.issue_url)
    verdict_command = None
    if approval_comment and getattr(args, "manifest", None):
        manifest = shlex.quote(str(Path(args.manifest).expanduser()))
        quoted_issue_url = shlex.quote(issue_url)
        verdict_command = (
            "python3 tools/local-ci/local_ci.py desktop verdict "
            f"{manifest} --approved --issue-url {quoted_issue_url}"
        )
        if getattr(args, "close_issue", False):
            verdict_command += " --close-issue"
    payload = {
        "kind": "desktop-video-proof-review-status",
        "issue_url": issue_url,
        "state": issue.get("state"),
        "close_trigger": "looks good to me",
        "approved": approval_comment is not None,
        "approval_comment": approval_comment,
        "verdict_command": verdict_command,
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0
    print_fn(f"Desktop video review issue: {issue_url}")
    print_fn(f"  state: {payload['state'] or 'unknown'}")
    print_fn(f"  approved: {str(payload['approved']).lower()}")
    if approval_comment:
        author = approval_comment.get("author")
        if isinstance(author, dict) and author.get("login"):
            print_fn(f"  approved_by: {author['login']}")
        if approval_comment.get("url"):
            print_fn(f"  approval_comment: {approval_comment['url']}")
    if verdict_command:
        print_fn(f"  verdict_command: {verdict_command}")
    elif not approval_comment:
        print_fn("  waiting_for: looks good to me")
    return 0


def _load_review_watch_state(path: Path | None) -> dict:
    if not path or not path.is_file():
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    if not isinstance(payload, dict):
        return {"kind": "desktop-video-proof-review-watch-state", "issues": {}}
    issues = payload.get("issues")
    if not isinstance(issues, dict):
        payload["issues"] = {}
    payload.setdefault("kind", "desktop-video-proof-review-watch-state")
    return payload


def _load_review_manifest_map(path: Path | None) -> dict[str, str]:
    if not path:
        return {}
    payload = json.loads(path.read_text())
    if not isinstance(payload, dict):
        raise ValueError("manifest map must be a JSON object")
    out: dict[str, str] = {}
    for key, value in payload.items():
        if isinstance(value, str):
            out[str(key)] = value
    return out


def _review_manifest_for_issue(issue: dict, manifest_map: Mapping[str, str]) -> str | None:
    keys = [
        str(issue.get("url") or ""),
        str(issue.get("number") or ""),
        f"#{issue.get('number')}" if issue.get("number") is not None else "",
    ]
    for key in keys:
        if key and key in manifest_map:
            return manifest_map[key]
    return None


def _review_verdict_command(issue_url: str, manifest: str | None, *, close_issue: bool) -> str | None:
    if not manifest:
        return None
    command = (
        "python3 tools/local-ci/local_ci.py desktop verdict "
        f"{shlex.quote(manifest)} --approved --issue-url {shlex.quote(issue_url)}"
    )
    if close_issue:
        command += " --close-issue"
    return command


def _review_watch_once(
    args: argparse.Namespace,
    *,
    state_payload: dict,
    manifest_map: Mapping[str, str],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> tuple[dict, dict]:
    list_argv = [
        "gh",
        "issue",
        "list",
        "--state",
        args.state,
        "--label",
        args.label,
        "--json",
        "number,title,url,updatedAt",
    ]
    if getattr(args, "repo", None):
        list_argv.extend(["--repo", args.repo])
    list_process = run_fn(list_argv, capture_output=True, text=True, check=False)
    if list_process.returncode != 0:
        detail = (list_process.stderr or list_process.stdout or "gh issue list failed").strip()
        raise RuntimeError(detail)
    try:
        listed_issues = json.loads(list_process.stdout or "[]")
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid gh issue list JSON: {exc}") from exc
    if not isinstance(listed_issues, list):
        raise RuntimeError("gh issue list did not return a JSON array")

    cached_issues = state_payload.setdefault("issues", {})
    if not isinstance(cached_issues, dict):
        cached_issues = {}
        state_payload["issues"] = cached_issues

    checked_count = 0
    skipped_count = 0
    issues: list[dict] = []
    for listed in listed_issues:
        if not isinstance(listed, dict):
            continue
        issue_url = str(listed.get("url") or listed.get("number") or "")
        issue_number = listed.get("number")
        updated_at = str(listed.get("updatedAt") or "")
        cached = cached_issues.get(issue_url) if issue_url else None
        unchanged = bool(
            cached
            and not getattr(args, "refresh", False)
            and updated_at
            and cached.get("updated_at") == updated_at
        )
        if unchanged:
            skipped_count += 1
            issues.append(
                {
                    "number": issue_number,
                    "title": listed.get("title"),
                    "url": issue_url,
                    "updated_at": updated_at,
                    "checked": False,
                    "approved": bool(cached.get("approved")),
                    "approval_comment": cached.get("approval_comment"),
                    "verdict_command": cached.get("verdict_command"),
                }
            )
            continue

        view_ref = issue_url or str(issue_number)
        view_argv = ["gh", "issue", "view", view_ref, "--json", "state,url,number,title,updatedAt,comments"]
        if getattr(args, "repo", None):
            view_argv.extend(["--repo", args.repo])
        view_process = run_fn(view_argv, capture_output=True, text=True, check=False)
        if view_process.returncode != 0:
            detail = (view_process.stderr or view_process.stdout or "gh issue view failed").strip()
            issue_payload = {
                "number": issue_number,
                "title": listed.get("title"),
                "url": issue_url,
                "updated_at": updated_at,
                "checked": True,
                "error": detail,
                "approved": False,
                "verdict_command": None,
            }
            issues.append(issue_payload)
            cached_issues[issue_url] = issue_payload
            checked_count += 1
            continue
        try:
            issue = json.loads(view_process.stdout or "{}")
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"invalid gh issue view JSON for {view_ref}: {exc}") from exc
        comments = issue.get("comments") if isinstance(issue.get("comments"), list) else []
        approval_comment = _review_status_approval_comment(comments)
        resolved_url = str(issue.get("url") or issue_url)
        manifest = _review_manifest_for_issue(issue, manifest_map) or _review_manifest_for_issue(listed, manifest_map)
        verdict_command = _review_verdict_command(
            resolved_url,
            manifest,
            close_issue=bool(getattr(args, "close_issue", False)),
        ) if approval_comment else None
        issue_payload = {
            "number": issue.get("number") or issue_number,
            "title": issue.get("title") or listed.get("title"),
            "url": resolved_url,
            "state": issue.get("state"),
            "updated_at": issue.get("updatedAt") or updated_at,
            "checked": True,
            "approved": approval_comment is not None,
            "approval_comment": approval_comment,
            "manifest": manifest,
            "verdict_command": verdict_command,
        }
        issues.append(issue_payload)
        if resolved_url:
            cached_issues[resolved_url] = {
                "updated_at": issue_payload["updated_at"],
                "approved": issue_payload["approved"],
                "approval_comment": approval_comment,
                "verdict_command": verdict_command,
            }
        checked_count += 1

    payload = {
        "kind": "desktop-video-proof-review-watch",
        "repo": getattr(args, "repo", None),
        "label": args.label,
        "state": args.state,
        "issue_count": len(issues),
        "checked_count": checked_count,
        "skipped_unchanged_count": skipped_count,
        "approved_count": sum(1 for issue in issues if issue.get("approved")),
        "issues": issues,
    }
    return payload, state_payload


def cmd_desktop_review_watch(
    args: argparse.Namespace,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    print_fn: Callable[[str], None] = print,
    sleep_fn: Callable[[float], None] = time.sleep,
) -> int:
    state_path = Path(args.state_file).expanduser().resolve() if getattr(args, "state_file", None) else None
    try:
        manifest_map = _load_review_manifest_map(Path(args.manifest_map).expanduser().resolve() if getattr(args, "manifest_map", None) else None)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print_fn(f"Error: could not read manifest map: {exc}")
        return 1
    state_payload = _load_review_watch_state(state_path)
    max_iterations = max(1, int(getattr(args, "max_iterations", 1) or 1))
    interval = max(0.0, float(getattr(args, "interval", 0.0) or 0.0))
    payloads: list[dict] = []
    for index in range(max_iterations):
        try:
            payload, state_payload = _review_watch_once(
                args,
                state_payload=state_payload,
                manifest_map=manifest_map,
                run_fn=run_fn,
            )
        except RuntimeError as exc:
            print_fn(f"Error: {exc}")
            return 1
        payload["iteration"] = index + 1
        payloads.append(payload)
        if state_path:
            state_path.parent.mkdir(parents=True, exist_ok=True)
            state_path.write_text(json.dumps(state_payload, indent=2) + "\n")
        if index + 1 < max_iterations and interval > 0:
            sleep_fn(interval)
    final_payload = payloads[-1] if len(payloads) == 1 else {
        "kind": "desktop-video-proof-review-watch-series",
        "iterations": payloads,
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(final_payload, indent=2))
        return 0
    if final_payload["kind"] == "desktop-video-proof-review-watch-series":
        summary = final_payload["iterations"][-1]
    else:
        summary = final_payload
    print_fn(f"Desktop video review watch: label={summary['label']} state={summary['state']}")
    print_fn(f"  issues: {summary['issue_count']}")
    print_fn(f"  checked: {summary['checked_count']}")
    print_fn(f"  skipped_unchanged: {summary['skipped_unchanged_count']}")
    print_fn(f"  approved: {summary['approved_count']}")
    for issue in summary["issues"]:
        if issue.get("approved"):
            print_fn(f"  approved_issue: {issue.get('url')}")
            if issue.get("verdict_command"):
                print_fn(f"    verdict_command: {issue['verdict_command']}")
    return 0


def cmd_desktop_compose_video(
    args: argparse.Namespace,
    *,
    compose_desktop_video_proof_fn: Callable[..., dict],
    create_issue_video_variant_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    manifest_path = Path(args.manifest).expanduser()
    if not manifest_path.is_file():
        print_fn(f"Error: desktop run manifest not found: {manifest_path}")
        return 1
    manifest_path = manifest_path.resolve()
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print_fn(f"Error: could not read desktop run manifest: {exc}")
        return 1

    run_dir = manifest_path.parent
    video_dir = run_dir / "video"
    output_path = Path(args.output).expanduser().resolve() if args.output else video_dir / "proof-composed.mp4"
    metadata_path = Path(args.metadata).expanduser().resolve() if args.metadata else video_dir / "composed-metadata.json"
    issue_output_path = Path(args.issue_output).expanduser().resolve() if args.issue_output else video_dir / "proof.issue.mp4"
    issue_metadata_path = Path(args.issue_metadata).expanduser().resolve() if args.issue_metadata else video_dir / "issue-metadata.json"
    small_output_path = Path(args.small_output).expanduser().resolve() if getattr(args, "small_output", None) else video_dir / "proof.small.mp4"
    small_metadata_path = Path(args.small_metadata).expanduser().resolve() if getattr(args, "small_metadata", None) else video_dir / "small-metadata.json"
    attachment_budget_bytes = int(float(args.video_attachment_budget_mb) * 1_000_000)
    small_budget_bytes = int(float(getattr(args, "small_video_budget_mb", 10.0)) * 1_000_000)

    template = getattr(args, "template", None) or None
    source_image = Path(args.source_image).expanduser().resolve() if getattr(args, "source_image", None) else None
    source_label = getattr(args, "source_label", None) or None
    diff_image = Path(args.diff_image).expanduser().resolve() if getattr(args, "diff_image", None) else None
    diff_label = getattr(args, "diff_label", None) or None
    title = getattr(args, "title", None) or None
    notes = [note for note in (getattr(args, "note", None) or []) if note]
    if source_image and not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1
    if diff_image and not diff_image.is_file():
        print_fn(f"Error: diff image not found: {diff_image}")
        return 1

    artifacts = manifest.setdefault("artifacts", {})
    existing_composition = manifest.get("video_proof_composition") if isinstance(manifest.get("video_proof_composition"), dict) else {}
    resolved_template = template or existing_composition.get("template") or "validation-proof"
    artifact_diff_image: Path | None = None
    if diff_image is None and resolved_template == "design-parity":
        artifact_diff = artifacts.get("diff_screenshot")
        if artifact_diff:
            candidate = Path(str(artifact_diff)).expanduser().resolve()
            if candidate.is_file():
                artifact_diff_image = candidate
    diff_image_for_compose = diff_image or artifact_diff_image

    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        metadata_path.parent.mkdir(parents=True, exist_ok=True)
        issue_output_path.parent.mkdir(parents=True, exist_ok=True)
        issue_metadata_path.parent.mkdir(parents=True, exist_ok=True)
        if getattr(args, "small_video", False):
            small_output_path.parent.mkdir(parents=True, exist_ok=True)
            small_metadata_path.parent.mkdir(parents=True, exist_ok=True)
        composed_summary = compose_desktop_video_proof_fn(
            manifest_path,
            output_path,
            template=template,
            source_image=source_image,
            source_label=source_label,
            diff_image=diff_image_for_compose,
            diff_label=diff_label,
            title=title,
            notes=notes,
        )
        atomic_write_text_fn(metadata_path, json.dumps(composed_summary, indent=2) + "\n")
        issue_summary = create_issue_video_variant_fn(
            output_path,
            issue_output_path,
            issue_metadata_path,
            attachment_budget_bytes=attachment_budget_bytes,
        )
        small_summary = None
        if getattr(args, "small_video", False):
            small_summary = create_issue_video_variant_fn(
                output_path,
                small_output_path,
                small_metadata_path,
                attachment_budget_bytes=small_budget_bytes,
            )
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    manifest["video_composed"] = composed_summary
    manifest["video_issue"] = issue_summary
    if small_summary is not None:
        manifest["video_small"] = small_summary
    if notes:
        manifest["video_proof_notes"] = notes
    if template or source_image or source_label or diff_image_for_compose or diff_label or title or notes:
        composition = manifest.setdefault("video_proof_composition", {})
        if not isinstance(composition, dict):
            composition = {}
            manifest["video_proof_composition"] = composition
        composition.update(
            {
                "template": template or composition.get("template") or "validation-proof",
                "source_image": str(source_image) if source_image else composition.get("source_image"),
                "source_label": source_label if source_label is not None else composition.get("source_label"),
                "diff_image": str(diff_image_for_compose) if diff_image_for_compose else composition.get("diff_image"),
                "diff_label": diff_label if diff_label is not None else composition.get("diff_label"),
                "title": title if title is not None else composition.get("title"),
                "notes": notes if notes else composition.get("notes", []),
            }
        )
    if output_path.exists():
        artifacts["video_composed"] = str(output_path)
    if metadata_path.exists():
        artifacts["video_composed_metadata"] = str(metadata_path)
    if issue_output_path.exists():
        artifacts["video_issue"] = str(issue_output_path)
    if issue_metadata_path.exists():
        artifacts["video_issue_metadata"] = str(issue_metadata_path)
    if small_summary is not None and small_output_path.exists():
        artifacts["video_small"] = str(small_output_path)
    if small_summary is not None and small_metadata_path.exists():
        artifacts["video_small_metadata"] = str(small_metadata_path)
    atomic_write_text_fn(manifest_path, json.dumps(manifest, indent=2) + "\n")

    payload = {
        "manifest": str(manifest_path),
        "video_composed": composed_summary,
        "video_issue": issue_summary,
        "video_small": small_summary,
        "artifacts": {
            "video_composed": artifacts.get("video_composed"),
            "video_composed_metadata": artifacts.get("video_composed_metadata"),
            "video_issue": artifacts.get("video_issue"),
            "video_issue_metadata": artifacts.get("video_issue_metadata"),
            "video_small": artifacts.get("video_small"),
            "video_small_metadata": artifacts.get("video_small_metadata"),
        },
    }
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
    else:
        print_fn("Desktop proof video composed.")
        print_fn(f"  manifest: {manifest_path}")
        print_fn(f"  video_composed: {artifacts.get('video_composed')}")
        print_fn(f"  video_issue: {artifacts.get('video_issue')}")
        print_fn(f"  issue_status: {issue_summary.get('status')}")
        if small_summary is not None:
            print_fn(f"  video_small: {artifacts.get('video_small')}")
            print_fn(f"  small_status: {small_summary.get('status')}")
    return 0


def cmd_desktop_design_diff(
    args: argparse.Namespace,
    *,
    design_parity_diff_summary_fn: Callable[..., dict],
    atomic_write_text_fn: Callable[[Path, str], None],
    print_fn: Callable[[str], None] = print,
) -> int:
    source_image = Path(args.source_image).expanduser()
    if not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1
    source_image = source_image.resolve()

    native_image: Path | None = Path(args.native_image).expanduser() if getattr(args, "native_image", None) else None
    manifest_path: Path | None = Path(args.manifest).expanduser() if getattr(args, "manifest", None) else None
    if manifest_path:
        if not manifest_path.is_file():
            print_fn(f"Error: desktop run manifest not found: {manifest_path}")
            return 1
        manifest_path = manifest_path.resolve()
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print_fn(f"Error: could not read desktop run manifest: {exc}")
            return 1
        if native_image is None:
            screenshot = (manifest.get("artifacts") or {}).get("screenshot")
            if screenshot:
                native_image = Path(str(screenshot)).expanduser()
    if native_image is None:
        print_fn("Error: native image required; pass --native-image or --manifest with artifacts.screenshot")
        return 1
    if not native_image.is_file():
        print_fn(f"Error: native image not found: {native_image}")
        return 1
    native_image = native_image.resolve()

    output = Path(args.output).expanduser().resolve()
    resized_source_output = Path(args.resized_source_output).expanduser().resolve() if getattr(args, "resized_source_output", None) else None
    metadata_path = Path(args.metadata).expanduser().resolve() if getattr(args, "metadata", None) else output.with_suffix(".json")
    try:
        summary = design_parity_diff_summary_fn(
            source_image,
            native_image,
            diff_output_path=output,
            resized_source_output_path=resized_source_output,
            enhance_brightness=float(getattr(args, "enhance_brightness", 3.0)),
        )
    except Exception as exc:
        print_fn(f"Error: {exc}")
        return 1

    payload = {
        "kind": "desktop-design-parity-diff",
        "source_image": str(source_image),
        "native_image": str(native_image),
        "manifest": str(manifest_path) if manifest_path else None,
        "diff_image": str(output),
        "resized_source_image": str(resized_source_output) if resized_source_output else None,
        "metadata": str(metadata_path),
        "summary": summary,
        "compose_args": [
            "--diff-image",
            str(output),
            "--diff-label",
            "Source vs native screenshot diff",
        ],
    }
    atomic_write_text_fn(metadata_path, json.dumps(payload, indent=2) + "\n")
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return 0
    print_fn("Desktop design parity diff generated")
    print_fn(f"  source_image: {source_image}")
    print_fn(f"  native_image: {native_image}")
    print_fn(f"  diff_image: {output}")
    if resized_source_output:
        print_fn(f"  resized_source_image: {resized_source_output}")
    print_fn(f"  metadata: {metadata_path}")
    if summary.get("bbox"):
        bbox = summary["bbox"]
        print_fn(f"  bbox: {bbox['left']},{bbox['top']} -> {bbox['right']},{bbox['bottom']}")
    else:
        print_fn("  bbox: unchanged")
    print_fn(f"  compose_args: --diff-image {shlex.quote(str(output))} --diff-label 'Source vs native screenshot diff'")
    return 0


def cmd_desktop_serve(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_serve_candidate_urls_fn: Callable[[str, int], list[str]] = desktop_serve_candidate_urls,
    serve_directory_fn: Callable[..., None] = serve_directory,
    start_serve_process_fn: Callable[..., dict] = start_desktop_serve_process,
    persist_serve_urls_fn: Callable[[Path, list[str]], None] = persist_desktop_serve_urls,
    read_serve_state_fn: Callable[[Path, str], dict | None] = read_desktop_serve_state,
    stop_serve_process_fn: Callable[..., dict] = stop_desktop_serve_process,
    is_running_fn: Callable[[int], bool] = process_is_running,
    find_available_port_fn: Callable[[str, int], int] = find_available_desktop_serve_port,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    publish_root = desktop_publish_root_from_config(config)
    label = getattr(args, "label", None) or "desktop-proof"

    if getattr(args, "stop", False):
        result = stop_serve_process_fn(publish_root, label)
        if getattr(args, "json", False):
            print_fn(json.dumps(result, indent=2))
        else:
            print_fn(f"Desktop proof server {result['status']}: {label}")
            if result.get("pid"):
                print_fn(f"  pid: {result['pid']}")
        return 0 if result.get("status") in {"stopped", "missing"} else 1

    if getattr(args, "status", False):
        state = read_serve_state_fn(publish_root, label)
        if not state:
            result = {"status": "missing", "label": label, "state_path": str(desktop_serve_state_path(publish_root, label))}
        else:
            pid = int(state.get("pid") or 0)
            if is_running_fn(pid):
                status = "running"
            elif state.get("status") == "failed":
                status = "failed"
            else:
                status = "stale"
            result = {**state, "status": status}
        if getattr(args, "json", False):
            print_fn(json.dumps(result, indent=2))
        else:
            print_fn(f"Desktop proof server {result['status']}: {label}")
            if result.get("pid"):
                print_fn(f"  pid: {result['pid']}")
            for url in result.get("urls") or []:
                print_fn(f"  url: {url}")
            if result.get("directory"):
                print_fn(f"  directory: {result['directory']}")
        return 0 if result.get("status") in {"running", "missing", "stale"} else 1

    if args.path:
        serve_dir = Path(args.path).expanduser()
    else:
        reports = desktop_publish_reports_fn(config, limit=1)
        if not reports:
            print_fn("Error: no desktop publish reports found. Run `pulp ci-local desktop publish` first.")
            return 1
        serve_dir = Path(reports[0]["output_dir"]).expanduser()

    resolved_serve_dir = serve_dir.resolve()
    if not _path_is_relative_to(resolved_serve_dir, publish_root):
        print_fn(f"Error: desktop serve only serves reports under configured publish root: {publish_root}")
        return 1
    if not serve_dir.is_dir():
        print_fn(f"Error: desktop report directory not found: {serve_dir}")
        return 1
    if not (serve_dir / "index.html").exists():
        print_fn(f"Error: desktop report index.html not found: {serve_dir / 'index.html'}")
        return 1

    port = int(getattr(args, "port", 8765) or 8765)
    if getattr(args, "auto_port", False):
        try:
            port = int(find_available_port_fn(args.host, port))
        except Exception as exc:
            print_fn(f"Error: {exc}")
            return 1

    urls = desktop_serve_candidate_urls_fn(args.host, port)
    primary_url = urls[0] if urls else f"http://{args.host}:{port}/"
    if getattr(args, "background", False):
        state = start_serve_process_fn(
            resolved_serve_dir,
            host=args.host,
            port=port,
            label=label,
            publish_root=publish_root,
            urls=urls,
        )
        if state.get("status") == "failed":
            if getattr(args, "json", False):
                print_fn(json.dumps(state, indent=2))
            else:
                print_fn(f"Error: desktop proof server failed to start: {label}")
                if state.get("exit_code") is not None:
                    print_fn(f"  exit_code: {state['exit_code']}")
                if state.get("stderr_tail"):
                    print_fn(f"  stderr: {state['stderr_tail'].strip()}")
                if state.get("stdout_tail"):
                    print_fn(f"  stdout: {state['stdout_tail'].strip()}")
            return 1
        persist_serve_urls_fn(resolved_serve_dir, urls)
        if getattr(args, "json", False):
            print_fn(json.dumps({**state, "status": "started"}, indent=2))
        else:
            print_fn(f"Serving desktop report: {primary_url}")
            for url in urls[1:]:
                print_fn(f"  also: {url}")
            print_fn(f"  directory: {serve_dir}")
            print_fn(f"  background: {label}")
            print_fn(f"  pid: {state['pid']}")
            if port != int(getattr(args, "port", 8765) or 8765):
                print_fn(f"  auto_port: {port}")
            print_fn(f"  stop: python3 tools/local-ci/local_ci.py desktop serve --stop --label {label}")
        return 0
    print_fn(f"Serving desktop report: {primary_url}")
    for url in urls[1:]:
        print_fn(f"  also: {url}")
    print_fn(f"  directory: {serve_dir}")
    persist_serve_urls_fn(resolved_serve_dir, urls)
    serve_directory_fn(serve_dir, host=args.host, port=port)
    return 0


def _parse_report_generated_at(value: object) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        return None
    text = value.strip()
    if text.endswith("Z"):
        text = f"{text[:-1]}+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def published_report_cleanup_paths(
    reports: list[dict],
    *,
    older_than_days: int,
    keep_last: int,
    now_fn: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
) -> list[Path]:
    keep_count = max(0, int(keep_last or 0))
    cutoff = now_fn().astimezone(timezone.utc).timestamp() - (max(0, older_than_days) * 86400)
    paths: list[Path] = []
    for index, report in enumerate(reports):
        if index < keep_count:
            continue
        generated_at = _parse_report_generated_at(report.get("generated_at"))
        if generated_at is None or generated_at.timestamp() > cutoff:
            continue
        output_dir = report.get("output_dir")
        if output_dir:
            paths.append(Path(str(output_dir)))
    return paths


def cmd_desktop_cleanup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    prune_desktop_run_manifests_fn: Callable[..., list[Path]],
    write_desktop_run_rollups_fn: Callable[..., None],
    desktop_cleanup_empty_line_fn: Callable[[], str],
    desktop_cleanup_lines_fn: Callable[[list[Path]], list[str]],
    desktop_publish_reports_fn: Callable[..., list[dict]] | None = None,
    write_desktop_publish_rollups_fn: Callable[..., None] | None = None,
    now_fn: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
    remove_tree_fn: Callable[..., None] = shutil.rmtree,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
    if getattr(args, "published", False):
        if getattr(args, "target", None):
            print_fn("Error: desktop cleanup --published does not accept a target filter.")
            return 1
        if desktop_publish_reports_fn is None or write_desktop_publish_rollups_fn is None:
            print_fn("Error: desktop publish cleanup dependencies are not available.")
            return 1
        reports = desktop_publish_reports_fn(config)
        paths = published_report_cleanup_paths(
            reports,
            older_than_days=older_than,
            keep_last=args.keep_last,
            now_fn=now_fn,
        )
        if not paths:
            if getattr(args, "json", False):
                print_fn(json.dumps({"kind": "desktop-published-report-cleanup", "removed": []}, indent=2))
            else:
                print_fn("Desktop cleanup: no published reports to remove.")
            return 0
        for path in paths:
            remove_tree_fn(path, ignore_errors=False)
        write_desktop_publish_rollups_fn(config)
        if getattr(args, "json", False):
            print_fn(
                json.dumps(
                    {
                        "kind": "desktop-published-report-cleanup",
                        "removed": [str(path) for path in paths],
                    },
                    indent=2,
                )
            )
            return 0
        print_fn(f"Desktop cleanup removed {len(paths)} published report(s).")
        for path in paths[:10]:
            print_fn(f"  {path}")
        return 0

    paths = prune_desktop_run_manifests_fn(
        config,
        target_name=args.target,
        older_than_days=older_than,
        keep_last=args.keep_last,
    )
    if not paths:
        print_fn(desktop_cleanup_empty_line_fn())
        return 0

    for path in paths:
        remove_tree_fn(path, ignore_errors=False)

    write_desktop_run_rollups_fn(config, target_name=args.target if args.target else None)
    if args.target is not None:
        write_desktop_run_rollups_fn(config)

    if getattr(args, "json", False):
        print_fn(json.dumps({"removed": [str(path) for path in paths]}, indent=2))
        return 0

    _print_lines(desktop_cleanup_lines_fn(paths), print_fn=print_fn)
    return 0
