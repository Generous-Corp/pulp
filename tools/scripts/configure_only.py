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
  `make`, `xcodebuild`), and after configure the tree must hold no `.ninja_log`
  and no object file outside CMake's own configure-time probe directories
  (compiler identification, try_compile, the LTO support check). Either
  violation fails the run.
- a temporary `build-configure-only.*` directory under the checkout (ignored by
  git, never under /tmp, which the root configure refuses), removed afterwards
  on success, failure or SIGTERM.
- governed: the configure runs under tools/ci/governed-build.sh as a background
  one-core lease (PULP_BUILD_CLASS=background, PULP_BUILD_JOBS=1) and at nice
  10, so it takes the host's background share and never more.

Before configuring it materializes what the gate's configure sees, so the
inventory matches the gate's: `setup.sh --deps-only` (the shared dependency
cache), and `npm ci` for tools/import-design/jsx-runtime when its
node_modules are missing (without them the configure omits two Node test
registrations and the generators refuse the tree as INCOMPLETE_CONFIGURE).

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
import shutil
import signal
import subprocess
import sys
import tempfile
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
# Directories CMake itself compiles probes into while configuring: compiler
# identification (CompilerIdC/CXX), try_compile scratch (CMakeScratch, CMakeTmp)
# and the check_ipo_supported() test project (_CMakeLTOTest-*).
PROBE_DIR_MARKERS = ("CompilerId", "CMakeScratch", "CMakeTmp", "_CMake")
BUILD_TOOLS = ("ninja", "make", "gmake", "xcodebuild", "msbuild")

GOVERNED_ENV = {"PULP_BUILD_CLASS": "background", "PULP_BUILD_JOBS": "1"}

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


def compile_evidence(build: Path) -> list[str]:
    """What in ``build`` shows a compile ran: a `.ninja_log`, or an object file
    outside CMake's configure-time probe directories. Empty after a pure configure."""
    found = []
    if (build / ".ninja_log").exists():
        found.append(".ninja_log (ninja ran a build step)")
    for dirpath, dirnames, filenames in os.walk(build):
        rel = Path(dirpath).relative_to(build)
        if any(marker in part for part in rel.parts for marker in PROBE_DIR_MARKERS):
            dirnames[:] = []
            continue
        for name in filenames:
            if name.endswith(OBJECT_SUFFIXES):
                found.append((rel / name).as_posix())
    return sorted(found)


def governor_prefix(root: Path) -> list[str]:
    """The command prefix that bounds the configure to the host's background share."""
    governor = os.environ.get("PULP_CONFIGURE_ONLY_GOVERNOR",
                              str(root / "tools" / "ci" / "governed-build.sh"))
    prefix = ["nice", "-n", "10"] if shutil.which("nice") else []
    if governor and Path(governor).is_file():
        return ["bash", governor, *prefix]
    return prefix


def _run(cmd: list[str], root: Path, env: dict[str, str] | None = None) -> str:
    """Run one setup or configure step; "" on success, else why not."""
    shown = " ".join(cmd[:4])
    say(f"running {shown}{' …' if len(cmd) > 4 else ''}")
    try:
        proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", env=env)
    except OSError as exc:
        return f"{cmd[0]} could not run: {exc}"
    if proc.returncode != 0:
        for line in (proc.stdout + proc.stderr).strip().splitlines()[-15:]:
            say(f"  {line}")
        return f"`{shown} …` exited {proc.returncode}"
    return ""


def prepare_dependencies(root: Path) -> str:
    """Materialize the inputs the gate's configure sees; "" on success."""
    setup = os.environ.get("PULP_GATES_SETUP", str(root / "setup.sh"))
    if setup and Path(setup).is_file():
        why = _run(["nice", "-n", "10", "bash", setup, "--deps-only", "--non-interactive"]
                   if shutil.which("nice") else ["bash", setup, "--deps-only", "--non-interactive"], root)
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
            why = _run([npm, "ci", "--prefix", str(jsx), "--prefer-offline", "--no-audit", "--no-fund"], root)
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


@contextlib.contextmanager
def temporary_build(root: Path) -> Iterator[Path]:
    """A fresh build directory under ``root``, removed on exit, SIGTERM included."""
    build = Path(tempfile.mkdtemp(prefix=TEMP_PREFIX, dir=root))
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
        say(f"configured {build.name} (configure only, no compile); running the command")
        argv_out = [part.replace("{build}", str(build)) for part in command]
        try:
            return subprocess.run(argv_out, cwd=root).returncode
        except OSError as exc:
            say(f"{argv_out[0]} could not run: {exc}")
            return CONFIGURE_FAILED


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
