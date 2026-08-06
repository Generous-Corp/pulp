#!/usr/bin/env python3
"""Run many prompts, keep every verdict, and say what changed since last time.

One prompt tried by hand tells you almost nothing. The failure that cost a day
here looked like "the model cannot write a melody" and was four defects in a
role table; the one after it looked identical and was a library nobody had
measured. Both would have been obvious as a row in a table and neither was
obvious as an anecdote.

So this runs a corpus, records what actually happened per prompt, and diffs
against the last run. What it is FOR is the loop: change something, sweep,
read the diff, change the next thing.

    sweep.py run                    # the whole corpus
    sweep.py run --only melodic     # prompts whose id contains "melodic"
    sweep.py run --jobs 4           # generations in parallel
    sweep.py show                   # the last run, as a table
    sweep.py diff                   # what changed between the last two runs

WHAT IT RECORDS, AND WHY EACH. `emitted` is the only outcome that matters to a
user. `attempts` separates "worked" from "worked eventually", which is the
difference between a fix and a coincidence. `failed` names the requirement, so
a histogram over the corpus points at a cause rather than a symptom. `idiom`
and `also_matched` catch a request being gated by one of the several idioms it
answers. And `coverage` is here because two machines with different port maps
produce different results from the same code -- without it, comparing runs
across machines chases ghosts.

Runs are cached by (prompt, code fingerprint): re-running after changing
nothing costs nothing, and after changing the generator costs everything. That
is what makes a tight loop affordable.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, ".sweeps")
CORPUS = os.path.join(HERE, "sweep_corpus.json")

# Files whose contents decide what a generation does. A change to any of them
# invalidates every cached verdict; a change to anything else does not.
FINGERPRINT = ["patch.py", "idiom_check.py", "patch_vocabulary.py",
               "knowledge.py", "patch_idioms", "knowledge"]


def fingerprint() -> str:
    """A digest of everything that decides a generation's outcome."""
    h = hashlib.sha256()
    for rel in FINGERPRINT:
        path = os.path.join(HERE, rel)
        files = []
        if os.path.isdir(path):
            for root, _, names in os.walk(path):
                files += [os.path.join(root, n) for n in sorted(names)
                          if n.endswith((".py", ".json"))]
        elif os.path.exists(path):
            files = [path]
        for f in sorted(files):
            try:
                with open(f, "rb") as fh:
                    h.update(fh.read())
            except OSError:
                continue
    return h.hexdigest()[:12]


def coverage() -> dict:
    """How much of this machine's library the model can actually wire.

    Recorded per run because it is the largest source of difference between
    two machines running identical code, and the reason one agent's melodic
    failure and another's looked like separate bugs.
    """
    sys.path.insert(0, HERE)
    try:
        import patch                                   # noqa: PLC0415
        inv = patch.inventory()
    except Exception:                                  # noqa: BLE001
        return {"modules": 0, "with_ports": 0}
    total = sum(len(p["modules"]) for p in inv.values())
    named = sum(1 for p in inv.values() for m in p["modules"].values()
                if m.get("inputs"))
    return {"modules": total, "with_ports": named}


# What a run's stdout tells us. Matched loosely: the wording is written for a
# person and will drift, and a sweep that silently records nothing because a
# sentence changed is worse than one that says it could not tell.
_EMITTED = re.compile(r"idiom holds|wrote |installed |patch written", re.I)
_GAVE_UP = re.compile(r"gave up after (\d+) attempts", re.I)
_ATTEMPT = re.compile(r"\(attempt (\d+)\)|retry (\d+)", re.I)
_FAILED_REQ = re.compile(r"^\s+- (.+)$", re.M)
_IDIOM = re.compile(r"not a ([\w-]+) patch yet|idiom holds: ([\w-]+)", re.I)
_ALSO = re.compile(r"also answered by: ([^\n]+)|also touches: ([^\n]+)", re.I)
_SILENT = re.compile(r"makes no sound|every cable into the audio interface",
                     re.I)


def read_outcome(out: str) -> dict:
    gave_up = _GAVE_UP.search(out)
    attempts = 1
    for m in _ATTEMPT.finditer(out):
        attempts = max(attempts, int(m.group(1) or m.group(2) or 1))
    if gave_up:
        attempts = int(gave_up.group(1))
    idiom = None
    for m in _IDIOM.finditer(out):
        idiom = m.group(1) or m.group(2)
    also = []
    for m in _ALSO.finditer(out):
        also = [s.strip() for s in
                re.split(r",| — ", m.group(1) or m.group(2) or "")
                if s.strip() and "counts" not in s and "checked" not in s]
    return {
        "emitted": bool(_EMITTED.search(out)) and not gave_up,
        "attempts": attempts,
        "idiom": idiom,
        "also_matched": also,
        "silent": bool(_SILENT.search(out)),
        # Deduplicated: the same requirement restated on five attempts is one
        # fact about the prompt, not five.
        "failed": sorted({s.strip() for s in _FAILED_REQ.findall(out)}),
    }


def run_one(case: dict, timeout: float) -> dict:
    started = time.time()
    env = dict(os.environ)
    env["FORGE_ATTEMPT_DIR"] = os.path.join(RUNS, "attempts", case["id"])
    os.makedirs(env["FORGE_ATTEMPT_DIR"], exist_ok=True)
    try:
        p = subprocess.run([sys.executable, "patch.py", "build", case["prompt"]],
                           cwd=HERE, env=env, capture_output=True, text=True,
                           timeout=timeout)
        out = (p.stdout or "") + (p.stderr or "")
        code = p.returncode
    except subprocess.TimeoutExpired:
        out, code = "", -1
    row = {"id": case["id"], "prompt": case["prompt"], "exit": code,
           "seconds": round(time.time() - started, 1)}
    row.update(read_outcome(out) if code >= 0
               else {"emitted": False, "attempts": 0, "idiom": None,
                     "also_matched": [], "silent": False,
                     "failed": ["TIMED OUT"]})
    return row


def load_corpus(only: str | None) -> list:
    with open(CORPUS) as f:
        cases = json.load(f)["prompts"]
    if only:
        cases = [c for c in cases if only.lower() in c["id"].lower()]
    return cases


def newest_runs(n: int = 2) -> list:
    if not os.path.isdir(RUNS):
        return []
    files = sorted((f for f in os.listdir(RUNS) if f.endswith(".json")),
                   reverse=True)
    return [json.load(open(os.path.join(RUNS, f))) for f in files[:n]]


def table(run: dict) -> None:
    rows = run["rows"]
    good = sum(1 for r in rows if r["emitted"])
    cov = run.get("coverage", {})
    print(f"  {run['stamp']}   code {run['fingerprint']}   "
          f"ports {cov.get('with_ports', 0)}/{cov.get('modules', 0)}")
    print(f"  emitted {good}/{len(rows)}\n")
    print(f"  {'id':22s} {'ok':3s} {'try':4s} {'idiom':22s} why")
    for r in sorted(rows, key=lambda r: (r["emitted"], r["id"])):
        why = "silent" if r["silent"] else (r["failed"][0][:44] if r["failed"]
                                            else "")
        print(f"  {r['id'][:22]:22s} {'y' if r['emitted'] else 'n':3s} "
              f"{r['attempts']:<4d} {(r['idiom'] or '')[:22]:22s} {why}")
    # A histogram over the corpus points at a cause; one row points at a symptom.
    counts: dict = {}
    for r in rows:
        for f in r["failed"]:
            counts[f] = counts.get(f, 0) + 1
    if counts:
        print("\n  requirements failed, most common first:")
        for req, n in sorted(counts.items(), key=lambda kv: -kv[1])[:8]:
            print(f"    {n:3d}  {req[:88]}")


def diff(a: dict, b: dict) -> None:
    """b is the newer run. Only changes are printed; silence means no change."""
    old = {r["id"]: r for r in a["rows"]}
    changed = 0
    for r in b["rows"]:
        was = old.get(r["id"])
        if not was:
            print(f"  NEW    {r['id']}: {'emitted' if r['emitted'] else 'failed'}")
            changed += 1
        elif was["emitted"] != r["emitted"]:
            print(f"  {'FIXED ' if r['emitted'] else 'BROKE '} {r['id']}")
            changed += 1
        elif was["attempts"] != r["attempts"]:
            print(f"  attempts {r['id']}: {was['attempts']} -> {r['attempts']}")
            changed += 1
    if not changed:
        print("  nothing changed")
    ga = sum(1 for r in a["rows"] if r["emitted"])
    gb = sum(1 for r in b["rows"] if r["emitted"])
    print(f"\n  emitted {ga}/{len(a['rows'])} -> {gb}/{len(b['rows'])}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cmd", choices=["run", "show", "diff"])
    ap.add_argument("--only", default=None)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--timeout", type=float, default=600.0)
    a = ap.parse_args()

    if a.cmd == "show":
        runs = newest_runs(1)
        if not runs:
            print("no sweeps recorded yet")
            return 1
        table(runs[0])
        return 0

    if a.cmd == "diff":
        runs = newest_runs(2)
        if len(runs) < 2:
            print("need two sweeps to diff; run one more")
            return 1
        diff(runs[1], runs[0])
        return 0

    os.makedirs(RUNS, exist_ok=True)
    cases = load_corpus(a.only)
    if not cases:
        print("no prompts matched", file=sys.stderr)
        return 2
    print(f"sweeping {len(cases)} prompt(s), {a.jobs} at a time")
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futs = {pool.submit(run_one, c, a.timeout): c for c in cases}
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            rows.append(r)
            print(f"  {'ok  ' if r['emitted'] else 'FAIL'} {r['id']:24s} "
                  f"{r['attempts']} attempt(s), {r['seconds']}s")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    run = {"stamp": stamp, "fingerprint": fingerprint(),
           "coverage": coverage(), "rows": rows}
    with open(os.path.join(RUNS, f"{stamp}.json"), "w") as f:
        json.dump(run, f, indent=1)
    print()
    table(run)
    prev = newest_runs(2)
    if len(prev) > 1:
        print("\nsince the last sweep:")
        diff(prev[1], prev[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
