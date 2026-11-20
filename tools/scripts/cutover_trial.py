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
does not hide the rest. Repair writes must resolve inside that scratch tree.
Relative file includes are resolved from their CMake source directory. Bare
module names depend on CMake's effective scoped module search path; without
that evidence they remain unresolved. Ambiguous statements are also left
untouched, never guessed to be expected cutover deletions.
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
TOOL_ERROR = re.compile(r"^(?:ccache|ninja|cmake): error:\s*(.*)")


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
            continue
        tool_error = TOOL_ERROR.match(line)
        if tool_error:
            current["kind"] = "tool-error"
            current["detail"] = tool_error.group(1)
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


# ---- cutover edits ----------------------------------------------------------
# A real cutover edits the CMake files that name deleted paths. The trial
# derives those edits from configure's own errors, in the scratch checkout
# only: each error names the statement that failed, and that statement is
# commented out. Every edit is recorded so the report says what a cutover
# must change, and why.

ERROR_HEAD = re.compile(r"^CMake Error at (\S+?):(\d+) \((\w+)\):", re.M)
CALL_STACK = re.compile(r"Call Stack \(most recent call first\):\n\s+(\S+?):(\d+) \((\w+)\)")
CUTOVER_MARK = "# cutover-trial: "
# Statements whose failure means a path they name is gone.
FIXABLE = re.compile(
    r"(does not contain a CMakeLists.txt|include could not find|Cannot find source file|Unknown CMake command|"
    r"matches nothing in the checkout|No SOURCES given|does not exist|not found|"
    r"Cannot find|No such file|is not a directory|could not find)", re.I)


def parse_configure_errors(text: str) -> list[dict]:
    errors = []
    heads = list(ERROR_HEAD.finditer(text))
    for index, head in enumerate(heads):
        end = heads[index + 1].start() if index + 1 < len(heads) else len(text)
        body = text[head.end():end]
        stack = CALL_STACK.search(body)
        message = " ".join(line.strip() for line in body.split("Call Stack")[0].splitlines()
                           if line.strip())
        source_frames = re.findall(r"^\s+(\S*CMakeLists\.txt):\d+ \(\w+\)",
                                   body.split("Call Stack", 1)[-1], re.M)
        errors.append({"file": head.group(1), "line": int(head.group(2)), "command": head.group(3),
                       "message": message[:300],
                       "caller": (stack.group(1), int(stack.group(2))) if stack else None,
                       "source_file": source_frames[-1] if source_frames else None})
    return errors


def statement_span(lines: list[str], start: int) -> tuple[int, int] | None:
    """Find a whole call without interpreting delimiters inside CMake arguments.

    Refuse incomplete syntax or a second command on the closing line: a
    line-based edit must never consume an adjacent statement.
    """
    if not 0 <= start < len(lines):
        return None
    text = "\n".join(lines[start:])
    head = re.match(r"\s*[A-Za-z_][A-Za-z_0-9]*[ \t]*\(", lines[start])
    if not head:
        return None
    pos, depth, quoted = head.end(), 1, False
    argument_boundary = True
    while pos < len(text):
        char = text[pos]
        if char == "\\":
            argument_boundary = False
            pos += 2
            continue
        if char == '"':
            argument_boundary = False
            quoted = not quoted
            pos += 1
            continue
        if quoted:
            pos += 1
            continue
        bracket = re.match(r"#?\[(=*)\[", text[pos:])
        if bracket:
            if char == "[" and not argument_boundary:
                return None  # Embedded brackets belong to an unquoted argument.
            close = "]" + bracket.group(1) + "]"
            end = text.find(close, pos + bracket.end())
            if end < 0:
                return None
            argument_boundary = char == "#"
            pos = end + len(close)
            continue
        if char == "#":
            end = text.find("\n", pos)
            if end < 0:
                return None
            argument_boundary = True
            pos = end + 1
            continue
        argument_boundary = char.isspace() or char == "("
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                tail = text[pos + 1:].split("\n", 1)[0].strip()
                # A trailing bracket comment can span further lines; leave it
                # untouched rather than commenting out only its opening line.
                if tail and (not tail.startswith("#") or re.match(r"#\[=*\[", tail)):
                    return None
                return start, start + text[:pos].count("\n")
        pos += 1
    return None


def scratch_path(scratch: Path, path: str | Path) -> Path | None:
    """Resolve before checking containment, including existing symlink parents."""
    try:
        root = scratch.resolve()
        resolved = (root / path).resolve()
        resolved.relative_to(root)
    except (ValueError, OSError, RuntimeError):
        return None
    return resolved


def disable_statement(scratch: Path, rel: str, line: int, why: str) -> dict | None:
    path = scratch_path(scratch, rel)
    if path is None or not path.is_file():
        return None
    lines = path.read_text(encoding="utf-8").split("\n")
    span = statement_span(lines, line - 1)
    if span is None:
        return None
    start, end = span
    first = lines[start].strip()
    for index in range(start, end + 1):
        lines[index] = CUTOVER_MARK + lines[index]
    path.write_text("\n".join(lines), encoding="utf-8")
    return {"file": path.relative_to(scratch.resolve()).as_posix(), "line": line,
            "statement": first[:160], "why": why}


INCLUDE_MISSING = re.compile(r"include could not find requested file:\s*(\S+)")


def restore_deleted_module(scratch: Path, message: str, deleted: set[str],
                           source_dir: Path) -> dict | None:
    """When kept code includes a deleted CMake module, put that module back from
    the original tree so the trial can go on, and report it as a stranded
    dependency instead of dropping the targets that use it."""
    found = INCLUDE_MISSING.search(message)
    if not found:
        return None
    name = Path(found.group(1))
    # A bare name searches the effective CMAKE_MODULE_PATH, which can change
    # in project/function scope. Never infer that scope from cache or basenames.
    if not name.is_absolute() and not name.suffix:
        return None
    target = scratch_path(scratch, source_dir / name)
    if target is None or target.exists():
        return None
    rel = target.relative_to(scratch.resolve()).as_posix()
    if rel not in deleted or not rel.endswith(".cmake"):
        return None
    shown = run(["git", "-C", str(scratch), "show", f"HEAD:{rel}"])
    if shown.returncode:
        return None
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(shown.stdout, encoding="utf-8")
    return {"restored": rel}


def repair_configure(scratch: Path, configure, limit: int = 300,
                     deleted: set[str] | None = None) -> tuple[list[dict], str, bool]:
    """Configure, disabling the statement each fixable error names, until it
    passes, stops making progress, or the limit is reached. Returns the edits,
    the last configure log and whether configure passed."""
    edits: list[dict] = []
    seen: set[tuple] = set()
    text = ""
    for iteration in range(1, limit + 1):
        ok, text = configure()
        if ok:
            return edits, text, True
        progress = False
        for error in parse_configure_errors(text):
            if not (FIXABLE.search(error["message"]) or error["command"] != "message"
                    or error["caller"]):
                continue
            # An error raised inside a helper function belongs to its caller.
            target = error["caller"] if error["caller"] and error["file"].startswith(
                ("tools/cmake/", "cmake/")) else (error["file"], error["line"])
            if target in seen:
                continue
            # Functions execute in their caller's source directory, even when
            # defined in a different CMakeLists.txt.
            source_file = error["source_file"] or (
                error["file"] if error["file"].endswith("CMakeLists.txt") else None)
            source_path = scratch_path(scratch, source_file) if source_file else None
            source_dir = source_path.parent if source_path is not None else None
            restored = (restore_deleted_module(scratch, error["message"], deleted or set(),
                                               source_dir)
                        if source_dir is not None else None)
            if restored:
                restored.update({"class": "b-stranded-restored", "consumer": f"{target[0]}:{target[1]}",
                                 "why": error["message"], "iteration": iteration})
                edits.append(restored)
                progress = True
                continue  # Restoration is not a completed removal: the caller may need more.
            seen.add(target)
            missing_include = INCLUDE_MISSING.search(error["message"])
            if missing_include and not Path(missing_include.group(1)).suffix:
                continue  # Effective module search scope is unknown; retain the error.
            edit = disable_statement(scratch, target[0], target[1], error["message"])
            if edit:
                edit["iteration"] = iteration
                edit["class"] = classify_edit(error["message"], deleted or set(),
                                              scratch, source_dir)
                edits.append(edit)
                progress = True
        if not progress:
            return edits, text, False
    return edits, text, False


LINKS_MISSING = re.compile(r'Target "([^"]+)" links to: (\S+) but the target was not found')
CASCADE = re.compile(r"(No SOURCES given|Cannot specify .* not built by this project|"
                     r"could not find TARGET|non-existent target|Error evaluating generator|"
                     r"Can not find (target|test) to add|cannot create ALIAS)")


def classify_edit(message: str, deleted: set[str], scratch: Path | None = None,
                  source_dir: Path | None = None) -> str:
    """a-expected: the statement names a deleted path, so a cutover removes it too.
    b-stranded-consumer: a kept target links a target the deletion removed.
    cascade: a follow-on error from an earlier edit (a target that no longer exists).
    Anything else is listed as unclassified for review."""
    if LINKS_MISSING.search(message):
        return "b-stranded-consumer"
    if CASCADE.search(message):
        return "cascade"
    source = re.search(r'Cannot find source file:\s*(?:"([^"]+)"|(\S+))', message)
    directory = re.search(r'The source directory "([^"]+)" does not contain a CMakeLists', message)
    missing = ((source.group(1) or source.group(2)) if source else
               str(Path(directory.group(1)) / "CMakeLists.txt") if directory else None)
    # Unknown/glob diagnostics do not establish which actual deletion caused it.
    if missing and scratch is not None and source_dir is not None:
        resolved = scratch_path(scratch, source_dir / missing)
        if resolved is not None and resolved.relative_to(scratch.resolve()).as_posix() in deleted:
            return "a-expected"
    return "unclassified"


def deleted_data_still_referenced(scratch: Path, paths: list[str]) -> list[dict]:
    """Deleted non-source data files (fonts, headers) that kept files still
    name: deleted UI data that audio tests or packaging still use."""
    found = []
    for rel in paths:
        if not rel.startswith("external/") or rel.endswith((".cpp", ".mm")):
            continue
        done = run(["git", "-C", str(scratch), "grep", "-l", "-F", rel, "--", ".",
                    ":(exclude)docs", ":(exclude).github", ":(exclude).agents",
                    ":(exclude)tools/scripts/cutover_trial_inputs.json"])
        users = [u for u in done.stdout.split() if not u.startswith(("planning/",))]
        if users:
            found.append({"path": rel, "referenced_by": users[:10], "count": len(users)})
    return found


DATA_SUFFIXES = (".css", ".js", ".json", ".py", ".svg", ".wgsl", ".glsl", ".ttf", ".otf")


def restore_deleted_inputs(scratch: Path, paths: list[str]) -> list[dict]:
    """Restore deleted data inputs that surviving source files still require.

    A cutover deletion can remove a checked-in runtime prelude while its CMake
    producer remains. Restore only non-source data with an exact in-tree
    reference, and report the dependency so the row still gets a finding.
    """
    restored = []
    for rel in paths:
        if not rel.endswith(DATA_SUFFIXES):
            continue
        target = scratch_path(scratch, rel)
        if target is None or target.exists():
            continue
        needles = [rel]
        if "/js/" in rel:
            needles.append("js/" + rel.split("/js/", 1)[1])
        referenced_by = set()
        for needle in needles:
            users = run(["git", "-C", str(scratch), "grep", "-l", "-F", needle, "--", ".",
                         ":(exclude)docs", ":(exclude).github", ":(exclude).agents",
                         ":(exclude)tools/scripts/cutover_trial_inputs.json"])
            referenced_by.update(u for u in users.stdout.split() if not u.startswith("planning/"))
        referenced_by = sorted(referenced_by)
        if not referenced_by:
            continue
        shown = run(["git", "-C", str(scratch), "show", f"HEAD:{rel}"])
        if shown.returncode:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(shown.stdout, encoding="utf-8")
        restored.append({"restored": rel, "referenced_by": referenced_by[:10],
                         "count": len(referenced_by)})
    return restored


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
                 build_ran: bool, build_returncode: int | None = None,
                 edits: list[dict] | None = None,
                 data_refs: list[dict] | None = None) -> dict:
    rows = inputs["stranded_rows"]
    failures = parse_build_log(build_text) if build_ran else []
    configure_errors = [] if configure_ok else parse_configure_log(configure_text)
    grouping = group_failures(failures, rows, configure_errors)
    first_by_row: dict[str, dict] = {}
    for failure in failures:
        for row_id in failure.get("rows", []):
            first_by_row.setdefault(row_id, failure)
    row_results = []
    for row in rows:
        failure = first_by_row.get(row["id"])
        if failure:
            detail = failure["detail"] or "unknown build error"
            first_error = f"{failure['kind']}: {detail}"
            target = failure["target"]
        else:
            first_error = ("No target-specific failure was emitted for this row. The keep-going "
                           "build reached other failures and stopped without building this row.")
            target = None
        row_results.append({"id": row["id"], "capability": row["capability"],
                            "result": "fails", "target": target,
                            "first_error": first_error})
    return {
        "schema": SCHEMA,
        "export_id": inputs.get("export_id"),
        "paths": {"listed": len(inputs["delete_paths"]), "deleted": deleted,
                  "already_absent": missing},
        "configure": {"ok": configure_ok, "errors": configure_errors},
        "cutover_required_changes": [e for e in edits or [] if e.get("class") == "a-expected"],
        "stranded_dependencies": [e for e in edits or [] if e.get("class") == "b-stranded-restored"]
                                 + (data_refs or []),
        "stranded_consumers": [
            dict(e, consumer_target=LINKS_MISSING.search(e["why"]).group(1),
                 needs=LINKS_MISSING.search(e["why"]).group(2))
            for e in edits or [] if e.get("class") == "b-stranded-consumer"],
        "cascade_edits": sum(1 for e in edits or [] if e.get("class") == "cascade"),
        "unclassified_edits": [e for e in edits or [] if e.get("class") == "unclassified"],
        "deleted_data_still_referenced": data_refs or [],
        "build": {"ran": build_ran,
                  "ok": (build_returncode == 0
                         if build_returncode is not None else None),
                  "returncode": build_returncode,
                  "failed_targets": failures},
        "stranded_rows_total": len(rows),
        "rows_without_paths": [r["id"] for r in rows if not r["paths"]],
        "stranded_rows_hit": len(grouping["by_row"]),
        "row_results": row_results,
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
        target = scratch.resolve() / rel
        # Unlinking a symlink itself is safe; traversing a symlinked parent is not.
        if scratch_path(scratch, target.parent) is None:
            raise InputError(f"deletion path escapes scratch: {rel}")
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
    parser.add_argument("--no-repair", action="store_true",
                        help="report configure errors as they are; apply no cutover edits")
    parser.add_argument("--prepare", help="shell command run in the scratch checkout "
                        "after the deletion and before configure (dependency bootstrap)")
    parser.add_argument("--build-wrapper", type=Path,
                        help="wrapper used for the build command (default: governed-build.sh)")
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
            def configure_once() -> tuple[bool, str]:
                done = run(["cmake", "-S", str(args.scratch), "-B", str(build_dir),
                            "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                            "-DPULP_BUILD_EXAMPLES=OFF"])
                return done.returncode == 0, done.stdout + done.stderr

            if args.no_repair:
                configure_ok, configure_text = configure_once()
                edits = []
            else:
                edits, configure_text, configure_ok = repair_configure(
                    args.scratch, configure_once,
                    deleted=set(inputs["delete_paths"]) - set(missing))
            data_refs = deleted_data_still_referenced(args.scratch, inputs["delete_paths"])
            data_refs.extend(restore_deleted_inputs(args.scratch, inputs["delete_paths"]))
            (log_dir / "cutover-configure.log").write_text(configure_text, encoding="utf-8")
            build_text, ran, build_returncode = "", False, None
            if configure_ok:
                wrapper = args.build_wrapper or (args.scratch / "tools/ci/governed-build.sh")
                build_command = [str(wrapper), "cmake", "--build", str(build_dir), "--", "-k", "0"]
                ccache_tmp = build_dir / "cutover-ccache-tmp"
                ccache_tmp.mkdir(parents=True, exist_ok=True)
                build_env = os.environ.copy()
                build_env["CCACHE_TEMPDIR"] = str(ccache_tmp)
                built = run(build_command, cwd=args.scratch, env=build_env)
                build_text, ran = built.stdout + built.stderr, True
                build_returncode = built.returncode
                (log_dir / "cutover-build.log").write_text(build_text, encoding="utf-8")
            report = build_report(inputs, configure_text, build_text, deleted, missing,
                                  configure_ok, ran, build_returncode, edits, data_refs)
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
