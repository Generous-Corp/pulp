#!/usr/bin/env python3
"""Record what a `macos` job tested, per test, for offline reuse replay.

A reuse policy ("this merge group may skip test T because an earlier run
already proved it") can only be scored against history that says, per test,
what ran, how it ended, how many attempts it took, and which exact bytes it
executed. ctest's JUnit report holds the first part but its artifact expires
after 7 days, and per-binary digests exist only where a receipt was issued.
This tool writes that history once per job, from ctest's own result files,
in the neutral record the replay harness reads:

    tests.jsonl      one record per test:
                     run_id, run_attempt, run_kind (pr_head|merge_group|push|
                     dispatch), pr, head_sha, base_sha, merge_tree, suite
                     (full|pr-fast|pr-affected), test_id, executable, outcome
                     (pass|fail|timeout|skipped|notrun), attempts, duration_s,
                     runner_image, output_key
    identity.json    per executable that ran: sha256 (the same digest
                     `protected_merge_receipt.artifact_identity` puts in a
                     receipt) and the runtime closure it loads (transitive
                     Mach-O dylib/framework linkage, a script's interpreter),
                     each member with its own sha256
    job.json         the run context, per-suite counts, the runner image
                     fingerprint, the declared script-input list's blob id,
                     and the byte size of each file

It records and decides nothing. It never runs a test, and its exit status is
read only by the workflow step that announces a missing record.

Sources, never log scraping:
- outcome and duration: the JUnit report ctest writes (`--output-junit`);
- attempts: ctest's own `Testing/Temporary/LastTest.log`, one block per
  execution. Any later ctest call in the same build directory (even
  `--show-only`) rewrites that file, so the workflow copies it next to the
  JUnit report right after the run. Without a copy, `attempts` is null for a
  suite that ran with `--repeat`; a suite without `--repeat` runs each test
  once;
- executable: the ctest inventory (`ctest --show-only=json-v1`);
- output_key: the merge-group per-test receipt key
  (`test_receipts_shadow.py --keys-out`), where that shadow ran; else null.

    reuse_record.py write --out-dir D [--build-dir B --source-root S]
        [--suite NAME=JUNIT_FILE_OR_DIR[,attempts=LOG][,repeat]]...
        [--selected-json I] [--identity-json F] [--test-keys F] [--not-before-epoch T]
        [--build-outcome O] [--context KEY=VALUE]... [--no-suite-reason TEXT]

Context (run, kind, pr, shas) comes from the Actions environment and the
event variables the workflow passes (PR_NUMBER, PR_HEAD_SHA, PR_BASE_SHA,
MERGE_GROUP_HEAD_REF, MERGE_GROUP_BASE_SHA).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "scripts"))

SCHEMA = "pulp-reuse-record/v1"
TITLE = "reuse-record"
RECORD_FIELDS = ("run_id", "run_attempt", "run_kind", "pr", "head_sha", "base_sha", "merge_tree",
                 "suite", "test_id", "executable", "outcome", "attempts", "duration_s",
                 "runner_image", "output_key")
OUTCOMES = ("pass", "fail", "timeout", "skipped", "notrun")
RUN_KINDS = {"pull_request": "pr_head", "merge_group": "merge_group", "push": "push",
             "workflow_dispatch": "dispatch"}
SCRIPT_INPUTS_LIST = "test/ctest_script_inputs.json"
SYSTEM_PREFIXES = ("/usr/lib/", "/System/")


# --------------------------------------------------------------------------
# Results


def junit_cases(path: Path) -> list[dict]:
    """[{test_id, outcome, duration_s}] in report order, from a ctest JUnit file."""
    out = []
    for case in ET.parse(path).getroot().iter("testcase"):
        name = case.get("name") or ""
        if not name:
            continue
        status = case.get("status") or ""
        failure = next((c for c in case if c.tag in ("failure", "error")), None)
        skipped = next((c for c in case if c.tag == "skipped"), None)
        if failure is not None or status == "fail":
            message = (failure.get("message") if failure is not None else "") or ""
            outcome = "timeout" if message.startswith("Timeout") else "fail"
        elif skipped is not None and (skipped.get("message") or "").startswith("SKIP_"):
            outcome = "skipped"  # the test ran and asked to be counted as skipped
        elif status in ("notrun", "disabled") or skipped is not None:
            outcome = "notrun"
        else:
            outcome = "pass"
        try:
            duration = round(float(case.get("time") or 0.0), 3)
        except ValueError:
            duration = None
        out.append({"test_id": name, "outcome": outcome, "duration_s": duration})
    return out


_TESTING = re.compile(r"^\d+/\d+ Testing: (.*)$")
_TEST = re.compile(r"^\d+/\d+ Test: (.*)$")


def attempts_from_log(path: Path) -> dict[str, int]:
    """Executions per test from ctest's LastTest.log (one Testing/Test pair each)."""
    counts: dict[str, int] = {}
    pending = None
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = _TEST.match(line)
            if m and pending == m.group(1):
                counts[pending] = counts.get(pending, 0) + 1
            t = _TESTING.match(line)
            pending = t.group(1) if t else None
    return counts


def attempts_for(outcome: str, name: str, log_counts: dict[str, int] | None, repeat: bool) -> int | None:
    if outcome == "notrun":
        return 0
    if log_counts is not None:
        return log_counts.get(name)
    return None if repeat else 1


# --------------------------------------------------------------------------
# Runtime closure (Mach-O load commands, parsed without otool)

LC_REQ_DYLD = 0x80000000
DYLIB_COMMANDS = {0xC, 0x18 | LC_REQ_DYLD, 0x1F | LC_REQ_DYLD, 0x20, 0x23 | LC_REQ_DYLD}
LC_RPATH = 0x1C | LC_REQ_DYLD
# Keyed by the magic read little-endian: MH_MAGIC means a little-endian file.
THIN_MAGICS = {0xFEEDFACE: ("<", 28), 0xCEFAEDFE: (">", 28),
               0xFEEDFACF: ("<", 32), 0xCFFAEDFE: (">", 32)}


def _cstr(blob: bytes, start: int) -> str:
    end = blob.find(b"\0", start)
    return blob[start:end if end >= 0 else len(blob)].decode("utf-8", "replace")


def _thin_load_commands(fh, offset: int) -> tuple[list[str], list[str]] | None:
    fh.seek(offset)
    head = fh.read(32)
    if len(head) < 28:
        return None
    magic = struct.unpack_from("<I", head, 0)[0]
    if magic not in THIN_MAGICS:
        return None
    endian, header = THIN_MAGICS[magic]
    ncmds, sizeofcmds = struct.unpack_from(endian + "II", head, 16)
    fh.seek(offset + header)
    data = fh.read(min(sizeofcmds, 16 * 1024 * 1024))
    pos = 0
    dylibs, rpaths = [], []
    for _ in range(ncmds):
        if pos + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack_from(endian + "II", data, pos)
        if cmdsize < 8:
            break
        body = data[pos:pos + cmdsize]
        if (cmd in DYLIB_COMMANDS or cmd == LC_RPATH) and len(body) >= 12:
            name_off = struct.unpack_from(endian + "I", body, 8)[0]
            (dylibs if cmd != LC_RPATH else rpaths).append(_cstr(body, name_off))
        pos += cmdsize
    return dylibs, rpaths


def macho_load_commands(path: Path) -> tuple[list[str], list[str]] | None:
    """(dylib install names, rpaths) across every slice, or None if not Mach-O.
    Reads only the headers and load commands, never the whole file."""
    try:
        with path.open("rb") as fh:
            head = fh.read(8)
            if len(head) < 8:
                return None
            magic = struct.unpack_from(">I", head, 0)[0]
            if magic not in (0xCAFEBABE, 0xCAFEBABF):
                return _thin_load_commands(fh, 0)
            nfat = struct.unpack_from(">I", head, 4)[0]
            if nfat > 32:  # a Java class file shares this magic
                return None
            width = 20 if magic == 0xCAFEBABE else 32
            table = fh.read(nfat * width)
            dylibs, rpaths = [], []
            for i in range(nfat):
                if len(table) < (i + 1) * width:
                    break
                if magic == 0xCAFEBABE:
                    off = struct.unpack_from(">I", table, i * width + 8)[0]
                else:
                    off = struct.unpack_from(">Q", table, i * width + 8)[0]
                got = _thin_load_commands(fh, off)
                if got:
                    dylibs += [d for d in got[0] if d not in dylibs]
                    rpaths += [r for r in got[1] if r not in rpaths]
            return dylibs, rpaths
    except (OSError, struct.error):
        return None


def _expand(name: str, loader: Path, main_exe: Path) -> str:
    if name.startswith("@executable_path/"):
        return str(main_exe.parent / name[len("@executable_path/"):])
    if name.startswith("@loader_path/"):
        return str(loader.parent / name[len("@loader_path/"):])
    return name


def _shebang(path: Path) -> str | None:
    try:
        with path.open("rb") as fh:
            head = fh.read(256)
    except OSError:
        return None
    if not head.startswith(b"#!"):
        return None
    words = head[2:].split(b"\n", 1)[0].decode("utf-8", "replace").split()
    if not words:
        return None
    if os.path.basename(words[0]) == "env" and len(words) > 1:
        return shutil.which(words[1]) or f"unresolved:{words[1]}"
    return words[0]


def runtime_closure(exe: Path, max_images: int = 256) -> list[str]:
    """Every image `exe` loads through its load commands, transitively, plus a
    script's interpreter. System images (dyld shared cache) are named with a
    `system:` prefix and not read; names that resolve to no file keep an
    `unresolved:` prefix. Plugins opened with dlopen at run time are not
    visible here."""
    main_exe = exe
    seen: set[str] = set()
    out: set[str] = set()
    queue: list[tuple[Path, list[str]]] = [(exe, [])]
    interp = _shebang(exe)
    if interp:
        if interp.startswith("unresolved:"):
            out.add(interp)
        else:
            queue.append((Path(interp), []))
            out.add(str(Path(interp).resolve()) if Path(interp).exists() else f"unresolved:{interp}")
    while queue and len(seen) < max_images:
        image, inherited_rpaths = queue.pop()
        key = str(image)
        if key in seen:
            continue
        seen.add(key)
        parsed = macho_load_commands(image)
        if not parsed:
            continue
        dylibs, rpaths = parsed
        stack = [_expand(r, image, main_exe) for r in rpaths] + inherited_rpaths
        for name in dylibs:
            candidates = ([str(Path(r) / name[len("@rpath/"):]) for r in stack]
                          if name.startswith("@rpath/") else [_expand(name, image, main_exe)])
            resolved = next((c for c in candidates if os.path.isfile(c)), None)
            if resolved is None:
                if any(c.startswith(SYSTEM_PREFIXES) for c in candidates):
                    out.add(f"system:{candidates[0]}")
                else:
                    out.add(f"unresolved:{name}")
                continue
            real = os.path.realpath(resolved)
            if real.startswith(SYSTEM_PREFIXES):
                out.add(f"system:{real}")
                continue
            out.add(real)
            queue.append((Path(real), stack))
    return sorted(out)


# --------------------------------------------------------------------------
# Executable identity


def _normalise(path: str, roots: list[tuple[str, str]]) -> str:
    for real, token in roots:
        if path == real or path.startswith(real + os.sep):
            return token + path[len(real):]
    return path


def _roots(build_dir: Path, source_root: Path) -> list[tuple[str, str]]:
    return sorted({(os.path.realpath(build_dir), "<build>"), (str(build_dir), "<build>"),
                   (os.path.realpath(source_root), "<src>"), (str(source_root), "<src>")},
                  key=lambda r: (-len(r[0]), r[1] != "<build>"))


def executable_identity(build_dir: Path, source_root: Path, tests: list[dict],
                        seed: dict[str, str]) -> tuple[dict, dict[str, str]]:
    """Identity of every executable the given inventory entries name.

    The executable digest is exactly `protected_merge_receipt`'s
    (`artifact_identity` over the one test), so these records compare path by
    path with a receipt and with binary_identity_shadow's `our-identity.json`,
    whose hashes `seed` supplies so nothing is read twice.
    Returns (identity document, test name -> executable key)."""
    import protected_merge_receipt as pmr

    build_dir = build_dir.resolve()  # the receipt's paths are relative to the resolved directory
    roots = _roots(build_dir, source_root)
    hashes: dict[str, str] = dict(seed)

    def sha(path: str) -> str:
        if path not in hashes:
            hashes[path] = pmr._sha256_file(Path(path))
        return hashes[path]

    executables: dict[str, dict] = {}
    by_test: dict[str, str] = {}
    by_command0: dict[str, str] = {}
    closure_files: dict[str, str | None] = {}
    errors = 0
    for test in tests:
        name, cmd = test.get("name"), test.get("command")
        if not name or not isinstance(cmd, list) or not cmd:
            continue
        if cmd[0] in by_command0:
            by_test[name] = by_command0[cmd[0]]
            continue
        try:
            ident = pmr.artifact_identity(build_dir, {"tests": [test]})
            rel = ident["files"][0]["path"]
            real = os.path.realpath(build_dir / rel)
            hashes.setdefault(real, ident["files"][0]["sha256"])
        except pmr.ReceiptError as exc:
            key = f"unresolved:{cmd[0]}"
            executables[key] = {"sha256": None, "error": str(exc)}
            by_test[name] = by_command0[cmd[0]] = key
            errors += 1
            continue
        key = _normalise(real, roots)
        by_test[name] = by_command0[cmd[0]] = key
        if key in executables:
            continue
        members, system_images = [], 0
        for member in runtime_closure(Path(real)):
            if member.startswith("system:"):
                system_images += 1  # the dyld shared cache: named by runner_image
                continue
            if member.startswith("unresolved:"):
                members.append(member)
                continue
            mkey = _normalise(member, roots)
            try:
                closure_files[mkey] = sha(member)
            except OSError:
                closure_files[mkey] = None
            members.append(mkey)
        rows = [f"{m}\t{closure_files.get(m) or ''}" for m in members]
        executables[key] = {
            "sha256": hashes[real],
            "closure": members,
            "system_images": system_images,
            "closure_digest": hashlib.sha256("\n".join(rows).encode("utf-8")).hexdigest(),
        }
    doc = {"schema": SCHEMA, "executables": executables, "closure_files": closure_files,
           "unresolved_executables": errors}
    return doc, by_test


# --------------------------------------------------------------------------
# Run context


def _git(repo: Path, *args: str) -> str | None:
    proc = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    return (proc.stdout.strip() or None) if proc.returncode == 0 else None


def _commit_parents(repo: Path, sha: str) -> tuple[str | None, list[str]]:
    """(tree, parents) from the commit object; a shallow boundary keeps both."""
    raw = _git(repo, "cat-file", "-p", sha) or ""
    tree, parents = None, []
    for line in raw.splitlines():
        if not line:
            break
        if line.startswith("tree "):
            tree = line[5:]
        elif line.startswith("parent "):
            parents.append(line[7:])
    return tree, parents


def run_context(env: dict[str, str], source_root: Path | None) -> dict:
    event = env.get("GITHUB_EVENT_NAME", "")
    sha = env.get("GITHUB_SHA") or None
    tree, parents = (_commit_parents(source_root, sha) if source_root and sha else (None, []))
    ctx = {
        "run_id": env.get("GITHUB_RUN_ID") or None,
        "run_attempt": int(env["GITHUB_RUN_ATTEMPT"]) if env.get("GITHUB_RUN_ATTEMPT", "").isdigit() else None,
        "run_kind": RUN_KINDS.get(event, event or None),
        "pr": None, "head_sha": None, "base_sha": None, "merge_tree": tree,
        "merge_sha": sha, "event": event or None,
        "merge_group_head_ref": env.get("MERGE_GROUP_HEAD_REF") or None,
    }
    if event == "pull_request":
        ctx["pr"] = int(env["PR_NUMBER"]) if env.get("PR_NUMBER", "").isdigit() else None
        ctx["head_sha"] = env.get("PR_HEAD_SHA") or None
        ctx["base_sha"] = env.get("PR_BASE_SHA") or None
    elif event == "merge_group":
        m = re.search(r"/pr-(\d+)-", ctx["merge_group_head_ref"] or "")
        ctx["pr"] = int(m.group(1)) if m else None
        # The group commit's second parent is the last pull request's head in
        # the batch; that is the head a replay pairs this run with.
        ctx["head_sha"] = parents[1] if len(parents) == 2 else None
        ctx["base_sha"] = env.get("MERGE_GROUP_BASE_SHA") or None
    else:
        ctx["head_sha"] = sha
        ctx["base_sha"] = parents[0] if parents else None
    return ctx


def _probe(cmd: list[str]) -> str:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    text = (proc.stdout or proc.stderr).strip().splitlines()
    return text[0].strip() if proc.returncode == 0 and text else "unknown"


def runner_image(env: dict[str, str]) -> dict:
    """Fingerprint of the machine image from inside the job: an ephemeral VM
    is told nothing about its golden, so the image is identified by what it
    carries (OS build, Xcode, SDK, compiler)."""
    fields = {"os": platform.system(), "arch": platform.machine(),
              "image_os": env.get("ImageOS") or None, "image_version": env.get("ImageVersion") or None}
    if platform.system() == "Darwin":
        fields.update({
            "os_version": _probe(["sw_vers", "-productVersion"]),
            "os_build": _probe(["sw_vers", "-buildVersion"]),
            "developer_dir": _probe(["xcode-select", "-p"]),
            "sdk_version": _probe(["xcrun", "--show-sdk-version"]),
            "sdk_build": _probe(["xcrun", "--show-sdk-build-version"]),
            "clang": _probe(["clang", "--version"]),
        })
    else:
        fields.update({"os_version": platform.release(), "clang": _probe(["clang", "--version"])})
    digest = hashlib.sha256(json.dumps(fields, sort_keys=True).encode("utf-8")).hexdigest()[:16]
    return {"digest": digest, "fields": fields}


# --------------------------------------------------------------------------
# Assembly


def parse_suite(spec: str) -> dict:
    name, _, rest = spec.partition("=")
    if not name or not rest:
        raise ValueError(f"--suite wants NAME=PATH[,attempts=LOG][,repeat], got {spec!r}")
    parts = rest.split(",")
    suite = {"name": name, "path": Path(parts[0]), "attempts_log": None, "repeat": False}
    for p in parts[1:]:
        if p == "repeat":
            suite["repeat"] = True
        elif p.startswith("attempts="):
            suite["attempts_log"] = Path(p[len("attempts="):])
        else:
            raise ValueError(f"unknown --suite option {p!r}")
    return suite


def suite_files(path: Path, not_before: float | None = None) -> tuple[list[Path], int]:
    """(report files, stale count). A self-hosted runner keeps its build
    directory between jobs, so a report older than `not_before` (the job's
    start) belongs to an earlier run and is left out."""
    files = sorted(path.glob("*.xml")) if path.is_dir() else ([path] if path.is_file() else [])
    fresh = [f for f in files if not_before is None or f.stat().st_mtime >= not_before]
    return fresh, len(files) - len(fresh)


def build_records(ctx: dict, image_digest: str, suites: list[dict],
                  executables: dict[str, str], keys: dict[str, str | None],
                  not_before: float | None = None) -> tuple[list[dict], dict]:
    records, summary = [], {}
    for suite in suites:
        files, stale = suite_files(suite["path"], not_before)
        log = suite["attempts_log"]
        counts = attempts_from_log(log) if log and log.is_file() else None
        per = {o: 0 for o in OUTCOMES}
        n = 0
        for f in files:
            for case in junit_cases(f):
                name = case["test_id"]
                per[case["outcome"]] += 1
                n += 1
                records.append({
                    "run_id": ctx["run_id"], "run_attempt": ctx["run_attempt"],
                    "run_kind": ctx["run_kind"], "pr": ctx["pr"],
                    "head_sha": ctx["head_sha"], "base_sha": ctx["base_sha"],
                    "merge_tree": ctx["merge_tree"], "suite": suite["name"],
                    "test_id": name, "executable": executables.get(name),
                    "outcome": case["outcome"],
                    "attempts": attempts_for(case["outcome"], name, counts, suite["repeat"]),
                    "duration_s": case["duration_s"], "runner_image": image_digest,
                    "output_key": keys.get(name),
                })
        summary[suite["name"]] = {
            "reports": len(files), "stale_reports": stale, "tests": n, "outcomes": per,
            "attempts_source": "last-test-log" if counts is not None else (
                "unknown (repeat without log)" if suite["repeat"] else "single-run"),
        }
    return records, summary


def _load_inventory(build_dir: Path, selected_json: Path | None, ran: set[str]) -> list[dict]:
    """Inventory entries for the tests that ran: the full suite's selection
    listing when it covers them, else the build directory's full listing (the
    fast tiers select tests by label, which the selection listing omits)."""
    import protected_merge_receipt as pmr

    tests: list[dict] = []
    if selected_json and selected_json.is_file():
        tests = json.loads(selected_json.read_text(encoding="utf-8")).get("tests", [])
    names = {t.get("name") for t in tests}
    if not ran - names:
        return [t for t in tests if t.get("name") in ran]
    full = pmr.ctest_inventory(build_dir).get("tests", [])
    return [t for t in tests + [t for t in full if t.get("name") not in names] if t.get("name") in ran]


def cmd_write(a: argparse.Namespace) -> int:
    t0 = time.monotonic()
    env = dict(os.environ)
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    source_root = Path(a.source_root).resolve() if a.source_root else None
    build_dir = Path(a.build_dir).resolve() if a.build_dir else None
    suites = [parse_suite(s) for s in a.suite]
    ctx = run_context(env, source_root)
    image = runner_image(env)
    problems: list[str] = []

    not_before = float(a.not_before_epoch) if (a.not_before_epoch or "").isdigit() else None
    if suites and not_before is None:
        problems.append("no job start time: reports left in the build directory by an earlier job "
                        "cannot be told apart from this job's")
    ran = {c["test_id"] for s in suites for f in suite_files(s["path"], not_before)[0] for c in junit_cases(f)}
    identity: dict = {"schema": SCHEMA, "executables": {}, "closure_files": {}, "unresolved_executables": 0}
    by_test: dict[str, str] = {}
    if build_dir and ran:
        try:
            seed = {}
            if a.identity_json and Path(a.identity_json).is_file():
                ident = json.loads(Path(a.identity_json).read_text(encoding="utf-8"))
                seed = {os.path.realpath(build_dir / f["path"]): f["sha256"] for f in ident.get("files", [])}
            inventory = _load_inventory(build_dir, Path(a.selected_json) if a.selected_json else None, ran)
            identity, by_test = executable_identity(build_dir, source_root or build_dir, inventory, seed)
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            problems.append(f"executable identity unavailable: {exc}")
    keys: dict[str, str | None] = {}
    if a.test_keys and Path(a.test_keys).is_file():
        raw = json.loads(Path(a.test_keys).read_text(encoding="utf-8"))
        keys = {n: (v.get("key") if isinstance(v, dict) else v) for n, v in raw.items()}

    records, summary = build_records(ctx, image["digest"], suites, by_test, keys, not_before)
    for name, info in summary.items():
        if info["reports"] == 0:
            problems.append(f"suite {name} ran but left no report from this job "
                            f"({info['stale_reports']} older report(s) ignored)")
    tests_path = out / "tests.jsonl"
    with tests_path.open("w", encoding="utf-8") as fh:
        for r in records:
            fh.write(json.dumps(r, separators=(",", ":"), ensure_ascii=False) + "\n")
    identity_path = out / "identity.json"
    identity_path.write_text(json.dumps(identity, sort_keys=True, separators=(",", ":")), encoding="utf-8")

    script_inputs_blob = _git(source_root, "rev-parse", f"HEAD:{SCRIPT_INPUTS_LIST}") if source_root else None
    job = {
        "schema": SCHEMA, **ctx,
        "job": env.get("GITHUB_JOB") or None, "runner_name": env.get("RUNNER_NAME") or None,
        "runner_environment": env.get("RUNNER_ENVIRONMENT") or None,
        "runner_image": image, "build_outcome": a.build_outcome,
        "steps": dict(c.split("=", 1) for c in a.context if "=" in c),
        "no_suite_reason": a.no_suite_reason, "suites": summary,
        "script_inputs_blob": script_inputs_blob,
        "identity": {"executables": len(identity["executables"]),
                     "closure_files": len(identity["closure_files"]),
                     "unresolved_executables": identity["unresolved_executables"],
                     "output_keys": sum(1 for r in records if r["output_key"])},
        "problems": problems,
    }
    job["bytes"] = {"tests_jsonl": tests_path.stat().st_size, "identity_json": identity_path.stat().st_size}
    job["seconds"] = round(time.monotonic() - t0, 1)
    (out / "job.json").write_text(json.dumps(job, sort_keys=True, indent=1), encoding="utf-8")

    note = {"schema": SCHEMA, "run_kind": ctx["run_kind"], "tests": len(records),
            "suites": {k: v["tests"] for k, v in summary.items()},
            "executables": job["identity"]["executables"], "bytes": sum(job["bytes"].values()),
            "runner_image": image["digest"], "seconds": job["seconds"]}
    print(f"::notice title={TITLE}::{json.dumps(note, sort_keys=True)}")
    for p in problems:
        print(f"::warning title=reuse-record incomplete::{p}")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write")
    w.add_argument("--out-dir", required=True)
    w.add_argument("--build-dir")
    w.add_argument("--source-root")
    w.add_argument("--suite", action="append", default=[],
                   help="NAME=JUNIT_FILE_OR_DIR[,attempts=LAST_TEST_LOG][,repeat]; missing paths record zero tests")
    w.add_argument("--selected-json")
    w.add_argument("--identity-json", help="binary_identity_shadow's our-identity.json, to avoid rehashing")
    w.add_argument("--test-keys", help="test_receipts_shadow --keys-out file (per-test output keys)")
    w.add_argument("--not-before-epoch", help="ignore reports last written before this time (the job's start)")
    w.add_argument("--build-outcome", default=None)
    w.add_argument("--context", action="append", default=[],
                   help="KEY=VALUE recorded under job.json `steps` (e.g. ctest=failure)")
    w.add_argument("--no-suite-reason", default=None)
    w.set_defaults(func=cmd_write)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
