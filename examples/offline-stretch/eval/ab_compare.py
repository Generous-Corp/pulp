#!/usr/bin/env python3
"""Agent-runnable A/B harness for Pulp's offline stretch engine.

Render a source through `stretchcli` under one or more configs (or presets) across
a set of ratios, print the quality metrics side by side, and (optionally) diff
against a reference render you supply. This is the loop we used to tune the engine
— now packaged so a user or their agent can do the same to dial in a preset.

Examples
  # compare clean vs varispeed on a drum across ratios
  python ab_compare.py drum.wav --cli ../../../build/examples/offline-stretch/stretchcli \\
      --ratios 0.75,1.5,2.0 --configs "clean:--character clean" "tape:--character varispeed"

  # diff a render against a reference file (e.g. one you made with another tool)
  python ab_compare.py vocal.wav --ratios 2.0 \\
      --configs "ours:--character clean" --reference ref_2x.wav

Only numpy + soundfile are required. Rubber Band (GPL) is NOT bundled; if you want
to compare against it, render with your own `rubberband` binary and pass the file
via --reference. See NOTICE.md.
"""
import argparse
import subprocess
import sys
import tempfile
import os

import stretch_metrics as m


def render(cli, src, out, ratio, extra_args):
    cmd = [cli, src, out, "--ratio", str(ratio), "--quality", "2", "--max-ratio", "4", *extra_args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"render failed ({ratio}, {extra_args}): {r.stderr.strip()}\n")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="A/B the Pulp offline stretch engine.")
    ap.add_argument("source", help="input wav")
    ap.add_argument("--cli", default="stretchcli", help="path to stretchcli binary")
    ap.add_argument("--ratios", default="1.5,2.0", help="comma list of time ratios")
    ap.add_argument("--configs", nargs="+", default=["clean:--character clean"],
                    help='one or more "label:cli args" configs')
    ap.add_argument("--reference", default=None,
                    help="optional reference wav to diff the FIRST ratio against")
    args = ap.parse_args()

    ratios = [float(x) for x in args.ratios.split(",")]
    configs = []
    for c in args.configs:
        label, _, rest = c.partition(":")
        configs.append((label, rest.split()))

    src_summary = m.summary(args.source)
    print(f"SOURCE {args.source}: {src_summary}\n")
    print(f"{'config':14} {'ratio':>5} {'centroid':>9} {'onset':>6} {'peakHz':>7} {'wobble':>7}")
    print("-" * 56)

    tmp = tempfile.mkdtemp(prefix="stretch_ab_")
    for label, extra in configs:
        for r in ratios:
            out = os.path.join(tmp, f"{label}_{r}.wav")
            if not render(args.cli, args.source, out, r, extra):
                continue
            s = m.summary(out)
            print(f"{label:14} {r:>5} {s['centroid']:>9.0f} {s['onset']:>6.1f} "
                  f"{s['peak_hz']:>7.1f} {s['wobble']:>7.2f}")
            if args.reference and r == ratios[0]:
                l1 = m.spectral_l1(out, args.reference)
                print(f"  -> spectral-L1 vs reference {os.path.basename(args.reference)}: {l1:.3f}")
    print(f"\n(renders in {tmp}; centroid=brightness, onset=punch, peakHz vs source=pitch, wobble=instability)")


if __name__ == "__main__":
    main()
