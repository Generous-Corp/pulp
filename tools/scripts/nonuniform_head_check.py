#!/usr/bin/env python3
"""Check that NonUniformPartitionedConvolver's tail-multiplier table matches its receipts.

The table in core/signal/include/pulp/signal/convolver_non_uniform.hpp is
fitted from the per-run CSVs in docs/validation/nonuniform-head/. This
recomputes it with the documented rule and fails if the header and the data
disagree, so neither can change without the other.

Rule (docs/validation/nonuniform-head.md): per receipt file (one chip, all of
its runs), IR length and block size,
each K's cost is the median over rounds of its mean ns per frame, and its
regret is that cost over the cheapest K's in the same file. The shipped K is the
one whose worst regret over the files is lowest (the smaller K on a tie).

Usage: nonuniform_head_check.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import statistics
import sys

HEADER = pathlib.Path("core/signal/include/pulp/signal/convolver_non_uniform.hpp")
DATA = pathlib.Path("docs/validation/nonuniform-head")


def costs(path: pathlib.Path) -> dict[tuple[int, int], dict[int, float]]:
    samples: dict[tuple[int, int], dict[int, list[float]]] = {}
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            cell = (int(row["ir"]), int(row["B"]))
            samples.setdefault(cell, {}).setdefault(int(row["K"]), []).append(
                float(row["mean_ns_frame"]))
    return {cell: {k: statistics.median(v) for k, v in ks.items()} for cell, ks in samples.items()}


def fit(runs: list[dict[tuple[int, int], dict[int, float]]]) -> dict[tuple[int, int], int]:
    table = {}
    for cell in sorted(runs[0]):
        ks = sorted(runs[0][cell])
        worst = {}
        for k in ks:
            worst[k] = max(run[cell][k] / min(run[cell].values()) for run in runs)
        table[cell] = min(ks, key=lambda k: (worst[k], k))
    return table


def header_table(text: str) -> tuple[list[int], dict[int, list[int]]]:
    """Read choose_tail_multiplier()'s measured IR columns and rows."""
    body = text[text.index("static constexpr std::size_t choose_tail_multiplier") :]
    body = body[: body.index("#else")]
    columns = [int(v) for v in re.search(r"kMeasuredIr\{([^}]*)\}", body).group(1).split(",")]
    rows: dict[int, list[int]] = {}
    pending: list[int] = []
    for line in body.splitlines():
        line = line.strip()
        if m := re.fullmatch(r"case (\d+):", line):
            pending.append(int(m.group(1)))
        elif m := re.fullmatch(r"return std::array<std::size_t, \d+>\{([^}]*)\}\[column\];", line):
            for b in pending:
                rows[b] = [int(v) for v in m.group(1).split(",")]
            pending = []
    return columns, rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.repo_root

    files = sorted((root / DATA).glob("*.csv"))
    if not files:
        print(f"nonuniform-head: no CSVs under {DATA}", file=sys.stderr)
        return 1
    runs = [costs(f) for f in files]
    if any(set(r) != set(runs[0]) for r in runs):
        print("nonuniform-head: the receipts do not cover the same cells", file=sys.stderr)
        return 1
    expected = fit(runs)
    columns, rows = header_table((root / HEADER).read_text(encoding="utf-8"))

    measured_ir = sorted({cell[0] for cell in expected})
    measured_b = sorted({cell[1] for cell in expected})
    problems = []
    if columns != measured_ir:
        problems.append(f"header IR columns {columns}, receipts measure {measured_ir}")
    if sorted(rows) != measured_b:
        problems.append(f"header block rows {sorted(rows)}, receipts measure {measured_b}")
    for (ir, b), k in sorted(expected.items()):
        if b in rows and ir in columns:
            got = rows[b][columns.index(ir)]
            if got != k:
                problems.append(f"IR {ir}, block {b}: header {got}, data {k}")
    if problems:
        print("nonuniform-head: header table and receipts disagree:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1
    print(f"nonuniform-head: header table matches {len(files)} receipts over "
          f"{len(expected)} IR/block cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
