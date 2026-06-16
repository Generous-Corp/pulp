"""Desktop video-proof setup + doctor commands."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import shlex

from desktop_video_prerequisites import (
    _desktop_check_status,
    _missing_video_setup_config_payload,
    _print_missing_video_setup_payload,
    _remote_setup_probe_metadata,
    _video_setup_prerequisites_payload,
    _video_setup_tool_addon_payload,
    desktop_video_doctor_payload,
    desktop_video_enable_target_capture,
    desktop_video_init_config,
    desktop_video_install_model,
    desktop_video_setup_prerequisite_checks,
    desktop_video_setup_prerequisite_remediations,
    desktop_video_setup_remote_prerequisite_checks,
    desktop_video_setup_steps,
    desktop_video_tool_addon_checks,
)
from desktop_video_prerequisites import *  # noqa: F401,F403 (re-export for callers/tests)
from desktop_video_prerequisites import _remote_setup_probe_metadata  # noqa: F401



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
            if item.get("rerun_command"):
                print_fn(f"    rerun: {item['rerun_command']}")
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
    init_config_fn: Callable[[], dict] = desktop_video_init_config,
    save_config_fn: Callable[[dict], None] | None = None,
    print_fn: Callable[[str], None] = print,
) -> int:
    steps = desktop_video_setup_steps(
        args.target,
        machine_label=getattr(args, "machine", None),
        pulp_command=getattr(args, "pulp_command", None),
    )
    init_config_payload = None
    if getattr(args, "init_config", False):
        init_config_payload = init_config_fn()
    try:
        config = load_config_fn()
        target_config_payload = None
        if getattr(args, "enable_video_capture", False):
            target_config_payload = desktop_video_enable_target_capture(config, args.target)
            if save_config_fn is None:
                raise ValueError("save_config_fn is required when --enable-video-capture is used")
            save_config_fn(config)
        target = resolve_desktop_target_fn(config, args.target)
    except FileNotFoundError as exc:
        setup_prerequisites = _video_setup_prerequisites_payload(setup_prerequisite_checks_fn) if getattr(args, "check", False) else None
        tool_addon = (
            _video_setup_tool_addon_payload(args, tool_addon_checks_fn)
            if getattr(args, "check", False) and getattr(args, "check_tool_addon", False)
            else None
        )
        exit_code, payload = _missing_video_setup_config_payload(
            args,
            exc,
            steps,
            setup_prerequisites=setup_prerequisites,
            tool_addon=tool_addon,
        )
        payload["init_config"] = init_config_payload
        if getattr(args, "json", False):
            print_fn(json.dumps(payload, indent=2))
            return exit_code
        _print_missing_video_setup_payload(payload, print_fn)
        return exit_code
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    payload = {
        "target": args.target,
        "machine": getattr(args, "machine", None) or args.target,
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "install_model": desktop_video_install_model(pulp_command=getattr(args, "pulp_command", None)),
        "init_config": init_config_payload,
        "target_config": target_config_payload,
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
            payload["tool_addon"] = _video_setup_tool_addon_payload(args, tool_addon_checks_fn)
            if not payload["tool_addon"]["ok"]:
                exit_code = 1
        probe_host = getattr(args, "probe_host", None)
        if probe_host:
            remote_checks = remote_setup_prerequisite_checks_fn(probe_host)
            remote_ok = all(check["ok"] for check in remote_checks if check.get("required", True))
            payload["remote_setup_prerequisites"] = {
                "host": probe_host,
                "ok": remote_ok,
                "probe": _remote_setup_probe_metadata(remote_checks),
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
                design_parity_native_image=getattr(args, "design_parity_native_image", None),
            )

    if getattr(args, "json", False):
        print_fn(json.dumps(payload, indent=2))
        return exit_code

    print_fn(f"Desktop video setup for `{args.target}`")
    print_fn(f"  machine: {payload['machine']}")
    print_fn(f"  adapter: {target['adapter']}")
    print_fn(f"  bootstrap: {target['bootstrap']}")
    print_fn(f"  install: {payload['install_model']['current_command']} (future: {payload['install_model']['future_command']})")
    if payload.get("init_config"):
        print_fn(f"  init_config: {payload['init_config']['detail']}")
    if payload.get("target_config"):
        print_fn(f"  target_config: {payload['target_config']['detail']}")
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
                if item.get("rerun_command"):
                    print_fn(f"    rerun: {item['rerun_command']}")
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
