#!/usr/bin/env python3
"""GPU-fault continuity runs in Logic Pro, GarageBand and Bitwig (WP-K).

REAPER is driven end to end by reaper_smoke.py. These hosts are not
scriptable that way, so a run is split between this driver and the operating
agent:

* this driver owns everything that is not a UI action: the preflight (the
  host must not already be running, nothing left over from an earlier run),
  the run directory and the sandbox-reachable rendezvous the plug-in reads
  (examples/gpu-fault-continuity/gpu_fault_rendezvous.hpp), a throwaway copy
  of the host's template project, the launch (`open -g`, never activating
  the host), the claim, the realtime render's provenance record, the v2
  receipt and its verification, and the revert of every change it made;
* the agent performs the UI steps through cmux-cua, by accessibility element
  and posted events to the host's pid: confirm the template's insert, input
  and monitoring settings, start the realtime bounce or export, and quit
  without saving.

Each subcommand is one step, and the state lives in the run directory
(state.json, revert_log.json), so a run survives the agent switching between
the driver and the UI. A run whose revert log was not completed blocks the
next preflight: nothing this driver changed may outlive a crashed harness
unnoticed.

    real_host_smoke.py preflight  --host logic --run DIR
    real_host_smoke.py install-au --run DIR --component PATH.component
    real_host_smoke.py snapshot-prefs --run DIR      (before launch; required for Logic, GarageBand)
    real_host_smoke.py stage      --run DIR --fault force_loss_before --at 96000 [...]
    real_host_smoke.py launch     --run DIR
    real_host_smoke.py await-claim --run DIR
    real_host_smoke.py realtime-begin --run DIR      (just before the agent starts the render)
    real_host_smoke.py realtime-end   --run DIR --artifact PATH.wav
    real_host_smoke.py collect    --run DIR --bounce PATH.wav --host-evidence AX.json
    real_host_smoke.py revert     --run DIR

Exit codes: 0 done, 1 the run's verified outcome is not a full pass, 2 a
usage or state error, 3 the preflight refused (the run must not start).
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import plistlib
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Callable

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]


def _load(name: str, path: Path):
    import importlib.util
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


V2 = _load("verify_gpu_audio_daw_continuity_receipt_v2",
           REPO / "tools/validation/verify_gpu_audio_daw_continuity_receipt_v2.py")
gfc, hosts = V2.gfc, V2.hosts

EXIT_OK, EXIT_NOT_PASS, EXIT_USAGE, EXIT_REFUSED = 0, 1, 2, 3
RENDEZVOUS_SCHEMA = "pulp.gpu-fault.rendezvous.v1"
# Long enough for a Logic launch with AU validation; deleting active.json
# right after the claim is what actually closes the window (Fable S9).
RENDEZVOUS_LIFETIME_S = 900
PLUGIN_NAME = "Pulp GPU Fault Continuity"


HOSTS: dict[str, dict[str, Any]] = {
    "logic": {
        "app": "/Applications/Logic Pro.app",
        "bundle_id": "com.apple.logic10",
        "executable": "/Applications/Logic Pro.app/Contents/MacOS/Logic Pro",
        "formats": ("au",),
        # -ApplePersistenceIgnoreState keeps window restoration from reopening
        # the user's last project next to the throwaway one.
        "open_args": ("--args", "-ApplePersistenceIgnoreState", "YES"),
        "project_suffix": ".logicx",
        "prefs_domains": ("com.apple.logic10",),
    },
    "garageband": {
        "app": "/Applications/GarageBand.app",
        "bundle_id": "com.apple.garageband10",
        "executable": "/Applications/GarageBand.app/Contents/MacOS/GarageBand",
        "formats": ("au",),
        "open_args": ("--args", "-ApplePersistenceIgnoreState", "YES"),
        "project_suffix": ".band",
        "prefs_domains": ("com.apple.garageband10",),
    },
    "bitwig": {
        "app": "/Applications/Bitwig Studio.app",
        "bundle_id": "com.bitwig.studio",
        "executable": "/Applications/Bitwig Studio.app/Contents/MacOS/BitwigStudio",
        "engine_prefix": "/Applications/Bitwig Studio.app/Contents/MacOS/Bitwig Audio Engine",
        "formats": ("clap", "vst3"),
        "open_args": (),
        "project_suffix": ".bwproject",
        "prefs_dir": "~/Library/Application Support/Bitwig/Bitwig Studio/prefs",
    },
}
# Any of these running means the machine is someone's session or another
# run: a run never attaches to, quits or competes with it.
OTHER_DAW_EXECUTABLES = (
    "/Applications/Logic Pro.app/Contents/MacOS/Logic Pro",
    "/Applications/GarageBand.app/Contents/MacOS/GarageBand",
    "/Applications/Bitwig Studio.app/Contents/MacOS/BitwigStudio",
    "/Applications/REAPER.app/Contents/MacOS/REAPER",
)


def log(msg: str) -> None:
    print(f"[real-host-smoke] {msg}", flush=True)


def now_us() -> int:
    return time.time_ns() // 1000


# ── Seams (tests replace these) ───────────────────────────────────────────

def list_processes() -> list[tuple[int, str]]:
    """(pid, executable path) of this user's processes."""
    out = subprocess.run(["ps", "-x", "-o", "pid=,comm="], capture_output=True, text=True,
                         encoding="utf-8", check=False).stdout
    procs = []
    for line in out.splitlines():
        pid, _, comm = line.strip().partition(" ")
        if pid.isdigit():
            procs.append((int(pid), comm.strip()))
    return procs


def console_locked() -> bool:
    """True while the login window covers the console. Hosts like Logic expose
    an accessibility tree dominated by their menu bar, so the run's UI steps
    need pixel clicks on a visible window, which a locked screen blocks."""
    out = subprocess.run(["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, text=True,
                         encoding="utf-8", check=False).stdout
    try:
        doc = plistlib.loads(out.encode("utf-8"))
    except Exception:  # noqa: BLE001 - unknown is treated as locked (fail closed)
        return True
    return bool(doc.get("IOConsoleLocked", True)) if isinstance(doc, dict) else True


def defaults(args: list[str]) -> None:
    """This user's `defaults` (export, import, delete on one domain)."""
    subprocess.run(["/usr/bin/defaults", *args], capture_output=True, check=True)


def responsible_pid(pid: int) -> int:
    """The macOS responsible process for `pid` (the same call the plug-in
    makes), or -1 when unavailable."""
    try:
        fn = ctypes.CDLL(None).responsibility_get_pid_responsible_for_pid
    except (AttributeError, OSError):
        return -1
    fn.restype, fn.argtypes = ctypes.c_int, [ctypes.c_int]
    return int(fn(pid))


def rendezvous_root() -> Path:
    override = os.environ.get("PULP_GPU_FAULT_RENDEZVOUS_ROOT")
    if override:
        return Path(override)
    import pwd
    return (Path(pwd.getpwuid(os.getuid()).pw_dir) / "Library" / "Application Support" /
            "Pulp" / "gpu-fault-continuity")


def components_dir() -> Path:
    override = os.environ.get("PULP_REAL_HOST_COMPONENTS_DIR")
    return Path(override) if override else Path.home() / "Library/Audio/Plug-Ins/Components"


def refresh_au_registry(directory: Path) -> None:
    """AudioComponentRegistrar rescans when the Components directory changes;
    restarting this user's registrar makes it immediate (as run-auval-component.sh
    does). Skipped when the directory is a test override."""
    os.utime(directory, None)
    if "PULP_REAL_HOST_COMPONENTS_DIR" not in os.environ:
        subprocess.run(["killall", "-KILL", "AudioComponentRegistrar"], capture_output=True,
                       check=False)


# ── State and revert log ──────────────────────────────────────────────────

def _read_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def _write_json(path: Path, value: Any) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(path)


def state(run: Path) -> dict:
    return _read_json(run / "state.json", {})


def save_state(run: Path, **fields: Any) -> dict:
    current = state(run)
    current.update(fields)
    _write_json(run / "state.json", current)
    return current


def record_change(run: Path, kind: str, **details: Any) -> None:
    """Every change outside the run directory, before it is made, so a crash
    between the change and the record cannot hide it."""
    entries = _read_json(run / "revert_log.json", [])
    entries.append({"kind": kind, "reverted": False, "at_unix_us": now_us(), **details})
    _write_json(run / "revert_log.json", entries)


def unreverted(run: Path) -> list[dict]:
    return [e for e in _read_json(run / "revert_log.json", []) if not e.get("reverted")]


def _remove_active_if_ours(path: Path, nonce: str | None) -> bool:
    """Delete the rendezvous only if it is still this run's (same nonce): a
    rendezvous written by another run is never removed by this one."""
    doc = _read_json(path, None)
    if doc is None:
        return not path.exists()
    if not nonce or doc.get("nonce") != nonce:
        log(f"leaving {path}: it belongs to another run (nonce {doc.get('nonce')!r})")
        return False
    path.unlink(missing_ok=True)
    return True


def _restore_prefs(target: Path, saved: Path) -> None:
    """Put the saved byte copy back. The current prefs are moved aside, not
    deleted, until the copy is in place; on any failure they are moved back."""
    if not saved.is_dir():
        raise RuntimeError(f"cannot restore {target}: the saved copy {saved} is missing; the "
                           "current prefs were left untouched")
    aside = target.with_name(f"{target.name}.wpk-revert-{now_us()}")
    if target.exists() or target.is_symlink():
        target.rename(aside)
    try:
        shutil.copytree(saved, target, symlinks=True)
    except BaseException:
        if target.exists():
            shutil.rmtree(target, ignore_errors=True)
        if aside.exists() or aside.is_symlink():
            aside.rename(target)
        raise
    if aside.exists() or aside.is_symlink():
        shutil.rmtree(aside) if aside.is_dir() and not aside.is_symlink() else aside.unlink()


def _canonical_plist(path: Path) -> bytes:
    """A preference export in one canonical byte form (XML, sorted keys), so
    two exports of the same preferences compare equal byte for byte."""
    return plistlib.dumps(plistlib.loads(path.read_bytes()), fmt=plistlib.FMT_XML,
                          sort_keys=True)


def _restore_domain(domain: str, saved: Path) -> None:
    """Put a preference domain back to its export. `defaults import` merges,
    so keys the run added are deleted one by one; the result is exported
    again and must equal the saved export byte for byte (canonical form), or
    the entry stays unreverted and the next preflight refuses."""
    if not saved.is_file():
        raise RuntimeError(f"cannot restore {domain}: the saved export {saved} is missing; the "
                           "current preferences were left untouched")
    before = plistlib.loads(saved.read_bytes())
    with tempfile.TemporaryDirectory(prefix="wpk-prefs-") as tmp:
        now = Path(tmp) / "now.plist"
        defaults(["export", domain, str(now)])
        if _canonical_plist(now) == _canonical_plist(saved):
            return
        added = sorted(plistlib.loads(now.read_bytes()).keys() - before.keys())
        defaults(["import", domain, str(saved)])
        for key in added:
            defaults(["delete", domain, key])
        defaults(["export", domain, str(now)])
        if _canonical_plist(now) != _canonical_plist(saved):
            raise RuntimeError(f"REFUSING TO MARK {domain} REVERTED: after the restore it still "
                               f"differs from {saved}. Compare them by hand before the next run.")


def _revert_entry(entry: dict) -> None:
    kind = entry["kind"]
    if kind == "rendezvous_active":
        # Ours is removed; one that now belongs to another run is left in
        # place (there is nothing of this run's to undo there).
        if not _remove_active_if_ours(Path(entry["path"]), entry.get("nonce")):
            entry["note"] = "left in place: it belongs to another run"
    elif kind in ("au_install", "rendezvous_run"):
        path = Path(entry["path"])
        if path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        elif path.exists() or path.is_symlink():
            path.unlink()
        if kind == "au_install":
            refresh_au_registry(path.parent)
    elif kind == "prefs_snapshot":
        _restore_prefs(Path(entry["path"]), Path(entry["saved"]))
    elif kind == "prefs_domain_snapshot":
        saved = Path(entry["saved"])
        if entry.get("sha256") and saved.is_file() and gfc.sha256_file(saved) != entry["sha256"]:
            raise RuntimeError(f"cannot restore {entry['domain']}: the saved export {saved} no "
                               "longer matches the snapshot's sha256; the current preferences "
                               "were left untouched")
        _restore_domain(entry["domain"], saved)
    elif kind == "throwaway_project":
        shutil.rmtree(entry["path"], ignore_errors=True)
    else:
        raise ValueError(f"unknown revert kind {kind!r}")


def host_alive(run: Path, processes: Callable[[], list[tuple[int, str]]] = list_processes
               ) -> list[int]:
    """The launched host's processes that are still running (app and engine)."""
    st = state(run)
    host = HOSTS.get(st.get("host", ""), {})
    pids = {st.get("host_pid"), *st.get("engine_pids", [])} - {None}
    if not pids:
        return []
    exes = {host.get("executable", "\0")}
    prefix = host.get("engine_prefix", "\0")
    return sorted(pid for pid, path in processes()
                  if pid in pids and (path in exes or path.startswith(prefix)))


def revert(run: Path, *, processes: Callable[[], list[tuple[int, str]]] = list_processes,
           wait_s: float = 30.0, poll: float = 0.5) -> list[dict]:
    """Undo every recorded change, newest first. Idempotent: an entry is
    marked reverted only after its undo succeeded.

    Never while the host this run launched is still running: it may hold the
    AU, the throwaway project and the plug-in's run directory open, and
    anything it wrote after the revert would be outside the log. The agent
    quits the host first (AX quit, Don't Save); this waits up to `wait_s`
    for it to exit, then refuses and leaves the run unreverted."""
    deadline = time.monotonic() + wait_s
    alive = host_alive(run, processes)
    while alive and time.monotonic() < deadline:
        time.sleep(poll)
        alive = host_alive(run, processes)
    if alive:
        raise RuntimeError(f"REFUSING TO REVERT: the host this run launched is still running "
                           f"(pids {alive}). Quit it (AX quit, Don't Save), then run "
                           f"`revert --run {run}` again. Nothing was reverted.")
    entries = _read_json(run / "revert_log.json", [])
    for entry in reversed(entries):
        if entry.get("reverted"):
            continue
        _revert_entry(entry)
        entry["reverted"] = True
        entry["reverted_at_unix_us"] = now_us()
        _write_json(run / "revert_log.json", entries)
    return entries


def _install_revert_on_signals(run: Path) -> None:
    """For the long-running steps: an interrupt or termination still reverts,
    but only once the host is gone; otherwise it says so loudly and leaves the
    run for `revert` (which the next preflight insists on)."""
    def handler(signum, _frame):
        log(f"signal {signum}: reverting before exit")
        try:
            revert(run, wait_s=10.0)
        except RuntimeError as exc:
            log(str(exc))
        raise SystemExit(EXIT_USAGE)
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, handler)


# ── Steps ─────────────────────────────────────────────────────────────────

def preflight(host_id: str, run: Path, *, runs_parent: Path | None = None,
              processes: Callable[[], list[tuple[int, str]]] = list_processes,
              host_preflight: Callable[[], dict] | None = None,
              locked: Callable[[], bool] = console_locked) -> tuple[bool, list[str]]:
    """Refuse a run that could touch someone's session or inherit a previous
    run's state. Writes preflight.json (the P2 quiet-host document the
    receipt binds) and state.json."""
    host = HOSTS[host_id]
    reasons: list[str] = []
    running = {path for _, path in processes()}
    for exe in OTHER_DAW_EXECUTABLES:
        if exe in running:
            reasons.append(f"{Path(exe).name} is already running; a run never attaches to or "
                           "quits a session it did not launch")
    if any(path.startswith(host.get("engine_prefix", "\0")) for path in running):
        reasons.append("a Bitwig audio engine is already running")
    if locked():
        reasons.append("the console is locked; the run's UI steps need an unlocked screen")
    root = rendezvous_root()
    if (root / "active.json").exists():
        reasons.append(f"a rendezvous is already active at {root / 'active.json'}")
    for previous in sorted((runs_parent or run.parent).glob("*/revert_log.json")):
        if previous.parent != run and unreverted(previous.parent):
            reasons.append(f"{previous.parent} has changes that were never reverted; run "
                           f"`revert --run {previous.parent}` first")
    if host_id not in hosts.HOSTS:
        reasons.append(f"unknown host {host_id!r}")
    run.mkdir(parents=True, exist_ok=True)
    doc = (host_preflight or _p2_preflight)()
    _write_json(run / "preflight.json", doc)
    save_state(run, host=host_id, preflight_refusals=reasons)
    return not reasons, reasons


def _p2_preflight() -> dict:
    """The P2 quiet-host preflight document, as reaper_smoke.py writes it. An
    unavailable preflight is a not-quiet host, never a quiet one."""
    try:
        module = _load("gpu_audio_p2_host_preflight",
                       REPO / "tools/scripts/gpu_audio_p2_host_preflight.py")
        return module.collect()
    except Exception as exc:  # noqa: BLE001 - the label must survive any probe failure
        return {"status": "unavailable", "quiet_host": False,
                "reasons": [f"preflight_error: {exc}"]}


def install_au(run: Path, component: Path) -> Path:
    """An isolated, ad-hoc-signed copy of the test AU for this column,
    recorded before it is made so revert always removes it."""
    token = secrets.token_hex(4)
    target = components_dir() / f"{component.stem}.wpk-{token}.component"
    record_change(run, "au_install", path=str(target))
    shutil.copytree(component, target, symlinks=True)
    if "PULP_REAL_HOST_COMPONENTS_DIR" not in os.environ:
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(target)],
                       check=True, capture_output=True)
    refresh_au_registry(target.parent)
    save_state(run, au_component=str(target))
    return target


PREFS_SNAPSHOT_KINDS = ("prefs_snapshot", "prefs_domain_snapshot")


def snapshot_prefs(run: Path, host_id: str) -> Path | None:
    """Before the host can change its preferences: keep a copy to restore,
    recorded in the revert log. Bitwig's prefs directory is copied byte for
    byte; Logic's and GarageBand's preference domains are exported with
    `defaults` (the launch alone rewrites them: recent projects, the plug-in
    list, new-track settings)."""
    host = HOSTS[host_id]
    if not host.get("prefs_dir") and not host.get("prefs_domains"):
        return None
    saved = run / "prefs-before"
    # One snapshot per run: a second one would capture prefs this run already
    # changed and make revert restore those.
    if saved.exists() or any(e["kind"] in PREFS_SNAPSHOT_KINDS
                             for e in _read_json(run / "revert_log.json", [])):
        raise SystemExit("snapshot-prefs: this run already holds a prefs snapshot")
    if host.get("prefs_dir"):
        source = Path(os.path.expanduser(host["prefs_dir"]))
        shutil.copytree(source, saved, symlinks=True)
        record_change(run, "prefs_snapshot", path=str(source), saved=str(saved))
        return saved
    # Export every domain into a staging directory first: a domain that
    # cannot be exported (a host never launched on this machine has none)
    # must not leave a half-made snapshot that wedges the next attempt.
    staging = saved.with_name(f"{saved.name}.partial-{now_us()}")
    staging.mkdir()
    try:
        for domain in host["prefs_domains"]:
            try:
                defaults(["export", domain, str(staging / f"{domain}.plist")])
            except (OSError, subprocess.CalledProcessError) as error:
                raise SystemExit(f"snapshot-prefs: cannot export {domain} ({error}); launch "
                                 "the host once so its preferences exist, then retry") from error
        staging.rename(saved)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    for domain in host["prefs_domains"]:
        export = saved / f"{domain}.plist"
        record_change(run, "prefs_domain_snapshot", domain=domain, saved=str(export),
                      sha256=gfc.sha256_file(export))
    return saved


def prefs_snapshot_taken(run: Path) -> bool:
    return any(e["kind"] in PREFS_SNAPSHOT_KINDS for e in _read_json(run / "revert_log.json", []))


TEMPLATE_MANIFEST = "template.manifest.json"


def template_fingerprint(template: Path) -> dict[str, str]:
    """sha256 of every file in a template project package, by relative path."""
    return {str(p.relative_to(template)): gfc.sha256_file(p)
            for p in sorted(template.rglob("*")) if p.is_file() and not p.is_symlink()}


def write_template_manifest(template: Path, *, host_id: str, kind: str) -> Path:
    """Fingerprint a reviewed template, next to it (not inside it)."""
    manifest = template.parent / f"{template.name}.{TEMPLATE_MANIFEST}"
    _write_json(manifest, {"host": host_id, "kind": kind,
                           "files": template_fingerprint(template)})
    return manifest


def check_template(template: Path, host_id: str, kind: str) -> None:
    manifest = template.parent / f"{template.name}.{TEMPLATE_MANIFEST}"
    doc = _read_json(manifest, None)
    if doc is None:
        raise SystemExit(f"stage: {template} has no reviewed manifest ({manifest.name})")
    if doc.get("host") != host_id or doc.get("kind") != kind:
        raise SystemExit(f"stage: {template} is a {doc.get('host')}/{doc.get('kind')} template")
    if doc.get("files") != template_fingerprint(template):
        raise SystemExit(f"stage: {template} changed since its manifest was written")


TEMPLATES = HERE / "real_host_templates.json"


def save_as_plan(host_id: str, target: Path) -> dict[str, Any]:
    """The UI steps that save the host's open project as `target`, a new
    project under the system temp directory. Pressing File > Save As through
    accessibility did not open Logic's save sheet, so the plan sends the key
    command with the host briefly in front and types the folder into the
    sheet's Go to Folder field. Only the host's own window is fronted."""
    resolved = Path(os.path.realpath(target))
    roots = {Path(os.path.realpath(p)) for p in (tempfile.gettempdir(), "/tmp")}
    if not any(r == resolved.parent or r in resolved.parents for r in roots):
        raise SystemExit(f"save-as-plan: {target} is not under the system temp directory")
    if resolved.suffix != HOSTS[host_id]["project_suffix"]:
        raise SystemExit(f"save-as-plan: a {host_id} project ends in "
                         f"{HOSTS[host_id]['project_suffix']}")
    if resolved.exists():
        raise SystemExit(f"save-as-plan: {target} exists; a template is never overwritten")
    if not resolved.parent.is_dir():
        raise SystemExit(f"save-as-plan: {resolved.parent} does not exist")
    front = {"delivery_mode": "foreground", "window": "project"}
    return {"host": host_id, "bundle_id": HOSTS[host_id]["bundle_id"],
            "target": str(resolved), "steps": [
                dict(front, action="hotkey", keys=["cmd", "shift", "s"]),
                {"action": "await_window", "role": "AXSheet"},
                dict(front, action="hotkey", keys=["cmd", "shift", "g"]),
                {"action": "type_text", "text": str(resolved.parent)},
                {"action": "press_key", "key": "return"},
                {"action": "type_text", "field": "name", "text": resolved.stem},
                {"action": "press_key", "key": "return"},
                {"action": "verify_exists", "path": str(resolved)}]}


def stage(run: Path, *, fault: str, at_sample: int, window_post: int, stimulus_frames: int,
          planted: str | None = None, provider: str | None = None, input_mode: str = "host",
          template: Path | None = None, fmt: str | None = None, template_kind: str = "playback",
          now: Callable[[], float] = time.time) -> dict:
    """Write the run's inputs where the sandboxed plug-in can read them, the
    rendezvous that points at them, and a throwaway copy of the template."""
    st = state(run)
    host_id = st.get("host")
    if not host_id:
        raise SystemExit("stage: run `preflight` first")
    if st.get("preflight_refusals"):
        raise SystemExit("stage: the preflight refused this run")
    if template_kind == "live" and input_mode != "internal":
        raise SystemExit("stage: a live-template row runs on the internal stimulus (M6)")
    fmt = fmt or HOSTS[host_id]["formats"][0]
    if fmt not in HOSTS[host_id]["formats"]:
        raise SystemExit(f"stage: {host_id} does not load {fmt} plug-ins")
    nonce = secrets.token_hex(16)
    root = rendezvous_root()
    plugin_run = root / "runs" / nonce
    record_change(run, "rendezvous_run", path=str(plugin_run))
    plugin_run.mkdir(parents=True)
    if input_mode == "internal":
        stim = gfc.internal_stimulus_span(0, stimulus_frames)
    else:
        stim = gfc.make_stimulus(stimulus_frames)
    gfc.write_wav_f32(plugin_run / "stimulus.wav", [stim] * gfc.CHANNELS)
    gfc.write_f32(plugin_run / "ir.f32", gfc.make_impulse_response())
    request: dict[str, Any] = {"fault": fault, "at_project_sample": at_sample,
                               "after_project_sample": at_sample + window_post,
                               "input": input_mode}
    if planted:
        request["planted_negative"] = planted
    (plugin_run / "fault-request.json").write_text(json.dumps(request) + "\n",
                                                   encoding="utf-8")
    active = {"schema": RENDEZVOUS_SCHEMA, "run_dir": str(plugin_run),
              "request": str(plugin_run / "fault-request.json"),
              "ir": str(plugin_run / "ir.f32"), "nonce": nonce,
              "expires_unix": int(now()) + RENDEZVOUS_LIFETIME_S}
    if provider == "staged":
        active["provider"] = "staged"
    if input_mode == "internal":
        active["stimulus"] = "internal"
    project = None
    if template is not None:
        check_template(template, host_id, template_kind)
        copy_dir = Path(tempfile.mkdtemp(prefix=f"pulp-wpk-{host_id}-"))
        record_change(run, "throwaway_project", path=str(copy_dir))
        project = copy_dir / template.name
        shutil.copytree(template, project, symlinks=True)
    # Exclusive: a rendezvous that appeared since the preflight (another run)
    # is never overwritten.
    record_change(run, "rendezvous_active", path=str(root / "active.json"), nonce=nonce)
    try:
        fd = os.open(root / "active.json", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o644)
    except FileExistsError:
        _mark_reverted(run, "rendezvous_active", str(root / "active.json"))
        raise SystemExit("stage: another rendezvous appeared since the preflight; not "
                         "overwriting it")
    with os.fdopen(fd, "w", encoding="utf-8") as out:
        out.write(json.dumps(active, indent=1, sort_keys=True) + "\n")
    return save_state(run, nonce=nonce, plugin_run=str(plugin_run), fault=fault,
                      at_sample=at_sample, window_post=window_post,
                      stimulus_frames=stimulus_frames, planted=planted, provider=provider,
                      input=input_mode, format=fmt, template_kind=template_kind,
                      project=str(project) if project else None,
                      control_kind=("positive_control" if fault == "positive_control" and
                                    not planted else
                                    "planted_negative" if planted else "fault"))


def launch_command(run: Path, *, rosetta: bool = False) -> list[str]:
    st = state(run)
    host = HOSTS[st["host"]]
    cmd = ["open", "-g", "-n"]
    if rosetta:
        # The arm64 AU then loads out of process (AUHostingServiceXPC_arrow):
        # a PROXY for out-of-process hosting, never quoted as the OOP result.
        cmd += ["--arch", "x86_64"]
    cmd += ["-a", host["app"]]
    if st.get("project"):
        cmd.append(st["project"])
    return cmd + list(host["open_args"])


def launch(run: Path, *, rosetta: bool = False, timeout: float = 60.0,
           processes: Callable[[], list[tuple[int, str]]] = list_processes,
           execute: Callable[[list[str]], None] | None = None) -> dict:
    """Open the host in the background (never activating it) and find the
    process this launch started."""
    st = state(run)
    host = HOSTS[st["host"]]
    if host.get("prefs_domains") and not prefs_snapshot_taken(run):
        raise SystemExit("launch: run `snapshot-prefs` first; launching the host rewrites its "
                         "preferences")
    before = {pid for pid, path in processes() if path == host["executable"]}
    if before:
        raise SystemExit("launch: the host is already running")
    launched = now_us()
    cmd = launch_command(run, rosetta=rosetta)
    (execute or (lambda c: subprocess.run(c, check=True)))(cmd)
    deadline = time.monotonic() + timeout
    pid = None
    while time.monotonic() < deadline and pid is None:
        fresh = [p for p, path in processes() if path == host["executable"] and p not in before]
        pid = max(fresh) if fresh else None
        if pid is None:
            time.sleep(0.5)
    if pid is None:
        raise SystemExit("launch: the host process never appeared")
    engines = [p for p, path in processes()
               if host.get("engine_prefix") and path.startswith(host["engine_prefix"])]
    return save_state(run, launch_command=cmd, launch_unix_us=launched, host_pid=pid,
                      host_responsible_pid=responsible_pid(pid), engine_pids=engines,
                      binary_arch="x86_64-rosetta" if rosetta else "arm64",
                      hosting_mode_hint="auhostingservice_arrow" if rosetta else None)


def await_claim(run: Path, *, timeout: float = 300.0, poll: float = 0.25) -> dict:
    """Wait for the plug-in instance to claim the run, then delete the
    rendezvous at once so no later instance (a scan, a second insert) can
    pick it up."""
    st = state(run)
    claim = Path(st["plugin_run"]) / "claim"
    active = rendezvous_root() / "active.json"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not claim.exists():
        time.sleep(poll)
    if _remove_active_if_ours(active, st.get("nonce")):
        _mark_reverted(run, "rendezvous_active", str(active))
    if not claim.exists():
        raise SystemExit("await-claim: no instance claimed the run (INCONCLUSIVE)")
    return save_state(run, claimed=True)


def _mark_reverted(run: Path, kind: str, path: str) -> None:
    entries = _read_json(run / "revert_log.json", [])
    for entry in entries:
        if entry["kind"] == kind and entry["path"] == path and not entry.get("reverted"):
            entry["reverted"] = True
            entry["reverted_at_unix_us"] = now_us()
    _write_json(run / "revert_log.json", entries)


def realtime_begin(run: Path) -> dict:
    """Immediately before the agent starts the host's realtime render."""
    return save_state(run, realtime_started_unix_us=now_us())


def realtime_end(run: Path, artifact: Path, *, settle_s: float = 2.0,
                 timeout: float = 600.0, poll: float = 0.25) -> dict:
    """Wait for the realtime render to finish writing (its size stable for
    `settle_s`), then write the provenance record v2 requires (M1): this
    artifact, rendered in realtime by this host, over at least the audio's
    duration."""
    st = state(run)
    started = st.get("realtime_started_unix_us")
    if not started:
        raise SystemExit("realtime-end: `realtime-begin` was never recorded")
    deadline = time.monotonic() + timeout
    last, stable_since = -1, None
    while time.monotonic() < deadline:
        size = artifact.stat().st_size if artifact.exists() else -1
        if size > 0 and size == last:
            stable_since = stable_since or time.monotonic()
            if time.monotonic() - stable_since >= settle_s:
                break
        else:
            stable_since = None
        last = size
        time.sleep(poll)
    else:
        raise SystemExit("realtime-end: the realtime render never finished")
    # The finish time is when the file last changed, not when we noticed.
    finished = int(artifact.stat().st_mtime * 1e6)
    record = {"artifact": "realtime.wav", "mode": "realtime", "host": st["host"],
              "sample_rate": gfc.SAMPLE_RATE, "started_unix_us": started,
              "finished_unix_us": finished}
    return save_state(run, realtime_artifact=str(artifact), realtime_provenance=record)


def collect(run: Path, *, bounce: Path | None, host_evidence: Path, hosting_mode: str,
            host_alive: bool, host_version: str, code_commit: str,
            plugin_executable: Path, tolerance: float = gfc.DEFAULT_TOLERANCE,
            window_pre: int = gfc.DEFAULT_WINDOW_PRE,
            host_settings: dict | None = None) -> tuple[dict, str, str]:
    """Bring the plug-in's run directory and the host's renders together,
    write the v2 receipt, and verify it."""
    st = state(run)
    ax = json.loads(host_evidence.read_text(encoding="utf-8"))
    # The snapshot taken before playback must show a track no input device
    # could reach; refuse before writing a receipt the verifier would reject.
    problem = hosts.track_state_problem(ax.get("track"), st["host"])
    if problem:
        raise SystemExit(f"collect: {problem}")
    out = run / "artifacts"
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(st["plugin_run"], out, symlinks=False)
    shutil.copyfile(run / "preflight.json", out / "preflight.json")
    if bounce is not None:
        shutil.copyfile(bounce, out / "bounce.wav")
    realtime = st.get("realtime_artifact")
    if realtime:
        shutil.copyfile(realtime, out / "realtime.wav")
        _write_json(out / hosts.REALTIME_PROVENANCE, st["realtime_provenance"])
    _write_json(out / "host_evidence.json", {
        "plugins": ax.get("plugins", []), "track": ax["track"], "host_pid": st["host_pid"],
        "host_executable": HOSTS[st["host"]]["executable"],
        "ax_snapshot_sha256": gfc.sha256_file(host_evidence), "ax": ax})
    host = {"id": st["host"], "version": host_version, "pid": st["host_pid"],
            "responsible_pid": st.get("host_responsible_pid", -1),
            "executable": HOSTS[st["host"]]["executable"],
            "engine_pids": st.get("engine_pids", []),
            "launch_unix_us": st["launch_unix_us"], "binary_arch": st.get("binary_arch"),
            "launch": " ".join(st.get("launch_command", []))}
    _, stimulus = gfc.read_wav(out / "stimulus.wav")
    receipt, verdict, outcome = _write_receipt(
        out, control_kind=st["control_kind"], fault=st["fault"], fault_sample=st["at_sample"],
        transport="rendezvous_file", nonce=st["nonce"], host=host, hosting_mode=hosting_mode,
        settings=dict(host_settings or {}, template=st.get("template_kind", "playback")),
        plugin={"format": st["format"], "name": PLUGIN_NAME,
                "executable_sha256": gfc.sha256_file(plugin_executable)},
        code_commit=code_commit, bounce=bounce is not None, realtime=bool(realtime),
        tolerance=tolerance, window_pre=window_pre, window_post=st["window_post"],
        render_frames=len(stimulus[0]) + st["window_post"], host_alive=host_alive)
    save_state(run, verdict=verdict, outcome=outcome)
    return receipt, verdict, outcome


def _write_receipt(out: Path, *, control_kind: str, fault: str, fault_sample: int,
                   transport: str, nonce: str, host: dict, hosting_mode: str, settings: dict,
                   plugin: dict, code_commit: str, bounce: bool, realtime: bool,
                   tolerance: float, window_pre: int, window_post: int, render_frames: int,
                   host_alive: bool, extra_artifacts: tuple[str, ...] = ()
                   ) -> tuple[dict, str, str]:
    """Evaluate the run in `out`, write its v2 receipt there, and verify it."""
    names = ["stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
             "claim", "host_evidence.json", *extra_artifacts]
    if bounce:
        names.append("bounce.wav")
    if realtime:
        names += ["realtime.wav", hosts.REALTIME_PROVENANCE]
    artifacts = gfc.artifact_digests(out, names)
    preflight_doc = gfc.summarize_preflight(_read_json(out / "preflight.json", {}))
    evidence = hosts.evaluate_v2(
        out, control_kind=control_kind, fault=fault, fault_sample=fault_sample,
        tolerance=tolerance, window_pre=window_pre, window_post=window_post,
        host_alive=host_alive, bounce_path=(out / "bounce.wav") if bounce else None,
        realtime_path=(out / "realtime.wav") if realtime else None,
        render_frames=render_frames, nonce=nonce, host=host, hosting_mode=hosting_mode,
        host_quiet=preflight_doc["quiet_host"], code_under_test={"commit": code_commit},
        listed=list(artifacts))
    receipt = hosts.build_receipt_v2(
        control_kind=control_kind, trigger_transport=transport,
        nonce=nonce, host=host, hosting_mode=hosting_mode, host_settings=settings,
        plugin=plugin, code_under_test={"commit": code_commit}, artifacts=artifacts,
        bounce_artifact="bounce.wav" if bounce else None,
        host_realtime_artifact="realtime.wav" if realtime else None,
        preflight=preflight_doc, host_quiet=preflight_doc["quiet_host"],
        analysis={"tolerance": tolerance, "window": {"pre": window_pre, "post": window_post},
                  "render_frames": render_frames},
        column_outcome=hosts.column_outcome(evidence, control_kind=control_kind,
                                            host_id=host["id"]),
        **{k: evidence[k] for k in V2.RECOMPUTED})
    _write_json(out / "receipt.json", receipt)
    verdict, outcome = V2.verify(receipt, out)
    return receipt, verdict, outcome


def _preflight_sampled_us(doc: dict) -> int | None:
    """When the preflight sampled the machine, from host_vitals' own
    `sampled_at` (whole seconds, so this rounds down). REAPER's harness takes
    the preflight before it launches REAPER, which makes this a lower bound
    on the launch."""
    for obs in doc.get("observations", []):
        if Path((obs.get("argv") or [""])[0]).name != "host_vitals.sh":
            continue
        try:
            return int(json.loads(obs.get("stdout", ""))["sampled_at"]) * 1_000_000
        except (ValueError, KeyError, TypeError):
            return None
    return None


def reaper_control_kind(controls: dict, fault_kind: str) -> str:
    """The v2 control kind of a v1 REAPER run, from what the plug-in armed."""
    if controls.get("planted_negative") or controls.get("gpu_off"):
        return "planted_negative"
    return hosts.POSITIVE_CONTROL if fault_kind == hosts.POSITIVE_CONTROL else "fault"


def wrap_reaper(reaper_run: Path, out: Path, *, code_commit: str) -> tuple[dict, str, str]:
    """A v2 receipt for a REAPER run that reaper_smoke.py already verified as
    v1, so REAPER rows read in the same terms as every other host (a planted
    negative is `negative_proved`, not just "fail"). Nothing is re-run: the
    v1 artifacts are copied and re-evaluated."""
    v1_receipt = json.loads((reaper_run / "receipt.json").read_text(encoding="utf-8"))
    V2.v1.verify(v1_receipt, reaper_run)
    if out.exists():
        raise SystemExit(f"wrap-reaper: {out} exists")
    out.mkdir(parents=True)
    for name in v1_receipt["artifacts"]:
        shutil.copyfile(reaper_run / name, out / name)
    launch = _preflight_sampled_us(_read_json(out / "preflight.json", {}))
    if launch is None:
        raise SystemExit("wrap-reaper: preflight.json has no host_vitals sample to bound the "
                         "launch")
    v1_host, plugin = v1_receipt["host"], v1_receipt["plugin"]
    log_text = (out / "reaper.out.txt").read_text(encoding="utf-8", errors="replace")
    _write_json(out / "host_evidence.json", {
        "plugins": [plugin["name"]] if plugin["name"] in log_text else [],
        "host_pid": v1_host["reaper_pid"], "host_executable": v1_host["reaper_bin"],
        "source": "reaper.out.txt"})
    host = {"id": "reaper", "version": v1_host["reaper_version"], "pid": v1_host["reaper_pid"],
            "responsible_pid": -1, "executable": v1_host["reaper_bin"], "engine_pids": [],
            "launch_unix_us": launch, "launch_source": "preflight host_vitals sampled_at",
            "binary_arch": v1_host.get("machine"), "launch": "reaper_smoke.py"}
    fault, analysis = v1_receipt["fault"], v1_receipt["analysis"]
    return _write_receipt(
        out, control_kind=reaper_control_kind(v1_receipt["controls"], fault["kind"]),
        fault=fault["kind"],
        fault_sample=fault["requested_project_sample"],
        transport=v1_receipt["trigger_transport"], nonce=v1_receipt["nonce"], host=host,
        hosting_mode="in_process",
        settings={"template": "reaper_smoke",
                  "anticipative_fx": v1_receipt.get("anticipative_fx")},
        plugin={k: plugin[k] for k in ("format", "name", "executable_sha256")},
        code_commit=code_commit, bounce=v1_receipt.get("bounce_artifact") is not None,
        realtime=v1_receipt.get("host_realtime_artifact") is not None,
        tolerance=analysis["tolerance"], window_pre=analysis["window"]["pre"],
        window_post=analysis["window"]["post"], render_frames=analysis["render_frames"],
        host_alive=bool(v1_receipt["host_alive"]), extra_artifacts=("reaper.out.txt",))


# ── CLI ───────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("preflight")
    p.add_argument("--host", choices=sorted(HOSTS), required=True)
    p.add_argument("--run", type=Path, required=True)
    p = sub.add_parser("install-au")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--component", type=Path, required=True)
    p = sub.add_parser("snapshot-prefs")
    p.add_argument("--run", type=Path, required=True)
    p = sub.add_parser("stage")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--fault", required=True,
                   choices=sorted(set(gfc.FAULT_KINDS)))
    p.add_argument("--at", type=int, required=True, help="Fault project sample.")
    p.add_argument("--window-post", type=int, default=gfc.DEFAULT_WINDOW_POST)
    p.add_argument("--stimulus-frames", type=int, default=4 * gfc.SAMPLE_RATE)
    p.add_argument("--planted", choices=sorted(hosts.PLANTED_NEGATIVES))
    p.add_argument("--provider", choices=["shared", "staged"], default="shared")
    p.add_argument("--input", choices=["host", "internal"], default="host")
    p.add_argument("--template", type=Path)
    p.add_argument("--template-kind", choices=["playback", "live"], default="playback")
    p.add_argument("--format", choices=["au", "clap", "vst3"])
    p = sub.add_parser("launch")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--rosetta", action="store_true")
    p = sub.add_parser("await-claim")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--timeout", type=float, default=300.0)
    p = sub.add_parser("realtime-begin")
    p.add_argument("--run", type=Path, required=True)
    p = sub.add_parser("realtime-end")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--artifact", type=Path, required=True)
    p = sub.add_parser("collect")
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--bounce", type=Path)
    p.add_argument("--host-evidence", type=Path, required=True,
                   help="The AX snapshot (JSON) showing the plug-in on the insert.")
    p.add_argument("--hosting-mode", choices=hosts.HOSTING_MODES, required=True)
    p.add_argument("--host-version", required=True)
    p.add_argument("--code-commit", required=True)
    p.add_argument("--plugin-executable", type=Path, required=True)
    p.add_argument("--host-dead", action="store_true")
    p = sub.add_parser("template-manifest")
    p.add_argument("--template", type=Path, required=True)
    p.add_argument("--host", choices=sorted(HOSTS), required=True)
    p.add_argument("--kind", choices=["playback", "live"], required=True)
    p = sub.add_parser("revert")
    p.add_argument("--run", type=Path, required=True)
    p = sub.add_parser("save-as-plan")
    p.add_argument("--host", choices=sorted(HOSTS), required=True)
    p.add_argument("--target", type=Path, required=True)
    p = sub.add_parser("wrap-reaper")
    p.add_argument("--reaper-run", type=Path, required=True,
                   help="A reaper_smoke.py --mode gpu-fault --out-dir holding a v1 receipt.")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--code-commit", required=True)
    args = ap.parse_args(argv)

    if args.cmd == "wrap-reaper":
        try:
            _, verdict, outcome = wrap_reaper(args.reaper_run, args.out,
                                              code_commit=args.code_commit)
        except SystemExit as exc:
            log(str(exc.code))
            return EXIT_USAGE
        except V2.ReceiptError as exc:
            log(f"INVALID: {exc}")
            return EXIT_USAGE
        log(f"verdict={verdict} outcome={outcome}")
        return EXIT_OK if outcome in V2.FULL_OUTCOMES else EXIT_NOT_PASS

    if args.cmd == "template-manifest":
        log(f"wrote {write_template_manifest(args.template, host_id=args.host, kind=args.kind)}")
        return EXIT_OK
    if args.cmd == "save-as-plan":
        try:
            plan = save_as_plan(args.host, args.target)
        except SystemExit as exc:
            log(str(exc.code))
            return EXIT_USAGE
        print(json.dumps(plan, indent=1))
        return EXIT_OK
    run = args.run
    try:
        if args.cmd == "preflight":
            ok, reasons = preflight(args.host, run)
            for reason in reasons:
                log(f"REFUSED: {reason}")
            return EXIT_OK if ok else EXIT_REFUSED
        if args.cmd == "install-au":
            log(f"installed {install_au(run, args.component)}")
        elif args.cmd == "snapshot-prefs":
            log(f"prefs saved to {snapshot_prefs(run, state(run)['host'])}")
        elif args.cmd == "stage":
            st = stage(run, fault=args.fault, at_sample=args.at, window_post=args.window_post,
                       stimulus_frames=args.stimulus_frames, planted=args.planted,
                       provider=None if args.provider == "shared" else args.provider,
                       input_mode=args.input, template=args.template, fmt=args.format,
                       template_kind=args.template_kind)
            log(f"staged nonce {st['nonce']}; launch with: {' '.join(launch_command(run))}")
        elif args.cmd == "launch":
            _install_revert_on_signals(run)
            st = launch(run, rosetta=args.rosetta)
            log(f"host pid {st['host_pid']} (responsible {st['host_responsible_pid']})")
        elif args.cmd == "await-claim":
            _install_revert_on_signals(run)
            await_claim(run, timeout=args.timeout)
            log("claimed; rendezvous removed")
        elif args.cmd == "realtime-begin":
            realtime_begin(run)
        elif args.cmd == "realtime-end":
            _install_revert_on_signals(run)
            realtime_end(run, args.artifact)
        elif args.cmd == "collect":
            _, verdict, outcome = collect(
                run, bounce=args.bounce, host_evidence=args.host_evidence,
                hosting_mode=args.hosting_mode, host_alive=not args.host_dead,
                host_version=args.host_version, code_commit=args.code_commit,
                plugin_executable=args.plugin_executable)
            log(f"verdict={verdict} outcome={outcome}")
            return EXIT_OK if outcome in V2.FULL_OUTCOMES else EXIT_NOT_PASS
        elif args.cmd == "revert":
            try:
                entries = revert(run)
            except RuntimeError as exc:
                log(str(exc))
                return EXIT_REFUSED
            log(f"reverted {sum(1 for e in entries if e.get('reverted'))} of {len(entries)}")
    except SystemExit as exc:
        if isinstance(exc.code, str):
            log(exc.code)
            return EXIT_USAGE
        raise
    except (OSError, ValueError, KeyError) as exc:
        log(f"error: {exc}")
        return EXIT_USAGE
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
