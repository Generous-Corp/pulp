#!/usr/bin/env python3
"""Reject test code that builds a file path from __FILE__.

__FILE__ is whatever path the compiler was handed, after any rewriting. With
ccache's `base_dir` set (tools/ci/bootstrap-macos-host.sh writes it on the
self-hosted Macs), ccache rewrites absolute paths under that directory to paths
relative to the compiler's working directory, and Ninja compiles from the build
tree. A test then sees __FILE__ as `../test/test_x.cpp`, which resolves from
the build directory and from nowhere else: run from `build/test` its fixture
path points at a file that does not exist. Hosts without `base_dir`, including
GitHub's runners, keep the absolute path, so the defect only appears on the
lanes that set it.

The checkout root comes from a compile-time definition instead:
pulp_test_data() defines PULP_SOURCE_DIR for a declaring source, and
test/support/fixture_root.hpp turns it into a checked path. A definition's
value is a string the compiler never rewrites.

Using __FILE__ as a label (a log line, a registry entry, a provenance field)
is fine; this rejects only the forms that treat it as a path: passing it to a
path, string, stream or file-open constructor, assigning it to a path
variable, concatenating a literal onto it, or navigating it on the same line
(`parent_path`, `rfind`, `substr` and the like). Escape a deliberate exception
with an inline `file-path-lint: allow <reason>` comment on the line.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

SCAN_ROOT = "test"
SUFFIXES = (".cpp", ".cc", ".hpp", ".h", ".mm", ".inl")

ESCAPE = re.compile(r"file-path-lint:\s*allow\s+\S")

PATH_FORMS = (
    # path(__FILE__), std::filesystem::path{__FILE__}, std::string(__FILE__),
    # std::ifstream(__FILE__), fopen(__FILE__, ...), u8path(__FILE__)
    re.compile(
        r"\b(?:path|u8path|string|string_view|ifstream|ofstream|fstream|fopen|open)"
        r"\s*[({]\s*__FILE__\b"
    ),
    # fs::path here = __FILE__;  std::filesystem::path here{__FILE__};
    re.compile(r"\bpath\s+\w+\s*(?:=|\{)\s*__FILE__\b"),
    # __FILE__ "/../fixtures/x"  (literal concatenation)
    re.compile(r"\b__FILE__\s*\""),
)
NAVIGATION = re.compile(
    r"\b(?:parent_path|remove_filename|replace_filename|rfind|find_last_of|substr)\b"
)
FILE_MACRO = re.compile(r"\b__FILE__\b")


def findings(text: str) -> list[tuple[int, str]]:
    out = []
    for number, line in enumerate(text.splitlines(), start=1):
        code = line.split("//", 1)[0]
        if not FILE_MACRO.search(code) or ESCAPE.search(line):
            continue
        if any(form.search(code) for form in PATH_FORMS) or NAVIGATION.search(code):
            out.append((number, line.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".")
    ap.add_argument("--file", action="append", default=[],
                    help="scan only these files (relative to --root)")
    args = ap.parse_args()
    root = pathlib.Path(args.root)

    if args.file:
        files = [root / name for name in args.file]
    else:
        files = sorted(p for p in (root / SCAN_ROOT).rglob("*")
                       if p.suffix in SUFFIXES and p.is_file())
    if not files:
        print(f"check_test_file_paths: no sources found under {root / SCAN_ROOT} -- "
              "the scan matched nothing, so it proves nothing", file=sys.stderr)
        return 2

    bad = 0
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print(f"check_test_file_paths: cannot read {path}: {error}", file=sys.stderr)
            return 2
        for number, line in findings(text):
            bad += 1
            print(f"{path.relative_to(root)}:{number}: __FILE__ used as a path: {line}")
    if bad:
        print(f"\n{bad} finding(s). Build the path from pulp_test::fixture_root() "
              "(test/support/fixture_root.hpp) and declare the data with pulp_test_data().",
              file=sys.stderr)
        return 1
    print(f"check_test_file_paths: {len(files)} files, no __FILE__ path use")
    return 0


if __name__ == "__main__":
    sys.exit(main())
