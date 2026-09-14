#!/usr/bin/env python3
"""A `catch_discover_tests` TIMEOUT must be a scaled variable, not a literal.

`pulp_scaled_test_timeout` widens a per-test budget on the instrumented lanes,
where the same work takes several times longer than on a plain Release build. A
budget written as a bare integer inside a `catch_discover_tests(...)` block
bypasses the scaler entirely: it is the same number on every lane, so the test
is killed on ASan/TSan/UBSan/coverage at a budget that was only ever sized for
an uninstrumented run.

The failure is quiet in the way that matters. Nothing about a literal is wrong
on the lane it was measured on, and the sanitizer lane reports `***Timeout`
rather than an assertion, so it reads as a slow machine rather than as a budget
that was never scaled. Worse, a literal usually sits beside a scaled sibling
registration of the SAME binary in the SAME file, so the file looks converted.

Scope is deliberately the `catch_discover_tests` blocks, not every `TIMEOUT` in
the test manifests. A raw `set_tests_properties(... TIMEOUT n)` is also
unscaled, but those are the majority of the declarations and converting them is
a separate decision per site; this guard freezes the shape that already has a
repo-wide idiom so it cannot regress.

Escape a deliberate exception with an inline
`catch-discover-timeout-guard: skip <reason>` comment on the same line.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CALL = re.compile(r"\bcatch_discover_tests\s*\(")

# A literal budget: TIMEOUT followed by a bare integer. A scaled one reads
# TIMEOUT "${_pulp_..._timeout}" and does not match.
LITERAL_TIMEOUT = re.compile(r"\bTIMEOUT\s+([0-9]+)\b")

SKIP_MARKER = "catch-discover-timeout-guard: skip"

HELPER = "pulp_scaled_test_timeout"

_FIX = f"""
Route the budget through the scaler instead of writing it as a literal:

    {HELPER}(_pulp_<name>_timeout <seconds>)
    catch_discover_tests(<target>
        PROPERTIES TIMEOUT "${{_pulp_<name>_timeout}}")

Prior art: test/cmake/dsp_rt_contract_tests.cmake (modal-bank bench) and
test/cmake/app_audio_host_tests.cmake (modal analysis).
""".strip()


def _strip_comment(line: str) -> str:
    """Drop a trailing CMake `#` comment, ignoring `#` inside a quoted string."""
    out = []
    quoted = False
    prev = ""
    for ch in line:
        if ch == '"' and prev != "\\":
            quoted = not quoted
        elif ch == "#" and not quoted:
            break
        out.append(ch)
        prev = ch
    return "".join(out)


def blocks(text: str):
    """Yield (start_line, body) for every catch_discover_tests(...) call.

    Paren-matched rather than line-matched: the registrations span several
    lines, and a line-scoped scan cannot see a TIMEOUT on a continuation line.
    """
    for call in CALL.finditer(text):
        depth = 1
        i = call.end()
        quoted = False
        while i < len(text) and depth:
            ch = text[i]
            if ch == '"' and text[i - 1] != "\\":
                quoted = not quoted
            elif not quoted:
                if ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
            i += 1
        if depth:
            continue  # unbalanced: leave it to CMake to reject
        yield text.count("\n", 0, call.start()) + 1, text[call.end() : i - 1]


def scan(root: Path):
    """Return (violations, blocks_parsed) over the test manifests."""
    paths = sorted((root / "test" / "cmake").glob("*.cmake"))
    top = root / "test" / "CMakeLists.txt"
    if top.is_file():
        paths.append(top)

    found = []
    parsed = 0
    for path in paths:
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        for start, body in blocks(text):
            parsed += 1
            offset = 0
            for raw in body.splitlines():
                line = _strip_comment(raw)
                for hit in LITERAL_TIMEOUT.finditer(line):
                    lineno = start + offset
                    if SKIP_MARKER in lines[lineno - 1]:
                        continue
                    found.append(
                        (path.relative_to(root), lineno, hit.group(1), raw.strip())
                    )
                offset += 1
    return found, parsed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root to scan (default: this checkout)",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    found, parsed = scan(root)

    # A zero finding is only meaningful against a non-zero control. If nothing
    # parsed, the scan missed the manifests (wrong root, moved directory) and
    # an empty result is an instrument failure wearing the costume of a pass.
    if not parsed:
        print(
            f"catch-discover-timeout-guard: parsed 0 catch_discover_tests "
            f"block(s) under {root}/test — the scan found no manifests to "
            f"read, so a clean result here would prove nothing.",
            file=sys.stderr,
        )
        return 1

    if not found:
        print(
            f"catch-discover-timeout-guard: ok — 0 literal TIMEOUT(s) across "
            f"{parsed} catch_discover_tests block(s)."
        )
        return 0

    print(
        f"{len(found)} literal TIMEOUT(s) inside catch_discover_tests "
        f"(of {parsed} block(s) parsed). A literal is not scaled on the "
        f"instrumented lanes, so the test is killed at an uninstrumented "
        f"budget:",
        file=sys.stderr,
    )
    for path, lineno, seconds, raw in found:
        print(f"  {path}:{lineno}: TIMEOUT {seconds}  ({raw})", file=sys.stderr)
    print("", file=sys.stderr)
    print(_FIX, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
