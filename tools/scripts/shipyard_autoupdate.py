#!/usr/bin/env python3
"""Converge this machine's installed Shipyard onto the version pinned in
`tools/shipyard.toml`, when the machine is idle.

WHY "converge onto the pin" AND NOT "update to latest"
------------------------------------------------------
`tools/shipyard.toml` is the single source of truth for the Shipyard
version this repo uses: its header requires a PR (`shipyard pin bump`) to
move it, `tools/install-shipyard.sh` installs exactly it, and
`check_shipyard_pin.py` fails the build when any workflow's
`SHIPYARD_VERSION` disagrees with it. A machine running a version other
than the pin runs a Shipyard that was never validated against Pulp's CI
matrix, and disagrees with what the workflows declare.

So "up to date" here means "equal to the pin", in BOTH directions:

    installed < pin   →  upgrade   (the machine fell behind — the stated pain)
    installed > pin   →  downgrade (the machine drifted ahead — see below)
    installed == pin  →  no-op, silently

The downgrade direction is not hypothetical. A bare `shipyard update`
tracks `latest`, which is well ahead of the pin, so a single stray
`shipyard update` on any machine strands it ahead of the pin
indefinitely.

WHY THIS DOES NOT JUST CALL `shipyard update`
---------------------------------------------
It does — for the upgrade direction, which is the documented in-tool
path. But `shipyard update` is an *updater*, not a *converger*: asked to
move to a version older than the installed one it reports
`update_available: false` and does nothing. That is correct for its own
purpose and useless for ours, because it means the ahead-of-pin machine
can never come back. So this script dispatches on direction:

    behind the pin  →  `shipyard update --to <pin>`   (in-tool path)
    ahead of the pin →  `tools/install-shipyard.sh`   (unconditional pinned
                                                       install, checksum-
                                                       verified upstream)

Both are existing, checksum-verifying install paths; this script chooses
between them and verifies the outcome. It never downloads or swaps a
binary itself.

SAFETY MODEL
------------
* Never mid-job. Updating while this host validates a PR could swap the
  binary under an in-flight ship. The idle gate refuses to update while a
  Pulp `Runner.Worker` or a validating `shipyard` subcommand is alive.
  The persistent `shipyard daemon` does NOT count as busy (it always
  runs; counting it would mean never updating).
* Fail closed. Any probe that cannot answer (ps fails, version
  unreadable, host offline) is treated as "do not update". The working
  binary is left exactly where it is.
* One installer at a time. The install step is held under a machine-wide
  lock, so a hand-run converger and a background tick cannot both write
  the binary.
* Verify the outcome. After any install path runs, the installed version
  is re-read and must equal the pin. If it does not, that is reported as
  a failure rather than assumed to have worked.
* Quiet. Silence when already at the pin (the overwhelmingly common
  case). Every decision is still published to a state file for
  observability without nagging.

KILL SWITCH
-----------
Either of these disables the whole thing:

    PULP_SHIPYARD_AUTOUPDATE=0                     (env; for shells + tests)
    ~/.config/pulp/shipyard-autoupdate → `off`     (file; for the LaunchAgent,
                                                    which has no useful env)

The file is the one that matters in practice: a launchd agent does not
inherit your shell env, so the env var alone would be a kill switch that
cannot reach the thing it is meant to kill.

Exit codes:
    0 — at the pin, converged, or deliberately skipped (disabled/busy/offline)
    1 — an update was attempted and failed (working binary left in place)
    2 — configuration error (no pin file / unparseable pin)

Test seams (production leaves them unset):
    PULP_SHIPYARD_AUTOUPDATE_REPO       checkout holding tools/shipyard.toml
    PULP_SHIPYARD_AUTOUPDATE_CONFIG     kill-switch file path
    PULP_SHIPYARD_AUTOUPDATE_STATE_DIR  where the decision JSON is published
    PULP_SHIPYARD_BIN                   shipyard binary to probe/invoke
    PULP_SHIPYARD_AUTOUPDATE_PS         stub replacing `ps`

Pure stdlib; no third-party deps.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

# `version = "v0.70.0"` inside the [shipyard] table. Mirrors the regex in
# check_shipyard_pin.py so the two agree on what "the pin" is.
PIN_VERSION_RE = re.compile(r'^\s*version\s*=\s*"(v?[\d.]+)"\s*$', re.MULTILINE)

# `shipyard --version` prints e.g. "shipyard 0.70.0".
VERSION_OUT_RE = re.compile(r"(\d+(?:\.\d+)*)")

# A Pulp CI job checks out under this path; a live Runner.Worker or build
# child under it means this host is mid-job.
ACTIONS_RUNNER_WORKSPACE_MARKER = "actions-runner/_work/pulp"

# Shipyard subcommands that mean "actively validating something". The
# persistent `daemon` is deliberately absent: it always runs, so treating
# it as busy would block every update forever.
BUSY_SUBCOMMANDS = frozenset({"ship", "pr", "run", "rescue", "auto-merge", "watch"})

# Shipyard flags that take a separate value token, so the value is not
# mistaken for the subcommand. `shipyard --mode shipyard daemon run` is
# the motivating case: naive parsing reads "shipyard" as the subcommand,
# and a naive substring match reads its trailing "run" as a busy ship.
VALUE_FLAGS = frozenset({"--mode", "--repo", "--base", "--to"})

BUILD_CHILD_TOKENS = ("cmake", "ctest", "ninja", "make", "clang", "swift")


class ConfigError(Exception):
    """The pin could not be read — a setup problem, not a runtime one."""


# ── Pin + installed version ─────────────────────────────────────────────────


def repo_root() -> Path:
    override = os.environ.get("PULP_SHIPYARD_AUTOUPDATE_REPO")
    if override:
        return Path(override)
    return Path(__file__).resolve().parent.parent.parent


def pin_ref() -> str:
    """Git ref to read the pin from; empty means "use the working tree".

    Defaults to `origin/main` because the authoritative pin is the
    project's, not whatever branch this checkout happens to be sitting on.
    A dev machine's canonical checkout is routinely parked on a feature
    branch, and a branch is free to carry an experimental pin — converging
    the whole machine onto that would be a bug. Deliberately does not
    fetch: a background agent doing network I/O to decide what to install
    is a worse failure mode than converging one tick later off the last
    fetch (and a CI host fetches constantly anyway).
    """
    return os.environ.get("PULP_SHIPYARD_AUTOUPDATE_PIN_REF", "origin/main").strip()


def _parse_pin(text: str, source: str) -> str:
    m = PIN_VERSION_RE.search(text)
    if not m:
        raise ConfigError(f'could not parse `version = "..."` from {source}')
    return m.group(1).lstrip("v")


def _pin_from_ref(root: Path, ref: str) -> Optional[str]:
    """The pin as of `ref`, or None if git can't answer (not a checkout,
    ref never fetched, git absent). None means "fall back", never "fail"."""
    try:
        p = subprocess.run(
            ["git", "-C", str(root), "show", f"{ref}:tools/shipyard.toml"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (subprocess.SubprocessError, OSError):
        return None
    if p.returncode != 0 or not p.stdout.strip():
        return None
    try:
        return _parse_pin(p.stdout, f"{ref}:tools/shipyard.toml")
    except ConfigError:
        return None


def read_pin(root: Optional[Path] = None) -> str:
    """The pinned version, normalized without the leading `v`."""
    root = root if root is not None else repo_root()
    ref = pin_ref()
    if ref and ref.lower() != "worktree":
        from_ref = _pin_from_ref(root, ref)
        if from_ref is not None:
            return from_ref
    pin_file = root / "tools" / "shipyard.toml"
    if not pin_file.exists():
        raise ConfigError(f"pin file not found: {pin_file}")
    return _parse_pin(pin_file.read_text(encoding="utf-8"), str(pin_file))


def shipyard_bin() -> str:
    return os.environ.get("PULP_SHIPYARD_BIN", "shipyard")


def installed_version() -> Optional[str]:
    """Installed Shipyard version, or None if absent/unreadable.

    None is the fail-closed answer: the caller declines to update rather
    than guessing a direction from an unknown version.
    """
    try:
        out = subprocess.run(
            [shipyard_bin(), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    m = VERSION_OUT_RE.search(out or "")
    return m.group(1) if m else None


def version_tuple(v: str) -> tuple[int, ...]:
    return tuple(int(p) for p in v.lstrip("v").split(".") if p.isdigit())


def compare(installed: str, pin: str) -> int:
    """-1 installed is behind the pin, 0 equal, 1 ahead."""
    a, b = version_tuple(installed), version_tuple(pin)
    # Zero-pad so 0.70 vs 0.70.0 compares equal rather than behind.
    width = max(len(a), len(b))
    a += (0,) * (width - len(a))
    b += (0,) * (width - len(b))
    return (a > b) - (a < b)


# ── Kill switch ─────────────────────────────────────────────────────────────


def config_path() -> Path:
    override = os.environ.get("PULP_SHIPYARD_AUTOUPDATE_CONFIG")
    if override:
        return Path(override)
    return Path.home() / ".config" / "pulp" / "shipyard-autoupdate"


OFF_VALUES = frozenset({"0", "off", "no", "false", "disabled"})


def disabled_reason() -> Optional[str]:
    """Why auto-update is off, or None if it is on.

    Env wins over the file so a one-off shell run can override the
    machine's persistent setting in either direction.
    """
    env = os.environ.get("PULP_SHIPYARD_AUTOUPDATE")
    if env is not None:
        if env.strip().lower() in OFF_VALUES:
            return "disabled by PULP_SHIPYARD_AUTOUPDATE"
        return None
    path = config_path()
    try:
        if path.exists():
            body = path.read_text(encoding="utf-8").strip().lower()
            # Ignore comment/blank lines so the file can explain itself.
            for line in body.splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line in OFF_VALUES:
                    return f"disabled by {path}"
                break
    except OSError:
        # Unreadable config is not a reason to start updating.
        return f"disabled: could not read {path}"
    return None


# ── Idle gate ───────────────────────────────────────────────────────────────


def _ps_lines() -> Optional[list[str]]:
    ps = os.environ.get("PULP_SHIPYARD_AUTOUPDATE_PS")
    cmd = [ps] if ps else ["ps", "-axww", "-o", "command="]
    try:
        out = subprocess.run(
            cmd, check=True, capture_output=True, text=True, timeout=15
        ).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    return out.splitlines()


def _shipyard_subcommand(tokens: list[str]) -> Optional[str]:
    """The subcommand of a `shipyard ...` command line, or None.

    Skips global flags and their values, so `shipyard --mode shipyard
    daemon run` yields "daemon" rather than "shipyard" or "run".
    """
    for i, tok in enumerate(tokens):
        if os.path.basename(tok) != "shipyard":
            continue
        j = i + 1
        while j < len(tokens):
            t = tokens[j]
            if t in VALUE_FLAGS:
                j += 2
                continue
            if t.startswith("-"):
                j += 1
                continue
            return t
        return None
    return None


def is_busy() -> Optional[bool]:
    """True if this host is mid-job, False if idle, None if unknowable.

    None (probe failure) is NOT idle — the caller must not update on it.
    """
    lines = _ps_lines()
    if lines is None:
        return None
    for line in lines:
        if ACTIONS_RUNNER_WORKSPACE_MARKER in line:
            if any(tok in line for tok in BUILD_CHILD_TOKENS):
                return True
            if "Runner.Worker" in line and "spawnclient" in line:
                return True
        sub = _shipyard_subcommand(line.split())
        if sub in BUSY_SUBCOMMANDS:
            return True
    return False


# ── Convergence ─────────────────────────────────────────────────────────────


class _Lock:
    """Whole-machine mutex around the install step.

    launchd will not run two copies of one label, but a hand-run
    converger and a tick can still overlap, and two installers writing
    ~/.local/bin/shipyard at once is precisely the half-installed binary
    this must never produce. Non-blocking: whoever holds it is already
    doing the work, so the loser skips rather than queues.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self._fh = None

    def __enter__(self) -> bool:
        import fcntl

        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._fh = self.path.open("w")
            fcntl.flock(self._fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            if self._fh:
                self._fh.close()
                self._fh = None
            return False

    def __exit__(self, *exc: object) -> None:
        if self._fh:
            import fcntl

            try:
                fcntl.flock(self._fh, fcntl.LOCK_UN)
            except OSError:
                pass
            self._fh.close()


def _run(cmd: list[str], cwd: Optional[Path] = None) -> tuple[int, str]:
    try:
        p = subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=600,
        )
    except (subprocess.SubprocessError, OSError) as exc:
        return 1, f"{exc}"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def apply_convergence(installed: str, pin: str, root: Path) -> tuple[bool, str]:
    """Move the installed Shipyard onto the pin. Returns (ok, detail).

    Direction matters: `shipyard update` will not go backwards, so the
    ahead-of-pin case must go through the unconditional pinned installer.
    """
    direction = compare(installed, pin)
    if direction < 0:
        cmd = [shipyard_bin(), "update", "--to", f"v{pin}"]
        path_label = "shipyard update --to"
    else:
        installer = root / "tools" / "install-shipyard.sh"
        if not installer.exists():
            return False, f"installer not found: {installer}"
        cmd = ["bash", str(installer)]
        path_label = "install-shipyard.sh (downgrade: shipyard update refuses)"

    code, output = _run(cmd, cwd=root)
    if code != 0:
        return False, f"{path_label} failed (exit {code}): {output.strip()[-500:]}"

    # Verify rather than trust. A checksum mismatch, a partial write, or a
    # silently-declined update all land here as a version that still is
    # not the pin.
    now = installed_version()
    if now is None:
        return False, f"{path_label} ran but `shipyard --version` is unreadable"
    if compare(now, pin) != 0:
        return False, f"{path_label} ran but version is {now}, expected {pin}"
    return True, f"{installed} → {now} via {path_label}"


# ── State publication ───────────────────────────────────────────────────────


def state_dir() -> Path:
    override = os.environ.get("PULP_SHIPYARD_AUTOUPDATE_STATE_DIR")
    return Path(override) if override else Path.home() / ".local" / "state" / "pulp"


def publish(decision: dict) -> None:
    """Atomically publish the last decision. Never fatal: observability
    must not be able to break the updater."""
    try:
        d = state_dir()
        d.mkdir(parents=True, exist_ok=True)
        tmp = d / "shipyard_autoupdate.json.tmp"
        tmp.write_text(json.dumps(decision, indent=2, sort_keys=True) + "\n")
        tmp.replace(d / "shipyard_autoupdate.json")
    except OSError:
        pass


# ── Entry point ─────────────────────────────────────────────────────────────


def run(check_only: bool = False) -> tuple[int, dict]:
    decision: dict = {
        "schema_version": 1,
        "checked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }

    try:
        root = repo_root()
        pin = read_pin(root)
    except ConfigError as exc:
        decision.update(action="config-error", detail=str(exc))
        return 2, decision
    decision["pin"] = pin

    reason = disabled_reason()
    if reason:
        decision.update(action="disabled", detail=reason)
        return 0, decision

    inst = installed_version()
    decision["installed"] = inst
    if inst is None:
        # Not installed, or unreadable. Bootstrapping a machine from zero
        # is install-shipyard.sh's job, not a background agent's.
        decision.update(
            action="skipped",
            detail="shipyard not installed or version unreadable; "
            "run tools/install-shipyard.sh once to bootstrap",
        )
        return 0, decision

    if compare(inst, pin) == 0:
        decision.update(action="at-pin", detail=f"installed {inst} == pin {pin}")
        return 0, decision

    drift = "behind" if compare(inst, pin) < 0 else "ahead of"
    decision["drift"] = drift

    if check_only:
        decision.update(
            action="drift-detected", detail=f"installed {inst} is {drift} pin {pin}"
        )
        return 0, decision

    busy = is_busy()
    if busy is None:
        decision.update(action="deferred", detail="busy probe failed; not updating")
        return 0, decision
    if busy:
        decision.update(
            action="deferred", detail="host is mid-job; will converge when idle"
        )
        return 0, decision

    with _Lock(state_dir() / "shipyard_autoupdate.lock") as acquired:
        if not acquired:
            decision.update(
                action="deferred", detail="another converger holds the lock"
            )
            return 0, decision
        ok, detail = apply_convergence(inst, pin, root)
    decision.update(action="converged" if ok else "failed", detail=detail)
    return (0 if ok else 1), decision


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Converge installed Shipyard onto the tools/shipyard.toml pin."
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="report drift without installing anything",
    )
    ap.add_argument("--json", action="store_true", help="emit the decision as JSON")
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="print the decision even when already at the pin",
    )
    args = ap.parse_args(argv)

    code, decision = run(check_only=args.check)
    publish(decision)

    if args.json:
        print(json.dumps(decision, indent=2, sort_keys=True))
        return code

    action = decision.get("action")
    # Zero nag: the common steady state (already at the pin) says nothing
    # unless asked. Real events and failures always speak.
    if action in ("at-pin", "disabled") and not args.verbose:
        return code
    stream = sys.stderr if code != 0 else sys.stdout
    print(f"shipyard-autoupdate: {action}: {decision.get('detail', '')}", file=stream)
    return code


if __name__ == "__main__":
    sys.exit(main())
