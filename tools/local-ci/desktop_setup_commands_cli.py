"""Desktop automation setup and health command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

try:
    import reaper_video_recipe
except ModuleNotFoundError:
    _reaper_recipe_spec = importlib.util.spec_from_file_location(
        "reaper_video_recipe",
        Path(__file__).resolve().with_name("reaper_video_recipe.py"),
    )
    if _reaper_recipe_spec is None or _reaper_recipe_spec.loader is None:
        raise
    reaper_video_recipe = importlib.util.module_from_spec(_reaper_recipe_spec)
    _reaper_recipe_spec.loader.exec_module(reaper_video_recipe)


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


def desktop_video_recorder_backend_check(target: dict) -> dict:
    adapter = target.get("adapter")
    if adapter == "macos-local":
        return {
            "name": "backend.recorder",
            "ok": True,
            "detail": "macOS ffmpeg/AVFoundation recorder with screencapture fallback",
            "required": True,
        }
    if adapter == "linux-xvfb":
        return {
            "name": "backend.recorder",
            "ok": False,
            "detail": "Linux/Xvfb video recorder is not implemented yet; planned backend is ffmpeg x11grab against the target display",
            "required": True,
        }
    if adapter == "windows-session-agent":
        return {
            "name": "backend.recorder",
            "ok": False,
            "detail": "Windows session-agent video recorder is not implemented yet; planned backend is ffmpeg ddagrab/gdigrab from the interactive session",
            "required": True,
        }
    return {
        "name": "backend.recorder",
        "ok": False,
        "detail": f"video recorder is not implemented for desktop adapter `{adapter}`",
        "required": True,
    }


def desktop_video_doctor_remediations(checks: list[dict], *, target_name: str) -> list[dict]:
    checks_by_name = {check.get("name"): check for check in checks}
    remediations: list[dict] = []
    backend = checks_by_name.get("backend.recorder")
    if backend and not backend.get("ok"):
        remediations.append(
            {
                "check": "backend.recorder",
                "title": "Use a supported video recorder backend",
                "detail": f"Desktop video recording for `{target_name}` is not implemented yet. Use macOS video proofs, iOS Simulator, Android emulator, or still screenshots until this backend lands.",
            }
        )
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
    receipt = checks_by_name.get("receipt")
    if receipt and not receipt.get("ok"):
        remediations.append(
            {
                "check": "receipt",
                "title": "Prepare the desktop target",
                "detail": f"Install the local desktop target receipt before running video proof checks for `{target_name}`.",
                "command": f"python3 tools/local-ci/local_ci.py desktop install {target_name}",
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
                "detail": "Install the optional `video-proof` tool add-on, or use the source-tree npm command while iterating on this branch, so ffmpeg-static and Remotion are available.",
                "command": "npm --prefix tools/local-ci install",
                "future_command": "pulp tool install video-proof",
                "future_check_command": "pulp tool doctor video-proof --run",
            }
        )
    avfoundation = checks_by_name.get("avfoundation_screen")
    if avfoundation and not avfoundation.get("ok") and avfoundation.get("required", True):
        remediations.append(
            {
                "check": "avfoundation_screen",
                "title": "Confirm ffmpeg can enumerate the macOS screen input",
                "detail": "The recorder expects AVFoundation input `Capture screen 0`; rerun video-doctor after ffmpeg is installed and Screen Recording is granted.",
                "command": "python3 tools/local-ci/local_ci.py desktop video-doctor mac --json",
            }
        )
    avfoundation_audio = checks_by_name.get("avfoundation_audio")
    if avfoundation_audio and not avfoundation_audio.get("ok") and avfoundation_audio.get("required", True):
        remediations.append(
            {
                "check": "avfoundation_audio",
                "title": "Configure an explicit macOS audio input",
                "detail": "Install/select a loopback audio device, then pass --video-audio-device or set PULP_VIDEO_AUDIO_DEVICE before recording system-audio proofs.",
                "command": f"PULP_VIDEO_AUDIO_DEVICE=\"BlackHole 2ch\" python3 tools/local-ci/local_ci.py desktop video-doctor {target_name} --video-audio system --json",
            }
        )
    reaper_clap_bundle = checks_by_name.get("reaper.clap_bundle")
    if reaper_clap_bundle and not reaper_clap_bundle.get("ok"):
        plugin = reaper_clap_bundle.get("plugin") or "<Plugin>"
        remediations.append(
            {
                "check": "reaper.clap_bundle",
                "title": "Build and install the CLAP bundle for REAPER",
                "detail": f"Build the {plugin} CLAP target and install or symlink it under ~/Library/Audio/Plug-Ins/CLAP before recording the REAPER proof.",
                "command": f"cmake --build build-video-nogpu --target {plugin}_CLAP -j$(sysctl -n hw.ncpu)",
            }
        )
    reaper_clap_cache = checks_by_name.get("reaper.clap_cache")
    if reaper_clap_cache and not reaper_clap_cache.get("ok"):
        remediations.append(
            {
                "check": "reaper.clap_cache",
                "title": "Refresh REAPER's CLAP plug-in cache",
                "detail": "Open REAPER Preferences > Plug-ins > CLAP and rescan, or remove the stale plugin stanza from the REAPER CLAP cache and relaunch REAPER.",
                "command": "open -a REAPER",
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
                "future_command": "pulp tool doctor video-proof --run",
            }
        )
    return remediations


def desktop_video_install_model() -> dict:
    return {
        "current": "source-tree",
        "current_command": "npm --prefix tools/local-ci install",
        "future": "pulp-tool-add-on",
        "future_command": "pulp tool install video-proof",
        "future_check_command": "pulp tool doctor video-proof --run",
        "tool_info_command": "pulp tool info video-proof --json",
        "pack_command": "python3 tools/local-ci/pack_video_proof_tool.py --json",
        "pack_npm_script": "npm --prefix tools/local-ci run pack-video-proof-tool -- --json",
        "verify_command": "python3 tools/local-ci/pack_video_proof_tool.py --verify <manifest> --json",
        "artifact_install_command": "pulp tool install video-proof --artifact-manifest <manifest> --force",
        "pack_manifest_schema": "pulp.video-proof-tool-package.v1",
        "install_scope": "machine",
        "distribution_lane": "tool_addon",
        "package_format": "not_pulp_add",
        "artifact_status": "source_tree_iteration",
        "detail": (
            "The feature branch supports direct repo-local npm tooling for iteration, "
            "and `pulp tool install video-proof` is the optional developer-tool install path. "
            "The recorder/composer is not a normal `pulp add` project package and should "
            "be packaged as a versioned tool add-on before mainline release."
        ),
    }


def desktop_video_setup_prerequisite_checks(*, which_fn: Callable[[str], str | None] = shutil.which) -> list[dict]:
    required_tools = [
        {
            "name": "pulp",
            "title": "Pulp CLI",
            "detail": "required for the optional `pulp tool install video-proof` and `pulp tool info video-proof --json` setup path",
            "remediation": "Install or build the Pulp CLI, then ensure `pulp` is on PATH.",
        },
        {
            "name": "npm",
            "title": "npm",
            "detail": "required for the current source-tree Remotion/ffmpeg-static install path",
            "remediation": "Install Node.js/npm, for example with Homebrew or the Node.js installer.",
        },
        {
            "name": "node",
            "title": "Node.js",
            "detail": "required by Remotion composition scripts",
            "remediation": "Install Node.js, then rerun `node --version` and this setup check.",
        },
        {
            "name": "cmake",
            "title": "CMake",
            "detail": "required to build the source checkout and validate the CLI/tool install path on a fresh machine",
            "remediation": "Install CMake, then rebuild the Release CLI before running the tool install smoke.",
        },
    ]
    checks: list[dict] = []
    for tool in required_tools:
        path = which_fn(tool["name"])
        checks.append(
            {
                "name": f"setup.{tool['name']}",
                "ok": bool(path),
                "detail": path or tool["detail"],
                "required": True,
                "title": tool["title"],
                "remediation": tool["remediation"],
            }
        )
    return checks


def desktop_video_setup_prerequisite_remediations(checks: list[dict]) -> list[dict]:
    remediations: list[dict] = []
    for check in checks:
        if check.get("ok"):
            continue
        remediations.append(
            {
                "check": check["name"],
                "title": f"Install {check.get('title') or check['name']}",
                "detail": check.get("remediation") or check.get("detail") or "Install the missing setup prerequisite.",
            }
        )
    return remediations


def desktop_video_tool_addon_checks(
    *,
    subprocess_run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    pulp_command: str | None = None,
) -> list[dict]:
    resolved_pulp_command = pulp_command or os.environ.get("PULP_CLI") or "pulp"
    checks: list[dict] = []
    info_cmd = [resolved_pulp_command, "tool", "info", "video-proof", "--json"]
    try:
        info_result = subprocess_run_fn(info_cmd, capture_output=True, text=True, check=False)
    except OSError as exc:
        checks.append(
            {
                "name": "tool_addon.info",
                "ok": False,
                "detail": f"could not run `{shlex.join(info_cmd)}`: {exc}",
                "required": True,
                "command": shlex.join(info_cmd),
            }
        )
        checks.append(
            {
                "name": "tool_addon.doctor",
                "ok": False,
                "detail": "skipped because tool info failed",
                "required": True,
                "command": shlex.join([resolved_pulp_command, "tool", "doctor", "video-proof", "--run"]),
            }
        )
        return checks

    info_stdout = (info_result.stdout or "").strip()
    info_stderr = (info_result.stderr or "").strip()
    info_ok = info_result.returncode == 0
    info_detail = info_stdout or info_stderr or f"exit {info_result.returncode}"
    info_payload = None
    if info_ok:
        try:
            info_payload = json.loads(info_stdout)
        except json.JSONDecodeError as exc:
            info_ok = False
            info_detail = f"invalid JSON from tool info: {exc}"
    if info_ok and info_payload:
        install_scope = info_payload.get("install_scope")
        package_format = info_payload.get("package_format")
        distribution_lane = info_payload.get("distribution_lane")
        info_detail = (
            f"{info_payload.get('id', 'video-proof')} "
            f"scope={install_scope or 'unknown'} "
            f"lane={distribution_lane or 'unknown'} "
            f"format={package_format or 'unknown'}"
        )
        if install_scope != "machine" or distribution_lane != "tool_addon" or package_format != "not_pulp_add":
            info_ok = False
            info_detail = (
                f"{info_detail}; expected machine-scoped tool_addon not_pulp_add policy"
            )
    checks.append(
        {
            "name": "tool_addon.info",
            "ok": info_ok,
            "detail": info_detail,
            "required": True,
            "command": shlex.join(info_cmd),
            "payload": info_payload,
        }
    )

    doctor_cmd = [resolved_pulp_command, "tool", "doctor", "video-proof", "--run"]
    if not info_ok:
        checks.append(
            {
                "name": "tool_addon.doctor",
                "ok": False,
                "detail": "skipped because tool info failed",
                "required": True,
                "command": shlex.join(doctor_cmd),
            }
        )
        return checks
    try:
        doctor_result = subprocess_run_fn(doctor_cmd, capture_output=True, text=True, check=False)
    except OSError as exc:
        checks.append(
            {
                "name": "tool_addon.doctor",
                "ok": False,
                "detail": f"could not run `{shlex.join(doctor_cmd)}`: {exc}",
                "required": True,
                "command": shlex.join(doctor_cmd),
            }
        )
        return checks

    doctor_stdout = (doctor_result.stdout or "").strip()
    doctor_stderr = (doctor_result.stderr or "").strip()
    checks.append(
        {
            "name": "tool_addon.doctor",
            "ok": doctor_result.returncode == 0,
            "detail": doctor_stdout or doctor_stderr or f"exit {doctor_result.returncode}",
            "required": True,
            "command": shlex.join(doctor_cmd),
        }
    )
    return checks


def desktop_video_tool_addon_remediations(checks: list[dict]) -> list[dict]:
    if all(check.get("ok") for check in checks if check.get("required", True)):
        return []
    return [
        {
            "check": "tool_addon",
            "title": "Install or repair the optional video-proof tool add-on",
            "detail": (
                "Install the machine-scoped video-proof tool, then rerun the add-on setup check. "
                "For reviewed local artifacts, use the artifact manifest install command."
            ),
            "command": "pulp tool install video-proof",
            "check_command": "pulp tool doctor video-proof --run",
            "artifact_install_command": "pulp tool install video-proof --artifact-manifest <manifest> --force",
        }
    ]


def desktop_video_setup_remote_prerequisite_checks(
    host: str,
    *,
    subprocess_run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> list[dict]:
    script = (
        "PATH=\"$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH\"; export PATH; "
        "for tool in pulp npm node cmake; do "
        "found_path=$(command -v \"$tool\" 2>/dev/null || true); "
        "if [ -n \"$found_path\" ]; then printf '%s\\t%s\\n' \"$tool\" \"$found_path\"; "
        "else printf '%s\\t\\n' \"$tool\"; fi; "
        "done"
    )
    try:
        result = subprocess_run_fn(
            ["ssh", "-o", "ConnectTimeout=5", "-o", "BatchMode=yes", host, "zsh", "-lc", shlex.quote(script)],
            capture_output=True,
            text=True,
            timeout=20,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return [
            {
                "name": "remote_setup.ssh",
                "ok": False,
                "detail": str(exc),
                "required": True,
                "title": "SSH",
                "remediation": f"Make `{host}` reachable with non-interactive SSH before probing video setup prerequisites.",
            }
        ]
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or f"ssh exited {result.returncode}").strip()
        return [
            {
                "name": "remote_setup.ssh",
                "ok": False,
                "detail": detail,
                "required": True,
                "title": "SSH",
                "remediation": f"Make `{host}` reachable with non-interactive SSH before probing video setup prerequisites.",
            }
        ]

    found: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if "\t" not in line:
            continue
        name, path = line.split("\t", 1)
        found[name] = path
    checks: list[dict] = []
    for check in desktop_video_setup_prerequisite_checks(which_fn=lambda name: found.get(name) or None):
        remote_check = dict(check)
        remote_check["name"] = check["name"].replace("setup.", "remote_setup.", 1)
        remote_check["host"] = host
        checks.append(remote_check)
    return checks


def append_video_recipe_doctor_checks(args: argparse.Namespace, checks: list[dict]) -> None:
    recipe = getattr(args, "recipe", None)
    if not recipe:
        return
    if recipe != "reaper-plugin-editor":
        checks.append(
            {
                "name": "recipe",
                "ok": False,
                "detail": f"video-doctor does not know recipe-specific checks for `{recipe}`",
                "required": True,
            }
        )
        return

    plugin = getattr(args, "plugin", None)
    plugin_format = getattr(args, "plugin_format", None)
    if not plugin or not plugin_format:
        checks.append(
            {
                "name": "recipe.reaper",
                "ok": False,
                "detail": "recipe `reaper-plugin-editor` requires --plugin and --plugin-format for readiness checks",
                "required": True,
            }
        )
        return

    checks.append(
        {
            "name": "recipe.reaper",
            "ok": True,
            "detail": f"checking {plugin_format} plugin `{plugin}` in REAPER",
            "required": True,
        }
    )
    if plugin_format != "clap":
        return

    ok, detail = reaper_video_recipe.installed_clap_bundle_status(plugin)
    checks.append(
        {
            "name": "reaper.clap_bundle",
            "ok": ok,
            "detail": detail,
            "required": True,
            "plugin": plugin,
        }
    )
    if not ok:
        return

    ok, detail = reaper_video_recipe.reaper_clap_cache_status(plugin)
    checks.append(
        {
            "name": "reaper.clap_cache",
            "ok": ok,
            "detail": detail,
            "required": True,
            "plugin": plugin,
        }
    )


def desktop_video_setup_steps(target_name: str, *, machine_label: str | None = None) -> list[dict]:
    label = (machine_label or target_name).strip() or target_name
    smoke_label = f"{label}-video-setup-smoke"
    return [
        {
            "name": "create_config",
            "title": "Create the local CI config",
            "command": "cp tools/local-ci/config.example.json tools/local-ci/config.json",
            "detail": "Creates the machine-local config file used by desktop target checks and artifact paths.",
        },
        {
            "name": "install_tools",
            "title": "Install repo-local video tools",
            "command": "npm --prefix tools/local-ci install",
            "detail": "Installs pinned developer-only ffmpeg-static and Remotion packages. Prefer `pulp tool install video-proof` for user-facing setup; use this command for source-tree iteration.",
            "future_command": "pulp tool install video-proof",
        },
        {
            "name": "inspect_tool_addon",
            "title": "Inspect the optional video-proof install policy",
            "command": "pulp tool info video-proof --json",
            "detail": "Confirms video-proof is machine-scoped optional tooling, not a core runtime or project-level `pulp add` package.",
        },
        {
            "name": "check_tool_addon",
            "title": "Validate the optional video-proof tool",
            "command": "pulp tool doctor video-proof --run",
            "detail": "Runs the managed video-proof wrapper smoke check so setup failures surface before any screen recording permission handoff.",
        },
        {
            "name": "enable_target_capability",
            "title": "Enable video capture for the desktop target",
            "command": f"python3 tools/local-ci/local_ci.py desktop config set target.{target_name}.video_capture true",
            "detail": "Records the opt-in video_capture capability in the local desktop target config.",
        },
        {
            "name": "prepare_target",
            "title": "Prepare the desktop target",
            "command": f"python3 tools/local-ci/local_ci.py desktop install {target_name}",
            "detail": "Writes the local desktop automation receipt used by readiness checks and artifact reporting.",
        },
        {
            "name": "grant_screen_recording",
            "title": "Grant Screen Recording to Terminal.app",
            "command": "open 'x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture'",
            "detail": "Enable Terminal.app in System Settings > Privacy & Security > Screen Recording, then restart Terminal.",
        },
        {
            "name": "doctor",
            "title": "Run video-doctor through Terminal",
            "command": f"python3 tools/local-ci/local_ci.py desktop video-doctor {target_name} --run-in-terminal",
            "detail": "Verifies screencapture, ffmpeg, Remotion, AVFoundation/fallback capture, and target config.",
        },
        {
            "name": "audio_doctor",
            "title": "Optional: validate system-audio input",
            "command": (
                f"PULP_VIDEO_AUDIO_DEVICE=\"BlackHole 2ch\" python3 tools/local-ci/local_ci.py desktop "
                f"video-doctor {target_name} --run-in-terminal --video-audio system"
            ),
            "detail": "Only needed for audio-bearing proofs. Select an explicit AVFoundation loopback/input device; the harness will not guess one.",
        },
        {
            "name": "smoke_proof",
            "title": "Record a short TextEdit proof",
            "command": (
                f"python3 tools/local-ci/local_ci.py desktop video {target_name} --run-in-terminal "
                f"--action smoke --bundle-id com.apple.TextEdit --duration 2 --video-fps 4 --label {smoke_label} --json"
            ),
            "detail": "Produces a small local MP4 proof bundle tied to the current source commit.",
        },
        {
            "name": "publish",
            "title": "Publish the smoke proof for review",
            "command": "python3 tools/local-ci/local_ci.py desktop publish --manifest <run-bundle>/manifest.json --label video-setup-review --json",
            "detail": "Stages an HTML report with watchable video controls.",
        },
        {
            "name": "serve",
            "title": "Serve the report locally or over Tailscale",
            "command": "python3 tools/local-ci/local_ci.py desktop serve <published-report-dir> --host 0.0.0.0 --port 8765",
            "detail": "Prints localhost, hostname, configured public hosts, and Tailscale candidate URLs.",
        },
    ]


def _default_video_setup_target(target_name: str) -> dict:
    if target_name == "mac":
        return {"adapter": "macos-local", "bootstrap": "launchagent"}
    return {"adapter": "unknown", "bootstrap": "unknown"}


def _missing_video_setup_config_payload(
    args: argparse.Namespace,
    error: Exception,
    steps: list[dict],
    *,
    setup_prerequisites: dict | None = None,
    tool_addon: dict | None = None,
) -> tuple[int, dict]:
    target = _default_video_setup_target(args.target)
    check = None
    exit_code = 0
    if getattr(args, "check", False):
        detail = str(error)
        check = {
            "target": args.target,
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "ok": False,
            "checks": [
                {
                    "name": "config",
                    "ok": False,
                    "detail": detail,
                    "required": True,
                }
            ],
            "remediations": [
                {
                    "check": "config",
                    "title": "Create the local CI config",
                    "detail": "Copy the example config, then enable the desktop video capture capability for this machine.",
                    "command": "cp tools/local-ci/config.example.json tools/local-ci/config.json",
                }
            ],
        }
        exit_code = 1
    return exit_code, {
        "target": args.target,
        "machine": getattr(args, "machine", None) or args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "install_model": desktop_video_install_model(),
        "steps": steps,
        "setup_prerequisites": setup_prerequisites,
        "tool_addon": tool_addon,
        "check": check,
    }


def desktop_video_doctor_payload(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    video_proof_smoke_fn: Callable[[], dict],
    probe_macos_avfoundation_audio_fn: Callable[[str | None], tuple[bool, str]] | None = None,
) -> tuple[int, dict]:
    config = load_config_fn()
    target = resolve_desktop_target_fn(config, args.target)

    checks = desktop_doctor_checks_fn(config, args.target)
    checks.append(desktop_video_recorder_backend_check(target))
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
    checks_by_name = {check.get("name"): check for check in checks}
    screencapture_ok = bool((checks_by_name.get("screencapture") or {}).get("ok"))
    for check in checks:
        if check.get("name") == "video_capture":
            check["required"] = True
        if check.get("name") == "avfoundation_screen":
            check["required"] = not screencapture_ok
            if not check.get("ok") and screencapture_ok:
                check["detail"] = f"{check['detail']} (screencapture fallback available)"

    video_audio = getattr(args, "video_audio", "none")
    if video_audio == "plugin":
        audio_file = getattr(args, "video_audio_file", None)
        audio_path = Path(audio_file).expanduser().resolve() if audio_file else None
        ok = bool(audio_path and audio_path.exists())
        checks.append(
            {
                "name": "video_audio",
                "ok": ok,
                "detail": f"plugin audio WAV ready: {audio_path}" if ok else "pass --video-audio-file <wav> pointing at rendered plugin audio",
                "required": True,
            }
        )
    elif video_audio == "system":
        if target.get("adapter") != "macos-local":
            checks.append(
                {
                    "name": "avfoundation_audio",
                    "ok": False,
                    "detail": "--video-audio system is currently implemented only for macOS AVFoundation capture",
                    "required": True,
                }
            )
        elif probe_macos_avfoundation_audio_fn is None:
            checks.append(
                {
                    "name": "avfoundation_audio",
                    "ok": False,
                    "detail": "AVFoundation audio probe is not available in this runner",
                    "required": True,
                }
            )
        else:
            ok, detail = probe_macos_avfoundation_audio_fn(getattr(args, "video_audio_device", None))
            checks.append({"name": "avfoundation_audio", "ok": ok, "detail": detail, "required": True})

    append_video_recipe_doctor_checks(args, checks)

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
    payload = {
        "target": args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "ok": all_ok,
        "install_model": desktop_video_install_model(),
        "checks": checks,
        "remediations": desktop_video_doctor_remediations(checks, target_name=args.target),
    }
    return (0 if all_ok else 1), payload


def cmd_desktop_video_doctor(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    video_proof_smoke_fn: Callable[[], dict],
    probe_macos_avfoundation_audio_fn: Callable[[str | None], tuple[bool, str]] | None = None,
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        exit_code, payload = desktop_video_doctor_payload(
            args,
            load_config_fn=load_config_fn,
            resolve_desktop_target_fn=resolve_desktop_target_fn,
            desktop_doctor_checks_fn=desktop_doctor_checks_fn,
            normalize_desktop_optional_config_fn=normalize_desktop_optional_config_fn,
            video_proof_smoke_fn=video_proof_smoke_fn,
            probe_macos_avfoundation_audio_fn=probe_macos_avfoundation_audio_fn,
        )
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1
    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return exit_code

    print_fn(f"Desktop video doctor for `{args.target}`")
    print_fn(f"  adapter: {payload['adapter']}")
    print_fn(f"  bootstrap: {payload['bootstrap']}")
    for check in payload["checks"]:
        print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
    if payload["remediations"]:
        print_fn("")
        print_fn("Remediation:")
        for item in payload["remediations"]:
            print_fn(f"  - {item['title']}: {item['detail']}")
            if item.get("command"):
                print_fn(f"    command: {item['command']}")
    return exit_code


def cmd_desktop_video_setup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    resolve_desktop_target_fn: Callable[[dict, str], dict],
    desktop_doctor_checks_fn: Callable[[dict, str], list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    video_proof_smoke_fn: Callable[[], dict],
    probe_macos_avfoundation_audio_fn: Callable[[str | None], tuple[bool, str]] | None = None,
    desktop_video_matrix_payload_fn: Callable[..., dict] | None = None,
    setup_prerequisite_checks_fn: Callable[[], list[dict]] = desktop_video_setup_prerequisite_checks,
    remote_setup_prerequisite_checks_fn: Callable[[str], list[dict]] = desktop_video_setup_remote_prerequisite_checks,
    tool_addon_checks_fn: Callable[..., list[dict]] = desktop_video_tool_addon_checks,
    print_fn: Callable[[str], None] = print,
) -> int:
    steps = desktop_video_setup_steps(args.target, machine_label=getattr(args, "machine", None))
    try:
        config = load_config_fn()
        target = resolve_desktop_target_fn(config, args.target)
    except FileNotFoundError as exc:
        setup_prerequisites = None
        tool_addon = None
        if getattr(args, "check", False):
            setup_checks = setup_prerequisite_checks_fn()
            setup_prerequisites = {
                "ok": all(check["ok"] for check in setup_checks if check.get("required", True)),
                "checks": setup_checks,
                "remediations": desktop_video_setup_prerequisite_remediations(setup_checks),
            }
            if getattr(args, "check_tool_addon", False):
                pulp_command = getattr(args, "pulp_command", None)
                if pulp_command:
                    tool_checks = tool_addon_checks_fn(pulp_command=pulp_command)
                else:
                    tool_checks = tool_addon_checks_fn()
                tool_addon = {
                    "ok": all(check["ok"] for check in tool_checks if check.get("required", True)),
                    "checks": tool_checks,
                    "remediations": desktop_video_tool_addon_remediations(tool_checks),
                }
        exit_code, payload = _missing_video_setup_config_payload(
            args,
            exc,
            steps,
            setup_prerequisites=setup_prerequisites,
            tool_addon=tool_addon,
        )
        if getattr(args, "json", False):
            print_fn(json.dumps(payload, indent=2))
            return exit_code
        print_fn(f"Desktop video setup for `{args.target}`")
        print_fn(f"  machine: {payload['machine']}")
        print_fn(f"  adapter: {payload['adapter']}")
        print_fn(f"  bootstrap: {payload['bootstrap']}")
        print_fn(f"  install: {payload['install_model']['current_command']} (future: {payload['install_model']['future_command']})")
        print_fn("")
        print_fn("Steps:")
        for index, step in enumerate(steps, start=1):
            print_fn(f"  {index}. {step['title']}")
            print_fn(f"     {step['detail']}")
            print_fn(f"     command: {step['command']}")
        if payload.get("setup_prerequisites"):
            print_fn("")
            print_fn(f"Setup prerequisites: {'PASS' if payload['setup_prerequisites']['ok'] else 'FAIL'}")
            for check in payload["setup_prerequisites"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            if payload["setup_prerequisites"]["remediations"]:
                print_fn("")
                print_fn("Setup remediation:")
                for item in payload["setup_prerequisites"]["remediations"]:
                    print_fn(f"  - {item['title']}: {item['detail']}")
        if payload.get("tool_addon"):
            print_fn("")
            print_fn(f"Tool add-on check: {'PASS' if payload['tool_addon']['ok'] else 'FAIL'}")
            for check in payload["tool_addon"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            if payload["tool_addon"]["remediations"]:
                print_fn("")
                print_fn("Tool add-on remediation:")
                for item in payload["tool_addon"]["remediations"]:
                    print_fn(f"  - {item['title']}: {item['detail']}")
                    if item.get("command"):
                        print_fn(f"    command: {item['command']}")
        if payload["check"] is not None:
            print_fn("")
            print_fn("Current check: FAIL")
            for check in payload["check"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            print_fn("")
            print_fn("Remediation:")
            for item in payload["check"]["remediations"]:
                print_fn(f"  - {item['title']}: {item['detail']}")
                if item.get("command"):
                    print_fn(f"    command: {item['command']}")
        return exit_code
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    payload = {
        "target": args.target,
        "machine": getattr(args, "machine", None) or args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "install_model": desktop_video_install_model(),
        "steps": steps,
        "setup_prerequisites": None,
        "remote_setup_prerequisites": None,
        "tool_addon": None,
        "check": None,
    }
    exit_code = 0
    if getattr(args, "check", False):
        setup_checks = setup_prerequisite_checks_fn()
        setup_ok = all(check["ok"] for check in setup_checks if check.get("required", True))
        payload["setup_prerequisites"] = {
            "ok": setup_ok,
            "checks": setup_checks,
            "remediations": desktop_video_setup_prerequisite_remediations(setup_checks),
        }
        doctor_args = argparse.Namespace(
            target=args.target,
            skip_remotion_smoke=getattr(args, "skip_remotion_smoke", False),
            video_audio=getattr(args, "video_audio", "none"),
            video_audio_file=getattr(args, "video_audio_file", None),
            video_audio_device=getattr(args, "video_audio_device", None),
            recipe=getattr(args, "recipe", None),
            plugin=getattr(args, "plugin", None),
            plugin_format=getattr(args, "plugin_format", None),
        )
        exit_code, doctor_payload = desktop_video_doctor_payload(
            doctor_args,
            load_config_fn=lambda: config,
            resolve_desktop_target_fn=resolve_desktop_target_fn,
            desktop_doctor_checks_fn=desktop_doctor_checks_fn,
            normalize_desktop_optional_config_fn=normalize_desktop_optional_config_fn,
            video_proof_smoke_fn=video_proof_smoke_fn,
            probe_macos_avfoundation_audio_fn=probe_macos_avfoundation_audio_fn,
        )
        payload["check"] = doctor_payload
        if not setup_ok:
            exit_code = 1
        if getattr(args, "check_tool_addon", False):
            pulp_command = getattr(args, "pulp_command", None)
            if pulp_command:
                tool_checks = tool_addon_checks_fn(pulp_command=pulp_command)
            else:
                tool_checks = tool_addon_checks_fn()
            tool_ok = all(check["ok"] for check in tool_checks if check.get("required", True))
            payload["tool_addon"] = {
                "ok": tool_ok,
                "checks": tool_checks,
                "remediations": desktop_video_tool_addon_remediations(tool_checks),
            }
            if not tool_ok:
                exit_code = 1
        probe_host = getattr(args, "probe_host", None)
        if probe_host:
            remote_checks = remote_setup_prerequisite_checks_fn(probe_host)
            remote_ok = all(check["ok"] for check in remote_checks if check.get("required", True))
            payload["remote_setup_prerequisites"] = {
                "host": probe_host,
                "ok": remote_ok,
                "checks": remote_checks,
                "remediations": desktop_video_setup_prerequisite_remediations(remote_checks),
            }
            if not remote_ok:
                exit_code = 1
        if desktop_video_matrix_payload_fn is not None:
            payload["demo_matrix"] = desktop_video_matrix_payload_fn(
                target=args.target,
                check=True,
                design_parity_manifest=getattr(args, "design_parity_manifest", None),
                design_parity_source_image=getattr(args, "design_parity_source_image", None),
            )

    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return exit_code

    print_fn(f"Desktop video setup for `{args.target}`")
    print_fn(f"  machine: {payload['machine']}")
    print_fn(f"  adapter: {target['adapter']}")
    print_fn(f"  bootstrap: {target['bootstrap']}")
    print_fn(f"  install: {payload['install_model']['current_command']} (future: {payload['install_model']['future_command']})")
    print_fn("")
    print_fn("Steps:")
    for index, step in enumerate(steps, start=1):
        print_fn(f"  {index}. {step['title']}")
        print_fn(f"     {step['detail']}")
        print_fn(f"     command: {step['command']}")
    if payload["check"] is not None:
        if payload.get("setup_prerequisites"):
            print_fn("")
            print_fn(f"Setup prerequisites: {'PASS' if payload['setup_prerequisites']['ok'] else 'FAIL'}")
            for check in payload["setup_prerequisites"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            if payload["setup_prerequisites"]["remediations"]:
                print_fn("")
                print_fn("Setup remediation:")
                for item in payload["setup_prerequisites"]["remediations"]:
                    print_fn(f"  - {item['title']}: {item['detail']}")
        if payload.get("remote_setup_prerequisites"):
            print_fn("")
            print_fn(
                "Remote setup prerequisites "
                f"({payload['remote_setup_prerequisites']['host']}): "
                f"{'PASS' if payload['remote_setup_prerequisites']['ok'] else 'FAIL'}"
            )
            for check in payload["remote_setup_prerequisites"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            if payload["remote_setup_prerequisites"]["remediations"]:
                print_fn("")
                print_fn("Remote setup remediation:")
                for item in payload["remote_setup_prerequisites"]["remediations"]:
                    print_fn(f"  - {item['title']}: {item['detail']}")
        if payload.get("tool_addon"):
            print_fn("")
            print_fn(f"Tool add-on check: {'PASS' if payload['tool_addon']['ok'] else 'FAIL'}")
            for check in payload["tool_addon"]["checks"]:
                print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
            if payload["tool_addon"]["remediations"]:
                print_fn("")
                print_fn("Tool add-on remediation:")
                for item in payload["tool_addon"]["remediations"]:
                    print_fn(f"  - {item['title']}: {item['detail']}")
                    if item.get("command"):
                        print_fn(f"    command: {item['command']}")
        print_fn("")
        print_fn(f"Current check: {'PASS' if payload['check']['ok'] else 'FAIL'}")
        for check in payload["check"]["checks"]:
            print_fn(f"  {_desktop_check_status(check):4s}  {check['name']}: {check['detail']}")
        if payload["check"]["remediations"]:
            print_fn("")
            print_fn("Remediation:")
            for item in payload["check"]["remediations"]:
                print_fn(f"  - {item['title']}: {item['detail']}")
                if item.get("command"):
                    print_fn(f"    command: {item['command']}")
    if payload.get("demo_matrix"):
        print_fn("")
        print_fn("Demo matrix readiness:")
        for item in payload["demo_matrix"].get("scenarios", []):
            line = f"  - {item['id']}: {item['status']}"
            declared = item.get("declared_status")
            if declared and declared != item.get("status"):
                line += f" (declared: {declared})"
            print_fn(line)
            readiness = item.get("local_readiness") or {}
            for check in readiness.get("checks", []):
                if check.get("required", True) and not check.get("ok"):
                    print_fn(f"      blocker: {check['name']}: {check['detail']}")
    return exit_code
