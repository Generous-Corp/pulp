#!/usr/bin/env python3
"""The build job fetches from the network before the build, with retries.

Two downloads used to fail the required macos check on a gate VM that briefly
lost DNS or had a transfer reset:

  * experimental/pulp-rs is built by a CMake custom command and tested by
    `cargo test` ctests. On an ephemeral VM the cargo registry cache starts
    empty, so those commands pulled ~100 crates from crates.io in the middle
    of the Build and Test steps. "Fetch pulp-rs crates" now fetches the locked
    set first, with spaced retries.
  * The pinned Chrome for Testing download retried only on the errors curl
    calls transient, which excludes a resolve failure (exit 6) and a receive
    reset (exit 56) - the two that actually happened.

The cargo cases run the step's real script, taken out of build.yml, with a stub
`cargo` first on PATH that fails a set number of attempts, so the retry, the
opt-in offline export and the non-fatal exhaustion are exercised rather than
restated. The ordering and curl-flag cases are structural because the property
IS the structure: which step runs first, and which flags curl receives.
"""

from __future__ import annotations

import os
import re
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/build.yml"
CARGO_STEP = "Fetch pulp-rs crates"
CHROME_STEP = "Install pinned Chrome for browser-source fidelity (macOS ARM64)"
# Steps that run cargo against experimental/pulp-rs: the CMake custom command
# in Build and the `cargo test` ctests in either test step.
CARGO_CONSUMERS = ("Build", "Test (non-Windows)", "Test fast deterministic tier (pull request head)")


def _build_steps() -> list[dict]:
    workflow = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    return workflow["jobs"]["build"]["steps"]


def _step(name: str) -> dict:
    for step in _build_steps():
        if step.get("name") == name:
            return step
    raise AssertionError(f"build.yml's build job declares no step named {name!r}")


def _run_cargo_step(fail_attempts: int, offline_var: str | None) -> tuple[subprocess.CompletedProcess, str, list[str]]:
    """Run the real step script with a stub cargo that fails `fail_attempts` times."""
    script = _step(CARGO_STEP)["run"]
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        bindir = tmpdir / "bin"
        bindir.mkdir()
        calls = tmpdir / "calls"
        counter = tmpdir / "count"
        counter.write_text("0")
        stub = bindir / "cargo"
        stub.write_text(
            textwrap.dedent(
                f"""\
                #!/bin/bash
                echo "$*" >> {calls}
                n=$(( $(cat {counter}) + 1 ))
                echo "$n" > {counter}
                [ "$n" -gt {fail_attempts} ]
                """
            )
        )
        stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
        github_env = tmpdir / "github_env"
        github_env.write_text("")
        env = {
            "PATH": f"{bindir}:/usr/bin:/bin",
            "HOME": str(tmpdir),
            "GITHUB_ENV": str(github_env),
            "PULP_CARGO_FETCH_RETRY_DELAY_SECS": "0",
        }
        if offline_var is not None:
            env["PULP_CARGO_NET_OFFLINE"] = offline_var
        proc = subprocess.run(
            ["bash", "-c", script],
            cwd=REPO,
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )
        recorded = calls.read_text().splitlines() if calls.exists() else []
        return proc, github_env.read_text(), recorded


class CargoFetchStep(unittest.TestCase):
    def test_fetches_the_pulp_rs_lockfile(self) -> None:
        proc, github_env, calls = _run_cargo_step(fail_attempts=0, offline_var="0")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(calls, ["fetch --manifest-path experimental/pulp-rs/Cargo.toml"])
        self.assertTrue((REPO / "experimental/pulp-rs/Cargo.lock").is_file())
        # Offline is opt-in: the default leaves later cargo calls untouched.
        self.assertNotIn("CARGO_NET_OFFLINE", github_env)

    def test_retries_a_transient_failure(self) -> None:
        proc, _, calls = _run_cargo_step(fail_attempts=2, offline_var="0")
        self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        self.assertEqual(len(calls), 3)
        self.assertEqual(proc.stdout.count("::warning::cargo fetch attempt"), 2)

    def test_exhaustion_warns_and_does_not_fail_the_job(self) -> None:
        proc, github_env, calls = _run_cargo_step(fail_attempts=99, offline_var="1")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(calls), 3)
        self.assertIn("could not fetch", proc.stdout)
        # Never declare offline when the crates are not on disk.
        self.assertNotIn("CARGO_NET_OFFLINE", github_env)

    def test_opt_in_offline_after_a_successful_fetch(self) -> None:
        proc, github_env, _ = _run_cargo_step(fail_attempts=1, offline_var="1")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("CARGO_NET_OFFLINE=true", github_env.splitlines())

    def test_offline_opt_in_reads_the_repo_variable(self) -> None:
        env = _step(CARGO_STEP).get("env", {})
        self.assertIn("vars.PULP_CARGO_NET_OFFLINE", env.get("PULP_CARGO_NET_OFFLINE", ""))
        self.assertIn("'0'", env.get("PULP_CARGO_NET_OFFLINE", ""))

    def test_runs_before_every_cargo_consumer(self) -> None:
        names = [step.get("name") for step in _build_steps()]
        fetch = names.index(CARGO_STEP)
        present = [name for name in CARGO_CONSUMERS if name in names]
        self.assertIn("Build", present)
        for name in present:
            self.assertLess(fetch, names.index(name), f"{CARGO_STEP!r} must run before {name!r}")


class ChromeDownloadStep(unittest.TestCase):
    def _curl_args(self) -> str:
        run = _step(CHROME_STEP)["run"]
        joined = re.sub(r"\\\n\s*", " ", run)
        lines = [line for line in joined.splitlines() if re.match(r"\s*curl\s", line)]
        self.assertEqual(len(lines), 1, "expected exactly one curl invocation in the Chrome step")
        return lines[0]

    def test_resolve_and_reset_failures_are_retried(self) -> None:
        args = self._curl_args()
        self.assertIn("--retry-all-errors", args)
        match = re.search(r"--retry (\d+)", args)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 5)
        self.assertRegex(args, r"--retry-delay \d+")

    def test_retried_bytes_are_still_hash_checked(self) -> None:
        self.assertIn("shasum -a 256", _step(CHROME_STEP)["run"])


class NoUnretriedCurlInTheBuildJob(unittest.TestCase):
    def test_every_curl_names_retry_all_errors(self) -> None:
        offenders = []
        for step in _build_steps():
            run = re.sub(r"\\\n\s*", " ", step.get("run") or "")
            for line in run.splitlines():
                code = line.split("#", 1)[0]
                if re.search(r"(^|[\s;&|(])curl\s", code) and "--retry-all-errors" not in code:
                    offenders.append(f"{step.get('name')}: {code.strip()[:120]}")
        self.assertEqual(offenders, [], "a curl in the build job lacks --retry-all-errors")


if __name__ == "__main__":
    unittest.main()
