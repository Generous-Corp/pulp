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
                     runner_image, output_key, commit_bound (the test's executable
                     embeds a build identity, by label or declaration; null
                     without a codemodel)
    identity.json    every executable a registered ctest runs (whatever this
                     job ran, so a fast-tier head's hashes can be compared
                     with a merge group's), plus every module the build linked
                     (link-members' `kind: module`, with --link-members): sha256 (the same digest
                     `protected_merge_receipt.artifact_identity` puts in a
                     receipt) and the runtime closure it loads (transitive
                     Mach-O dylib/framework linkage, a script's interpreter),
                     each member with its own sha256
    codemodel-<sha>.json
                     per CMake target, digests of its sources, compile groups,
                     link line and ctest registrations from the file-API
                     codemodel (tools/ci/codemodel_digest.py), with --codemodel
    link-members-<sha>.json
                     per executable, the archive members its link pulled and
                     which archives it loads whole (tools/ci/link_members.py),
                     when the build recorded them (--link-members)
    object-deps-<sha>.json
                     per object file, the in-tree headers Ninja recorded it
                     including, and which objects each archive member comes
                     from (tools/ci/object_deps.py), with --object-deps
    registrations-<sha>.json
                     the configuration-neutral ctest registration multiset
                     (changed_surface_inventory.project_registrations), with
                     --inventory: the base inventory a changed-surface plan
                     compares a pull request's registrations against. It is
                     `recordable` only when no registration lacks a command
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
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "scripts"))

SCHEMA = "pulp-reuse-record/v1"
TITLE = "reuse-record"
RECORD_FIELDS = ("run_id", "run_attempt", "run_kind", "pr", "head_sha", "base_sha", "merge_tree",
                 "suite", "test_id", "executable", "outcome", "attempts", "duration_s",
                 "runner_image", "output_key", "commit_bound")
OUTCOMES = ("pass", "fail", "timeout", "skipped", "notrun")
RUN_KINDS = {"pull_request": "pr_head", "merge_group": "merge_group", "push": "push",
             "workflow_dispatch": "dispatch"}
# Environment that changes what a compile produces without changing the
# compiler or the sources; part of the toolchain identity.
# Toolchain fields a record must have found to be keyable; the SDK pair is
# required on macOS as well.
TOOLCHAIN_REQUIRED = ("compiler_id", "compiler_version", "compiler", "effective")
# Environment read while the build runs, so its live value is the one that
# shaped the objects: part of the key. Path remapping (CCACHE_BASEDIR),
# __DATE__/__TIME__ (SOURCE_DATE_EPOCH) and archive member times (ZERO_AR_DATE)
# all change bytes.
BUILD_ENV = ("CCACHE_BASEDIR", "CCACHE_COMPILERCHECK", "CCACHE_DISABLE", "CCACHE_NODEPEND",
             "CCACHE_SLOPPINESS", "PULP_OFFLINE_BUILD", "SOURCE_DATE_EPOCH", "ZERO_AR_DATE")
# Environment CMake reads at configure time. The live value when the record is
# written may differ from the one the build directory was configured under, so
# it is recorded for diagnosis only; the configured result is `effective`.
CONFIGURE_ENV = ("CC", "CFLAGS", "CXX", "CXXFLAGS", "DEVELOPER_DIR", "LDFLAGS",
                 "MACOSX_DEPLOYMENT_TARGET", "SDKROOT")
# What configure made of the above, from CMakeCache.txt.
EFFECTIVE_CACHE = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS",
                   "CMAKE_EXE_LINKER_FLAGS", "CMAKE_OSX_SYSROOT", "CMAKE_OSX_DEPLOYMENT_TARGET")
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


def _artifact_entry(real: str, digest: str, roots: list[tuple[str, str]],
                    closure_files: dict[str, str | None], sha) -> dict:
    """One /executables entry: its sha256 and the runtime closure it loads."""
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
    return {"sha256": digest, "closure": members, "system_images": system_images,
            "closure_digest": hashlib.sha256("\n".join(rows).encode("utf-8")).hexdigest()}


def add_modules(identity: dict, modules: list[str], build_dir: Path, source_root: Path) -> int:
    """Hash every linked module (a `<build>/` key from link members) into the
    identity's /executables map, in the shape its executables have: a module a
    test loads is as much part of what the test runs as its executable.
    A module that cannot be read gets sha256 null and an error. Returns how
    many were added."""
    import protected_merge_receipt as pmr

    build_dir = build_dir.resolve()
    roots = _roots(build_dir, source_root)
    hashes: dict[str, str] = {}

    def sha(path: str) -> str:
        if path not in hashes:
            hashes[path] = pmr._sha256_file(Path(path))
        return hashes[path]

    added = 0
    for key in sorted(modules):
        if key in identity["executables"] or not key.startswith("<build>/"):
            continue
        real = os.path.realpath(build_dir / key.removeprefix("<build>/"))
        try:
            entry = _artifact_entry(real, sha(real), roots, identity["closure_files"], sha)
        except OSError as exc:
            entry = {"sha256": None, "error": f"module unreadable: {exc}"}
        entry["kind"] = "module"
        identity["executables"][_normalise(real, roots)] = entry
        added += 1
    return added


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
        executables[key] = _artifact_entry(real, hashes[real], roots, closure_files, sha)
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


def lane_context(source_root: Path | None, run_id: str | None) -> dict:
    """Identity of a run outside GitHub Actions (the local mac lane): the
    commit is the checkout's HEAD, which is the tree the lane validated."""
    sha = _git(source_root, "rev-parse", "HEAD") if source_root else None
    tree, parents = _commit_parents(source_root, sha) if source_root and sha else (None, [])
    return {"run_id": run_id or None, "run_attempt": None, "run_kind": "lane", "pr": None,
            "head_sha": sha, "base_sha": parents[0] if parents else None, "merge_tree": tree,
            "merge_sha": sha, "event": "lane", "merge_group_head_ref": None,
            "dirty": _dirty(source_root) if source_root else None}


def _dirty(repo: Path) -> bool | None:
    """Whether the checkout differs from HEAD (any `git status --porcelain`
    line, untracked files included): a dirty lane record did not test its
    commit. None when git could not say."""
    proc = subprocess.run(["git", "-C", str(repo), "status", "--porcelain"], capture_output=True, text=True)
    return bool(proc.stdout.strip()) if proc.returncode == 0 else None


def _cmake_cache(build_dir: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    try:
        text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return out
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z_]+=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def _cmake_compiler(build_dir: Path) -> dict[str, str]:
    """CMAKE_CXX_COMPILER, _ID and _VERSION as CMake's compiler check recorded
    them (CMakeFiles/<cmake version>/CMakeCXXCompiler.cmake; the cache holds
    only the path). The newest file wins when CMake was upgraded in place."""
    files = sorted((build_dir / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"),
                   key=lambda f: f.stat().st_mtime)
    out: dict[str, str] = {}
    if files:
        text = files[-1].read_text(encoding="utf-8", errors="replace")
        for key in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
            m = re.search(rf'^set\({key} "([^"]*)"\)', text, re.M)
            if m:
                out[key] = m.group(1)
    return out


def _known(fields: dict) -> dict:
    """The fields whose value was found. An unknown value is left out, never
    written: a reader takes an absent key as unknown, and the literal string
    can never be mistaken for a real version that two records share."""
    return {k: v for k, v in fields.items() if v != "unknown"}


def _env_values(env: dict[str, str], names: tuple[str, ...]) -> dict[str, str]:
    # Only listed names are ever read: the record is uploaded, and the
    # environment is where secrets live. Unset is the literal `unset`; an empty
    # value stays "" so the two are distinct.
    return {k: env[k] if k in env else "unset" for k in names}


def toolchain_identity(build_dir: Path | None, env: dict[str, str], image_fields: dict) -> dict | None:
    """What a compile depends on besides its sources, for a reuse key.

    `fields` is keyed (and digested): the compiler CMake's check recorded (id,
    version, its --version line), the SDK version and build, `effective` (what
    configure resolved from the compiler and flag environment, read from
    CMakeCache.txt), and `build_env` (environment read while compiling:
    ccache settings, SOURCE_DATE_EPOCH, ZERO_AR_DATE). A key uses these, never
    the live configure-time environment.

    Outside `fields`, so never keyed: `configure_env`, the live values of the
    variables CMake reads at configure time, which can differ from those the
    build directory was configured under; and `diagnostic.target`, the
    compiler's -print-target-triple, which names the host OS rather than the
    deployment target.

    One function, so the gate and the local lane compute it identically.
    `complete` is false when a required field was not found."""
    if build_dir is None:
        return None
    cache = _cmake_cache(build_dir)
    probed = _cmake_compiler(build_dir)
    compiler = probed.get("CMAKE_CXX_COMPILER") or cache.get("CMAKE_CXX_COMPILER") or ""
    effective = {k: cache[k] for k in EFFECTIVE_CACHE if k in cache}
    fields = _known({
        "compiler_id": probed.get("CMAKE_CXX_COMPILER_ID") or "unknown",
        "compiler_version": probed.get("CMAKE_CXX_COMPILER_VERSION") or "unknown",
        "compiler": _probe([compiler, "--version"]) if compiler else "unknown",
        "sdk_version": image_fields.get("sdk_version", "unknown"),
        "sdk_build": image_fields.get("sdk_build", "unknown"),
        "effective": effective if "CMAKE_CXX_COMPILER" in effective else "unknown",
        "build_env": _env_values(env, BUILD_ENV),
    })
    required = TOOLCHAIN_REQUIRED + (("sdk_version", "sdk_build") if image_fields.get("os") == "Darwin" else ())
    missing = [k for k in required if k not in fields]
    digest = hashlib.sha256(json.dumps(fields, sort_keys=True).encode("utf-8")).hexdigest()[:16]
    diagnostic = _known({"target": _probe([compiler, "-print-target-triple"]) if compiler else "unknown"})
    return {"digest": digest, "complete": not missing, "missing": missing, "fields": fields,
            "configure_env": _env_values(env, CONFIGURE_ENV), "diagnostic": diagnostic}


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


def _probe(cmd: list[str], why: dict[str, str] | None = None) -> str:
    """First line of a command's output, or "unknown". When `why` is given,
    the reason a probe failed is filed under the command's last argument."""
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        if why is not None:
            why[cmd[-1]] = "timed out after 30s"
        return "unknown"
    except (OSError, subprocess.SubprocessError) as exc:
        if why is not None:
            why[cmd[-1]] = f"{type(exc).__name__}: {exc}"
        return "unknown"
    text = (proc.stdout or proc.stderr).strip().splitlines()
    if proc.returncode == 0 and text:
        return text[0].strip()
    if why is not None:
        why[cmd[-1]] = f"exit {proc.returncode}: {(proc.stderr or proc.stdout).strip()[:300]}"
    return "unknown"


def sdk_from_files(developer_dir: str) -> tuple[str, str]:
    """The default macOS SDK's version and build, read from the SDK itself
    (SDKSettings.json and its SystemVersion.plist): the same values
    `xcrun --show-sdk-version` / `--show-sdk-build-version` print, without
    xcrun, whose first run on a fresh VM can fail or stall."""
    sdk = Path(developer_dir) / "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
    version = build = "unknown"
    try:
        version = str(json.loads((sdk / "SDKSettings.json").read_text(encoding="utf-8"))["Version"])
    except (OSError, ValueError, KeyError):
        pass
    try:
        import plistlib

        with (sdk / "System/Library/CoreServices/SystemVersion.plist").open("rb") as fh:
            build = str(plistlib.load(fh)["ProductBuildVersion"])
    except (OSError, ValueError, KeyError, Exception):  # noqa: BLE001 - a malformed plist is "unknown"
        pass
    return version, build


def runner_image(env: dict[str, str]) -> dict:
    """Fingerprint of the machine image from inside the job: an ephemeral VM
    is told nothing about its golden, so the image is identified by what it
    carries (OS build, Xcode, SDK, compiler)."""
    fields = {"os": platform.system(), "arch": platform.machine(),
              "image_os": env.get("ImageOS") or None, "image_version": env.get("ImageVersion") or None}
    why: dict[str, str] = {}
    sources: dict[str, str] = {}
    if platform.system() == "Darwin":
        developer_dir = _probe(["xcode-select", "-p"], why)
        fields.update({
            "os_version": _probe(["sw_vers", "-productVersion"], why),
            "os_build": _probe(["sw_vers", "-buildVersion"], why),
            "developer_dir": developer_dir,
            "sdk_version": _probe(["xcrun", "--show-sdk-version"], why),
            "sdk_build": _probe(["xcrun", "--show-sdk-build-version"], why),
            "clang": _probe(["clang", "--version"], why),
        })
        # The SDK files carry the same two values; read them when xcrun could
        # not answer. Where the source differs only `probe` records it, so the
        # digest of one toolchain does not depend on which probe answered.
        if "unknown" in (fields["sdk_version"], fields["sdk_build"]) and developer_dir != "unknown":
            version, build = sdk_from_files(developer_dir)
            for key, value in (("sdk_version", version), ("sdk_build", build)):
                if fields[key] == "unknown" and value != "unknown":
                    fields[key] = value
                    sources[key] = "sdk-files"
    else:
        fields.update({"os_version": platform.release(), "clang": _probe(["clang", "--version"], why)})
    fields = _known(fields)
    digest = hashlib.sha256(json.dumps(fields, sort_keys=True).encode("utf-8")).hexdigest()[:16]
    return {"digest": digest, "fields": fields, "probe": {"failed": why, "sources": sources}}


def platform_id() -> str:
    """The host the record was written on, as `<os>-<arch>` (`darwin-arm64`,
    `linux-x86_64`). A reader wanting one platform's record refuses others:
    the gate's alias jobs write a no-suite record under the macOS artifact name
    from a Linux runner."""
    return f"{platform.system().lower()}-{platform.machine().lower()}"


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
                    "commit_bound": None,  # set once the codemodel says which targets are
                })
        summary[suite["name"]] = {
            "reports": len(files), "stale_reports": stale, "tests": n, "outcomes": per,
            "attempts_source": "last-test-log" if counts is not None else (
                "unknown (repeat without log)" if suite["repeat"] else "single-run"),
        }
    return records, summary


def _labels(test: dict) -> set[str]:
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            value = prop.get("value") or []
            return set(value if isinstance(value, list) else str(value).split(";"))
    return set()


def _load_inventory(build_dir: Path, selected_json: Path | None, ran: set[str], every: bool,
                    listing=None) -> list[dict]:
    """Inventory entries: every registered test when `every`, else those that
    ran. The full suite's selection listing is used where it covers them; the
    build directory's full listing fills in the rest (the fast tiers select
    tests by label, which the selection listing omits). `listing` returns the
    build directory's ctest listing, shared with the other record files."""
    import protected_merge_receipt as pmr

    listing = listing or pmr.ctest_inventory
    tests: list[dict] = []
    if selected_json and selected_json.is_file():
        tests = json.loads(selected_json.read_text(encoding="utf-8")).get("tests", [])
    names = {t.get("name") for t in tests}
    if not every and not ran - names:
        return [t for t in tests if t.get("name") in ran]
    full = listing(build_dir).get("tests", [])
    merged = tests + [t for t in full if t.get("name") not in names]
    return merged if every else [t for t in merged if t.get("name") in ran]


def cmd_write(a: argparse.Namespace) -> int:
    t0 = time.monotonic()
    env = dict(os.environ)
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    source_root = Path(a.source_root).resolve() if a.source_root else None
    build_dir = Path(a.build_dir).resolve() if a.build_dir else None
    suites = [parse_suite(s) for s in a.suite]
    if a.run_kind == "lane" and not a.run_id:
        print("reuse_record: --run-kind lane needs --run-id (a lane run has no Actions run id)", file=sys.stderr)
        return 2
    ctx = lane_context(source_root, a.run_id) if a.run_kind == "lane" else run_context(env, source_root)
    image = runner_image(env)
    problems: list[str] = []
    toolchain = toolchain_identity(build_dir, env, image["fields"])
    if toolchain is not None and not toolchain["complete"]:
        problems.append(f"toolchain identity incomplete (no {', '.join(toolchain['missing'])}): "
                        "no reuse key can match it")
    if a.run_kind == "lane" and not ctx["merge_sha"]:
        problems.append("lane run has no commit: --source-root is not a git checkout")

    not_before = float(a.not_before_epoch) if (a.not_before_epoch or "").isdigit() else None
    if suites and not_before is None:
        problems.append("no job start time: reports left in the build directory by an earlier job "
                        "cannot be told apart from this job's")
    ran = {c["test_id"] for s in suites for f in suite_files(s["path"], not_before)[0] for c in junit_cases(f)}
    listed: dict = {}

    def listing(path: Path) -> dict:
        """One `ctest --show-only` per job, shared by every record file."""
        import protected_merge_receipt as pmr

        if "payload" not in listed:
            listed["payload"] = pmr.ctest_inventory(path)
        return listed["payload"]

    identity: dict = {"schema": SCHEMA, "executables": {}, "closure_files": {}, "unresolved_executables": 0}
    by_test: dict[str, str] = {}
    # Every test executable is hashed whenever the build succeeded, whatever
    # this job ran: a fast-tier head's hashes are what a merge group's
    # binary-identity comparison needs.
    every = a.identity_scope == "all" and a.build_outcome in (None, "success")
    identity_ok = True
    if build_dir and (ran or every):
        try:
            seed = {}
            if a.identity_json and Path(a.identity_json).is_file():
                ident = json.loads(Path(a.identity_json).read_text(encoding="utf-8"))
                seed = {os.path.realpath(build_dir / f["path"]): f["sha256"] for f in ident.get("files", [])}
            inventory = _load_inventory(build_dir, Path(a.selected_json) if a.selected_json else None, ran, every,
                                        listing)
            identity, by_test = executable_identity(build_dir, source_root or build_dir, inventory, seed)
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            identity_ok = False
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
    identity_path = out / "identity.json"
    identity_path.write_text(json.dumps(identity, sort_keys=True, separators=(",", ":")), encoding="utf-8")

    codemodel = None
    commit_bound_artifacts: list[str] = []
    if a.codemodel and build_dir:
        import codemodel_digest as cmd_digest

        try:
            import protected_merge_receipt as pmr

            registrations = listing(build_dir).get("tests", [])
            doc = cmd_digest.digest_targets(build_dir, source_root or build_dir, registrations)
            name = f"codemodel-{ctx['merge_sha'] or 'unknown'}.json"
            (out / name).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
            commit_bound_artifacts = [a for t in doc["targets"].values() if t.get("commit_bound")
                                      for a in t["artifacts"]]
            codemodel = {"file": name, "targets": len(doc["targets"]),
                         "commit_bound_targets": sum(1 for t in doc["targets"].values() if t.get("commit_bound")),
                         "commit_bound_declared": (len(doc["commit_bound_declared"])
                                                   if isinstance(doc.get("commit_bound_declared"), list)
                                                   else doc.get("commit_bound_declared")),
                         "executables": sum(1 for t in doc["targets"].values() if t["type"] == "EXECUTABLE"),
                         "with_tests": sum(1 for t in doc["targets"].values() if t["tests"]),
                         "tests_unmatched": doc["tests_unmatched"],
                         "with_generated": sum(1 for t in doc["targets"].values() if t.get("generated")),
                         "generated_headers": doc.get("generated_headers"),
                         "bytes": (out / name).stat().st_size}
            if doc.get("commit_bound_declared") == "unavailable" and a.build_outcome in (None, "success"):
                problems.append("codemodel digest found no commit-bound declarations: tests whose "
                                "executables embed a build identity are not marked")
            if doc.get("generated_headers") != "ninja-deps" and a.build_outcome in (None, "success"):
                problems.append("codemodel digest has no Ninja dependency log: generated headers are not "
                                "keyed, so a target that includes one may keep its digest when it changes")
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            problems.append(f"codemodel digest unavailable: {exc}")

    # A test is commit-bound when its registration says so (the `commit-bound`
    # label) or its executable belongs to a target the build declared
    # commit-bound or that depends on one. Unknown (null) without a codemodel.
    if codemodel is not None:
        bound_files = set(commit_bound_artifacts)
        labelled = {t.get("name") for t in listing(build_dir).get("tests", [])
                    if "commit-bound" in _labels(t)} if build_dir else set()
        for r in records:
            r["commit_bound"] = r["test_id"] in labelled or r["executable"] in bound_files
    tests_path = out / "tests.jsonl"
    with tests_path.open("w", encoding="utf-8") as fh:
        for r in records:
            fh.write(json.dumps(r, separators=(",", ":"), ensure_ascii=False) + "\n")

    link_members = None
    modules_added = 0
    if a.link_members and build_dir:
        import link_members as lm

        try:
            doc = lm.collect(build_dir)
            if every and identity_ok:
                modules_added = add_modules(identity, [k for k, r in doc["executables"].items()
                                                       if r.get("kind") == "module"],
                                            build_dir, source_root or build_dir)
                identity_path.write_text(json.dumps(identity, sort_keys=True, separators=(",", ":")),
                                         encoding="utf-8")
            name = f"link-members-{ctx['merge_sha'] or 'unknown'}.json"
            (out / name).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
            kinds = [rec.get("kind", "executable") for rec in doc["executables"].values()]
            link_members = {"file": name, "executables": kinds.count("executable"),
                            "modules": kinds.count("module"), "shared": kinds.count("shared"),
                            # Executables and modules that load a build-produced
                            # shared library: always_run `shared_link`, not keyed.
                            "shared_loaders": len(lm.shared_scope(doc)),
                            "unreadable": doc["unreadable"], "unrecorded": doc["unrecorded"], "bytes": (out / name).stat().st_size,
                            "whole_archives": sorted({arch for rec in doc["executables"].values()
                                                      for arch, info in rec["archives"].items() if info["whole"]})}
            if not doc["executables"] and a.build_outcome in (None, "success"):
                problems.append("link members requested but the build recorded none "
                                "(PULP_RECORD_LINK_MAPS off, or every executable was already linked)")
            reason = lm.unusable(doc)
            # One boolean a reader can point at instead of parsing problems:
            # false (or the whole summary null) means do not key on the map.
            link_members["usable"] = reason is None and bool(doc["executables"])
            if reason:
                problems.append(f"link members not usable for reuse: {reason}")
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            problems.append(f"link members unavailable: {exc}")

    object_deps = None
    if a.object_deps and build_dir:
        import object_deps as od

        try:
            if not a.source_root:
                raise od.Unavailable("--object-deps needs --source-root to write <src> paths")
            doc = od.collect(build_dir, Path(a.source_root).resolve())
            name = f"object-deps-{ctx['merge_sha'] or 'unknown'}.json"
            (out / name).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
            object_deps = {"file": name, "objects": len(doc["objects"]), "headers": len(doc["headers"]),
                           "stale": len(doc["stale"]), "archives": len(doc["members"]),
                           "bytes": (out / name).stat().st_size}
            reason = od.unusable(doc)
            # Stale objects stay usable: a reader treats each one as changed.
            object_deps["usable"] = reason is None
            if reason:
                problems.append(f"object deps not usable for reuse: {reason}")
            elif doc["stale"]:
                problems.append(f"object deps: {len(doc['stale'])} object(s) STALE in the Ninja log; "
                                "their headers are unknown")
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            problems.append(f"object deps unavailable: {exc}")

    registration_projection = None
    if a.inventory and build_dir:
        import changed_surface_inventory as csi

        try:
            doc = csi.project_registrations(listing(build_dir), source_root or build_dir,
                                            build_dir)
            name = f"registrations-{ctx['merge_sha'] or 'unknown'}.json"
            (out / name).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
            registration_projection = {"file": name, "rows": doc["row_count"], "digest": doc["digest"],
                                        "recordable": doc["recordable"],
                                        "incomplete": len(doc["incomplete"]),
                                        "config_gated": len(doc["config_gated"]),
                                        "bytes": (out / name).stat().st_size}
            if not doc["recordable"] and a.build_outcome in (None, "success"):
                problems.append(f"registration projection not recordable: {len(doc['incomplete'])} "
                                "registration(s) have no command after a successful build")
        except Exception as exc:  # noqa: BLE001 - results are still worth writing
            problems.append(f"registration projection unavailable: {exc}")

    script_inputs_blob = _git(source_root, "rev-parse", f"HEAD:{SCRIPT_INPUTS_LIST}") if source_root else None
    job = {
        "schema": SCHEMA, **ctx,
        "job": env.get("GITHUB_JOB") or None, "runner_name": env.get("RUNNER_NAME") or None,
        "runner_environment": env.get("RUNNER_ENVIRONMENT") or None,
        "platform": platform_id(), "runner_image": image, "toolchain": toolchain,
        "build_outcome": a.build_outcome,
        "steps": dict(c.split("=", 1) for c in a.context if "=" in c),
        "no_suite_reason": a.no_suite_reason, "suites": summary,
        "script_inputs_blob": script_inputs_blob,
        # One boolean for a pointer reader: identity.json can be compared. An
        # empty map is never usable; it means the listing found nothing to hash.
        "identity": {"usable": identity_ok and bool(identity["executables"]),
                     "modules": modules_added,
                     "executables": len(identity["executables"]),
                     "closure_files": len(identity["closure_files"]),
                     "unresolved_executables": identity["unresolved_executables"],
                     "output_keys": sum(1 for r in records if r["output_key"])},
        "link_members": link_members,
        "object_deps": object_deps,
        "codemodel": codemodel,
        "registration_projection": registration_projection,
        "problems": problems,
    }
    job["bytes"] = {"tests_jsonl": tests_path.stat().st_size, "identity_json": identity_path.stat().st_size,
                    "link_members": link_members["bytes"] if link_members else 0,
                    "object_deps": object_deps["bytes"] if object_deps else 0,
                    "codemodel": codemodel["bytes"] if codemodel else 0,
                    "registration_projection": registration_projection["bytes"] if registration_projection else 0}
    job["seconds"] = round(time.monotonic() - t0, 1)
    (out / "job.json").write_text(json.dumps(job, sort_keys=True, indent=1), encoding="utf-8")

    note = {"schema": SCHEMA, "run_kind": ctx["run_kind"], "tests": len(records),
            "suites": {k: v["tests"] for k, v in summary.items()},
            "executables": job["identity"]["executables"], "bytes": sum(job["bytes"].values()),
            "linked": link_members["executables"] if link_members else None,
            "codemodel_targets": codemodel["targets"] if codemodel else None,
            "registration_rows": registration_projection["rows"] if registration_projection else None,
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
    w.add_argument("--run-kind", choices=("github", "lane"), default="github",
                   help="lane: a run outside GitHub Actions; its commit is --source-root's HEAD")
    w.add_argument("--run-id", help="lane run id; required with --run-kind lane")
    w.add_argument("--identity-scope", choices=("all", "ran"), default="all",
                   help="hash every registered test executable (default) or only those that ran")
    w.add_argument("--codemodel", action="store_true",
                   help="digest every CMake codemodel target (tools/ci/codemodel_digest.py) into codemodel-<sha>.json")
    w.add_argument("--inventory", action="store_true",
                   help="also write the configuration-neutral ctest registration projection "
                        "(changed_surface_inventory.project_registrations)")
    w.add_argument("--object-deps", action="store_true",
                   help="record each object's in-tree headers (tools/ci/object_deps.py) into object-deps-<sha>.json")
    w.add_argument("--link-members", action="store_true",
                   help="collect <build>/link-members (tools/ci/link_members.py) into link-members-<sha>.json")
    w.add_argument("--context", action="append", default=[],
                   help="KEY=VALUE recorded under job.json `steps` (e.g. ctest=failure)")
    w.add_argument("--no-suite-reason", default=None)
    w.set_defaults(func=cmd_write)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
