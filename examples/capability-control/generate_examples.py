#!/usr/bin/env python3
"""Generate installed CLI and MCP examples from one reviewable corpus."""

from __future__ import annotations

import argparse
import json
import shlex
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "control-examples.json"
GENERATED = ROOT / "generated"


def load_examples() -> list[dict]:
    data = json.loads(SOURCE.read_text())
    if data.get("schema") != "dev.pulp.control/examples@1":
        raise ValueError("unknown example corpus schema")
    examples = data.get("examples")
    if not isinstance(examples, list) or not examples:
        raise ValueError("example corpus must contain examples")
    ids = [item.get("id") for item in examples]
    if any(not isinstance(item, str) or not item for item in ids) or len(ids) != len(set(ids)):
        raise ValueError("example IDs must be nonempty and unique")
    return examples


def render_cli(examples: list[dict]) -> str:
    def shell_token(token: str) -> str:
        if "${" not in token:
            return shlex.quote(token)
        return '"' + token.replace("\\", "\\\\").replace('"', '\\"') + '"'

    lines = [
        "#!/bin/sh",
        "# Generated from control-examples.json. Run with an installed `pulp` on PATH.",
        "set -eu",
        "",
        'case "${1:-inventory}" in',
    ]
    for item in examples:
        lines.extend((f"  {item['id']})",
                      f"    # {item['tier']}: {item['description']}"))
        for name in item.get("requires", []):
            lines.append(f'    : "${{{name}:?set {name} before running {item["id"]}}}"')
        lines.extend(("    " + " ".join(shell_token(token) for token in item["cli"]),
                      "    ;;"))
    choices = "|".join(item["id"] for item in examples)
    lines.extend(("  *)",
                  f'    echo "usage: $0 {{{choices}}}" >&2',
                  "    exit 2",
                  "    ;;",
                  "esac",
                  ""))
    return "\n".join(lines)


def render_mcp(examples: list[dict]) -> str:
    return "".join(
        json.dumps({"id": item["id"], "tier": item["tier"], **item["mcp"]},
                   separators=(",", ":"), sort_keys=True) + "\n"
        for item in examples
    )


def outputs(examples: list[dict]) -> dict[Path, str]:
    return {
        GENERATED / "cli-walkthrough.sh": render_cli(examples),
        GENERATED / "mcp-tools.jsonl": render_mcp(examples),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rendered = outputs(load_examples())
    if args.check:
        stale = [str(path.relative_to(ROOT)) for path, text in rendered.items()
                 if not path.exists() or path.read_text() != text]
        if stale:
            raise SystemExit("stale generated capability-control examples: " + ", ".join(stale))
        return 0
    GENERATED.mkdir(parents=True, exist_ok=True)
    for path, text in rendered.items():
        path.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
