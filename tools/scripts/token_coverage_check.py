#!/usr/bin/env python3
"""token_coverage_check.py — ratchet guard against NEW hardcoded theme colors.

Reskinnable widgets must resolve their colors from theme tokens
(`resolve_color("token", fallback)`); a bare color literal in paint code can
never be restyled by a theme/token swap.

This is a RATCHET, not an absolute gate. It freezes the current per-file count
of non-`resolve_color` color literals in the view layer as a baseline and fails
only when a file's count INCREASES. Existing literals — material-effect shadows
and highlights, legacy paint not yet tokenised — are grandfathered; new ones are
blocked, so coverage only ever improves. Lowering a count below the baseline
auto-tightens the ratchet on the next `--update-baseline`.

Conventions:
  * A literal is "covered" (not counted) when its line also contains
    `resolve_color` — i.e. it is the fallback argument of a token lookup.
  * Mark an intentional material-effect literal (a drop shadow, a hairline
    highlight) with a trailing `// token-lint:allow` to exclude it.
  * Color-authoring files (theme*, design_tokens, color_picker) are excluded —
    their job is to DEFINE colors, not resolve tokens.

Usage:
  token_coverage_check.py                 # check against the baseline (exit 1 on regression)
  token_coverage_check.py --update-baseline   # rewrite the baseline to current counts
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_DIR = REPO_ROOT / "core" / "view" / "src"
BASELINE = Path(__file__).resolve().parent / "token_coverage_baseline.json"

# Files whose purpose is color authoring, not widget paint — excluded wholesale.
EXCLUDE_BASENAMES = {
    "theme.cpp",
    "theme_presets.cpp",
    "theme_contrast.cpp",
    "design_tokens.cpp",
    "color_picker.cpp",
}

LITERAL_RE = re.compile(r"color_from_hex\(\s*0x|Color::rgba8?\(|Color::rgb\(")


def baseline_key(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def count_hardcodes(path: Path) -> int:
    n = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "resolve_color" in line:
            continue  # the literal is a token fallback — correct pattern
        if "token-lint:allow" in line:
            continue  # explicitly-allowed material-effect literal
        if LITERAL_RE.search(line):
            n += 1
    return n


def scan() -> dict[str, int]:
    counts: dict[str, int] = {}
    for path in sorted(SCAN_DIR.rglob("*.cpp")):
        if path.name in EXCLUDE_BASENAMES:
            continue
        c = count_hardcodes(path)
        if c:
            counts[baseline_key(path)] = c
    return counts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite the baseline to the current counts")
    args = ap.parse_args()

    current = scan()

    if args.update_baseline:
        BASELINE.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        print(f"[token-coverage] baseline updated: {len(current)} files, "
              f"{sum(current.values())} grandfathered literals")
        return 0

    if not BASELINE.exists():
        print(f"[token-coverage] missing baseline {BASELINE}; run --update-baseline",
              file=sys.stderr)
        return 2

    baseline = json.loads(BASELINE.read_text())
    regressions = []
    for f, c in sorted(current.items()):
        allowed = baseline.get(f, 0)
        if c > allowed:
            regressions.append((f, allowed, c))

    if regressions:
        print("[token-coverage] RATCHET FAIL — new hardcoded theme color(s):",
              file=sys.stderr)
        for f, allowed, c in regressions:
            print(f"  {f}: {c} literals (baseline {allowed}, +{c - allowed})",
                  file=sys.stderr)
        print("  Use resolve_color(\"token\", fallback), or mark a deliberate "
              "material-effect literal with `// token-lint:allow`.", file=sys.stderr)
        return 1

    print(f"[token-coverage] ok — {sum(current.values())} grandfathered literals, "
          f"no new hardcodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
