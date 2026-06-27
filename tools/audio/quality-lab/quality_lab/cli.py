"""CLI entry — parse + dispatch only; the work lives in `pipeline` (§14.4 boundary).

P0a surface (the first-class `pulp audio quality` verb arrives in P1):

    python -m quality_lab.cli run-p0a --out report.json [--mode good|bad]
                                      [--smear-ms 8] [--latency-ms 5]
"""
from __future__ import annotations

import argparse
import json
import sys

from . import pipeline


def _cmd_run_p0a(args: argparse.Namespace) -> int:
    report = pipeline.run_p0a(
        smear=(args.mode == "bad"),
        latency_ms=args.latency_ms,
        smear_ms=args.smear_ms,
    )
    text = json.dumps(report, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    det = report["detectors"][0]
    print(
        f"[quality-lab P0a] mode={args.mode} verdict={report['verdict']} "
        f"transient_sharpness scalar={det['scalar']:.3f} fired={det['fired']}"
    )
    for w in report["worst_regions"][:3]:
        print(f"  worst: t={w['time_s']:.3f}s severity={w['severity']:.3f} {w['label']}")
    if args.out:
        print(f"[quality-lab P0a] wrote {args.out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="quality-lab", description="Audio Quality Lab (P0a)")
    sub = p.add_subparsers(dest="cmd", required=True)

    rp = sub.add_parser("run-p0a", help="run the P0a drum-break slice")
    rp.add_argument("--out", default="", help="write report.json to this path")
    rp.add_argument("--mode", choices=["good", "bad"], default="bad")
    rp.add_argument("--smear-ms", type=float, default=8.0, dest="smear_ms")
    rp.add_argument("--latency-ms", type=float, default=5.0, dest="latency_ms")
    rp.set_defaults(func=_cmd_run_p0a)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
