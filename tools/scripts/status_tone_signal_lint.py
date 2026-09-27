#!/usr/bin/env python3
"""A widget that colours itself by status Tone must also draw a non-colour mark.

Status tone carries MEANING — success against danger is the whole point — and
colour alone cannot carry it. The light theme cannot separate those two under
protanopia and no token value fixes it: the shipped pair retains ~8.3 dE00, and
the intuitive remedy is worse, because protanopia darkens red until a dark green
lands on top of it. Roughly 8% of men are affected.

Contrast checking cannot see this. Contrast is a luminance measure, so
`--success` and `--danger` can each clear their bar against the surface and
still be one colour to the same reader. `check_palette_health.py` judges token
VALUES and never sees a widget at all, so nothing upstream of this catches a
component that differentiates by hue alone.

The rule: a `core/view` paint path that resolves a colour from a Tone must also
emit a second, non-colour signal. `tone_token()` is the shared Tone-to-colour
helper, so a file that calls it owes a `paint_tone_glyph()` call as well.

BLIND SPOT, stated rather than hidden: this hooks on `tone_token(`. A renderer
that maps Tone to a Color by some other route — a local switch returning a
literal, a theme lookup keyed off the enum — is invisible here. The check is a
tripwire on the one shared helper, not a proof that every component is
accessible. It catches the next component that does the obvious thing.

Escape a deliberate exception with an inline `status-tone-signal-lint: skip
<reason>` comment on the same line as the `tone_token(` call.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

COLOUR_FROM_TONE = "tone_token("
NON_COLOUR_SIGNAL = "paint_tone_glyph("
SKIP_MARKER = "status-tone-signal-lint: skip"
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".mm"}
SCAN_ROOTS = ("core/view/src",)


def _is_comment(line: str) -> bool:
    stripped = line.lstrip()
    return stripped.startswith(("//", "*", "/*"))


def source_files(root: Path) -> list[Path]:
    """Tracked sources under the scanned roots.

    Uses `git ls-files` when available: a plain walk from a repo root also
    descends into nested checkouts (this repo carries worktrees under
    `.claude/worktrees/`), which would lint other branches and report findings
    that do not exist on this one.
    """
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", *SCAN_ROOTS],
            capture_output=True, check=True, text=True,
        ).stdout
        paths = [root / n for n in out.split("\0") if n]
    except (subprocess.CalledProcessError, FileNotFoundError):
        paths = [p for scan in SCAN_ROOTS for p in (root / scan).rglob("*")
                 if p.is_file()]
    return sorted(p for p in paths if p.suffix in SOURCE_SUFFIXES and p.is_file())


def violations(root: Path) -> list[tuple[Path, int]]:
    """(path, lineno) for each colour-by-tone file with no non-colour signal."""
    found: list[tuple[Path, int]] = []
    for path in source_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if COLOUR_FROM_TONE not in text:
            continue
        # The signal may be emitted anywhere in the translation unit: the
        # helper that resolves the colour and the paint that draws the mark
        # are usually different functions.
        if NON_COLOUR_SIGNAL in text:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            if COLOUR_FROM_TONE in line and not _is_comment(line) \
                    and SKIP_MARKER not in line:
                found.append((path.relative_to(root), lineno))
                break
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[2],
        help="repository root to scan (default: this checkout)",
    )
    args = parser.parse_args(argv)
    found = violations(args.root.resolve())
    if not found:
        return 0

    print(
        f"{len(found)} widget source(s) colour themselves by status Tone with no "
        f"non-colour signal. Success and danger carry opposite meanings and are "
        f"indistinguishable by hue for ~8% of men, which no contrast check can see:",
        file=sys.stderr,
    )
    for path, lineno in found:
        print(f"  {path}:{lineno}: resolves a colour from Tone, but the file never "
              f"calls {NON_COLOUR_SIGNAL[:-1]}", file=sys.stderr)
    print(
        f"\nDraw a per-tone mark alongside the colour, or annotate the line "
        f"`{SKIP_MARKER} <reason>`.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
