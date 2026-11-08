#!/usr/bin/env python3
"""Check a dependency request log against what a configuration may ask for.

The log is the JSON-lines file written by dependency_request_recorder.cmake:
one {"method": ..., "name": ...} object per find_package() or
FetchContent_MakeAvailable() request. This script prints every request, then
fails when a forbidden name was requested or a required one was not.

  dependency_request_check.py --log build/dependency-requests.jsonl \\
      --forbid 'skia' --forbid 'dawn*' --forbid 'vellum*' --forbid 'rmlui*'

  # The control: the same instrument on a configuration that must need it.
  dependency_request_check.py --log build-gpu/dependency-requests.jsonl \\
      --require 'skia'

Patterns are shell globs matched case-insensitively against the whole
requested name.

Exit codes: 0 the log passes; 1 a forbidden name was requested or a required
one is missing; 2 the log is missing, unreadable, malformed or empty. An empty
log is an error, not a pass: every real configure requests something, so an
empty log means the recorder never ran, and absence measured with a broken
instrument proves nothing.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Request:
    method: str
    name: str


class LogError(Exception):
    pass


def read_log(path: Path) -> list[Request]:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise LogError(f"cannot read {path}: {exc}") from exc
    requests: list[Request] = []
    for number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            entry = json.loads(line)
        except json.JSONDecodeError as exc:
            raise LogError(f"{path}:{number}: not JSON: {exc}") from exc
        method = entry.get("method") if isinstance(entry, dict) else None
        name = entry.get("name") if isinstance(entry, dict) else None
        if not isinstance(method, str) or not isinstance(name, str) or not name:
            raise LogError(f"{path}:{number}: needs a string method and a non-empty name")
        requests.append(Request(method=method, name=name))
    if not requests:
        raise LogError(
            f"{path} records no requests; the recorder did not run, so nothing can be "
            "concluded about absence"
        )
    return requests


def matches(name: str, pattern: str) -> bool:
    return fnmatch.fnmatchcase(name.casefold(), pattern.casefold())


def evaluate(
    requests: list[Request], forbid: list[str], require: list[str]
) -> list[str]:
    problems: list[str] = []
    for pattern in forbid:
        hits = sorted({r.name for r in requests if matches(r.name, pattern)})
        if hits:
            problems.append(f"forbidden '{pattern}' was requested: {', '.join(hits)}")
    for pattern in require:
        if not any(matches(r.name, pattern) for r in requests):
            problems.append(f"required '{pattern}' was never requested")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--forbid", action="append", default=[], metavar="GLOB")
    parser.add_argument("--require", action="append", default=[], metavar="GLOB")
    args = parser.parse_args(argv)

    if not args.forbid and not args.require:
        parser.error("give at least one --forbid or --require")

    try:
        requests = read_log(args.log)
    except LogError as exc:
        print(f"dependency-requests: ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"dependency-requests: {len(requests)} request(s) in {args.log}")
    for request in requests:
        print(f"  {request.method:<34} {request.name}")

    problems = evaluate(requests, args.forbid, args.require)
    if problems:
        for problem in problems:
            print(f"dependency-requests: FAIL: {problem}", file=sys.stderr)
        return 1
    checked = ", ".join(
        [f"forbid {p}" for p in args.forbid] + [f"require {p}" for p in args.require]
    )
    print(f"dependency-requests: OK ({checked})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
