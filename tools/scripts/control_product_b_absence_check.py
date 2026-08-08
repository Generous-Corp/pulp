#!/usr/bin/env python3
"""Fail closed if the frozen Product B transport leaks into shipped surfaces.

The capability-control broker is Product A only. Product B collaboration is an
explicit NO-GO, so its provisional namespace and route vocabulary must not land
accidentally in public source, public documentation, or a shipped binary.
Only the accepted NO-GO policy may name Product B, and that exception is checked.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re


# Keep this set narrow and frozen: it catches provisional product/route names
# without treating ordinary host terminology as a leak.
FORBIDDEN_MARKERS = {
    "Product B": re.compile(r"\bProduct B\b", re.IGNORECASE),
    # Camel/snake/kebab markers deliberately accept a namespace-style suffix
    # while rejecting unrelated ProductBundle vocabulary.
    "ProductB": re.compile(r"\bProductB(?:[A-Z]\w*)?\b"),
    "product_b": re.compile(r"\bproduct_b(?:_[a-z]\w*)?\b"),
    "product-b": re.compile(r"\bproduct-b(?:-[a-z]\w*)?\b"),
    "cross-vendor route": re.compile(r"\bcross-vendor route\b", re.IGNORECASE),
    "cross-vendor support": re.compile(r"\bcross-vendor support\b", re.IGNORECASE),
}

SOURCE_ROOTS = (
    Path("apple"),
    Path("android"),
    Path("bindings"),
    Path("core"),
    Path("cmake"),
    Path("packages"),
    Path("scripts"),
    Path("templates"),
    Path("inspect"),
    Path("ship"),
    Path("examples"),
    Path("tools"),
    Path("experimental"),
)
TEXT_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm", ".rs", ".swift", ".kt", ".kts", ".java", ".md", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".py", ".sh", ".cmake", ".json", ".toml", ".xml", ".plist", ".html", ".css", ".template", ".in",
})
SOURCE_EXCLUSIONS = frozenset({
    Path("tools/check-docs.sh"),
    Path("tools/scripts/control_product_b_absence_check.py"),
    Path("tools/scripts/test_control_product_b_absence_check.py"),
    Path("experimental/pulp-rs/CMakeLists.txt"),
})
ROOT_SOURCE_FILES = (Path("CMakeLists.txt"),)

# This policy is the only durable location that may name the frozen Product B
# scope. Do not add a broad docs exemption: a public claim would then pass.
NO_GO_ALLOWLIST = {
    Path("docs/policies/plugin-collaboration.md"): {
        "Product B": "Product B remains unshipped under this NO-GO; this policy is not an API or\nproduct commitment.",
    },
}


def _allowlisted_hits(relative: Path, marker: str, text: str) -> tuple[set[int], list[str]]:
    """Return byte offsets of the exact policy text allowed to name a marker."""
    expected = NO_GO_ALLOWLIST.get(relative, {}).get(marker)
    if expected is None:
        return set(), []
    if text.count(expected) != 1:
        return set(), [f"{relative}: required explicit NO-GO policy text for {marker!r} is missing or duplicated"]
    start = text.index(expected)
    return set(range(start, start + len(expected))), []


def _required_allowlist_errors(root: Path) -> list[str]:
    errors: list[str] = []
    for relative, markers in NO_GO_ALLOWLIST.items():
        path = root / relative
        if not path.is_file():
            errors.append(f"required explicit NO-GO policy is missing: {relative}")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for marker, expected in markers.items():
            if text.count(expected) != 1:
                errors.append(f"{relative}: required explicit NO-GO policy text for {marker!r} is missing or duplicated")
    return errors


def _text_errors(root: Path) -> list[str]:
    errors = _required_allowlist_errors(root)
    paths: set[Path] = set()
    paths.update(root / relative for relative in ROOT_SOURCE_FILES)
    for relative_root in SOURCE_ROOTS:
        directory = root / relative_root
        if directory.is_dir():
            paths.update(path for path in directory.rglob("*")
                         if path.suffix in TEXT_SUFFIXES or path.name == "CMakeLists.txt")
    docs = root / "docs"
    if docs.is_dir():
        paths.update(docs.rglob("*.md"))

    for path in sorted(paths):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative in SOURCE_EXCLUSIONS:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for marker, pattern in FORBIDDEN_MARKERS.items():
            allowed_offsets, allow_errors = _allowlisted_hits(relative, marker, text)
            # _required_allowlist_errors reports malformed allowances once.
            if allow_errors:
                allowed_offsets = set()
            for match in pattern.finditer(text):
                if match.start() in allowed_offsets:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                errors.append(f"{relative}:{line}: forbidden Product B marker {marker!r}")
    return errors


def _binary_errors(paths: tuple[Path, ...]) -> list[str]:
    errors: list[str] = []
    for path in paths:
        if not path.is_file():
            errors.append(f"binary does not exist: {path}")
            continue
        data = path.read_bytes()
        for marker, pattern in FORBIDDEN_MARKERS.items():
            if pattern.search(data.decode("latin-1")):
                errors.append(f"{path}: forbidden Product B marker {marker!r} in binary")
    return errors


def verify(root: Path, binaries: tuple[Path, ...] = ()) -> list[str]:
    return _text_errors(root) + _binary_errors(binaries)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--binary", type=Path, action="append", default=[],
                        help="compiled or installed artifact that must not retain Product B markers")
    args = parser.parse_args()
    errors = verify(args.root.resolve(), tuple(path.resolve() for path in args.binary))
    if errors:
        print("product_b_absence_verified=false")
        print("\n".join(errors))
        return 1
    print("product_b_absence_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
