#!/usr/bin/env python3
"""Generate `migration_index.cpp` from the `docs/migrations/*.md` tree.

Release-discovery Slice 3 (#548, parent #499).

Each migration doc is a Markdown file with a TOML frontmatter block:

    ---
    version = "0.27.0"
    breaking = true
    applies_if = "cli_version_from < 0.27.0 && cli_version_to >= 0.27.0"
    summary = "CLI config file moved from ~/.pulp/config to ~/.pulp/config.toml"
    ---

    ## Breaking changes
    - ...

The script scans every ``docs/migrations/*.md`` except ``README.md``,
parses the frontmatter, and emits a compile-time ``MigrationEntry`` table
keyed off ``version``. The resulting ``tools/cli/migration_index.cpp`` is
compiled into ``pulp-cli`` so upgrade notes never need a network fetch
or a filesystem scan at runtime.

Run by CMake at configure time (see ``tools/cli/CMakeLists.txt``). Also
usable standalone for local iteration / tests:

    python3 tools/scripts/build_migration_index.py \
        --docs-dir docs/migrations \
        --out tools/cli/migration_index.cpp

The output is deterministic — entries are sorted by parsed semver so
running the script twice with the same input always yields byte-identical
output. This matters because CMake's ``configure_file`` downstream build
caches are keyed off the file contents.

The parser is deliberately minimal — only the fields we actually use
are recognised. Anything else in the frontmatter is preserved verbatim
in the generated file as a comment so reviewers can see what was
dropped.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from dataclasses import dataclass, field
from typing import Iterable


SUPPORTED_FIELDS = {"version", "breaking", "applies_if", "summary"}


@dataclass
class Entry:
    version: str
    breaking: bool
    applies_if: str
    summary: str
    body: str
    source_path: pathlib.Path
    extra: dict = field(default_factory=dict)


# ── Tiny TOML frontmatter parser ────────────────────────────────────────────
#
# Intentionally NOT a full TOML parser — we support the exact shapes the
# schema doc advertises:
#
#     key = "string value"
#     key = true | false
#
# Quotes are mandatory for strings. Bare-string values are rejected so
# we never silently accept a typo like `version = 0.27.0` (which TOML
# would treat as a float). Comments (`# ...`) and blank lines are
# ignored. Arrays / inline tables / multi-line strings are not
# supported; if one sneaks in, the script exits non-zero with a
# pointer to the offending file.


def _parse_toml_frontmatter(text: str, source: pathlib.Path) -> dict:
    out: dict = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            _fatal(source, f"expected `key = value`, got: {raw_line!r}")
        key, _, value = line.partition("=")
        key = key.strip()
        value = value.strip()

        # Strip trailing comment (but NOT `#` inside a quoted string).
        if value.startswith('"'):
            # Find closing quote respecting backslash escapes.
            end = 1
            while end < len(value):
                if value[end] == "\\" and end + 1 < len(value):
                    end += 2
                    continue
                if value[end] == '"':
                    end += 1
                    break
                end += 1
            else:
                _fatal(source, f"unterminated string on line: {raw_line!r}")
            # Anything after the closing quote (other than whitespace + comment) is an error.
            trailing = value[end:].lstrip()
            if trailing and not trailing.startswith("#"):
                _fatal(source, f"trailing content after string: {raw_line!r}")
            parsed = _unescape(value[1:end - 1])
            out[key] = parsed
            continue

        # Non-quoted value: bool or error.
        comment_idx = value.find("#")
        if comment_idx >= 0:
            value = value[:comment_idx].rstrip()
        if value == "true":
            out[key] = True
        elif value == "false":
            out[key] = False
        else:
            _fatal(source,
                   f"unsupported value shape for key {key!r}: {raw_line!r}. "
                   "Strings must be double-quoted; booleans are true|false.")
    return out


def _unescape(s: str) -> str:
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == "n":      out.append("\n")
            elif nxt == "t":    out.append("\t")
            elif nxt == "r":    out.append("\r")
            elif nxt == "\\":   out.append("\\")
            elif nxt == '"':    out.append('"')
            else:
                out.append(c)
                out.append(nxt)
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _fatal(source: pathlib.Path, msg: str) -> None:
    sys.stderr.write(f"build_migration_index: {source}: {msg}\n")
    sys.exit(2)


# ── Entry discovery ─────────────────────────────────────────────────────────


FRONTMATTER_RE = re.compile(r"^---\s*\n(.*?)\n---\s*\n?(.*)$", re.DOTALL)


def load_entry(path: pathlib.Path) -> Entry:
    raw = path.read_text(encoding="utf-8")
    m = FRONTMATTER_RE.match(raw)
    if not m:
        _fatal(path, "missing or malformed TOML frontmatter block "
                     "(expected `---\\n...\\n---`)")
    frontmatter = _parse_toml_frontmatter(m.group(1), path)
    body = m.group(2).lstrip("\n")

    if "version" not in frontmatter:
        _fatal(path, "frontmatter missing required `version` field")
    version = frontmatter["version"]
    if not isinstance(version, str):
        _fatal(path, f"`version` must be a string, got {type(version).__name__}")
    # Reject non-semver `version` strings up front so typos like
    # `version = "0.29"` or `version = "abc"` fail the build instead of
    # sorting as (0,0,0) and silently disappearing from the runtime
    # hop-filter (which parses the same semver at load time and drops
    # anything it can't parse). Every migration doc must pin a real
    # released version to be discoverable by `pulp upgrade migration-notes`.
    if not SEMVER_RE.match(version):
        _fatal(
            path,
            f"`version` must be semver (X.Y.Z), got {version!r}. "
            "Examples: \"0.27.0\", \"1.2.3\", \"v0.30.0\".",
        )

    # Types must match the schema. `bool("false")` is True — accepting
    # strings for a boolean field would silently flip migration notes
    # to breaking in shipped CLI output; accepting ints for string
    # fields would stringify "42" into the summary. Fail fast instead.
    breaking_raw = frontmatter.get("breaking", False)
    if not isinstance(breaking_raw, bool):
        _fatal(
            path,
            f"`breaking` must be a boolean (true|false), got "
            f"{type(breaking_raw).__name__}: {breaking_raw!r}.",
        )
    for key in ("applies_if", "summary"):
        if key in frontmatter and not isinstance(frontmatter[key], str):
            _fatal(
                path,
                f"`{key}` must be a double-quoted string, got "
                f"{type(frontmatter[key]).__name__}: {frontmatter[key]!r}.",
            )

    extra = {k: v for k, v in frontmatter.items() if k not in SUPPORTED_FIELDS}
    if extra:
        # Not fatal — just noted in the generated file header.
        pass

    return Entry(
        version=version,
        breaking=breaking_raw,
        applies_if=frontmatter.get("applies_if", ""),
        summary=frontmatter.get("summary", ""),
        body=body,
        source_path=path,
        extra=extra,
    )


SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:[.\-+].*)?$")


def semver_key(version: str) -> tuple[int, int, int]:
    m = SEMVER_RE.match(version)
    if not m:
        return (0, 0, 0)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


# ── C++ codegen ─────────────────────────────────────────────────────────────


def _cpp_escape(s: str) -> str:
    out = []
    for c in s:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif ord(c) < 0x20:
            out.append(f"\\x{ord(c):02x}")
        else:
            out.append(c)
    return "".join(out)


HEADER = """// migration_index.cpp — GENERATED at CMake configure time.
//
// Source: tools/scripts/build_migration_index.py
// Inputs: docs/migrations/*.md
//
// DO NOT EDIT BY HAND. Re-run `cmake --build` to regenerate, or invoke
// the Python script directly for iteration:
//
//     python3 tools/scripts/build_migration_index.py \\
//         --docs-dir docs/migrations \\
//         --out tools/cli/migration_index.cpp
//
// Schema / semantics: see tools/cli/migration_index.hpp and #548.

#include "migration_index.hpp"

namespace pulp::cli::migration {

namespace {

"""

FOOTER = """}  // namespace

const MigrationEntry* const kMigrationIndex = kTable;
const std::size_t kMigrationIndexSize = sizeof(kTable) / sizeof(kTable[0]);

}  // namespace pulp::cli::migration
"""


def emit_cpp(entries: list[Entry]) -> str:
    buf: list[str] = [HEADER]
    if not entries:
        # An empty array is not valid C++; emit a 1-entry sentinel whose
        # version sorts before every real release so lookups still work.
        buf.append("// No migration docs found under docs/migrations/.\n")
        buf.append("constexpr MigrationEntry kTable[] = {\n")
        buf.append('    {"", false, "", "", ""},\n')
        buf.append("};\n\n")
        buf.append("}  // namespace\n\n")
        buf.append(
            "const MigrationEntry* const kMigrationIndex = nullptr;\n"
            "const std::size_t kMigrationIndexSize = 0;\n\n"
            "}  // namespace pulp::cli::migration\n"
        )
        return "".join(buf)

    buf.append("constexpr MigrationEntry kTable[] = {\n")
    for e in entries:
        rel = e.source_path.as_posix()
        buf.append(f'    // source: {rel}\n')
        if e.extra:
            buf.append(f'    // note: extra frontmatter keys ignored: {sorted(e.extra)}\n')
        buf.append("    {\n")
        buf.append(f'        "{_cpp_escape(e.version)}",\n')
        buf.append(f'        {"true" if e.breaking else "false"},\n')
        buf.append(f'        "{_cpp_escape(e.applies_if)}",\n')
        buf.append(f'        "{_cpp_escape(e.summary)}",\n')
        buf.append(f'        "{_cpp_escape(e.body)}",\n')
        buf.append("    },\n")
    buf.append("};\n\n")
    buf.append(FOOTER)
    return "".join(buf)


# ── CLI driver ──────────────────────────────────────────────────────────────


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--docs-dir", type=pathlib.Path, required=True,
                    help="Directory containing vX.Y.Z.md migration files.")
    ap.add_argument("--out", type=pathlib.Path, required=True,
                    help="Path to write the generated migration_index.cpp.")
    args = ap.parse_args(argv)

    docs_dir: pathlib.Path = args.docs_dir
    if not docs_dir.is_dir():
        sys.stderr.write(f"build_migration_index: docs dir not found: {docs_dir}\n")
        return 2

    entries: list[Entry] = []
    for md in sorted(docs_dir.glob("*.md")):
        if md.name.lower() == "readme.md":
            continue
        entries.append(load_entry(md))

    entries.sort(key=lambda e: semver_key(e.version))

    # Detect duplicate versions — they would silently shadow each other.
    seen: dict[str, pathlib.Path] = {}
    for e in entries:
        if e.version in seen:
            _fatal(e.source_path,
                   f"duplicate `version = {e.version!r}` already defined in "
                   f"{seen[e.version]}")
        seen[e.version] = e.source_path

    generated = emit_cpp(entries)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Only rewrite when the content actually changes so CMake avoids
    # spurious recompiles on no-op reconfigures.
    old = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
    if old != generated:
        args.out.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
