#!/usr/bin/env python3
# tools/scripts/lcov_strip_excl.py
#
# Strip lines wrapped in `LCOV_EXCL_*` markers from an .lcov file before
# it reaches the Cobertura converter.
#
# Why this script exists
# ----------------------
# `llvm-cov export --format=lcov` does NOT honor `LCOV_EXCL_LINE` /
# `LCOV_EXCL_START` / `LCOV_EXCL_STOP` markers. Those markers are a
# `gcov` / `lcov` convention; LLVM's exporter just emits the raw line
# data and the markers are silently ignored. Result: any code wrapped in
# `LCOV_EXCL_START..STOP` STILL appears in `diff-cover`'s "missing
# lines" report — making the markers documentation-only.
#
# This helper closes that gap. It walks each `SF:<path>` record in the
# input .lcov, opens the source file, scans for the LCOV_EXCL markers,
# and drops every `DA:<line>,...`, `BRDA:<line>,...`, and `FN[A]:<line>,...`
# record whose line falls inside an excluded range. The line/branch/fn
# totals (`LF`, `LH`, `BRF`, `BRH`, `FNF`, `FNH`) are recomputed so the
# resulting file is internally consistent.
#
# Markers honored
# ---------------
#   LCOV_EXCL_LINE             — exclude that one line (the marker line itself)
#   LCOV_EXCL_START..STOP      — exclude every line in the (inclusive) range
#   LCOV_EXCL_BR_LINE          — exclude branch records on that line only
#   LCOV_EXCL_BR_START..STOP   — exclude branch records in the range
#
# Mismatched markers (an unmatched START or STOP) print a warning to
# stderr and are treated leniently (orphan STOP is ignored; orphan START
# excludes everything to EOF — same as lcov(1)).
#
# Usage
# -----
#   python3 tools/scripts/lcov_strip_excl.py INPUT.lcov OUTPUT.lcov
#   python3 tools/scripts/lcov_strip_excl.py --in-place INPUT.lcov
#   cat foo.lcov | python3 tools/scripts/lcov_strip_excl.py - -   # stdin/stdout
#
# Exit codes
# ----------
#   0 — success (or no markers found; input copied through unchanged)
#   1 — I/O error or malformed input
#   2 — argument error
#
# Refs: pulp #1058, parent #1049

from __future__ import annotations

import argparse
import os
import re
import sys
from typing import Dict, List, Optional, Set, TextIO, Tuple


# Regex catches the marker token even when it's inside a `//` or `/* */`
# comment, with optional leading whitespace and trailing junk. The
# trailing `\b` boundary keeps us from matching `LCOV_EXCL_LINE_FOO` as
# a `LCOV_EXCL_LINE`.
_MARKER_RE = re.compile(
    r"\bLCOV_EXCL_("
    r"LINE|START|STOP|BR_LINE|BR_START|BR_STOP"
    r")\b"
)


class _ExcludeMap:
    """Per-source-file map of which lines are excluded.

    Keys are 1-based line numbers (matching the .lcov line-number
    convention). `branch_lines` is a SUPERSET of `lines` because
    excluding a regular line also excludes branches on it. Hand-rolled
    instead of @dataclass to avoid a Python-3.14 quirk where
    `importlib.util.spec_from_file_location`-loaded modules trip a
    dataclass introspection edge case (`AttributeError: 'NoneType'`).
    """

    __slots__ = ("lines", "branch_lines")

    def __init__(self) -> None:
        self.lines: Set[int] = set()
        self.branch_lines: Set[int] = set()

    def is_line_excluded(self, n: int) -> bool:
        return n in self.lines

    def is_branch_excluded(self, n: int) -> bool:
        # Excluding a line via LCOV_EXCL_LINE/START..STOP also excludes
        # any branch records on that line. lcov(1) behavior.
        return n in self.lines or n in self.branch_lines


def compute_exclusions(source_text: str) -> _ExcludeMap:
    """Scan source text for LCOV_EXCL markers and build an exclude map.

    A standalone helper so tests can exercise the marker grammar
    directly without spinning up a full .lcov fixture.
    """
    em = _ExcludeMap()
    in_block = False
    block_start: Optional[int] = None
    in_br_block = False
    br_block_start: Optional[int] = None

    for idx, line in enumerate(source_text.splitlines(), start=1):
        m = _MARKER_RE.search(line)
        if not m:
            if in_block:
                em.lines.add(idx)
            if in_br_block:
                em.branch_lines.add(idx)
            continue

        kind = m.group(1)

        if kind == "LINE":
            em.lines.add(idx)
            # Continue tracking enclosing blocks, but the marker line
            # itself is always excluded.
            if in_block:
                em.lines.add(idx)
            if in_br_block:
                em.branch_lines.add(idx)
        elif kind == "BR_LINE":
            em.branch_lines.add(idx)
            if in_block:
                em.lines.add(idx)
            if in_br_block:
                em.branch_lines.add(idx)
        elif kind == "START":
            # Marker line itself is excluded (lcov behavior).
            in_block = True
            block_start = idx
            em.lines.add(idx)
        elif kind == "STOP":
            if not in_block:
                # Orphan STOP — warn and ignore.
                print(
                    f"[lcov_strip_excl] warning: orphan LCOV_EXCL_STOP at "
                    f"line {idx} (no matching START)",
                    file=sys.stderr,
                )
            else:
                em.lines.add(idx)
                in_block = False
                block_start = None
        elif kind == "BR_START":
            in_br_block = True
            br_block_start = idx
            em.branch_lines.add(idx)
        elif kind == "BR_STOP":
            if not in_br_block:
                print(
                    f"[lcov_strip_excl] warning: orphan LCOV_EXCL_BR_STOP at "
                    f"line {idx} (no matching BR_START)",
                    file=sys.stderr,
                )
            else:
                em.branch_lines.add(idx)
                in_br_block = False
                br_block_start = None

    if in_block:
        # Orphan START: lcov(1) excludes everything to EOF.
        print(
            f"[lcov_strip_excl] warning: unterminated LCOV_EXCL_START at "
            f"line {block_start}; excluding through EOF",
            file=sys.stderr,
        )
    if in_br_block:
        print(
            f"[lcov_strip_excl] warning: unterminated LCOV_EXCL_BR_START at "
            f"line {br_block_start}; excluding through EOF",
            file=sys.stderr,
        )

    return em


def _read_source(path: str) -> Optional[str]:
    """Best-effort source read. Returns None if the file is unreadable.

    .lcov SF: paths can point at vendored / generated files we can't
    open (especially after `-ignore-filename-regex` filtering on llvm-cov
    side). Returning None means "no markers" — equivalent to passing
    the records through.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _split_da(record: str) -> Tuple[Optional[int], str]:
    """Parse `DA:<line>,<hits>[,<checksum>]` and return (line, rest).

    rest is the unmodified original record so we can pass through
    untouched if the line is kept.
    """
    # `DA:NN,HH` — split off the prefix.
    if not record.startswith("DA:"):
        return None, record
    payload = record[3:]
    comma = payload.find(",")
    if comma < 0:
        return None, record
    try:
        return int(payload[:comma]), record
    except ValueError:
        return None, record


def _split_brda(record: str) -> Optional[int]:
    """Parse `BRDA:<line>,<block>,<branch>,<taken>` → line number."""
    if not record.startswith("BRDA:"):
        return None
    payload = record[5:]
    comma = payload.find(",")
    if comma < 0:
        return None
    try:
        return int(payload[:comma])
    except ValueError:
        return None


def _split_fn(record: str) -> Optional[int]:
    """Parse `FN:<line>,<name>` or `FNA:<line>,<hits>,<name>` → line."""
    if record.startswith("FN:"):
        payload = record[3:]
    elif record.startswith("FNA:"):
        payload = record[4:]
    else:
        return None
    comma = payload.find(",")
    if comma < 0:
        return None
    try:
        return int(payload[:comma])
    except ValueError:
        return None


def _split_fnda(record: str) -> Optional[Tuple[int, str]]:
    """Parse `FNDA:<hits>,<name>` — but FNDA carries no line number,
    so we have to look up the function's line via the FN/FNA records
    we've already seen. Returns (hits, name) or None if malformed.
    """
    if not record.startswith("FNDA:"):
        return None
    payload = record[5:]
    comma = payload.find(",")
    if comma < 0:
        return None
    try:
        return int(payload[:comma]), payload[comma + 1 :]
    except ValueError:
        return None


def filter_lcov(input_text: str, source_lookup=None) -> str:
    """Filter an .lcov stream, dropping records on LCOV_EXCL'd lines.

    Args:
        input_text: full contents of the .lcov file.
        source_lookup: callable taking a path string, returning source
            text or None. Defaults to reading from the filesystem; the
            indirection lets tests inject in-memory fixtures without
            writing temp files.

    Returns:
        The filtered .lcov text, with LF/LH/BRF/BRH/FNF/FNH counters
        recomputed per record block.
    """
    if source_lookup is None:
        source_lookup = _read_source

    out: List[str] = []
    cache: Dict[str, _ExcludeMap] = {}

    # State that resets at each `SF:` record:
    current_excl: Optional[_ExcludeMap] = None
    block_lines: List[str] = []
    # Function name → line, so we can apply LCOV_EXCL to FNDA via the
    # earlier FN/FNA record on the same line.
    fn_line_by_name: Dict[str, int] = {}

    def flush_block() -> None:
        """Flush `block_lines` to `out`, recomputing summary counters.

        Counters in lcov format:
          LF / LH = line-found / line-hit
          BRF / BRH = branch-found / branch-hit
          FNF / FNH = function-found / function-hit
        """
        if not block_lines:
            return

        # First pass: count after filtering (DA/BRDA/FN[A]/FNDA already
        # filtered upstream in this function — block_lines only holds
        # records we plan to keep).
        lf = lh = brf = brh = fnf = fnh = 0
        for rec in block_lines:
            if rec.startswith("DA:"):
                lf += 1
                # DA:<line>,<hits>...
                _, rest = _split_da(rec)
                # Re-parse the hits column.
                payload = rec[3:]
                first_c = payload.find(",")
                second_c = payload.find(",", first_c + 1)
                hits_str = (
                    payload[first_c + 1 : second_c]
                    if second_c >= 0
                    else payload[first_c + 1 :]
                )
                try:
                    if int(hits_str) > 0:
                        lh += 1
                except ValueError:
                    pass
            elif rec.startswith("BRDA:"):
                brf += 1
                # BRDA:<line>,<block>,<branch>,<taken>
                payload = rec[5:]
                last_c = payload.rfind(",")
                taken = payload[last_c + 1 :] if last_c >= 0 else ""
                # `taken` is "-" when never executed, else an integer.
                if taken not in ("-", "", "0"):
                    try:
                        if int(taken) > 0:
                            brh += 1
                    except ValueError:
                        pass
            elif rec.startswith("FN:") or rec.startswith("FNA:"):
                fnf += 1
            elif rec.startswith("FNDA:"):
                parsed = _split_fnda(rec)
                if parsed is not None and parsed[0] > 0:
                    fnh += 1

        # Strip out the original LF/LH/BRF/BRH/FNF/FNH lines (we
        # recompute them) and emit the rest, then append fresh totals.
        skip_prefixes = ("LF:", "LH:", "BRF:", "BRH:", "FNF:", "FNH:")
        for rec in block_lines:
            if rec.startswith(skip_prefixes):
                continue
            if rec == "end_of_record":
                continue
            out.append(rec)

        # Counter ordering matches lcov's own `geninfo` output so
        # downstream tools that pattern-match are unsurprised.
        if any(r.startswith(("FN:", "FNA:", "FNDA:")) for r in block_lines):
            out.append(f"FNF:{fnf}")
            out.append(f"FNH:{fnh}")
        if any(r.startswith("BRDA:") for r in block_lines):
            out.append(f"BRF:{brf}")
            out.append(f"BRH:{brh}")
        if any(r.startswith("DA:") for r in block_lines):
            out.append(f"LF:{lf}")
            out.append(f"LH:{lh}")
        out.append("end_of_record")

        block_lines.clear()
        fn_line_by_name.clear()

    for raw in input_text.split("\n"):
        rec = raw.rstrip("\r")
        if not rec:
            # Preserve blank lines outside any record.
            if current_excl is None:
                out.append("")
            continue

        if rec.startswith("SF:"):
            # Start of a new file record. Flush whatever we had and
            # set up exclusions for this file.
            flush_block()
            path = rec[3:].strip()
            if path not in cache:
                src = source_lookup(path)
                cache[path] = (
                    compute_exclusions(src) if src is not None else _ExcludeMap()
                )
            current_excl = cache[path]
            block_lines.append(rec)
            continue

        if rec == "end_of_record":
            flush_block()
            current_excl = None
            continue

        if current_excl is None:
            # Pre-amble or post-amble outside any SF block — preserve.
            out.append(rec)
            continue

        # Inside an SF..end_of_record block: filter excluded records.
        line_no, _ = _split_da(rec)
        if line_no is not None:
            if current_excl.is_line_excluded(line_no):
                continue
            block_lines.append(rec)
            continue

        line_no = _split_brda(rec)
        if line_no is not None:
            if current_excl.is_branch_excluded(line_no):
                continue
            block_lines.append(rec)
            continue

        line_no = _split_fn(rec)
        if line_no is not None:
            # Track function name → line so we can filter FNDA below.
            payload = rec[3:] if rec.startswith("FN:") else rec[4:]
            comma = payload.find(",")
            if comma >= 0:
                name = payload[comma + 1 :]
                fn_line_by_name[name] = line_no
            if current_excl.is_line_excluded(line_no):
                continue
            block_lines.append(rec)
            continue

        parsed_fnda = _split_fnda(rec)
        if parsed_fnda is not None:
            _, name = parsed_fnda
            fline = fn_line_by_name.get(name)
            if fline is not None and current_excl.is_line_excluded(fline):
                continue
            block_lines.append(rec)
            continue

        # Unknown record kind (TN, KF, VER, etc.) — pass through.
        block_lines.append(rec)

    # If the input ended without a closing end_of_record (rare/malformed
    # but defensive), flush the last block.
    flush_block()

    return "\n".join(out)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Strip LCOV_EXCL'd source lines from an .lcov file before "
            "the Cobertura converter sees them. Closes the gap that "
            "llvm-cov-export ignores LCOV_EXCL markers."
        ),
    )
    parser.add_argument(
        "input",
        help="Input .lcov path, or '-' for stdin",
    )
    parser.add_argument(
        "output",
        nargs="?",
        help="Output .lcov path, or '-' for stdout. Required unless --in-place.",
    )
    parser.add_argument(
        "--in-place",
        action="store_true",
        help="Overwrite the input file in place (mutually exclusive with output).",
    )
    args = parser.parse_args(argv)

    if args.in_place and args.output:
        parser.error("--in-place is mutually exclusive with a positional output path")
    if not args.in_place and args.output is None:
        parser.error("output path is required (or pass --in-place)")

    # Read input.
    if args.input == "-":
        text = sys.stdin.read()
    else:
        try:
            with open(args.input, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as e:
            print(f"[lcov_strip_excl] error reading {args.input}: {e}", file=sys.stderr)
            return 1

    filtered = filter_lcov(text)

    # Write output.
    out_path = args.input if args.in_place else args.output
    if out_path == "-":
        sys.stdout.write(filtered)
        return 0
    try:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(filtered)
    except OSError as e:
        print(f"[lcov_strip_excl] error writing {out_path}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
