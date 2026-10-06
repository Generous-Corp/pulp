#!/usr/bin/env python3
"""Verify an installed Pulp SDK is internally consistent with its source tree.

The design importer is a BINARY plus a `browser_capture-v1/` JavaScript runtime
and (for materialized imports) a sibling `jsx-runtime/` contract that ships
beside it. They are the halves of one tool: the binary drives the browser, the
runtime defines the capture protocol and every gate that runs inside the page,
and the sibling contract defines the shared binding schema. Refreshing one
without the others produces an SDK that looks updated, links fine, and silently
runs an old protocol.

That is not hypothetical. A capture gate added to the runtime was never copied
into an SDK whose binary HAD been refreshed, so a panel that the gate rejects
locally installed anyway on the target machine, truncated. The failure surfaced
as a rendering bug in a shipped app, hours after the "fix" was believed
delivered.

Checked here rather than at build time because this is about an INSTALLED tree:
a developer SDK is assembled by copying, and copying is where halves drift.

    tools/scripts/check_sdk_complete.py <sdk-prefix> [--source <pulp-checkout>]

Exit 0 when consistent, 1 when not, 2 on bad usage.
"""

from __future__ import annotations

import argparse
import filecmp
import json
import re
import sys
from pathlib import Path


VERSION_RE = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")


def _version_tuple(value: str) -> tuple[int, int, int]:
    if not VERSION_RE.fullmatch(value):
        raise ValueError(f"invalid SDK version: {value!r}")
    return tuple(int(part) for part in value.split("."))  # type: ignore[return-value]


def _node_runtime_required(prefix: Path, source: Path) -> bool:
    version = (prefix / "version.txt").read_text(encoding="utf-8").strip()
    matrix_path = source / "tools/scripts/release_product_matrix.json"
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    floor = str(matrix.get("node_runtime_floor", "999999.0.0"))
    return _version_tuple(version) >= _version_tuple(floor)


def _materialized_contract_required(prefix: Path, source: Path) -> bool:
    version = (prefix / "version.txt").read_text(encoding="utf-8").strip()
    matrix_path = source / "tools/scripts/release_product_matrix.json"
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    floor = str(matrix.get("materialized_binding_contract_floor", "999999.0.0"))
    return _version_tuple(version) >= _version_tuple(floor)


def check(prefix: Path, source: Path) -> list[str]:
    """Return a list of problems; empty means the SDK is consistent."""
    problems: list[str] = []

    importer = prefix / "bin" / "pulp-import-design"
    runtime = prefix / "bin" / "browser_capture-v1"
    node = runtime / ("node.exe" if sys.platform == "win32" else "node")
    node_license = runtime / "node.LICENSE"
    contract = prefix / "bin" / "jsx-runtime" / "materialized_binding_contract.mjs"
    src_runtime = source / "tools" / "import-design" / "browser_capture"
    src_contract = (
        source / "tools" / "import-design" / "jsx-runtime"
        / "materialized_binding_contract.mjs"
    )

    try:
        require_node = _node_runtime_required(prefix, source)
        require_contract = _materialized_contract_required(prefix, source)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        problems.append(f"cannot determine bundled Node requirement: {exc}")
        return problems

    if not importer.exists():
        problems.append(f"missing importer binary: {importer}")
    if require_node and not node.is_file():
        problems.append(f"missing bundled Node runtime: {node}")
    if require_node and not node_license.is_file():
        problems.append(f"missing bundled Node license: {node_license}")
    if require_contract and not contract.is_file():
        problems.append(f"missing materialized binding contract: {contract}")
    if not runtime.is_dir():
        problems.append(f"missing capture runtime directory: {runtime}")
    if problems:
        return problems

    if not src_runtime.is_dir():
        problems.append(f"source capture runtime not found: {src_runtime}")
        return problems
    if require_contract and not src_contract.is_file():
        problems.append(f"source materialized binding contract not found: {src_contract}")
    elif require_contract and not filecmp.cmp(src_contract, contract, shallow=False):
        problems.append(
            f"materialized binding contract is STALE in the SDK: {src_contract.name} "
            f"(copy {src_contract} -> {contract})"
        )

    # The manifest is the shipping contract, including non-JavaScript assets
    # such as interaction_plan_protocol.json. Checking only *.mjs lets a stale
    # JSON protocol survive an otherwise byte-identical SDK check.
    manifest = src_runtime / "runtime_manifest.txt"
    if not manifest.is_file():
        problems.append(f"runtime manifest missing from source: {manifest}")
        return problems
    shipped_names = tuple(
        line.strip() for line in manifest.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )
    if not shipped_names:
        problems.append(f"runtime manifest is empty: {manifest}")
        return problems

    for name in shipped_names:
        src = src_runtime / name
        installed = runtime / name
        if not src.is_file():
            problems.append(f"source runtime manifest entry is missing: {name}")
        elif not installed.is_file():
            problems.append(f"runtime asset missing from the SDK: {name}")
        elif not filecmp.cmp(src, installed, shallow=False):
            problems.append(
                f"runtime asset is STALE in the SDK: {name} "
                f"(copy {src} -> {installed}; rebuilding the binary will not "
                "fix it)")

    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path, help="installed SDK prefix")
    parser.add_argument(
        "--source", type=Path, default=Path(__file__).resolve().parents[2],
        help="Pulp source checkout the SDK was built from")
    args = parser.parse_args(argv)

    if not args.prefix.is_dir():
        print(f"error: no such SDK prefix: {args.prefix}", file=sys.stderr)
        return 2

    problems = check(args.prefix, args.source)
    if problems:
        print(f"SDK is inconsistent with {args.source}:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print(f"OK: {args.prefix} carries a complete, current design importer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
