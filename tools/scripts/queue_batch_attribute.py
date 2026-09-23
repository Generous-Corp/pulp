#!/usr/bin/env python3
"""Attribute a failed merge_group batch to the PR that owns its failing tests.

A merge_group batch is NAMED for one PR but CONTAINS every entry ahead of it,
so the branch name is not the culprit. Reading it as one is the misattribution
that cost 2026-09-23 roughly two hours: three batches were blamed on their
names while a single PR failed all of them.

The signal that works: a failing ctest name usually maps to a file, and the PR
that ADDS or TOUCHES that file owns the failure. `prepush-cannot-measure` ->
tools/scripts/test_prepush_cannot_measure.py -> the one PR adding it.

Read-only. Prints findings; mutates nothing.
"""
import json, re, subprocess, sys

REPO = "Generous-Corp/pulp"


def gh(path, jq=None):
    cmd = ["ghapp", "api", path] + (["--jq", jq] if jq else [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return None
    return p.stdout.strip()


def failing_tests(run_id):
    """ctest names that failed in a run's macos job."""
    jid = gh(f"repos/{REPO}/actions/runs/{run_id}/jobs?per_page=50",
             '.jobs[]|select(.name=="macos")|.id')
    if not jid:
        return []
    jid = jid.splitlines()[0]
    log = gh(f"repos/{REPO}/actions/jobs/{jid}/logs")
    if not log:
        return []
    names, seen = [], False
    for line in log.splitlines():
        if "The following tests FAILED" in line:
            seen = True
            continue
        if seen:
            m = re.search(r"\d+\s+-\s+(.+?)\s+\((Failed|Timeout|Subprocess aborted)\)", line)
            if m:
                names.append(m.group(1).strip())
            elif "Errors while running CTest" in line:
                break
    return names


def slug(test_name):
    """A ctest name -> tokens likely to appear in the owning file path."""
    return [t for t in re.split(r"[^a-z0-9]+", test_name.lower()) if len(t) > 3]


def open_prs():
    raw = gh(f"repos/{REPO}/pulls?state=open&per_page=100", ".[].number")
    return [int(n) for n in raw.splitlines()] if raw else []


def pr_files(n):
    raw = gh(f"repos/{REPO}/pulls/{n}/files?per_page=100", ".[].filename")
    return raw.splitlines() if raw else []


def main():
    run_id = sys.argv[1] if len(sys.argv) > 1 else None
    if not run_id:
        raw = gh(f"repos/{REPO}/actions/workflows/build.yml/runs?event=merge_group&per_page=20",
                 '[.workflow_runs[]|select(.conclusion=="failure")]|.[0].id')
        run_id = (raw or "").strip()
    if not run_id:
        print("no failed merge_group batch found")
        return 0

    tests = failing_tests(run_id)
    if not tests:
        print(f"run {run_id}: no ctest failure block (failure is not a test failure)")
        return 0

    print(f"batch run {run_id}: {len(tests)} failing test(s)")
    files = {n: pr_files(n) for n in open_prs()}
    scores = {}
    for t in tests:
        toks = slug(t)
        for n, fl in files.items():
            for f in fl:
                fl_low = f.lower()
                # Score match STRENGTH, not just token count. An exact
                # slug match (prepush-cannot-measure -> prepush_cannot_measure)
                # is decisive; incidental overlap on generic tokens like
                # "sdk"/"consumer" is not, and tie-breaking on count alone
                # picks the wrong PR.
                canon = re.sub(r"[^a-z0-9]+", "_", t.lower()).strip("_")
                stem = fl_low.rsplit("/", 1)[-1].rsplit(".", 1)[0]
                stem = re.sub(r"^test_", "", stem)
                if canon and canon == stem:
                    weight = 100           # exact test-name == file stem
                elif canon and canon in fl_low.replace("-", "_"):
                    weight = 50            # test name contained in the path
                else:
                    hits = sum(1 for tok in toks if tok in fl_low)
                    weight = hits if hits >= 2 else 0
                if weight:
                    prev = scores.setdefault(n, {}).setdefault(t, (0, set()))
                    scores[n][t] = (max(prev[0], weight), prev[1] | {f})

    if not scores:
        print("  NO OPEN PR OWNS THESE TESTS -> likely pre-existing on main, or an")
        print("  entry already merged. Do NOT blame the batch's branch name.")
        for t in tests[:6]:
            print(f"    - {t}")
        return 0

    strength = {n: sum(w for w, _ in ts.values()) for n, ts in scores.items()}
    # A WEAK match is not a culprit. Incidental token overlap (e.g.
    # "cmake-control-sdk-consumer" brushing test_gpu_audio_sdk_consumer.cmake)
    # scores 2; an exact test-name/file-stem match scores 100. Reporting the
    # former as CULPRIT is a false accusation, and a false accusation is worse
    # than no attribution - it sends someone to fix an innocent PR while the
    # real break sits on main. Below the threshold, say so plainly.
    CONFIDENT = 50
    if strength and max(strength.values()) < CONFIDENT:
        print(f"  {len(tests)} failing test(s); NO open PR matches any of them"
              f" strongly (best score {max(strength.values())}).")
        print("  => LIKELY PRE-EXISTING ON MAIN. Do not blame a batch member;")
        print("     check whether main's own suite is red before re-queueing.")
        for n, ts in sorted(scores.items(), key=lambda kv: -strength[kv[0]])[:3]:
            t, (w, fs) = list(ts.items())[0]
            print(f"       (weak, {w}) #{n}: {t} ~ {sorted(fs)[0]}")
        return 0
    for n, ts in sorted(scores.items(), key=lambda kv: -strength[kv[0]]):
        print(f"  #{n} owns {len(ts)} failing test(s), match strength {strength[n]}:")
        for t, (w, fs) in list(ts.items())[:4]:
            print(f"      [{w:>3}] {t}  <- {sorted(fs)[0]}")
    top = max(strength, key=lambda n: strength[n])
    print(f"  => CULPRIT: #{top}. Do not re-arm it until fixed; it fails every batch it joins.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
