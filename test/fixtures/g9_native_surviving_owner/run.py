#!/usr/bin/env python3
"""Independent, finite native composition observer. Census never grants signal authority."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

CASES = ("healthy", "host_eof", "host_kill", "retry", "leak", "leak_restored",
         "census_error", "anchor_stop", "anchor_kill")
NS = 1_000_000_000

def now():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)

def save(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")

def events(path):
    if not path.exists():
        return []
    data = path.read_bytes()
    # A concurrent final append is not a malformed completed record.
    lines = data[:data.rfind(b"\n") + 1].splitlines()
    return [json.loads(line) for line in lines]

def selected(data, role, name):
    return [e for e in data if e["role"] == role and e["event"] == name]

def one(data, role, name):
    found = selected(data, role, name)
    return found[-1] if found else None

def census(data, deadline):
    start = now()
    remaining = (deadline - start) / NS
    if remaining <= 0:
        return {"start_ns": start, "end_ns": now(), "ok": False, "rows": [], "raw": "deadline expired"}
    result = subprocess.run(["ps", "-axo", "pid=,ppid=,pgid=,stat=,lstart=,comm="],
                            text=True, capture_output=True, timeout=min(1, remaining))
    pids = {e["pid"] for e in data} | {os.getpid()}
    pids.update(e["a"] for e in selected(data, "W", "vendor_fork"))
    w = one(data, "A", "worker_started")
    rows, raw = [], []
    for line in result.stdout.splitlines():
        f = line.split(None, 9)
        if len(f) != 10:
            continue
        pid, ppid, pgid = map(int, f[:3])
        if pid not in pids and not (w and pgid == w["b"]):
            continue
        rows.append(dict(pid=pid, ppid=ppid, pgid=pgid, state=f[3],
                         start=" ".join(f[4:9]), executable=f[9]))
        raw.append(line)
    return dict(start_ns=start, end_ns=now(), returncode=result.returncode,
                ok=result.returncode == 0 and any(r["pid"] == os.getpid() for r in rows),
                rows=rows, raw="\n".join(raw), stderr=result.stderr)

def assess(data, baseline, final, mode, action, hygiene_at, deadline):
    w = one(data, "A", "worker_started")
    a = one(data, "A", "born")
    wb = one(data, "W", "born")
    fd = one(data, "W", "fd_audit")
    cleanup = one(data, "A", "cleanup_result")
    obs = one(data, "A", "observation")
    eof = one(data, "A", "host_eof")
    closed = one(data, "H", "host_channel_closed")
    signals = selected(data, "A", "signal")
    waits = selected(data, "A", "retained_wait")
    reaps = [e for e in selected(data, "A", "reap") if w and e["b"] == w["a"]]
    rows = final["rows"]
    group_rows = [r for r in rows if w and r["pgid"] == w["b"]]
    alive = lambda pid: any(r["pid"] == pid and not r["state"].startswith("Z") for r in rows)
    common = {
        "ready_before_trigger": bool(baseline and baseline["ok"] and action),
        "outer_census_positive_control": bool(final["ok"]),
        "within_fixed_deadline": final["end_ns"] < deadline,
        "pre_hygiene_observation": final["end_ns"] <= hygiene_at,
        "actual_parent_separate_group": bool(w and a and wb and wb["ppid"] == a["pid"] and
                                               w["a"] == wb["pid"] == w["b"] and a["pgid"] != w["b"]),
        "reservation_retained_before_trigger": bool(w and w["c"] == 1 and action and
                                                      w["t_ns"] < action["t_ns"]),
        "live_worker_and_owner_before_loss": bool(mode == "healthy" or
            (baseline and w and a and all(any(r["pid"] == pid and not r["state"].startswith("Z")
                for r in baseline["rows"]) for pid in (w["a"], a["pid"])))),
        "baseline_precedes_loss": bool(baseline and action and baseline["end_ns"] < action["t_ns"]),
        "vendor_only_in_worker": bool(w and selected(data, "W", "vendor_enter") and
            all(e["pid"] == w["a"] for e in data if e["event"] == "vendor_enter")),
        "no_h_signal_to_worker_or_group": bool(w and all(e["a"] not in (w["a"], -w["b"])
            for e in selected(data, "H", "signal"))),
        "no_signal_after_reap": not selected(data, "A", "signal_after_reap"),
        "no_nonretained_wait": not any(e["event"] == "nonretained_wait" for e in data),
        "no_fuse_before_verdict": not any(e["event"] == "fuse_expired" for e in data),
    }
    containment = bool(cleanup and cleanup["a"] == 0 and cleanup["b"] == 1 and
                       cleanup["c"] == 1 and eof and not group_rows and a and alive(a["pid"]))
    if mode in ("anchor_stop", "anchor_kill"):
        fault = one(data, "H", "anchor_fault_injected")
        anchor_rows = [r for r in rows if a and r["pid"] == a["pid"]]
        anchor_state = (bool(anchor_rows) and all("T" in r["state"] for r in anchor_rows)
                        if mode == "anchor_stop" else
                        all(r["state"].startswith("Z") for r in anchor_rows))
        common.update(independent_anchor_failure_state=anchor_state,
            anchor_failure_injected=bool(fault and fault["c"] == 0),
            anchor_failure_unknown=bool(one(data, "H", "anchor_unknown")),
            no_custody_success=not containment and cleanup is None,
            live_worker_at_refusal=bool(w and alive(w["a"])))
    elif mode == "census_error":
        common.update(actual_census_failure=bool(selected(data, "A", "census_error")),
            independent_zombie_reservation=bool(w and len(group_rows) == 1 and
                group_rows[0]["pid"] == w["a"] and group_rows[0]["state"].startswith("Z")),
            incomplete_unreaped=bool(cleanup and (cleanup["a"],cleanup["b"],cleanup["c"]) == (1,0,0)),
            post_cleanup_identity_retained=bool(obs and cleanup and obs["t_ns"] > cleanup["t_ns"] and
                                                obs["a"] == 3 and obs["b"] == 1),
            quarantine_alive=bool(one(data, "A", "quarantine") and a and alive(a["pid"])),
            no_successful_reap=not reaps, no_custody_success=not containment)
    elif mode == "leak":
        common.update(same_containment_oracle_rejects=not containment,
            actual_alias_leak=bool(fd and (fd["a"],fd["b"],fd["c"]) == (1,1,1) and one(data,"A","leak_injected")),
            host_closed_but_eof_masked=bool(closed and not eof and cleanup is None),
            live_vendor_after_dwell=bool(closed and w and alive(w["a"]) and
                                        final["start_ns"] - closed["t_ns"] >= 400_000_000))
    else:
        common.update(containment=containment,
            aliases_closed=bool(fd and (fd["a"],fd["b"],fd["c"]) == (0,0,1)),
            native_cloexec_observed=any(e["a"] & e["b"] for e in selected(data,"A","spawn_flags")),
            actual_host_loss=bool(eof and (closed or (action and "direct_child_sigkill" in action))),
            successful_nonreaping_observation=bool(waits and all(e["b"] == 0 for e in waits)),
            final_reap_after_signals=bool(reaps and reaps[-1]["c"] & os.WNOHANG and
                all(e["t_ns"] < reaps[-1]["t_ns"] for e in signals)),
            final_reaped_state=bool(obs and obs["a"] == 4 and obs["b"] == 0),
            native_census_observed=bool(selected(data,"A","census")))
        if mode != "healthy":
            common["live_descendant_before_loss"] = bool(w and baseline and any(
                r["pgid"] == w["b"] and r["pid"] != w["a"] and not r["state"].startswith("Z")
                for r in baseline["rows"]))
            common["term_and_kill_observed"] = all(any(e["b"] == s and e["c"] == 0 for e in signals)
                                                        for s in (signal.SIGTERM, signal.SIGKILL))
        if mode == "retry":
            inject = one(data,"A","kill_injected")
            kills = [e for e in signals if e["b"] == signal.SIGKILL and e["c"] == 0]
            common["finite_fork_before_native_retry"] = bool(inject and kills and
                any(inject["t_ns"] < e["t_ns"] < kills[0]["t_ns"] and e["a"] > 0
                    for e in selected(data,"W","vendor_fork")))
            common["retry_spacing_at_least_10ms"] = bool(inject and kills and
                kills[0]["t_ns"] - inject["t_ns"] >= 10_000_000)
    return common, containment

def run(binary, vendor, root, label):
    mode = "host_eof" if label == "leak_restored" else label
    folder = root / label
    folder.mkdir()  # Refuse overwriting any previous evidence.
    log = folder / "events.jsonl"
    start = now()
    deadline = start + 5 * NS
    command = [str(binary), "H", str(folder), mode, str(vendor), str(deadline)]
    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    baseline, final, action = None, None, None
    data = []
    try:
        while now() < deadline:
            data = events(log)
            if baseline is None and one(data, "H", "ready") and one(data, "W", "vendor_enter"):
                baseline = census(data, deadline)
                action = dict(t_ns=now(), kind=mode)
                if mode == "host_kill":
                    # Our actual direct child; no process census supplies authority.
                    proc.kill()
                    action["direct_child_sigkill"] = proc.pid
                else:
                    save(folder / "trigger", action)
            done = one(data, "A", "complete") or one(data, "A", "quarantine") or one(data, "H", "anchor_unknown")
            closed = one(data, "H", "host_channel_closed")
            leak_dwell = mode == "leak" and closed and now() - closed["t_ns"] >= 400_000_000
            if done or leak_dwell:
                final = census(data, deadline)
                break
            time.sleep(.005)
        if final is None:
            final = census(data, deadline)
        # Freeze proof before markers permit any hygiene or reservation release.
        data = [e for e in events(log) if e["t_ns"] <= final["start_ns"]]
        save(folder / "verdict-events.json", data)
        save(folder / "baseline.json", baseline)
        save(folder / "pre-hygiene-census.json", final)
    finally:
        hygiene_at = now()
        save(folder / "observed", {"t_ns": hygiene_at})
        save(folder / "hygiene", {"t_ns": hygiene_at})
        # No W/A/descendant or group signals. H resumes its own retained stopped A.
        try:
            proc.wait(timeout=max(.01, (deadline - now()) / NS))
        except subprocess.TimeoutExpired:
            proc.kill()  # Only our actual direct child, after the proof cutoff.
            proc.wait(timeout=1)

    common, containment = assess(data, baseline, final, mode, action, hygiene_at, deadline)
    receipt = dict(schema=1, mode=label, native_mode=mode, start_ns=start, deadline_ns=deadline,
                   command=command, action=action, hygiene_started_ns=hygiene_at,
                   assertions=common, containment_oracle=containment, passed=all(common.values()),
                   host_returncode=proc.returncode, production_custody="HOLD",
                   binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                   vendor_sha256=hashlib.sha256(vendor.read_bytes()).hexdigest(),
                   observer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    save(folder / "receipt.json", receipt)
    print(json.dumps({"case":label,"passed":receipt["passed"],
                      "failed":[k for k,v in common.items() if not v]}), flush=True)
    return receipt

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--binary", type=Path, required=True)
    p.add_argument("--vendor", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cases", default=",".join(CASES))
    p.add_argument("--recheck", action="store_true", help="Assess saved raw observations without spawning")
    a=p.parse_args()
    modes=a.cases.split(",")
    if any(m not in CASES for m in modes): p.error("unknown case")
    root=a.output.resolve(); root.mkdir(parents=True,exist_ok=True)
    results=[]
    for m in modes:
        if a.recheck:
            folder = root / m
            receipt = json.loads((folder / "receipt.json").read_text())
            data = json.loads((folder / "verdict-events.json").read_text())
            baseline = json.loads((folder / "baseline.json").read_text())
            final = json.loads((folder / "pre-hygiene-census.json").read_text())
            checks, containment = assess(data, baseline, final, receipt["native_mode"],
                receipt["action"], receipt["hygiene_started_ns"], receipt["deadline_ns"])
            if m == "host_kill": checks["actual_host_sigkill_exit"] = receipt["host_returncode"] == -9
            receipt.update(assertions=checks, containment_oracle=containment,
                passed=all(checks.values()),
                canonical_observer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
            save(folder / "canonical-receipt.json", receipt)
            results.append(receipt)
            print(json.dumps({"recheck":m,"passed":receipt["passed"],
                "failed":[k for k,v in checks.items() if not v]}), flush=True)
        else:
            results.append(run(a.binary.resolve(),a.vendor.resolve(),root,m))
        if not results[-1]["passed"]: break
    save(root / ("canonical.json" if a.recheck else "summary-"+"-".join(modes)+".json"), results)
    return 0 if len(results)==len(modes) and all(r["passed"] for r in results) else 1

if __name__ == "__main__":
    raise SystemExit(main())
