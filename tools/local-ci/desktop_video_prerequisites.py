"""Video-proof setup/doctor prerequisite checks, remediations, and payloads."""
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

REMOTE_SETUP_PROBE_SHELL = "zsh -lc"
REMOTE_SETUP_PROBE_PATH_PREFIX = "$HOME/.local/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"


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
                "detail": (
                    "Open System Settings > Privacy & Security > Screen Recording, enable Terminal.app, "
                    f"restart Terminal, then rerun video-doctor for `{target_name}` through Terminal re-entry. "
                    "Direct agent sessions may still fail with `could not create image from display` even when Terminal is allowed."
                ),
                "command": "open 'x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture'",
                "rerun_command": f"python3 tools/local-ci/local_ci.py desktop video-doctor {target_name} --run-in-terminal --json",
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


def _resolved_pulp_command(pulp_command: str | None = None) -> str:
    return pulp_command or os.environ.get("PULP_CLI") or "pulp"


def desktop_video_install_model(*, pulp_command: str | None = None) -> dict:
    resolved_pulp_command = _resolved_pulp_command(pulp_command)
    return {
        "current": "source-tree",
        "current_command": "npm --prefix tools/local-ci install",
        "future": "pulp-tool-add-on",
        "future_command": shlex.join([resolved_pulp_command, "tool", "install", "video-proof"]),
        "future_check_command": shlex.join([resolved_pulp_command, "tool", "doctor", "video-proof", "--run"]),
        "tool_info_command": shlex.join([resolved_pulp_command, "tool", "info", "video-proof", "--json"]),
        "pack_command": "python3 tools/local-ci/pack_video_proof_tool.py --json",
        "pack_npm_script": "npm --prefix tools/local-ci run pack-video-proof-tool -- --json",
        "verify_command": "python3 tools/local-ci/pack_video_proof_tool.py --verify <manifest> --json",
        "artifact_install_command": (
            shlex.join([resolved_pulp_command, "tool", "install", "video-proof"])
            + " --artifact-manifest <manifest> --force"
        ),
        "pack_manifest_schema": "pulp.video-proof-tool-package.v1",
        "install_scope": "machine",
        "distribution_lane": "tool_addon",
        "package_format": "not_pulp_add",
        "artifact_status": "source_tree_iteration",
        "detail": (
            "The feature branch supports direct repo-local npm tooling for iteration, "
            f"and `{shlex.join([resolved_pulp_command, 'tool', 'install', 'video-proof'])}` is "
            "the optional developer-tool install path. "
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
    resolved_pulp_command = _resolved_pulp_command(pulp_command)
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


def desktop_video_tool_addon_remediations(checks: list[dict], *, pulp_command: str | None = None) -> list[dict]:
    if all(check.get("ok") for check in checks if check.get("required", True)):
        return []
    resolved_pulp_command = _resolved_pulp_command(pulp_command)
    return [
        {
            "check": "tool_addon",
            "title": "Install or repair the optional video-proof tool add-on",
            "detail": (
                "Install the machine-scoped video-proof tool, then rerun the add-on setup check. "
                "For reviewed local artifacts, use the artifact manifest install command."
            ),
            "command": shlex.join([resolved_pulp_command, "tool", "install", "video-proof"]),
            "check_command": shlex.join([resolved_pulp_command, "tool", "doctor", "video-proof", "--run"]),
            "artifact_install_command": (
                shlex.join([resolved_pulp_command, "tool", "install", "video-proof"])
                + " --artifact-manifest <manifest> --force"
            ),
        }
    ]


def desktop_video_init_config(
    *,
    config_path: Path | None = None,
    example_path: Path | None = None,
) -> dict:
    env_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
    destination = config_path or (Path(env_config).expanduser() if env_config else Path(__file__).resolve().with_name("config.json"))
    source = example_path or Path(__file__).resolve().with_name("config.example.json")
    if destination.exists():
        return {
            "ok": True,
            "created": False,
            "path": str(destination),
            "source": str(source),
            "detail": f"config already exists at {destination}",
        }
    if not source.exists():
        return {
            "ok": False,
            "created": False,
            "path": str(destination),
            "source": str(source),
            "detail": f"example config not found at {source}",
        }
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(source.read_text())
    return {
        "ok": True,
        "created": True,
        "path": str(destination),
        "source": str(source),
        "detail": f"created {destination} from {source}",
    }


def desktop_video_enable_target_capture(config: dict, target_name: str) -> dict:
    desktop_cfg = config.setdefault("desktop_automation", {})
    targets = desktop_cfg.setdefault("targets", {})
    target_cfg = targets.setdefault(target_name, {})
    optional_cfg = dict(target_cfg.get("optional", {}))
    was_enabled = bool(optional_cfg.get("video_capture"))
    optional_cfg["video_capture"] = True
    target_cfg["optional"] = optional_cfg
    return {
        "ok": True,
        "target": target_name,
        "changed": not was_enabled,
        "field": f"target.{target_name}.video_capture",
        "value": True,
        "detail": "already enabled" if was_enabled else f"enabled video_capture for `{target_name}`",
    }


def desktop_video_setup_remote_prerequisite_checks(
    host: str,
    *,
    subprocess_run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> list[dict]:
    script = (
        f"PATH=\"{REMOTE_SETUP_PROBE_PATH_PREFIX}\"; export PATH; "
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
        remote_check["probe_shell"] = REMOTE_SETUP_PROBE_SHELL
        remote_check["probe_path_prefix"] = REMOTE_SETUP_PROBE_PATH_PREFIX
        checks.append(remote_check)
    return checks


def _remote_setup_probe_metadata(checks: list[dict]) -> dict | None:
    for check in checks:
        shell = check.get("probe_shell")
        path_prefix = check.get("probe_path_prefix")
        if shell or path_prefix:
            return {
                "shell": shell or "",
                "path_prefix": path_prefix or "",
                "detail": "Remote setup probes use a login-style zsh command so Homebrew/local tool paths are visible over non-interactive SSH.",
            }
    return None


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


def desktop_video_setup_steps(
    target_name: str,
    *,
    machine_label: str | None = None,
    pulp_command: str | None = None,
) -> list[dict]:
    label = (machine_label or target_name).strip() or target_name
    smoke_label = f"{label}-video-setup-smoke"
    resolved_pulp_command = _resolved_pulp_command(pulp_command)
    install_command = shlex.join([resolved_pulp_command, "tool", "install", "video-proof"])
    info_command = shlex.join([resolved_pulp_command, "tool", "info", "video-proof", "--json"])
    doctor_command = shlex.join([resolved_pulp_command, "tool", "doctor", "video-proof", "--run"])
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
            "detail": (
                "Installs pinned developer-only ffmpeg-static and Remotion packages. "
                f"Prefer `{install_command}` for user-facing setup; use this command "
                "for source-tree iteration."
            ),
            "future_command": install_command,
        },
        {
            "name": "inspect_tool_addon",
            "title": "Inspect the optional video-proof install policy",
            "command": info_command,
            "detail": "Confirms video-proof is machine-scoped optional tooling, not a core runtime or project-level `pulp add` package.",
        },
        {
            "name": "check_tool_addon",
            "title": "Validate the optional video-proof tool",
            "command": doctor_command,
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
        "install_model": desktop_video_install_model(pulp_command=getattr(args, "pulp_command", None)),
        "steps": steps,
        "init_config": None,
        "setup_prerequisites": setup_prerequisites,
        "tool_addon": tool_addon,
        "check": check,
    }


def _video_setup_prerequisites_payload(setup_prerequisite_checks_fn: Callable[[], list[dict]]) -> dict:
    setup_checks = setup_prerequisite_checks_fn()
    return {
        "ok": all(check["ok"] for check in setup_checks if check.get("required", True)),
        "checks": setup_checks,
        "remediations": desktop_video_setup_prerequisite_remediations(setup_checks),
    }


def _video_setup_tool_addon_payload(
    args: argparse.Namespace,
    tool_addon_checks_fn: Callable[..., list[dict]],
) -> dict:
    pulp_command = getattr(args, "pulp_command", None)
    if pulp_command:
        tool_checks = tool_addon_checks_fn(pulp_command=pulp_command)
    else:
        tool_checks = tool_addon_checks_fn()
    return {
        "ok": all(check["ok"] for check in tool_checks if check.get("required", True)),
        "checks": tool_checks,
        "remediations": desktop_video_tool_addon_remediations(
            tool_checks,
            pulp_command=getattr(args, "pulp_command", None),
        ),
    }


def _print_missing_video_setup_payload(payload: dict, print_fn: Callable[[str], None]) -> None:
    print_fn(f"Desktop video setup for `{payload['target']}`")
    print_fn(f"  machine: {payload['machine']}")
    print_fn(f"  adapter: {payload['adapter']}")
    print_fn(f"  bootstrap: {payload['bootstrap']}")
    print_fn(f"  install: {payload['install_model']['current_command']} (future: {payload['install_model']['future_command']})")
    if payload.get("init_config"):
        print_fn("")
        print_fn(f"Init config: {'PASS' if payload['init_config'].get('ok') else 'FAIL'}")
        print_fn(f"  {payload['init_config'].get('detail')}")
    print_fn("")
    print_fn("Steps:")
    for index, step in enumerate(payload["steps"], start=1):
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
            if item.get("rerun_command"):
                print_fn(f"    rerun: {item['rerun_command']}")


def desktop_video_doctor_payload(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    save_config_fn: Callable[[dict], None] | None = None,
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


