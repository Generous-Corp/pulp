#!/usr/bin/env python3
"""Configure the gate's CMake tree in a temporary directory, run one command, delete it.

Four generated files are written from a configured build and never from a
compiled one: `.shipyard/changed-surface-families.toml`,
`test/ctest_script_inputs/`, `tools/ci/source_selftests.json` and
`docs/status/consumption-profiles.json`. Their generators read the CTest
inventory (`ctest --show-only=json-v1`) and the CMake file-API codemodel, and
both exist after configure. This runs that configure where any fleet host may
run it, including the gate and dev-overflow hosts whose heavy local lanes
gate_host_policy.sh refuses:

    python3 tools/scripts/configure_only.py -- \\
        python3 tools/scripts/changed_surface_script_families.py --build-dir {build} --write

The bounds, which gate_host_policy.sh names as the configure-only allowance:

- configure only. The command may not be a build (`cmake --build`, `ninja`,
  `make`, `xcodebuild` as its argv[0]), and after the configure AND again after
  the command the tree must hold no `.ninja_log` and no object file outside
  CMake's own configure-time probe directories (compiler identification,
  try_compile, the LTO support check). Any violation fails the run with 3, so a
  build hidden in a wrapper (`bash -c 'cmake --build {build}'`) is still caught,
  after the fact.
- a temporary `build-configure-only.*` directory under the checkout (ignored by
  git, never under /tmp, which the root configure refuses), removed afterwards
  on success, failure or SIGTERM. The child runs in its own process group; on
  SIGTERM the whole group (governor and cmake) gets TERM and a grace period
  before KILL, so the governor's EXIT trap releases its lease.
  A tree whose owner is gone (its lock is free) is swept at the next start.
- governed: the configure and the command both run under
  tools/ci/governed-build.sh as a background one-core lease
  (PULP_BUILD_CLASS=background, PULP_BUILD_JOBS=1) at nice 10, so together they
  take the host's background share and never more.

Before configuring it materializes what the gate's configure sees, so the
inventory matches the gate's: `setup.sh --deps-only` (the shared dependency
cache), and `npm ci` for tools/import-design/jsx-runtime when its
node_modules are missing (without them the configure omits two Node test
registrations and the generators refuse the tree as INCOMPLETE_CONFIGURE).
Those two steps run at nice 10 but OUTSIDE the governor: they fetch and link
into shared caches (network and disk bound, no compile), and a warm cache makes
them a few seconds.

Exit codes: the command's own exit code; 2 the configure (or its setup) failed;
3 the configure compiled something, or the command is a build.

Env: CMAKE (default `cmake`), PULP_GATES_SETUP (default `<root>/setup.sh`; empty
skips it), PULP_CONFIGURE_ONLY_GOVERNOR (default
`<root>/tools/ci/governed-build.sh`; empty runs without one),
PULP_CONFIGURE_ONLY_NPM=0 skips the jsx-runtime install.
"""
from __future__ import annotations

import argparse
import contextlib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Iterator

HERE = Path(__file__).resolve().parent
TEMP_PREFIX = "build-configure-only."
CODEMODEL_QUERY = Path(".cmake") / "api" / "v1" / "query" / "codemodel-v2"
# The gate's configure switches (build.yml's macos job, gates_script_inputs.py).
GATE_CONFIGURE_ARGS = ("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPULP_BUILD_TESTS=ON",
                       "-DPULP_BUILD_EXAMPLES=OFF")
JSX_RUNTIME = Path("tools") / "import-design" / "jsx-runtime"
JSX_RUNTIME_MODULES = ("esbuild/package.json", "@babel/parser/package.json")
OBJECT_SUFFIXES = (".o", ".obj")
# Directories CMake itself compiles probes into while configuring, matched by
# their exact place under a `CMakeFiles` directory so a target or source
# directory that merely contains one of these words is never exempt:
#   CMakeFiles/<version>/CompilerId<LANG>   compiler identification
#   CMakeFiles/CMakeScratch, CMakeFiles/CMakeTmp   try_compile scratch
#   CMakeFiles/_CMakeLTOTest-<LANG>   the check_ipo_supported() test project
PROBE_UNDER_CMAKEFILES = re.compile(r"CMakeScratch|CMakeTmp|_CMakeLTOTest-[A-Za-z]+")
PROBE_COMPILER_ID = re.compile(r"CompilerId[A-Z][A-Za-z]*")
CMAKE_VERSION_DIR = re.compile(r"\d+\.\d+(\.\d+)*(-[A-Za-z0-9.]+)?")
BUILD_TOOLS = ("ninja", "make", "gmake", "xcodebuild", "msbuild")

GOVERNED_ENV = {"PULP_BUILD_CLASS": "background", "PULP_BUILD_JOBS": "1"}
LOCK_NAME = ".configure-only.lock"
# A tree with no lock to test (no fcntl) counts as stale after this long.
STALE_WITHOUT_LOCK_S = 6 * 3600
TERM_GRACE_S = float(os.environ.get("PULP_CONFIGURE_ONLY_TERM_GRACE", "10"))

CONFIGURE_FAILED = 2
COMPILED = 3


def say(msg: str) -> None:
    print(f"configure-only: {msg}", file=sys.stderr, flush=True)


def is_build_command(argv: list[str]) -> bool:
    """Whether ``argv`` compiles: `cmake --build`, or a build tool run directly."""
    if not argv:
        return False
    tool = Path(argv[0]).name.lower()
    if tool in BUILD_TOOLS:
        return True
    return tool.startswith("cmake") and "--build" in argv[1:]


def is_probe_dir(parts: tuple[str, ...]) -> bool:
    """Whether the build-relative directory ``parts`` is, or is inside, one of
    CMake's configure-time probe directories (PROBE_UNDER_CMAKEFILES,
    PROBE_COMPILER_ID), judged by exact names at their exact place."""
    for i, part in enumerate(parts):
        if i >= 1 and parts[i - 1] == "CMakeFiles" and PROBE_UNDER_CMAKEFILES.fullmatch(part):
            return True
        if (i >= 2 and parts[i - 2] == "CMakeFiles" and CMAKE_VERSION_DIR.fullmatch(parts[i - 1])
                and PROBE_COMPILER_ID.fullmatch(part)):
            return True
    return False


def compile_evidence(build: Path) -> list[str]:
    """What in ``build`` shows a compile ran: a `.ninja_log`, or an object file
    outside CMake's configure-time probe directories. Empty after a pure configure."""
    found = []
    if (build / ".ninja_log").exists():
        found.append(".ninja_log (ninja ran a build step)")
    for dirpath, dirnames, filenames in os.walk(build):
        rel = Path(dirpath).relative_to(build)
        if is_probe_dir(rel.parts):
            dirnames[:] = []
            continue
        for name in filenames:
            if name.endswith(OBJECT_SUFFIXES):
                found.append((rel / name).as_posix())
    return sorted(found)


def _niced(cmd: list[str]) -> list[str]:
    return ["nice", "-n", "10", *cmd] if shutil.which("nice") else cmd


def governor_prefix(root: Path) -> list[str]:
    """The prefix that bounds the configure and the command to the host's
    background share: the governor, then nice 10."""
    governor = os.environ.get("PULP_CONFIGURE_ONLY_GOVERNOR",
                              str(root / "tools" / "ci" / "governed-build.sh"))
    prefix = _niced([])
    if governor and Path(governor).is_file():
        return ["bash", governor, *prefix]
    return prefix


def _signal_group(proc: subprocess.Popen, sig: int) -> None:
    """Send ``sig`` to the child's whole process group (POSIX), else to the child."""
    try:
        if hasattr(os, "killpg"):
            os.killpg(proc.pid, sig)
        elif sig == signal.SIGTERM:
            proc.terminate()
        else:
            proc.kill()
    except (ProcessLookupError, PermissionError):
        pass


# Signals whose default action kills this process outright, skipping the
# group TERM below and orphaning the child's whole group (governor, its
# foreground `taskpolicy -b` command, the lease heartbeat) in its own session.
_FATAL_SIGNALS = tuple(getattr(signal, name) for name in ("SIGTERM", "SIGHUP") if hasattr(signal, name))


def _raise_exit(signum, frame):  # noqa: ARG001
    raise SystemExit(128 + signum)


@contextlib.contextmanager
def _signals_raise_exit() -> Iterator[None]:
    """While the child runs, turn a default-action SIGTERM/SIGHUP into
    SystemExit so run_child's cleanup runs whoever called it. A handler the
    caller installed (temporary_build's) or an ignored signal is left alone."""
    restore = []
    for sig in _FATAL_SIGNALS:
        try:
            if signal.getsignal(sig) == signal.SIG_DFL:
                signal.signal(sig, _raise_exit)
                restore.append(sig)
        except ValueError:  # not the main thread: signals are the main thread's
            break
    try:
        yield
    finally:
        for sig in restore:
            signal.signal(sig, signal.SIG_DFL)


def run_child(cmd: list[str], root: Path, env: dict[str, str] | None = None,
              capture: bool = True) -> tuple[int, str]:
    """Run ``cmd`` in its own process group. On any interruption (SIGTERM or
    SIGHUP arrive as SystemExit, whether or not the caller installed a handler)
    TERM the whole group and wait TERM_GRACE_S before KILL. The group matters:
    governed-build.sh runs its command in the foreground, and bash defers a
    trapped TERM until that foreground child exits, so TERM to the governor
    alone would wait out the grace and be KILLed with its lease held. TERM to
    the group stops cmake too, then the governor's EXIT trap releases the lease
    and its live-build marker. Without the handler a default-action TERM would
    kill only this process: the child is in its own session, so nothing else
    would ever signal it. Returns (returncode, combined output when captured)."""
    out = subprocess.PIPE if capture else None
    with _signals_raise_exit():
        proc = subprocess.Popen(cmd, cwd=root, env=env, stdout=out,
                                stderr=subprocess.STDOUT if capture else None,
                                text=True, encoding="utf-8", errors="replace",
                                start_new_session=hasattr(os, "killpg"))
        try:
            text, _ = proc.communicate()
        except BaseException:
            _signal_group(proc, signal.SIGTERM)
            try:
                proc.wait(timeout=TERM_GRACE_S)
            except subprocess.TimeoutExpired:
                _signal_group(proc, signal.SIGKILL if hasattr(signal, "SIGKILL") else signal.SIGTERM)
                proc.wait()
            raise
    return proc.returncode, text or ""


def _run(cmd: list[str], root: Path, env: dict[str, str] | None = None) -> str:
    """Run one setup or configure step; "" on success, else why not."""
    shown = " ".join(cmd[:4])
    say(f"running {shown}{' …' if len(cmd) > 4 else ''}")
    try:
        rc, text = run_child(cmd, root, env)
    except OSError as exc:
        return f"{cmd[0]} could not run: {exc}"
    lines = text.strip().splitlines()
    if rc != 0:
        for line in lines[-15:]:
            say(f"  {line}")
        return f"`{shown} …` exited {rc}"
    # The lease the governor granted is the evidence the share was bounded.
    for line in lines:
        if line.startswith("[governed-build]"):
            say(f"  {line}")
    return ""


def prepare_dependencies(root: Path) -> str:
    """Materialize the inputs the gate's configure sees; "" on success."""
    setup = os.environ.get("PULP_GATES_SETUP", str(root / "setup.sh"))
    if setup and Path(setup).is_file():
        why = _run(_niced(["bash", setup, "--deps-only", "--non-interactive"]), root)
        if why:
            return why
    jsx = root / JSX_RUNTIME
    if os.environ.get("PULP_CONFIGURE_ONLY_NPM", "1") != "0" and (jsx / "package-lock.json").is_file() \
            and not all((jsx / "node_modules" / m).is_file() for m in JSX_RUNTIME_MODULES):
        npm = shutil.which("npm")
        if npm is None:
            say(f"npm not found; {JSX_RUNTIME.as_posix()} dependencies stay missing and the "
                "generators will refuse the tree if the configure finds Node")
        else:
            why = _run(_niced([npm, "ci", "--prefix", str(jsx), "--prefer-offline", "--no-audit", "--no-fund"]), root)
            if why:
                return why
    return ""


def configure(root: Path, build: Path, *, governed: bool = True) -> str:
    """Configure ``build`` with the gate's switches and a codemodel query; "" on success."""
    (build / CODEMODEL_QUERY).parent.mkdir(parents=True, exist_ok=True)
    (build / CODEMODEL_QUERY).touch()
    cmd = [os.environ.get("CMAKE", "cmake"), "-S", str(root), "-B", str(build), *GATE_CONFIGURE_ARGS]
    env = None
    if governed:
        env = {**os.environ, **GOVERNED_ENV}
        cmd = governor_prefix(root) + cmd
    return _run(cmd, root, env)


def _try_lock(path: Path):
    """An exclusive non-blocking lock on ``path``: the open file, None when
    another process holds it, or False where locks are unavailable."""
    try:
        import fcntl  # noqa: PLC0415  (POSIX only)
    except ImportError:
        return False
    handle = open(path, "a+", encoding="utf-8")  # noqa: SIM115  (held for the tree's life)
    try:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        return None
    return handle


def sweep_stale(root: Path) -> list[str]:
    """Remove temporary trees whose owner is gone (their lock is free, or with
    no lock support, older than STALE_WITHOUT_LOCK_S). Returns their names."""
    swept = []
    for tree in sorted(root.glob(TEMP_PREFIX + "*")):
        if not tree.is_dir():
            continue
        lock = tree / LOCK_NAME
        if lock.is_file():
            held = _try_lock(lock)
            if held is None:
                continue  # a live run owns it
            if held is False and time.time() - tree.stat().st_mtime < STALE_WITHOUT_LOCK_S:
                continue
            if held:
                held.close()
        elif time.time() - tree.stat().st_mtime < 60:
            continue  # being created right now, before its lock exists
        shutil.rmtree(tree, ignore_errors=True)
        swept.append(tree.name)
    if swept:
        say(f"swept {len(swept)} stale temporary tree(s): {', '.join(swept)}")
    return swept


@contextlib.contextmanager
def temporary_build(root: Path) -> Iterator[Path]:
    """A fresh build directory under ``root``, removed on exit, SIGTERM included.
    Holds a lock inside it for its life, so a later run can tell it is live."""
    sweep_stale(root)
    build = Path(tempfile.mkdtemp(prefix=TEMP_PREFIX, dir=root))
    lock = _try_lock(build / LOCK_NAME)
    previous = signal.getsignal(signal.SIGTERM)

    def _terminate(signum, frame):  # noqa: ARG001
        raise SystemExit(128 + signum)

    try:
        signal.signal(signal.SIGTERM, _terminate)
    except ValueError:  # not the main thread
        previous = None
    try:
        yield build
    finally:
        shutil.rmtree(build, ignore_errors=True)
        if lock:
            lock.close()
        if previous is not None:
            signal.signal(signal.SIGTERM, previous)


@contextlib.contextmanager
def configured(root: Path) -> Iterator[tuple[Path | None, str, int]]:
    """Yield (build, "", 0) for a configured temporary build, or (None, why, code)
    when it could not be configured without compiling. The directory is removed
    on exit either way."""
    with temporary_build(root) as build:
        why = prepare_dependencies(root) or configure(root, build)
        if why:
            yield None, why, CONFIGURE_FAILED
            return
        compiled = compile_evidence(build)
        if compiled:
            yield None, ("the configure compiled outside CMake's probe directories: "
                         + ", ".join(compiled[:5])), COMPILED
            return
        yield build, "", 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        usage="configure_only.py [--repo-root DIR] -- COMMAND [ARG ...]  ({build} names the configured dir)")
    ap.add_argument("--repo-root", type=Path, default=HERE.parents[1])
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        ap.error("a command to run against the configured build is required (after --)")
    if is_build_command(command):
        say(f"REFUSED `{' '.join(command[:3])}`: this path configures only and never compiles. "
            "Build with `pulp build` (governed) on a host whose role allows it.")
        return COMPILED
    root = args.repo_root.resolve()
    with configured(root) as (build, why, code):
        if build is None:
            say(f"FAILED: {why}")
            return code
        say(f"configured {build.name} (configure only, no compile); running the command "
            "under the same governed share")
        argv_out = governor_prefix(root) + [part.replace("{build}", str(build)) for part in command]
        try:
            rc, _ = run_child(argv_out, root, {**os.environ, **GOVERNED_ENV}, capture=False)
        except OSError as exc:
            say(f"{argv_out[0]} could not run: {exc}")
            return CONFIGURE_FAILED
        compiled = compile_evidence(build)
        if compiled:
            say("FAILED: the command compiled in the configure-only tree: " + ", ".join(compiled[:5])
                + ". This path never builds; build with `pulp build` on a host whose role allows it.")
            return COMPILED
        return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
