"""Core harness for the Pulp sandbox E2E test suite.

Every test runs in an isolated tempdir + shadowed `$PATH`. The harness
MUST never write to the user's real `~/.pulp/` install. The
``Sandbox.assert_no_contamination()`` check is invoked at teardown by
the pytest ``sandbox`` fixture and must pass after every scenario.

Design constraints (see issue #732 + Shipyard #248 for the spec):

* ``$PATH`` is shadowed to exactly ``$SANDBOX/bin:/usr/bin:/bin`` so a
  stray ``pulp`` elsewhere on ``PATH`` cannot be invoked accidentally.
* ``$PULP_HOME`` is set to ``$SANDBOX/home`` for every subprocess —
  isolates ``config.toml``, ``projects.json``, ``update-cache.json``,
  pending-upgrade markers, and any other state the CLI persists.
* Binaries are *copied* (not symlinked) into ``$SANDBOX/bin``. On macOS
  the C++ binary pulls ``libwgpu_native.dylib`` via
  ``@rpath/@loader_path``, so any dylibs discovered via ``otool -L``
  are copied alongside the binary to keep the rpath contract intact.
* ``cmake --install`` and ``pulp sdk install`` are never invoked.
  The harness copies already-built artifacts — it does not build or
  install them.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


# ----- Contamination audit --------------------------------------------------

#: Directories that must never be written to by any sandbox test.
#: The audit records mtimes at sandbox setup; any file ``newer`` than
#: the sandbox-start sentinel inside these trees fails the teardown.
PROTECTED_PATHS: tuple[Path, ...] = (
    Path.home() / ".pulp",
    Path.home() / ".local" / "bin",
    Path.home() / ".cargo" / "bin",
)


@dataclass(frozen=True)
class ContaminationReport:
    """What the contamination audit found, if anything."""

    offenders: tuple[Path, ...]
    sentinel_mtime: float

    @property
    def clean(self) -> bool:
        return not self.offenders

    def format(self) -> str:
        if self.clean:
            return "clean — no writes to protected paths"
        lines = [
            "CONTAMINATION DETECTED — test wrote outside its sandbox:",
        ]
        lines.extend(f"  {p}" for p in self.offenders)
        lines.append(f"(sentinel mtime: {self.sentinel_mtime})")
        return "\n".join(lines)


def _find_newer(root: Path, sentinel_mtime: float) -> list[Path]:
    """Return paths under ``root`` with mtime strictly greater than
    ``sentinel_mtime``. Skips missing roots. Never follows symlinks."""
    if not root.exists():
        return []
    newer: list[Path] = []
    # rglob is slow but correct; protected paths are small and bounded.
    # Catch OSError on unreadable files (e.g. macOS's sealed folders).
    try:
        iterator = root.rglob("*")
    except OSError:
        return []
    for path in iterator:
        try:
            st = path.lstat()
        except (OSError, FileNotFoundError):
            continue
        if st.st_mtime > sentinel_mtime:
            newer.append(path)
    return newer


# ----- Binary staging -------------------------------------------------------


def _otool_dylibs(binary: Path) -> list[Path]:
    """Return ``@rpath`` / ``@loader_path`` dylibs that must sit next to
    the binary for the rpath contract. Returns ``[]`` on non-macOS or
    on an ``otool`` error — the harness is macOS-first but must not
    crash elsewhere."""
    if sys.platform != "darwin":
        return []
    try:
        result = subprocess.run(
            ["otool", "-L", str(binary)],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (FileNotFoundError, subprocess.SubprocessError):
        return []

    dylibs: list[Path] = []
    for line in result.stdout.splitlines():
        line = line.strip()
        # Format: "@rpath/libwgpu_native.dylib (compat ...)"
        m = re.match(r"^(@rpath|@loader_path)/(\S+\.dylib)\b", line)
        if not m:
            continue
        # Search for the dylib alongside the binary first — that's how
        # the installer ships it.
        candidate = binary.parent / m.group(2)
        if candidate.exists():
            dylibs.append(candidate)
    return dylibs


# ----- Sandbox --------------------------------------------------------------


@dataclass
class RunResult:
    """Captured result of a ``Sandbox.run`` invocation."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str

    def expect_success(self) -> "RunResult":
        if self.returncode != 0:
            raise AssertionError(
                f"command failed (rc={self.returncode}): {' '.join(self.argv)}\n"
                f"stdout: {self.stdout}\nstderr: {self.stderr}"
            )
        return self


class Sandbox:
    """Isolated sandbox for running the ``pulp`` CLI under test.

    Usage:

        with Sandbox() as sbx:
            sbx.stage_binary(Path("/.../pulp"), as_name="pulp")
            sbx.run(["version"]).expect_success()
            sbx.assert_no_contamination()
    """

    def __init__(self, *, keep: bool = False) -> None:
        self._keep = keep
        self._tmpdir: Path | None = None
        self._sentinel_mtime: float | None = None

    # -- lifecycle --

    def __enter__(self) -> "Sandbox":
        self.setup()
        return self

    def __exit__(self, *exc_info) -> None:
        self.teardown()

    def setup(self) -> None:
        if self._tmpdir is not None:
            raise RuntimeError("Sandbox already set up")
        self._tmpdir = Path(tempfile.mkdtemp(prefix="pulp-sandbox-e2e."))
        (self._tmpdir / "bin").mkdir()
        (self._tmpdir / "home").mkdir()
        # Record a sentinel mtime just before the test starts. We use
        # a tempfile's mtime as an unambiguous "now" reference so we
        # don't race the filesystem clock.
        sentinel = self._tmpdir / ".sentinel"
        sentinel.write_text("")
        self._sentinel_mtime = sentinel.stat().st_mtime
        # Give the filesystem clock a moment so writes during the test
        # unambiguously post-date the sentinel. Most filesystems have
        # 1s mtime resolution on macOS.
        time.sleep(0.05)

    def teardown(self) -> None:
        if self._tmpdir is None:
            return
        if not self._keep:
            shutil.rmtree(self._tmpdir, ignore_errors=True)
        self._tmpdir = None
        self._sentinel_mtime = None

    # -- accessors --

    @property
    def root(self) -> Path:
        assert self._tmpdir is not None, "sandbox not set up"
        return self._tmpdir

    @property
    def bin_dir(self) -> Path:
        return self.root / "bin"

    @property
    def home(self) -> Path:
        return self.root / "home"

    @property
    def sentinel_mtime(self) -> float:
        assert self._sentinel_mtime is not None
        return self._sentinel_mtime

    # -- staging --

    def stage_binary(self, source: Path, as_name: str) -> Path:
        """Copy ``source`` into ``$SANDBOX/bin/<as_name>`` along with
        any ``@rpath`` / ``@loader_path`` dylibs it needs. Returns the
        staged path.

        Copies (not symlinks) are mandatory — the harness must never
        give a test a back-door to the real installed binary.
        """
        source = source.resolve()
        if not source.is_file():
            raise FileNotFoundError(f"binary not found: {source}")
        dest = self.bin_dir / as_name
        shutil.copy2(source, dest)
        dest.chmod(0o755)
        for dylib in _otool_dylibs(source):
            dylib_dest = self.bin_dir / dylib.name
            if not dylib_dest.exists():
                shutil.copy2(dylib, dylib_dest)
        return dest

    def write_stub(self, name: str, body: str) -> Path:
        """Write an executable shell stub to ``$SANDBOX/bin/<name>``."""
        dest = self.bin_dir / name
        dest.write_text(body)
        dest.chmod(0o755)
        return dest

    # -- execution --

    def env(self, overrides: Mapping[str, str] | None = None) -> dict[str, str]:
        """Minimal environment for subprocess invocation.

        ``PATH`` is shadowed to ``$SANDBOX/bin:/usr/bin:/bin`` so any
        stray ``pulp`` on the user's ``PATH`` cannot be invoked.
        ``PULP_HOME`` points at the sandbox's isolated state dir.
        """
        # Start from a minimal baseline. Keep HOME so the CLI's "am I
        # inside a project?" heuristic works; the contamination guard
        # defends against writes under HOME.
        base: dict[str, str] = {
            "PATH": f"{self.bin_dir}:/usr/bin:/bin",
            "PULP_HOME": str(self.home),
            "HOME": os.environ.get("HOME", str(Path.home())),
            "LANG": os.environ.get("LANG", "C"),
            "LC_ALL": os.environ.get("LC_ALL", "C"),
            # Silence test-environment markers so the CLI behaves
            # like a real interactive invocation — no `pulp build`
            # treating us like CI.
            "TERM": "dumb",
        }
        # Some platforms need USER / LOGNAME / TMPDIR for subprocesses
        # that shell out to Git or similar; carry them through.
        for k in ("USER", "LOGNAME", "TMPDIR", "SHELL"):
            if k in os.environ:
                base[k] = os.environ[k]
        if overrides:
            base.update(overrides)
        return base

    def run(
        self,
        argv: Sequence[str],
        *,
        binary: str = "pulp",
        env_overrides: Mapping[str, str] | None = None,
        cwd: Path | None = None,
        timeout: float = 30.0,
    ) -> RunResult:
        """Invoke ``$SANDBOX/bin/<binary>`` with ``argv`` under the
        sandbox environment.

        Returns a ``RunResult`` with captured stdout/stderr/returncode.
        Raises ``TimeoutError`` if the child outruns ``timeout``."""
        exe = self.bin_dir / binary
        if not exe.exists():
            raise FileNotFoundError(
                f"binary '{binary}' not staged in {self.bin_dir}"
            )
        full_argv = (str(exe),) + tuple(argv)
        try:
            proc = subprocess.run(
                full_argv,
                env=self.env(env_overrides),
                cwd=str(cwd) if cwd else str(self.root),
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            raise TimeoutError(
                f"timeout running {' '.join(full_argv)} after {timeout}s\n"
                f"partial stdout: {exc.stdout}\nstderr: {exc.stderr}"
            ) from exc
        return RunResult(
            argv=full_argv,
            returncode=proc.returncode,
            stdout=proc.stdout or "",
            stderr=proc.stderr or "",
        )

    # -- state readback --

    def read(self, rel_path: str) -> str:
        """Read a file under ``$SANDBOX/home`` (typical: ``config.toml``,
        ``projects.json``). Raises ``FileNotFoundError`` if missing."""
        return (self.home / rel_path).read_text()

    def exists(self, rel_path: str) -> bool:
        return (self.home / rel_path).exists()

    # -- contamination audit --

    def audit_contamination(
        self,
        extra_roots: Iterable[Path] = (),
    ) -> ContaminationReport:
        """Scan the protected paths (plus any ``extra_roots``) for files
        with mtime newer than the sandbox-start sentinel."""
        offenders: list[Path] = []
        for root in (*PROTECTED_PATHS, *extra_roots):
            offenders.extend(_find_newer(root, self.sentinel_mtime))
        return ContaminationReport(
            offenders=tuple(offenders),
            sentinel_mtime=self.sentinel_mtime,
        )

    def assert_no_contamination(self) -> None:
        """Raise ``AssertionError`` if the audit finds any writes to
        protected paths since the sandbox was set up."""
        report = self.audit_contamination()
        if not report.clean:
            raise AssertionError(report.format())


# ----- Plugin-command parsing -----------------------------------------------

#: Matches ``pulp <word>`` at the start of a line, inside a fenced code
#: block, or after a ``./build/tools/cli/`` prefix. Captures the first
#: subcommand token so scenarios can enumerate the surface.
_PLUGIN_CMD_RE = re.compile(
    r"""(?:^|\s|`)              # start, whitespace, or backtick
        (?:\./build/tools/cli/)?  # optional relative-path prefix
        pulp\s+                    # the binary
        ([a-z][a-z-]*)             # subcommand
    """,
    re.VERBOSE | re.MULTILINE,
)


def enumerate_plugin_commands(commands_dir: Path) -> set[str]:
    """Parse every ``.claude/commands/*.md`` file under ``commands_dir``
    and return the set of ``pulp <subcommand>`` invocations the plugin
    shells out to.

    Skips variables / placeholders (``pulp $ARGUMENTS``, ``pulp <x>``,
    ``pulp [target]``). The result is the list of subcommand tokens a
    real user can expect the plugin to invoke."""
    subcommands: set[str] = set()
    for md in sorted(commands_dir.glob("*.md")):
        text = md.read_text(encoding="utf-8", errors="replace")
        for match in _PLUGIN_CMD_RE.finditer(text):
            token = match.group(1)
            # Filter obvious placeholders and docs-example tokens.
            if token in {"docs", "pr"}:
                subcommands.add(token)
                continue
            subcommands.add(token)
    # These aren't subcommands, they're noise from prose examples.
    subcommands.discard("the")
    subcommands.discard("and")
    subcommands.discard("is")
    return subcommands


# ----- doctor --versions --json JSON schema probe --------------------------


def parse_versions_json(stdout: str) -> dict:
    """Parse ``pulp doctor --versions --json`` output. Strips any
    leading non-JSON garbage (some builds print a header before the
    JSON payload) and returns the decoded object."""
    # Find the first `{` and parse from there; tolerate trailing text.
    idx = stdout.find("{")
    if idx < 0:
        raise ValueError(f"no JSON object in output: {stdout!r}")
    decoder = json.JSONDecoder()
    obj, _end = decoder.raw_decode(stdout[idx:])
    if not isinstance(obj, dict):
        raise ValueError(f"expected object, got {type(obj).__name__}")
    return obj


#: Keys the JSON schema MUST expose (both binaries). Scenarios check
#: these are present and comparable across the C++ and Rust surfaces.
REQUIRED_DOCTOR_KEYS: tuple[str, ...] = (
    "cli",
    "plugin",
    "plugin_min_cli",
    "project_root",
)
