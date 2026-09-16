#!/usr/bin/env python3
"""Catch shell syntax that silently changes meaning under zsh.

This is intentionally narrow: it checks tracked shell scripts for unbraced
variable-plus-colon expansions and Bash-only PIPESTATUS usage. It does not try
to lint shell generally.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

UNBRACED_COLON = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*):([A-Za-z])")
PIPESTATUS = re.compile(r"\$(?:\{PIPESTATUS(?:\[|\})|PIPESTATUS(?:\[0-9]+)?\b)")


def check_file(path: pathlib.Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    bash = bool(re.search(r"(?:env +)?bash|shell:\s*bash", text[:400]))
    findings: list[str] = []
    for number, line in enumerate(lines, 1):
        match = UNBRACED_COLON.search(line)
        if match and "$env:" not in line:
            findings.append(
                f"{path}:{number}: zsh colon-expansion hazard "
                f"'${match.group(1)}:{match.group(2)}'; use "
                f"'${{{match.group(1)}}}:{match.group(2)}...'"
            )
        if PIPESTATUS.search(line) and not bash:
            findings.append(
                f"{path}:{number}: Bash-only PIPESTATUS in a non-Bash file; "
                "use zsh pipestatus or an explicit bash boundary"
            )
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", type=pathlib.Path)
    parser.add_argument("--rules", type=pathlib.Path,
                        default=pathlib.Path(__file__).with_name("shell_portability_rules.json"))
    args = parser.parse_args()
    rules = __import__("json").loads(args.rules.read_text(encoding="utf-8"))
    if rules.get("schema_version") != 1 or not rules.get("rules"):
        parser.error("invalid shell portability rules manifest")
    paths = args.paths or [pathlib.Path("tools/ci"), pathlib.Path("scripts"), pathlib.Path("tools/scripts")]
    files = [
        p for root in paths
        for p in ([root] if root.is_file() else root.rglob("*"))
        if p.is_file() and p.suffix in {".sh", ".bash", ".zsh"}
    ]
    findings = [finding for path in sorted(files) for finding in check_file(path)]
    if findings:
        print("\n".join(findings), file=sys.stderr)
        return 1
    print(f"shell portability OK across {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
