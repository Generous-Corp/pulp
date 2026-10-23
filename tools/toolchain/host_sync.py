#!/usr/bin/env python3
"""Make this host match the toolchain pins on origin/main, additively.

    python3 tools/toolchain/host_sync.py                    # this checkout's origin/main
    python3 tools/toolchain/host_sync.py --repo ~/Code/pulp

check_env.py names every difference and the command that fixes it; this runs
those commands. It reads the pins from origin/main after a fetch, never from
the working tree, so a host stays on what landed whatever branch its checkout
has out. Then:

  1. `mise trust` of this checkout's own .mise.toml, then
     `MISE_ENV=toolchain <pinned mise> install --locked` in a copy of the
     pinned mise config: it installs only what is missing.
  2. `rustup toolchain install <rust pin>` with the components and targets
     rust-toolchain.toml names, when any of them is missing.
  3. check_env.py from the same ref, inside the pinned environment, written
     with every step's outcome to ~/.config/pulp/toolchain-sync.json.

It only adds. It never uninstalls, never runs `mise use`, never changes the
default rustup toolchain, a shell profile, Xcode or anything Homebrew owns. A
mise binary that is not the pinned one by digest is never run: installing it is
tartci's job. Those, and a rustc on PATH that is not rustup's, are reported and
left alone. A download or network failure is reported for the next run, never
counted as fixed. Runs on the stock python3 of a host (3.9 on a Mac) with the
standard library only.

Exit status:

  0  every pinned tool is installed and the check finds nothing
  1  a difference remains that this script does not fix
  2  a step failed on a download or network error; run again later
  3  the pins could not be read from the ref, so nothing was changed
  4  another run holds the lock, so nothing was changed
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
EXIT_OK = 0
EXIT_FINDINGS = 1
EXIT_RETRY = 2
EXIT_NO_PINS = 3
EXIT_BUSY = 4

RECEIPT = ".config/pulp/toolchain-sync.json"
LOCK = ".config/pulp/toolchain-sync.lock"
RUST_TOOLCHAIN = "experimental/pulp-rs/rust-toolchain.toml"
# Files read from the ref, and whether the sync can go on without each.
PINNED_FILES: Tuple[Tuple[str, bool], ...] = (
    ("tools/toolchain/expected.json", True),
    ("tools/toolchain/check_env.py", True),
    (".mise.toolchain.toml", True),
    ("mise.toolchain.lock", True),
    (".mise.toml", False),
    (RUST_TOOLCHAIN, False),
)
# Seconds. A pinned install downloads a few hundred MB on a cold host.
TIMEOUT = {"fetch": 300, "mise": 1800, "rustup": 1800, "check": 300, "probe": 10}
NETWORK = re.compile(
    r"could not resolve|resolve host|name or service not known|nodename nor servname|timed out|timeout"
    r"|connection (refused|reset|closed|aborted)|network is unreachable|temporary failure"
    r"|failed to (download|fetch)|error downloading|unable to access|ssl|tls handshake"
    r"|http (error|status)|\b(429|500|502|503|504)\b|rate limit|could not download",
    re.I,
)
HOMEBREW_PREFIXES = ("/opt/homebrew/", "/usr/local/Homebrew/", "/usr/local/Cellar/", "/home/linuxbrew/")
# Tools whose resolved path is worth a Homebrew note: the ones the manifest pins.
PINNED_COMMANDS = ("python3", "cmake", "ninja", "node", "clang-format", "tart", "mise", "rustc", "cargo")

Result = Tuple[int, str, str]
Runner = Callable[[List[str], Optional[Path], Dict[str, str], float], Result]


def run_cmd(argv: List[str], cwd: Optional[Path], env: Dict[str, str], timeout: float) -> Result:
    """(exit, stdout, stderr). A command that cannot start is 127; one that
    outlives its bound is 124 with "timed out" in stderr, which is a network
    failure for a download step.

    A timed-out child is killed and then given a few seconds, never waited on
    without a bound: a process that opened a /Volumes path without the
    Removable Volumes grant sits in the kernel where no signal reaches it, and
    an unbounded wait would hang this run with it.
    """
    try:
        proc = subprocess.Popen(
            argv, cwd=str(cwd) if cwd else None, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
        )
    except OSError as exc:
        return 127, "", f"cannot run {argv[0]}: {exc}"
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            pass  # abandoned: it cannot be reaped, and this run must not hang on it
        return 124, "", f"{argv[0]} timed out after {timeout:.0f}s"
    return proc.returncode, out or "", err or ""


def host_platform() -> str:
    """The mise lock platform name of this host, as check_env.py spells it."""
    system = {"darwin": "macos", "linux": "linux"}.get(sys.platform, sys.platform)
    if sys.platform.startswith("win"):
        system = "windows"
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64" if machine in ("x86_64", "amd64") else machine
    return f"{system}-{arch}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tail(text: str, limit: int = 1500) -> str:
    text = text.strip()
    return text if len(text) <= limit else "..." + text[-limit:]


class Step:
    """One step's outcome in the receipt."""

    def __init__(self, name: str, status: str, detail: str = "", argv: Optional[List[str]] = None,
                 network: bool = False) -> None:
        self.name = name
        self.status = status  # ok, failed, skipped
        self.detail = detail
        self.argv = argv or []
        self.network = network

    def as_dict(self) -> Dict[str, object]:
        return {"step": self.name, "status": self.status, "network_failure": self.network,
                "detail": self.detail, "argv": self.argv}


def ran(name: str, argv: List[str], result: Result) -> Step:
    code, out, err = result
    if code == 0:
        return Step(name, "ok", tail(out + err, 400), argv)
    text = out + "\n" + err
    return Step(name, "failed", f"exit {code}: {tail(text)}", argv, network=bool(NETWORK.search(text)))


@contextlib.contextmanager
def held_lock(path: Path) -> Iterator[bool]:
    """True while this process holds the lock, False when another run does."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(path, "a+")
    try:
        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            yield False
            return
        yield True
    finally:
        handle.close()


def read_pins(repo: Path, ref: str, dest: Path, env: Dict[str, str], run: Runner) -> Tuple[Optional[str], List[Step]]:
    """Fetch, then copy the pinned files at `ref` into `dest`.

    Returns the commit the files came from (None when they could not be read)
    and the steps. A failed fetch still reads the last fetched ref, so an
    offline host keeps converging on what it already knows about.
    """
    steps: List[Step] = []
    remote, _, branch = ref.partition("/")
    git_env = dict(env, GIT_TERMINAL_PROMPT="0")
    if branch:
        argv = ["git", "-C", str(repo), "fetch", "--quiet", remote, branch]
        step = ran("fetch", argv, run(argv, None, git_env, TIMEOUT["fetch"]))
        if step.status == "failed":
            step.network = True  # a fetch has nothing to fail on but the transport
        steps.append(step)
    code, out, err = run(["git", "-C", str(repo), "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
                         None, git_env, TIMEOUT["probe"])
    sha = out.strip()
    if code != 0 or not re.match(r"^[0-9a-f]{40}$", sha):
        steps.append(Step("export", "failed", f"{ref} does not name a commit in {repo}: {tail(err)}"))
        return None, steps
    missing: List[str] = []
    for rel, required in PINNED_FILES:
        code, out, err = run(["git", "-C", str(repo), "show", f"{sha}:{rel}"], None, git_env, TIMEOUT["probe"])
        if code != 0:
            if required:
                missing.append(rel)
            continue
        target = dest / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(out, encoding="utf-8")
    if missing:
        steps.append(Step("export", "failed", f"{sha} has no " + ", ".join(missing)))
        return None, steps
    steps.append(Step("export", "ok", f"pins from {ref} at {sha}"))
    return sha, steps


def pinned_mise(expected: dict, home: Path, plat: str) -> Tuple[Optional[Path], str]:
    """The pinned mise binary, or None and why it must not be run."""
    pin = expected["mise"]
    path = Path(pin["path"].replace("~", str(home), 1))
    digest = pin.get("sha256", {}).get(plat)
    if not path.is_file():
        return None, f"the pinned mise {pin['version']} is not installed at {path}; tartci's host installer installs it by digest"
    if digest is None:
        return None, f"no pinned mise digest for {plat}, so {path} cannot be trusted; report only"
    have = sha256_file(Path(os.path.realpath(path)))
    if have != digest:
        return None, (f"{path} has sha256 {have}, the pin is {digest}; never run, never replaced here "
                      "(tartci's host installer owns the binary)")
    return path, ""


def trust_checkout(mise: Path, repo: Path, env: Dict[str, str], run: Runner) -> Step:
    """Trust exactly this checkout's .mise.toml, as .githooks/post-checkout
    does for a new worktree. Never a parent directory: mise's trust paths are
    prefixes, and a .mise.toml can run code."""
    config = repo / ".mise.toml"
    if not config.is_file():
        return Step("trust", "skipped", f"{config} does not exist")
    argv = [str(mise), "trust", "--quiet", str(config)]
    step = ran("trust", argv, run(argv, repo, dict(env, MISE_DISABLE_UPDATE_WARNING="1"), TIMEOUT["probe"]))
    step.network = False  # trusting a file is local; a failure is a finding, not a retry
    return step


def mise_env(env: Dict[str, str], pins: Path) -> Dict[str, str]:
    """mise settings for a locked, install-only run against the copied pins.

    The global config is pointed at a file that does not exist so a host's own
    ~/.config/mise tools are neither installed nor refused by the lock, and the
    config search stops at the copy.
    """
    return dict(
        env,
        MISE_ENV="toolchain",
        MISE_YES="1",
        MISE_TRUSTED_CONFIG_PATHS=str(pins),
        MISE_LOCKED="1",
        MISE_AUTO_INSTALL="0",
        MISE_DISABLE_UPDATE_WARNING="1",
        MISE_GLOBAL_CONFIG_FILE=str(pins / ".no-global-mise-config.toml"),
        MISE_CEILING_PATHS=str(pins.parent),
    )


def cargo_bin(env: Dict[str, str], home: Path) -> str:
    return os.path.join(env.get("CARGO_HOME") or str(home / ".cargo"), "bin")


def find_rustup(env: Dict[str, str], home: Path) -> Optional[str]:
    return shutil.which("rustup", path=os.pathsep.join([cargo_bin(env, home), env.get("PATH", "")]))


def rust_missing(rustup: str, expected: dict, env: Dict[str, str], run: Runner) -> List[str]:
    """Components and targets the pinned toolchain lacks; all of them when the
    toolchain itself is not installed."""
    pin = expected["tools"]["rust"]
    components = list(expected.get("rust_components", []))
    targets = list(expected.get("rust_targets", []))
    probe_env = dict(env, RUSTUP_AUTO_INSTALL="0")
    code, out, _ = run([rustup, "toolchain", "list"], None, probe_env, TIMEOUT["probe"])
    names = [line.split()[0] for line in out.splitlines() if line.strip()] if code == 0 else []
    if not any(name == pin or name.startswith(pin + "-") for name in names):
        return [f"toolchain {pin}"] + components + targets
    installed: List[str] = []
    for kind in ("component", "target"):
        code, out, _ = run([rustup, kind, "list", "--installed", "--toolchain", pin], None, probe_env, TIMEOUT["probe"])
        installed += [line.strip() for line in out.splitlines() if line.strip()] if code == 0 else []
    missing = [c for c in components if not any(line == c or line.startswith(c + "-") for line in installed)]
    return missing + [t for t in targets if t not in installed]


def sync_rust(expected: dict, plat: str, env: Dict[str, str], home: Path, cwd: Path, run: Runner) -> Step:
    pin = expected["tools"]["rust"]
    os_name = plat.split("-", 1)[0]
    if expected.get("platform_overrides", {}).get(os_name, {}).get("rust") == "absent":
        return Step("rustup", "skipped", f"platform.{os_name} rust = absent")
    rustup = find_rustup(env, home)
    if rustup is None:
        return Step("rustup", "skipped", "rustup is not installed; this script never installs rustup itself")
    missing = rust_missing(rustup, expected, env, run)
    if not missing:
        return Step("rustup", "ok", f"{pin} already has every pinned component and target")
    argv = [rustup, "toolchain", "install", pin, "--profile", "minimal", "--no-self-update"]
    if expected.get("rust_components"):
        argv += ["--component", ",".join(expected["rust_components"])]
    if expected.get("rust_targets"):
        argv += ["--target", ",".join(expected["rust_targets"])]
    step = ran("rustup", argv, run(argv, cwd, dict(env, RUSTUP_AUTO_INSTALL="0"), TIMEOUT["rustup"]))
    if step.status == "ok":
        step.detail = "installed " + ", ".join(missing)
    return step


def is_homebrew(path: str) -> bool:
    return path.startswith(HOMEBREW_PREFIXES) or os.path.realpath(path).startswith(HOMEBREW_PREFIXES)


def advisories(expected: dict, env: Dict[str, str], home: Path, run: Runner) -> List[str]:
    """What this host does differently that this script must not change."""
    notes: List[str] = []
    path = env.get("PATH", "")
    for name in PINNED_COMMANDS:
        found = shutil.which(name, path=path)
        if found and is_homebrew(found):
            notes.append(f"{name} on PATH is Homebrew's ({found}); Homebrew is never changed here")
    rustc = shutil.which("rustc", path=path)
    proxies = cargo_bin(env, home)
    if rustc and os.path.dirname(os.path.abspath(rustc)) != os.path.abspath(proxies):
        notes.append(
            f"rustc on PATH is {rustc}, not rustup's proxy in {proxies}; a shell with this PATH "
            "ignores rust-toolchain.toml. Put the proxy first; it is not changed here"
        )
    rustup = find_rustup(env, home)
    if rustup:
        code, out, _ = run([rustup, "default"], None, dict(env, RUSTUP_AUTO_INSTALL="0"), TIMEOUT["probe"])
        default = out.split()[0] if code == 0 and out.split() else "none"
        pin = expected["tools"]["rust"]
        if not (default == pin or default.startswith(pin + "-")):
            notes.append(f"the default rustup toolchain is {default}; cargo uses {pin} inside the pulp-rs "
                         "crate, and the default is never changed here")
    if sys.platform == "darwin":
        code, out, _ = run(["xcodebuild", "-version"], None, env, TIMEOUT["probe"])
        found = " ".join(out.split()) if code == 0 else "not found"
        notes.append(f"Xcode: {found}; Xcode is never installed or switched here")
    return notes


def run_check(
    pins: Path, mise: Optional[Path], env: Dict[str, str], run: Runner,
) -> Tuple[Step, List[dict], List[dict]]:
    """check_env.py from the ref, inside the pinned environment when the
    pinned mise is usable. Returns the step, the findings that count, and the
    per-checkout npm findings, which a host sync cannot fix."""
    check = str(pins / "tools/toolchain/check_env.py")
    args = ["--root", str(pins), "--fleet-path", "--format", "json"]
    if mise is not None:
        argv = [str(mise), "exec", "--", "python3", check] + args
        check_env = mise_env(env, pins)
    else:
        argv = [sys.executable, check] + args
        check_env = dict(env)
    check_env["RUSTUP_AUTO_INSTALL"] = "0"
    code, out, err = run(argv, pins, check_env, TIMEOUT["check"])
    try:
        report = json.loads(out)
        findings = list(report["findings"])
    except (ValueError, KeyError, TypeError):
        unreadable = {"tool": "check_env", "expected": "a JSON report", "found": f"exit {code}: {tail(out + err, 600)}",
                      "path": check, "fix": "run the argv in this step by hand", "note": ""}
        return Step("check", "failed", "check_env.py produced no report", argv), [unreadable], []
    counted = [f for f in findings if not str(f.get("tool", "")).startswith("npm ")]
    ignored = [f for f in findings if str(f.get("tool", "")).startswith("npm ")]
    detail = "no difference" if not counted else f"{len(counted)} difference(s) remain"
    return Step("check", "ok" if not counted else "failed", detail, argv), counted, ignored


def write_receipt(path: Path, doc: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    scratch = path.with_name(path.name + ".tmp")
    scratch.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    os.replace(str(scratch), str(path))


def sync(
    repo: Path, ref: str, env: Dict[str, str], home: Path, receipt: Path, run: Runner = run_cmd,
) -> Tuple[int, dict]:
    plat = host_platform()
    steps: List[Step] = []
    findings: List[dict] = []
    ignored: List[dict] = []
    notes: List[str] = []
    sha: Optional[str] = None
    digest = ""
    with tempfile.TemporaryDirectory(prefix="pulp-host-sync-") as td:
        pins = Path(td) / "pins"
        pins.mkdir()
        sha, pin_steps = read_pins(repo, ref, pins, env, run)
        steps += pin_steps
        if sha is None:
            status = EXIT_NO_PINS
        else:
            expected = json.loads((pins / "tools/toolchain/expected.json").read_text(encoding="utf-8"))
            digest = str(expected.get("source_sha256", ""))
            mise, why = pinned_mise(expected, home, plat)
            if mise is None:
                steps.append(Step("mise", "skipped", why))
            else:
                steps.append(trust_checkout(mise, repo, env, run))
                argv = [str(mise), "install", "--locked"]
                steps.append(ran("mise", argv, run(argv, pins, mise_env(env, pins), TIMEOUT["mise"])))
            steps.append(sync_rust(expected, plat, env, home, pins, run))
            check, findings, ignored = run_check(pins, mise, env, run)
            steps.append(check)
            notes = advisories(expected, env, home, run)
            if any(s.network for s in steps):
                status = EXIT_RETRY
            elif findings or any(s.status == "failed" for s in steps):
                status = EXIT_FINDINGS
            else:
                status = EXIT_OK
    doc = {
        "schema": 1,
        "tool": "tools/toolchain/host_sync.py",
        "generated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "platform": plat,
        "repo": str(repo),
        "ref": ref,
        "ref_sha": sha,
        "manifest_sha256": digest,
        "exit": status,
        "network_failures": [s.name for s in steps if s.network],
        "steps": [s.as_dict() for s in steps],
        "findings": findings,
        "ignored_per_checkout": ignored,
        "report_only": notes,
    }
    write_receipt(receipt, doc)
    return status, doc


def main(argv: Optional[List[str]] = None, run: Runner = run_cmd) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--repo", type=Path, default=ROOT,
                        help="the Pulp checkout to fetch in and read the ref from (default: this one)")
    parser.add_argument("--ref", default="origin/main", help="where the pins come from (default: origin/main)")
    parser.add_argument("--receipt", type=Path, help=f"default: ~/{RECEIPT}")
    parser.add_argument("--quiet", action="store_true", help="print nothing when the host matches")
    args = parser.parse_args(argv)
    env = dict(os.environ)
    home = Path(env.get("HOME") or os.path.expanduser("~"))
    receipt = args.receipt or home / RECEIPT
    with held_lock(home / LOCK) as held:
        if not held:
            print(f"host-sync: another run holds {home / LOCK}; nothing changed", file=sys.stderr)
            return EXIT_BUSY
        status, doc = sync(args.repo, args.ref, env, home, receipt, run)
    if status == EXIT_OK and args.quiet:
        return status
    for step in doc["steps"]:
        flag = " (network: retry later)" if step["network_failure"] else ""
        print(f"host-sync: {step['step']}: {step['status']}{flag}: {step['detail'].splitlines()[0] if step['detail'] else ''}")
    for finding in doc["findings"]:
        print(f"host-sync: {finding.get('tool')}: expected {finding.get('expected')}, found {finding.get('found')}; "
              f"fix: {finding.get('fix')}")
    for note in doc["report_only"]:
        print(f"host-sync: report only: {note}")
    print(f"host-sync: exit {status}; receipt {receipt}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
