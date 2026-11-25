"""Isolation, busy-host, and stall detection for the REAPER smoke harness.

REAPER is a shared desktop application, so a smoke run has to answer four
questions before and while it drives one:

* Is somebody else's REAPER running?  A second launch can hand its script to
  the running instance, and the run would steal a session that is not ours.
* Will the run scan only the plug-in under test?  Every other installed
  bundle is someone else's code; one that crashes on scan poisons the run and
  arms AppKit's crash-restore alert for every later launch.
* Will AppKit's "reopen windows?" alert block the launch?  After any REAPER
  crash, including a crash in its scan subprocess, the next launch waits on
  that modal, and so does the scan subprocess.
* Is the run making progress?  A blocked modal or a hung scan otherwise looks
  like an ordinary slow launch and burns the whole cap.

Everything here except `list_windows` and `list_processes` is pure, so the
unit tests drive it with recorded process and window snapshots.
"""
from __future__ import annotations

import ctypes
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

REAPER_PROCESS_NAME = "REAPER"
REAPER_DEFAULTS_DOMAIN = "com.cockos.reaper"
PERSISTENCE_KEY = "ApplePersistenceIgnoreState"

# REAPER prints this through NSLog when AppKit honors the persistence opt-out,
# which is how a run proves the restore alert cannot appear.
PERSISTENCE_HONORED_MARKER = "ApplePersistenceIgnoreState: Existing state will not be touched"

# A scan subprocess is REAPER re-executing itself with a private argument
# such as -__vst_scan__.
SCAN_ARG = re.compile(r"-__\w*scan\w*__")

# Every plug-in scan-path key REAPER reads from reaper.ini on any platform or
# architecture: vstpath, vstpath64, vstpath_arm64, clap_path,
# clap_path_macos-aarch64, clap_path_macos-x86_64, lv2path_*.  Any one that is
# left at the user's value widens the scan to their whole plug-in folder.
SCAN_PATH_KEY = re.compile(r"^(?:vstpath|clap[_-]?path|lv2[_-]?path)", re.IGNORECASE)
SCAN_PATH_KEYS = (
    "vstpath", "vstpath64", "vstpath_arm64",
    "clap_path", "clap_path_macos-aarch64", "clap_path_macos-x86_64",
)

# Built-in REAPER plug-ins live inside the app bundle and are always cached.
BUILTIN_CACHE_ENTRY = re.compile(r"^rea[\w.-]*\.(?:vst\.dylib|clap)$", re.IGNORECASE)

# An alert is small; REAPER's main window is not.  A window this size or
# smaller with no progress is treated as a modal.
MODAL_MAX_WIDTH = 800
MODAL_MAX_HEIGHT = 500


def launch_argv(reaper: Path | str, cfgfile: Path | str, *rest: str) -> list[str]:
    """The argv every smoke launch uses.

    * ``-newinst`` keeps REAPER from handing the script to an instance that is
      already running.
    * ``-nosplash`` and ``-ignoreerrors`` keep startup free of windows that
      wait for a click.
    * ``-ApplePersistenceIgnoreState YES`` lands in NSUserDefaults' argument
      domain, which AppKit reads before deciding to show the crash-restore
      alert, so it covers this process without writing the user's defaults.
      It must come LAST: REAPER stops reading its own options at the first
      argument it does not know, so the pair placed before ``-cfgfile`` made
      REAPER 7.84 ignore the isolated config and open the user's real
      profile.  REAPER re-launches itself for scans without the pair, which is
      why `scoped_persistence_opt_out` exists as well.
    """
    return [str(reaper), "-newinst", "-nosplash", "-ignoreerrors",
            "-cfgfile", str(cfgfile), *rest, f"-{PERSISTENCE_KEY}", "YES"]


def profile_in_use(portable: Path) -> bool:
    """True once REAPER has initialized the isolated resource directory.

    REAPER writes reaper-install-rev.txt into whichever resource directory it
    adopted within a second or two of launch.  If it is still absent, REAPER is
    running against some other profile (the user's real one) and must be
    stopped before it scans or saves anything there.
    """
    return (portable / "reaper-install-rev.txt").exists()


def isolated_ini_text(base: str, scan_dir: Path | str, machine: str,
                      extra: dict[str, str] | None = None) -> str:
    """Return a reaper.ini whose only plug-in scan root is `scan_dir`.

    `base` is the user's reaper.ini (licence, audio device, preferences are
    kept so REAPER does not come up as a fresh install).  Every scan-path key
    in it is dropped, whatever its spelling, and every key for every
    architecture is set to `scan_dir`, so no stray key can point REAPER back at
    ~/Library/Audio/Plug-Ins.  The recent-project list and last project are
    removed so the run never opens somebody's session.
    """
    scan = str(scan_dir)
    ours: dict[str, str] = {key: scan for key in SCAN_PATH_KEYS}
    machine = machine.lower()
    if machine in ("arm64", "aarch64"):
        ours["vstpath_arm64"] = scan
        ours["clap_path_macos-aarch64"] = scan
    # Marks plug-in path preferences initialized; without it REAPER appends
    # every system plug-in directory to the configured paths.
    ours["vstfullstate"] = "33605"
    ours["splashscreen"] = "0"
    ours["lastproject"] = ""
    ours["loadlastproj"] = "0"
    ours.update(extra or {})

    kept: list[str] = []
    section = ""
    for line in base.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped.lower()
        if section == "[recent]" and not stripped.startswith("["):
            continue
        key = line.split("=", 1)[0].strip()
        if key in ours or SCAN_PATH_KEY.match(key):
            continue
        kept.append(line)
    out: list[str] = []
    inserted = False
    for line in kept:
        out.append(line)
        if not inserted and line.strip().lower() == "[reaper]":
            out.extend(f"{k}={v}" for k, v in ours.items())
            inserted = True
    if not inserted:
        out = ["[REAPER]", *(f"{k}={v}" for k, v in ours.items()), *out]
    return "\n".join(out) + "\n"


def foreign_cache_entries(cache_text: str, allowed_bundle: str) -> list[str]:
    """Bundles a REAPER plug-in cache lists that are neither built in nor ours.

    The VST cache keys one bundle per line under ``[vstcache]``; the CLAP
    cache opens one ``[<bundle>.clap]`` section per bundle.  A non-empty
    result means the scan was not confined to the temp scan directory,
    whatever the ini said.
    """
    allowed = {allowed_bundle.casefold(), allowed_bundle.replace(" ", "_").casefold()}
    foreign: list[str] = []

    def judge(name: str) -> None:
        if name.casefold() not in allowed and not BUILTIN_CACHE_ENTRY.match(name):
            foreign.append(name)

    section = ""
    for line in cache_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip()
            if section.casefold() != "vstcache":
                judge(section)
            continue
        if section.casefold() == "vstcache" and "=" in stripped:
            judge(stripped.split("=", 1)[0].strip())
    return foreign


# --------------------------------------------------------------------------
# Busy host.

def busy_reason(reaper_pids: Iterable[int], busy_flag: Path | None) -> str | None:
    """Why this run must not launch REAPER now, or None when it may."""
    pids = sorted(set(reaper_pids))
    if pids:
        return ("another REAPER is running (pid " + ", ".join(map(str, pids)) +
                "); a second launch would interfere with it")
    if busy_flag is not None and busy_flag.exists():
        return f"the on-screen busy flag {busy_flag} is present"
    return None


def running_reaper_pids() -> list[int]:
    """PIDs whose process name is exactly REAPER (never an argv match)."""
    r = subprocess.run(["pgrep", "-x", REAPER_PROCESS_NAME], capture_output=True, text=True, encoding="utf-8")
    return [int(tok) for tok in r.stdout.split() if tok.isdigit()]


def busy_flag_path() -> Path | None:
    raw = os.environ.get("PULP_DAW_SMOKE_BUSY_FILE", "").strip()
    return Path(os.path.expanduser(raw)) if raw else None


# --------------------------------------------------------------------------
# Scoped crash-restore opt-out.

DefaultsRunner = Callable[[list[str]], "subprocess.CompletedProcess[str]"]


def _run_defaults(argv: list[str]) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(["defaults", *argv], capture_output=True, text=True, encoding="utf-8")


class scoped_persistence_opt_out:
    """Set REAPER's ApplePersistenceIgnoreState for one run, then put it back.

    REAPER's scan subprocess is a fresh launch that does not carry the parent's
    argv, so only the per-app default reaches it.  The prior state is recorded
    exactly (absent, or its value) and restored on exit, including on an
    exception, so the user's REAPER is left as it was found.  Use it only after
    `busy_reason` says no other REAPER is running: the setting is per-user.
    """

    def __init__(self, runner: DefaultsRunner = _run_defaults):
        self._run = runner
        self.prior: str | None = None
        self.active = False

    def __enter__(self) -> "scoped_persistence_opt_out":
        if sys.platform != "darwin" and self._run is _run_defaults:
            return self
        read = self._run(["read", REAPER_DEFAULTS_DOMAIN, PERSISTENCE_KEY])
        self.prior = read.stdout.strip() if read.returncode == 0 else None
        self._run(["write", REAPER_DEFAULTS_DOMAIN, PERSISTENCE_KEY, "-bool", "YES"])
        self.active = True
        return self

    def __exit__(self, *exc) -> None:
        self.restore()

    def restore(self) -> None:
        if not self.active:
            return
        self.active = False
        if self.prior is None:
            self._run(["delete", REAPER_DEFAULTS_DOMAIN, PERSISTENCE_KEY])
        else:
            value = "YES" if self.prior in ("1", "YES", "true") else "NO"
            self._run(["write", REAPER_DEFAULTS_DOMAIN, PERSISTENCE_KEY, "-bool", value])


# --------------------------------------------------------------------------
# Stall classification.

@dataclass(frozen=True)
class Proc:
    pid: int
    ppid: int
    elapsed: float
    args: str


@dataclass(frozen=True)
class Window:
    owner_pid: int
    layer: int
    width: float
    height: float
    onscreen: bool


def descendants(root: int, procs: Iterable[Proc]) -> list[Proc]:
    by_parent: dict[int, list[Proc]] = {}
    for p in procs:
        by_parent.setdefault(p.ppid, []).append(p)
    out: list[Proc] = []
    stack = [root]
    while stack:
        for child in by_parent.get(stack.pop(), []):
            out.append(child)
            stack.append(child.pid)
    return out


def classify_stall(root_pid: int, procs: list[Proc], windows: list[Window], *,
                   idle_seconds: float, stall_after: float,
                   scan_budget: float) -> str | None:
    """Name the reason a launch is not progressing, or None while it may be.

    * A scan subprocess never owns a window; one that does is showing an
      alert (in practice AppKit's crash-restore prompt) and will wait forever.
    * A scan subprocess alive past `scan_budget` is hung on a bundle.
    * The main process, idle past `stall_after`, whose only on-screen windows
      are alert-sized, is blocked on a modal.
    `idle_seconds` is the time since the run last showed progress (a cache
    write, captured output, or the status handshake).
    """
    tree = descendants(root_pid, procs)
    scans = [p for p in tree if SCAN_ARG.search(p.args)]
    for scan in scans:
        if any(w.owner_pid == scan.pid and w.onscreen for w in windows):
            return (f"BLOCKED-MODAL: the scan subprocess (pid {scan.pid}) is showing a "
                    "window, which is an alert waiting for a click (usually AppKit's "
                    "crash-restore prompt after an earlier REAPER crash)")
    for scan in scans:
        if scan.elapsed > scan_budget:
            return (f"SCAN-HUNG: the scan subprocess (pid {scan.pid}) has run for "
                    f"{scan.elapsed:.0f}s: {scan.args}")
    # A live scan subprocess is progress the file mtimes cannot see, and the
    # scan-progress window is alert-sized, so the main-process rules wait.
    if idle_seconds < stall_after or scans:
        return None
    mine = [w for w in windows if w.owner_pid == root_pid and w.onscreen and w.layer == 0]
    if mine and all(w.width <= MODAL_MAX_WIDTH and w.height <= MODAL_MAX_HEIGHT for w in mine):
        biggest = max(mine, key=lambda w: w.width * w.height)
        return (f"BLOCKED-MODAL: REAPER (pid {root_pid}) made no progress for "
                f"{idle_seconds:.0f}s and shows only an alert-sized window "
                f"({biggest.width:.0f}x{biggest.height:.0f})")
    if not mine:
        return (f"STALLED: REAPER (pid {root_pid}) made no progress for {idle_seconds:.0f}s "
                "and shows no window")
    return None


def _parse_etime(text: str) -> float:
    """ps etime: [[dd-]hh:]mm:ss."""
    days = 0
    if "-" in text:
        d, text = text.split("-", 1)
        days = int(d)
    parts = [int(x) for x in text.split(":")]
    while len(parts) < 3:
        parts.insert(0, 0)
    h, m, s = parts
    return float(days * 86400 + h * 3600 + m * 60 + s)


def parse_ps(text: str) -> list[Proc]:
    procs: list[Proc] = []
    for line in text.splitlines():
        fields = line.split(None, 3)
        if len(fields) < 3 or not fields[0].isdigit():
            continue
        try:
            procs.append(Proc(int(fields[0]), int(fields[1]), _parse_etime(fields[2]),
                              fields[3] if len(fields) > 3 else ""))
        except ValueError:
            continue
    return procs


def list_processes() -> list[Proc]:
    r = subprocess.run(["ps", "-axo", "pid=,ppid=,etime=,args="],
                       capture_output=True, text=True, encoding="utf-8")
    return parse_ps(r.stdout)


def list_windows() -> list[Window]:
    """Every window's owner, layer, size, and visibility via CGWindowList.

    These fields need no Screen Recording grant (only titles do).  Returns an
    empty list off macOS or if CoreGraphics cannot be loaded.
    """
    if sys.platform != "darwin":
        return []
    try:
        cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
    except OSError:
        return []
    vp = ctypes.c_void_p

    class CGRect(ctypes.Structure):
        _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double),
                    ("w", ctypes.c_double), ("h", ctypes.c_double)]

    cg.CGWindowListCopyWindowInfo.restype = vp
    cg.CGWindowListCopyWindowInfo.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    cg.CGRectMakeWithDictionaryRepresentation.restype = ctypes.c_bool
    cg.CGRectMakeWithDictionaryRepresentation.argtypes = [vp, ctypes.POINTER(CGRect)]
    cf.CFArrayGetCount.restype = ctypes.c_long
    cf.CFArrayGetCount.argtypes = [vp]
    cf.CFArrayGetValueAtIndex.restype = vp
    cf.CFArrayGetValueAtIndex.argtypes = [vp, ctypes.c_long]
    cf.CFDictionaryGetValue.restype = vp
    cf.CFDictionaryGetValue.argtypes = [vp, vp]
    cf.CFStringCreateWithCString.restype = vp
    cf.CFStringCreateWithCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_uint32]
    cf.CFNumberGetValue.restype = ctypes.c_bool
    cf.CFNumberGetValue.argtypes = [vp, ctypes.c_int, vp]
    cf.CFBooleanGetValue.restype = ctypes.c_bool
    cf.CFBooleanGetValue.argtypes = [vp]
    cf.CFRelease.argtypes = [vp]

    utf8 = 0x08000100
    keys = {name: cf.CFStringCreateWithCString(None, name.encode(), utf8)
            for name in ("kCGWindowOwnerPID", "kCGWindowLayer",
                         "kCGWindowBounds", "kCGWindowIsOnscreen")}

    def number(d: int, name: str) -> int:
        value = cf.CFDictionaryGetValue(d, keys[name])
        out = ctypes.c_longlong(0)
        if value:
            cf.CFNumberGetValue(value, 4, ctypes.byref(out))  # kCFNumberSInt64Type
        return out.value

    windows: list[Window] = []
    array = cg.CGWindowListCopyWindowInfo(0, 0)  # kCGWindowListOptionAll
    if not array:
        return windows
    try:
        for i in range(cf.CFArrayGetCount(array)):
            d = cf.CFArrayGetValueAtIndex(array, i)
            rect = CGRect()
            bounds = cf.CFDictionaryGetValue(d, keys["kCGWindowBounds"])
            if bounds:
                cg.CGRectMakeWithDictionaryRepresentation(bounds, ctypes.byref(rect))
            onscreen = cf.CFDictionaryGetValue(d, keys["kCGWindowIsOnscreen"])
            windows.append(Window(number(d, "kCGWindowOwnerPID"), number(d, "kCGWindowLayer"),
                                  rect.w, rect.h,
                                  bool(onscreen and cf.CFBooleanGetValue(onscreen))))
    finally:
        cf.CFRelease(array)
        for key in keys.values():
            cf.CFRelease(key)
    return windows


def latest_progress(paths: Iterable[Path]) -> float:
    """Newest mtime among the files a run writes as it advances."""
    newest = 0.0
    for path in paths:
        try:
            newest = max(newest, path.stat().st_mtime)
        except OSError:
            continue
    return newest
