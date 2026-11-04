"""Command line entry point for the pure authored identity package."""

import argparse
import json
import sys
from pathlib import Path

from .identity import IdentityError, diff, fingerprint, parse_document, PROFILE


def _error(code, path=()):
    return {"schema": "pulp.graph.diff.v1", "domain": "semantic", "profile": PROFILE,
            "status": "error", "before": None, "after": None, "equal": None,
            "changes": [], "diagnostics": [{"code": code, "path": list(path),
                                                 "suggested_fix": "Provide a valid explicit authored I/O and Gain document."}]}


class CliParser(argparse.ArgumentParser):
    def error(self, message):
        print(json.dumps(_error("invalid_usage"), sort_keys=True, separators=(",", ":")))
        raise SystemExit(2)


def main(argv=None):
    parser = CliParser(prog="pulp-graph-identity")
    subs = parser.add_subparsers(dest="command", required=True)
    for name in ("fingerprint", "diff"):
        command = subs.add_parser(name)
        command.add_argument("before")
        if name == "diff":
            command.add_argument("after")
        command.add_argument("--domain", default="semantic")
        command.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.domain != "semantic":
            raise IdentityError("unsupported_domain")
        before = parse_document(Path(args.before).read_bytes())
        if args.command == "diff":
            result = diff(before, parse_document(Path(args.after).read_bytes()))
            status = 0 if result["equal"] else 1
        else:
            info = fingerprint(before)
            result = {"schema": "pulp.graph.diff.v1", "domain": "semantic", "profile": PROFILE,
                      "status": "fingerprinted", "before": info["digest"], "after": None,
                      "equal": None, "changes": [], "diagnostics": [],
                      "canonical_payload": info["payload"].decode("utf-8")}
            status = 0
    except (IdentityError, OSError) as exc:
        code = exc.code if isinstance(exc, IdentityError) else "file_io_error"
        path = exc.path if isinstance(exc, IdentityError) else []
        result = _error(code, path)
        status = 2
    print(json.dumps(result, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    return status


if __name__ == "__main__":
    sys.exit(main())
