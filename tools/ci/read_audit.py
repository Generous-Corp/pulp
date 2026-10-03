#!/usr/bin/env python3
"""Which checkout files do compiled tests actually read, against what they declare?

A test-reuse selector may skip a compiled test whose build inputs did not
change, trusting `pulp_test_data()` (tools/cmake/PulpTestData.cmake, folded
into test/ctest_script_inputs.json) to name every checkout file the test reads
at run time. That declaration is derived from a static scan of the test's
sources, so it can miss a read: a path assembled at run time, a fixture a
helper library opens, a file a spawned tool reads. This audit measures the
reads instead of inferring them.

It runs each compiled test executable's ctest registrations under
`strace -f` (Linux), resolves every file syscall's path, and keeps the paths
that are tracked files inside the checkout but outside the build directory.
A path is covered when the executable's declared inputs match it (the same
prefix-or-glob rule the selector uses, affected_tests_shadow.declared_hit) or
when a fail-closed rule already reruns every test on its change (CMake files).
Anything else is a finding naming the executable, the test and the path.

    read         the file was opened (its content can reach the verdict)
    listing      a directory was opened for listing
    probe        stat/access/readlink only: the test checked that it exists

Before the tests it audits a control: a tiny program, run through ctest and
strace exactly like a test, that opens one undeclared tracked file and one
declared one. The audit is only trusted when the control's undeclared read is
a finding and its declared read is not; otherwise the instrument is blind and
the run exits 2 without a verdict.

Coverage gaps are part of the output, not footnotes: executables the manifest
lists that this platform does not register (macOS-only tests) are named, as
are executables that were not built, and script-driven tests are not audited.

    read_audit.py run --build-dir B --out report.json [--summary summary.md]
                      [--jobs N] [--only REGEX] [--fail-on-findings]
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from affected_tests_shadow import classify_changes, declared_hit, executable_name, is_binary_test  # noqa: E402

SCHEMA = "pulp-read-audit/v1"
MANIFEST = Path("test") / "ctest_script_inputs.json"
STRACE_FILTER = "trace=%file,%process,fchdir,getdents64"
CONTROL_NAME = "read-audit-control"
# The control's undeclared and declared reads. Both are tracked files every
# checkout has.
CONTROL_UNDECLARED = "README.md"
CONTROL_DECLARED = "LICENSE.md"
PROBE_SYSCALLS = {"stat", "lstat", "newfstatat", "fstatat64", "statx", "access", "faccessat",
                  "faccessat2", "readlink", "readlinkat", "statfs", "getxattr", "lgetxattr"}
SPAWN_SYSCALLS = {"clone", "clone3", "fork", "vfork"}
# Syscalls whose first argument is a directory fd the path is relative to.
DIRFD_SYSCALLS = {"openat", "openat2", "newfstatat", "fstatat64", "statx", "faccessat", "faccessat2",
                  "readlinkat", "execveat", "mkdirat", "unlinkat", "fchmodat", "fchownat", "utimensat",
                  "renameat", "renameat2", "linkat", "mknodat", "futimesat", "name_to_handle_at"}
SOURCE_SUFFIXES = (".cpp", ".cc", ".c", ".mm", ".m", ".h", ".hpp", ".hh", ".inl")

_LINE = re.compile(r"^(\d+)\s+(.*)$")
_CALL = re.compile(r"^(\w+)\((.*)$")
_RESUMED = re.compile(r"^<\.\.\. (\w+) resumed>(.*)$")
_RESULT = re.compile(r"\)\s+=\s+(-?\d+|\?)(?:<[^>]*>)?(?:\s.*)?$")
_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')
_FD = re.compile(r"^\s*(AT_FDCWD|-?\d+)(?:<([^>]*)>)?")


def _unescape(text: str) -> str:
    """Undo strace's C-style string escaping (\\n, \\", \\x41, \\101)."""
    out = bytearray()
    i = 0
    raw = text.encode("utf-8", "surrogateescape")
    while i < len(raw):
        c = raw[i]
        if c != 0x5C or i + 1 >= len(raw):
            out.append(c)
            i += 1
            continue
        n = chr(raw[i + 1])
        if n == "x" and i + 4 <= len(raw):
            out.append(int(raw[i + 2:i + 4], 16))
            i += 4
        elif n in "01234567":
            j = i + 1
            while j < len(raw) and j < i + 4 and chr(raw[j]) in "01234567":
                j += 1
            out.append(int(raw[i + 1:j], 8) & 0xFF)
            i = j
        else:
            out.append({"n": 10, "t": 9, "r": 13, "v": 11, "f": 12}.get(n, ord(n)))
            i += 2
    return out.decode("utf-8", "surrogateescape")


@dataclass
class Access:
    """One resolved file syscall."""
    pid: int
    syscall: str
    path: str
    directory: bool = False


@dataclass
class Trace:
    accesses: list[Access] = field(default_factory=list)
    unresolved: int = 0
    pids: set[int] = field(default_factory=set)
    root_pid: int | None = None
    programs: dict[int, str] = field(default_factory=dict)


def parse_strace(lines, initial_cwd: str) -> Trace:
    """Resolve every path a `strace -f -y` log names to an absolute path.

    Relative paths resolve against the calling process's working directory:
    the root process starts in `initial_cwd`, a child inherits its parent's
    directory at the clone that created it, and a successful chdir moves it.
    A relative path whose directory cannot be known is counted, never
    guessed."""
    pending: dict[int, tuple[str, str]] = {}
    calls: dict[int, list[tuple[str, str, str]]] = {}
    children: dict[int, tuple[int, int]] = {}  # child -> (parent, index in parent's calls)
    order: list[int] = []
    for raw in lines:
        m = _LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        pid, rest = int(m.group(1)), m.group(2)
        if pid not in calls:
            calls[pid] = []
            order.append(pid)
        if rest.endswith("<unfinished ...>"):
            cm = _CALL.match(rest)
            if cm:
                pending[pid] = (cm.group(1), cm.group(2)[: -len("<unfinished ...>")])
            continue
        rm = _RESUMED.match(rest)
        if rm:
            name, head = pending.pop(pid, (rm.group(1), ""))
            rest_args = head + rm.group(2)
        else:
            cm = _CALL.match(rest)
            if not cm:
                continue
            name, rest_args = cm.group(1), cm.group(2)
        res = _RESULT.search(rest_args)
        result = res.group(1) if res else "?"
        args = rest_args[: res.start()] if res else rest_args
        calls[pid].append((name, args, result))
        if name in SPAWN_SYSCALLS and result.isdigit() and int(result) > 0:
            children[int(result)] = (pid, len(calls[pid]))
    trace = Trace(pids=set(order), root_pid=order[0] if order else None)
    cwd_at: dict[int, list[str | None]] = {}

    def resolve(pid: int) -> None:
        if pid in cwd_at:
            return
        parent = children.get(pid)
        if parent is None:
            start: str | None = initial_cwd if pid == trace.root_pid else None
        else:
            resolve(parent[0])
            history = cwd_at.get(parent[0]) or [None]
            start = history[min(parent[1], len(history) - 1)]
        history: list[str | None] = [start]
        cwd = start
        for name, args, result in calls.get(pid, []):
            strings = [_unescape(s) for s in _STRING.findall(args)]
            path = strings[0] if strings else None
            fd = _FD.match(args) if name in DIRFD_SYSCALLS else None
            base = cwd
            if fd:
                if fd.group(2) is not None:
                    base = fd.group(2)
                elif fd.group(1) != "AT_FDCWD":
                    base = None
            if name == "fchdir":
                annotated = _FD.match(args)
                cwd = annotated.group(2) if annotated and annotated.group(2) and result == "0" else (
                    cwd if result != "0" else None)
            elif name == "getdents64":
                annotated = _FD.match(args)
                if annotated and annotated.group(2):
                    trace.accesses.append(Access(pid, name, os.path.normpath(annotated.group(2)), True))
            elif path is not None:
                if os.path.isabs(path):
                    full = os.path.normpath(path)
                elif base is not None:
                    full = os.path.normpath(os.path.join(base, path))
                else:
                    full = None
                    trace.unresolved += 1
                if full is not None:
                    if name == "chdir" and result == "0":
                        cwd = full
                    elif name in ("execve", "execveat") and result == "0":
                        trace.programs[pid] = full
                    directory = "O_DIRECTORY" in args
                    trace.accesses.append(Access(pid, name, full, directory))
            history.append(cwd)
        cwd_at[pid] = history

    for pid in order:
        resolve(pid)
    return trace


@dataclass
class Finding:
    path: str
    kind: str          # read | listing | probe
    program: str       # the process that made the access (basename of its executable)
    tests: list[str] = field(default_factory=list)

    def as_dict(self) -> dict:
        return {"path": self.path, "kind": self.kind, "program": self.program, "tests": self.tests}


def tracked_files(root: Path) -> set[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True).stdout
    return {p for p in out.decode("utf-8", "surrogateescape").split("\0") if p}


def tracked_dirs(files: set[str]) -> set[str]:
    dirs = set()
    for f in files:
        parts = f.split("/")[:-1]
        for n in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:n]))
    return dirs


def covered_by(rel: str, inputs: list[str]) -> str | None:
    """Why a read needs no declaration, or None when it is a finding."""
    if declared_hit(inputs, [rel]):
        return "declared"
    if classify_changes([rel])["cmake_changed"]:
        return "rule:cmake"
    return None


def _real(path: str) -> str | None:
    """The resolved path, or None for a path that cannot be a checkout file:
    a kernel pseudo-filesystem (another process's /proc/<pid>/cwd raises
    EACCES on readlink) or one the resolver is refused."""
    if path == "/proc" or path.startswith(("/proc/", "/sys/", "/dev/")):
        return None
    try:
        return os.path.realpath(path)
    except OSError:
        return None


def checkout_accesses(trace: Trace, root: Path, excluded: list[Path], files: set[str],
                      dirs: set[str]) -> dict[tuple[str, str], str]:
    """(relative path, kind) -> program for every access of a tracked path.
    The root process (ctest) is not the test, so its accesses are dropped."""
    root_real = os.path.realpath(root)
    skip = [os.path.realpath(p) for p in excluded]
    rank = {"probe": 0, "listing": 1, "read": 2}
    best: dict[str, tuple[str, str]] = {}
    for a in trace.accesses:
        if a.pid == trace.root_pid:
            continue
        real = _real(a.path)
        if real is None or not (real == root_real or real.startswith(root_real + os.sep)):
            continue
        if any(real == s or real.startswith(s + os.sep) for s in skip):
            continue
        rel = os.path.relpath(real, root_real)
        if rel == "." or rel == ".git" or rel.startswith(".git/"):
            continue
        if rel in files:
            kind = "probe" if a.syscall in PROBE_SYSCALLS else "read"
        elif rel in dirs and (a.directory or a.syscall == "getdents64"):
            kind = "listing"
        else:
            continue
        if rel not in best or rank[kind] > rank[best[rel][0]]:
            best[rel] = (kind, os.path.basename(trace.programs.get(a.pid, "?")))
    return {(rel, kind): program for rel, (kind, program) in best.items()}


def findings_for(accesses: dict[tuple[str, str], str], inputs: list[str]) -> tuple[list[Finding], int]:
    found, covered = [], 0
    for (rel, kind), program in sorted(accesses.items()):
        if covered_by(rel, inputs):
            covered += 1
        else:
            found.append(Finding(rel, kind, program))
    return found, covered


def strace_run(argv: list[str], cwd: Path, log: Path, timeout: int, env: dict | None = None) -> int | None:
    cmd = ["strace", "-f", "-qq", "-y", "-s", "4096", "-e", STRACE_FILTER, "-o", str(log), *argv]
    try:
        return subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        return None


def trace_tests(build_dir: Path, names: list[str], work: Path, timeout: int) -> tuple[Trace | None, int | None]:
    """Run exactly these ctest registrations under strace."""
    work.mkdir(parents=True, exist_ok=True)
    listing = work / "tests.txt"
    listing.write_text("".join(n + "\n" for n in names), encoding="utf-8")
    log = work / "strace.log"
    rc = strace_run(["ctest", "--test-dir", str(build_dir), "-C", "Release", "--timeout", str(timeout),
                     "--tests-from-file", str(listing)], build_dir, log, timeout * len(names) + 120)
    if not log.is_file():
        return None, rc
    with log.open("r", encoding="utf-8", errors="surrogateescape") as fh:
        trace = parse_strace(fh, str(build_dir))
    log.unlink()
    return trace, rc


@dataclass
class Group:
    executable: str
    tests: list[str]
    manifest: dict | None


def groups_from(inventory: dict, build_dir: Path, manifest: dict, only: str | None) -> tuple[list[Group], int]:
    by_exe: dict[str, list[str]] = {}
    scripts = 0
    for t in inventory.get("tests", []):
        if not is_binary_test(t, build_dir):
            scripts += 1
            continue
        exe = executable_name(t)
        if only and not re.search(only, exe):
            continue
        by_exe.setdefault(exe, []).append(t["name"])
    entries = manifest.get("executables") or {}
    return [Group(exe, tests, entries.get(exe)) for exe, tests in sorted(by_exe.items())], scripts


def audit_group(g: Group, build_dir: Path, root: Path, work: Path, files: set[str], dirs: set[str],
                timeout: int, per_test_limit: int) -> dict:
    inputs = list((g.manifest or {}).get("inputs") or [])
    data = (g.manifest or {}).get("data", "absent")
    trace, rc = trace_tests(build_dir, g.tests, work / g.executable, timeout)
    rec: dict = {"data": data, "inputs": inputs, "tests": len(g.tests), "ctest_exit": rc}
    if trace is None or len(trace.pids) < 2:
        rec["status"] = "unobserved"  # strace saw no process besides ctest: nothing ran
        return rec
    accesses = checkout_accesses(trace, root, [build_dir], files, dirs)
    found, covered = findings_for(accesses, inputs)
    rec.update(status="audited", processes=len(trace.pids), unresolved=trace.unresolved,
               checkout_accesses=len(accesses), covered=covered)
    if found and len(g.tests) > 1 and len(g.tests) <= per_test_limit:
        # Attribute each finding to the registrations that make it.
        by_path = {f.path: f for f in found}
        for i, name in enumerate(g.tests):
            t, _ = trace_tests(build_dir, [name], work / g.executable / f"t{i}", timeout)
            if t is None:
                continue
            for rel, _kind in checkout_accesses(t, root, [build_dir], files, dirs):
                if rel in by_path:
                    by_path[rel].tests.append(name)
    elif found and len(g.tests) == 1:
        for f in found:
            f.tests = list(g.tests)
    rec["findings"] = [f.as_dict() for f in found]
    return rec


CONTROL_SOURCE = r"""
#include <fcntl.h>
#include <unistd.h>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) return 1;
        char b[16];
        (void)read(fd, b, sizeof b);
        close(fd);
    }
    return 0;
}
"""


def run_control(root: Path, work: Path, files: set[str], dirs: set[str]) -> dict:
    """The negative control: the same ctest + strace path over a program that
    reads one undeclared and one declared tracked file."""
    cdir = work / "control"
    cdir.mkdir(parents=True, exist_ok=True)
    src, exe = cdir / "control.c", cdir / CONTROL_NAME
    src.write_text(CONTROL_SOURCE, encoding="utf-8")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None or subprocess.run([cc, "-o", str(exe), str(src)], capture_output=True).returncode != 0:
        return {"ok": False, "reason": "could not compile the control program"}
    (cdir / "CTestTestfile.cmake").write_text(
        f'add_test({CONTROL_NAME} "{exe}" "{root / CONTROL_UNDECLARED}" "{root / CONTROL_DECLARED}")\n',
        encoding="utf-8")
    trace, rc = trace_tests(cdir, [CONTROL_NAME], cdir / "run", 60)
    if trace is None:
        return {"ok": False, "reason": "strace wrote no log", "ctest_exit": rc}
    accesses = checkout_accesses(trace, root, [cdir], files, dirs)
    found, _ = findings_for(accesses, [CONTROL_DECLARED])
    paths = {f.path for f in found}
    ok = CONTROL_UNDECLARED in paths and CONTROL_DECLARED not in paths and \
        (CONTROL_DECLARED, "read") in accesses and rc == 0
    return {"ok": ok, "ctest_exit": rc, "findings": sorted(paths),
            "saw_declared_read": (CONTROL_DECLARED, "read") in accesses,
            "reason": None if ok else "the control's undeclared read was not flagged, or its declared read was "
                                      "flagged or not seen"}


def summarize(report: dict) -> str:
    t = report["totals"]
    lines = ["## Compiled-test read audit (Linux, strace)", ""]
    c = report["control"]
    lines.append(f"Control: {'flagged its undeclared read' if c.get('ok') else 'FAILED: ' + str(c.get('reason'))}")
    lines += ["", f"- executables audited: {t['audited']} of {t['executables']} registered "
                  f"({t['unobserved']} unobserved, {t.get('errors', 0)} failed to audit, "
                  f"{t['tests']} ctest registrations)",
              f"- findings: {t['findings']} undeclared checkout accesses in {t['executables_with_findings']} "
              f"executables ({t['finding_reads']} reads, {t['finding_listings']} listings, "
              f"{t['finding_probes']} probes)",
              f"- covered accesses: {t['covered']}; relative paths with an unknown directory: {t['unresolved']}",
              "", "Not covered by this audit:",
              f"- {len(report['gaps']['manifest_not_registered'])} manifest executables this platform does not "
              "register (macOS-only tests)",
              f"- {report['gaps']['script_tests']} script-driven tests (their inputs are not pulp_test_data)",
              "- a read that only happens on another platform, or on a path the run did not take", ""]
    rows = [(exe, rec) for exe, rec in sorted(report["executables"].items()) if rec.get("findings")]
    if rows:
        lines += ["| executable | declared | kind | path | tests | via |", "|---|---|---|---|---|---|"]
        for exe, rec in rows:
            for f in rec["findings"]:
                tests = ", ".join(f["tests"][:3]) + (" ..." if len(f["tests"]) > 3 else "")
                lines.append(f"| `{exe}` | {rec['data']} | {f['kind']} | `{f['path']}` | {tests or '?'} | "
                             f"{f['program']} |")
    return "\n".join(lines) + "\n"


def cmd_run(a: argparse.Namespace) -> int:
    if shutil.which("strace") is None:
        print("read-audit: strace is not installed; this audit runs on Linux", file=sys.stderr)
        return 2
    root = Path(a.source_root).resolve()
    build_dir = Path(a.build_dir).resolve()
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    inventory = json.loads(subprocess.run(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
                                          capture_output=True, text=True, check=True).stdout)
    files = tracked_files(root)
    dirs = tracked_dirs(files)
    work = Path(tempfile.mkdtemp(prefix="read-audit-"))
    control = run_control(root, work, files, dirs)
    print(f"read-audit: control {'flagged its undeclared read' if control['ok'] else 'FAILED'}: "
          f"{json.dumps(control, sort_keys=True)}", flush=True)
    report: dict = {"schema": SCHEMA, "source_root": str(root), "build_dir": str(build_dir),
                    "commit": subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                                             text=True).stdout.strip(),
                    "control": control, "executables": {}}
    if not control["ok"]:
        report["totals"] = {}
        Path(a.out).write_text(json.dumps(report, indent=1, sort_keys=True), encoding="utf-8")
        print(f"read-audit: control failed ({control['reason']}); the instrument cannot see reads, "
              "so no verdict", file=sys.stderr)
        return 2
    groups, scripts = groups_from(inventory, build_dir, manifest, a.only)
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = {pool.submit(audit_group, g, build_dir, root, work, files, dirs, a.timeout, a.per_test_limit): g
                   for g in groups}
        for done, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            exe = futures[fut].executable
            try:
                rec = fut.result()
            except Exception as exc:  # noqa: BLE001 - one executable must not lose the whole run
                rec = {"status": "error", "error": f"{type(exc).__name__}: {exc}"}
            report["executables"][exe] = rec
            print(f"read-audit: [{done}/{len(groups)}] {exe}: {rec.get('status')} "
                  f"{len(rec.get('findings') or [])} finding(s)", flush=True)
    registered = {g.executable for g in groups}
    recs = report["executables"].values()
    found = [f for r in recs for f in r.get("findings") or []]
    report["gaps"] = {"manifest_not_registered": sorted(set(manifest.get("executables") or {}) - registered),
                      "script_tests": scripts}
    report["totals"] = {
        "executables": len(groups), "tests": sum(len(g.tests) for g in groups),
        "audited": sum(1 for r in recs if r.get("status") == "audited"),
        "unobserved": sum(1 for r in recs if r.get("status") == "unobserved"),
        "errors": sum(1 for r in recs if r.get("status") == "error"),
        "findings": len(found), "executables_with_findings": sum(1 for r in recs if r.get("findings")),
        "finding_reads": sum(f["kind"] == "read" for f in found),
        "finding_listings": sum(f["kind"] == "listing" for f in found),
        "finding_probes": sum(f["kind"] == "probe" for f in found),
        "covered": sum(r.get("covered", 0) for r in recs), "unresolved": sum(r.get("unresolved", 0) for r in recs)}
    Path(a.out).write_text(json.dumps(report, indent=1, sort_keys=True), encoding="utf-8")
    text = summarize(report)
    if a.summary:
        Path(a.summary).write_text(text, encoding="utf-8")
    print(text)
    for exe, rec in sorted(report["executables"].items()):
        for f in rec.get("findings") or []:
            if f["kind"] != "probe":
                print(f"::warning title=undeclared test read::{exe} {f['kind']}s {f['path']} "
                      f"(tests: {', '.join(f['tests'][:3]) or 'unattributed'}; declared data: {rec['data']})")
    shutil.rmtree(work, ignore_errors=True)
    return 1 if a.fail_on_findings and report["totals"]["finding_reads"] + report["totals"]["finding_listings"] \
        else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--build-dir", required=True)
    r.add_argument("--source-root", default=str(HERE.parents[1]))
    r.add_argument("--out", required=True)
    r.add_argument("--summary")
    r.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    r.add_argument("--timeout", type=int, default=300, help="per ctest registration, seconds")
    r.add_argument("--per-test-limit", type=int, default=200,
                   help="attribute findings per registration only for executables with at most this many")
    r.add_argument("--only", help="audit only executables whose name matches this regex")
    r.add_argument("--fail-on-findings", action="store_true",
                   help="exit 1 when an undeclared read or listing is found (probes never fail the run)")
    r.set_defaults(func=cmd_run)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
