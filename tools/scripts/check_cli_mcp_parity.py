#!/usr/bin/env python3
"""CLI ↔ MCP parity check.

Closes part 2 of issue #1997: when a new top-level CLI command lands in
``tools/cli/pulp_cli.cpp``, the developer should consciously decide
whether to expose it via MCP (``tools/mcp/pulp_mcp.cpp``). Today we have
no forcing function and the surface has drifted.

This script is a structural invariant gate, mirroring
``cli_sync_check.py``, ``skill_sync_check.py``, and the versioning gate:

- Parse the CLI command set from ``tools/cli/pulp_cli.cpp``
- Parse the MCP tool set from ``tools/mcp/pulp_mcp.cpp``
- Diff the two sets against ``tools/scripts/cli_mcp_parity_baseline.json``
- Fail in ``--mode=report`` when a NEW gap appears (CLI command added
  without either an MCP tool of the same name OR an explicit baseline
  allow-list entry).
- Hint mode (``--mode=hint``) is advisory — runs in the
  ``cli-plugin-sync`` PostToolUse hook so agents see drift early.

Naming convention: hyphenated CLI command ``import-design`` ↔
underscored MCP tool name ``pulp_import_design``. The baseline file
keeps the CLI form (hyphenated) on the ``cli_only`` side and the MCP
form (underscored, with ``pulp_`` prefix) on the ``mcp_only`` side.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


# ── Constants ────────────────────────────────────────────────────────────


# Aliases / hidden compatibility commands from pulp_cli.cpp that should not
# count toward parity. Mirrors the suppression list in cli_sync_check.py.
HIDDEN_CLI_ALIASES = {"add-component", "install"}

# Package-manager and tool-registry commands that ship as part of the
# package-manager surface. They live in a separate dispatch lane and do
# not need MCP exposure (the package-manager surface is its own UX).
# Listed here rather than in the baseline to keep the baseline focused
# on top-level commands.
PACKAGE_MANAGER_COMMANDS = {
    "remove",
    "list",
    "search",
    "update",
    "suggest",
    "target",
}


# ── Repo helpers ─────────────────────────────────────────────────────────


def find_repo_root(start: Path | None = None) -> Path | None:
    """Walk upward from ``start`` (default: cwd) to a Pulp repo root."""
    d = (start or Path.cwd()).resolve()
    while d != d.parent:
        if (d / "CMakeLists.txt").exists() and (d / "core").exists():
            return d
        d = d.parent
    return None


# ── CLI parsing ──────────────────────────────────────────────────────────


def extract_cli_commands(cli_cpp: Path) -> set[str]:
    """Return the set of user-visible command names from ``pulp_cli.cpp``.

    Three sources, mirroring the runtime dispatch order:

    1. The three structured tables: ``commands[]``, ``script_commands[]``,
       ``binary_commands[]``.
    2. Special-case ``if (command == "X")`` dispatches at the bottom of
       ``main()`` (e.g. ``add``, ``audit``, ``tool``, package-manager
       commands).
    3. The ``handle`` / ``cmd_*`` aliases — skipped, not user-facing.

    Hidden compatibility aliases (``add-component``, ``install``) are
    suppressed so they don't count toward the parity surface.
    """
    if not cli_cpp.exists():
        return set()

    content = cli_cpp.read_text()
    names: set[str] = set()

    def add_table(table_name: str) -> None:
        m = re.search(
            rf"static const \w+ {table_name}\[\] = \{{(?P<body>.*?)\n\}};",
            content,
            re.DOTALL,
        )
        if not m:
            return
        for entry in re.finditer(
            r'^\s*\{"(\w[\w-]*)"', m.group("body"), re.MULTILINE
        ):
            names.add(entry.group(1))

    for table in ("commands", "script_commands", "binary_commands"):
        add_table(table)

    for entry in re.finditer(r'if\s*\(\s*command\s*==\s*"(\w[\w-]*)"\s*\)', content):
        names.add(entry.group(1))

    return names - HIDDEN_CLI_ALIASES


# ── MCP parsing ──────────────────────────────────────────────────────────


def extract_mcp_tools(mcp_cpp: Path) -> set[str]:
    """Return the set of MCP tool names registered in ``pulp_mcp.cpp``.

    Looks for ``"name":"pulp_<x>"`` JSON entries inside the
    ``tools_list_json()`` block (the source of truth advertised over
    ``tools/list``).
    """
    if not mcp_cpp.exists():
        return set()
    content = mcp_cpp.read_text()
    return set(re.findall(r'"name"\s*:\s*"(pulp_\w+)"', content))


# ── Baseline ────────────────────────────────────────────────────────────


@dataclass
class Baseline:
    """Allow-list of intentional CLI ↔ MCP parity gaps."""

    cli_only: dict[str, str] = field(default_factory=dict)
    mcp_only: dict[str, str] = field(default_factory=dict)


def load_baseline(path: Path) -> Baseline:
    """Load the parity baseline from ``cli_mcp_parity_baseline.json``.

    Missing file → empty baseline. Malformed JSON → raise ``ValueError``;
    silently swallowing it would mask drift.
    """
    if not path.exists():
        return Baseline()
    raw = json.loads(path.read_text())
    if not isinstance(raw, dict):
        raise ValueError(f"baseline must be a JSON object: {path}")
    cli_only = raw.get("cli_only", {}) or {}
    mcp_only = raw.get("mcp_only", {}) or {}
    if not isinstance(cli_only, dict) or not isinstance(mcp_only, dict):
        raise ValueError(f"baseline cli_only/mcp_only must be objects: {path}")
    return Baseline(cli_only=cli_only, mcp_only=mcp_only)


# ── Parity computation ──────────────────────────────────────────────────


def cli_to_mcp_name(cli_name: str) -> str:
    """Map a hyphenated CLI command to its expected MCP tool name."""
    return "pulp_" + cli_name.replace("-", "_")


def mcp_to_cli_name(mcp_name: str) -> str:
    """Map a ``pulp_xxx`` MCP tool back to its expected CLI command."""
    if not mcp_name.startswith("pulp_"):
        return mcp_name
    return mcp_name[len("pulp_"):].replace("_", "-")


@dataclass
class Diff:
    cli_commands: set[str]
    mcp_tools: set[str]
    new_cli_only: set[str]   # CLI commands without MCP & not in baseline → fail
    new_mcp_only: set[str]   # MCP tools without CLI & not in baseline → warn
    accepted_cli_only: set[str]  # in baseline, present in CLI today
    accepted_mcp_only: set[str]  # in baseline, present in MCP today
    stale_cli_only_baseline: set[str]  # baseline entry whose CLI command vanished
    stale_mcp_only_baseline: set[str]  # baseline entry whose MCP tool vanished


def compute_diff(
    cli_commands: set[str], mcp_tools: set[str], baseline: Baseline
) -> Diff:
    # Forward direction: CLI → MCP. Package-manager commands are excluded
    # from the parity surface (they live in their own dispatch lane).
    eligible_cli = cli_commands - PACKAGE_MANAGER_COMMANDS
    cli_without_mcp = {c for c in eligible_cli if cli_to_mcp_name(c) not in mcp_tools}
    accepted_cli_only = cli_without_mcp & set(baseline.cli_only)
    new_cli_only = cli_without_mcp - set(baseline.cli_only)

    # Reverse direction: MCP → CLI. Anything starting with "pulp_audio_",
    # "pulp_inspect_", "pulp_get_view_tree", "pulp_simulate_click",
    # "pulp_screenshot" intentionally has no CLI parent — covered by the
    # baseline. New MCP-only tools also flag, but at warning level.
    mcp_without_cli = {
        m for m in mcp_tools if mcp_to_cli_name(m) not in cli_commands
    }
    accepted_mcp_only = mcp_without_cli & set(baseline.mcp_only)
    new_mcp_only = mcp_without_cli - set(baseline.mcp_only)

    stale_cli_baseline = set(baseline.cli_only) - cli_without_mcp
    stale_mcp_baseline = set(baseline.mcp_only) - mcp_without_cli

    return Diff(
        cli_commands=cli_commands,
        mcp_tools=mcp_tools,
        new_cli_only=new_cli_only,
        new_mcp_only=new_mcp_only,
        accepted_cli_only=accepted_cli_only,
        accepted_mcp_only=accepted_mcp_only,
        stale_cli_only_baseline=stale_cli_baseline,
        stale_mcp_only_baseline=stale_mcp_baseline,
    )


# ── Output helpers ──────────────────────────────────────────────────────


def _color(code: str, text: str, *, enabled: bool) -> str:
    if not enabled:
        return text
    return f"\033[{code}m{text}\033[0m"


def render_text(diff: Diff, *, color: bool, mode: str) -> str:
    out: list[str] = []
    out.append("CLI ↔ MCP Parity Check")
    out.append("=" * 50)
    out.append(
        f"  CLI commands : {len(diff.cli_commands)}    "
        f"MCP tools : {len(diff.mcp_tools)}"
    )
    out.append("")

    ok = _color("32", "✓", enabled=color)
    warn = _color("33", "⚠", enabled=color)
    fail = _color("31", "✗", enabled=color)

    if not diff.new_cli_only:
        out.append(
            f"  {ok} No new CLI commands without MCP exposure "
            f"(allow-list covers {len(diff.accepted_cli_only)})"
        )
    else:
        out.append(
            f"  {fail} {len(diff.new_cli_only)} new CLI command(s) without "
            f"MCP exposure or baseline entry:"
        )
        for name in sorted(diff.new_cli_only):
            out.append(f"      - {name}    "
                       f"(expected MCP tool: {cli_to_mcp_name(name)})")
        out.append("")
        out.append(
            "    To resolve: either add the corresponding tool to "
            "tools/mcp/pulp_mcp.cpp, or"
        )
        out.append(
            "    add an entry to tools/scripts/cli_mcp_parity_baseline.json "
            "with a one-line reason."
        )

    if diff.new_mcp_only:
        out.append("")
        out.append(
            f"  {warn} {len(diff.new_mcp_only)} MCP tool(s) without CLI "
            f"counterpart and not in baseline:"
        )
        for name in sorted(diff.new_mcp_only):
            out.append(f"      - {name}")
        out.append(
            "    (warning only — many MCP tools are intentionally library- or "
            "inspector-shaped and"
        )
        out.append(
            "    have no top-level CLI peer; promote the tool name into the "
            "mcp_only baseline section)"
        )

    if diff.stale_cli_only_baseline or diff.stale_mcp_only_baseline:
        out.append("")
        out.append(
            f"  {warn} Baseline contains entries whose underlying gap no "
            f"longer exists — clean these up:"
        )
        for name in sorted(diff.stale_cli_only_baseline):
            out.append(f"      - cli_only.{name}")
        for name in sorted(diff.stale_mcp_only_baseline):
            out.append(f"      - mcp_only.{name}")

    out.append("")
    if diff.new_cli_only:
        if mode == "report":
            out.append("FAIL: new CLI ↔ MCP parity gap. See above for resolution.")
        else:
            out.append("HINT: new CLI ↔ MCP parity gap. Will fail in --mode=report.")
    else:
        out.append("OK: parity check clean.")
    return "\n".join(out)


# ── CLI ─────────────────────────────────────────────────────────────────


def make_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "--mode",
        choices=("hint", "report"),
        default="report",
        help="hint: advisory (always exit 0). report: hard-fail on new gap.",
    )
    p.add_argument("--json", action="store_true", help="Emit JSON output.")
    p.add_argument(
        "--no-color",
        action="store_true",
        help="Disable ANSI color in text output.",
    )
    p.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Override repo root (default: walk up from cwd).",
    )
    p.add_argument(
        "--cli-source",
        type=Path,
        default=None,
        help="Override CLI source file (default: <repo>/tools/cli/pulp_cli.cpp).",
    )
    p.add_argument(
        "--mcp-source",
        type=Path,
        default=None,
        help="Override MCP source file (default: <repo>/tools/mcp/pulp_mcp.cpp).",
    )
    p.add_argument(
        "--baseline",
        type=Path,
        default=None,
        help="Override baseline file "
        "(default: <repo>/tools/scripts/cli_mcp_parity_baseline.json).",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = make_arg_parser().parse_args(argv)

    repo = args.repo_root or find_repo_root()
    if repo is None:
        print("Error: not in a Pulp project directory", file=sys.stderr)
        return 1

    cli_source = args.cli_source or (repo / "tools" / "cli" / "pulp_cli.cpp")
    mcp_source = args.mcp_source or (repo / "tools" / "mcp" / "pulp_mcp.cpp")
    baseline_path = args.baseline or (
        repo / "tools" / "scripts" / "cli_mcp_parity_baseline.json"
    )

    cli_commands = extract_cli_commands(cli_source)
    mcp_tools = extract_mcp_tools(mcp_source)
    baseline = load_baseline(baseline_path)
    diff = compute_diff(cli_commands, mcp_tools, baseline)

    if args.json:
        payload = {
            "mode": args.mode,
            "cli_commands": sorted(cli_commands),
            "mcp_tools": sorted(mcp_tools),
            "new_cli_only": sorted(diff.new_cli_only),
            "new_mcp_only": sorted(diff.new_mcp_only),
            "accepted_cli_only": sorted(diff.accepted_cli_only),
            "accepted_mcp_only": sorted(diff.accepted_mcp_only),
            "stale_cli_only_baseline": sorted(diff.stale_cli_only_baseline),
            "stale_mcp_only_baseline": sorted(diff.stale_mcp_only_baseline),
        }
        print(json.dumps(payload, indent=2))
    else:
        print(render_text(diff, color=not args.no_color, mode=args.mode))

    if args.mode == "report" and diff.new_cli_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
