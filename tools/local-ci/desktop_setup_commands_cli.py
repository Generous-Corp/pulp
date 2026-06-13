"""Desktop automation setup and health command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
from pathlib import Path
import subprocess


def cmd_desktop_install(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    check_writable_dir_fn: Callable[[Path], tuple[bool, str]],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    ensure_host_reachable_fn: Callable[[str, dict, dict], str | None],
    bootstrap_windows_session_agent_fn: Callable[[str, dict], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    subprocess_run_fn: Callable[..., subprocess.CompletedProcess],
    root_path: Path,
    new_install_job_id_fn: Callable[[], str],
    sync_job_bundle_to_ssh_host_fn: Callable[[str, dict], tuple[str, str]],
    ensure_windows_remote_tooling_fn: Callable[[str], dict],
    windows_remote_tooling_ready_fn: Callable[[dict], bool],
    ensure_windows_remote_repo_checkout_fn: Callable[..., dict],
    git_origin_clone_url_fn: Callable[[Path], str],
    windows_repo_checkout_ready_fn: Callable[[dict], bool],
    update_target_repo_path_fn: Callable[[dict, str, str], None],
    save_config_fn: Callable[[dict], None],
    now_iso_fn: Callable[[], str],
    desktop_target_receipt_path_fn: Callable[[str], Path],
    atomic_write_text_fn: Callable[[Path, str], None],
    windows_tooling_detail_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    artifact_root = Path(config["desktop_automation"]["artifact_root"])
    ok, detail = check_writable_dir_fn(artifact_root)
    if not ok:
        print_fn(f"Error: desktop artifact root is not writable: {detail}")
        return 1

    contract = desktop_target_contract_fn(args.target, target)
    remote_bootstrap_ready = target["target_type"] != "ssh"
    remote_tooling_ready = target["target_type"] != "ssh"
    remote_repo_checkout_ready = target["target_type"] != "ssh"
    tooling_probe = None
    tooling_installed: list[str] = []
    repo_checkout_probe = None
    if target["target_type"] == "ssh" and target["adapter"] == "windows-session-agent":
        host = ensure_host_reachable_fn(args.target, target, config.get("defaults", {}))
        if host:
            try:
                bootstrap_result = bootstrap_windows_session_agent_fn(host, contract)
                probe = probe_windows_session_agent_fn(host, contract)
                remote_bootstrap_ready = bool(
                    probe.get("task_present")
                    and probe.get("agent_root_exists")
                    and probe.get("jobs_dir_exists")
                    and probe.get("results_dir_exists")
                    and probe.get("script_exists")
                )
                contract = {
                    **contract,
                    "remote_root": bootstrap_result.get("remote_root", contract.get("remote_root")),
                    "script_path": bootstrap_result.get("script_path", contract.get("script_path")),
                }
                install_bundle_sha = subprocess_run_fn(
                    ["git", "rev-parse", "HEAD"],
                    cwd=root_path,
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
                install_bundle_job = {"id": new_install_job_id_fn(), "sha": install_bundle_sha}
                install_bundle_name, install_bundle_ref = sync_job_bundle_to_ssh_host_fn(host, install_bundle_job)
                tooling_result = ensure_windows_remote_tooling_fn(host)
                tooling_probe = tooling_result["probe"]
                tooling_installed = tooling_result["installed"]
                remote_tooling_ready = windows_remote_tooling_ready_fn(tooling_probe)
                repo_checkout_probe = ensure_windows_remote_repo_checkout_fn(
                    host,
                    target.get("repo_path"),
                    remote_url=git_origin_clone_url_fn(root_path),
                    bundle_name=install_bundle_name,
                    bundle_ref=install_bundle_ref,
                )
                remote_repo_checkout_ready = windows_repo_checkout_ready_fn(repo_checkout_probe)
                effective_repo_path = repo_checkout_probe.get("repo_path")
                if effective_repo_path and effective_repo_path != target.get("repo_path"):
                    update_target_repo_path_fn(config, args.target, effective_repo_path)
                    save_config_fn(config)
                    target = resolve_desktop_target_fn(config, args.target)
            except (subprocess.SubprocessError, OSError, RuntimeError) as exc:
                remote_bootstrap_ready = False
                remote_tooling_ready = False
                remote_repo_checkout_ready = False
                print_fn(f"Warning: remote bootstrap did not complete for `{args.target}`: {exc}")
        else:
            remote_bootstrap_ready = False
            remote_tooling_ready = False
            remote_repo_checkout_ready = False

    receipt = {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "target_type": target["target_type"],
        "host": target.get("host"),
        "repo_path": target.get("repo_path"),
        "artifact_root": str(artifact_root),
        "capability_tier": target.get("capability_tier", "v1"),
        "installed_at": now_iso_fn(),
        "remote_bootstrap_ready": remote_bootstrap_ready,
        "remote_tooling_ready": remote_tooling_ready,
        "remote_repo_checkout_ready": remote_repo_checkout_ready,
        "tooling_probe": tooling_probe,
        "repo_checkout_probe": repo_checkout_probe,
        "contract": contract,
    }
    atomic_write_text_fn(
        desktop_target_receipt_path_fn(args.target),
        json.dumps(receipt, indent=2) + "\n",
    )

    print_fn(f"Desktop target `{args.target}` prepared.")
    print_fn(f"  adapter: {target['adapter']}")
    print_fn(f"  bootstrap: {target['bootstrap']}")
    print_fn(f"  artifact_root: {artifact_root}")
    if target["target_type"] == "ssh":
        if remote_bootstrap_ready:
            print_fn("  remote bootstrap: ready")
        else:
            print_fn("  remote bootstrap: pending; target profile recorded locally")
        if target["adapter"] == "windows-session-agent":
            if remote_tooling_ready:
                git_detail = windows_tooling_detail_fn(tooling_probe or {}, "git") if tooling_probe else "git ready"
                print_fn(f"  remote tooling: ready ({git_detail})")
            else:
                print_fn("  remote tooling: pending; run `pulp ci-local desktop doctor windows` for remediation")
            if tooling_installed:
                print_fn(f"  remote tooling installed: {', '.join(tooling_installed)}")
            if repo_checkout_probe and repo_checkout_probe.get("repo_path"):
                print_fn(f"  remote repo checkout: {repo_checkout_probe['repo_path']}")
        if contract.get("task_name"):
            print_fn(f"  task_name: {contract['task_name']}")
        if contract.get("remote_root"):
            print_fn(f"  remote_root: {contract['remote_root']}")
    else:
        print_fn("  remote bootstrap: not required for local target")
    return 0


def cmd_desktop_doctor(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    checks = desktop_doctor_checks_fn(config, args.target)
    all_ok = True
    for check in checks:
        if check.get("required", True):
            all_ok = all_ok and check["ok"]
    if getattr(args, "json", False):
        payload = {
            "target": args.target,
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "ok": all_ok,
            "checks": checks,
        }
        print_fn(json.dumps(payload, indent=2))
        return 0 if all_ok else 1
    print_fn(f"Desktop doctor for `{args.target}`")
    print_fn(f"  adapter: {target['adapter']}")
    print_fn(f"  bootstrap: {target['bootstrap']}")
    for check in checks:
        if check["ok"]:
            status = "PASS"
        elif not check.get("required", True):
            status = "WARN"
        else:
            status = "FAIL"
        print_fn(f"  {status:4s}  {check['name']}: {check['detail']}")
    return 0 if all_ok else 1


def _desktop_check_status(check: dict) -> str:
    if check["ok"]:
        return "PASS"
    if not check.get("required", True):
        return "WARN"
    return "FAIL"


def desktop_video_doctor_remediations(checks: list[dict], *, target_name: str) -> list[dict]:
    checks_by_name = {check.get("name"): check for check in checks}
    remediations: list[dict] = []
    screencapture = checks_by_name.get("screencapture")
    if screencapture and not screencapture.get("ok"):
        remediations.append(
            {
                "check": "screencapture",
                "title": "Grant macOS Screen Recording permission",
                "detail": "Open System Settings > Privacy & Security > Screen Recording, enable the terminal/agent app running local CI, then restart that app.",
                "command": "open 'x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture'",
            }
        )
    target_video = checks_by_name.get("target.video_capture")
    if target_video and not target_video.get("ok"):
        remediations.append(
            {
                "check": "target.video_capture",
                "title": "Enable video capture for this desktop target",
                "detail": f"Set the optional video_capture capability before recording proof videos for `{target_name}`.",
                "command": f"python3 tools/local-ci/local_ci.py desktop config set target.{target_name}.video_capture true",
            }
        )
    video_capture = checks_by_name.get("video_capture")
    if video_capture and not video_capture.get("ok"):
        remediations.append(
            {
                "check": "video_capture",
                "title": "Install the repo-local video tooling",
                "detail": "Install pinned npm developer tools so ffmpeg-static and Remotion are available.",
                "command": "npm --prefix tools/local-ci install",
            }
        )
    avfoundation = checks_by_name.get("avfoundation_screen")
    if avfoundation and not avfoundation.get("ok"):
        remediations.append(
            {
                "check": "avfoundation_screen",
                "title": "Confirm ffmpeg can enumerate the macOS screen input",
                "detail": "The recorder expects AVFoundation input `Capture screen 0`; rerun video-doctor after ffmpeg is installed and Screen Recording is granted.",
                "command": "python3 tools/local-ci/local_ci.py desktop video-doctor mac --json",
            }
        )
    remotion = checks_by_name.get("remotion_smoke")
    if remotion and not remotion.get("ok"):
        remediations.append(
            {
                "check": "remotion_smoke",
                "title": "Run the Remotion proof smoke test",
                "detail": "This verifies the local Remotion package and ffmpeg render path without needing Screen Recording.",
                "command": "npm --prefix tools/local-ci run smoke-video-proof",
            }
        )
    return remediations


def cmd_desktop_video_doctor(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    video_proof_smoke_fn: Callable[[], dict],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    checks = desktop_doctor_checks_fn(config, args.target)
    optional = normalize_desktop_optional_config_fn(target.get("optional"))
    checks.append(
        {
            "name": "target.video_capture",
            "ok": bool(optional.get("video_capture")),
            "detail": "enabled"
            if optional.get("video_capture")
            else f"disabled; run `python3 tools/local-ci/local_ci.py desktop config set target.{args.target}.video_capture true`",
            "required": True,
        }
    )
    for check in checks:
        if check.get("name") in {"video_capture", "avfoundation_screen"}:
            check["required"] = True

    if getattr(args, "skip_remotion_smoke", False):
        checks.append(
            {
                "name": "remotion_smoke",
                "ok": True,
                "detail": "skipped by --skip-remotion-smoke",
                "required": False,
            }
        )
    else:
        try:
            smoke = video_proof_smoke_fn()
            checks.append(
                {
                    "name": "remotion_smoke",
                    "ok": bool(smoke.get("ok")),
                    "detail": smoke.get("detail") or smoke.get("output") or "ok",
                    "required": True,
                    "payload": smoke,
                }
            )
        except (OSError, RuntimeError, ValueError) as exc:
            checks.append(
                {
                    "name": "remotion_smoke",
                    "ok": False,
                    "detail": str(exc),
                    "required": True,
                }
            )

    all_ok = all(check["ok"] for check in checks if check.get("required", True))
    remediations = desktop_video_doctor_remediations(checks, target_name=args.target)
    if getattr(args, "json", False):
        payload = {
            "target": args.target,
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "ok": all_ok,
            "checks": checks,
            "remediations": remediations,
        }
        print_fn(json.dumps(payload, indent=2))
        return 0 if all_ok else 1

    print_fn(f"Desktop video doctor for `{args.target}`")
    print_fn(f"  adapter: {target['adapter']}")
    print_fn(f"  bootstrap: {target['bootstrap']}")
    for check in checks:
        print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
    if remediations:
        print_fn("")
        print_fn("Remediation:")
        for item in remediations:
            print_fn(f"  - {item['title']}: {item['detail']}")
            if item.get("command"):
                print_fn(f"    command: {item['command']}")
    return 0 if all_ok else 1
