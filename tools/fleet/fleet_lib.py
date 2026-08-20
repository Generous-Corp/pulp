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
import hashlib
import json
import os
import posixpath
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

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


RUNNER_POLICY_ENV = ("RUSTUP_HOME", "CARGO_HOME")
SYSTEM_PATH_PREFIX = "/usr/bin:/bin:/usr/sbin:/sbin"
PINNED_RUSTUP_VERSION = "1.29.0"
PINNED_RUSTUP_SHA256 = "aeb4105778ca1bd3c6b0e75768f581c656633cd51368fa61289b6a71696ac7e1"


def _runner_dirs(key):
    matches = []
    for pattern in key.get("runner_globs", []):
        for raw in glob.glob(expand(pattern)):
            path = Path(raw)
            if path.is_dir() and (path / ".runner").is_file():
                matches.append(path)
    return sorted(set(matches))


def _runner_glob_observation_error(key):
    """Return why runner discovery cannot be trusted, or None when observable."""
    for raw_pattern in key.get("runner_globs", []):
        pattern = expand(raw_pattern)
        magic = [pos for char in "*?[" if (pos := pattern.find(char)) >= 0]
        literal = pattern[:min(magic)] if magic else pattern
        if magic and literal.endswith(os.sep):
            parent = Path(literal)
        else:
            parent = Path(literal).parent if magic else Path(literal)
        candidate = parent
        while not candidate.exists() and candidate != candidate.parent:
            candidate = candidate.parent
        if not candidate.is_dir() or not os.access(candidate, os.R_OK | os.X_OK):
            return f"runner glob {raw_pattern!r} is not observable at {candidate}"
    return None


def _runner_private_paths(runner_dir):
    toolcache = runner_dir / "_toolcache"
    return toolcache / "rustup", toolcache / "cargo"


def _runner_path_value(runner_dir):
    _, cargo_home = _runner_private_paths(runner_dir)
    return ":".join([
        SYSTEM_PATH_PREFIX,
        str(cargo_home / "bin"),
        "/opt/homebrew/bin",
        "/opt/homebrew/sbin",
        "/usr/local/bin",
        str(Path.home() / ".local/bin"),
    ])


def _runner_private_env(runner_dir):
    rustup_home, cargo_home = _runner_private_paths(runner_dir)
    env = os.environ.copy()
    env.update({
        "RUSTUP_HOME": str(rustup_home),
        "CARGO_HOME": str(cargo_home),
        "PATH": _runner_path_value(runner_dir),
    })
    return env


def _sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as fh:
        for chunk in iter(lambda: fh.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_env_file(path):
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            name, value = line.split("=", 1)
            values[name] = value
    return values


def _runner_version(runner_dir):
    listener = runner_dir / "bin" / "Runner.Listener"
    if not listener.is_file():
        return None
    proc = subprocess.run(
        [str(listener), "--version"], capture_output=True, text=True, timeout=15
    )
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def _is_runner_private(path, runner_dir):
    if path.is_symlink() or not path.is_dir():
        return False
    try:
        resolved = path.resolve(strict=True)
        root = runner_dir.resolve(strict=True)
        resolved.relative_to(root)
        return resolved.stat().st_dev == root.stat().st_dev
    except (OSError, ValueError):
        return False


def _is_internal_apfs(path):
    try:
        df = subprocess.run(
            ["df", "-P", str(path)], capture_output=True, text=True,
            timeout=15, check=True,
        )
        lines = [line for line in df.stdout.splitlines() if line.strip()]
        if len(lines) < 2:
            return False, "df returned no device"
        device = lines[-1].split()[0]
        info = subprocess.run(
            ["diskutil", "info", device], capture_output=True, text=True,
            timeout=15, check=True,
        ).stdout
    except Exception as exc:
        return None, f"storage probe failed: {exc}"
    apfs = "File System Personality:" in info and "APFS" in info
    internal = "Device Location:" in info and "Internal" in info
    fixed = "Removable Media:" in info and "Fixed" in info
    return apfs and internal and fixed, f"device={device} apfs={apfs} internal={internal} fixed={fixed}"


def _runner_policy_findings(key):
    findings, unobservable = [], []
    dirs = _runner_dirs(key)
    for runner_dir in dirs:
        label = str(runner_dir)
        try:
            config = json.loads((runner_dir / ".runner").read_text(encoding="utf-8-sig"))
        except Exception as exc:
            unobservable.append(f"{label}: unreadable .runner ({exc})")
            continue
        if config.get("disableUpdate") is not True:
            findings.append(f"{label}: disableUpdate is not true")
        listener = _runner_process_present(runner_dir, "Runner.Listener")
        if listener is None:
            unobservable.append(f"{label}: cannot inspect Runner.Listener process")
        elif not listener:
            findings.append(f"{label}: Runner.Listener is offline")
        got_version = _runner_version(runner_dir)
        if got_version != str(key["runner_version"]):
            findings.append(
                f"{label}: runner version {got_version or 'unreadable'} != {key['runner_version']}"
            )

        rustup_home, cargo_home = _runner_private_paths(runner_dir)
        expected_env = {"RUSTUP_HOME": str(rustup_home), "CARGO_HOME": str(cargo_home)}
        env = _read_env_file(runner_dir / ".env")
        for name, want in expected_env.items():
            if env.get(name) != want:
                findings.append(f"{label}: {name}={env.get(name)!r}, expected {want!r}")
        expected_path = _runner_path_value(runner_dir)
        try:
            got_path = (runner_dir / ".path").read_text(encoding="utf-8").strip()
        except OSError:
            got_path = None
        if got_path != expected_path:
            findings.append(f"{label}: .path is not the system-first policy value")
        for name, path in (("RUSTUP_HOME", rustup_home), ("CARGO_HOME", cargo_home)):
            if not _is_runner_private(path, runner_dir):
                findings.append(f"{label}: {name} is absent, symlinked, or outside runner-local storage")
        private_env = _runner_private_env(runner_dir)
        tools = (
            ("cargo", cargo_home / "bin" / "cargo", ["--version"]),
            ("rustup toolchain", cargo_home / "bin" / "rustup", ["show", "active-toolchain"]),
        )
        for name, binary, args in tools:
            try:
                proc = subprocess.run(
                    [str(binary), *args], capture_output=True, text=True,
                    timeout=15, env=private_env,
                )
            except Exception:
                proc = None
            if proc is None or proc.returncode != 0:
                findings.append(f"{label}: private {name} is not runnable")
        if key.get("require_internal_apfs", True):
            storage_ok, detail = _is_internal_apfs(runner_dir)
            if storage_ok is None:
                unobservable.append(f"{label}: {detail}")
            elif not storage_ok:
                findings.append(f"{label}: runner storage is not internal APFS ({detail})")
    return dirs, findings, unobservable


def probe_actions_runner_policy(key):
    if not all(key.get(name) for name in ("runner_globs", "runner_version", "runner_sha256")):
        return Probe(
            UNOBS, None,
            "runner policy requires runner_globs, runner_version, and runner_sha256",
        )
    observation_error = _runner_glob_observation_error(key)
    if observation_error:
        return Probe(UNOBS, None, observation_error)
    dirs, findings, unobservable = _runner_policy_findings(key)
    observed = json.dumps({"runners": len(dirs), "findings": findings}, sort_keys=True)
    if unobservable:
        return Probe(UNOBS, observed, "; ".join(unobservable))
    if findings:
        return Probe(DRIFT, observed, "; ".join(findings))
    return Probe(OK, observed, f"{len(dirs)} configured runner(s) comply")


PROBES = {
    "dir": probe_dir,
    "file_content": probe_file_content,
    "disk_floor_gb": probe_disk_floor_gb,
    "launchd_env": probe_launchd_env,
    "github_var": probe_github_var,
    "github_var_unset": probe_github_var_unset,
    "actions_runner_policy": probe_actions_runner_policy,
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
# Apply actions. Runner hardening is idle-gated and limited to runner-local
# configuration/toolchain state. Other unknown changes are escalated.
# --------------------------------------------------------------------------

def _runner_process_present(runner_dir, process_name):
    try:
        proc = subprocess.run(
            ["ps", "ax", "-o", "command="], capture_output=True, text=True,
            timeout=15, check=True,
        )
    except Exception:
        return None
    needle = str(runner_dir / "bin" / process_name)
    return any(needle in line for line in proc.stdout.splitlines())


def _wait_runner_process(runner_dir, process_name, expected, timeout=30):
    """Bounded service-state convergence; never retries a workflow/job."""
    deadline = time.monotonic() + timeout
    while True:
        observed = _runner_process_present(runner_dir, process_name)
        if observed is None:
            return None
        if observed is expected:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.5)


def _run_checked(argv, *, cwd=None, env=None, timeout=600):
    proc = subprocess.run(
        [str(x) for x in argv], cwd=str(cwd) if cwd else None, env=env,
        capture_output=True, text=True, timeout=timeout,
    )
    if proc.returncode != 0:
        detail = (proc.stderr.strip() or proc.stdout.strip() or "no output")[-500:]
        raise RuntimeError(f"{' '.join(str(x) for x in argv[:2])} exited {proc.returncode}: {detail}")
    return proc


def _atomic_write(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _merge_runner_env(runner_dir):
    path = runner_dir / ".env"
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines() if path.exists() else []
    rustup_home, cargo_home = _runner_private_paths(runner_dir)
    wanted = {"RUSTUP_HOME": str(rustup_home), "CARGO_HOME": str(cargo_home)}
    out, seen = [], set()
    for line in lines:
        if "=" not in line or line.lstrip().startswith("#"):
            out.append(line)
            continue
        name = line.split("=", 1)[0]
        if name in wanted:
            if name not in seen:
                out.append(f"{name}={wanted[name]}")
                seen.add(name)
        else:
            out.append(line)
    for name in RUNNER_POLICY_ENV:
        if name not in seen:
            out.append(f"{name}={wanted[name]}")
    _atomic_write(path, "\n".join(out) + "\n")


def _archive_link_escapes(member):
    """Return whether a tar link resolves outside its extraction root."""
    base = posixpath.dirname(member.name) if member.issym() else ""
    target = posixpath.normpath(posixpath.join(base, member.linkname))
    return (
        posixpath.isabs(member.linkname)
        or target == ".."
        or target.startswith("../")
    )


def _install_runner_package(runner_dir, version, expected_sha256):
    name = f"actions-runner-osx-arm64-{version}.tar.gz"
    url = f"https://github.com/actions/runner/releases/download/v{version}/{name}"
    with tempfile.TemporaryDirectory(prefix="runner-pin-", dir=str(runner_dir.parent)) as td:
        archive = Path(td) / name
        _run_checked([
            "/usr/bin/curl", "--proto", "=https", "--tlsv1.2", "-fsSL",
            url, "-o", archive,
        ], timeout=300)
        actual_sha256 = _sha256_file(archive)
        if actual_sha256 != expected_sha256:
            raise RuntimeError(
                f"runner archive sha256 {actual_sha256} != {expected_sha256}"
            )
        with tarfile.open(archive, "r:gz") as tf:
            for member in tf.getmembers():
                member_path = Path(member.name)
                if member_path.is_absolute() or ".." in member_path.parts:
                    raise RuntimeError(f"runner archive contains unsafe path {member.name!r}")
                if member.issym() or member.islnk():
                    # A symlink target is relative to the link's containing
                    # directory; a tar hardlink target is archive-root-relative.
                    # The runner archive legitimately uses targets such as
                    # ../lib/node_modules/... that normalize inside the root.
                    # Reject only targets whose normalized archive path escapes.
                    if _archive_link_escapes(member):
                        raise RuntimeError(
                            f"runner archive contains unsafe link {member.linkname!r}"
                        )
            tf.extractall(runner_dir)


def _ensure_runner_rust(runner_dir):
    rustup_home, cargo_home = _runner_private_paths(runner_dir)
    rustup_home.mkdir(parents=True, exist_ok=True)
    cargo_home.mkdir(parents=True, exist_ok=True)
    env = _runner_private_env(runner_dir)

    def ready():
        probes = (
            (cargo_home / "bin" / "cargo", ["--version"]),
            (cargo_home / "bin" / "rustup", ["show", "active-toolchain"]),
        )
        try:
            return all(
                subprocess.run(
                    [str(binary), *args], capture_output=True, text=True,
                    timeout=15, env=env,
                ).returncode == 0
                for binary, args in probes
            )
        except Exception:
            return False

    if ready():
        return
    installer = runner_dir / "_toolcache" / "rustup-init"
    rustup_url = (
        "https://static.rust-lang.org/rustup/archive/"
        f"{PINNED_RUSTUP_VERSION}/aarch64-apple-darwin/rustup-init"
    )
    _run_checked([
        "/usr/bin/curl", "--proto", "=https", "--tlsv1.2", "-fsSL",
        rustup_url, "-o", installer,
    ], timeout=300)
    actual_sha256 = _sha256_file(installer)
    if actual_sha256 != PINNED_RUSTUP_SHA256:
        raise RuntimeError(
            f"rustup-init sha256 {actual_sha256} != {PINNED_RUSTUP_SHA256}"
        )
    installer.chmod(0o700)
    try:
        _run_checked([
            installer, "-y", "--no-modify-path", "--profile", "minimal",
            "--default-toolchain", "stable-aarch64-apple-darwin",
        ], env=env, timeout=900)
    finally:
        try:
            installer.unlink()
        except OSError:
            pass
    if not ready():
        raise RuntimeError("private cargo/rustup toolchain is not runnable after install")


def apply_actions_runner_policy(key, pr):
    dirs = _runner_dirs(key)
    for runner_dir in dirs:
        active = _runner_process_present(runner_dir, "Runner.Worker")
        if active is None:
            return "unobservable", f"cannot inspect processes before changing {runner_dir}"
        if active:
            return "manual", f"{runner_dir} has an active Runner.Worker; retry after it is idle"

    stopped = []
    try:
        for runner_dir in dirs:
            listener = _runner_process_present(runner_dir, "Runner.Listener")
            if listener is None:
                raise RuntimeError(f"cannot inspect listener process for {runner_dir}")
            if listener:
                _run_checked([runner_dir / "svc.sh", "stop"], cwd=runner_dir, timeout=120)
                stopped.append(runner_dir)
                stopped_cleanly = _wait_runner_process(
                    runner_dir, "Runner.Listener", False,
                )
                if stopped_cleanly is None:
                    raise RuntimeError(f"cannot verify listener stopped for {runner_dir}")
                if not stopped_cleanly:
                    raise RuntimeError(f"listener did not stop within 30s for {runner_dir}")

        # Close the assignment race: after listeners are stopped, no new job
        # can arrive, and every worker must still be absent before mutation.
        for runner_dir in dirs:
            active = _runner_process_present(runner_dir, "Runner.Worker")
            if active is None:
                raise RuntimeError(f"cannot re-check worker process for {runner_dir}")
            if active:
                for stopped_dir in stopped:
                    _run_checked([stopped_dir / "svc.sh", "start"], cwd=stopped_dir, timeout=120)
                return "manual", f"{runner_dir} acquired a Runner.Worker; retry after it is idle"

        for runner_dir in dirs:
            if _runner_version(runner_dir) != str(key["runner_version"]):
                _install_runner_package(
                    runner_dir, str(key["runner_version"]), str(key["runner_sha256"]),
                )
            # Stage the private toolchain before switching the runner's env or
            # PATH. A failed download then restarts against the old working
            # bootstrap instead of publishing a half-applied configuration.
            _ensure_runner_rust(runner_dir)
            config_path = runner_dir / ".runner"
            config = json.loads(config_path.read_text(encoding="utf-8-sig"))
            config["disableUpdate"] = True
            _atomic_write(config_path, json.dumps(config, indent=2, sort_keys=True) + "\n")
            _merge_runner_env(runner_dir)
            _atomic_write(runner_dir / ".path", _runner_path_value(runner_dir) + "\n")
    except Exception as exc:
        for runner_dir in stopped:
            try:
                _run_checked([runner_dir / "svc.sh", "start"], cwd=runner_dir, timeout=120)
            except Exception:
                pass
        return "failed", f"{type(exc).__name__}: {exc}"

    restart_failures = []
    for runner_dir in dirs:
        try:
            listener = _runner_process_present(runner_dir, "Runner.Listener")
            if listener is None:
                raise RuntimeError("cannot inspect listener after hardening")
            if not listener:
                _run_checked([runner_dir / "svc.sh", "start"], cwd=runner_dir, timeout=120)
                started = _wait_runner_process(runner_dir, "Runner.Listener", True)
                if started is None:
                    raise RuntimeError("cannot inspect listener after start")
                if not started:
                    raise RuntimeError("listener did not start within 30s")
        except Exception as exc:
            restart_failures.append(f"{runner_dir}: {exc}")
    if restart_failures:
        return "failed", "; ".join(restart_failures)
    after = probe_actions_runner_policy(key)
    if after.state == OK:
        return "fixed", f"hardened {len(dirs)} configured runner(s)"
    return "failed", f"still {after.state} after runner hardening ({after.detail})"

def apply_key(key, pr):
    """Return (outcome, detail). Outcome: ok | fixed | manual | failed | unobservable."""
    if pr.state == OK:
        return "ok", "already compliant"
    if pr.state == UNOBS:
        return "unobservable", pr.detail
    action = key.get("apply", "none")
    if action == "none":
        return "manual", f"drifted (observed {pr.observed}); apply=none, needs a human-free escalation"
    if action == "runner_harden" and key.get("kind") == "actions_runner_policy":
        return apply_actions_runner_policy(key, pr)
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
