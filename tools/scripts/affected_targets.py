#!/usr/bin/env python3
"""Print the CMake targets and CTest tests affected by the working diff.

This is the command-line face of the build-target projection in
``changed_surface_inventory.py`` (the same module Shipyard's exact-head
changed-surface plan reads its CTest inventory through). ``pulp build``,
``pulp dev``, ``pulp loop``, and ``pulp test`` run it before building and read
the files it writes; ``pulp affected`` forwards its arguments here.

The projection maps the branch diff (merge-base with ``--base``) plus staged,
unstaged, and untracked files to the targets that own them, adds the test
programs named after them, follows ``add_dependencies`` and CTest fixture
edges, honours the Shipyard policy families in ``.shipyard/config.toml``, and
falls back to ``all`` (saying why) whenever the mapping is incomplete or too
large to be worth focusing. The pre-push hook and Shipyard keep building
``all``; this is a development-loop optimization, not a correctness gate.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import changed_surface_inventory as inventory  # noqa: E402

DEFAULT_BASE = os.environ.get("PULP_AFFECTED_BASE", "origin/main")
SHIPYARD_CONFIG_RELATIVE = Path(".shipyard") / "config.toml"
SHIPYARD_TARGET = "mac"


def policy_families(source_root: Path | None) -> list[dict[str, Any]]:
    """The Shipyard changed-surface families for this checkout, or none."""
    if source_root is None:
        return []
    config = source_root / SHIPYARD_CONFIG_RELATIVE
    if not config.is_file():
        return []
    try:
        import run_changed_surface_tests as runner

        policy = runner.load_policy(config, SHIPYARD_TARGET)
    except Exception:  # noqa: BLE001 - a broken policy must not break a build
        return []
    families = policy.get("families", [])
    return [f for f in families if isinstance(f, dict)]


def write_outputs(selection: inventory.Selection, write_dir: Path) -> None:
    write_dir.mkdir(parents=True, exist_ok=True)
    (write_dir / "selection.json").write_text(
        json.dumps(selection.to_json(), indent=2) + "\n", encoding="utf-8")
    (write_dir / "banner.txt").write_text(selection.banner + "\n", encoding="utf-8")
    for name, lines in (("targets.txt", selection.targets), ("tests.txt", selection.tests)):
        path = write_dir / name
        if selection.mode == "focused":
            path.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
        elif path.exists():
            path.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--build-dir", default="build", help="configured CMake build directory")
    parser.add_argument("--source-root", default=None,
                        help="repository root (default: from the codemodel)")
    parser.add_argument("--base", default=DEFAULT_BASE,
                        help="ref whose merge-base bounds the branch diff "
                             "(default: $PULP_AFFECTED_BASE or origin/main)")
    parser.add_argument("--threshold", type=float, default=inventory.DEFAULT_PROJECTION_THRESHOLD,
                        help="fall back to all above this fraction of targets")
    parser.add_argument("--file", action="append", dest="files",
                        help="use this changed path instead of asking git (repeatable)")
    parser.add_argument("--no-tests", action="store_true", help="skip the ctest inventory")
    parser.add_argument("--no-families", action="store_true",
                        help="ignore the Shipyard changed-surface policy families")
    parser.add_argument("--json", action="store_true", help="print the selection as JSON")
    parser.add_argument("--write-dir", default=None,
                        help="also write selection.json, banner.txt, targets.txt, tests.txt here")
    parser.add_argument("--quiet", action="store_true", help="print nothing but errors")
    args = parser.parse_args(argv)

    build_dir = Path(args.build_dir).resolve()
    source_root = Path(args.source_root).resolve() if args.source_root else None
    families = [] if args.no_families else policy_families(source_root)
    selection = inventory.project_working_diff(
        build_dir, source_root, args.base, args.threshold, args.files,
        with_tests=not args.no_tests, families=families)
    if args.write_dir:
        write_outputs(selection, Path(args.write_dir))
    if args.json:
        print(json.dumps(selection.to_json(), indent=2))
    elif not args.quiet:
        print(selection.banner)
        if selection.mode == "focused":
            print("targets:")
            for name in selection.targets:
                print(f"  {name}")
            print(f"tests: {len(selection.tests)} of {selection.total_tests}")
            if selection.unmapped:
                print(f"unmapped (no build target): {len(selection.unmapped)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
