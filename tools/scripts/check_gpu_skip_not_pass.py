#!/usr/bin/env python3
"""Enforce that an unmet GPU precondition is reported as skipped, not passed.

A GPU test that cannot reach a device has exactly one honest thing to say, and
Catch2 gives it: SKIP(), which ctest surfaces as ***Skipped because
`tools/cmake/PulpCatch.cmake` sets SKIP_RETURN_CODE 4 on every discovered case.

The three shapes this rejects all report the opposite. SUCCEED() records a
passing assertion, so a case that found no adapter reports green having verified
nothing. WARN() emits a message and still leaves the case passing. A bare
`return;` is the quietest of the three: Catch2 records the case as passed with
no output at all.

The failure mode is not that a test breaks. It is that the suite's pass count
stays identical whether the GPU lane ran or the adapter vanished, so the day the
hardware goes away nothing changes colour. `test/cmake/gpu_audio_web_tests.cmake`
already states this rule in prose; it was written once and then ignored across
eleven files, which is why it is checked mechanically here instead.

Escape a deliberate exception with an inline `gpu-skip-lint: allow <reason>`
comment on the offending line or the line above it.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SCAN_GLOBS = ("test/test_*gpu*.cpp", "test/test_skia_*.cpp")

ESCAPE = re.compile(r"gpu-skip-lint:\s*allow\s+\S")

# Reporting macros that leave a case passing. A bare `return;` guarded by an
# availability test is handled separately below.
PASSING_REPORT = re.compile(r"\b(SUCCEED|WARN)\s*\(")

# One-line guard: `if (<cond>) return;`
IF_RETURN = re.compile(r"\bif\s*\((?P<cond>.*)\)\s*return\s*;")

# A call that asks whether a device/context is usable.
AVAIL_CALL = re.compile(
    r"(initialize_standalone|initialize_from_surface|is_available|gpu_available"
    r"|create_dawn|->\s*initialize\s*\(|\.\s*(available|ready|valid)\s*\(\s*\))"
)
# A handle that is null exactly when the device/context could not be made.
AVAIL_HANDLE = re.compile(
    r"^!\s*(gpu|gpu_surface|skia|surface|compute|device|adapter)\b"
)


def is_availability_guard(cond: str) -> bool:
    """True when any NEGATED term of `cond` tests device availability.

    Splitting on the boolean operators is what keeps `if (node.gpu_available())`
    -- which skips when the GPU IS present, the opposite situation -- out of the
    finding. Only a negated term counts.
    """
    for term in re.split(r"\|\||&&", cond):
        term = term.strip()
        if not term.startswith("!"):
            continue
        if AVAIL_HANDLE.search(term) or AVAIL_CALL.search(term):
            return True
    return False


def check_file(path: pathlib.Path) -> tuple[list[str], int]:
    """Return (violations, guard_population) for one file."""
    lines = path.read_text().split("\n")
    problems: list[str] = []
    population = 0

    for i, line in enumerate(lines):
        code = line.split("//", 1)[0]
        escaped = ESCAPE.search(line) or (i > 0 and ESCAPE.search(lines[i - 1]))

        if "SKIP(" in code:
            population += 1

        m = PASSING_REPORT.search(code)
        if m:
            population += 1
            if not escaped:
                problems.append(
                    f"{path}:{i + 1}: {m.group(1)}() reports a pass for a "
                    f"condition the case could not verify -- use SKIP()"
                )
            continue

        r = IF_RETURN.search(code)
        if r and is_availability_guard(r.group("cond")):
            population += 1
            if not escaped:
                problems.append(
                    f"{path}:{i + 1}: a bare `return;` on an availability guard "
                    f"records the case as passed -- use SKIP()"
                )

    return problems, population


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--file", action="append", default=[])
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    if args.file:
        files = [pathlib.Path(f) for f in args.file]
    else:
        files = sorted({p for g in SCAN_GLOBS for p in root.glob(g)})

    problems: list[str] = []
    population = 0
    for f in files:
        p, n = check_file(f)
        problems += p
        population += n

    # A lint that matched nothing would exit 0 and prove nothing. These files
    # are full of availability guards; a zero population means the scan stopped
    # reaching them, which must fail rather than read as clean.
    if not files:
        problems.append(
            f"no files matched {SCAN_GLOBS} under {root} -- the scan set is "
            f"empty, so a clean result here would be meaningless"
        )
    elif population == 0:
        problems.append(
            f"scanned {len(files)} files and found no availability guards at "
            f"all -- the patterns no longer match, so a clean result here "
            f"would be meaningless"
        )

    if problems:
        for p in problems:
            print(p, file=sys.stderr)
        print(
            f"\n{len(problems)} problem(s). An unmet GPU precondition must be "
            f"reported with Catch2's SKIP(), not as a pass.",
            file=sys.stderr,
        )
        return 1

    print(
        f"gpu-skip-not-pass: ok ({len(files)} files, {population} guards)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
