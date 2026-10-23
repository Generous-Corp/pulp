#!/usr/bin/env python3
"""Check that FixedIrConvolver's direct-engine table matches its receipts.

The table in core/signal/include/pulp/signal/fixed_ir_convolver.hpp is
fitted from the per-host crossover CSVs in docs/validation/convolver-crossover/.
This recomputes it with the documented rule and fails if the header and the
data disagree, so neither can change without the other.

Rule (docs/validation/convolver-crossover.md): per chip and block size, the
limit is the largest measured IR length up to which the direct engine's mean
cost is no higher than the FFT engine's at every instance count, for every
measured length up to it. The shipped limit is the lowest over the chips.

Usage: convolver_crossover_check.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys

HEADER = pathlib.Path("core/signal/include/pulp/signal/fixed_ir_convolver.hpp")
DATA = pathlib.Path("docs/validation/convolver-crossover")


def fit(path: pathlib.Path) -> dict[int, int]:
    cost: dict[tuple[int, int, int, str], float] = {}
    with path.open(newline="") as fh:
        for row in csv.DictReader(fh):
            cost[(int(row["taps"]), int(row["block"]), int(row["instances"]), row["arm"])] = float(
                row["mean_ns_frame"]
            )
    taps = sorted({k[0] for k in cost})
    blocks = sorted({k[1] for k in cost})
    instances = sorted({k[2] for k in cost})
    limits = {}
    for block in blocks:
        best = 0
        for t in taps:
            if all(cost[(t, block, i, "direct")] <= cost[(t, block, i, "fft")] for i in instances):
                best = t
            else:
                break
        limits[block] = best
    return limits


def header_table(text: str) -> dict[int, int]:
    """Read the measured switch in direct_fir_limit() as {block: limit}."""
    body = text[text.index("static constexpr std::size_t direct_fir_limit") :]
    body = body[body.index("switch") : body.index("#else")]
    table: dict[int, int] = {}
    pending: list[int] = []
    default = None
    for line in body.splitlines():
        line = line.strip()
        if m := re.fullmatch(r"case (\d+):", line):
            pending.append(int(m.group(1)))
        elif line == "default:":
            pending.append(-1)
        elif m := re.fullmatch(r"return (\d+);", line):
            for b in pending:
                if b == -1:
                    default = int(m.group(1))
                else:
                    table[b] = int(m.group(1))
            pending = []
    if default is None:
        raise ValueError("direct_fir_limit() has no default row")
    table[-1] = default
    return table


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.repo_root

    files = sorted((root / DATA).glob("*.csv"))
    if not files:
        print(f"convolver-crossover: no CSVs under {DATA}", file=sys.stderr)
        return 1
    fits = {f.name: fit(f) for f in files}
    blocks = sorted(next(iter(fits.values())))
    expected = {b: min(f[b] for f in fits.values()) for b in blocks}
    table = header_table((root / HEADER).read_text())

    mismatches = []
    for b in blocks:
        got = table.get(b, table[-1])
        if got != expected[b]:
            mismatches.append(f"block {b}: header {got}, data {expected[b]} " + str({n: f[b] for n, f in fits.items()}))
    if mismatches:
        print("convolver-crossover: header table and receipts disagree:", file=sys.stderr)
        for m in mismatches:
            print("  " + m, file=sys.stderr)
        return 1
    print(f"convolver-crossover: header table matches {len(files)} receipts over {len(blocks)} block sizes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
