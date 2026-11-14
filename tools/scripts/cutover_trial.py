#!/usr/bin/env python3
"""Report what stops building when the cutover deletion set is absent.

Builds Pulp in a scratch checkout with every path in the deletion set removed
and writes a JSON report: each target that failed, the first missing header or
unresolved symbol for it, and the capability-ledger rows it belongs to. It is
a report, not a gate: the exit status is 0 whatever the build did, and 2 only
when its inputs cannot be read.

    cutover_trial.py --scratch <dir> --report <report.json> [--ref origin/main]
    cutover_trial.py --analyze-log build.log --report <report.json>

The scratch checkout is a detached git worktree created by this script. It
must be new and must not be the caller's checkout. The build runs through
tools/ci/governed-build.sh with Ninja's keep-going flag, so one broken target
does not hide the rest.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = "pulp.cutover-trial-report.v1"
INPUTS = Path(__file__).with_name("cutover_trial_inputs.json")


class InputError(Exception):
    pass


def load_inputs(path: Path = INPUTS) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise InputError("inputs root must be an object")
        paths = data["delete_paths"]
        rows = data["stranded_rows"]
    except (OSError, ValueError, KeyError, InputError) as error:
        raise InputError(f"cannot read inputs {path}: {error}") from error
    if (not isinstance(paths, list) or not paths
            or not all(isinstance(p, str) and p and ".." not in p.split("/")
                       and not p.startswith("/") for p in paths)):
        raise InputError("delete_paths must be a non-empty list of relative paths")
    if not isinstance(rows, list) or not all(
            isinstance(r, dict) and isinstance(r.get("id"), str)
            and isinstance(r.get("paths"), list)
            and all(isinstance(p, str) and p for p in r["paths"])
            for r in rows):
        raise InputError("stranded_rows is malformed")
    if data.get("ledger_stranded_count") != len(rows):
        raise InputError("stranded_rows does not match ledger_stranded_count")
    return data


# ---- build log parsing ----------------------------------------------------

FAILED = re.compile(r"^FAILED: (?:\[code=\d+\] )?(\S+)")
MISSING_HEADER = re.compile(r"fatal error: ['\"<]([^'\">]+)['\">] file not found")
SOURCE_ERROR = re.compile(r"^(\S+\.(?:c|cc|cpp|cxx|m|mm|h|hpp)):\d+:\d+: (?:fatal )?error: (.*)")
UNDEFINED = re.compile(r'^\s+"([^"]+)", referenced from:')
CMAKE_ERROR = re.compile(r"^CMake Error at (\S+?):(\d+)")
CMAKE_MISSING = re.compile(
    r"(Cannot find source file:|add_subdirectory given source|"
    r"include could not find|Could not find a package|No SOURCES given)\s*(.*)")


def parse_build_log(text: str) -> list[dict]:
    """One entry per failed ninja target, with its first error."""
    failures: list[dict] = []
    current: dict | None = None
    for line in text.splitlines():
        match = FAILED.match(line)
        if match:
            current = {"target": match.group(1), "kind": None, "detail": None,
                       "source": None}
            failures.append(current)
            continue
        if current is None or current["kind"] is not None:
            continue
        header = MISSING_HEADER.search(line)
        if header:
            current["kind"] = "missing-header"
            current["detail"] = header.group(1)
            source = SOURCE_ERROR.match(line) or re.match(r"^(\S+?):\d+:\d+:", line)
            current["source"] = source.group(1) if source else None
            continue
        symbol = UNDEFINED.match(line)
        if symbol:
            current["kind"] = "undefined-symbol"
            current["detail"] = symbol.group(1)
            continue
        error = SOURCE_ERROR.match(line)
        if error:
            current["kind"] = "compile-error"
            current["detail"] = error.group(2)
            current["source"] = error.group(1)
    for failure in failures:
        if failure["kind"] is None:
            failure["kind"] = "unknown"
    return failures


def parse_configure_log(text: str) -> list[dict]:
    errors: list[dict] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = CMAKE_ERROR.match(line)
        if not match:
            continue
        detail = ""
        for offset, follower in enumerate(lines[index + 1:index + 6]):
            found = CMAKE_MISSING.search(follower)
            if found:
                detail = f"{found.group(1)} {found.group(2)}".strip()
                if not found.group(2).strip():
                    for candidate in lines[index + offset + 2:index + 7]:
                        if candidate.strip():
                            detail = f"{detail} {candidate.strip()}"
                            break
                break
            if follower.strip() and not detail:
                detail = follower.strip()
        errors.append({"where": f"{match.group(1)}:{match.group(2)}", "detail": detail})
    return errors


# ---- grouping -------------------------------------------------------------

def path_matches(row_path: str, candidate: str) -> bool:
    """Whether a ledger path names `candidate`: a file, a directory prefix, a
    glob, or the tail of an include path such as pulp/view/motion.hpp."""
    if not candidate:
        return False
    candidate = candidate.lstrip("./")
    if row_path.endswith("/"):
        return candidate.startswith(row_path) or f"/{row_path}" in f"/{candidate}"
    if any(ch in row_path for ch in "*?["):
        return fnmatch.fnmatch(candidate, row_path)
    return (candidate == row_path or candidate.endswith("/" + row_path)
            or row_path.endswith("/" + candidate))


def group_failures(failures: list[dict], rows: list[dict],
                   configure_errors: list[dict]) -> dict:
    by_row: dict[str, list[str]] = {r["id"]: [] for r in rows}
    ungrouped: list[str] = []
    for failure in failures:
        keys = [failure.get("source"), failure.get("target")]
        if failure["kind"] == "missing-header":
            keys.append(failure["detail"])
        hit = sorted({r["id"] for r in rows for p in r["paths"]
                      for k in keys if k and path_matches(p, k)})
        failure["rows"] = hit
        label = failure["target"]
        if not hit:
            ungrouped.append(label)
        for row_id in hit:
            by_row[row_id].append(label)
    for error in configure_errors:
        keys = [error["where"].rsplit(":", 1)[0], error["detail"]]
        error["rows"] = sorted({r["id"] for r in rows for p in r["paths"]
                                for k in keys if k and any(
                                    path_matches(p, t) for t in re.findall(r"[\w./+-]+", k))})
        for row_id in error["rows"]:
            by_row[row_id].append(f"configure:{error['where']}")
    return {"by_row": {k: v for k, v in by_row.items() if v},
            "ungrouped_targets": ungrouped}


def build_report(inputs: dict, configure_text: str, build_text: str,
                 deleted: int, missing: list[str], configure_ok: bool,
                 build_ran: bool, build_returncode: int | None = None) -> dict:
    rows = inputs["stranded_rows"]
    failures = parse_build_log(build_text) if build_ran else []
    configure_errors = [] if configure_ok else parse_configure_log(configure_text)
    grouping = group_failures(failures, rows, configure_errors)
    return {
        "schema": SCHEMA,
        "export_id": inputs.get("export_id"),
        "paths": {"listed": len(inputs["delete_paths"]), "deleted": deleted,
                  "already_absent": missing},
        "configure": {"ok": configure_ok, "errors": configure_errors},
        "build": {"ran": build_ran,
                  "ok": (build_returncode == 0
                         if build_returncode is not None else None),
                  "returncode": build_returncode,
                  "failed_targets": failures},
        "stranded_rows_total": len(rows),
        "rows_without_paths": [r["id"] for r in rows if not r["paths"]],
        "stranded_rows_hit": len(grouping["by_row"]),
        **grouping,
    }


# ---- scratch checkout and build -------------------------------------------

def run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(args, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False, **kwargs)


def make_scratch(source: Path, scratch: Path, ref: str) -> None:
    if scratch.exists():
        raise InputError(f"scratch path already exists: {scratch}")
    if scratch.resolve() == source.resolve() or source.resolve() in scratch.resolve().parents:
        raise InputError("scratch must be outside the source checkout")
    if str(scratch.resolve()).startswith(("/tmp/", "/private/tmp/", "/var/folders/")):
        raise InputError("scratch must be under the worktree root, not a temporary directory")
    done = run(["git", "-C", str(source), "worktree", "add", "--detach", str(scratch), ref])
    if done.returncode:
        raise InputError(f"git worktree add failed: {done.stderr.strip()}")


def delete_paths(scratch: Path, paths: list[str]) -> tuple[int, list[str]]:
    deleted, missing = 0, []
    for rel in paths:
        target = scratch / rel
        if target.is_file() or target.is_symlink():
            target.unlink()
            deleted += 1
        else:
            missing.append(rel)
    return deleted, missing


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--scratch", type=Path)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--ref", default="origin/main")
    parser.add_argument("--inputs", type=Path, default=INPUTS)
    parser.add_argument("--log-dir", type=Path)
    parser.add_argument("--prepare", help="shell command run in the scratch checkout "
                        "after the deletion and before configure (dependency bootstrap)")
    parser.add_argument("--analyze-log", type=Path,
                        help="parse an existing build log instead of building")
    args = parser.parse_args(argv)
    try:
        inputs = load_inputs(args.inputs)
        if args.analyze_log:
            text = args.analyze_log.read_text(encoding="utf-8", errors="replace")
            report = build_report(inputs, "", text, 0, [], True, True)
        else:
            if args.scratch is None:
                raise InputError("--scratch is required unless --analyze-log is given")
            make_scratch(args.source, args.scratch, args.ref)
            deleted, missing = delete_paths(args.scratch, inputs["delete_paths"])
            log_dir = args.log_dir or args.report.parent
            log_dir.mkdir(parents=True, exist_ok=True)
            build_dir = args.scratch / "build"
            if args.prepare:
                prepared = subprocess.run(args.prepare, shell=True, cwd=args.scratch,
                                          text=True, encoding="utf-8", errors="replace", capture_output=True, check=False)
                (log_dir / "cutover-prepare.log").write_text(
                    prepared.stdout + prepared.stderr, encoding="utf-8")
            configure = run(["cmake", "-S", str(args.scratch), "-B", str(build_dir),
                             "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
            configure_text = configure.stdout + configure.stderr
            (log_dir / "cutover-configure.log").write_text(configure_text, encoding="utf-8")
            build_text, ran, build_returncode = "", False, None
            if configure.returncode == 0:
                governed = args.scratch / "tools/ci/governed-build.sh"
                built = run([str(governed), "cmake", "--build", str(build_dir), "--",
                             "-k", "0"], cwd=args.scratch)
                build_text, ran, build_returncode = (built.stdout + built.stderr,
                                                     True, built.returncode)
                (log_dir / "cutover-build.log").write_text(build_text, encoding="utf-8")
            report = build_report(inputs, configure_text, build_text, deleted, missing,
                                  configure.returncode == 0, ran, build_returncode)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    except (InputError, OSError) as error:
        print(f"cutover-trial: {error}", file=sys.stderr)
        return 2
    print(f"cutover-trial: configure_ok={report['configure']['ok']} "
          f"failed_targets={len(report['build']['failed_targets'])} "
          f"rows_hit={report['stranded_rows_hit']}/{report['stranded_rows_total']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
