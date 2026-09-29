#!/usr/bin/env python3
"""Content digest of everything the iOS compile gate can read, and the skip it keys.

The iOS compile gate (`test/cmake/test_ios_compile_gate.sh`) is the largest
piece of the required `macos` gate's Build step (6–13 min per job, three to
four times the whole macOS Ninja build), and it re-runs on every PR-head push
and every merge group whose diff the path allowlist cannot clear — 77% of
merge groups and 100% of PR-head runs, although 64% of consecutive PR-head
pushes and 23% of merge groups leave every file it compiles untouched.

This tool gives each gate run an exact identity: a SHA-256 over the blob ids
of every tracked file the gate's three configures can reach, plus the Apple
toolchain that compiles them. Two trees with the same digest cannot produce a
different gate result. The workflow records a successful run under its digest
(a workflow artifact named `ios-gate-ok-<digest>`) and, before the next run,
looks the digest up.

`decide` turns the lookup into the gate's action. In ENFORCE mode (the default,
decisions contract row 23) the gate is skipped when a PASS receipt for the same
digest exists AND was written by a trusted gate run: an unexpired artifact from
this repository's own `build.yml`, on a `merge_group` or same-repository
`pull_request` run (the trust root `protected_merge_receipt.py` already uses),
whose marker names the same digest and run. Anything else runs the gate:
no receipt, an untrusted or unreadable one, an API error. One run in
`CONTROL_EVERY` (by run id) and every `schedule`/`push` run is a CONTROL: it
runs the gate even with a trusted receipt, so the "a matching digest cannot
fail" claim keeps being measured (`control_run` then `ran_ok`/`ran_failed`).
SHADOW mode (`PULP_IOS_GATE_DIGEST_MODE=shadow`, a repository variable) keeps
the old behaviour: annotate `would_skip` and run anyway; `off` skips the lookup.

Proxies read from the job annotations (`pulp-ios-gate-shadow/v1`):
- `skipped` (+ `would_skip`) ÷ gate runs, merge group and PR head separately;
- the safety control: `control_run` or `would_skip` verdicts whose real gate
  run then FAILED. It was 0 over the shadow window that justified enforcement
  and must stay 0; one such failure means the input set is too narrow, and the
  mode goes back to `shadow` until the missing input is added.

Input set. The gate configures with PULP_BUILD_TESTS=OFF for the two Ninja
SDK legs and PULP_BUILD_EXAMPLES=ON + PULP_ENABLE_GPU=ON for the Xcode GPU
leg, so its inputs are the product sources, examples, every CMake file,
dependency pins, the Skia fetch script, the gate's own scripts, and the
test/docs paths that non-test CMake names (`IOS_COMPILE_REQUIRED_PATTERNS`).
The set below is deliberately a SUPERSET of what the configures list: a path
that is in the set but unread costs a needless re-run, a path that is read but
not in the set would let a change through, so the set errs wide. `*.md` files
are excluded everywhere: no CMake or compiler reads them.

Usage:
    ios_gate_digest.py compute --repo . [--rev HEAD] [--toolchain-id ID]
    ios_gate_digest.py decide --repo . --repository O/R --token T --run-id N
                              [--event E] [--mode enforce|shadow|off] --env-out F
    ios_gate_digest.py lookup --repository O/R --token T --digest D
    ios_gate_digest.py note --verdict would_skip|run|ran_ok|ran_failed --digest D
                            [--source-run-id N] [--event E]
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import subprocess
import sys
import io
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

SCHEMA = "pulp-ios-gate-shadow/v1"
DIGEST_VERSION = "1"
ARTIFACT_PREFIX = "ios-gate-ok-"
WORKFLOW_PATH = ".github/workflows/build.yml"
# Runs whose passing marker may skip a later gate: the merge queue's own runs
# and same-repository PR heads, both on the gate runners. A fork PR never
# qualifies (its head repository differs).
TRUSTED_EVENTS = ("merge_group", "pull_request")
MODES = ("enforce", "shadow", "off")
# One gate run in CONTROL_EVERY (run id modulo) runs even with a trusted
# receipt, as do the scheduled and push-to-main runs, so the false-skip rate
# stays measured after enforcement.
CONTROL_EVERY = 10
CONTROL_EVENTS = ("schedule", "push")
MAX_CANDIDATES = 5

# Paths the gate's configures can read (see the module docstring). Matched
# with fnmatch against the repository-relative path; `**` here means "any
# path under", so patterns are checked both as written and as a prefix.
INPUT_PATTERNS = (
    "core/**",
    "examples/**",
    "apple/**",
    "inspect/**",
    "external/**",
    "cmake/**",
    "tools/cmake/**",
    "tools/deps/**",
    "CMakeLists.txt",
    "**/CMakeLists.txt",
    "**/*.cmake",
    "setup.sh",
    "tools/scripts/fetch_skia_for_release.py",
    "test/cmake/test_ios_*",
    # The digest's own definition: a change to what it covers must not match a
    # receipt written under the old definition.
    "tools/ci/ios_gate_digest.py",
    # The build wrapper both legs run through.
    "tools/ci/governed-build.sh",
    # The GPU leg's AUv3 post-build step runs the Three.js bundler, which
    # `npm install`s the pinned esbuild from this package manifest + lockfile
    # (tools/cmake/PulpAuv3.cmake).
    "tools/scripts/bundle_threejs_for_jsc.mjs",
    "tools/scripts/package.json",
    "tools/scripts/package-lock.json",
)
# Directories the root CMakeLists.txt adds on EVERY configure, iOS included
# (not behind NOT IOS). Their CMake files are inputs through `**/CMakeLists.txt`;
# the sources those files list are read too (existence at generate time, and
# compiled when a gate target depends on them). Their scripts, web assets and
# docs are not: no configure or compile of these directories reads them.
CONFIGURE_REACHED_DIRS = (
    "ship/",
    "tools/cli/gpu_health/",
    "tools/audio/",
    "tools/design-ab/",
    "tools/screenshot/",
    "tools/appearance/",
    "tools/design/",
    "tools/import-design/",
)
NOT_CONFIGURE_READ_SUFFIXES = (".py", ".mjs", ".js", ".cjs", ".ts", ".tsx", ".html", ".css",
                               ".svg", ".png", ".jpg", ".md")
EXCLUDE_PATTERNS = ("*.md", "**/*.md")


def _required_patterns() -> tuple[str, ...]:
    """`IOS_COMPILE_REQUIRED_PATTERNS` from classify_changes.py: the test/docs
    paths the iOS configures name. Read from the sibling module so the two
    lists cannot drift; absent (a partial checkout) they are simply not added,
    which only widens nothing and is reported by `--print-inputs`."""
    here = Path(__file__).resolve().parent.parent / "scripts"
    sys.path.insert(0, str(here))
    try:
        import classify_changes  # type: ignore
        return tuple(classify_changes.IOS_COMPILE_REQUIRED_PATTERNS)
    except Exception:  # pragma: no cover - partial checkouts
        return ()
    finally:
        sys.path.pop(0)


def _match(path: str, pattern: str) -> bool:
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-2])
    if fnmatch.fnmatchcase(path, pattern):
        return True
    # `**/x` also matches a top-level `x`.
    return pattern.startswith("**/") and fnmatch.fnmatchcase(path, pattern[3:])


def is_input(path: str, patterns: tuple[str, ...] | None = None) -> bool:
    if any(_match(path, p) for p in EXCLUDE_PATTERNS):
        return False
    if patterns is None and path.startswith(CONFIGURE_REACHED_DIRS) \
            and not path.endswith(NOT_CONFIGURE_READ_SUFFIXES):
        return True
    pats = patterns if patterns is not None else INPUT_PATTERNS + _required_patterns()
    return any(_match(path, p) for p in pats)


def tracked_blobs(repo: Path, rev: str) -> list[tuple[str, str]]:
    """(path, blob sha) for every tracked file at `rev`, submodule gitlinks
    included as their commit id (a moved gitlink is a changed input)."""
    out = subprocess.run(["git", "-C", str(repo), "ls-tree", "-r", "--full-tree", rev],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) >= 3:
            rows.append((path, parts[2]))
    return rows


def toolchain_id() -> str:
    """Apple toolchain identity. Off macOS, or without Xcode, `unknown`, which
    still yields a valid (if never matching cross-host) digest."""
    parts = []
    for cmd in (["xcodebuild", "-version"], ["node", "--version"],
                ["xcrun", "--sdk", "iphoneos", "--show-sdk-version"],
                ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-version"]):
        try:
            parts.append(" ".join(subprocess.run(cmd, capture_output=True, text=True,
                                                 check=True, timeout=60).stdout.split()))
        except Exception:
            parts.append("unknown")
    return " | ".join(parts)


def compute(repo: Path, rev: str = "HEAD", toolchain: str | None = None,
            patterns: tuple[str, ...] | None = None) -> dict:
    blobs = [(p, b) for p, b in tracked_blobs(repo, rev) if is_input(p, patterns)]
    tc = toolchain if toolchain is not None else toolchain_id()
    h = hashlib.sha256()
    h.update(f"{SCHEMA} digest-v{DIGEST_VERSION}\n".encode())
    h.update(f"toolchain\t{tc}\n".encode())
    for path, blob in sorted(blobs):
        h.update(f"{blob}\t{path}\n".encode())
    return {"digest": h.hexdigest(), "inputs": len(blobs), "toolchain": tc,
            "rev": subprocess.run(["git", "-C", str(repo), "rev-parse", rev],
                                  capture_output=True, text=True, check=True).stdout.strip()}


def artifact_name(digest: str) -> str:
    return ARTIFACT_PREFIX + digest


def lookup(repository: str, token: str, digest: str, fetch=None) -> dict:
    """Is there an unexpired `ios-gate-ok-<digest>` artifact? `fetch(url, token)`
    returns the parsed JSON page; injected by tests."""
    fetch = fetch or _fetch_json
    name = artifact_name(digest)
    url = (f"https://api.github.com/repos/{repository}/actions/artifacts?"
           + urllib.parse.urlencode({"name": name, "per_page": 100}))
    page = fetch(url, token)
    live = [a for a in page.get("artifacts", []) if a.get("name") == name and not a.get("expired")]
    live.sort(key=lambda a: a.get("created_at") or "")
    if not live:
        return {"found": False, "digest": digest, "candidates": len(page.get("artifacts", []))}
    src = live[-1]
    return {"found": True, "digest": digest,
            "source_run_id": (src.get("workflow_run") or {}).get("id"),
            "created_at": src.get("created_at"), "candidates": len(live)}


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
        return resp.read(1024 * 1024 + 1)


def _check_trusted(repository: str, digest: str, artifact: dict, run: dict, archive: bytes) -> str | None:
    """None when `artifact` is a trusted PASS receipt for `digest`, else the reason."""
    wr = artifact.get("workflow_run") or {}
    run_id = wr.get("id")
    if run.get("id") != run_id:
        return "run id differs from the artifact's run"
    if run.get("path") != WORKFLOW_PATH:
        return f"not written by {WORKFLOW_PATH} (got {run.get('path')!r})"
    if run.get("event") not in TRUSTED_EVENTS:
        return f"event {run.get('event')!r} is not a trusted gate event"
    for key in ("repository", "head_repository"):
        if (run.get(key) or {}).get("full_name") != repository:
            return f"{key} is not {repository} (fork or foreign run)"
    if len(archive) > 1024 * 1024:
        return "marker archive exceeds size bound"
    meta = artifact.get("digest") or ""
    if meta and meta != "sha256:" + hashlib.sha256(archive).hexdigest():
        return "archive digest does not match GitHub metadata"
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            if bundle.namelist() != ["marker.json"]:
                return "unexpected archive layout"
            marker = json.loads(bundle.read("marker.json"))
    except (zipfile.BadZipFile, json.JSONDecodeError, KeyError) as exc:
        return f"marker unreadable: {exc}"
    if marker.get("schema") != SCHEMA or marker.get("digest") != digest:
        return "marker does not name this digest"
    if str(marker.get("run_id")) != str(run_id):
        return "marker run differs from the artifact's run"
    return None


def trusted_lookup(repository: str, token: str, digest: str, fetch=None, download=None) -> dict:
    """The newest trusted PASS receipt for `digest`, or why none qualifies.
    `fetch(url, token)` returns parsed JSON and `download(url, token)` bytes;
    both are injected by tests."""
    fetch = fetch or _fetch_json
    download = download or _download
    name = artifact_name(digest)
    url = (f"https://api.github.com/repos/{repository}/actions/artifacts?"
           + urllib.parse.urlencode({"name": name, "per_page": 100}))
    page = fetch(url, token)
    live = [a for a in page.get("artifacts", []) if a.get("name") == name and not a.get("expired")]
    live.sort(key=lambda a: a.get("created_at") or "", reverse=True)
    refusals = []
    for art in live[:MAX_CANDIDATES]:
        run_id = (art.get("workflow_run") or {}).get("id")
        try:
            run = fetch(f"https://api.github.com/repos/{repository}/actions/runs/{run_id}", token)
            archive = download(art["archive_download_url"], token)
        except Exception as exc:  # noqa: BLE001 - one bad candidate never skips anything
            refusals.append(f"run {run_id}: {exc}")
            continue
        why = _check_trusted(repository, digest, art, run, archive)
        if why is None:
            return {"found": True, "trusted": True, "digest": digest, "source_run_id": run_id,
                    "source_event": run.get("event"), "created_at": art.get("created_at"),
                    "candidates": len(live)}
        refusals.append(f"run {run_id}: {why}")
    return {"found": bool(live), "trusted": False, "digest": digest, "source_run_id": None,
            "candidates": len(live), "refusals": refusals}


def is_control(run_id: int | None, event: str | None) -> bool:
    """A control run executes the gate even with a trusted receipt."""
    if event in CONTROL_EVENTS:
        return True
    return run_id is not None and run_id % CONTROL_EVERY == 0


def decide(mode: str, event: str | None, run_id: int | None, hit: dict | None) -> dict:
    """The gate's action for one run: `skip` only in enforce mode, with a
    trusted receipt, on a non-control run. The verdict names why."""
    if mode not in MODES:
        mode = "shadow"
    trusted = bool(hit and hit.get("trusted"))
    src = hit.get("source_run_id") if trusted else None
    if mode == "off":
        return {"action": "run", "verdict": None, "mode": mode, "source_run_id": None,
                "reason": "digest lookup disabled"}
    if not trusted:
        return {"action": "run", "verdict": "run", "mode": mode, "source_run_id": None,
                "reason": "no trusted PASS receipt for this digest"}
    if mode == "shadow":
        return {"action": "run", "verdict": "would_skip", "mode": mode, "source_run_id": src,
                "reason": f"shadow mode; digest passed in run {src}"}
    if is_control(run_id, event):
        return {"action": "run", "verdict": "control_run", "mode": mode, "source_run_id": src,
                "reason": f"control run (1 in {CONTROL_EVERY} and every schedule/push); digest passed in run {src}"}
    return {"action": "skip", "verdict": "skipped", "mode": mode, "source_run_id": src,
            "reason": f"digest passed in trusted run {src}"}


def summary_line(decision: dict, digest: str) -> str:
    short = digest[:12] if digest else "unknown"
    if decision["action"] == "skip":
        return (f"- iOS compile gate: **SKIPPED** — input digest `{short}` passed in trusted run "
                f"{decision['source_run_id']} (`ios_gate_digest.py`, mode {decision['mode']})\n")
    return (f"- iOS compile gate: ran — {decision['reason']} (digest `{short}`, "
            f"mode {decision['mode']})\n")


def note(verdict: str, digest: str, event: str | None, source_run_id: int | None,
         mode: str = "shadow") -> str:
    """The annotation line the proxies read. Always a `notice`: the note never
    fails a job (a skip is a decision of the Build step, not of the note)."""
    record = {"schema": SCHEMA, "mode": mode, "verdict": verdict, "digest": digest,
              "event": event, "source_run_id": source_run_id}
    return f"::notice title=ios-gate-shadow::{json.dumps(record, sort_keys=True)}"


def cmd_decide(a: argparse.Namespace) -> int:
    """Compute, look up, decide; write `ios_digest/ios_action/ios_src/ios_mode`
    for the shell. Every failure path decides `run`."""
    digest, decision = "", {"action": "run", "verdict": None, "mode": a.mode,
                            "source_run_id": None, "reason": "digest unavailable"}
    try:
        digest = compute(Path(a.repo), "HEAD", a.toolchain_id)["digest"]
        hit = None
        if a.mode != "off":
            if not a.token:
                raise RuntimeError("no token for the receipt lookup")
            hit = trusted_lookup(a.repository, a.token, digest)
            for why in hit.get("refusals", []):
                print(f"iOS gate receipt refused: {why}", file=sys.stderr)
        decision = decide(a.mode, a.event, a.run_id, hit)
    except Exception as exc:  # noqa: BLE001 - the lookup is never the gate
        print(f"iOS gate digest: deciding to run ({exc})", file=sys.stderr)
        decision["reason"] = f"lookup failed ({exc}); running the gate"
    if digest and decision.get("verdict"):
        print(note(decision["verdict"], digest, a.event, decision.get("source_run_id"), decision["mode"]))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(summary_line(decision, digest))
    src = decision.get("source_run_id")
    with open(a.env_out, "w", encoding="utf-8") as fh:
        fh.write(f"ios_digest={digest if all(c in '0123456789abcdef' for c in digest) else ''}\n")
        fh.write(f"ios_action={'skip' if decision['action'] == 'skip' else 'run'}\n")
        fh.write(f"ios_src={int(src) if isinstance(src, int) else ''}\n")
        fh.write(f"ios_mode={decision['mode'] if decision['mode'] in MODES else 'shadow'}\n")
    print(f"iOS gate digest: action={decision['action']} verdict={decision.get('verdict')} "
          f"digest={digest or 'unknown'} ({decision['reason']})")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compute")
    c.add_argument("--repo", default=".")
    c.add_argument("--rev", default="HEAD")
    c.add_argument("--toolchain-id", default=None)
    c.add_argument("--print-inputs", action="store_true")
    l = sub.add_parser("lookup")
    l.add_argument("--repository", required=True)
    l.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    l.add_argument("--digest", required=True)
    n = sub.add_parser("note")
    n.add_argument("--verdict", required=True,
                   choices=("would_skip", "run", "ran_ok", "ran_failed", "skipped", "control_run"))
    n.add_argument("--digest", required=True)
    n.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME"))
    n.add_argument("--source-run-id", type=int, default=None)
    n.add_argument("--mode", choices=MODES, default="shadow")
    d = sub.add_parser("decide")
    d.add_argument("--repo", default=".")
    d.add_argument("--repository", required=True)
    d.add_argument("--token", default=os.environ.get("GITHUB_TOKEN", ""))
    d.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME"))
    d.add_argument("--run-id", type=int, default=None)
    d.add_argument("--mode", default="enforce")
    d.add_argument("--toolchain-id", default=None)
    d.add_argument("--env-out", required=True)
    a = ap.parse_args(argv[1:])
    if a.cmd == "decide":
        if a.mode not in MODES:
            print(f"decide: unknown mode {a.mode!r}; using shadow", file=sys.stderr)
            a.mode = "shadow"
        return cmd_decide(a)
    if a.cmd == "compute":
        repo = Path(a.repo)
        result = compute(repo, a.rev, a.toolchain_id)
        if a.print_inputs:
            for p, _ in sorted(tracked_blobs(repo, a.rev)):
                if is_input(p):
                    print(p, file=sys.stderr)
        print(json.dumps(result, sort_keys=True))
        return 0
    if a.cmd == "lookup":
        if not a.token:
            print("lookup: no token", file=sys.stderr)
            return 2
        try:
            print(json.dumps(lookup(a.repository, a.token, a.digest), sort_keys=True))
        except Exception as exc:  # the API is not the gate; report and continue
            print(json.dumps({"found": False, "digest": a.digest, "error": str(exc)}))
            return 3
        return 0
    print(note(a.verdict, a.digest, a.event, a.source_run_id, a.mode))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
