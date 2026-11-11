#!/usr/bin/env python3
"""Advisory report of transferred UI paths that drifted from the export.

A transferred UI path that is not frozen can still change in Pulp. This lists
each such path whose blob at the checked head differs from the blob recorded
in the export receipt, so every drifted path is ported to Vellum or dropped
before cutover. The report never blocks: it exits 0 whether or not it finds
drift, and exits 2 only when an input cannot be read or is inconsistent.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parents[2]
RECEIPT_DIR = ".github/vellum-export-receipt"
PIN_PATH = f"{RECEIPT_DIR}/pin.json"
TRANSFER_PATH = f"{RECEIPT_DIR}/framework-transfer.json"
UNIT_PATH = f"{RECEIPT_DIR}/unit-of.json"

UI_UNITS = frozenset(
    {"canvas", "render-2d", "view-core", "design-import", "view-script"}
)
NON_UI_UNITS = frozenset(
    {"foundation", "state-core", "dataplane", "gpu-device-api"}
)


class ReportError(RuntimeError):
    pass


def _git(root: pathlib.Path, *args: str) -> bytes:
    proc = subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, check=False
    )
    if proc.returncode != 0:
        raise ReportError(
            f"git {' '.join(args)} failed: {proc.stderr.decode().strip()}"
        )
    return proc.stdout


def _load(root: pathlib.Path, path: str) -> Any:
    try:
        return json.loads((root / path).read_text())
    except OSError as exc:
        raise ReportError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ReportError(f"{path} is not valid JSON: {exc}") from exc


def head_blobs(root: pathlib.Path, head: str) -> dict[str, str]:
    blobs = {}
    for line in _git(root, "ls-tree", "-r", "-z", head).split(b"\0"):
        if not line:
            continue
        meta, _, path = line.partition(b"\t")
        blobs[path.decode()] = meta.split()[2].decode()
    return blobs


def build_report(root: pathlib.Path, head: str) -> dict[str, Any]:
    """Compare blobs at head with the export records in the checkout."""
    pin = _load(root, PIN_PATH)
    transfer = _load(root, TRANSFER_PATH)
    units = _load(root, UNIT_PATH)
    receipt = _load(root, pin["receipt_path"])

    transferred = transfer["transferred_paths"]
    unknown = sorted(
        {units.get(p, "<missing>") for p in transferred}
        - UI_UNITS
        - NON_UI_UNITS
    )
    if unknown:
        raise ReportError(f"units not on the allowlist: {', '.join(unknown)}")

    recorded = {f["pulp_path"]: f["blob"] for f in receipt["files"]}
    absent = sorted(p for p in transferred if p not in recorded)
    if absent:
        raise ReportError(
            f"{len(absent)} transferred paths have no receipt blob, "
            f"first: {absent[0]}"
        )

    frozen = set(pin["frozen_paths"])
    current = head_blobs(root, head)
    ui_paths = [p for p in transferred if units[p] in UI_UNITS]
    candidates = [p for p in ui_paths if p not in frozen]
    drifted = [
        {
            "path": p,
            "unit": units[p],
            "recorded_blob": recorded[p],
            "head_blob": current.get(p),
        }
        for p in sorted(candidates)
        if current.get(p) != recorded[p]
    ]
    return {
        "head": _git(root, "rev-parse", head).decode().strip(),
        "ui_paths": len(ui_paths),
        "unfrozen_ui_paths": len(candidates),
        "drift_count": len(drifted),
        "drifted": drifted,
    }


def render_text(report: dict[str, Any]) -> str:
    lines = [
        f"head {report['head']}",
        f"{report['ui_paths']} transferred UI paths, "
        f"{report['unfrozen_ui_paths']} not frozen, "
        f"{report['drift_count']} drifted from the export",
    ]
    for item in report["drifted"]:
        state = "deleted" if item["head_blob"] is None else "changed"
        lines.append(f"  {state}: {item['path']} ({item['unit']})")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--head", default="HEAD", help="ref whose blobs are compared")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    try:
        report = build_report(args.root, args.head)
    except (ReportError, KeyError, TypeError) as exc:
        print(f"vellum-ui-drift-report: cannot read input: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2) if args.as_json else render_text(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
