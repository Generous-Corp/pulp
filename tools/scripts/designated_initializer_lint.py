#!/usr/bin/env python3
"""designated_initializer_lint.py — catch MSVC-only designated-initializer breaks.

C++20 requires the designators in an aggregate initializer to appear in member
declaration order, and forbids naming the same member twice. Clang and GCC are
lenient about both in practice; **MSVC is not**, and rejects them outright:

    error C7560: 'custom_latency_for': designators must appear in member
    declaration order of class 'pulp::host::ExecutorSnapshotBinders'

That asymmetry is the whole problem. Pulp's required CI gate is macOS, so a bad
merge that duplicates a designated-initializer block compiles clean everywhere a
contributor and every blocking gate will look, and breaks only on Windows —
where the failure surfaces as an unrelated-looking library build error, hours
later, to whoever next tries to build there. `core/host/src/signal_graph.cpp`
carried exactly that: a byte-identical `.custom_latency_for = ...` block
duplicated at the end of an initializer by a merge resolution, which made
`pulp-host` (and therefore `pulp-view`, and therefore every Windows plug-in)
un-buildable on MSVC while main stayed green.

This lint finds the duplicate-designator half of the rule, which is the half a
merge actually produces and the half that can be checked without a compiler:
within a single brace-balanced initializer body, no designator may appear twice
at the same nesting depth.

It deliberately does NOT try to verify declaration ORDER. Doing that correctly
needs the struct's definition, which may live in another header, behind macros,
or in a template — a regex approximation would produce false positives on
exactly the code people care about most. Order violations that are not also
duplicates are left to the Windows build.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error.

Bypass: put `// designated-init-lint: skip` on the line of the second
occurrence, for the rare case where two same-named designators genuinely belong
to different nested aggregates the depth heuristic cannot tell apart.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".h", ".hxx", ".mm"}

# A designator at the start of a line: `.name =`.
#
# Two exclusions matter, both found by running this over the tree:
#   * anchoring to line start keeps it off member ACCESS like `a.b = c;`, which
#     is written mid-expression rather than as a line's first token;
#   * `=(?!=)` keeps it off a wrapped COMPARISON like
#         REQUIRE(f(...)
#                     .status == Status::Allowed);
#     where `.status ==` begins the line and a bare `=` would match its first
#     character. That pattern is common in this repo's Catch2 tests and was the
#     lint's entire initial false-positive set.
#
# Line-start anchoring alone is still not enough, because a long member
# ASSIGNMENT wraps into the same shape:
#     std::get<Block>(changed.voice[6].parameters)
#         .stopband_attenuation_db = 72.0f;
# So a match must also be PRECEDED by `{` or `,` — the only two tokens an
# aggregate element can follow. A wrapped assignment follows `)`, an
# identifier, or `]`, and is rejected.
DESIGNATOR = re.compile(r"^\s*\.([A-Za-z_]\w*)\s*=(?!=)")
ELEMENT_PRECEDERS = ("{", ",")
SKIP_MARKER = "designated-init-lint: skip"


def _strip_noise(line: str) -> str:
    """Blank out line comments and string literals so braces inside them do not
    move the depth counter."""
    out = []
    i = 0
    in_str = None
    while i < len(line):
        ch = line[i]
        if in_str:
            if ch == "\\":
                i += 2
                continue
            if ch == in_str:
                in_str = None
            out.append(" ")
        elif ch in "\"'":
            in_str = ch
            out.append(" ")
        elif ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
            break
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def scan_text(text: str, path: str) -> list[str]:
    """Return one message per duplicated designator."""
    findings: list[str] = []
    depth = 0
    # depth -> {designator: first line number}
    seen: dict[int, dict[str, int]] = {}
    prev_code = ""  # last non-blank line with comments/strings stripped

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = _strip_noise(raw)

        match = DESIGNATOR.match(line)
        if match and prev_code.rstrip().endswith(ELEMENT_PRECEDERS):
            name = match.group(1)
            bucket = seen.setdefault(depth, {})
            if name in bucket and SKIP_MARKER not in raw:
                findings.append(
                    f"{path}:{lineno}: designator '.{name}' initialized twice in "
                    f"the same aggregate (first at line {bucket[name]}). MSVC "
                    f"rejects this with C7560; Clang accepts it, so this only "
                    f"breaks the Windows build."
                )
            else:
                bucket.setdefault(name, lineno)

        for ch in line:
            if ch == "{":
                depth += 1
            elif ch == "}":
                # Leaving a scope retires everything recorded inside it, so two
                # sibling aggregates may each name the same member.
                seen.pop(depth, None)
                depth = max(0, depth - 1)

        if line.strip():
            prev_code = line

    return findings


def _changed_files(base: str) -> list[pathlib.Path]:
    result = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", base, "HEAD"],
        capture_output=True, text=True, check=True)
    return [pathlib.Path(p) for p in result.stdout.split("\n") if p]


def _tree_files() -> list[pathlib.Path]:
    result = subprocess.run(["git", "ls-files"], capture_output=True,
                            text=True, check=True)
    return [pathlib.Path(p) for p in result.stdout.split("\n") if p]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("tree", "changed"), default="tree")
    parser.add_argument("--base", default="origin/main",
                        help="base ref for --mode=changed")
    args = parser.parse_args()

    try:
        paths = (_changed_files(args.base) if args.mode == "changed"
                 else _tree_files())
    except subprocess.CalledProcessError as exc:
        print(f"designated_initializer_lint: git failed: {exc}", file=sys.stderr)
        return 2

    findings: list[str] = []
    for path in paths:
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        # external/ is vendored; we do not own its style and MSVC breaks there
        # are the vendor's to fix.
        if str(path).startswith("external/"):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue  # deleted between the diff and now
        findings.extend(scan_text(text, str(path)))

    if findings:
        print("designated_initializer_lint: FAIL")
        for f in findings:
            print(f"  {f}")
        return 1

    print(f"designated_initializer_lint: ok ({len(paths)} path(s) considered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
