#!/usr/bin/env python3
"""Derive the Pulp-kept parts of the shared mirror-edge table from their sources.

tools/scripts/vellum_mirror_edge_patterns.json is read by Pulp's closure gate
and vendored byte for byte into Vellum's extraction scan. Its Pulp-kept rules
must name every file, header and free function Pulp keeps, so those parts are
generated rather than written by hand:

- kept headers: every public header of the Pulp-kept view-audio targets
  (core/view/audio/include), plus every header a D3 unit lists under
  pulp_kept.files in vellum_d3_units.json;
- kept files: every file a D3 unit lists under pulp_kept.files (the
  core/view/audio tree is kept by its prefix);
- kept symbols: the namespace-scope free functions those view-audio headers
  declare or define;
- approved exceptions: each line a docs/status/view-audio-seam-debts.json
  entry records that a content rule matches. A debt row is the only way a
  kept edge may stay in a mirrored file, and an exception holds for that
  exact line only.

`--check` fails when the table differs from what its sources produce;
`--write` regenerates it.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = Path("tools/scripts/vellum_mirror_edge_patterns.json")
UNITS = Path("tools/scripts/vellum_d3_units.json")
SEAM_DEBTS = Path("docs/status/view-audio-seam-debts.json")
VIEW_AUDIO_INCLUDE = Path("core/view/audio/include")
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}

# Kept names that predate derivation and are not under core/view/audio (for
# example view-core widgets still awaiting their carve). They stay listed so
# no rule is weakened; the derived sets are added to them.
BASE_KEPT_VIEW_NAMES = (
    r"audio_bridge|audio_inspector_\w+|eq_curve_view|midi_binding|midi_keyboard|"
    r"musical_typing_keyboard\w*|plugin_manager_panel|sample_asset_drop_target|"
    r"step_grid_view|waveform_\w+|graph_editor_view"
)

KEPT_INCLUDE = "mirror-pulp-kept-include"
KEPT_SYMBOL = "mirror-pulp-kept-symbol"
KEPT_FILE = "mirror-pulp-kept-file"


def kept_view_audio_headers(root: Path) -> list[Path]:
    base = root / VIEW_AUDIO_INCLUDE
    return sorted(p for p in base.rglob("*") if p.is_file() and p.suffix in HEADER_SUFFIXES)


def d3_kept_files(root: Path) -> list[str]:
    units = json.loads((root / UNITS).read_text(encoding="utf-8"))
    files: set[str] = set()
    for unit in units.get("units", []):
        kept = unit.get("pulp_kept")
        if isinstance(kept, dict):
            files.update(str(path) for path in kept.get("files", []))
    return sorted(files)


def kept_include_paths(root: Path) -> list[str]:
    """Include spellings (`pulp/view/x.hpp`) of every Pulp-kept header."""
    paths = {p.relative_to(root / VIEW_AUDIO_INCLUDE).as_posix() for p in kept_view_audio_headers(root)}
    for rel in d3_kept_files(root):
        match = re.match(r"^core/[^/]+/include/(.+)$", rel)
        if match and Path(rel).suffix in HEADER_SUFFIXES:
            paths.add(match.group(1))
    return sorted(paths)


def free_functions(text: str) -> set[str]:
    """Names of free functions declared or defined at namespace scope."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"^\s*#[^\n]*", "", text, flags=re.M)
    declaration = re.compile(
        r"(?:^|;)\s*(?:(?:inline|static|extern|constexpr|\[\[\w+\]\])\s+)*"
        r"[A-Za-z_][\w:<>,*&\s]*?[\s*&]([a-z_][a-z0-9_]*)\s*"
        r"\([^;{}()]*(?:\([^()]*\)[^;{}()]*)*\)\s*(?:const\s*)?(?:noexcept\s*)?;"
    )
    definition = re.compile(
        r"[\s*&]([a-z_][a-z0-9_]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?$")
    stack: list[str] = []
    names: set[str] = set()
    start = 0
    for brace in re.finditer(r"[{}]", text):
        chunk = text[start:brace.start()]
        at_namespace_scope = all(kind == "ns" for kind in stack)
        if at_namespace_scope:
            names.update(match.group(1) for match in declaration.finditer(chunk))
        if brace.group(0) == "{":
            opens_namespace = re.search(r"\bnamespace\s*[\w:]*\s*$", chunk)
            if at_namespace_scope and not opens_namespace:
                defined = definition.search(chunk)
                if defined and not re.search(r"\b(?:struct|class|enum|union)\b[^;]*$", chunk):
                    names.add(defined.group(1))
            stack.append("ns" if opens_namespace else "block")
        elif stack:
            stack.pop()
        start = brace.end()
    return names


def kept_symbols(root: Path) -> list[str]:
    names: set[str] = set()
    for header in kept_view_audio_headers(root):
        names.update(free_functions(header.read_text(encoding="utf-8")))
    return sorted(names)


def alternation(items: list[str]) -> str:
    return "|".join(re.escape(item) for item in items)


def derived_rules(root: Path) -> dict[str, str]:
    includes = alternation(kept_include_paths(root))
    files = alternation(d3_kept_files(root))
    symbols = alternation(kept_symbols(root))
    return {
        KEPT_INCLUDE: (
            r"#\s*(?:include|import)\s*[<\"](?:pulp/view/(?:audio/|(?:widgets/)?(?:"
            + BASE_KEPT_VIEW_NAMES
            + r")\.h)|pulp/state/content_registry\.h|(?:" + includes + r")[>\"])"
        ),
        KEPT_SYMBOL: r"\b(?:" + symbols + r")\b",
        KEPT_FILE: (
            r"\A(?:core/(?:view/(?:audio/|(?:.*/)?(?:" + BASE_KEPT_VIEW_NAMES
            + r")\.)|host/|state/(?:.*/)?content_registry\.)|(?:" + files + r")\Z)"
        ),
    }


def approved_exceptions(root: Path, content_patterns: dict[str, str]) -> list[dict[str, str]]:
    """Each seam-debt line that a content rule matches, by rule, path and exact line."""
    ledger = json.loads((root / SEAM_DEBTS).read_text(encoding="utf-8"))
    rules = {name: re.compile(pattern) for name, pattern in content_patterns.items()}
    exceptions: list[dict[str, str]] = []
    for entry in ledger.get("entries", []):
        path = str(entry.get("file", ""))
        lines = (root / path).read_text(encoding="utf-8").splitlines()
        debt_lines = [line for line in lines if any(re.search(p, line) for p in entry.get("patterns", []))]
        for line in debt_lines:
            for name, rule in rules.items():
                if rule.search(line):
                    exceptions.append({"rule": name, "path": path, "line": line, "debt": str(entry["id"])})
    return sorted(exceptions, key=lambda row: (row["path"], row["rule"], row["line"]))


def generate(root: Path) -> dict:
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    rules = derived_rules(root)
    table["content_patterns"][KEPT_INCLUDE] = rules[KEPT_INCLUDE]
    table["content_patterns"][KEPT_SYMBOL] = rules[KEPT_SYMBOL]
    table["path_patterns"][KEPT_FILE] = rules[KEPT_FILE]
    table["approved_exceptions"] = approved_exceptions(root, table["content_patterns"])
    return table


def render(table: dict) -> str:
    return json.dumps(table, indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = parser.parse_args()
    path = args.root / TABLE
    expected = render(generate(args.root))
    if args.write:
        path.write_text(expected, encoding="utf-8")
        print(f"wrote {TABLE}")
        return 0
    if path.read_text(encoding="utf-8") != expected:
        print(f"{TABLE} is stale: run python3 tools/scripts/vellum_mirror_edge_table.py --write", file=sys.stderr)
        return 1
    print(f"{TABLE}: in sync with its sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
