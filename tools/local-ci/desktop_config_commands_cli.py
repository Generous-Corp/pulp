"""Desktop automation config command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
from pathlib import Path

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_command_config,
    run_desktop_command_step,
)


def _updated_desktop_config_payload(
    args: argparse.Namespace,
    *,
    config: dict,
    config_path_fn: Callable[[], Path],
    normalize_publish_mode_fn: Callable[[str], str],
    parse_config_bool_fn: Callable[[str], bool],
    normalize_desktop_config_fn: Callable[[dict], dict],
) -> tuple[dict, dict]:
    desktop_cfg = config.setdefault("desktop_automation", {})
    key = args.key
    raw_value = args.value
    if key == "artifact_root":
        desktop_cfg["artifact_root"] = raw_value
    elif key == "publish_mode":
        desktop_cfg["publish_mode"] = normalize_publish_mode_fn(raw_value)
    elif key == "publish_branch":
        desktop_cfg["publish_branch"] = raw_value
    elif key == "retention_days":
        retention_days = int(raw_value)
        if retention_days < 0:
            raise ValueError("retention_days must be >= 0")
        desktop_cfg["retention_days"] = retention_days
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
    else:
        raise ValueError(
            f"Unsupported desktop config key `{key}`. Use one of: artifact_root, publish_mode, publish_branch, retention_days, or target.<name>.<field>."
        )

    normalized = normalize_desktop_config_fn(config)
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
    return normalized, payload


def cmd_desktop_config_show(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_config_show_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    desktop_cfg = config["desktop_automation"]
    return emit_desktop_command_result(
        payload=desktop_cfg,
        json_output=getattr(args, "json", False),
        text_lines=desktop_config_show_lines_fn(desktop_cfg),
        print_fn=print_fn,
    )


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
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    result, status = run_desktop_command_step(
        lambda: _updated_desktop_config_payload(
            args,
            config=config,
            config_path_fn=config_path_fn,
            normalize_publish_mode_fn=normalize_publish_mode_fn,
            parse_config_bool_fn=parse_config_bool_fn,
            normalize_desktop_config_fn=normalize_desktop_config_fn,
        ),
        print_fn=print_fn,
        handled_exceptions=(ValueError,),
    )
    if status is not None:
        return status
    normalized, payload = result

    save_config_fn(normalized)
    return emit_desktop_command_result(
        payload=payload,
        json_output=getattr(args, "json", False),
        text_lines=desktop_config_update_lines_fn(payload),
        print_fn=print_fn,
    )


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
