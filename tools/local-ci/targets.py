"""Target enable/parse/resolve helpers for local CI.

Extracted from local_ci.py as part of the R2-1 phased split. These
three pure functions own the `--targets` CLI argument plumbing and the
config-driven default-target resolution. They do NOT know about
target *type* (local/ssh/windows-session-agent) or remote bootstrap —
those are downstream concerns owned by the orchestrator and the
normalize-desktop helpers.

`resolve_targets` is the canonical entry point. Order of precedence:

  1. Explicit `--targets a,b,c` from the CLI (passed in as `requested`)
  2. `defaults.targets` in the active config (string or list)
  3. Every target with `enabled: true` in the config

Raises ValueError on unknown or disabled targets so the orchestrator
can surface a clear diagnostic before queuing work.
"""

from __future__ import annotations


def enabled_targets(config: dict) -> list[str]:
    return [
        name
        for name, target_cfg in config.get("targets", {}).items()
        if target_cfg.get("enabled", True)
    ]


def parse_targets_arg(value: str | None) -> list[str] | None:
    if value is None or value.strip() == "":
        return None
    parts = [part.strip() for part in value.split(",") if part.strip()]
    return sorted(dict.fromkeys(parts))


def resolve_targets(config: dict, requested: list[str] | None) -> list[str]:
    if requested is None:
        configured = config.get("defaults", {}).get("targets")
        if configured is not None:
            if isinstance(configured, str):
                requested = parse_targets_arg(configured)
            else:
                requested = sorted(dict.fromkeys(configured))
        else:
            requested = enabled_targets(config)

    if not requested:
        return []

    valid = set(config.get("targets", {}).keys())
    unknown = [target for target in requested if target not in valid]
    if unknown:
        raise ValueError(f"Unknown target(s): {', '.join(unknown)}")

    disabled = [
        target
        for target in requested
        if not config["targets"].get(target, {}).get("enabled", True)
    ]
    if disabled:
        raise ValueError(
            f"Requested target(s) disabled in config: {', '.join(disabled)}"
        )

    return sorted(dict.fromkeys(requested))
