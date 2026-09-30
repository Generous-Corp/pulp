#!/usr/bin/env python3
"""Per-test content-hash receipts for the merge-group ctest run — shadow mode.

A merge group runs all ~21,700 selected ctest entries whatever changed, and an
ejected group's successor re-runs every one of them again although most of
their inputs did not move. A Bazel-style test result cache would run only the
tests whose inputs changed: each test gets a KEY, a hash of everything its
result can depend on, and a trusted gate run that saw the test PASS records a
receipt under that key. A later run whose key matches could skip the test.

This tool computes the keys and receipts and reports what a receipt-keyed
skip WOULD have done. It changes nothing that runs: every test still runs, and
the report is read against what really happened in the same run.

Key (per ctest entry), `KEY_VERSION` + sha256 over:
- the test's command with the build and source roots normalised, and its
  ctest properties (WORKING_DIRECTORY, ENVIRONMENT, TIMEOUT, WILL_FAIL, ...);
- the bytes of the executable (a compiled test binary, or the interpreter of
  a script test) and of every file the command line names;
- a script test: the git blob ids of its declared inputs in
  `test/ctest_script_inputs.json` (a directory input covers every tracked
  file under it); a script test without an entry has no key;
- a compiled test: the RUNTIME SURFACE — git blob ids of every tracked file a
  compiled test can read at run time that its own bytes do not capture
  (fixtures, fonts, JSON, JS, scripts, CMake; compiled sources and docs are
  excluded) — and the build's PRODUCTS (sha256 of every executable file in
  the build directory that is not itself a test binary: the CLI, plugin
  bundles, helpers). Compiled tests reach both through PULP_SOURCE_DIR /
  PULP_BUILD_DIR compile definitions, which never change their bytes;
- the toolchain (`protected_merge_receipt.toolchain_identity` + OS build) and
  the CI policy files (`POLICY_PATHS`).

Receipts are written only by trusted gate runs — this repository's
`build.yml` on a `merge_group` event, the macOS job — as the artifact
`test-receipts-macos` (keys of the tests that passed, plus the names that
failed). Reads accept only such artifacts: same workflow path, same
repository and head repository, and a receipt whose run id matches.

Always-run (never counted as skippable, whatever the receipts say):
- whole-tree drift/lint/registry/sync/guard/census/inventory tests and
  host/GPU probes (`ALWAYS_RUN_NAME_RE`, `ALWAYS_RUN_LABELS`), and every
  `pr-fast` test (cheap by definition; the cross-PR conflicts live there);
- a script test that declares a top-level directory (it reads the tree);
- a test with no key: undeclared script tests, nested cmake/ninja builds,
  a command naming a build-directory directory;
- any test that failed in one of the receipt runs read.
A CMake change in the group's own diff, and one run id in `CONTROL_EVERY`,
would force a full run (reported as `would_force_full`).

Proxies read from the annotation (`pulp-test-receipts-shadow/v1`):
- `would_skip` ÷ `selected` and `would_skip_seconds` (sum of test times);
- the SAFETY control: `would_skip_failed`, tests a receipt would have skipped
  that FAILED in this very run. It must stay 0 over a long window before any
  enforcement is proposed (a decisions-contract amendment).

    test_receipts_shadow.py run --build-dir B --source-root S --repository O/R \\
        --merge-sha SHA --token T --junit J --selected-json I --run-id N \\
        --receipts-out F [--identity-json F] [--keys-out F] [--summary F]
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from typing import Callable, Iterable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "scripts"))
import affected_tests_shadow as ats  # noqa: E402
import protected_merge_receipt as pmr  # noqa: E402

SCHEMA = "pulp-test-receipts-shadow/v1"
RECEIPT_SCHEMA = "pulp-test-receipts/v1"
TITLE = "test-receipts-shadow"
KEY_VERSION = "1"
ARTIFACT_NAME = "test-receipts-macos"
RECEIPT_FILE = "test-receipts.json"
WORKFLOW_PATH = ".github/workflows/build.yml"
TRUSTED_EVENT = "merge_group"
LOOKBACK = 20
CONTROL_EVERY = 10
MAX_RECEIPT_BYTES = 32 * 1024 * 1024
READ_BUDGET_SECS = 120.0

POLICY_PATHS = pmr.POLICY_PATHS + (
    "tools/ci/ctest_gate_args.py",
    "tools/ci/test_receipts_shadow.py",
)
ALWAYS_RUN_LABELS = frozenset({"pr-fast", "gpu", "gpu-health", "gpu-probe", "host", "lint",
                               "docs", "ci", "drift"})
ALWAYS_RUN_NAME_RE = re.compile(r"drift|census|registry|sync|guard|lint|inventory|probe", re.I)
# Tracked files whose content only reaches a compiled test through its own
# bytes (so the binary hash already covers them), or that nothing reads at
# run time. Everything else is the runtime surface.
COMPILED_ONLY_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".mm", ".m", ".h", ".hh", ".hpp", ".hxx",
                          ".inl", ".ipp", ".swift", ".metal", ".rs", ".md")
NON_RUNTIME_PREFIXES = ("docs/", ".agents/", ".claude/", ".codex/", ".github/", "planning")
UNDECLARABLE_EXES = ("cmake", "ninja", "make", "ctest", "xcodebuild", "cargo")
VOLATILE_PROPERTIES = frozenset({"COST", "PROCESSORS", "RESOURCE_LOCK", "RUN_SERIAL",
                                 "_BACKTRACE_TRIPLES", "LABELS"})


def _h(*parts: str) -> str:
    h = hashlib.sha256()
    for p in parts:
        h.update(p.encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()


# --------------------------------------------------------------------------
# Inputs from the checkout and the build


def tracked_blobs(repo: Path, rev: str = "HEAD") -> dict[str, str]:
    out = subprocess.run(["git", "-C", str(repo), "ls-tree", "-r", "--full-tree", rev],
                         capture_output=True, text=True, check=True).stdout
    blobs = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) >= 3:
            blobs[path] = parts[2]
    return blobs


def is_runtime_surface(path: str) -> bool:
    return not (path.endswith(COMPILED_ONLY_SUFFIXES) or path.startswith(NON_RUNTIME_PREFIXES))


def runtime_surface_digest(blobs: dict[str, str]) -> str:
    return _h("runtime-surface", *(f"{b}\t{p}" for p, b in sorted(blobs.items()) if is_runtime_surface(p)))


def policy_digest(blobs: dict[str, str]) -> str:
    return _h("policy", *(f"{p}\t{blobs.get(p, 'absent')}" for p in POLICY_PATHS))


def declared_inputs_digest(inputs: Iterable[str], blobs: dict[str, str]) -> str:
    """Blob ids of the declared inputs; a directory input covers every tracked
    file under it, an absent one is recorded as absent (never skipped over)."""
    ordered = _sorted_paths(blobs)
    rows = []
    for i in sorted(set(inputs)):
        i = i.rstrip("/")
        if i in blobs:
            rows.append(f"{blobs[i]}\t{i}")
            continue
        prefix = i + "/"
        lo = bisect.bisect_left(ordered, prefix)
        under = []
        for path in ordered[lo:]:
            if not path.startswith(prefix):
                break
            under.append(f"{blobs[path]}\t{path}")
        rows.extend(under or [f"absent\t{i}"])
    return _h("declared", *rows)


_SORTED: dict[int, list[str]] = {}


def _sorted_paths(blobs: dict[str, str]) -> list[str]:
    key = id(blobs)
    if key not in _SORTED or len(_SORTED[key]) != len(blobs):
        _SORTED.clear()
        _SORTED[key] = sorted(blobs)
    return _SORTED[key]


class FileHasher:
    """sha256 of a file, cached per realpath for one run; seeded with a
    precomputed identity (binary_identity_shadow's `our-identity.json`)."""

    def __init__(self, seed: dict[str, str] | None = None) -> None:
        self.cache: dict[str, str] = dict(seed or {})

    def __call__(self, path: str) -> str:
        real = os.path.realpath(path)
        if real not in self.cache:
            h = hashlib.sha256()
            with open(real, "rb") as fh:
                for chunk in iter(lambda: fh.read(1024 * 1024), b""):
                    h.update(chunk)
            self.cache[real] = h.hexdigest()
        return self.cache[real]


def products_digest(build_dir: Path, test_executables: set[str], hasher: FileHasher) -> tuple[str, int]:
    """sha256 over every executable regular file in the build directory that is
    not a test binary (the CLI, plugin bundle executables, helper tools)."""
    rows = []
    skip_dirs = {"CMakeFiles", "_deps", "Testing", ".cmake"}
    for root, dirs, files in os.walk(build_dir):
        dirs[:] = sorted(d for d in dirs if d not in skip_dirs and not d.endswith("-ios"))
        for f in sorted(files):
            full = os.path.join(root, f)
            if f.endswith((".o", ".a")) or os.path.islink(full):
                continue
            try:
                if not os.access(full, os.X_OK) or not os.path.isfile(full):
                    continue
            except OSError:
                continue
            real = os.path.realpath(full)
            if real in test_executables:
                continue
            rows.append(f"{hasher(real)}\t{os.path.relpath(full, build_dir)}")
    return _h("products", *rows), len(rows)


def toolchain_digest(build_dir: Path) -> str:
    try:
        tc = pmr.toolchain_identity("macos", build_dir)["digest"]
    except Exception as exc:  # noqa: BLE001 - an unknown toolchain keys nothing
        raise RuntimeError(f"toolchain identity unavailable: {exc}") from exc
    try:
        os_build = " ".join(subprocess.run(["sw_vers"], capture_output=True, text=True,
                                           timeout=30).stdout.split())
    except (OSError, subprocess.SubprocessError):
        os_build = "unknown"
    return _h("toolchain", tc, os_build)


# --------------------------------------------------------------------------
# Keys


def _props(test: dict) -> dict:
    return {p["name"]: p["value"] for p in test.get("properties", []) if "name" in p}


def labels(test: dict) -> set[str]:
    return set(_props(test).get("LABELS") or [])


def normalise(value, roots: list[tuple[str, str]]):
    if isinstance(value, str):
        for real, token in roots:
            value = value.replace(real, token)
        return value
    if isinstance(value, list):
        return [normalise(v, roots) for v in value]
    if isinstance(value, dict):
        return {k: normalise(v, roots) for k, v in value.items()}
    return value


def always_run_reason(test: dict, script_inputs: dict[str, list[str]]) -> str | None:
    name = test.get("name", "")
    if ALWAYS_RUN_NAME_RE.search(name):
        return "tree-reader-or-probe"
    if labels(test) & ALWAYS_RUN_LABELS:
        return "label"
    for i in script_inputs.get(name, []):
        if "/" not in i.strip("/") and "." not in i:
            return "declares-a-top-level-directory"
    return None


def test_key(test: dict, ctx: dict) -> tuple[str | None, str]:
    """(key, kind) for one ctest entry, or (None, why-unkeyable).

    `ctx` carries: build_dir, source_root (Paths), blobs, script_inputs,
    hasher, toolchain, policy, runtime_surface, products."""
    build_dir, source_root = ctx["build_dir"], ctx["source_root"]
    cmd = test.get("command") or []
    if not cmd:
        return None, "no-command"
    exe = os.path.basename(cmd[0])
    if exe in UNDECLARABLE_EXES:
        return None, "nested-build"
    # Both spellings of each root: ctest records the configured path, which
    # may reach the tree through a symlink (macOS /var -> /private/var).
    roots = sorted({(os.path.realpath(str(build_dir)), "<build>"), (str(build_dir), "<build>"),
                    (os.path.realpath(str(source_root)), "<src>"), (str(source_root), "<src>")},
                   key=lambda r: -len(r[0]))
    props = {k: v for k, v in _props(test).items() if k not in VOLATILE_PROPERTIES}
    binary = ats.is_binary_test(test, build_dir)
    files = []
    for path in sorted(ats.test_inputs(test, build_dir, source_root)):
        if os.path.isdir(path):
            real_build = os.path.realpath(str(build_dir))
            rel = os.path.relpath(path, os.path.realpath(str(source_root)))
            # The build directory usually lives inside the checkout; test it
            # first so a build subdirectory is never read as a source one.
            if path == real_build or path.startswith(real_build + os.sep) or rel.startswith(".."):
                return None, "names-a-build-directory"
            if rel in (".", ""):
                return None, "names-the-source-root"
            files.append(f"dir\t{rel}\t{declared_inputs_digest([rel], ctx['blobs'])}")
        elif os.path.isfile(path):
            files.append(f"file\t{normalise(path, roots)}\t{ctx['hasher'](path)}")
    try:
        exe_hash = ctx["hasher"](cmd[0])
    except OSError:
        return None, "executable-unreadable"
    parts = [f"key-v{KEY_VERSION}", json.dumps(normalise(cmd, roots)),
             json.dumps(normalise(props, roots), sort_keys=True), exe_hash, *files,
             ctx["toolchain"], ctx["policy"]]
    if binary:
        parts += ["binary", ctx["runtime_surface"], ctx["products"]]
        return _h(*parts), "binary"
    name = test.get("name", "")
    if name not in ctx["script_inputs"]:
        return None, "undeclared-script"
    parts += ["script", declared_inputs_digest(ctx["script_inputs"][name], ctx["blobs"])]
    return _h(*parts), "script"


# --------------------------------------------------------------------------
# Results, receipts and the shadow verdict


def junit_results(path: Path) -> dict[str, tuple[str, float]]:
    """name -> (pass|fail|skip, seconds)."""
    out: dict[str, tuple[str, float]] = {}
    for case in ET.parse(path).getroot().iter("testcase"):
        name = case.get("name") or ""
        if not name:
            continue
        tags = {c.tag for c in case}
        status = ("fail" if tags & {"failure", "error"} else
                  "skip" if "skipped" in tags or case.get("status") == "notrun" else "pass")
        try:
            secs = float(case.get("time") or 0.0)
        except ValueError:
            secs = 0.0
        out[name] = (status, secs)
    return out


def build_receipt(run_id: str, sha: str, keys: dict[str, str | None],
                  results: dict[str, tuple[str, float]]) -> dict:
    passed = {}
    for name, key in keys.items():
        status, secs = results.get(name, ("absent", 0.0))
        if key and status == "pass":
            passed[key] = {"name": name, "seconds": round(secs, 3)}
    failed = sorted(n for n, (s, _) in results.items() if s == "fail")
    return {"schema": RECEIPT_SCHEMA, "key_version": KEY_VERSION, "run_id": str(run_id),
            "sha": sha, "passed": passed, "failed": failed}


def is_control(run_id: int | None) -> bool:
    return run_id is not None and run_id % CONTROL_EVERY == 0


def evaluate(tests: list[dict], keys: dict[str, tuple[str | None, str]],
             prior: list[dict], results: dict[str, tuple[str, float]],
             script_inputs: dict[str, list[str]], cmake_changed: bool,
             run_id: int | None) -> dict:
    """What a receipt-keyed skip would have done in this run, against what the
    run really did."""
    known: dict[str, str] = {}
    recent_failed: set[str] = set()
    for r in prior:
        for k, v in (r.get("passed") or {}).items():
            known.setdefault(k, str(r.get("run_id")))
        recent_failed.update(r.get("failed") or [])
    would_skip, reasons = [], {}
    for t in tests:
        name = t.get("name", "")
        key, kind = keys.get(name, (None, "not-keyed"))
        why = always_run_reason(t, script_inputs)
        if why is None and name in recent_failed:
            why = "failed-recently"
        if why is None and key is None:
            why = f"unkeyed:{kind}"
        if why is None and key not in known:
            why = "no-receipt"
        if why is None:
            would_skip.append(name)
        else:
            cls = why.split(":")[0]
            reasons[cls] = reasons.get(cls, 0) + 1
    skip_failed = sorted(n for n in would_skip if results.get(n, ("", 0))[0] == "fail")
    forced = "cmake-changed" if cmake_changed else ("control" if is_control(run_id) else None)
    return {"schema": SCHEMA, "mode": "shadow", "selected": len(tests),
            "keyed": sum(1 for k, _ in keys.values() if k), "receipt_runs": len(prior),
            "receipt_keys": len(known), "would_skip": len(would_skip),
            "would_skip_seconds": round(sum(results.get(n, ("", 0.0))[1] for n in would_skip), 1),
            "would_skip_failed": len(skip_failed), "would_skip_failed_names": skip_failed[:50],
            "ran_reasons": dict(sorted(reasons.items())), "would_force_full": forced,
            "run_failed": sum(1 for s, _ in results.values() if s == "fail")}


def summary_line(v: dict) -> str:
    forced = f"; this run would run in full ({v['would_force_full']})" if v.get("would_force_full") else ""
    return (f"- Per-test receipts (shadow): would skip {v['would_skip']} of {v['selected']} tests, "
            f"~{v['would_skip_seconds']:.0f} s test time, from {v['receipt_runs']} receipt run(s); "
            f"would-skip tests that failed here: **{v['would_skip_failed']}**{forced}\n")


# --------------------------------------------------------------------------
# Receipt storage: workflow artifacts from trusted merge-group runs


def _fetch_json(url: str, token: str) -> dict:
    req = urllib.request.Request(url, headers={
        "Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.urlopen(req, timeout=60) as resp:  # noqa: S310 - fixed GitHub host
        return json.load(resp)


def _download(url: str, token: str) -> bytes:
    req = urllib.request.Request(url, headers={
        "Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.urlopen(req, timeout=60) as resp:  # noqa: S310 - GitHub API / blob redirect
        return resp.read(MAX_RECEIPT_BYTES + 1)


def trust_refusal(repository: str, artifact: dict, run: dict, receipt: dict | None) -> str | None:
    run_id = (artifact.get("workflow_run") or {}).get("id")
    if run.get("id") != run_id:
        return "run id differs from the artifact's run"
    if run.get("path") != WORKFLOW_PATH:
        return f"not written by {WORKFLOW_PATH}"
    if run.get("event") != TRUSTED_EVENT:
        return f"event {run.get('event')!r} is not {TRUSTED_EVENT}"
    for key in ("repository", "head_repository"):
        if (run.get(key) or {}).get("full_name") != repository:
            return f"{key} is not {repository}"
    if receipt is None:
        return "receipt unreadable"
    if receipt.get("schema") != RECEIPT_SCHEMA or receipt.get("key_version") != KEY_VERSION:
        return "receipt schema or key version differs"
    if str(receipt.get("run_id")) != str(run_id):
        return "receipt run differs from the artifact's run"
    return None


def _unzip_receipt(archive: bytes) -> dict | None:
    if len(archive) > MAX_RECEIPT_BYTES:
        return None
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            if bundle.namelist() != [RECEIPT_FILE]:
                return None
            return json.loads(bundle.read(RECEIPT_FILE))
    except (zipfile.BadZipFile, json.JSONDecodeError, KeyError):
        return None


def load_receipts(repository: str, token: str, exclude_run: str | None,
                  fetch: Callable = _fetch_json, download: Callable = _download,
                  lookback: int = LOOKBACK, budget_secs: float = READ_BUDGET_SECS,
                  clock: Callable[[], float] = time.monotonic) -> tuple[list[dict], list[str]]:
    """The newest `lookback` trusted receipts, read within `budget_secs` (the
    shadow must not hold the gate job), and the reasons any were refused."""
    start = clock()
    url = (f"https://api.github.com/repos/{repository}/actions/artifacts?"
           + urllib.parse.urlencode({"name": ARTIFACT_NAME, "per_page": 100}))
    arts = [a for a in fetch(url, token).get("artifacts", [])
            if a.get("name") == ARTIFACT_NAME and not a.get("expired")]
    arts.sort(key=lambda a: a.get("created_at") or "", reverse=True)
    receipts, refusals = [], []
    for art in arts:
        if len(receipts) >= lookback:
            break
        if clock() - start > budget_secs:
            refusals.append(f"read budget of {budget_secs:.0f}s spent; {len(receipts)} receipt run(s) read")
            break
        run_id = (art.get("workflow_run") or {}).get("id")
        if exclude_run is not None and str(run_id) == str(exclude_run):
            continue
        try:
            run = fetch(f"https://api.github.com/repos/{repository}/actions/runs/{run_id}", token)
            receipt = _unzip_receipt(download(art["archive_download_url"], token))
        except Exception as exc:  # noqa: BLE001 - a bad candidate is refused, never trusted
            refusals.append(f"run {run_id}: {exc}")
            continue
        why = trust_refusal(repository, art, run, receipt)
        if why:
            refusals.append(f"run {run_id}: {why}")
        else:
            receipts.append(receipt)
    return receipts, refusals


# --------------------------------------------------------------------------


def compute_keys(tests: list[dict], build_dir: Path, source_root: Path,
                 script_inputs: dict[str, list[str]], hasher: FileHasher,
                 toolchain: str, blobs: dict[str, str]) -> dict[str, tuple[str | None, str]]:
    test_exes = {os.path.realpath(t["command"][0]) for t in tests
                 if t.get("command") and ats.is_binary_test(t, build_dir)}
    products, _count = products_digest(build_dir, test_exes, hasher)
    ctx = {"build_dir": build_dir, "source_root": source_root, "blobs": blobs,
           "script_inputs": script_inputs, "hasher": hasher, "toolchain": toolchain,
           "policy": policy_digest(blobs), "runtime_surface": runtime_surface_digest(blobs),
           "products": products}
    return {t.get("name", ""): test_key(t, ctx) for t in tests}


def cmd_run(a: argparse.Namespace) -> int:
    t0 = time.monotonic()
    build_dir = Path(a.build_dir).resolve()
    source_root = Path(a.source_root).resolve()
    try:
        tests = json.loads(Path(a.selected_json).read_text(encoding="utf-8")).get("tests", [])
        results = junit_results(Path(a.junit))
        blobs = tracked_blobs(source_root, a.merge_sha)
        base = subprocess.run(["git", "-C", str(source_root), "rev-parse", "--verify", "--quiet",
                               f"{a.merge_sha}^1"], capture_output=True, text=True).stdout.strip()
        changed = (subprocess.run(["git", "-C", str(source_root), "diff", "--name-only", base, a.merge_sha],
                                  capture_output=True, text=True, check=True).stdout.split() if base else [])
        seed = {}
        if a.identity_json and Path(a.identity_json).is_file():
            ident = json.loads(Path(a.identity_json).read_text(encoding="utf-8"))
            seed = {os.path.realpath(str(build_dir / f["path"])): f["sha256"] for f in ident.get("files", [])}
        toolchain = _h("toolchain-override", a.toolchain_id) if a.toolchain_id else toolchain_digest(build_dir)
        keys = compute_keys(tests, build_dir, source_root, ats.load_script_inputs(source_root),
                            FileHasher(seed), toolchain, blobs)
    except (OSError, ValueError, subprocess.CalledProcessError, ET.ParseError, RuntimeError) as exc:
        print(f"test-receipts shadow: no verdict, input unreadable: {exc}", file=sys.stderr)
        return 2
    keyed_at = time.monotonic()
    receipt = build_receipt(a.run_id, a.merge_sha, {n: k for n, (k, _) in keys.items()}, results)
    Path(a.receipts_out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.receipts_out).write_text(json.dumps(receipt, sort_keys=True), encoding="utf-8")
    if a.keys_out:
        # Every entry's key or unkeyable reason, passed or not: the per-job
        # reuse record (tools/ci/reuse_record.py) carries it as output_key.
        Path(a.keys_out).write_text(json.dumps({n: {"key": k, "kind": kind} for n, (k, kind) in keys.items()},
                                               sort_keys=True), encoding="utf-8")
    prior, refusals = [], []
    if a.token:
        try:
            prior, refusals = load_receipts(a.repository, a.token, a.run_id)
        except Exception as exc:  # noqa: BLE001 - no receipts is a verdict of zero hits, stated
            refusals.append(f"listing failed: {exc}")
    for why in refusals[:10]:
        print(f"test-receipts shadow: receipt refused: {why}", file=sys.stderr)
    cmake_changed = ats.classify_changes(changed)["cmake_changed"] if base else True
    verdict = evaluate(tests, keys, prior, results, ats.load_script_inputs(source_root),
                       cmake_changed, int(a.run_id) if str(a.run_id).isdigit() else None)
    verdict.update({"receipts_written": len(receipt["passed"]), "refused_receipts": len(refusals),
                    "key_seconds": round(keyed_at - t0, 1),
                    "seconds": round(time.monotonic() - t0, 1)})
    print(f"test-receipts shadow: would skip {verdict['would_skip']} of {verdict['selected']} "
          f"(~{verdict['would_skip_seconds']:.0f} s); would-skip tests that failed: "
          f"{verdict['would_skip_failed']}; wrote {verdict['receipts_written']} receipts")
    print(f"::notice title={TITLE}::{json.dumps(verdict, sort_keys=True)}")
    summary = a.summary or os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(summary_line(verdict))
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--build-dir", required=True)
    r.add_argument("--source-root", required=True)
    r.add_argument("--repository", required=True)
    r.add_argument("--merge-sha", required=True)
    r.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    r.add_argument("--junit", required=True)
    r.add_argument("--selected-json", required=True)
    r.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    r.add_argument("--receipts-out", required=True)
    r.add_argument("--identity-json", default=None)
    r.add_argument("--keys-out", default=None, help="write every entry's key (or unkeyable kind) here")
    r.add_argument("--summary", default=None)
    r.add_argument("--toolchain-id", default=None, help="override the probed toolchain identity (tests)")
    r.set_defaults(func=cmd_run)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
