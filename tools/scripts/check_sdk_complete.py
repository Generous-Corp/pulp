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


def _runtime_manifest(source_runtime: Path) -> list[Path]:
    """Return the shipped runtime paths from the source manifest.

    The manifest is authoritative because the runtime contains both modules
    and data files (currently a JSON interaction protocol). Enumerating only
    ``*.mjs`` silently lets a stale JSON payload pass the SDK completeness
    check.
    """
    manifest = source_runtime / "runtime_manifest.txt"
    entries: list[Path] = []
    for raw in manifest.read_text(encoding="utf-8").splitlines():
        value = raw.split("#", 1)[0].strip()
        if not value:
            continue
        relative = Path(value)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"invalid runtime manifest entry: {value!r}")
        entries.append(relative)
    return entries


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

    try:
        shipped = _runtime_manifest(src_runtime)
    except (OSError, ValueError) as exc:
        problems.append(f"cannot read source runtime manifest: {exc}")
        return problems
    if not shipped:
        problems.append(f"source runtime manifest is empty: {src_runtime}")
        return problems

    for relative in shipped:
        src = src_runtime / relative
        installed = runtime / relative
        if not src.is_file():
            problems.append(f"runtime file missing from source tree: {relative}")
            continue
        if not installed.exists():
            problems.append(f"runtime file missing from the SDK: {relative}")
        elif not filecmp.cmp(src, installed, shallow=False):
            # The message names the fix, because the instinct on seeing this is
            # to rebuild the binary — which is already current and is not what
            # drifted.
            problems.append(
                f"runtime module is STALE in the SDK: {src.name} "
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
