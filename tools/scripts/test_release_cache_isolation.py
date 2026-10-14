#!/usr/bin/env python3
"""Signed and published builds read no cache an untrusted run can write.

Self-hosted VMs mount the host's ccache and FetchContent source cache writable
into every job, PR gate jobs included, so any PR could plant an object or a
dependency source that a later signed, attested or deployed build links. Every
job that signs, notarizes, attests or publishes AND compiles must therefore:

* set CCACHE_DISABLE=1 at job level, so every object is compiled in the job;
* run the "Isolate the build from shared caches" step before its first build
  step, pointing PULP_SHARED_FETCHCONTENT_SOURCE_DIR and
  PULP_FETCHCONTENT_BASE_DIR at a fresh RUNNER_TEMP root so every dependency is
  re-fetched at its pin, and PULP_CONFIGURE_CHECK_CACHE_DIR at an empty one,
  because PulpConfigureCheckCache.cmake include()s the files it finds there;
* restore no actions/cache, no Namespace cache action or volume, and no
  setup-action cache (`with: cache:`, setup-emsdk `actions-cache-folder`,
  setup-go unless `cache: false`, setup-node unless
  `package-manager-cache: false`). Saving with
  actions/cache/save is allowed: it only publishes what this trusted job
  produced, for other lanes to verify before use.

The step body is also executed here, so the contract is checked by behaviour
and not only by text. Third-party sources these lanes clone (choc,
AudioUnitSDK) must name an exact commit or release tag, never a moving branch.

Run:
    python3 tools/scripts/test_release_cache_isolation.py
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

ISOLATION_STEP = "Isolate the build from shared caches"

# A job that holds signing, notarization, attestation or deploy capability.
PUBLISHES = re.compile(
    r"notarytool|actions/attest|attest-build-provenance|wrangler-action|pages deploy"
    r"|action-gh-release|gh release (?:create|upload|edit)|deploy-pages"
    r"|secrets\.(?:MACOS_CERTIFICATE|SIGNING_IDENTITY|APPLE_|CLOUDFLARE|NOTARY"
    r"|DEVELOPER_ID|P12|KEYCHAIN)"
)
# A step that compiles or fetches build dependencies.
BUILDS = re.compile(
    r"setup\.sh|cmake -S|cmake --build|governed-build|emcmake|cargo build|xcodebuild"
)
# The jobs this contract was written for. The detector must find each of them,
# so a detector that goes blind fails here instead of passing vacuously.
EXPECTED = {
    ("release-cli.yml", "build-cli"),
    ("sign-and-release.yml", "build-and-sign-macos"),
    ("wclap-cloudflare.yml", "build-prove-deploy"),
}
SHARED_CACHE_ACTIONS = ("actions/cache", "setup-namespace-macos-cache",
                        "namespacelabs/nscloud-cache-action")
# `with:` inputs through which a setup action restores its own cache
# (setup-python/-node/-java `cache: pip`, setup-emsdk `actions-cache-folder`).
CACHE_INPUTS = ("cache", "actions-cache-folder")
# Actions that restore a cache unless told not to: `cache` must be false.
CACHE_BY_DEFAULT = {
    "actions/setup-go@": "cache",
    # v5+ caches npm automatically when a package.json names a packageManager.
    "actions/setup-node@": "package-manager-cache",
}
# Write-only: a job that saves a cache reads nothing back from it.
WRITE_ONLY_CACHE_ACTIONS = ("actions/cache/save@",)


def reads_shared_cache(uses: str, with_inputs: dict | None = None) -> bool:
    if uses.startswith(WRITE_ONLY_CACHE_ACTIONS):
        return False
    if any(action in uses for action in SHARED_CACHE_ACTIONS):
        return True
    inputs = with_inputs or {}
    for key in CACHE_INPUTS:
        if key in inputs and str(inputs[key]).strip().lower() not in ("", "false"):
            return True
    for prefix, off_switch in CACHE_BY_DEFAULT.items():
        if uses.startswith(prefix):
            return str(inputs.get(off_switch, "")).strip().lower() != "false"
    return False


def _load(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def _commands(step: dict) -> str:
    """A step's shell commands with comment lines removed."""
    return "\n".join(
        line for line in (step.get("run") or "").splitlines()
        if not line.lstrip().startswith("#")
    )


def signing_build_jobs() -> dict[tuple[str, str], dict]:
    found = {}
    for path in sorted(WORKFLOWS.glob("*.yml")):
        for job_id, job in (_load(path).get("jobs") or {}).items():
            if not isinstance(job, dict):
                continue
            text = yaml.safe_dump(job, width=100000)
            if PUBLISHES.search(text) and BUILDS.search(text):
                found[(path.name, job_id)] = job
    return found


def _run_isolation(body: str, extra_env: dict[str, str]) -> tuple[int, str, Path, Path]:
    tmp = Path(tempfile.mkdtemp(prefix="isolation-"))
    runner_temp = tmp / "runner-temp"
    runner_temp.mkdir()
    github_env = tmp / "github-env"
    github_env.write_text("", encoding="utf-8")
    env = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": str(tmp),
        "RUNNER_TEMP": str(runner_temp),
        "GITHUB_ENV": str(github_env),
        "GITHUB_RUN_ID": "1234",
        "GITHUB_RUN_ATTEMPT": "2",
        **extra_env,
    }
    proc = subprocess.run(["bash", "-c", body], env=env, capture_output=True, text=True, encoding="utf-8")
    return proc.returncode, proc.stdout + proc.stderr, runner_temp, github_env


class SigningJobsReadNoSharedCache(unittest.TestCase):
    def setUp(self) -> None:
        self.jobs = signing_build_jobs()

    def test_detector_finds_every_known_signing_job(self) -> None:
        self.assertTrue(EXPECTED <= set(self.jobs),
                        f"detector missed {sorted(EXPECTED - set(self.jobs))}")

    def test_ccache_is_disabled_for_the_whole_job(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                env = job.get("env") or {}
                self.assertEqual(str(env.get("CCACHE_DISABLE")), "1",
                                 "job-level env must set CCACHE_DISABLE: \"1\"")
                for step in job.get("steps") or []:
                    step_env = step.get("env") or {}
                    self.assertNotIn("CCACHE_DISABLE", step_env,
                                     f"step {step.get('name')!r} overrides CCACHE_DISABLE")
                    self.assertNotIn("CCACHE_DISABLE=", step.get("run") or "",
                                     f"step {step.get('name')!r} rewrites CCACHE_DISABLE")

    def test_isolation_step_runs_before_the_first_build_step(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                steps = job.get("steps") or []
                names = [step.get("name", "") for step in steps]
                self.assertIn(ISOLATION_STEP, names)
                isolate = names.index(ISOLATION_STEP)
                first_build = next(
                    index for index, step in enumerate(steps)
                    if BUILDS.search(_commands(step))
                )
                self.assertLess(isolate, first_build,
                                f"{ISOLATION_STEP!r} must precede {names[first_build]!r}")
                self.assertNotIn("if", steps[isolate],
                                 "the isolation step must run unconditionally")

    def test_isolation_step_points_dependencies_at_a_fresh_job_root(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                step = next(s for s in job["steps"] if s.get("name") == ISOLATION_STEP)
                rc, out, runner_temp, github_env = _run_isolation(step["run"], {})
                self.assertEqual(rc, 0, out)
                written = dict(
                    line.split("=", 1)
                    for line in github_env.read_text(encoding="utf-8").splitlines()
                )
                for name in ("PULP_SHARED_FETCHCONTENT_SOURCE_DIR",
                             "PULP_FETCHCONTENT_BASE_DIR",
                             "PULP_CONFIGURE_CHECK_CACHE_DIR"):
                    self.assertIn(name, written)
                    target = Path(written[name])
                    self.assertTrue(target.is_relative_to(runner_temp), target)
                    self.assertTrue(target.is_dir(), target)
                    self.assertEqual(list(target.iterdir()), [], f"{target} is not fresh")

    def test_nothing_else_points_configure_checks_at_a_shared_dir(self) -> None:
        # The configure-check cache include()s CMake; only the isolation step
        # may choose its directory, and nothing may re-enable a host path.
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                self.assertNotIn("PULP_CONFIGURE_CHECK_CACHE_DIR", job.get("env") or {})
                for step in job.get("steps") or []:
                    if step.get("name") == ISOLATION_STEP:
                        continue
                    text = _commands(step) + yaml.safe_dump(step.get("env") or {})
                    self.assertNotIn("PULP_CONFIGURE_CHECK_CACHE_DIR", text,
                                     f"step {step.get('name')!r} re-points the configure-check cache")

    def test_isolation_step_clears_a_leftover_configure_check_file(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                step = next(s for s in job["steps"] if s.get("name") == ISOLATION_STEP)
                tmp = Path(tempfile.mkdtemp(prefix="isolation-cc-"))
                runner_temp = tmp / "runner-temp"
                planted = runner_temp / "pulp-isolated-1234-2" / "cc" / "SDL3-planted.cmake"
                planted.parent.mkdir(parents=True)
                planted.write_text("message(FATAL_ERROR planted)\n", encoding="utf-8")
                github_env = tmp / "github-env"
                github_env.write_text("", encoding="utf-8")
                proc = subprocess.run(
                    ["bash", "-c", step["run"]],
                    env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tmp),
                         "RUNNER_TEMP": str(runner_temp), "GITHUB_ENV": str(github_env),
                         "GITHUB_RUN_ID": "1234", "GITHUB_RUN_ATTEMPT": "2"},
                    capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertFalse(planted.exists(), "a planted configure-check file survived")
                self.assertIn(f"PULP_CONFIGURE_CHECK_CACHE_DIR={planted.parent}",
                              github_env.read_text(encoding="utf-8"))

    def test_isolation_step_clears_a_leftover_root(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                step = next(s for s in job["steps"] if s.get("name") == ISOLATION_STEP)
                # Plant a file where the job's root will be, then run the step.
                tmp = Path(tempfile.mkdtemp(prefix="isolation-planted-"))
                runner_temp = tmp / "runner-temp"
                planted = runner_temp / "pulp-isolated-1234-2" / "src" / "planted.txt"
                planted.parent.mkdir(parents=True)
                planted.write_text("planted", encoding="utf-8")
                github_env = tmp / "github-env"
                github_env.write_text("", encoding="utf-8")
                proc = subprocess.run(
                    ["bash", "-c", step["run"]],
                    env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tmp),
                         "RUNNER_TEMP": str(runner_temp), "GITHUB_ENV": str(github_env),
                         "GITHUB_RUN_ID": "1234", "GITHUB_RUN_ATTEMPT": "2"},
                    capture_output=True, text=True, encoding="utf-8")
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertFalse(planted.exists(), "a planted source survived the step")

    def test_isolation_step_refuses_a_per_dependency_source_override(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                step = next(s for s in job["steps"] if s.get("name") == ISOLATION_STEP)
                rc, out, _, _ = _run_isolation(
                    step["run"], {"PULP_FETCHCONTENT_SOURCE_DIR_CHOC": "/shared/choc"})
                self.assertNotEqual(rc, 0, out)
                self.assertIn("PULP_FETCHCONTENT_SOURCE_DIR_CHOC", out)

    def test_no_restored_cache_or_cache_volume(self) -> None:
        for key, job in self.jobs.items():
            with self.subTest(job=key):
                for step in job.get("steps") or []:
                    self.assertFalse(reads_shared_cache(step.get("uses") or "", step.get("with")),
                                     f"step {step.get('name')!r} restores a shared cache")

    def test_only_a_save_is_exempt(self) -> None:
        for uses in ("actions/cache@v4", "actions/cache/restore@v4",
                     "./.github/actions/setup-namespace-macos-cache",
                     "namespacelabs/nscloud-cache-action@v1"):
            self.assertTrue(reads_shared_cache(uses), uses)
        self.assertFalse(reads_shared_cache("actions/cache/save@v4"))
        self.assertFalse(reads_shared_cache("actions/checkout@v7"))

    def test_a_setup_action_cache_input_is_refused(self) -> None:
        for uses, inputs in (("actions/setup-python@v6", {"cache": "pip"}),
                             ("actions/setup-node@v4", {"cache": "npm"}),
                             ("mymindstorm/setup-emsdk@v14", {"actions-cache-folder": "emsdk"}),
                             ("actions/setup-go@v5", {}),
                             ("actions/setup-go@v5", {"cache": True}),
                             # setup-node caches by itself unless told not to.
                             ("actions/setup-node@v7", {"node-version": "22"}),
                             ("actions/setup-node@v7", {"package-manager-cache": True})):
            with self.subTest(uses=uses, inputs=inputs):
                self.assertTrue(reads_shared_cache(uses, inputs))
        for uses, inputs in (("actions/setup-python@v6", {"python-version": "3.12"}),
                             ("actions/setup-node@v7", {"cache": "", "package-manager-cache": False}),
                             ("actions/setup-go@v5", {"cache": False})):
            with self.subTest(uses=uses, inputs=inputs):
                self.assertFalse(reads_shared_cache(uses, inputs))

    def test_a_cache_input_in_a_real_signing_job_is_caught(self) -> None:
        # Control on the real detector path: plant `cache: pip` on a step of a
        # job the detector found, and the job-level check must refuse it.
        key = ("wclap-cloudflare.yml", "build-prove-deploy")
        job = self.jobs[key]
        node = next(s for s in job["steps"] if (s.get("uses") or "").startswith("actions/setup-node@"))
        self.assertFalse(reads_shared_cache(node["uses"], node.get("with")))
        self.assertTrue(reads_shared_cache(node["uses"], {**(node.get("with") or {}), "cache": "npm"}))


class WranglerIsPinned(unittest.TestCase):
    """wrangler-action v4 installs the latest wrangler unless told a version."""

    def test_every_wrangler_action_step_pins_an_exact_version(self) -> None:
        steps = 0
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for job_id, job in (_load(path).get("jobs") or {}).items():
                for step in (job or {}).get("steps") or []:
                    if not (step.get("uses") or "").startswith("cloudflare/wrangler-action@"):
                        continue
                    steps += 1
                    with self.subTest(workflow=path.name, job=job_id, step=step.get("name")):
                        version = str((step.get("with") or {}).get("wranglerVersion", ""))
                        self.assertRegex(version, r"^\d+\.\d+\.\d+$",
                                         "set with.wranglerVersion to an exact x.y.z")
        # Control: the production deploy step was found at all.
        self.assertGreaterEqual(steps, 1)


class ThirdPartyClonesArePinned(unittest.TestCase):
    """choc and AudioUnitSDK are cloned at an exact commit or release tag."""

    def setUp(self) -> None:
        self.texts = {path.name: path.read_text(encoding="utf-8")
                      for path in sorted(WORKFLOWS.glob("*.yml"))}

    def test_choc_is_fetched_at_a_commit(self) -> None:
        sites = 0
        for name, text in self.texts.items():
            if "danielraffel/choc.git" not in text:
                continue
            with self.subTest(workflow=name):
                self.assertNotIn("--branch pulp-webview-dnd-poc1", text)
                self.assertNotIn('"$HOME/choc-src"', text)
                for match in re.finditer(r"CHOC_COMMIT: (\S+)", text):
                    sites += 1
                    self.assertRegex(match.group(1), r"^[0-9a-f]{40}$")
                self.assertEqual(text.count("danielraffel/choc.git"),
                                 text.count('test "$(git -C "$dest" rev-parse HEAD)" = "$CHOC_COMMIT"'),
                                 "every choc fetch must verify the commit it checked out")
        # Control: the lanes that vendor choc were found at all.
        self.assertGreaterEqual(sites, 3)

    def test_audiounitsdk_clones_name_the_release_tag(self) -> None:
        clones = 0
        for name, text in self.texts.items():
            for match in re.finditer(r"git clone[^\n]*(?:\\\n[^\n]*)*AudioUnitSDK\.git", text):
                clones += 1
                with self.subTest(workflow=name, clone=match.group(0)):
                    self.assertIn("--branch AudioUnitSDK-1.3.0", match.group(0))
        self.assertGreaterEqual(clones, 2)


if __name__ == "__main__":
    unittest.main()
