#!/usr/bin/env python3
"""Probe engine for the fleet convergence manifest.

The whole correctness property of this module is the THREE-VALUED result. Every
probe returns exactly one of:

    OK     — observed the value, and it matches what is declared.
    DRIFT  — observed the value, and it does NOT match.
    UNOBS  — could not observe the value at all.

UNOBS is never folded into OK. A probe that raises, times out, finds no
candidate to read, or cannot reach its subject is UNOBS. "I could not check
this" and "this is fine" are different answers, and collapsing them is how a
convergence system silently stops converging while reporting green.

The inverse mistake is just as bad and is guarded here too: a missing directory
is DRIFT (we successfully observed that it is absent), not UNOBS. Absence is an
observation when the parent is readable.
"""

from __future__ import annotations

import glob
import os
import plistlib
import shutil
import subprocess
import sys

try:
    import tomllib
except ImportError:  # stdlib only from 3.11; macOS ships 3.9
    # `tomli` IS `tomllib` -- the stdlib module was adopted from it, same API.
    # Falling back keeps a host on stock system python usable without putting a
    # package manager on it: `pip install --user tomli` is one pure-python file
    # tree in the user's site-packages, versus Homebrew's /opt/homebrew.
    #
    # This widens which hosts can run; it does NOT weaken the refusal. If
    # neither module is importable the callers still exit 3 and emit no verdict,
    # because a parser that cannot load the manifest has not observed anything.
    import tomli as tomllib  # noqa: F401

OK = "ok"
DRIFT = "drift"
UNOBS = "unobservable"


class Probe:
    """Result of observing one declared key."""

    def __init__(self, state, observed=None, detail=""):
        self.state = state
        self.observed = observed
        self.detail = detail


def expand(path):
    return os.path.expanduser(os.path.expandvars(path))


def load_manifest(path):
    with open(path, "rb") as fh:
        data = tomllib.load(fh)
    if "epoch" not in data or not isinstance(data["epoch"], int):
        raise ValueError(f"{path}: missing or non-integer top-level `epoch`")
    if "hosts" not in data:
        raise ValueError(f"{path}: no [hosts] table")
    return data


def identify_host(manifest, override=None):
    """Map this machine to a manifest host id, or None if it is not in the fleet."""
    if override:
        if override not in manifest["hosts"]:
            raise ValueError(f"--host {override} is not declared in the manifest")
        return override
    try:
        short = subprocess.run(
            ["hostname", "-s"], capture_output=True, text=True, timeout=10, check=True
        ).stdout.strip()
    except Exception:
        return None
    for host_id, host in manifest["hosts"].items():
        if short in host.get("hostnames", []):
            return host_id
    return None


# --------------------------------------------------------------------------
# Probes. Each returns a Probe. Each must be able to answer UNOBS.
# --------------------------------------------------------------------------

def mount_gate(key):
    """Return an UNOBS Probe if the key's declared mount dependency is absent.

    A path can be absent for two completely different reasons, and conflating
    them breaks convergence in both directions:

      * It has not been created yet. That is an OBSERVATION -> DRIFT, and
        apply.sh can fix it with mkdir -p.
      * The volume it lives on is not mounted (or is TCC-blocked). We know
        nothing about the declared state -> UNOBS, and creating the path would
        write to the mount point on the boot disk, silently shadowing the real
        volume when it comes back.

    Nothing about the path string distinguishes those, so the manifest declares
    the dependency explicitly with `requires_mount`. Guessing from the path (an
    earlier version treated any missing parent as unobservable) makes ordinary
    nested directories permanently un-appliable.
    """
    mount = key.get("requires_mount")
    if not mount:
        return None
    mount = expand(mount)
    if not os.path.isdir(mount):
        return Probe(UNOBS, None, f"required mount {mount} is not present")
    if not os.access(mount, os.R_OK | os.X_OK):
        return Probe(UNOBS, None, f"required mount {mount} is not readable")
    return None


MOUNT_ROOTS = ("/Volumes", "/mnt", "/media")


def implicit_mount_guard(key, path):
    """Fail closed when a path lives on a volume the key forgot to declare.

    A path under /Volumes/<name> whose <name> is absent means the volume is not
    mounted. Without this guard the generic rule sees a readable /Volumes, calls
    the absence DRIFT, and apply.sh cheerfully mkdir -p's the whole chain --
    creating a directory ON THE BOOT DISK at the mount point, which then shadows
    the real volume when it is plugged back in and silently splits the fleet's
    VM storage in two.

    So: if the key did not declare `requires_mount`, we refuse to guess. UNOBS
    with an instruction is the right answer -- it is honest about not knowing,
    and it tells the manifest author exactly what to add.
    """
    if key.get("requires_mount"):
        return None  # already gated explicitly
    parts = os.path.normpath(path).split(os.sep)
    if len(parts) < 3:
        return None
    root = os.sep + parts[1]
    if root not in MOUNT_ROOTS:
        return None
    volume = os.sep.join([root, parts[2]])
    if os.path.isdir(volume):
        return None
    return Probe(UNOBS, None,
                 f"{volume} is not mounted and the key does not declare "
                 f"requires_mount = \"{volume}\" -- refusing to treat this as "
                 f"drift, because applying it would create {volume} on the boot disk")


def _nearest_existing_ancestor(path):
    cur = os.path.dirname(path.rstrip("/")) or "/"
    while True:
        if os.path.exists(cur):
            return cur
        parent = os.path.dirname(cur) or "/"
        if parent == cur:
            return cur
        cur = parent


def probe_dir(key):
    gate = mount_gate(key)
    if gate:
        return gate
    path = expand(key["value"])
    if os.path.isdir(path):
        return Probe(OK, path)
    guard = implicit_mount_guard(key, path)
    if guard:
        return guard
    if os.path.exists(path):
        return Probe(DRIFT, "not-a-directory", f"{path} exists but is not a directory")
    # Absence is an observation only if we can actually see the region of the
    # filesystem it would live in.
    anc = _nearest_existing_ancestor(path)
    if not os.access(anc, os.R_OK | os.X_OK):
        return Probe(UNOBS, None, f"nearest existing ancestor {anc} is not readable")
    return Probe(DRIFT, "absent", f"{path} does not exist (would be created under {anc})")


def probe_file_content(key):
    gate = mount_gate(key)
    if gate:
        return gate
    path = expand(key.get("path", ""))
    if not path:
        return Probe(UNOBS, None, "key declares no `path`")
    if not os.path.exists(path):
        guard = implicit_mount_guard(key, path)
        if guard:
            return guard
        anc = _nearest_existing_ancestor(path)
        if not os.access(anc, os.R_OK | os.X_OK):
            return Probe(UNOBS, None, f"nearest existing ancestor {anc} is not readable")
        return Probe(DRIFT, "absent", f"{path} does not exist")
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            got = fh.read().strip()
    except OSError as exc:
        return Probe(UNOBS, None, f"unreadable: {exc}")
    if got == str(key["value"]).strip():
        return Probe(OK, got)
    return Probe(DRIFT, got)


def probe_disk_floor_gb(key):
    path = expand(key.get("path", "/"))
    if not os.path.isdir(path):
        return Probe(UNOBS, None, f"{path} not mounted or not present")
    try:
        st = os.statvfs(path)
    except OSError as exc:
        return Probe(UNOBS, None, f"statvfs failed: {exc}")
    free_gb = (st.f_bavail * st.f_frsize) / (1024 ** 3)
    observed = f"{free_gb:.1f}GB"
    if free_gb >= float(key["value"]):
        return Probe(OK, observed)
    return Probe(DRIFT, observed, f"below floor of {key['value']}GB")


def probe_launchd_env(key):
    pattern = expand(key.get("plist_glob", ""))
    env_name = key.get("env_name")
    if not pattern or not env_name:
        return Probe(UNOBS, None, "key declares no `plist_glob` / `env_name`")
    matches = sorted(glob.glob(pattern))
    if not matches:
        # No plists is genuinely unknown: the agents may simply not be installed
        # on this host yet. That is not the same as "installed and wrong".
        return Probe(UNOBS, None, f"no plists match {pattern}")
    values, unreadable = {}, []
    for plist in matches:
        try:
            with open(plist, "rb") as fh:
                data = plistlib.load(fh)
        except Exception as exc:  # malformed / binary / permission
            unreadable.append(f"{os.path.basename(plist)}({type(exc).__name__})")
            continue
        env = data.get("EnvironmentVariables") or {}
        if env_name in env:
            values.setdefault(str(env[env_name]), []).append(os.path.basename(plist))
    if not values:
        if unreadable:
            return Probe(UNOBS, None, f"no readable plist declared {env_name}; unreadable: {','.join(unreadable)}")
        return Probe(UNOBS, None, f"{env_name} not set in any matching plist")
    want = str(key["value"])
    if set(values) == {want}:
        detail = f"{len(matches)} plists agree"
        if unreadable:
            # Partial read: some agreed, some could not be read at all. We do
            # NOT get to call that compliant.
            return Probe(UNOBS, want, f"readable plists agree but unreadable: {','.join(unreadable)}")
        return Probe(OK, want, detail)
    bad = {v: p for v, p in values.items() if v != want}
    return Probe(DRIFT, ";".join(f"{v}={','.join(p)}" for v, p in bad.items()))


def _gh_variable(repo, name):
    """Return (found, value) or raise for unobservable."""
    exe = shutil.which("ghapp")
    if not exe:
        raise RuntimeError("ghapp not on PATH (never fall back to plain gh)")
    proc = subprocess.run(
        [exe, "api", f"repos/{repo}/actions/variables/{name}", "-q", ".value"],
        capture_output=True, text=True, timeout=60,
    )
    if proc.returncode == 0:
        return True, proc.stdout.strip()
    combined = (proc.stderr + proc.stdout).lower()
    if "not found" in combined or "http 404" in combined:
        return False, None
    raise RuntimeError((proc.stderr.strip() or "ghapp failed")[:200])


def probe_github_var(key):
    try:
        found, value = _gh_variable(key["repo"], key["name"])
    except Exception as exc:
        return Probe(UNOBS, None, str(exc))
    if not found:
        return Probe(DRIFT, "unset", "variable is not set")
    if value == str(key["value"]):
        return Probe(OK, value)
    return Probe(DRIFT, value)


def probe_github_var_unset(key):
    """Declared MUST-BE-ABSENT. Present with any value is drift."""
    try:
        found, value = _gh_variable(key["repo"], key["name"])
    except Exception as exc:
        return Probe(UNOBS, None, str(exc))
    if not found:
        return Probe(OK, "unset")
    return Probe(DRIFT, value or "set", "must remain unset")


PROBES = {
    "dir": probe_dir,
    "file_content": probe_file_content,
    "disk_floor_gb": probe_disk_floor_gb,
    "launchd_env": probe_launchd_env,
    "github_var": probe_github_var,
    "github_var_unset": probe_github_var_unset,
}


def probe(key):
    fn = PROBES.get(key.get("kind"))
    if fn is None:
        # An unknown kind is UNOBS, never OK. A manifest written by a newer
        # agent than this script must degrade to "I cannot check that", so an
        # old checkout can never report a new key as compliant.
        return Probe(UNOBS, None, f"unknown kind {key.get('kind')!r}")
    try:
        return fn(key)
    except Exception as exc:
        return Probe(UNOBS, None, f"{type(exc).__name__}: {exc}")


# --------------------------------------------------------------------------
# Apply actions. Only `mkdir` and `write` ever mutate; everything else is
# reported `manual` and escalated rather than silently edited.
# --------------------------------------------------------------------------

def apply_key(key, pr):
    """Return (outcome, detail). Outcome: ok | fixed | manual | failed | unobservable."""
    if pr.state == OK:
        return "ok", "already compliant"
    if pr.state == UNOBS:
        return "unobservable", pr.detail
    action = key.get("apply", "none")
    if action == "none":
        return "manual", f"drifted (observed {pr.observed}); apply=none, needs a human-free escalation"
    try:
        if action == "mkdir":
            os.makedirs(expand(key["value"]), exist_ok=True)
        elif action == "write":
            path = expand(key["path"])
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(str(key["value"]).strip() + "\n")
        else:
            return "manual", f"unknown apply action {action!r}"
    except Exception as exc:
        return "failed", f"{type(exc).__name__}: {exc}"
    after = probe(key)
    if after.state == OK:
        return "fixed", f"was {pr.observed}"
    return "failed", f"still {after.state} after apply ({after.detail})"


def keys_for(manifest, host_id, include_routing=True):
    out = list(manifest["hosts"][host_id].get("keys", []))
    if include_routing:
        out += list(manifest.get("routing", {}).get("keys", []))
    return out


def die(msg, code=3):
    print(f"fleet: {msg}", file=sys.stderr)
    raise SystemExit(code)
