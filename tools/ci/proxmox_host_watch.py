#!/usr/bin/env python3
"""Read the macpro Proxmox CI host's own health signals and turn them into issues.

The host already produces every signal this needs. Its reaper logs
`SKIP <vmid> — <reason>` each time it cannot prove a VM is safe to destroy.
systemd parks a pool unit in `failed` once it hits its restart limit.
`pulp-proxmox-host-health.sh --json-out` writes /run/pulp-ci-host/health.json
on every reaper pass. Nothing read any of them. On 2026-10-12 an orphan clone
held the preamble slot's only VMID, the slot's unit failed after 96 restarts,
and all three signals stayed in the host's journal for days while the lane
served nothing.

This runs off-host, on a fleet Mac that reaches the host by its fleet ssh name
and holds an App-identity GitHub CLI. It reads, and never changes, the host.
Each problem gets one issue, identified by a hidden key, that is updated in
place each pass and closed when the problem clears. Findings:

  unit:<unit>      a pulp-* unit in `failed` for longer than --unit-grace-secs,
                   with its last error line and, for a VMID-exhaustion error,
                   the orphan clone holding the slot
  health:unknown   health.json absent, unreadable, or older than --health-stale-secs
  health:state     health.json reports a state other than `healthy`, or drift
                   older than an hour; prints its `reinstall_command`
  skip:<vmid>      the reaper skipped one VM on --skip-passes or more passes over
                   at least --skip-span-secs, while no GitHub runner registration
                   for that VM is online

An observation that could not be read never closes an issue: a host that could
not be seen is not a healthy host.

Exit status: 0 nothing firing, 1 a finding fired, 2 the host or GitHub could not
be read.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import subprocess
import sys
import time
from typing import Any, Callable

TITLE_PREFIX = "macpro watch:"
KEY_MARKER = "macpro-watch-key"
IPV4_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")
CLONE_ID_RE = re.compile(r"no free clone id in (\d+)\.\.(\d+)")

# Runs on the host with its own python3; emits one JSON object. Read-only.
REMOTE_PROBE = r'''
import json, subprocess, time

unreadable = set()

def run(kind, *argv):
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    except Exception:
        unreadable.add(kind)
        return ""
    if proc.returncode != 0:
        unreadable.add(kind)
    return proc.stdout

def journal(kind, unit, since=None, lines=None):
    argv = ["journalctl", "-u", unit, "-o", "json", "--no-pager"]
    if since:
        argv += ["--since", since]
    if lines:
        argv += ["-n", str(lines)]
    rows = []
    for line in run(kind, *argv).splitlines():
        try:
            row = json.loads(line)
        except ValueError:
            continue
        msg = row.get("MESSAGE")
        if isinstance(msg, list):
            msg = bytes(msg).decode("utf-8", "replace")
        rows.append({"ts": int(row.get("__REALTIME_TIMESTAMP", "0")) / 1e6,
                     "pid": str(row.get("_PID", "")), "comm": row.get("SYSLOG_IDENTIFIER", ""),
                     "msg": msg or ""})
    return rows

failed = []
listing = run("unit", "systemctl", "list-units", "--failed", "--plain", "--no-legend", "pulp-*")
for line in listing.splitlines():
    unit = line.split()[0] if line.split() else ""
    if not unit:
        continue
    props = {}
    for kv in run("unit", "systemctl", "show", "--timestamp=unix", "-p",
                  "Result,NRestarts,StateChangeTimestamp,ActiveState", unit).splitlines():
        if "=" in kv:
            k, v = kv.split("=", 1)
            props[k] = v
    errors = [r["msg"] for r in journal("unit", unit, lines=400)
              if r["comm"] != "systemd" and ("ERROR" in r["msg"] or "FATAL" in r["msg"])]
    failed.append({"unit": unit, "result": props.get("Result", ""),
                   "restarts": props.get("NRestarts", ""),
                   "since": props.get("StateChangeTimestamp", "").lstrip("@"),
                   "last_error": errors[-1] if errors else ""})

try:
    with open("/run/pulp-ci-host/health.json", encoding="utf-8") as fh:
        health = {"present": True, "raw": fh.read()}
except FileNotFoundError:
    health = {"present": False, "raw": ""}
except OSError as exc:
    health = {"present": True, "raw": "", "error": str(exc)}
    unreadable.add("health")

skips = []
for r in journal("skip", "pulp-ephemeral-reap.service", since="-6h"):
    if r["msg"].startswith("SKIP "):
        head, _, reason = r["msg"][5:].partition(" ")
        skips.append({"ts": r["ts"], "pid": r["pid"], "vmid": head,
                      "reason": reason.lstrip("—- ").strip()})

vms = {}
for line in run("skip", "qm", "list").splitlines()[1:]:
    parts = line.split()
    if len(parts) >= 3:
        vms[parts[0]] = {"name": parts[1], "status": parts[2]}

print(json.dumps({"now": time.time(), "failed_units": failed, "health": health,
                  "skips": skips, "vms": vms, "unreadable": sorted(unreadable)}))
'''


def redact(text: str) -> str:
    return IPV4_RE.sub("<addr>", text or "")


def finding(key: str, title: str, body: list[str]) -> dict[str, str]:
    return {"key": key, "title": f"{TITLE_PREFIX} {title}", "body": "\n".join(body) + "\n"}


def analyze(
    obs: dict[str, Any],
    runners: list[dict[str, Any]] | None,
    unit_grace_secs: int = 900,
    health_stale_secs: int = 1800,
    drift_secs: int = 3600,
    skip_passes: int = 4,
    skip_span_secs: int = 2700,
) -> list[dict[str, str]]:
    now = float(obs.get("now") or time.time())
    vms = obs.get("vms") or {}
    out: list[dict[str, str]] = []

    for unit in obs.get("failed_units") or []:
        since = float(unit.get("since") or 0) or now
        if now - since < unit_grace_secs:
            continue
        error = redact(unit.get("last_error", ""))
        cause = "unknown; read the unit's journal"
        fix = [f"`journalctl -u {unit['unit']} -n 100`, fix the cause, then "
               f"`systemctl reset-failed {unit['unit']} && systemctl start {unit['unit']}`."]
        match = CLONE_ID_RE.search(error)
        if match:
            lo, hi = int(match.group(1)), int(match.group(2))
            holders = [f"{vmid} ({vm['name']}, {vm['status']})" for vmid, vm in sorted(vms.items())
                       if vmid.isdigit() and lo <= int(vmid) <= hi]
            cause = (f"every clone id in {lo}..{hi} is taken"
                     + (f" by {', '.join(holders)}" if holders else "")
                     + "; an orphan clone is holding the slot's only VMID")
            fix = [f"`qm stop <vmid>` the orphan in {lo}..{hi}; the reaper's stopped-VM path "
                   "destroys it on its next pass (every 15 min).",
                   f"Then `systemctl reset-failed {unit['unit']} && systemctl start {unit['unit']}`."]
        out.append(finding(f"unit:{unit['unit']}", f"{unit['unit']} failed", [
            f"**What:** `{unit['unit']}` has been `failed` for {int((now - since) / 60)} min "
            f"(result `{unit.get('result')}`, {unit.get('restarts') or 0} restarts). systemd "
            "will not start it again on its own.",
            f"**Evidence:** last error line: `{error or '(none in the last 400 lines)'}`.",
            f"**Likely cause:** {cause}.",
            "**Fix:** " + " ".join(fix),
            "**Safe to auto-apply:** no; stopping a VM can end a running job, so prove it holds "
            "no runner first.",
            "**Verify:** `systemctl is-active " + unit["unit"] + "` reads `active`, and this issue "
            "closes on the next pass.",
        ]))

    health = obs.get("health") or {}
    parsed: dict[str, Any] | None = None
    if health.get("present") and health.get("raw"):
        try:
            parsed = json.loads(health["raw"])
        except ValueError:
            parsed = None
    checked = 0.0
    if parsed:
        try:
            checked = dt.datetime.fromisoformat(
                str(parsed.get("checked_at", "")).replace("Z", "+00:00")).timestamp()
        except ValueError:
            checked = 0.0
    if parsed is None or not checked or now - checked > health_stale_secs:
        why = ("absent" if not health.get("present") else
               "unreadable" if parsed is None else
               f"stale (checked_at {parsed.get('checked_at')})")
        out.append(finding("health:unknown", "host health check unread", [
            f"**What:** `/run/pulp-ci-host/health.json` is {why}, so the host's own health check "
            "cannot be read. Treated as unknown, never as healthy.",
            "**Likely cause:** the installed `pulp-proxmox-host-health.sh` predates `--json-out`, "
            "the reaper unit does not pass it, or the reaper has not run since boot.",
            "**Fix:** install the current host-health script and reaper unit "
            "(`pulp-proxmox-host-health.sh --install-staged`), then wait one reaper pass.",
            "**Safe to auto-apply:** no; it installs root-owned files.",
            "**Verify:** `cat /run/pulp-ci-host/health.json` shows a `checked_at` under 30 min old.",
        ]))
    elif parsed.get("state") != "healthy" or float(
            (parsed.get("drift") or {}).get("age_seconds") or 0) > drift_secs:
        out.append(finding("health:state", f"host health {parsed.get('state')}", [
            f"**What:** the host's health check reports `{parsed.get('state')}`.",
            "**Evidence:** "
            f"drift age {int(float((parsed.get('drift') or {}).get('age_seconds') or 0))} s; "
            f"failed slots {json.dumps(parsed.get('failed_slots') or parsed.get('slots') or [])[:400]}.",
            "**Fix:** " + (f"`{parsed.get('reinstall_command')}`" if parsed.get("reinstall_command")
                           else "read `pulp-proxmox-host-health.sh` output in the reaper journal."),
            "**Safe to auto-apply:** no.",
            "**Verify:** `state` reads `healthy` and this issue closes on the next pass.",
        ]))

    online_names = [str(r.get("name") or "") for r in runners or [] if r.get("status") == "online"]
    by_vm: dict[str, list[dict[str, Any]]] = {}
    for skip in obs.get("skips") or []:
        by_vm.setdefault(str(skip.get("vmid")), []).append(skip)
    for vmid, rows in sorted(by_vm.items()):
        passes = {row.get("pid") for row in rows}
        first, last = min(r["ts"] for r in rows), max(r["ts"] for r in rows)
        if len(passes) < skip_passes or last - first < skip_span_secs:
            continue
        if runners is None:
            continue  # without the runner census an idle-runner claim is unprovable
        registered = [n for n in online_names if re.search(rf"-{re.escape(vmid)}(-|$)", n)]
        if registered:
            continue
        vm = vms.get(vmid) or {}
        out.append(finding(f"skip:{vmid}", f"reaper cannot collect VM {vmid}", [
            f"**What:** the reaper has skipped VM {vmid} ({vm.get('name', '?')}, "
            f"{vm.get('status', 'unknown')}) on {len(passes)} passes over "
            f"{int((last - first) / 60)} min, and no online runner registration names it.",
            f"**Evidence:** reason `{redact(rows[-1].get('reason', ''))}`; first skip "
            f"{time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime(first))}.",
            "**Likely cause:** a clone whose guest agent died; its state cannot be proved, so the "
            "reaper correctly refuses it, and it keeps its VMID.",
            f"**Fix:** confirm no job runs on it, then `qm stop {vmid}`; the reaper's stopped-VM "
            "path destroys it on its next pass.",
            "**Safe to auto-apply:** no; that is exactly the judgment the reaper declined.",
            f"**Verify:** `qm status {vmid}` reports it gone and this issue closes.",
        ]))
    return out


class GitHub:
    def __init__(self, gh: str, runner: Callable[..., subprocess.CompletedProcess] | None = None):
        self.gh = gh
        self.run = runner or (lambda argv, stdin=None: subprocess.run(
            argv, input=stdin, capture_output=True, text=True, encoding="utf-8", timeout=120))

    def api(self, *args: str, stdin: str | None = None) -> Any:
        proc = self.run([self.gh, "api", *args], stdin=stdin)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr.strip()[:300])
        return json.loads(proc.stdout or "null")


def unreadable_kinds(obs: dict[str, Any], runners: list[dict[str, Any]] | None) -> set[str]:
    """Finding kinds whose evidence this pass could not read.

    An issue of such a kind is never closed: a probe that failed reads as
    "nothing wrong", which is the one conclusion it cannot support. An
    observation without the field predates it and is treated as unreadable.
    """
    kinds = set(obs.get("unreadable", ["unit", "health", "skip"]))
    if runners is None:
        kinds.add("skip")
    return kinds


def runner_census(gh: GitHub, repo: str) -> list[dict[str, Any]] | None:
    try:
        return gh.api(f"repos/{repo}/actions/runners?per_page=100").get("runners") or []
    except (RuntimeError, ValueError, AttributeError):
        return None


def key_of(body: str) -> str:
    match = re.search(rf"<!-- {KEY_MARKER}: (\S+) -->", body or "")
    return match.group(1) if match else ""


def reconcile(gh: GitHub, repo: str, findings: list[dict[str, str]], dry_run: bool,
              unreadable: set[str] | frozenset[str] = frozenset()) -> list[str]:
    issues = gh.api("--method", "GET", "/search/issues", "-f",
                    f'q=repo:{repo} is:issue in:title "{TITLE_PREFIX}"').get("items") or []
    mine: dict[str, dict[str, Any]] = {}
    for i in issues:
        key = key_of(i.get("body", ""))
        if not key or not str(i.get("title", "")).startswith(TITLE_PREFIX):
            continue
        # One issue per key; an open one wins over a closed duplicate.
        if key not in mine or (i.get("state") == "open" and mine[key].get("state") != "open"):
            mine[key] = i
    actions: list[str] = []
    firing = {f["key"] for f in findings}
    for f in findings:
        body = f["body"] + f"\n<!-- {KEY_MARKER}: {f['key']} -->\n"
        issue = mine.get(f["key"])
        if issue is None:
            actions.append(f"create {f['key']}")
            if not dry_run:
                gh.api("-X", "POST", f"/repos/{repo}/issues", "--input", "-",
                       stdin=json.dumps({"title": f["title"], "body": body}))
        else:
            actions.append(f"update #{issue['number']} {f['key']}")
            if not dry_run:
                gh.api("-X", "PATCH", f"/repos/{repo}/issues/{issue['number']}", "--input", "-",
                       stdin=json.dumps({"title": f["title"], "body": body, "state": "open"}))
    for key, issue in mine.items():
        if key.split(":", 1)[0] in unreadable and key not in firing:
            actions.append(f"keep #{issue['number']} {key} (evidence unreadable)")
            continue
        if key not in firing and issue.get("state") == "open":
            actions.append(f"close #{issue['number']} {key}")
            if not dry_run:
                gh.api("-X", "POST", f"/repos/{repo}/issues/{issue['number']}/comments", "-f",
                       f"body=Cleared on the pass at {time.strftime('%Y-%m-%d %H:%M UTC', time.gmtime())}. Closing.")
                gh.api("-X", "PATCH", f"/repos/{repo}/issues/{issue['number']}",
                       "-f", "state=closed", "-f", "state_reason=completed")
    return actions


def observe(host: str) -> dict[str, Any]:
    proc = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", host,
                           "python3", "-"], input=REMOTE_PROBE, capture_output=True,
                          text=True, encoding="utf-8", timeout=180)
    if proc.returncode != 0:
        raise RuntimeError(f"ssh {host} failed: {redact(proc.stderr.strip())[:300]}")
    return json.loads(proc.stdout)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--host", default="macpro", help="fleet ssh name, never an address")
    ap.add_argument("--issue-repo", default="Generous-Corp/infrastructure-issues")
    ap.add_argument("--runner-repo", default="Generous-Corp/pulp")
    ap.add_argument("--gh", default="ghapp")
    ap.add_argument("--observation-json", default="", help="read a recorded observation")
    ap.add_argument("--dry-run", action="store_true", help="print issue actions; change nothing")
    args = ap.parse_args(argv)
    gh = GitHub(args.gh)
    try:
        if args.observation_json:
            with open(args.observation_json, encoding="utf-8") as fh:
                obs = json.load(fh)
        else:
            obs = observe(args.host)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(json.dumps({"error": f"observation failed: {exc}"}))
        return 2
    runners = runner_census(gh, args.runner_repo)
    findings = analyze(obs, runners)
    try:
        actions = reconcile(gh, args.issue_repo, findings, args.dry_run,
                            unreadable_kinds(obs, runners))
    except (RuntimeError, ValueError) as exc:
        print(json.dumps({"findings": [f["key"] for f in findings], "error": str(exc)}))
        return 2
    print(json.dumps({"findings": [f["key"] for f in findings], "actions": actions,
                      "dry_run": args.dry_run}))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
