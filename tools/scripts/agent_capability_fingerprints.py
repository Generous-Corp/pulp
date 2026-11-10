"""Reviewed header fingerprints for capability bindings, one file per header.

A binding names a public header; the header's reviewed SHA-256 is recorded
once, in `tools/agent-capabilities/fingerprints/<include>.json`, rather than as
a literal on every catalog row that names it. The literal used to be copied
into each row, so two changes to different capabilities that shared nothing
but a catalog module still rewrote neighbouring lines, and one header edit
rewrote every row naming it.

The fingerprint is a review guard, not derived data: `--check` fails when a
header's bytes no longer match its recorded digest, and only an explicit
`agent_capability_manifest.py --accept-fingerprint <include>` records the new
digest. It is never stamped automatically, because the diff of the fragment is
what tells a reviewer that a contract header moved.
"""
from __future__ import annotations

import ast
import pathlib
from typing import Any, Iterable

import agent_capability_store as store

CATALOG_GLOB = "tools/scripts/agent_capability_catalog_*.py"


def binding_includes(exports: Iterable[dict[str, Any]]) -> list[str]:
    return sorted({item["include"] for row in exports for item in row["bindings"]})


def problems(exports: Iterable[dict[str, Any]], fingerprints: dict[str, str]) -> list[str]:
    """Every binding header has exactly one reviewed fingerprint, and nothing else does."""
    exports = list(exports)
    found: list[str] = []
    named = set(binding_includes(exports))
    for row in exports:
        for item in row["bindings"]:
            if "_legacy_header_fingerprint" in item:
                found.append(
                    f"capability {row['key']} binding {item['include']} still passes "
                    "header_fingerprint=; the reviewed digest lives in "
                    f"{store.fingerprint_path(item['include'])}. Run "
                    "tools/scripts/agent_capability_manifest.py --migrate-fingerprints")
    for include in sorted(named - set(fingerprints)):
        found.append(
            f"binding header {include} has no reviewed fingerprint "
            f"({store.fingerprint_path(include)}); review the header, then run "
            f"tools/scripts/agent_capability_manifest.py --accept-fingerprint {include}")
    for include in sorted(set(fingerprints) - named):
        found.append(
            f"{store.fingerprint_path(include)} records a header no capability binding "
            "names; rerun tools/scripts/agent_capability_manifest.py --write to drop it")
    return found


def claims(exports: Iterable[dict[str, Any]], fingerprints: dict[str, str]) -> list[dict[str, Any]]:
    """(include, fingerprint, capability key) for every binding.

    A header with no fragment yields no fingerprint; `problems` names it."""
    return [
        {"include": item["include"], "fingerprint": fingerprints.get(item["include"]),
         "capability_key": row["key"]}
        for row in exports
        for item in row["bindings"]
    ]


def _offsets(text: str) -> list[int]:
    """Character offset of the start of each 1-based line."""
    # Split on "\n" only: ast counts lines that way, while str.splitlines
    # also breaks on form feeds and Unicode separators.
    starts = [0, 0]
    for line in text.split("\n")[:-1]:
        starts.append(starts[-1] + len(line) + 1)
    return starts


def _at(text: str, starts: list[int], line: int, byte_column: int) -> int:
    """Character offset of an ast (line, UTF-8 byte column) position."""
    row = text[starts[line]:starts[line + 1]] if line + 1 < len(starts) else text[starts[line]:]
    return starts[line] + len(row.encode("utf-8")[:byte_column].decode("utf-8"))


def _literal_removals(text: str, path: pathlib.Path) -> list[tuple[str, str, int, int]]:
    """(include, fingerprint, start, end) of each `header_fingerprint=` argument.

    [start, end) is the character span to delete: the argument and the comma
    that separates it from the previous argument (or, for a first argument,
    the comma after it), so whatever layout the call used, the call stays
    well formed. A value that is not a string literal is refused."""
    tree = ast.parse(text, filename=str(path))
    starts = _offsets(text)
    removals: list[tuple[str, str, int, int]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or getattr(node.func, "id", None) != "binding":
            continue
        keywords = {kw.arg: kw for kw in node.keywords}
        literal = keywords.get("header_fingerprint")
        if literal is None:
            continue
        include = keywords.get("include")
        if not (isinstance(literal.value, ast.Constant) and isinstance(literal.value.value, str)
                and include is not None and isinstance(include.value, ast.Constant)
                and isinstance(include.value.value, str)):
            raise ValueError(f"{path}:{literal.lineno}: header_fingerprint is not a string literal "
                             "next to a literal include; move it by hand")
        start = _at(text, starts, literal.lineno, literal.col_offset)
        end = _at(text, starts, literal.value.end_lineno, literal.value.end_col_offset)
        # A value wrapped in parentheses ends at the matching `)`, which the
        # value node's own span excludes.
        opener = text[start:_at(text, starts, literal.value.lineno, literal.value.col_offset)]
        for _ in range(opener.count("(")):
            close = end
            while close < len(text) and text[close].isspace():
                close += 1
            if close >= len(text) or text[close] != ")":
                raise ValueError(f"{path}:{literal.lineno}: unbalanced header_fingerprint=( ... )")
            end = close + 1
        before = start
        while before > 0 and text[before - 1].isspace():
            before -= 1
        if before > 0 and text[before - 1] == ",":
            start = before - 1
        else:
            after = end
            while after < len(text) and text[after].isspace():
                after += 1
            if after < len(text) and text[after] == ",":
                end = after + 1
                while end < len(text) and text[end] in " \t":
                    end += 1
        if text[start:end].strip(", \t\n()").count("header_fingerprint") != 1:
            raise ValueError(f"{path}:{literal.lineno}: header_fingerprint span is ambiguous")
        removals.append((include.value.value, literal.value.value, start, end))
    return removals


def migrate_catalog_literals(
    root: pathlib.Path, fingerprints: dict[str, str]
) -> tuple[dict[pathlib.Path, str], dict[str, str], list[str]]:
    """Move every catalog `header_fingerprint=` literal into its fragment.

    Returns (rewritten catalog sources, merged fingerprints, problems). Two
    literals for one header must agree with each other and with an existing
    fragment; a disagreement is a problem, never silently resolved. A rewrite
    that no longer parses, or still passes a literal, is a problem too."""
    root = pathlib.Path(root)
    merged = dict(fingerprints)
    rewritten: dict[pathlib.Path, str] = {}
    found: list[str] = []
    seen: dict[str, tuple[str, str]] = {}
    for path in sorted(root.glob(CATALOG_GLOB)):
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(root)
        try:
            removals = _literal_removals(text, relative)
        except (SyntaxError, ValueError) as error:
            found.append(str(error))
            continue
        if not removals:
            continue
        for include, value, start, _ in removals:
            where = f"{relative}:{text.count(chr(10), 0, start) + 1}"
            previous = seen.get(include)
            if previous is not None and previous[0] != value:
                found.append(f"{where}: {include} literal {value} disagrees with {previous[1]} "
                             f"({previous[0]})")
            seen.setdefault(include, (value, where))
            if include in fingerprints and fingerprints[include] != value:
                found.append(f"{where}: {include} literal {value} disagrees with its fragment "
                             f"{store.fingerprint_path(include)} ({fingerprints[include]}); "
                             "decide which digest was reviewed, fix one, and rerun")
            merged.setdefault(include, value)
        updated = text
        for _, _, start, end in sorted(removals, key=lambda item: item[2], reverse=True):
            updated = updated[:start] + updated[end:]
        try:
            if _literal_removals(updated, relative):
                found.append(f"{relative}: header_fingerprint literals remain after the rewrite")
        except SyntaxError as error:
            found.append(f"{relative}: the rewrite does not parse ({error}); move the literals by hand")
            continue
        rewritten[path] = updated
    return rewritten, merged, found
