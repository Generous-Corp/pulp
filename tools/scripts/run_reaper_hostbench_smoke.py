#!/usr/bin/env python3
"""Run a small REAPER HostBench smoke and capture the produced bench log."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
from dataclasses import dataclass


DEFAULT_REAPER = pathlib.Path("/Applications/REAPER.app/Contents/MacOS/REAPER")
DEFAULT_LOG_DIR = pathlib.Path.home() / "Library" / "Logs" / "PulpHostBench"
REQUIRED_LOG_EVENTS = ("session_start", "processor_construct", "define_parameters", "prepare")


@dataclass(frozen=True)
class FormatConfig:
    label: str
    fx_names: tuple[str, ...]
    log_glob: str


FORMAT_CONFIGS = {
    "vst3": FormatConfig(
        label="VST3",
        fx_names=(
            "VST3: PulpHostBench (Pulp)",
            "PulpHostBench (Pulp)",
            "PulpHostBench",
        ),
        log_glob="REAPER-VST3-*.log",
    ),
    "clap": FormatConfig(
        label="CLAP",
        fx_names=(
            "CLAP: PulpHostBench (Pulp)",
            "PulpHostBench (Pulp)",
            "PulpHostBench",
        ),
        log_glob="REAPER-CLAP-*.log",
    ),
}


def lua_string(value: str) -> str:
    return json.dumps(value)


def build_lua_script(config: FormatConfig) -> str:
    names = ", ".join(lua_string(name) for name in config.fx_names)
    return textwrap.dedent(f"""\
        local out_path = os.getenv("PULP_REAPER_HOSTBENCH_STATUS") or ""
        local out = nil
        if out_path ~= "" then out = io.open(out_path, "a") end

        local function log(msg)
          if out then out:write(msg .. "\\n"); out:flush() end
        end

        log("start version=" .. tostring(reaper.GetAppVersion()))
        reaper.Main_OnCommand(40023, 0) -- File: New project
        reaper.InsertTrackAtIndex(0, true)
        local track = reaper.GetTrack(0, 0)
        if not track then
          log("error=no_track")
          if out then out:close() end
          reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER
          return
        end

        local names = {{{names}}}
        local fx = -1
        local used = ""
        for _, name in ipairs(names) do
          fx = reaper.TrackFX_AddByName(track, name, false, -1)
          if fx >= 0 then used = name; break end
        end
        log("fx=" .. tostring(fx) .. " name=" .. used)
        if fx < 0 then
          log("error=fx_not_found format={config.label}")
          if out then out:close() end
          reaper.Main_OnCommand(40004, 0)
          return
        end

        reaper.TrackFX_SetParam(track, fx, 0, 0.70) -- Bench Gain
        reaper.Main_OnCommand(1007, 0) -- Transport: Play

        local ticks = 0
        local function later()
          ticks = ticks + 1
          if ticks == 12 then
            reaper.Main_OnCommand(1016, 0) -- Transport: Stop
            reaper.TrackFX_SetParam(track, fx, 1, 1.0) -- Bench Bypass
            reaper.TrackFX_SetParam(track, fx, 1, 0.0)
          end
          if ticks < 24 then
            reaper.defer(later)
          else
            log("done ticks=" .. tostring(ticks))
            if out then out:close() end
            reaper.Main_OnCommand(40004, 0)
          end
        end
        reaper.defer(later)
        """)


def snapshot_logs(log_dir: pathlib.Path, pattern: str) -> set[pathlib.Path]:
    return {path.resolve() for path in log_dir.glob(pattern)}


def newest_added_log(before: set[pathlib.Path], log_dir: pathlib.Path, pattern: str) -> pathlib.Path | None:
    after = snapshot_logs(log_dir, pattern)
    added = sorted(after - before, key=lambda path: path.stat().st_mtime, reverse=True)
    return added[0] if added else None


def status_has_success(status_text: str) -> bool:
    return "fx=-1" not in status_text and "error=" not in status_text and "done ticks=" in status_text


def log_event_counts(path: pathlib.Path) -> dict[str, int]:
    counts: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        event = parts[1]
        counts[event] = counts.get(event, 0) + 1
    return counts


def missing_required_events(counts: dict[str, int]) -> list[str]:
    return [event for event in REQUIRED_LOG_EVENTS if counts.get(event, 0) <= 0]


def run_reaper(
    *,
    reaper: pathlib.Path,
    script: pathlib.Path,
    status_file: pathlib.Path,
    timeout_sec: float,
) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["PULP_REAPER_HOSTBENCH_STATUS"] = str(status_file)
    proc = subprocess.Popen(
        [str(reaper), "-newinst", "-nosplash", "-ignoreerrors", str(script)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate(timeout=5)
        return subprocess.CompletedProcess(proc.args, 124, stdout, stderr)
    return subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--format", choices=sorted(FORMAT_CONFIGS), required=True)
    parser.add_argument("--reaper", type=pathlib.Path, default=DEFAULT_REAPER)
    parser.add_argument("--log-dir", type=pathlib.Path, default=DEFAULT_LOG_DIR)
    parser.add_argument("--timeout-sec", type=float, default=30.0)
    parser.add_argument("--copy-log-to", type=pathlib.Path,
                        help="copy the produced HostBench log into this directory")
    parser.add_argument("--status-out", type=pathlib.Path,
                        help="write the REAPER script status lines here")
    parser.add_argument("--print-lua", action="store_true",
                        help="print the generated ReaScript and exit")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(list(argv or sys.argv[1:]))
    config = FORMAT_CONFIGS[args.format]
    script_text = build_lua_script(config)
    if args.print_lua:
        print(script_text, end="")
        return 0

    if not args.reaper.is_file():
        print(f"run_reaper_hostbench_smoke.py: missing REAPER binary: {args.reaper}", file=sys.stderr)
        return 2

    args.log_dir.mkdir(parents=True, exist_ok=True)
    before = snapshot_logs(args.log_dir, config.log_glob)
    started = time.time()

    with tempfile.TemporaryDirectory(prefix="pulp-reaper-hostbench-") as tmp:
        tmp_path = pathlib.Path(tmp)
        script = tmp_path / "hostbench_smoke.lua"
        status_file = args.status_out or (tmp_path / "status.txt")
        script.write_text(script_text, encoding="utf-8")

        result = run_reaper(
            reaper=args.reaper,
            script=script,
            status_file=status_file,
            timeout_sec=args.timeout_sec,
        )
        status_text = status_file.read_text(encoding="utf-8") if status_file.exists() else ""

    log_path = newest_added_log(before, args.log_dir, config.log_glob)
    if args.copy_log_to and log_path:
        args.copy_log_to.mkdir(parents=True, exist_ok=True)
        log_path = pathlib.Path(shutil.copy2(log_path, args.copy_log_to / log_path.name))
    counts = log_event_counts(log_path) if log_path else {}
    missing_events = missing_required_events(counts)

    payload = {
        "ok": (
            result.returncode == 0
            and status_has_success(status_text)
            and log_path is not None
            and not missing_events
        ),
        "format": config.label,
        "reaper": str(args.reaper),
        "elapsed_sec": round(time.time() - started, 3),
        "event_counts": counts,
        "status": [line for line in status_text.splitlines() if line],
        "log": str(log_path) if log_path else None,
        "missing_required_events": missing_events,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "returncode": result.returncode,
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if payload["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
