#!/usr/bin/env python3
"""Run the typed bridge drift check with language-level safety validation first.

This is the authoritative read-only contract gate for new bridge schemas.  It
uses the existing generator for parsing and deterministic output comparison,
but refuses to render or compare a schema whose generated C++ or TypeScript
names are unsafe.  ``--write`` is provided for local regeneration after the
source has passed the same safety audit.
"""

# CTest input tracking: keep dynamic module loading visible to the affected
# test selector so safety or generator changes rerun this production gate.
# "tools/bridge/bridge_contract_safety.py"
# "tools/bridge/bridge_gen.py"
# "tools/bridge/bridge.toml"
# "tools/bridge/generated_editor_bridge.hpp"
# "tools/bridge/generated_editor_bridge.ts"
# "docs/reference/generated-editor-bridge-contract.md"

from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path
from typing import TextIO


HERE = Path(__file__).resolve().parent


def _load_module(filename: str, module_name: str):
    path = HERE / filename
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load bridge module: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


safety = _load_module("bridge_contract_safety.py", "pulp_bridge_safety_for_check")
generator = safety.generator


def run(
    source: Path,
    outputs: dict[str, Path],
    *,
    write: bool = False,
    stream: TextIO | None = None,
) -> int:
    """Validate one contract and return a process-style status code."""

    stream = stream or sys.stderr
    try:
        data = generator.load_contract(source)
    except ValueError as exc:
        print(f"bridge contract: {exc}", file=stream)
        return 1

    problems = safety.audit(data)
    if problems:
        print("bridge contract safety failed:", file=stream)
        for problem in problems:
            print(f"  {problem}", file=stream)
        return 1

    rendered = generator.render(data, outputs)
    if write:
        for path, content in rendered.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        print(f"bridge contract: wrote {len(rendered)} outputs", file=stream)
        return 0

    if not generator.check(rendered):
        return 1
    print(f"bridge contract: OK ({source})", file=stream)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=generator.SOURCE)
    parser.add_argument("--cpp", type=Path, default=generator.OUTPUTS["cpp"])
    parser.add_argument("--ts", type=Path, default=generator.OUTPUTS["ts"])
    parser.add_argument("--docs", type=Path, default=generator.OUTPUTS["docs"])
    parser.add_argument(
        "--write",
        action="store_true",
        help="write deterministic outputs after the safety audit instead of checking drift",
    )
    args = parser.parse_args(argv)
    return run(
        args.source,
        {"cpp": args.cpp, "ts": args.ts, "docs": args.docs},
        write=args.write,
    )


if __name__ == "__main__":
    raise SystemExit(main())
