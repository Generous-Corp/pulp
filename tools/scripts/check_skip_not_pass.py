#!/usr/bin/env python3
"""Enforce that an unmet precondition is reported as skipped, not passed.

A test that cannot reach the thing it measures has exactly one honest thing to
say, and Catch2 gives it: SKIP(), which ctest surfaces as ***Skipped because
`tools/cmake/PulpCatch.cmake` sets SKIP_RETURN_CODE 4 on every discovered case.

The three shapes this rejects all report the opposite. SUCCEED() records a
passing assertion, so a case that found no adapter reports green having verified
nothing. WARN() emits a message and still leaves the case passing. A bare
`return;` is the quietest of the three: Catch2 records the case as passed with
no output at all.

The failure mode is not that a test breaks. It is that the suite's pass count
stays identical whether the lane ran or the precondition vanished, so the day
the hardware, the SDK or the built binary goes away nothing changes colour.

Which SUCCEED is a defect is a question about the MESSAGE, not the syntax. A
case ending `SUCCEED("no crash across repeated attach/detach")` measured a real
outcome and is correct. One ending `SUCCEED("skipped: pulp not built")` measured
nothing. So the finding is gated on precondition vocabulary (SKIP_VOCAB), with
an override for messages that state a positive result (INFO_VOCAB). Without that
split this rule reports every informational assertion in the tree and becomes a
false-positive generator nobody can land.

Escape a deliberate exception with an inline `skip-lint: allow <reason>` comment
on the offending line or the line above it. `gpu-skip-lint: allow` is the older
spelling and still works. A whole file whose conversion needs per-case judgement
is frozen at its current count in check_skip_not_pass.json instead.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

SCAN_GLOBS = (
    "test/test_*.cpp",
    "test/test_*.mm",
    "test/support/*.hpp",
    "test/*/*.cpp",
    "test/*/*.mm",
    "examples/*/test_*.cpp",
    "forge-seam/test/test_*.cpp",
    "tools/*/test_*.cpp",
)

# Only a Catch2 translation unit can be judged by this rule. `WARN(...)` is also
# a printf-style logging macro in non-test Pulp source (examples/forge-modular,
# tools/rack), and reading those calls as Catch2 assertions would report a
# defect in code that has no test cases at all.
CATCH_MARKER = re.compile(r"catch2/|CATCH_CONFIG|\bTEST_CASE\s*\(")

LEDGER_FILE = pathlib.Path(__file__).with_name("check_skip_not_pass.json")

ESCAPE = re.compile(r"(?:gpu-)?skip-lint:\s*allow\s+\S")

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

# The message says the case could not reach what it measures.
SKIP_VOCAB = re.compile(
    r"""
      \bskip(?:ped|ping)?\b
    | \bnot\s+(?:built|available|linked|compiled|installed|present|registered
               |resolvable|locatable|supported|active|enabled|reachable|running
               |set|a\s+\w+\s+host)\b
    | \bun(?:available|supported|installed|reachable)\b
    | \bmissing\b | \babsent\b
    | \bcannot\b | \bcould\s+not\b | \bcan(?:no|')t\b
    | \bno\s+\w+(?:\s+\w+)?\s+(?:present|available|found|registered|resolvable
                                |linked|detected|on\s+PATH)\b
    | \bno\s+(?:gpu|adapter|device|display|window\s+server|session\s+bus
               |typeface|emoji|backend)\b
    | \b(?:mac\s?os|linux|posix|windows|apple|android|ios|unix)-only\b
    | \bnon-(?:windows|apple|linux|macos)\b
    | \bnot\s+(?:macos|linux|windows|apple|a\s+linux|a\s+windows)\b
    | \brequires?\s+(?:macos|linux|windows|a\s|an\s|the\s|\w+\s+\+)
    | \bgated\s+to\b
    | \b(?:disabled|off|omitted)\s+in\s+(?:this|the)\s+build\b
    | \bintentionally\s+un(?:supported|available)\b
    | \bthis\s+(?:platform|host|build|environment|target)\b
    """,
    re.I | re.X,
)

# The message states a positive result the case actually observed. Wins over
# SKIP_VOCAB: "no crash", "returned cleanly" and friends are real findings even
# though they contain words the skip vocabulary also matches.
INFO_VOCAB = re.compile(
    r"""
      \bno\s+(?:crash|deadlock|leak|uaf|hang|abort|assert)\b
    | \breturned\s+cleanly\b
    | \bwithout\s+(?:uaf|crashing|a\s+crash|deadlock|races)\b
    | \bcompleted\b
    | \bare\s+(?:stable|ignored)\b
    | \btolerates?\b
    | \bis\s+the\s+only\s+state\b
    | \bdid\s+not\s+crash\b
    | \bfail(?:s|ed)?\s+(?:closed|before|cleanly)\b
    | \bare\s+rejected\b
    | \bis\s+covered\b
    """,
    re.I | re.X,
)


def message_of(text: str, start: int) -> str:
    """Concatenate the string literals of the macro call beginning at `start`."""
    depth = 0
    i = text.index("(", start)
    j = i
    while j < len(text):
        c = text[j]
        if c == '"':
            j += 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                break
        j += 1
    return " ".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text[i : j + 1]))


def reports_unmet_precondition(message: str) -> bool:
    if not message:
        return False
    if INFO_VOCAB.search(message):
        return False
    return bool(SKIP_VOCAB.search(message))


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
    text = path.read_text(errors="replace")
    if not CATCH_MARKER.search(text):
        return [], 0
    lines = text.split("\n")
    offsets = []
    pos = 0
    for line in lines:
        offsets.append(pos)
        pos += len(line) + 1

    problems: list[str] = []
    population = 0

    for i, line in enumerate(lines):
        code = line.split("//", 1)[0]
        stripped = line.lstrip()
        if stripped.startswith(("///", "//", "*", "/*")):
            continue
        escaped = ESCAPE.search(line) or (i > 0 and ESCAPE.search(lines[i - 1]))

        if "SKIP(" in code:
            population += 1

        m = PASSING_REPORT.search(code)
        if m:
            message = message_of(text, offsets[i] + m.start())
            if reports_unmet_precondition(message):
                population += 1
                if not escaped:
                    problems.append(
                        f"{path}:{i + 1}: {m.group(1)}() reports a pass for a "
                        f"precondition the case could not meet -- use SKIP()"
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


def load_ledger(path: pathlib.Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text())
    return {e["file"]: int(e["sites"]) for e in data.get("allow", [])}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".")
    ap.add_argument("--file", action="append", default=[])
    ap.add_argument("--ledger", default=str(LEDGER_FILE))
    ap.add_argument("--no-ledger", action="store_true")
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    if args.file:
        files = [pathlib.Path(f) for f in args.file]
    else:
        files = sorted({p for g in SCAN_GLOBS for p in root.glob(g)})

    # An explicit --file list is a spot check, not a census: the ledger's
    # stale-entry rule can only be evaluated against a full scan, so a targeted
    # run would otherwise report all 67 frozen files as unscanned.
    full_scan = not args.file
    ledger = {} if (args.no_ledger or not full_scan) else load_ledger(
        pathlib.Path(args.ledger)
    )

    problems: list[str] = []
    population = 0
    counts: dict[str, int] = {}
    for f in files:
        p, n = check_file(f)
        population += n
        key = f.as_posix()
        if key.startswith("./"):
            key = key[2:]
        counts[key] = len(p)
        if key in ledger:
            continue
        problems += p

    # The ledger freezes a reviewed backlog. It must not rot: an entry whose
    # file no longer carries that many findings is an error, not a courtesy,
    # because a stale exemption silently re-covers the next regression.
    ledger_problems: list[str] = []
    scanned = set(counts)
    for key, frozen in sorted(ledger.items()):
        if key not in scanned:
            ledger_problems.append(
                f"{key}: listed in the ledger but not scanned -- delete the entry"
            )
        elif counts[key] > frozen:
            ledger_problems.append(
                f"{key}: {counts[key]} findings, frozen at {frozen} -- the "
                f"backlog may shrink but never grow; use SKIP() in the new case"
            )
        elif counts[key] < frozen:
            ledger_problems.append(
                f"{key}: {counts[key]} findings, ledger still says {frozen} -- "
                f"lower it to {counts[key]} (or drop the entry at 0) in this change"
            )

    # A lint that matched nothing would exit 0 and prove nothing.
    if not files:
        problems.append(
            f"no files matched {SCAN_GLOBS} under {root} -- the scan set is "
            f"empty, so a clean result here would be meaningless"
        )
    elif population == 0:
        problems.append(
            f"scanned {len(files)} files and found no precondition guards at "
            f"all -- the patterns no longer match, so a clean result here "
            f"would be meaningless"
        )

    if problems or ledger_problems:
        for p in problems:
            print(p, file=sys.stderr)
        for p in ledger_problems:
            print(p, file=sys.stderr)
        print(
            f"\n{len(problems) + len(ledger_problems)} problem(s). An unmet "
            f"precondition must be reported with Catch2's SKIP(), not as a pass.",
            file=sys.stderr,
        )
        return 1

    frozen_total = sum(ledger.values())
    print(
        f"skip-not-pass: ok ({len(files)} files, {population} guards, "
        f"{len(ledger)} file(s) frozen at {frozen_total} finding(s))"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
