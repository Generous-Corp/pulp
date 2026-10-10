#!/usr/bin/env python3
"""Collect compact, redacted replay evidence into an append-only JSONL scratch cache.

The envelope is deliberately storage-neutral: one JSON object per line with a stable
record_id, schema, source, collected_at, observed_at and payload. A future repo-backed
store can ingest these lines unchanged.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, subprocess, sys
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable

SCHEMA = "pulp-replay-evidence/v1"
FACT_DAYS = 180
SESSION_DAYS = 30
_SECRET = re.compile(r"(?i)(?:ghp_|github_pat_|sk-[A-Za-z0-9_-]{8,}|xox[baprs]-[A-Za-z0-9-]{8,}|AKIA[0-9A-Z]{12,}|-----BEGIN [^-]+ KEY-----)[A-Za-z0-9_./+=:-]*")
_EMAIL = re.compile(r"\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b", re.I)
_KEY = re.compile(r"(?i)(api[_-]?key|access[_-]?token|secret|password|private[_-]?key)\s*[:=]\s*[^,\s}]+")

def redact(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(k): redact(v) for k, v in value.items()}
    if isinstance(value, list):
        return [redact(v) for v in value]
    if not isinstance(value, str):
        return value
    value = _SECRET.sub("[REDACTED_TOKEN]", value)
    value = _KEY.sub(lambda m: m.group(1) + "=[REDACTED_KEY]", value)
    return _EMAIL.sub("[REDACTED_EMAIL]", value)

def iso_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

def parse_time(value: Any, fallback: str) -> str:
    if not isinstance(value, str) or not value:
        return fallback
    return value

def record(kind: str, payload: dict[str, Any], source: str, collected_at: str, observed_at: str | None = None) -> dict[str, Any]:
    clean = redact(payload)
    basis = json.dumps({"kind": kind, "source": source, "observed_at": observed_at or collected_at, "payload": clean}, sort_keys=True, separators=(",", ":"))
    return {"schema": SCHEMA, "record_id": hashlib.sha256(basis.encode()).hexdigest(), "kind": kind, "source": source, "collected_at": collected_at, "observed_at": observed_at or collected_at, "payload": clean}

def read_json(path: Path) -> Any:
    return json.loads(path.read_text())

def read_jsonl(path: Path) -> Iterable[dict[str, Any]]:
    with path.open() as fh:
        for line in fh:
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                yield value

def github_records(data: Any, source: str, collected: str) -> Iterable[dict[str, Any]]:
    runs = data.get("runs", data) if isinstance(data, dict) else data
    if isinstance(runs, list):
        for run in runs:
            if not isinstance(run, dict): continue
            yield record("ci_job", {k: run.get(k) for k in ("run_id", "id", "sha", "head_sha", "event", "runner_name", "run_started_at", "updated_at", "conclusion", "status", "name", "failure_evidence") if k in run}, source, collected, parse_time(run.get("updated_at") or run.get("run_started_at"), collected))
    prs = data.get("prs", []) if isinstance(data, dict) else []
    if isinstance(prs, list):
        for pr in prs:
            if not isinstance(pr, dict): continue
            timeline = pr.get("timeline", [])
            if isinstance(timeline, list):
                for event in timeline:
                    if isinstance(event, dict):
                        yield record("pr_event", {"number": pr.get("number"), "event": event}, source, collected, parse_time(event.get("created_at"), collected))
            yield record("pr_event", {k: pr.get(k) for k in ("number", "sha", "event", "action", "reason", "reviewed", "verdict", "created_at") if k in pr}, source, collected, parse_time(pr.get("created_at"), collected))

def collect_host(path: Path, source: str, collected: str) -> Iterable[dict[str, Any]]:
    if path.is_file(): files = [path]
    else: files = sorted(path.rglob("*.jsonl")) if path.exists() else []
    for file in files:
        for item in read_jsonl(file):
            kind = str(item.get("kind") or item.get("type") or "host_event")
            observed = parse_time(item.get("timestamp") or item.get("observed_at") or item.get("created_at"), collected)
            yield record("host_event", {"event_kind": kind, "event": item}, f"{source}:{file}", collected, observed)

def collect_sessions(path: Path, collected: str) -> Iterable[dict[str, Any]]:
    if not path.exists(): return
    for file in path.rglob("*.jsonl"):
        for item in read_jsonl(file):
            if not isinstance(item, dict): continue
            if not any(k in item for k in ("error", "tool_error", "timestamp", "created_at")): continue
            observed = parse_time(item.get("timestamp") or item.get("created_at"), collected)
            yield record("session_error", {k: item.get(k) for k in ("session_id", "timestamp", "created_at", "tool", "error", "tool_error") if k in item}, f"session:{file}", collected, observed)

def within(record_: dict[str, Any], cutoff: datetime) -> bool:
    try: return datetime.fromisoformat(record_["observed_at"].replace("Z", "+00:00")) >= cutoff
    except (KeyError, ValueError): return True

def append_records(root: Path, records: Iterable[dict[str, Any]], now: datetime) -> dict[str, int]:
    root.mkdir(parents=True, exist_ok=True)
    (root / "README.md").write_text(SCRATCH_README)
    day = now.strftime("%Y-%m-%d")
    paths = {"ci_job": root / f"facts-{day}.jsonl", "pr_event": root / f"facts-{day}.jsonl", "host_event": root / f"facts-{day}.jsonl", "session_error": root / f"session-index-{day}.jsonl"}
    seen: set[str] = set()
    for path in root.glob("*.jsonl"):
        for item in read_jsonl(path):
            if item.get("record_id"): seen.add(item["record_id"])
    counts = {"ci_job": 0, "pr_event": 0, "host_event": 0, "session_error": 0, "skipped_duplicate": 0, "skipped_retention": 0}
    handles: dict[Path, Any] = {}
    try:
        for item in records:
            kind = item["kind"]
            cutoff = now - timedelta(days=SESSION_DAYS if kind == "session_error" else FACT_DAYS)
            if not within(item, cutoff): counts["skipped_retention"] += 1; continue
            if item["record_id"] in seen: counts["skipped_duplicate"] += 1; continue
            seen.add(item["record_id"]); handles.setdefault(paths[kind], paths[kind].open("a")).write(json.dumps(item, sort_keys=True) + "\n"); counts[kind] += 1
    finally:
        for fh in handles.values(): fh.close()
    prune(root, now)
    return counts

def prune(root: Path, now: datetime) -> None:
    for path in root.glob("*.jsonl"):
        is_session = path.name.startswith("session-index-")
        cutoff = now - timedelta(days=SESSION_DAYS if is_session else FACT_DAYS)
        kept = [x for x in read_jsonl(path) if within(x, cutoff)]
        if kept: path.write_text("".join(json.dumps(x, sort_keys=True) + "\n" for x in kept))
        else: path.unlink(missing_ok=True)

def gh_fetch(repo: str, since_days: int) -> Any:
    since = (datetime.now(timezone.utc) - timedelta(days=since_days)).date().isoformat()
    cmd = ["ghapp", "api", "--method", "GET", f"repos/{repo}/actions/runs?per_page=100&created=>={since}"]
    proc = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return json.loads(proc.stdout or "[]")

SCRATCH_README = """# Replay evidence scratch cache\n\nThis is a clearly labelled scratch cache on Atelier, populated by the Pulp replay evidence collector. It is derived from GitHub, Shipyard, tartci, and redacted session metadata. It is **not a backup or source of truth**, is not maintained forever, and may be deleted with `rm -f facts-*.jsonl session-index-*.jsonl README.md`. Facts roll for 180 days; the session-error index rolls for 30 days. Every record includes `source`, `collected_at`, and `observed_at`.\n"""

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path("/Volumes/Atelier/pulp-replay-evidence"))
    ap.add_argument("--github-json", type=Path)
    ap.add_argument("--repo", default="Generous-Corp/pulp")
    ap.add_argument("--host-root", type=Path)
    ap.add_argument("--sessions-root", type=Path)
    ap.add_argument("--now", help="UTC ISO timestamp, for tests/repro")
    ap.add_argument("--no-github", action="store_true")
    args = ap.parse_args()
    now = datetime.fromisoformat((args.now or iso_now()).replace("Z", "+00:00")); collected = now.replace(microsecond=0).isoformat().replace("+00:00", "Z")
    records: list[dict[str, Any]] = []
    if args.github_json: records.extend(github_records(read_json(args.github_json), "github", collected))
    elif not args.no_github:
        try: records.extend(github_records(gh_fetch(args.repo, FACT_DAYS), "github", collected))
        except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc: print(f"github collection skipped: {exc}", file=sys.stderr)
    if args.host_root: records.extend(collect_host(args.host_root, "host", collected))
    if args.sessions_root: records.extend(collect_sessions(args.sessions_root, collected))
    counts = append_records(args.root, records, now)
    print(json.dumps({"schema": SCHEMA, "collected_at": collected, "retention_days": {"facts": FACT_DAYS, "session_index": SESSION_DAYS}, "counts": counts}, sort_keys=True))
    return 0
if __name__ == "__main__": raise SystemExit(main())
