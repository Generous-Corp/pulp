#!/usr/bin/env python3
"""compat.json split / aggregate / sync tool (roadmap P5-NEW-B).

The repo-root ``compat.json`` is a single ~8.2k-line web-compat
inventory keyed by surface (css / rn / yoga / html / canvas2d /
imports) plus non-surface metadata (``compat-schema-version``,
``_comment``, ``_audit``, ``react``).

That single file is awkward to edit and review. This tool splits it
into per-surface parts under ``compat/`` while keeping the aggregate
``compat.json`` byte-for-byte unchanged, so every existing consumer
(the harness verifier, the compat-sync gate, the import validators,
the C++ tests) sees zero difference.

Why verbatim text slicing instead of ``json.loads`` / ``json.dumps``
round-tripping: the aggregate was hand-edited inconsistently — some
strings carry literal non-ASCII characters (e.g. an em-dash) and
others carry the ``\\uXXXX`` escaped form. No single ``json.dumps``
setting reproduces that mix, so re-serialization would NOT be
byte-stable. Instead this tool slices the raw text at the top-level
key boundaries: every part holds the exact bytes of its key block,
and the aggregate is rebuilt by concatenating those bytes with the
original ``{\\n`` prefix, ``,\\n`` separators, and ``\\n}`` suffix.

Layout produced::

    compat/
      _meta.json     non-surface keys: compat-schema-version,
                     _comment, _audit, react  (verbatim slices)
      css.json       one verbatim slice per surface
      rn.json
      yoga.json
      html.json
      canvas2d.json
      imports.json

Each part file is a normal JSON object whose single top-level key is
the surface name (``{"css": { ... }}``) — so a part is independently
loadable / lintable. The value text inside is the EXACT bytes lifted
from the aggregate (preserving its em-dash / escape inconsistencies),
so concatenation is lossless.

Commands::

    compat_aggregate.py split    # (re)generate compat/ parts from compat.json
    compat_aggregate.py build    # regenerate compat.json from compat/ parts
    compat_aggregate.py check    # assert aggregate == regenerated-from-parts

``check`` is the CI / pre-push gate: it fails loudly if compat.json
and the compat/ parts have drifted apart.

Zero third-party dependencies — stdlib only, PEP-668 safe.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

# Top-level keys, in the exact order they appear in compat.json.
# Order is load-bearing for byte-stable reassembly.
SURFACE_KEYS = ["css", "rn", "yoga", "html", "canvas2d", "imports"]
META_KEYS = ["compat-schema-version", "_comment", "_audit", "react"]
ALL_KEYS_ORDER = ["compat-schema-version", "_comment", "_audit",
                  "css", "rn", "yoga", "react", "html", "canvas2d",
                  "imports"]

META_PART = "_meta.json"


# ── repo layout ─────────────────────────────────────────────────────────


def repo_root() -> Path:
    """Locate the repo root (the dir holding compat.json + CMakeLists.txt)."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            check=True, capture_output=True, text=True,
        )
        return Path(out.stdout.strip())
    except (subprocess.CalledProcessError, FileNotFoundError):
        cur = Path(__file__).resolve()
        for parent in [cur, *cur.parents]:
            if (parent / "compat.json").exists():
                return parent
        raise SystemExit("compat_aggregate: cannot locate repo root")


# ── verbatim slicing ────────────────────────────────────────────────────


def slice_aggregate(raw: str) -> dict[str, str]:
    """Split the raw aggregate text into verbatim ``  "key": <value>``
    blocks, keyed by top-level key.

    The aggregate has the exact shape::

        {\\n
          "k0": <v0>,\\n
          "k1": <v1>,\\n
          ...
          "kN": <vN>\\n
        }

    so block_i is the substring ``  "ki": <vi>`` with no surrounding
    separators. Returns ``{key: block_text}``.
    """
    if not raw.startswith("{\n"):
        raise ValueError("compat.json does not start with '{\\n' — "
                          "structure assumption broken, refusing to slice")
    if not raw.endswith("\n}"):
        raise ValueError("compat.json does not end with '\\n}' — "
                          "structure assumption broken, refusing to slice")

    # Find the byte offset where each top-level key block begins.
    starts: list[tuple[str, int]] = []
    for key in ALL_KEYS_ORDER:
        needle = '\n  "' + key + '":'
        idx = raw.find(needle)
        if idx == -1:
            # First key has no leading '\n' (it follows the '{').
            needle0 = '  "' + key + '":'
            idx0 = raw.find(needle0)
            if idx0 == -1:
                raise ValueError(
                    f"compat.json: top-level key {key!r} not found"
                )
            starts.append((key, idx0))
        else:
            starts.append((key, idx + 1))  # +1: skip the '\n', point at '  '

    starts.sort(key=lambda kv: kv[1])
    found_order = [k for k, _ in starts]
    if found_order != ALL_KEYS_ORDER:
        raise ValueError(
            "compat.json: unexpected top-level key order "
            f"{found_order!r} (expected {ALL_KEYS_ORDER!r})"
        )

    blocks: dict[str, str] = {}
    for i, (key, start) in enumerate(starts):
        if i + 1 < len(starts):
            nxt = starts[i + 1][1]
            sep = raw[nxt - 2:nxt]
            if sep != ",\n":
                raise ValueError(
                    f"compat.json: expected ',\\n' before key after "
                    f"{key!r}, got {sep!r}"
                )
            blocks[key] = raw[start:nxt - 2]
        else:
            blocks[key] = raw[start:-2]  # trim trailing '\n}'
    return blocks


def assemble_aggregate(blocks: dict[str, str]) -> str:
    """Inverse of :func:`slice_aggregate` — concatenate verbatim blocks
    back into the byte-identical aggregate text."""
    missing = [k for k in ALL_KEYS_ORDER if k not in blocks]
    if missing:
        raise ValueError(f"missing key block(s): {missing}")
    body = ",\n".join(blocks[k] for k in ALL_KEYS_ORDER)
    return "{\n" + body + "\n}"


def part_text_for(keys: list[str], blocks: dict[str, str]) -> str:
    """Render a standalone part file for ``keys``.

    A part is a valid JSON object whose body is the verbatim key
    block(s) lifted from the aggregate, wrapped in ``{\\n ... \\n}``.
    Because the inner bytes are verbatim, the part round-trips exactly.
    """
    body = ",\n".join(blocks[k] for k in keys)
    return "{\n" + body + "\n}\n"


def blocks_from_part(text: str, expected_keys: list[str]) -> dict[str, str]:
    """Recover verbatim key blocks from a part file produced by
    :func:`part_text_for`."""
    stripped = text.rstrip("\n")
    if not stripped.startswith("{\n") or not stripped.endswith("\n}"):
        raise ValueError("part file: not a '{\\n ... \\n}' object")
    # Re-use the aggregate slicer logic: the inner structure is identical.
    starts: list[tuple[str, int]] = []
    for key in expected_keys:
        needle = '\n  "' + key + '":'
        idx = stripped.find(needle)
        if idx == -1:
            needle0 = '  "' + key + '":'
            idx0 = stripped.find(needle0)
            if idx0 == -1:
                raise ValueError(f"part file: key {key!r} not found")
            starts.append((key, idx0))
        else:
            starts.append((key, idx + 1))
    starts.sort(key=lambda kv: kv[1])
    blocks: dict[str, str] = {}
    for i, (key, start) in enumerate(starts):
        if i + 1 < len(starts):
            nxt = starts[i + 1][1]
            blocks[key] = stripped[start:nxt - 2]
        else:
            blocks[key] = stripped[start:-2]
    return blocks


# ── commands ─────────────────────────────────────────────────────────────


def cmd_split(root: Path) -> int:
    """Regenerate compat/ parts from the authoritative compat.json."""
    aggregate = root / "compat.json"
    raw = aggregate.read_text(encoding="utf-8")
    blocks = slice_aggregate(raw)

    parts_dir = root / "compat"
    parts_dir.mkdir(exist_ok=True)

    # Per-surface parts.
    for key in SURFACE_KEYS:
        (parts_dir / f"{key}.json").write_text(
            part_text_for([key], blocks), encoding="utf-8",
        )
    # Residual non-surface keys.
    (parts_dir / META_PART).write_text(
        part_text_for(META_KEYS, blocks), encoding="utf-8",
    )

    # Verify the round-trip immediately.
    rebuilt = _aggregate_from_parts(parts_dir)
    if rebuilt != raw:
        print("compat_aggregate: split produced a non-byte-identical "
              "round-trip — aborting (compat/ left in place for "
              "inspection).", file=sys.stderr)
        return 1
    print(f"compat_aggregate: wrote {len(SURFACE_KEYS) + 1} part files "
          f"under {parts_dir} (round-trip byte-identical).")
    return 0


def _aggregate_from_parts(parts_dir: Path) -> str:
    """Read compat/ parts and reassemble the aggregate text."""
    blocks: dict[str, str] = {}
    for key in SURFACE_KEYS:
        part = parts_dir / f"{key}.json"
        if not part.exists():
            raise FileNotFoundError(f"missing part: {part}")
        blocks.update(blocks_from_part(
            part.read_text(encoding="utf-8"), [key],
        ))
    meta = parts_dir / META_PART
    if not meta.exists():
        raise FileNotFoundError(f"missing part: {meta}")
    blocks.update(blocks_from_part(
        meta.read_text(encoding="utf-8"), META_KEYS,
    ))
    return assemble_aggregate(blocks)


def cmd_build(root: Path) -> int:
    """Regenerate compat.json from the compat/ parts."""
    parts_dir = root / "compat"
    if not parts_dir.is_dir():
        print(f"compat_aggregate: no parts dir at {parts_dir} — run "
              "`split` first.", file=sys.stderr)
        return 1
    rebuilt = _aggregate_from_parts(parts_dir)
    (root / "compat.json").write_text(rebuilt, encoding="utf-8")
    print(f"compat_aggregate: regenerated compat.json from {parts_dir}.")
    return 0


def cmd_check(root: Path) -> int:
    """Assert compat.json == aggregate-regenerated-from-parts.

    This is the CI / pre-push sync gate. Exit 0 when in sync, 1 when
    drifted, 2 on a structural error.
    """
    aggregate = root / "compat.json"
    parts_dir = root / "compat"
    if not aggregate.exists():
        print("compat_aggregate: compat.json missing", file=sys.stderr)
        return 2
    if not parts_dir.is_dir():
        print(f"compat_aggregate: no parts dir at {parts_dir} — run "
              "`split` to create it.", file=sys.stderr)
        return 2

    current = aggregate.read_text(encoding="utf-8")
    try:
        rebuilt = _aggregate_from_parts(parts_dir)
    except (ValueError, FileNotFoundError) as exc:
        print(f"compat_aggregate: cannot rebuild from parts: {exc}",
              file=sys.stderr)
        return 2

    if current == rebuilt:
        print("compat_aggregate: compat.json is byte-identical to the "
              "compat/ parts.")
        return 0

    # Drift: report the first divergence to help the author.
    n = min(len(current), len(rebuilt))
    first = next((i for i in range(n) if current[i] != rebuilt[i]), n)
    print("compat_aggregate: DRIFT — compat.json and compat/ parts "
          "disagree.", file=sys.stderr)
    print(f"  first difference at byte {first}", file=sys.stderr)
    print(f"  compat.json : ...{current[max(0, first-40):first+40]!r}...",
          file=sys.stderr)
    print(f"  from parts  : ...{rebuilt[max(0, first-40):first+40]!r}...",
          file=sys.stderr)
    print("  Fix: edit the relevant compat/<surface>.json part, then run "
          "`tools/scripts/compat_aggregate.py build` to regenerate "
          "compat.json (or `split` to regenerate the parts from the "
          "aggregate).", file=sys.stderr)
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="compat.json split / aggregate / sync (P5-NEW-B)",
    )
    parser.add_argument(
        "command", choices=("split", "build", "check"),
        help="split: compat.json -> compat/ parts; "
             "build: compat/ parts -> compat.json; "
             "check: assert the two are byte-identical",
    )
    parser.add_argument(
        "--repo-root", default=None,
        help="Override repo root (default: git toplevel / compat.json walk)",
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root) if args.repo_root else repo_root()

    if args.command == "split":
        return cmd_split(root)
    if args.command == "build":
        return cmd_build(root)
    return cmd_check(root)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
