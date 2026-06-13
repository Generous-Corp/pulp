"""Desktop automation management command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
from functools import partial
import http.server
import json
import os
from pathlib import Path
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


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n")


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
    _write_json(state_path, payload)
    return payload


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
            "cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe standalone-interaction "
            "--source-mode exact-sha "
            "--command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' "
            "--prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' "
            "--pulp-app-automation --capture-ui-snapshot --click-view-id bypass-toggle "
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
        "id": "reaper-plugin-editor",
        "title": "Plugin editor in REAPER",
        "platform": "mac",
        "status": "partial",
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
            "future slice should pop/focus the floating plugin editor reliably",
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
            "--capture-ui-snapshot --label inspector-open-and-select --compose-video-proof"
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
            "cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)"
        ),
        "command": (
            "python3 tools/local-ci/local_ci.py desktop video mac "
            "--recipe component-zoom "
            "--source-mode exact-sha "
            "--command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' "
            "--prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF && "
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)' "
            "--pulp-app-automation --capture-ui-snapshot --component-id bypass-toggle "
            "--click-view-id bypass-toggle --label component-bypass-toggle "
            "--compose-video-proof"
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
        "title": "iOS Simulator interaction",
        "platform": "ios-simulator",
        "status": "partial",
        "template": "mobile-simulator",
        "proves": "A booted iOS Simulator can launch an app and produce a bounded MP4 proof clip.",
        "command": (
            "python3 tools/local-ci/local_ci.py simulator video "
            "--app build/ios/PulpDemo.app --bundle-id com.pulp.demo "
            "--open-url https://example.com --action-label 'open validation URL' "
            "--label ios-simulator-launch-proof --duration 8 --video-fps 10 "
            "--compose-video-proof --video-title 'iOS Simulator open URL proof' "
            "--video-note 'Simulator opens the validation URL during recording.' --small-video"
        ),
        "doctor": "python3 tools/local-ci/local_ci.py simulator video-doctor",
        "watch_for": [
            "device/runtime is identified",
            "app launch or host setup is visible",
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
)


def desktop_video_matrix_payload(*, target: str | None = None, scenario: str | None = None) -> dict:
    scenarios: list[dict] = []
    for item in VIDEO_PROOF_DEMO_SCENARIOS:
        if target and item["platform"] != target:
            continue
        if scenario and item["id"] != scenario:
            continue
        row = {key: value for key, value in item.items()}
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
        row["serve_background_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve {report_placeholder} "
            f"--host 0.0.0.0 --port 8765 --background --label {label}-review --json"
        )
        row["serve_status_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve --status --label {label}-review --json"
        )
        row["serve_stop_command"] = (
            f"python3 tools/local-ci/local_ci.py desktop serve --stop --label {label}-review --json"
        )
        row["review_workflow"] = [
            {"step": "prepare", "command": row.get("prepare_command") or "not required"},
            {"step": "doctor", "command": row["doctor"]},
            {"step": "record_or_compose", "command": row["command"]},
            {"step": "publish", "command": row["publish_command"]},
            {"step": "draft_issue", "command": row["review_issue_command"]},
            {"step": "serve_background", "command": row["serve_background_command"]},
            {"step": "check_server", "command": row["serve_status_command"]},
            {"step": "stop_server", "command": row["serve_stop_command"]},
        ]
        scenarios.append(row)
    return {
        "kind": "desktop-video-proof-demo-matrix",
        "target": target or "all",
        "scenario": scenario or "all",
        "scenario_count": len(scenarios),
        "scenarios": scenarios,
    }


def desktop_video_matrix_lines(payload: dict) -> list[str]:
    lines = [
        "Desktop validation video proof demo matrix:",
        f"  target: {payload.get('target')}",
        f"  scenarios: {payload.get('scenario_count')}",
    ]
    for item in payload.get("scenarios", []):
        lines.extend(
            [
                "",
                f"- {item['id']} [{item['status']}]",
                f"  title: {item['title']}",
                f"  platform: {item['platform']}",
                f"  template: {item['template']}",
                f"  proves: {item['proves']}",
                f"  doctor: {item['doctor']}",
                f"  prepare: {item.get('prepare_command') or '(none)'}",
                f"  command: {item['command']}",
                f"  publish: {item['publish_command']}",
                f"  review issue: {item['review_issue_command']}",
                f"  serve background: {item['serve_background_command']}",
                f"  serve status: {item['serve_status_command']}",
                f"  serve stop: {item['serve_stop_command']}",
                "  watch for:",
            ]
        )
        lines.extend(f"    - {value}" for value in item.get("watch_for", []))
    return lines


def desktop_video_matrix_markdown(payload: dict) -> str:
    lines = [
        "# Desktop Validation Video Proof Demo Matrix",
        "",
        f"- Target: `{payload.get('target')}`",
        f"- Scenarios: `{payload.get('scenario_count')}`",
        "",
    ]
    for item in payload.get("scenarios", []):
        lines.extend(
            [
                f"## {item['title']}",
                "",
                f"- Scenario: `{item['id']}`",
                f"- Status: `{item['status']}`",
                f"- Platform: `{item['platform']}`",
                f"- Remotion template: `{item['template']}`",
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
                item["serve_stop_command"],
                "```",
                "",
                "Watch for:",
            ]
        )
        lines.extend(f"- {value}" for value in item.get("watch_for", []))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def cmd_desktop_video_matrix(
    args: argparse.Namespace,
    *,
    print_fn: Callable[[str], None] = print,
) -> int:
    payload = desktop_video_matrix_payload(
        target=getattr(args, "target", None) or None,
        scenario=getattr(args, "scenario", None) or None,
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
    title = getattr(args, "title", None) or None
    notes = [note for note in (getattr(args, "note", None) or []) if note]
    if source_image and not source_image.is_file():
        print_fn(f"Error: source image not found: {source_image}")
        return 1

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

    artifacts = manifest.setdefault("artifacts", {})
    manifest["video_composed"] = composed_summary
    manifest["video_issue"] = issue_summary
    if small_summary is not None:
        manifest["video_small"] = small_summary
    if notes:
        manifest["video_proof_notes"] = notes
    if template or source_image or source_label or title or notes:
        composition = manifest.setdefault("video_proof_composition", {})
        if not isinstance(composition, dict):
            composition = {}
            manifest["video_proof_composition"] = composition
        composition.update(
            {
                "template": template or composition.get("template") or "validation-proof",
                "source_image": str(source_image) if source_image else composition.get("source_image"),
                "source_label": source_label if source_label is not None else composition.get("source_label"),
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


def cmd_desktop_serve(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_serve_candidate_urls_fn: Callable[[str, int], list[str]] = desktop_serve_candidate_urls,
    serve_directory_fn: Callable[..., None] = serve_directory,
    start_serve_process_fn: Callable[..., dict] = start_desktop_serve_process,
    read_serve_state_fn: Callable[[Path, str], dict | None] = read_desktop_serve_state,
    stop_serve_process_fn: Callable[..., dict] = stop_desktop_serve_process,
    is_running_fn: Callable[[int], bool] = process_is_running,
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
            result = {**state, "status": "running" if is_running_fn(pid) else "stale"}
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

    urls = desktop_serve_candidate_urls_fn(args.host, args.port)
    primary_url = urls[0] if urls else f"http://{args.host}:{args.port}/"
    if getattr(args, "background", False):
        state = start_serve_process_fn(
            resolved_serve_dir,
            host=args.host,
            port=args.port,
            label=label,
            publish_root=publish_root,
            urls=urls,
        )
        if getattr(args, "json", False):
            print_fn(json.dumps({**state, "status": "started"}, indent=2))
        else:
            print_fn(f"Serving desktop report: {primary_url}")
            for url in urls[1:]:
                print_fn(f"  also: {url}")
            print_fn(f"  directory: {serve_dir}")
            print_fn(f"  background: {label}")
            print_fn(f"  pid: {state['pid']}")
            print_fn(f"  stop: python3 tools/local-ci/local_ci.py desktop serve --stop --label {label}")
        return 0
    print_fn(f"Serving desktop report: {primary_url}")
    for url in urls[1:]:
        print_fn(f"  also: {url}")
    print_fn(f"  directory: {serve_dir}")
    serve_directory_fn(serve_dir, host=args.host, port=args.port)
    return 0


def cmd_desktop_cleanup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    prune_desktop_run_manifests_fn: Callable[..., list[Path]],
    write_desktop_run_rollups_fn: Callable[..., None],
    desktop_cleanup_empty_line_fn: Callable[[], str],
    desktop_cleanup_lines_fn: Callable[[list[Path]], list[str]],
    remove_tree_fn: Callable[..., None] = shutil.rmtree,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
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
