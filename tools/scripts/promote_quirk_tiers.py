#!/usr/bin/env python3
"""promote_quirk_tiers.py — turn DAW-bench evidence into a quirks-header patch.

Pipeline:

  1. Read one or more **bench result files**. Two accepted shapes:

     a. The filled-in markdown templates under
        ``docs/validation/daw-bench/results/<date>/*.md`` — each contains
        a "Result" table whose rows look like::

            | `flag_name`              | Row | Confirmed | notes |

     b. The raw event-log files written by the bench plugin under
        ``~/Library/Logs/PulpHostBench/`` — the script can also infer
        a subset of flags directly from the events (e.g. seeing a
        ``process_without_prepare`` event Confirms
        ``fl_studio_setactive_process_mutex`` when the host tag is
        FLStudio).

  2. Build a ``{flag_name: highest_observed_status}`` map. Statuses sort
     ``Confirmed > Not Triggered > Refuted``: any Confirmed wins.

  3. Read the canonical quirks header (default:
     ``core/format/include/pulp/format/host_quirks.hpp``) and locate the
     ``HostQuirksMeta`` struct.

  4. For every flag with status ``Confirmed`` whose current meta tier is
     ``Speculative``, rewrite it to ``Validated`` in-place. Other tiers
     and flags untouched.

  5. Emit either a unified diff (``--output PATH`` or stdout) or write
     the file in place (``--in-place``).

Example::

    python3 tools/scripts/promote_quirk_tiers.py \\
        docs/validation/daw-bench/results/2026-06-01/*.md \\
        ~/Library/Logs/PulpHostBench/Reaper-CLAP-*.log \\
        --output /tmp/promote.patch
    git apply /tmp/promote.patch

The script intentionally never DOWNGRADES a tier. A Refuted row in the
result file does NOT flip Validated → Speculative or Speculative →
LessonOnly. That's an explicit ticket / decision, not an automation.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

# ---------------------------------------------------------------------------
# Status model
# ---------------------------------------------------------------------------

STATUS_CONFIRMED = "Confirmed"
STATUS_REFUTED = "Refuted"
STATUS_NOT_TRIGGERED = "NotTriggered"

# Ordering: higher wins.
_STATUS_ORDER = {
    STATUS_REFUTED: 0,
    STATUS_NOT_TRIGGERED: 1,
    STATUS_CONFIRMED: 2,
}

_STATUS_ALIASES = {
    "confirmed": STATUS_CONFIRMED,
    "c": STATUS_CONFIRMED,
    "yes": STATUS_CONFIRMED,
    "pass": STATUS_CONFIRMED,
    "refuted": STATUS_REFUTED,
    "r": STATUS_REFUTED,
    "no": STATUS_REFUTED,
    "fail": STATUS_REFUTED,
    "not triggered": STATUS_NOT_TRIGGERED,
    "nottriggered": STATUS_NOT_TRIGGERED,
    "nt": STATUS_NOT_TRIGGERED,
    "n/a": STATUS_NOT_TRIGGERED,
    "na": STATUS_NOT_TRIGGERED,
}


def _normalize_status(raw: str) -> str | None:
    key = raw.strip().lower()
    return _STATUS_ALIASES.get(key)


def _merge(prev: str | None, new: str) -> str:
    """Keep the highest-rank observation."""
    if prev is None:
        return new
    if _STATUS_ORDER[new] > _STATUS_ORDER[prev]:
        return new
    return prev


# ---------------------------------------------------------------------------
# Result-file parsing — markdown templates
# ---------------------------------------------------------------------------

# Flag column: backtick-quoted identifier OR bare identifier.
# Status column matches one of the alias strings above.
_RESULT_ROW = re.compile(
    r"^\|\s*`?(?P<flag>[A-Za-z_][A-Za-z0-9_]*)`?\s*"
    r"\|[^|]*"  # row-number column (catalog ref)
    r"\|\s*(?P<status>[^|]+?)\s*"
    r"\|.*\|\s*$"
)


def parse_markdown_results(path: Path) -> dict[str, str]:
    """Return ``{flag: status}`` extracted from the Result table of *path*."""
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = _RESULT_ROW.match(line)
        if not m:
            continue
        flag = m.group("flag")
        status = _normalize_status(m.group("status"))
        if status is None:
            # Could be the placeholder "<C/R/NT>" — silently skip.
            continue
        out[flag] = _merge(out.get(flag), status)
    return out


# ---------------------------------------------------------------------------
# Result-file parsing — raw bench logs
# ---------------------------------------------------------------------------

# Filename convention: <host>-<format>-<timestamp>-pidNNN.log
#
# The bench plugin writes filenames from `pulp::format::host_type_name()`, which
# returns display strings like "FL Studio", "Logic Pro", "Studio One", "Ableton
# Live", "REAPER", "WaveLab", "Bitwig Studio". Those contain spaces and mixed
# case, so the host segment is anything up to the last `-<format>-<timestamp>-`
# suffix. We match the trailing `<format>-<timestamp>-pid…` greedily from the
# right so the host segment can include spaces.
_LOG_FILENAME = re.compile(
    r"^(?P<host>[A-Za-z][A-Za-z0-9 ]*?)-(?P<format>[A-Za-z0-9]+)-\d"
)


# Map raw `host_type_name()` strings → the keys used in LOG_PROMOTION_RULES.
# Without normalization, rules like `Reaper`/`Wavelab`/`LogicPro` never match
# the emitted names `REAPER`/`WaveLab`/`Logic Pro` and only the `*` rule fires,
# silently under-promoting most host-specific quirks.
_HOST_NAME_ALIASES: dict[str, str] = {
    "reaper": "Reaper",
    "fl studio": "FLStudio",
    "flstudio": "FLStudio",
    "logic pro": "LogicPro",
    "logicpro": "LogicPro",
    "ableton live": "AbletonLive",
    "abletonlive": "AbletonLive",
    "studio one": "StudioOne",
    "studioone": "StudioOne",
    "cubase": "Cubase",
    "nuendo": "Nuendo",
    "wavelab": "Wavelab",
    "pro tools": "ProTools",
    "protools": "ProTools",
    "bitwig studio": "BitwigStudio",
    "bitwigstudio": "BitwigStudio",
    "bitwig": "Bitwig",
    "garageband": "GarageBand",
    "maschine": "Maschine",
    "ardour": "Ardour",
}


def normalize_host_name(raw: str) -> str:
    """Return the canonical host key used in LOG_PROMOTION_RULES.

    Accepts raw values produced by `pulp::format::host_type_name()` (which can
    have spaces and mixed case) and returns a stable key suitable for
    case-sensitive comparison against rule keys. Unknown hosts pass through
    with spaces stripped, preserving their original casing so caller-supplied
    custom rules still match if they happen to align.
    """
    if not raw:
        return raw
    key = raw.strip().lower()
    if key in _HOST_NAME_ALIASES:
        return _HOST_NAME_ALIASES[key]
    # Unknown host — collapse spaces but keep original case otherwise.
    return raw.replace(" ", "")

# Each log line: <iso>\t<event>\tkey=value\tkey=value...
_LOG_LINE = re.compile(r"^[^\t]+\t(?P<event>[A-Za-z_][A-Za-z0-9_]*)(?P<rest>(\t[^\t]+)*)\s*$")


def _parse_event_kv(rest: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for tok in rest.split("\t"):
        tok = tok.strip()
        if not tok or "=" not in tok:
            continue
        k, _, v = tok.partition("=")
        out[k] = v
    return out


# Mapping: a (host, observed-event-condition) → list of flags to mark Confirmed.
# The condition is either a literal event name (any occurrence) or a callable
# that inspects the event's key=value payload. Hosts use the same string as
# `pulp::format::host_type_name()` (e.g. "Reaper", "FLStudio", "AbletonLive").

HostEventRule = tuple[str, "object", tuple[str, ...]]

LOG_PROMOTION_RULES: tuple[HostEventRule, ...] = (
    # General defaults: any session that produced a `prepare` + a
    # successful `serialize_plugin_state` round-trip implies the cheap-
    # defense flags are observably valid in that host.
    ("*",      "prepare",                ("clamp_latency_to_nonneg",)),
    # Reaper rows.
    ("Reaper", "process_is_playing_edge",("reaper_vst3_gesture_ordering",)),
    ("Reaper", "process_without_prepare",("reaper_process_while_bypassed",)),
    ("Reaper", "bus_layout_proposal",    ("reaper_permissive_bus_arrangements",)),
    ("Reaper", "process_buffer_overrun", ("reaper_anticipative_fx_buffer_variability",)),
    ("Reaper", "process_sample_rate_drift", ("reaper_anticipative_fx_buffer_variability",)),
    ("Reaper", "deserialize_plugin_state",  ("reaper_midsession_setstate",)),
    # FL Studio.
    ("FLStudio", "process_without_prepare", ("fl_studio_setactive_process_mutex",)),
    ("FLStudio", "deserialize_plugin_state",("fl_studio_state_reader_skip",)),
    # Cubase 9.
    ("Cubase", "deserialize_plugin_state",  ("cubase9_state_blob_size_validation",)),
    # Cubase 10/11/12 — view resize cadence.
    ("Cubase", "view_resized",              ("cubase10_async_view_resize_queue",)),
    # Live.
    ("AbletonLive", "view_resized",         ("live_vst3_canresize_ignore",)),
    # Bitwig.
    ("BitwigStudio", "bus_layout_proposal", ("bitwig_vst3_setbusarrangements_while_active",)),
    ("Bitwig",       "bus_layout_proposal", ("bitwig_vst3_setbusarrangements_while_active",)),
    # Wavelab.
    ("Wavelab", "deserialize_plugin_state", ("wavelab_state_blob_fallback",)),
    ("Wavelab", "bus_layout_proposal",      ("wavelab_vst3_defer_activation",)),
    # Logic.
    ("LogicPro", "prepare",                 ("logic_au_tail_time_conversion",)),
)


def parse_log_results(path: Path) -> dict[str, str]:
    """Walk a single bench log file and return inferred flag→status hits."""
    if not path.is_file():
        return {}
    m = _LOG_FILENAME.match(path.name)
    if not m:
        return {}
    host = normalize_host_name(m.group("host"))
    events: set[str] = set()
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            mm = _LOG_LINE.match(line)
            if mm:
                events.add(mm.group("event"))
    except OSError:
        return {}

    out: dict[str, str] = {}
    for rule_host, rule_event, flags in LOG_PROMOTION_RULES:
        if rule_host != "*" and rule_host != host:
            continue
        if rule_event not in events:
            continue
        for f in flags:
            out[f] = _merge(out.get(f), STATUS_CONFIRMED)
    return out


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------


@dataclass
class Aggregation:
    flag_status: dict[str, str] = field(default_factory=dict)
    sources: dict[str, list[str]] = field(default_factory=dict)

    def merge(self, partial: dict[str, str], source: str) -> None:
        for flag, status in partial.items():
            prev = self.flag_status.get(flag)
            new = _merge(prev, status)
            if new != prev:
                self.flag_status[flag] = new
            self.sources.setdefault(flag, []).append(source)


def aggregate(paths: Iterable[Path]) -> Aggregation:
    agg = Aggregation()
    for p in paths:
        if p.suffix.lower() == ".md":
            agg.merge(parse_markdown_results(p), source=str(p))
        else:
            # Treat anything else as a bench log; the filename pattern guard
            # in parse_log_results will silently skip non-bench files.
            agg.merge(parse_log_results(p), source=str(p))
    return agg


# ---------------------------------------------------------------------------
# Quirks header rewrite
# ---------------------------------------------------------------------------

# Match a HostQuirksMeta field assignment line. The struct uses default-
# member-initializer syntax:
#
#     QuirkStatus some_flag = QuirkStatus::Speculative;
#
# (potentially with leading whitespace; we preserve it). Captures the flag
# name and the original status so we can pinpoint Speculative→Validated
# promotions exclusively.
_META_FIELD = re.compile(
    r"^(?P<indent>\s*)QuirkStatus\s+(?P<flag>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"QuirkStatus::(?P<tier>Validated|Speculative|LessonOnly)\s*;\s*$"
)


def promote_meta(src: str, confirmed: set[str]) -> tuple[str, list[str]]:
    """Return (new_source, promoted_flags) by flipping Speculative→Validated."""
    promoted: list[str] = []
    out_lines: list[str] = []
    in_meta = False
    for raw in src.splitlines(keepends=True):
        if not in_meta:
            if "struct HostQuirksMeta" in raw:
                in_meta = True
            out_lines.append(raw)
            continue

        if raw.lstrip().startswith("};"):
            in_meta = False
            out_lines.append(raw)
            continue

        m = _META_FIELD.match(raw.rstrip("\n"))
        if m and m.group("flag") in confirmed and m.group("tier") == "Speculative":
            new_line = (
                f"{m.group('indent')}QuirkStatus {m.group('flag')}"
                f" = QuirkStatus::Validated;\n"
            )
            out_lines.append(new_line)
            promoted.append(m.group("flag"))
        else:
            out_lines.append(raw)
    return "".join(out_lines), promoted


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

DEFAULT_QUIRKS_HEADER = Path("core/format/include/pulp/format/host_quirks.hpp")


def _make_diff(rel_path: str, before: str, after: str) -> str:
    return "".join(
        difflib.unified_diff(
            before.splitlines(keepends=True),
            after.splitlines(keepends=True),
            fromfile=f"a/{rel_path}",
            tofile=f"b/{rel_path}",
        )
    )


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument(
        "results",
        nargs="*",
        type=Path,
        help="Markdown result files and/or raw bench log files.",
    )
    p.add_argument(
        "--quirks-header",
        type=Path,
        default=DEFAULT_QUIRKS_HEADER,
        help=f"Path to host_quirks.hpp (default {DEFAULT_QUIRKS_HEADER}).",
    )
    p.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Write unified diff here (default: stdout).",
    )
    p.add_argument(
        "--in-place",
        action="store_true",
        help="Apply the promotion directly to --quirks-header instead of emitting a diff.",
    )
    p.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="Repo root used to compute the diff's relative path (default: cwd).",
    )
    p.add_argument(
        "--print-status",
        action="store_true",
        help="Just print the aggregated flag→status table; don't touch the header.",
    )
    args = p.parse_args(argv)

    if not args.results and not args.print_status:
        # Allow --print-status with no inputs (prints empty table); otherwise
        # require at least one result file.
        p.error("at least one result/log file required (or use --print-status)")

    agg = aggregate(args.results)

    if args.print_status:
        if not agg.flag_status:
            print("(no flags observed)")
            return 0
        width = max(len(f) for f in agg.flag_status)
        for f in sorted(agg.flag_status):
            print(f"{f:<{width}}  {agg.flag_status[f]:<13}  "
                  f"({len(agg.sources.get(f, []))} source(s))")
        return 0

    confirmed = {f for f, s in agg.flag_status.items() if s == STATUS_CONFIRMED}

    header_path: Path = args.quirks_header
    if not header_path.is_absolute():
        header_path = (args.repo_root / header_path).resolve()
    if not header_path.is_file():
        print(f"error: quirks header not found at {header_path}", file=sys.stderr)
        return 2

    before = header_path.read_text(encoding="utf-8")
    after, promoted = promote_meta(before, confirmed)

    if not promoted:
        print(
            f"no promotions: {len(confirmed)} flag(s) confirmed, "
            "but none currently sit at Speculative — nothing to do.",
            file=sys.stderr,
        )
        return 0

    print(f"promoting {len(promoted)} flag(s) Speculative → Validated:",
          file=sys.stderr)
    for f in promoted:
        print(f"  {f}", file=sys.stderr)

    if args.in_place:
        header_path.write_text(after, encoding="utf-8")
        return 0

    try:
        rel = os.path.relpath(header_path, args.repo_root)
    except ValueError:
        rel = str(header_path)
    diff = _make_diff(rel, before, after)
    if args.output:
        args.output.write_text(diff, encoding="utf-8")
    else:
        sys.stdout.write(diff)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
