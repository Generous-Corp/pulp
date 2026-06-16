"""Desktop video-proof demo matrix command (curated scenario listing + readiness)."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import os
from pathlib import Path
import shlex
import shutil


def _print_lines(lines, *, print_fn: Callable[[str], None]) -> None:
    for line in lines:
        print_fn(line)


def desktop_publish_root_from_config(config: dict) -> Path:
    return Path(config["desktop_automation"]["artifact_root"]).expanduser().resolve() / "_published"


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
            "--tap 540,1200 --action-label 'tap validation control' "
            "--label android-emulator-proof --duration 8 "
            "--compose-video-proof --video-title 'Android emulator tap proof' "
            "--video-note 'The emulator receives a timed adb input tap during recording.' --small-video"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py android video-doctor",
        "watch_for": [
            "adb serial/model and screenrecord readiness are identified",
            "app launch or current emulator state is visible",
            "timed adb tap action is marked in the proof; use open-url when a deep link is the better validation action",
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
    design_parity_native_image: Path | None = None,
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
        native_image = design_parity_native_image
        source_image = design_parity_source_image or repo_root / "planning" / "screenshots" / "reference.png"
        manifest_ok = bool(manifest and manifest.is_file())
        native_image_ok = bool(native_image and native_image.is_file())
        add(
            "design-parity.run-manifest",
            manifest_ok,
            str(manifest) if manifest_ok else "missing run manifest for design-parity composition",
            required=not native_image_ok,
            remediation="Pass --design-parity-manifest /path/to/run/manifest.json for an existing run, or --design-parity-native-image /path/to/native.png for one-shot design-proof.",
        )
        add(
            "design-parity.native-image",
            native_image_ok,
            str(native_image) if native_image_ok else "missing native image for one-shot design-proof",
            required=not manifest_ok,
            remediation="Pass --design-parity-native-image /path/to/native.png, or --design-parity-manifest /path/to/run/manifest.json for an existing run.",
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
    design_parity_native_image: str | Path | None = None,
    design_parity_publish_root: str | Path | None = None,
    repo_root: Path | None = None,
    which_fn: Callable[[str], str | None] = shutil.which,
) -> dict:
    root = repo_root or Path.cwd()
    design_parity_manifest_path = Path(design_parity_manifest).expanduser().resolve() if design_parity_manifest else None
    design_parity_source_image_path = Path(design_parity_source_image).expanduser().resolve() if design_parity_source_image else None
    design_parity_native_image_path = Path(design_parity_native_image).expanduser().resolve() if design_parity_native_image else None
    design_parity_discovered: dict | None = None
    should_discover_design_parity = (
        check
        and design_parity_publish_root
        and (
            not design_parity_source_image_path
            or (not design_parity_native_image_path and not design_parity_manifest_path)
        )
    )
    if should_discover_design_parity:
        design_parity_discovered = _latest_published_design_parity_inputs(Path(design_parity_publish_root).expanduser().resolve())
        if design_parity_discovered:
            if not design_parity_manifest_path and not design_parity_native_image_path:
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
            native_for_command = design_parity_native_image_path or Path("/path/to/native-render.png")
            source_label_for_command = "Source reference" if design_parity_discovered else "Figma reference"
            if design_parity_native_image_path:
                row["command"] = (
                    "python3 tools/local-ci/local_ci.py desktop design-proof "
                    f"--source-image {shlex.quote(str(source_for_command))} "
                    f"--native-image {shlex.quote(str(native_for_command))} "
                    "--label design-parity-proof "
                    f"--source-label {shlex.quote(source_label_for_command)} --title 'Design parity proof' "
                    "--small-video --small-video-budget-mb 10"
                )
            else:
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
                design_parity_native_image=design_parity_native_image_path,
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
        manifest_map_placeholder = f"/tmp/{label}-video-review-manifest-map.json"
        row["publish_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop publish --manifest {manifest_placeholder} "
            f"--label {label}-review"
        )
        row["review_issue_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop review-issue {report_placeholder} "
            "--repo owner/repo --check-files"
        )
        row["review_create_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop review-issue {report_placeholder} "
            f"--repo owner/repo --check-files --create --label video-review "
            f"--manifest-map-output {manifest_map_placeholder}"
        )
        row["review_status_command"] = (
            "python3 tools/local-ci/local_ci.py desktop review-status <issue-url> "
            f"--repo owner/repo --manifest {manifest_placeholder} --close-issue"
        )
        row["review_manifest_map"] = manifest_map_placeholder
        row["review_watch_command"] = (
            "python3 tools/local-ci/local_ci.py desktop review-watch "
            f"--repo owner/repo --label video-review --manifest-map {manifest_map_placeholder} "
            "--state-file /tmp/pulp-video-review-watch.json --close-issue"
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
            {"step": "create_issue_with_map", "command": row["review_create_command"]},
            {"step": "serve_background", "command": row["serve_background_command"]},
            {"step": "check_server", "command": row["serve_status_command"]},
            {"step": "check_review", "command": row["review_status_command"]},
            {"step": "watch_reviews", "command": row["review_watch_command"]},
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
                f"  review create: {item['review_create_command']}",
                f"  review status: {item['review_status_command']}",
                f"  review watch: {item['review_watch_command']}",
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
                item["review_create_command"],
                item["serve_background_command"],
                item["serve_status_command"],
                item["review_status_command"],
                item["review_watch_command"],
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
        design_parity_native_image=getattr(args, "design_parity_native_image", None) or None,
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
