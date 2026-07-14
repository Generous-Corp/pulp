#!/usr/bin/env python3
"""Enforce US-English spelling across Pulp's own source.

House style: Pulp is written in American English — identifiers AND prose. This is
a consistency rule, nothing more. A codebase with one spelling per word is
greppable and guessable; a mix of `color`/`colour` or `normalize`/`normalise`
means every reader has to remember which spelling the author reached for.

The rule applies to Pulp's own source. It does NOT touch third-party or vendored
trees, and it exempts the handful of places where a non-US spelling is part of an
external contract we don't control (a foreign API symbol, a data value quoted from
elsewhere) — those live in EXEMPT_SUBSTRINGS.

Usage:
    us_english_check.py                # report; exit 1 if anything is found
    us_english_check.py --fix          # rewrite in place, then report what's left
    us_english_check.py --selftest     # sanity-check the checker itself
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

SCANNED_ROOTS = ["core", "inspect", "ship", "packages", "experimental", "test",
                 "apple", "tools", "examples"]

SKIP_DIRS = {
    "build", ".git", "external", "node_modules", "__pycache__", "third_party",
    "vendor", ".venv", "dist", "generated",
}

SCANNED_SUFFIXES = {
    ".h", ".hpp", ".c", ".cc", ".cpp", ".mm", ".m", ".swift",
    ".js", ".mjs", ".ts", ".tsx", ".rs", ".py",
    ".md", ".txt", ".cmake", ".json",
}

# British / Canadian → American. Whole-word, case-insensitive, case-preserving.
# Curated: only entries with a single unambiguous American form. Ambiguous cases
# (e.g. "grey"/"gray" are both current in US usage) are deliberately omitted to
# keep the gate from crying wolf.
SPELLINGS = {
    "colour": "color",
    "colours": "colors",
    "coloured": "colored",
    "colourful": "colorful",
    "normalise": "normalize",
    "normalised": "normalized",
    "normalises": "normalizes",
    "normalising": "normalizing",
    "normalisation": "normalization",
    "centre": "center",
    "centred": "centered",
    "centres": "centers",
    "behaviour": "behavior",
    "behaviours": "behaviors",
    "initialise": "initialize",
    "initialised": "initialized",
    "initialises": "initializes",
    "initialising": "initializing",
    "initialisation": "initialization",
    "serialise": "serialize",
    "serialised": "serialized",
    "serialises": "serializes",
    "serialising": "serializing",
    "serialisation": "serialization",
    "deserialise": "deserialize",
    "deserialised": "deserialized",
    "analyse": "analyze",
    "analysed": "analyzed",
    "analyses": "analyzes",
    "analysing": "analyzing",
    "optimise": "optimize",
    "optimised": "optimized",
    "optimises": "optimizes",
    "optimising": "optimizing",
    "optimisation": "optimization",
    "organise": "organize",
    "organised": "organized",
    "organisation": "organization",
    "recognise": "recognize",
    "recognised": "recognized",
    "recognises": "recognizes",
    "synchronise": "synchronize",
    "synchronised": "synchronized",
    "synchronisation": "synchronization",
    "prioritise": "prioritize",
    "prioritised": "prioritized",
    "capitalise": "capitalize",
    "capitalised": "capitalized",
    "customise": "customize",
    "customised": "customized",
    "minimise": "minimize",
    "minimised": "minimized",
    "maximise": "maximize",
    "maximised": "maximized",
    "emphasise": "emphasize",
    "emphasised": "emphasized",
    "utilise": "utilize",
    "utilised": "utilized",
    "modelled": "modeled",
    "modelling": "modeling",
    "labelled": "labeled",
    "labelling": "labeling",
    "signalling": "signaling",
    "catalogue": "catalog",
    "catalogued": "cataloged",
    "artefact": "artifact",
    "artefacts": "artifacts",
    "licence": "license",          # noun; US uses "license" for both
    "licenced": "licensed",
    "defence": "defense",
    "offence": "offense",
    "favour": "favor",
    "favourite": "favorite",
    "neighbour": "neighbor",
    "neighbouring": "neighboring",
    "honour": "honor",
    "flavour": "flavor",
    "metre": "meter",
    "metres": "meters",
    "litre": "liter",
    "fibre": "fiber",
    "calibre": "caliber",
}
# Paths (substring match on the repo-relative path) whose non-US spelling is part
# of an external contract or is otherwise the point. Deleting it would be a bug.
EXEMPT_SUBSTRINGS = [
    # This file is the dictionary; it necessarily contains both spellings.
    "tools/scripts/us_english_check.py",
    # Third-party licence TEXT quoted verbatim — the words are not ours to edit.
    "NOTICE.md",
    "DEPENDENCIES.md",
    "LICENSE",
    "third-party",
    # A migration guide may quote a foreign framework's own symbol spelling.
    "docs/guides/coming-from",
]


def is_exempt(rel: str) -> bool:
    return any(sub in rel for sub in EXEMPT_SUBSTRINGS)


def preserve_case(src: str, repl: str) -> str:
    if src.isupper():
        return repl.upper()
    if src[:1].isupper():
        return repl[:1].upper() + repl[1:]
    return repl


# One alternation, whole-word, case-insensitive.
_PATTERN = re.compile(
    r"\b(" + "|".join(sorted(SPELLINGS, key=len, reverse=True)) + r")\b",
    re.IGNORECASE,
)


def _american(word: str) -> str | None:
    repl = SPELLINGS.get(word.lower())
    return preserve_case(word, repl) if repl else None


def iter_files(roots):
    for root in roots:
        base = REPO / root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SCANNED_SUFFIXES:
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            yield path


def scan(fix: bool):
    hits = []
    for path in iter_files(SCANNED_ROOTS):
        rel = str(path.relative_to(REPO))
        if is_exempt(rel):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if fix:
            new = _PATTERN.sub(lambda m: _american(m.group(0)) or m.group(0), text)
            if new != text:
                path.write_text(new, encoding="utf-8")
            text = new
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in _PATTERN.finditer(line):
                repl = _american(m.group(0))
                if repl and repl != m.group(0):
                    hits.append((rel, lineno, m.group(0), repl, line.strip()[:90]))
    return hits


def selftest() -> int:
    cases = [
        ("Colour", "Color"), ("colour", "color"), ("NORMALISE", "NORMALIZE"),
        ("centre", "center"), ("Behaviour", "Behavior"), ("analyse", "analyze"),
        ("color", None), ("center", None), ("normalize", None),
    ]
    ok = True
    for src, want in cases:
        got = _american(src)
        flag = "ok" if got == want else "FAIL"
        if got != want:
            ok = False
        print(f"[{flag}] {src!r} -> {got!r} (want {want!r})")
    print("us-english selftest:", "all cases pass" if ok else "FAILED")
    return 0 if ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true", help="rewrite in place")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    hits = scan(fix=args.fix)
    if not hits:
        print("us-english: clean (" + ", ".join(SCANNED_ROOTS) + ")")
        return 0

    print("us-english: non-US spellings in Pulp's own source\n")
    for rel, lineno, found, repl, ctx in hits[:200]:
        print(f"  {rel}:{lineno}: {found!r} -> {repl!r}")
        print(f"      {ctx}")
    print(f"\n{len(hits)} occurrence(s). Run `us_english_check.py --fix` to rewrite,")
    print("or add an EXEMPT_SUBSTRINGS entry if a spelling is an external contract.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
