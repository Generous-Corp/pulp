#!/usr/bin/env python3
"""Resolve and police the runs-on selector for a dispatch-routed Windows job.

A workflow_dispatch input can move a Windows job onto a self-hosted runner. A
typo or a pasted macOS selector would otherwise send the Windows build onto the
macOS gate VMs, or onto the Linux pool, where it either queues behind the
required gate or runs a Windows job on the wrong OS. This resolves the input
before any runner is chosen and refuses anything that is not a Windows
selector.

    windows_runner_selector.py --selector '<json or empty>' [--github-output]

An empty selector resolves to the GitHub-hosted fallback (--fallback,
default windows-latest). Exit 0 prints (or writes) runs_on=<compact JSON>;
exit 2 refuses the selector with a reason on stderr.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

LABEL = re.compile(r"^[A-Za-z0-9._-]+$")
# Labels that belong to another OS's pool. Matched case-insensitively against
# whole labels, and as substrings for capability labels such as
# pulp-build-linux-x64 or pulp-gate-fast.
FOREIGN_LABELS = {"macos", "osx", "darwin", "linux", "ubuntu"}
FOREIGN_FRAGMENTS = ("macos", "linux", "ubuntu", "pulp-gate", "pulp-build-vm", "pulp-preamble")


class SelectorError(ValueError):
    pass


def _check_label(label: object) -> str:
    if not isinstance(label, str) or not LABEL.match(label):
        raise SelectorError(f"invalid runner label: {label!r}")
    folded = label.casefold()
    if folded in FOREIGN_LABELS or any(fragment in folded for fragment in FOREIGN_FRAGMENTS):
        raise SelectorError(f"label {label!r} selects a non-Windows runner pool")
    return label


def resolve(raw: str, fallback: str = "windows-latest") -> str:
    """Return the compact JSON runs-on value for a selector input."""
    raw = raw.strip()
    if not raw:
        return json.dumps(fallback)
    try:
        value = json.loads(raw)
    except ValueError as exc:
        raise SelectorError(f"selector is not JSON: {exc}") from exc
    if isinstance(value, str):
        label = _check_label(value)
        if not label.casefold().startswith("windows-"):
            raise SelectorError(f"a single label must be a GitHub-hosted Windows image, not {label!r}")
        return json.dumps(label)
    if not isinstance(value, list) or not value:
        raise SelectorError("selector must be a label string or a non-empty list of labels")
    labels = [_check_label(label) for label in value]
    if len(set(label.casefold() for label in labels)) != len(labels):
        raise SelectorError("selector repeats a label")
    folded = {label.casefold() for label in labels}
    if "windows" not in folded:
        raise SelectorError("a self-hosted selector must include the Windows label")
    if "self-hosted" not in folded:
        raise SelectorError("a label list must include self-hosted")
    return json.dumps(labels, separators=(",", ":"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--selector", default="")
    parser.add_argument("--fallback", default="windows-latest")
    parser.add_argument("--github-output", action="store_true",
                        help="append runs_on=... to $GITHUB_OUTPUT")
    args = parser.parse_args(argv)
    try:
        _check_label(args.fallback)
        runs_on = resolve(args.selector, args.fallback)
    except SelectorError as exc:
        print(f"windows-runner-selector: refused: {exc}", file=sys.stderr)
        return 2
    line = f"runs_on={runs_on}"
    if args.github_output:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as handle:
            handle.write(line + "\n")
    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
