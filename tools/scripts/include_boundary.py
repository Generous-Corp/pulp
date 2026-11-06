#!/usr/bin/env python3
"""include_boundary.py — lexical include-edge checks for Pulp layer boundaries.

One parser, several boundaries. Each boundary names the source roots it owns,
the ``pulp/<area>`` header areas it may (or may not) include, the include
directories its sources compile against, and any subtrees inside its roots
that belong to a different target. A source violates its boundary if it has an
include edge, in any spelling the compiler accepts, that leaves it:

* ``#include <pulp/<area>/...>``, ``#include "pulp/<area>/..."`` and the same
  with ``#import`` (Objective-C++), where ``<area>`` is outside the boundary;
* a ``pulp/...`` header that exists only in an excluded subtree's include
  directory (the compiler would reach it only through that target);
* a relative include (``"../x.hpp"``) that resolves, from the including file
  and then from the boundary's include directories as the compiler searches,
  outside the roots or into an excluded subtree, or does not resolve at all;
* an include whose path starts with, or passes through, one of the boundary's
  forbidden prefixes (``vellum/`` for the core-vellum boundary).

The scan needs no configured build. Known edges tracked for removal are
allowlisted per boundary with a reason; an allowlist entry that no longer
matches anything is itself a failure, so a removed edge leaves no exemption.

Exit codes: 0 = clean, 1 = violation found, 2 = scan error (including a scan
that found no files, which means the instrument looked in the wrong place).
"""
from __future__ import annotations

import argparse
import bisect
import dataclasses
import pathlib
import re
import sys

import vellum_policy

SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
            ".ipp", ".m", ".mm"}
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include_next|include|import)\s*([<"])([^>"]+)[>"]')


@dataclasses.dataclass(frozen=True)
class Boundary:
    name: str
    summary: str
    roots: tuple[str, ...]
    include_dirs: tuple[str, ...]
    allowed_areas: frozenset[str] | None = None
    forbidden_areas: frozenset[str] = frozenset()
    excluded: tuple[str, ...] = ()
    # Include directories of retained targets whose headers share an allowed
    # area's prefix (pulp-content installs pulp/state/content_registry.hpp).
    forbidden_include_dirs: tuple[str, ...] = ()
    # Non-Pulp include prefixes the roots may never name (``vellum/``).
    forbidden_prefixes: tuple[str, ...] = ()
    # Whether relative includes must resolve inside the roots. Boundaries that
    # only police a foreign prefix leave relative includes to the others.
    check_relative: bool = True


BOUNDARIES: dict[str, Boundary] = {
    "foundation": Boundary(
        name="foundation",
        summary="core/platform, core/runtime and core/simd may include only foundation headers",
        roots=("core/platform", "core/runtime", "core/simd"),
        include_dirs=("core/platform/include", "core/runtime/include", "core/simd/include"),
        allowed_areas=frozenset({"platform", "runtime", "simd", "foundation"}),
    ),
    "view-core": Boundary(
        name="view-core",
        summary=("core/view outside core/view/audio may not include a retained subsystem's "
                 "headers (tools/scripts/vellum_policy.json), nor the audio widgets' headers"),
        roots=("core/view",),
        include_dirs=("core/view/include",),
        forbidden_areas=vellum_policy.include_areas(),
        excluded=("core/view/audio",),
        forbidden_include_dirs=vellum_policy.include_dirs(),
    ),
    "core-vellum": Boundary(
        name="core-vellum",
        summary=("no source under core/ may include a Vellum header: Pulp core knows "
                 "nothing about Vellum, and integration packages connect the two"),
        roots=("core",),
        include_dirs=(),
        forbidden_prefixes=("vellum/",),
        check_relative=False,
    ),
}

# boundary name -> {(file, include): reason}. Every entry must still match.
ALLOWED: dict[str, dict[tuple[str, str], str]] = {name: {} for name in BOUNDARIES}
# widgets.hpp's audio-widget include is guarded by the definition only
# pulp-view-audio publishes; it is the recorded debt
# vellum-header-audio-widgets-include (docs/status/view-audio-seam-debts.json).
# The reload export TU retains the audio displays and their registration for
# hot-reloaded editors, so it names the provider's headers on purpose.
ALLOWED["view-core"].update({
    ("core/view/include/pulp/view/widgets.hpp", "pulp/view/audio_widgets.hpp"):
        "guarded compatibility include; debt vellum-header-audio-widgets-include",
    ("core/view/src/reload_exports.cpp", "pulp/view/register_audio_widgets.hpp"):
        "hot-reload export set retains audio display registration",
    ("core/view/src/reload_exports.cpp", "pulp/view/audio_widgets.hpp"):
        "hot-reload export set retains the audio displays",
})
# The opt-in D15 GPU-audio provider registers its Dawn bootstrap through the
# installed Vellum coordinator. This edge is removed when the shared GPU runtime
# (gpu-runtime) lands and owns the one-per-process Dawn bootstrap; the matching
# CMake edge is allowlisted in core_vellum_direction.py.
ALLOWED["core-vellum"].update({
    ("core/gpu_audio/src/detail/dawn_shared_io_provider.cpp",
     "vellum/graphics/dawn_bootstrap.hpp"):
        "D15 provider; removed when the shared GPU runtime lands",
    ("core/gpu_audio/src/detail/dawn_shared_io_provider.cpp",
     "vellum/graphics/dawn_native_bootstrap.hpp"):
        "D15 provider; removed when the shared GPU runtime lands",
})


def _inside(path: pathlib.Path, dirs: list[pathlib.Path]) -> bool:
    return any(path == d or d in path.parents for d in dirs)


_SPLICE_RE = re.compile(r"\\\r?\n")
# Next construct that changes how the scan proceeds: a string or character
# literal (which can hold comment markers), a line comment, or a block comment.
_LEXEME_RE = re.compile(
    r"""(?P<lit>(["'])(?:\\.|(?!\2).)*?(?:\2|\Z))"""
    r"""|(?P<line>//[^\n]*)"""
    r"""|(?P<block>/\*.*?(?:\*/|\Z))""",
    re.S)


def _logical_preprocessor_lines(text: str) -> list[tuple[int, str]]:
    """Return comment-free preprocessor lines after C/C++ line splicing.

    The preprocessor joins a backslash-newline pair before it removes comments,
    so an include directive may put its header, or even the ``include`` token,
    on the next physical line. Keep the first physical line number for each
    resulting logical line so diagnostics still point at the source directive.

    The scan is regex-driven so its cost is proportional to the number of
    lexemes, not characters; a pure per-character loop here made scanning all of
    core/ take tens of seconds.
    """
    # Splice: remember where each backslash-newline was removed so a position in
    # the spliced text maps back to its physical line.
    splice_at: list[int] = []
    pieces: list[str] = []
    prev = 0
    removed = 0
    for m in _SPLICE_RE.finditer(text):
        pieces.append(text[prev:m.start()])
        splice_at.append(m.start() - removed)
        removed += m.end() - m.start()
        prev = m.end()
    pieces.append(text[prev:])
    spliced = "".join(pieces)
    newline_at = [i for i, ch in enumerate(spliced) if ch == "\n"] if "\n" in spliced else []

    def physical(pos: int) -> int:
        return 1 + bisect.bisect_left(newline_at, pos) + bisect.bisect_right(splice_at, pos)

    # Output is a list of segments; each remembers how to recover the physical
    # line of any character in it. ("plain", out_start, spliced_start) maps
    # linearly; ("fixed", out_start, line) is a single generated character.
    out: list[str] = []
    seg_out_start: list[int] = []
    seg_info: list[tuple[str, int]] = []
    out_len = 0
    last_char = ""

    def emit_plain(start: int, end: int) -> None:
        nonlocal out_len, last_char
        if end > start:
            out.append(spliced[start:end])
            seg_out_start.append(out_len)
            seg_info.append(("plain", start))
            out_len += end - start
            last_char = spliced[end - 1]

    def emit_fixed(ch: str, line: int) -> None:
        nonlocal out_len, last_char
        out.append(ch)
        seg_out_start.append(out_len)
        seg_info.append(("fixed", line))
        out_len += 1
        last_char = ch

    pos = 0
    for m in _LEXEME_RE.finditer(spliced):
        kind = m.lastgroup
        if kind == "lit":
            continue  # literals pass through untouched, as part of the next plain run
        emit_plain(pos, m.start())
        if kind == "line":
            pos = m.end()  # the terminating newline is not consumed
            continue
        start_line = physical(m.start())
        body = spliced[m.start() + 2:m.end()]
        if body.endswith("*/"):
            body = body[:-2]
        segs = body.split("\n")
        body_pos = m.start() + 2
        for idx, seg in enumerate(segs):
            if seg and last_char != " ":
                emit_fixed(" ", start_line)
            if idx < len(segs) - 1:
                emit_fixed("\n", physical(body_pos + len(seg)))
            body_pos += len(seg) + 1
        pos = m.end()
    emit_plain(pos, len(spliced))

    joined = "".join(out)
    result: list[tuple[int, str]] = []
    start = 0
    n = len(joined)
    while start < n:
        end = joined.find("\n", start)
        if end < 0:
            end = n
        if end > start:
            seg = bisect.bisect_right(seg_out_start, start) - 1
            kind, base = seg_info[seg]
            line = physical(base + start - seg_out_start[seg]) if kind == "plain" else base
            result.append((line, joined[start:end]))
        start = end + 1
    return result


def include_edges(path: pathlib.Path) -> list[tuple[int, str, str]]:
    """(line, delimiter, target) for every include/import directive in a file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    edges = []
    for line, logical in _logical_preprocessor_lines(text):
        m = INCLUDE_RE.match(logical)
        if m:
            edges.append((line, m.group(1), m.group(2)))
    return edges


def crossings(path: pathlib.Path, root: pathlib.Path, b: Boundary) -> list[tuple[int, str]]:
    """(line, include) for every edge of ``path`` that leaves boundary ``b``."""
    roots = [(root / r).resolve() for r in b.roots]
    excluded = [(root / e).resolve() for e in b.excluded]
    include_dirs = [(root / d).resolve() for d in b.include_dirs]
    forbidden_dirs = [(root / d).resolve() for d in b.forbidden_include_dirs]
    found: list[tuple[int, str]] = []
    for n, delim, target in include_edges(path):
        normalized_target = target.replace("\\", "/")
        if any(normalized_target.startswith(prefix) or f"/{prefix}" in normalized_target
               for prefix in b.forbidden_prefixes):
            found.append((n, target))
            continue
        parts = normalized_target.split("/")
        if len(parts) >= 2 and parts[0] == "pulp":
            area = parts[1]
            if b.allowed_areas is not None and area not in b.allowed_areas:
                found.append((n, target))
            elif area in b.forbidden_areas:
                found.append((n, target))
            elif excluded and not any((d / target).exists() for d in include_dirs) \
                    and any((e / "include" / target).exists() for e in excluded):
                found.append((n, target))
            elif forbidden_dirs and not any((d / target).exists() for d in include_dirs) \
                    and any((d / target).exists() for d in forbidden_dirs):
                found.append((n, target))
            continue
        if b.check_relative and ".." in parts:
            # Mirror the compiler: the including file's directory first (for a
            # quoted include), then the include directories. An include that
            # resolves nowhere cannot be proven to stay inside, so it crosses.
            bases = ([path.parent] if delim == '"' else []) + include_dirs
            resolved = next((c for c in ((base / normalized_target).resolve() for base in bases)
                             if c.exists()), None)
            if resolved is None or not _inside(resolved, roots) or _inside(resolved, excluded):
                found.append((n, target))
    return found


def sources(root: pathlib.Path, b: Boundary) -> list[pathlib.Path]:
    excluded = [(root / e).resolve() for e in b.excluded]
    files: list[pathlib.Path] = []
    for r in b.roots:
        base = root / r
        if base.is_dir():
            files.extend(p for p in sorted(base.rglob("*"))
                         if p.is_file() and p.suffix in SUFFIXES
                         and not _inside(p.resolve(), excluded))
    return files


def run(b: Boundary, root: pathlib.Path, allowed: dict[tuple[str, str], str]) -> int:
    root = root.resolve()
    files = sources(root, b)
    if not files:
        print(f"{b.name}-include-boundary: no sources found under {', '.join(b.roots)} "
              f"in {root}; refusing to report a clean tree", file=sys.stderr)
        return 2
    hits: list[tuple[str, int, str]] = []
    for f in files:
        rel = f.relative_to(root).as_posix()
        try:
            hits.extend((rel, n, target) for n, target in crossings(f, root, b))
        except OSError as exc:
            print(f"{b.name}-include-boundary: cannot read {rel}: {exc}", file=sys.stderr)
            return 2

    used: set[tuple[str, str]] = set()
    violations = []
    for rel, n, target in hits:
        key = (rel, target)
        if key in allowed:
            used.add(key)
        else:
            violations.append(f"{rel}:{n}: includes '{target}' from outside the boundary")
    stale = [f"{rel}: allowlisted include '{target}' no longer present; remove the ALLOWED entry"
             for rel, target in sorted(set(allowed) - used)]
    if violations or stale:
        print(f"{b.name}-include-boundary: {b.summary}\n")
        for v in violations + stale:
            print(f"  {v}")
        return 1
    print(f"{b.name}-include-boundary: ok ({len(files)} files, {len(used)} allowlisted edge(s))")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--boundary", choices=sorted(BOUNDARIES), required=True)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    return run(BOUNDARIES[args.boundary], args.root, ALLOWED[args.boundary])


if __name__ == "__main__":
    raise SystemExit(main())
