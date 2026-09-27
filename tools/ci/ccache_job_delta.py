#!/usr/bin/env python3
"""Print the ccache calls one CI job made, from two `ccache --print-stats` snapshots.

`ccache --show-stats` reports counters for the life of the cache directory. On
the self-hosted gate VMs that directory is a host mount shared by every VM the
host boots, so the hit line a job prints ("Hits: 67142 / 67142") is the host's
history, not the job's: its denominator swings from a few thousand to tens of
thousands between jobs that compile the same ~10k translation units, and a job
that missed on every one of its own compiles still prints 99%+.

The per-job figure is the difference between a snapshot taken before the
build and one taken after it. This tool prints that difference as one line a
log reader (or a proxy script) can grep, and a machine-readable JSON line.

Usage:
    ccache --print-stats > before.txt   # before the Build step
    ...
    ccache --print-stats > after.txt    # after it
    python3 tools/ci/ccache_job_delta.py before.txt after.txt

Exit 0 when both snapshots parse; 2 when either is missing or unparseable, so
a caller can treat "no per-job line" as an instrument failure, never as a hit.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# `ccache --print-stats` emits tab-separated `key\tvalue` lines (ccache 4.x).
# The counters that describe a job's compile calls:
HIT_FIELDS = ("direct_cache_hit", "preprocessed_cache_hit")
MISS_FIELD = "cache_miss"
# Calls ccache saw but could not cache. ccache has no single counter for these;
# `--show-stats` sums the same categories into its "Uncacheable calls" line.
UNCACHEABLE_FIELDS = (
    "autoconf_test",
    "bad_compiler_arguments",
    "called_for_link",
    "called_for_preprocessing",
    "could_not_use_modules",
    "could_not_use_precompiled_header",
    "disabled",
    "multiple_source_files",
    "no_input_file",
    "output_to_stdout",
    "unsupported_code_directive",
    "unsupported_compiler_option",
    "unsupported_environment_variable",
    "unsupported_source_encoding",
    "unsupported_source_language",
)
FIELDS = HIT_FIELDS + (MISS_FIELD,) + UNCACHEABLE_FIELDS
# Older ccache spellings of the same counters.
ALIASES = {
    "cache_hit_direct": "direct_cache_hit",
    "cache_hit_preprocessed": "preprocessed_cache_hit",
}


def parse_print_stats(text: str) -> dict[str, int]:
    """`ccache --print-stats` output → {counter: value}. Unknown keys are kept;
    non-integer values are ignored."""
    out: dict[str, int] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("\t")
        if not sep:
            continue
        key = ALIASES.get(key.strip(), key.strip())
        try:
            out[key] = int(value.strip())
        except ValueError:
            continue
    return out


def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    """Per-job counters. A counter that went backwards (cache zeroed or
    replaced mid-job) is reported as the after value, never negative."""
    per_field: dict[str, int] = {}
    for field in FIELDS:
        a = after.get(field)
        if a is None:
            continue
        b = before.get(field, 0)
        per_field[field] = a - b if a >= b else a
    hits = sum(per_field.get(f, 0) for f in HIT_FIELDS)
    misses = per_field.get(MISS_FIELD, 0)
    return {
        "hits": hits,
        "misses": misses,
        "cacheable": hits + misses,
        "uncacheable": sum(per_field.get(f, 0) for f in UNCACHEABLE_FIELDS),
        "direct_hits": per_field.get("direct_cache_hit", 0),
        "preprocessed_hits": per_field.get("preprocessed_cache_hit", 0),
    }


def render(d: dict[str, int]) -> str:
    cacheable = d["cacheable"]
    rate = (100.0 * d["hits"] / cacheable) if cacheable else 0.0
    return (f"ccache per-job: cacheable {cacheable}, hits {d['hits']} ({rate:.2f}%), "
            f"misses {d['misses']}, uncacheable {d['uncacheable']}")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        return 2
    snaps = []
    for path in argv[1:]:
        try:
            text = Path(path).read_text(encoding="utf-8")
        except OSError as exc:
            print(f"ccache per-job: snapshot unreadable: {exc}", file=sys.stderr)
            return 2
        parsed = parse_print_stats(text)
        if "cache_miss" not in parsed and "direct_cache_hit" not in parsed:
            print(f"ccache per-job: snapshot has no ccache counters: {path}", file=sys.stderr)
            return 2
        snaps.append(parsed)
    d = delta(snaps[0], snaps[1])
    print(render(d))
    print("ccache per-job json: " + json.dumps(d, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
