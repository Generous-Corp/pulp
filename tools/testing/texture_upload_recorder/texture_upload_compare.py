#!/usr/bin/env python3
"""Compare two TextureUploadRecorder runs.

Each run is a directory holding the recorder's manifest.jsonl. Uploads are
compared as a multiset keyed on (width, height, depth, pixel_format, sha256),
because two editors can upload the same textures in a different order. The
report names hashes, sizes and formats only, never pixels.

An upload whose bytes could not be read (empty sha256) cannot be compared; it
is reported as such and makes the comparison fail, so an unreadable upload can
never pass as a match.

    texture_upload_compare.py RUN_A RUN_B [--json]

Exit status: 0 when every upload matches, 1 when any differs or cannot be
compared, 2 when a run cannot be read.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

Key = tuple[int, int, int, str, str]


class RunError(Exception):
    pass


def check_record(record: object, where: str) -> None:
    """Raise RunError unless `record` has the fields the comparison keys on."""
    if not isinstance(record, dict):
        raise RunError(f"{where}: not a JSON object")
    for field in ("width", "height", "depth"):
        if field not in record:
            raise RunError(f"{where}: missing {field}")
        value = record[field]
        # bool is an int subclass; true/false is not a size.
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise RunError(f"{where}: {field} must be a non-negative integer, not {value!r}")
    for field in ("pixel_format", "sha256"):
        if field not in record:
            raise RunError(f"{where}: missing {field}")
        if not isinstance(record[field], str):
            raise RunError(f"{where}: {field} must be a string, not {record[field]!r}")


def load_run(path: pathlib.Path) -> list[dict]:
    manifest = path / "manifest.jsonl"
    if not manifest.is_file():
        raise RunError(f"{manifest}: no manifest")
    records = []
    with manifest.open(encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise RunError(f"{manifest}:{number}: {error}") from error
            check_record(record, f"{manifest}:{number}")
            records.append(record)
    return records


def key(record: dict) -> Key:
    return (
        int(record["width"]),
        int(record["height"]),
        int(record["depth"]),
        str(record["pixel_format"]),
        str(record["sha256"]),
    )


def compare(a: list[dict], b: list[dict]) -> dict:
    a_keys = collections.Counter(key(r) for r in a if r["sha256"])
    b_keys = collections.Counter(key(r) for r in b if r["sha256"])
    matched = a_keys & b_keys

    def describe(counter: collections.Counter) -> list[dict]:
        return [
            {"width": k[0], "height": k[1], "depth": k[2], "pixel_format": k[3], "sha256": k[4], "count": n}
            for k, n in sorted(counter.items())
        ]

    return {
        "matched": sum(matched.values()),
        "only_in_a": describe(a_keys - b_keys),
        "only_in_b": describe(b_keys - a_keys),
        "unreadable_in_a": sum(1 for r in a if not r["sha256"]),
        "unreadable_in_b": sum(1 for r in b if not r["sha256"]),
    }


def identical(result: dict) -> bool:
    return not (
        result["only_in_a"] or result["only_in_b"] or result["unreadable_in_a"] or result["unreadable_in_b"]
    )


def render(result: dict) -> str:
    lines = [f"matched: {result['matched']}"]
    for side in ("a", "b"):
        for item in result[f"only_in_{side}"]:
            lines.append(
                f"only in {side.upper()}: {item['width']}x{item['height']}x{item['depth']} "
                f"{item['pixel_format']} sha256={item['sha256']} (x{item['count']})"
            )
        if result[f"unreadable_in_{side}"]:
            lines.append(f"unreadable in {side.upper()}: {result[f'unreadable_in_{side}']} upload(s), not comparable")
    lines.append("IDENTICAL" if identical(result) else "DIFFERENT")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("run_a", type=pathlib.Path)
    parser.add_argument("run_b", type=pathlib.Path)
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    args = parser.parse_args(argv)
    try:
        result = compare(load_run(args.run_a), load_run(args.run_b))
    except RunError as error:
        print(f"texture-upload-compare: {error}", file=sys.stderr)
        return 2
    result["identical"] = identical(result)
    print(json.dumps(result, indent=1) if args.json else render(result))
    return 0 if result["identical"] else 1


if __name__ == "__main__":
    sys.exit(main())
