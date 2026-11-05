#!/usr/bin/env python3
"""Derive the Pulp-kept parts of the shared mirror-edge table from their sources.

tools/scripts/vellum_mirror_edge_patterns.json is read by Pulp's closure gate
and vendored byte for byte into Vellum's extraction scan. Its Pulp-kept rules
must name every file, header and free function Pulp keeps, so those parts are
generated rather than written by hand:

- kept headers: every public header of the Pulp-kept view-audio targets
  (core/view/audio/include), plus every header an extraction unit lists under
  pulp_kept.files in vellum_d3_units.json;
- kept files: every file an extraction unit lists under pulp_kept.files (the
  core/view/audio tree is kept by its prefix);
- kept symbols: the namespace-scope free functions those view-audio headers
  declare or define;
- product rules: the retained subsystems in tools/scripts/vellum_policy.json,
  as include areas (`pulp/<name>/`), namespaces (`pulp::<name>`) and target
  names (`pulp-<name>`), next to the vendor SDK and audio-framework prefixes;
- approved exceptions: each line a docs/status/view-audio-seam-debts.json
  entry records that a content rule matches. A debt row is the only way a
  kept edge may stay in a mirrored file, and an exception holds for that
  exact line only.

No rule spells a namespace, include root or target prefix itself. The table's
`roots` name them (`namespace`, `include`, `target`), each a list so a mirror
part-way through a rename can be scanned under both spellings, and every rule
is built from them. The table's `controls` give, per rule, text that rule must
match, written with `@namespace@`, `@include@` and `@target@` placeholders;
test_vellum_closure.py renders the table under each spelling and fails when a
rule matches none of its controls or still spells a root it was not given.

`--check` fails when the table differs from what its sources produce;
`--write` regenerates it.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import vellum_policy

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

PRODUCT_INCLUDE = "mirror-product-include"
PRODUCT_SYMBOL = "mirror-product-symbol"
# Vendor plugin SDKs and Apple audio frameworks a mirrored file must not include.
VENDOR_INCLUDE_PREFIXES = (
    r"pluginterfaces/|public\.sdk/|clap/|lv2/|AudioUnitSDK/|oboe/|AudioToolbox/|AudioUnit/|"
    r"CoreAudio/|CoreAudioKit/|CoreMIDI/|AVFAudio/"
)
KEPT_INCLUDE = "mirror-pulp-kept-include"
KEPT_SYMBOL = "mirror-pulp-kept-symbol"
KEPT_FILE = "mirror-pulp-kept-file"
ROOT_KINDS = ("namespace", "include", "target")
# The include root the kept headers sit under in this checkout. Derived include
# and file rules are respelled from it to the table's include roots.
SOURCE_INCLUDE_ROOT = "pulp"


def table_roots(table: dict) -> dict[str, tuple[str, ...]]:
    """The namespace, include-root and target-prefix spellings the rules use."""
    roots = table.get("roots")
    if not isinstance(roots, dict) or set(roots) != set(ROOT_KINDS):
        raise SystemExit(f"{TABLE}: roots must name exactly {', '.join(ROOT_KINDS)}")
    spelled: dict[str, tuple[str, ...]] = {}
    for kind in ROOT_KINDS:
        names = roots[kind]
        if not isinstance(names, list) or not names or not all(
                isinstance(name, str) and re.fullmatch(r"[a-z][a-z0-9_]*", name) for name in names):
            raise SystemExit(f"{TABLE}: roots.{kind} must be a non-empty list of identifiers")
        spelled[kind] = tuple(names)
    return spelled


def root_pattern(names: tuple[str, ...]) -> str:
    """One spelling as itself, several as a non-capturing alternation."""
    escaped = [re.escape(name) for name in names]
    return escaped[0] if len(escaped) == 1 else "(?:" + "|".join(escaped) + ")"


def respell_include(path: str, include: str) -> str:
    """`pulp/view/x.hpp` as a pattern under the include-root pattern `include`."""
    head, sep, rest = path.partition("/")
    if head != SOURCE_INCLUDE_ROOT or not sep:
        raise SystemExit(f"kept header {path} is not under {SOURCE_INCLUDE_ROOT}/")
    return include + re.escape("/" + rest)


def respell_file(path: str, include: str) -> str:
    """A mirror-relative path as a pattern, its `include/<root>/` respelled."""
    match = re.match(r"^(core/[^/]+/include/)([^/]+/.+)$", path)
    if not match:
        return re.escape(path)
    return re.escape(match.group(1)) + respell_include(match.group(2), include)


def instantiate(template: str, spelling: str) -> str:
    """A control written with root placeholders, under one root spelling."""
    for kind in ROOT_KINDS:
        template = template.replace(f"@{kind}@", spelling)
    return template


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


def derived_rules(root: Path, roots: dict[str, tuple[str, ...]]) -> dict[str, str]:
    ns, inc, target = (root_pattern(roots[kind]) for kind in ROOT_KINDS)
    includes = "|".join(respell_include(path, inc) for path in kept_include_paths(root))
    files = "|".join(respell_file(path, inc) for path in d3_kept_files(root))
    symbols = alternation(kept_symbols(root))
    policy = root / "tools/scripts/vellum_policy.json"
    areas = "|".join(vellum_policy.names(policy))
    stems = vellum_policy.alternation(policy)
    return {
        PRODUCT_INCLUDE: (
            r"#\s*(?:include|import)\s*[<\"](?:" + inc + r"/(?:" + areas + r")/|" + VENDOR_INCLUDE_PREFIXES + r")"
        ),
        PRODUCT_SYMBOL: r"\b" + ns + r"::(?:" + areas + r")\b|\b" + target + r"[-_](?:" + stems + r")\b",
        KEPT_INCLUDE: (
            r"#\s*(?:include|import)\s*[<\"](?:" + inc + r"/view/(?:audio/|(?:widgets/)?(?:"
            + BASE_KEPT_VIEW_NAMES
            + r")\.h)|" + inc + r"/state/content_registry\.h|(?:" + includes + r")[>\"])"
        ),
        KEPT_SYMBOL: r"\b(?:" + symbols + r")\b",
        KEPT_FILE: (
            r"\A(?:core/(?:view/(?:audio/|(?:.*/)?(?:" + BASE_KEPT_VIEW_NAMES
            + r")\.)|host/|state/(?:.*/)?content_registry\.)|(?:" + files + r")\Z)"
        ),
    }


def respell_exception(path: str, line: str, spelling: str) -> tuple[str, str]:
    """A seam-debt path and line as a mirror renamed to `spelling` carries them:
    the `include/<root>/` path segment, include directives and the namespace."""
    root = re.escape(SOURCE_INCLUDE_ROOT)
    path = re.sub(rf"(/include/){root}/", rf"\g<1>{spelling}/", path)
    line = re.sub(rf"([<\"]){root}/", rf"\g<1>{spelling}/", line)
    line = re.sub(rf"\b{root}::", f"{spelling}::", line)
    return path, line


def approved_exceptions(root: Path, content_patterns: dict[str, str],
                        spellings: tuple[str, ...] = (SOURCE_INCLUDE_ROOT,)) -> list[dict[str, str]]:
    """Each seam-debt line that a content rule matches, by rule, path and exact line.

    The ledger records a line as this checkout spells it. A mirror renamed to
    another root carries the same line respelled, so each row is emitted once
    per spelling the table's roots name.
    """
    ledger = json.loads((root / SEAM_DEBTS).read_text(encoding="utf-8"))
    rules = {name: re.compile(pattern) for name, pattern in content_patterns.items()}
    exceptions: list[dict[str, str]] = []
    for entry in ledger.get("entries", []):
        source_path = str(entry.get("file", ""))
        lines = (root / source_path).read_text(encoding="utf-8").splitlines()
        debt_lines = [line for line in lines if any(re.search(p, line) for p in entry.get("patterns", []))]
        for source_line in debt_lines:
            for spelling in spellings:
                path, line = respell_exception(source_path, source_line, spelling)
                for name, rule in rules.items():
                    if rule.search(line):
                        exceptions.append({"rule": name, "path": path, "line": line, "debt": str(entry["id"])})
    return sorted(exceptions, key=lambda row: (row["path"], row["rule"], row["line"]))


def generate(root: Path, roots: dict[str, tuple[str, ...]] | None = None) -> dict:
    """The table its sources produce, under its own roots or the given ones."""
    table = json.loads((root / TABLE).read_text(encoding="utf-8"))
    if roots is not None:
        table["roots"] = {kind: list(roots[kind]) for kind in ROOT_KINDS}
    rules = derived_rules(root, table_roots(table))
    table["content_patterns"][PRODUCT_INCLUDE] = rules[PRODUCT_INCLUDE]
    table["content_patterns"][PRODUCT_SYMBOL] = rules[PRODUCT_SYMBOL]
    table["content_patterns"][KEPT_INCLUDE] = rules[KEPT_INCLUDE]
    table["content_patterns"][KEPT_SYMBOL] = rules[KEPT_SYMBOL]
    table["path_patterns"][KEPT_FILE] = rules[KEPT_FILE]
    roots = table_roots(table)
    spellings = tuple(dict.fromkeys(roots["include"] + roots["namespace"]))
    table["approved_exceptions"] = approved_exceptions(root, table["content_patterns"], spellings)
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
