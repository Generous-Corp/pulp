#!/usr/bin/env python3
"""shipping_macro_guard.py: keep test-only compile macros out of shipped builds.

Some sources compile a test-only surface behind a macro that only a test target
defines. ``PULP_AU_V2_TEST_HOOKS`` guards ``+pulpEditorHostForTesting:`` in
``core/format/src/au_v2_cocoa_view.mm``; Objective-C dispatches by name at
runtime, so if any shipping path defined the macro, every AU v2 plug-in would
carry a method that hands out a raw ``PluginViewHost*`` to code in the host
process. ``pulp-test-au-v2-cocoa-shipping`` checks a build that sets its own
definitions, so it cannot see a shipping path (``pulp_add_plugin``, an install
rule, a workflow flag) that starts defining the macro. This guard can.

Outside ``test/``, a line that names a guarded macro may only be a preprocessor
guard on it (``#if defined(M)``, ``#ifdef M``, ``#endif // M``) or a comment.
Anything else (``#define M``, ``-DM``, ``M=1`` in a CMake definition list) is a
violation. ``test/`` is exempt: the test targets that define the macro live
there.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error (including no guard
found for a macro at all, which means the scan looked in the wrong place).
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

# Macro -> the source file that must still guard it (the instrument check).
GUARDED_MACROS: dict[str, str] = {
    "PULP_AU_V2_TEST_HOOKS": "core/format/src/au_v2_cocoa_view.mm",
}

SKIP_DIRS = {".git", "test", "docs", "external", "node_modules", "planning", "_deps",
             "__pycache__"}
SKIP_SUFFIXES = {".md", ".png", ".jpg", ".jpeg", ".gif", ".wav", ".ttf", ".otf", ".zip", ".a",
                 ".dylib", ".so", ".bin", ".pdf", ".ico", ".icns", ".woff", ".woff2", ".pyc"}
SELF = {"tools/scripts/shipping_macro_guard.py", "tools/scripts/test_shipping_macro_guard.py"}


C_FAMILY = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".m", ".mm"}


def _allowed(line: str, macro: str, suffix: str) -> bool:
    stripped = line.strip()
    if suffix in C_FAMILY:
        guard = re.compile(r"^#\s*(?:if\s+(?:!\s*)?defined\s*\(\s*%s\s*\)|ifn?def\s+%s\b|endif\b)"
                           % (macro, macro))
        # A preprocessor guard on the macro, or a comment. "# define M" is
        # neither: the preprocessor allows spaces after the '#'.
        return bool(guard.match(stripped)) or stripped.startswith(("//", "/*", "*"))
    # CMake, shell, YAML, Python and the like: only a comment may name it.
    return stripped.startswith("#")


def scan(root: pathlib.Path) -> tuple[list[str], list[str]]:
    violations: list[str] = []
    errors: list[str] = []
    guards = {macro: 0 for macro in GUARDED_MACROS}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in SKIP_DIRS and not d.startswith("build")]
        for name in filenames:
            path = pathlib.Path(dirpath) / name
            rel = path.relative_to(root).as_posix()
            # Symlinks point at trees scanned on their own (or at nothing).
            if rel in SELF or path.is_symlink() or path.suffix.lower() in SKIP_SUFFIXES:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError as exc:
                errors.append(f"{rel}: unreadable ({exc})")
                continue
            for macro in GUARDED_MACROS:
                if macro not in text:
                    continue
                for number, line in enumerate(text.splitlines(), 1):
                    if macro not in line:
                        continue
                    if _allowed(line, macro, path.suffix.lower()):
                        if rel == GUARDED_MACROS[macro] and line.lstrip().startswith("#"):
                            guards[macro] += 1
                        continue
                    violations.append(f"{rel}:{number}: {macro} outside a guard: {line.strip()}")
    for macro, count in guards.items():
        if count == 0:
            errors.append(f"{macro}: no preprocessor guard found in {GUARDED_MACROS[macro]}; "
                          "the scan looked in the wrong place or the guard moved")
    return violations, errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    violations, errors = scan(root)
    for e in errors:
        print(f"shipping-macro-guard: error: {e}", file=sys.stderr)
    for v in violations:
        print(f"shipping-macro-guard: {v}", file=sys.stderr)
    if errors:
        return 2
    if violations:
        print("shipping-macro-guard: a test-only macro is defined outside test/; only a test "
              "target may define it (see the module docstring)", file=sys.stderr)
        return 1
    print(f"shipping-macro-guard: ok ({', '.join(GUARDED_MACROS)} guarded, defined only in test/)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
