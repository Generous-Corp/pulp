#!/usr/bin/env python3
"""The build job installs esbuild before the build, never inside it.

tools/scripts/bundle_threejs_for_jsc.mjs runs as an iOS AUv3 POST_BUILD command
and as a ctest. Without tools/scripts/node_modules/esbuild it used to run
`npm install` mid-build, so a registry DNS failure on a gate VM failed the
required macos check as a compile error. build.yml now sets PULP_OFFLINE_BUILD
on the build job (the bundler refuses instead of fetching) and installs the
locked dependency in its own retried step ahead of the Build step.

These cases run that step's real script, taken out of build.yml, in a scratch
checkout holding the committed package.json and package-lock.json. A stub
`npm` first on PATH stands in for the registry: it can fail a set number of
attempts, and on success lays down a minimal esbuild package at whatever
version it is told to. The real node evaluates the step's readiness probe
against that package, so the probe is exercised, not restated.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/build.yml"
STEP_NAME = "Install build-time Node dependencies"
LOCK = REPO / "tools/scripts/package-lock.json"
PACKAGE = REPO / "tools/scripts/package.json"
NODE = shutil.which("node")


def _workflow() -> dict:
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def _build_job() -> dict:
    return _workflow()["jobs"]["build"]


def _step_script() -> str:
    for step in _build_job()["steps"]:
        if step.get("name") == STEP_NAME:
            return step["run"]
    raise AssertionError(f"build.yml's build job declares no step named {STEP_NAME!r}")


def _locked_version() -> str:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    return lock["packages"]["node_modules/esbuild"]["version"]


class NodeBuildDepsWiringTest(unittest.TestCase):
    """The job is offline for the build and installs before it."""

    def test_build_job_sets_offline_build(self) -> None:
        env = _build_job().get("env") or {}
        self.assertEqual(str(env.get("PULP_OFFLINE_BUILD")), "1")

    def test_install_step_runs_before_build(self) -> None:
        names = [step.get("name") for step in _build_job()["steps"]]
        self.assertIn(STEP_NAME, names)
        self.assertIn("Build", names)
        self.assertLess(names.index(STEP_NAME), names.index("Build"))

    def test_install_step_is_unconditional(self) -> None:
        # A condition would reopen the mid-build fetch on whatever leg it
        # excluded: every leg that runs the bundler runs with the job env.
        step = next(s for s in _build_job()["steps"] if s.get("name") == STEP_NAME)
        self.assertNotIn("if", step)

    def test_lock_pins_what_package_json_declares(self) -> None:
        declared = json.loads(PACKAGE.read_text(encoding="utf-8"))["dependencies"]["esbuild"]
        self.assertEqual(declared, _locked_version())


@unittest.skipIf(NODE is None, "node is not on PATH; the step's probe needs it")
class NodeBuildDepsStepTest(unittest.TestCase):
    """Run the real step against a stub registry."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.script = _step_script()
        cls.pin = _locked_version()

    def _run(
        self,
        *,
        preinstalled: str | None = None,
        failures: int = 0,
        installs_version: str | None = None,
        node_on_path: bool = True,
    ):
        """Returns (proc, npm_argv_lines).

        `preinstalled` lays down esbuild at that version before the step runs.
        The stub npm fails its first `failures` launches, then installs esbuild
        at `installs_version` (default: the locked pin).
        """
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            checkout = tmp_path / "checkout"
            scripts = checkout / "tools/scripts"
            scripts.mkdir(parents=True)
            shutil.copy(PACKAGE, scripts / "package.json")
            shutil.copy(LOCK, scripts / "package-lock.json")

            def lay_down(version: str) -> None:
                pkg = scripts / "node_modules/esbuild"
                (pkg / "lib").mkdir(parents=True, exist_ok=True)
                (pkg / "package.json").write_text(
                    json.dumps({"name": "esbuild", "version": version, "main": "lib/main.js"}),
                    encoding="utf-8",
                )
                (pkg / "lib/main.js").write_text(
                    f"module.exports = {{ version: {json.dumps(version)},"
                    " transformSync: () => ({ code: '' }) };\n",
                    encoding="utf-8",
                )

            if preinstalled is not None:
                lay_down(preinstalled)

            argv_log = tmp_path / "npm-argv.log"
            counter = tmp_path / "npm-attempts"
            payload = tmp_path / "payload.py"
            payload.write_text(
                textwrap.dedent(
                    f"""\
                    import json, pathlib
                    pkg = pathlib.Path({str(scripts)!r}) / "node_modules/esbuild"
                    (pkg / "lib").mkdir(parents=True, exist_ok=True)
                    v = {json.dumps(installs_version or self.pin)}
                    (pkg / "package.json").write_text(json.dumps(
                        {{"name": "esbuild", "version": v, "main": "lib/main.js"}}))
                    (pkg / "lib/main.js").write_text(
                        "module.exports = {{ version: " + json.dumps(v) +
                        ", transformSync: () => ({{ code: '' }}) }};\\n")
                    """
                ),
                encoding="utf-8",
            )
            bindir = tmp_path / "bin"
            bindir.mkdir()
            stub = bindir / "npm"
            stub.write_text(
                textwrap.dedent(
                    f"""\
                    #!/bin/bash
                    printf '%s\\n' "$*" >> {argv_log}
                    n=$(( $(cat {counter} 2>/dev/null || echo 0) + 1 ))
                    echo "$n" > {counter}
                    [ "$n" -gt {failures} ] || exit 1
                    exec python3 {payload}
                    """
                ),
                encoding="utf-8",
            )
            stub.chmod(0o755)

            path = f"{bindir}{os.pathsep}{os.environ.get('PATH', '')}"
            if not node_on_path:
                # Keep bash and python3 reachable but hide node.
                hidden = tmp_path / "no-node-bin"
                hidden.mkdir()
                for tool in ("bash", "python3", "cat", "sleep"):
                    found = shutil.which(tool)
                    if found:
                        (hidden / tool).symlink_to(found)
                path = f"{bindir}{os.pathsep}{hidden}"

            env = {
                **os.environ,
                "PATH": path,
                "PULP_NPM_RETRY_DELAY_SECS": "0",
            }
            proc = subprocess.run(
                [shutil.which("bash") or "bash", "-c", self.script],
                cwd=checkout,
                env=env,
                capture_output=True,
                text=True,
                timeout=120,
            )
            calls = argv_log.read_text(encoding="utf-8").splitlines() if argv_log.exists() else []
            return proc, calls

    def test_installs_from_the_lockfile_when_missing(self) -> None:
        proc, calls = self._run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(calls), 1, calls)
        self.assertTrue(calls[0].startswith("ci "), calls)
        self.assertIn("--prefix tools/scripts", calls[0])

    def test_skips_the_registry_when_the_locked_version_is_present(self) -> None:
        proc, calls = self._run(preinstalled=self.pin)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(calls, [])
        self.assertIn("no registry fetch", proc.stdout)

    def test_replaces_a_stale_version(self) -> None:
        proc, calls = self._run(preinstalled="0.0.1")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(calls), 1, calls)

    def test_retries_a_registry_outage(self) -> None:
        proc, calls = self._run(failures=2)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(len(calls), 3, calls)
        self.assertIn("npm ci attempt 1 failed", proc.stdout)
        self.assertIn("npm ci attempt 2 failed", proc.stdout)

    def test_fails_the_step_after_three_attempts(self) -> None:
        proc, calls = self._run(failures=5)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(len(calls), 3, calls)
        self.assertIn("after 3 attempts", proc.stderr)

    def test_an_install_that_does_not_satisfy_the_pin_is_a_failure(self) -> None:
        # npm exiting 0 is not proof: the probe must accept what was laid down.
        proc, calls = self._run(installs_version="0.0.1")
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertEqual(len(calls), 3, calls)

    def test_without_node_there_is_nothing_to_install(self) -> None:
        proc, calls = self._run(node_on_path=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(calls, [])
        self.assertIn("node is not on PATH", proc.stdout)


if __name__ == "__main__":
    unittest.main()
