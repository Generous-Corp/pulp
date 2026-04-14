#!/usr/bin/env python3
"""
Adapter-vs-production split (workstream 08 slice 8.6).

Reads `docs/status/support-matrix.yaml` and reports on two things:

1. Adapter status: the flat `formats:` table — does the adapter build + load?
2. Production status: the `production_validated:` section — which DAW hosts
   has the format been successfully exercised in?

Any `formats.<fmt>.<platform>` at status `usable` or `stable` should have at
least one host listed in `production_validated.<fmt>.<platform>`. Until the
host-smoke matrix is populated, we run in warn-mode so CI does not regress.

Modes:
  --mode=warn     print findings, exit 0 (default)
  --mode=report   print findings, exit 1 on any gap

Exit codes:
  0 — clean, or warn mode
  1 — gaps found in report mode
  2 — input error
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MATRIX_PATH = REPO_ROOT / "docs" / "status" / "support-matrix.yaml"

HOST_VALIDATION_REQUIRED = {"usable", "stable"}


def extract_formats(text: str) -> dict[str, dict[str, str]]:
    m = re.search(r"^formats:\s*\n(.*?)(?=^\S|\Z)", text, flags=re.MULTILINE | re.DOTALL)
    if not m:
        return {}
    block = m.group(1)
    out: dict[str, dict[str, str]] = {}
    cur: str | None = None
    for line in block.splitlines():
        mk = re.match(r"^  ([a-z0-9_]+):\s*$", line)
        if mk:
            cur = mk.group(1)
            out[cur] = {}
            continue
        mp = re.match(r"^    ([a-z]+):\s*([a-z]+)\s*$", line)
        if mp and cur:
            out[cur][mp.group(1)] = mp.group(2)
    return out


def extract_production(text: str) -> dict[str, dict[str, list[str]]]:
    m = re.search(
        r"^production_validated:\s*\n(.*?)(?=^\S|\Z)",
        text, flags=re.MULTILINE | re.DOTALL,
    )
    if not m:
        return {}
    block = m.group(1)
    out: dict[str, dict[str, list[str]]] = {}
    cur: str | None = None
    for line in block.splitlines():
        if line.lstrip().startswith("#") or not line.strip():
            continue
        mk = re.match(r"^  ([a-z0-9_]+):\s*$", line)
        if mk:
            cur = mk.group(1)
            out[cur] = {}
            continue
        me = re.match(r"^    ([a-z]+):\s*\[\]\s*$", line)
        if me and cur:
            out[cur][me.group(1)] = []
            continue
        mi = re.match(r"^    ([a-z]+):\s*\[([^\]]*)\]\s*$", line)
        if mi and cur:
            items = [s.strip().strip('"').strip("'")
                     for s in mi.group(2).split(",") if s.strip()]
            out[cur][mi.group(1)] = items
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("warn", "report"), default="warn")
    args = ap.parse_args()
    try:
        text = MATRIX_PATH.read_text()
    except Exception as e:
        sys.stderr.write(f"Failed to read {MATRIX_PATH}: {e}\n")
        return 2
    formats = extract_formats(text)
    prod = extract_production(text)
    gaps_missing: list[str] = []
    gaps_empty: list[tuple[str, str]] = []
    ok = 0
    for fmt, plats in formats.items():
        for plat, status in plats.items():
            if status not in HOST_VALIDATION_REQUIRED:
                continue
            hosts = prod.get(fmt, {}).get(plat)
            if hosts is None:
                gaps_missing.append(f"{fmt}.{plat}")
            elif not hosts:
                gaps_empty.append((f"{fmt}.{plat}", status))
            else:
                ok += 1
    print(
        f"format-validation: {ok} format/platform pairs host-validated, "
        f"{len(gaps_empty)} acknowledged but empty, "
        f"{len(gaps_missing)} missing from production_validated (mode={args.mode})"
    )
    for path in gaps_missing:
        print(f"  MISSING: production_validated.{path} absent")
    for path, status in gaps_empty:
        print(f"  EMPTY:   {path} is '{status}' but production_validated list is []")

    if args.mode == "report" and (gaps_missing or gaps_empty):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
