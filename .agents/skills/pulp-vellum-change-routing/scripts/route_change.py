#!/usr/bin/env python3
"""Route changed paths through Pulp's exact Vellum ownership projection."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Iterable

from routing_evidence import RoutingError, load_projection, route_changes


DEFAULT_PROJECTION = Path(".github/vellum-ownership.json")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", help="exact repository-relative changed paths")
    parser.add_argument("--projection", type=Path, default=DEFAULT_PROJECTION)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--change", action="append", default=[], metavar="REPOSITORY:PATH")
    parser.add_argument("--claimed-owner")
    parser.add_argument("--json", action="store_true", dest="json_output")
    return parser


def _split_change(value: str) -> tuple[str, str]:
    repository, separator, path = value.partition(":")
    if not separator or not repository or not path:
        raise RoutingError("--change must use REPOSITORY:PATH")
    return repository, path


def main(argv: Iterable[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        projection, expansion = load_projection(args.projection, require_expansion=True)
        assert expansion is not None
        changes = [(args.repository, path) for path in args.paths]
        changes.extend(_split_change(value) for value in args.change)
        result = route_changes(
            projection, expansion, changes, claimed_owner=args.claimed_owner
        )
        if args.json_output:
            print(json.dumps(result, sort_keys=True))
        else:
            print(f"decision: {result['decision']}")
            for row in result["changes"]:
                print(f"{row['repository']}:{row['path']} -> {row['owner']} ({row['route_kind']})")
        return 0
    except RoutingError as error:
        print(f"pulp-vellum-route: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
