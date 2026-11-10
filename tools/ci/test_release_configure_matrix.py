#!/usr/bin/env python3
"""Tests for the pre-merge release configure gate.

Covers tools/ci/release_configure_matrix.py and holds
.github/workflows/release-configure.yml, release-cli.yml and the checked-in
branch-protection ruleset to each other. Several cases mutate a copy of
release-cli.yml (a new unclassified step, a renamed configure step, an extra
build command, an unsupported expression) and assert the plan refuses it, so
the gate cannot quietly stop replaying what the release runs.

Needs PyYAML (the planner reads workflow YAML); bash for the run cases.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ci"))

import release_configure_matrix as rcm  # noqa: E402

GATE = REPO / ".github" / "workflows" / "release-configure.yml"
RULESET = REPO / ".github" / "rulesets" / "main-protection.json"
CONTRACT = REPO / ".agents" / "contract.toml"

# The error release-cli.yml's darwin-x64 leg stopped on from v1.81.0 to v1.96.0.
INCIDENT_LOG = """\
-- Pulp GPU runtime: forwarding the macOS device layer
CMake Error at build/_deps/vellum_gpu_runtime-src/gpu-runtime/cmake/GpuRuntimeDawn.cmake:74 (message):
  gpu-runtime: libdawn_combined.a does not match the lock; expected
  6c6c5535e3b004ca260f388536c1baa737c11479d0a95865d174888d7d3077f3, got
  09bbc2c20e305664d15036db6d9f2109afd026d5814e5664a9d816632d253e06
Call Stack (most recent call first):
  tools/cmake/PulpGpuRuntime.cmake:70 (include)

-- Configuring incomplete, errors occurred!
"""

# Files changed by the pull request that broke the darwin-x64 leg.
INCIDENT_FILES = [
    "CMakeLists.txt",
    "core/render/CMakeLists.txt",
    "tools/cmake/PulpGpuRuntime.cmake",
    "tools/cmake/PulpGpuRuntimeRecord.cmake",
    "tools/deps/manifest.json",
    "docs/guides/gpu-audio-sdk.md",
]


def load_yaml(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def triggers(doc: dict) -> dict:
    # PyYAML reads the bare key `on` as boolean True.
    return doc.get("on", doc.get(True))


class MutatedWorkflow:
    """A temp copy of release-cli.yml with build-cli edited by a callback."""

    def __init__(self, mutate):
        self.mutate = mutate

    def __enter__(self) -> Path:
        self.dir = tempfile.mkdtemp(prefix="rcm-test-")
        doc = load_yaml(rcm.RELEASE_WORKFLOW)
        self.mutate(doc["jobs"][rcm.JOB])
        path = Path(self.dir) / "release-cli.yml"
        path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
        return path

    def __exit__(self, *exc):
        shutil.rmtree(self.dir, ignore_errors=True)


def step(job: dict, name: str) -> dict:
    return next(s for s in job["steps"] if rcm.step_key(s) == name)


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.platforms = [leg["platform"] for leg in rcm.legs()]

    def test_every_release_step_is_classified(self):
        job = load_yaml(rcm.RELEASE_WORKFLOW)["jobs"][rcm.JOB]
        self.assertEqual(rcm.unclassified_steps(job), [])
        names = {rcm.step_key(s) for s in job["steps"]}
        # No stale names: each classified step still exists in release-cli.yml.
        for table in (rcm.REPLAYED_STEPS, rcm.MIRRORED_STEPS, rcm.SKIPPED_STEPS):
            self.assertEqual(sorted(set(table) - names), [], table)
        overlap = (set(rcm.REPLAYED_STEPS) & set(rcm.SKIPPED_STEPS)) | (
            set(rcm.REPLAYED_STEPS) & set(rcm.MIRRORED_STEPS)) | (
            set(rcm.MIRRORED_STEPS) & set(rcm.SKIPPED_STEPS))
        self.assertEqual(overlap, set())

    def test_every_configure_in_release_cli_is_replayed(self):
        job = load_yaml(rcm.RELEASE_WORKFLOW)["jobs"][rcm.JOB]
        configuring = [rcm.step_key(s) for s in job["steps"]
                       if re.search(r"^\s*cmake\s+-S\s", s.get("run", ""), re.M)]
        self.assertTrue(configuring)
        for name in configuring:
            self.assertIn(name, rcm.REPLAYED_STEPS)

    def test_legs_cover_every_active_release_platform(self):
        from release_artifact_contents import DEFAULT_MATRIX_PATH, ProductMatrix

        active = list(ProductMatrix.load(DEFAULT_MATRIX_PATH).active_platforms)
        self.assertEqual(sorted(self.platforms), sorted(active))
        hosted = {"macos-15", "ubuntu-24.04", "ubuntu-24.04-arm",
                  "windows-latest", "windows-11-arm"}
        routes = {leg["platform"]: json.loads(leg["runs_on_json"]) for leg in rcm.legs()}
        for platform, runner in routes.items():
            self.assertIn(runner, hosted, platform)
        # The release cross-configures darwin-x64 on Apple Silicon; so must this.
        self.assertEqual(routes["darwin-x64"], "macos-15")
        self.assertIn("darwin-x64", self.platforms)

    def test_macos_override_routes_only_the_darwin_legs(self):
        routes = {leg["platform"]: json.loads(leg["runs_on_json"])
                  for leg in rcm.legs('["self-hosted", "macOS", "ARM64"]')}
        self.assertEqual(routes["darwin-x64"], ["self-hosted", "macOS", "ARM64"])
        self.assertEqual(routes["darwin-arm64"], ["self-hosted", "macOS", "ARM64"])
        self.assertEqual(routes["linux-x64"], "ubuntu-24.04")
        self.assertEqual(routes["windows-x64"], "windows-latest")
        for bad in ("self-hosted", "[]", "{}", "[1]", '""'):
            with self.subTest(bad=bad), self.assertRaises(rcm.PlanError):
                rcm.legs(bad)

    def test_plans_replay_release_configures_without_building(self):
        for platform in self.platforms:
            with self.subTest(platform=platform):
                data = rcm.plan(platform)
                names = [s["name"] for s in data["steps"]]
                self.assertIn("Configure (CLI, no WebView on Linux)", names)
                blob = json.dumps(data)
                self.assertNotIn("${{", blob)
                for s in data["steps"]:
                    code = "\n".join(l for l in s["script"].splitlines()
                                     if not l.lstrip().startswith("#"))
                    self.assertNotRegex(code, r"cmake\s+--build|cmake\s+--install")
                self.assertEqual(data["job_env"].get("CCACHE_DISABLE"), "1")
                self.assertRegex(data["release_tag"], r"^v\d+\.\d+\.\d+$")
                os_name = rcm.runner_os(platform)
                self.assertEqual("Prepare SDK build dir (macOS)" in names, os_name == "macOS")
                self.assertEqual("Prepare SDK build dir (Linux)" in names, os_name == "Linux")

    def test_darwin_x64_plan_is_the_cross_configure(self):
        data = rcm.plan("darwin-x64")
        names = [s["name"] for s in data["steps"]]
        self.assertIn("Purge host-arch Skia before Intel cross-fetch (darwin-x64)", names)
        self.assertIn("Verify fetched Skia is x86_64 (darwin-x64)", names)
        configure = next(s for s in data["steps"]
                         if s["name"] == "Configure (CLI, no WebView on Linux)")
        self.assertIn('if [ "darwin-x64" = "darwin-x64" ]', configure["script"])
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", configure["script"])
        self.assertIn("-DPULP_BUILD_WEBVIEW=ON", configure["script"])
        arm = rcm.plan("darwin-arm64")
        self.assertNotIn("Purge host-arch Skia before Intel cross-fetch (darwin-x64)",
                         [s["name"] for s in arm["steps"]])

    def test_linux_plan_turns_webview_off(self):
        configure = next(s for s in rcm.plan("linux-x64")["steps"]
                         if s["name"] == "Configure (CLI, no WebView on Linux)")
        self.assertIn("-DPULP_BUILD_WEBVIEW=OFF", configure["script"])

    # Deliberate breaks: each mutation of release-cli.yml must stop the plan.

    def test_break_new_unclassified_step_fails_the_plan(self):
        def mutate(job):
            job["steps"].insert(10, {"name": "Patch the toolchain", "run": "echo hi"})
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "Patch the toolchain"):
                rcm.plan("darwin-x64", wf)

    def test_break_renamed_configure_step_fails_the_plan(self):
        def mutate(job):
            step(job, "Configure (CLI, no WebView on Linux)")["name"] = "Configure CLI"
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaises(rcm.PlanError):
                rcm.plan("darwin-x64", wf)

    def test_break_extra_build_in_a_configure_step_fails_the_plan(self):
        def mutate(job):
            s = step(job, "Prepare SDK build dir (macOS)")
            s["run"] += "\ncmake --build build-sdk --target everything\n"
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "still builds"):
                rcm.plan("darwin-arm64", wf)

    def test_break_missing_build_line_fails_the_plan(self):
        def mutate(job):
            s = step(job, "Prepare SDK build dir (macOS)")
            s["run"] = rcm.BUILD_LINE.sub("", s["run"])
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "0 build command"):
                rcm.plan("darwin-arm64", wf)

    def test_break_unsupported_expression_fails_the_plan(self):
        def mutate(job):
            s = step(job, "Configure (CLI, no WebView on Linux)")
            s["run"] = s["run"].replace("cmake -S . -B build",
                                        "cmake -S . -B ${{ steps.dir.outputs.path }}", 1)
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "unsupported expression"):
                rcm.plan("darwin-x64", wf)

    def test_break_unmodelled_step_key_fails_the_plan(self):
        def mutate(job):
            step(job, "Fetch Skia binaries")["working-directory"] = "external"
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "working-directory"):
                rcm.plan("darwin-x64", wf)

    def test_break_workflow_env_fails_the_plan(self):
        doc = load_yaml(rcm.RELEASE_WORKFLOW)
        doc["env"] = {"PULP_SOMETHING": "1"}
        with tempfile.TemporaryDirectory() as tmp:
            wf = Path(tmp) / "release-cli.yml"
            wf.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
            with self.assertRaisesRegex(rcm.PlanError, "env"):
                rcm.plan("darwin-x64", wf)

    def test_break_non_bash_replayed_step_fails_the_plan(self):
        def mutate(job):
            step(job, "Fetch Skia binaries")["shell"] = "pwsh"
        with MutatedWorkflow(mutate) as wf:
            with self.assertRaisesRegex(rcm.PlanError, "only bash"):
                rcm.plan("windows-x64", wf)


class ExpressionTests(unittest.TestCase):
    def test_ternary_and_conditions(self):
        linux = rcm.Context("linux-x64", "v1.0.0")
        mac = rcm.Context("darwin-x64", "v1.0.0")
        expr = "${{ runner.os == 'Linux' && '-DPULP_BUILD_WEBVIEW=OFF' || '-DPULP_BUILD_WEBVIEW=ON' }}"
        self.assertEqual(linux.evaluate(expr), "-DPULP_BUILD_WEBVIEW=OFF")
        self.assertEqual(mac.evaluate(expr), "-DPULP_BUILD_WEBVIEW=ON")
        self.assertTrue(mac.evaluate("matrix.platform == 'darwin-x64'"))
        self.assertFalse(linux.evaluate("runner.os != 'Linux'"))
        self.assertFalse(mac.evaluate("matrix.container != ''"))
        tag = "${{ github.event_name == 'workflow_dispatch' && inputs.version || github.ref_name }}"
        self.assertEqual(mac.evaluate(tag), "v1.0.0")
        self.assertTrue(mac.evaluate("${{ hashFiles('tools/scripts/prepare_node_runtime.py') != '' }}"))
        self.assertFalse(mac.evaluate("${{ hashFiles('no/such/file') != '' }}"))
        with self.assertRaises(rcm.PlanError):
            mac.evaluate("secrets.TOKEN")
        with self.assertRaises(rcm.PlanError):
            mac.evaluate("(runner.os == 'Linux')")
        # GitHub compares strings case-insensitively.
        self.assertTrue(linux.evaluate("runner.os == 'linux'"))
        self.assertFalse(linux.evaluate("runner.os != 'LINUX'"))
        with self.assertRaisesRegex(rcm.PlanError, "glob"):
            mac.evaluate("hashFiles('tools/**/*.py') != ''")


class ScopeTests(unittest.TestCase):
    def files(self, *names, patch=None):
        return [{"filename": n, "patch": patch} for n in names]

    def test_incident_change_is_in_scope(self):
        run, hits = rcm.decide(self.files(*INCIDENT_FILES))
        self.assertTrue(run)
        self.assertIn("tools/cmake/PulpGpuRuntime.cmake", hits)

    def test_in_scope_paths(self):
        for path in ("tools/cmake/PulpGpuRuntime.cmake", "tools/deps/manifest.json",
                     "core/render/CMakeLists.txt", "core/foo/Config.cmake.in",
                     "external/skia-build/VERSION.md", "setup.sh",
                     ".github/workflows/release-cli.yml",
                     "tools/scripts/release_product_matrix.json",
                     "tools/ci/release_configure_matrix.py"):
            with self.subTest(path=path):
                self.assertTrue(rcm.in_scope(path))

    def test_out_of_scope_paths(self):
        for path in ("docs/guides/x.md", "core/view/src/widgets.cpp",
                     "test/cmake/render_gpu_surface_tests.cmake",
                     "examples/foo/CMakeLists.txt", ".agents/skills/ci/SKILL.md"):
            with self.subTest(path=path):
                self.assertFalse(rcm.in_scope(path))
        self.assertEqual(rcm.decide(self.files("docs/a.md", "core/x.cpp")), (False, []))

    def test_version_line_only_bump_is_out_of_scope(self):
        bump = "@@ -8,7 +8,7 @@ project(Pulp\n-    VERSION 1.96.0\n+    VERSION 1.97.0\n"
        files = self.files("CMakeLists.txt", patch=bump)
        self.assertEqual(rcm.decide(files, project_version="1.97.0"), (False, []))
        other = bump + "+option(PULP_NEW \"x\" ON)\n"
        self.assertTrue(rcm.decide(self.files("CMakeLists.txt", patch=other),
                                   project_version="1.97.0")[0])
        # A VERSION line that is not the project's (a dependency floor) counts.
        self.assertTrue(rcm.decide(files, project_version="2.0.0")[0])
        # No patch (binary or too large): cannot prove version-only.
        self.assertTrue(rcm.decide(self.files("CMakeLists.txt", patch=None),
                                   project_version="1.97.0")[0])

    def test_configure_inputs_scope_in_what_cmake_runs_or_reads(self):
        inputs = rcm.configure_inputs()
        # execute_process'd at configure (Dawn provider identity, checkout guard),
        # file(READ) data, and a configure_file template.
        for path in ("tools/scripts/gpu_audio_provider_identity.py",
                     "tools/ci/checkout_location_guard.py",
                     "docs/status/gpu-recipes.yaml",
                     "core/runtime/include/pulp/runtime/build_info.hpp.in"):
            with self.subTest(path=path):
                self.assertIn(path, inputs)
                self.assertTrue(rcm.decide(self.files(path), inputs)[0])
        # Control: the scan is not just "everything".
        self.assertFalse(rcm.decide(self.files("docs/guides/local-ci.md"), inputs)[0])
        self.assertFalse(any(p.startswith(("test/", "examples/")) for p in inputs))

    def test_source_files_count_only_when_removed_or_renamed(self):
        inputs = rcm.configure_inputs()
        source = next(p for p in sorted(inputs) if p.endswith(".cpp"))
        self.assertFalse(rcm.decide([{"filename": source, "status": "modified"}], inputs)[0])
        self.assertTrue(rcm.decide([{"filename": source, "status": "removed"}], inputs)[0])
        moved = {"filename": "attic/x.cpp", "status": "renamed", "previous_filename": source}
        self.assertTrue(rcm.decide([moved], inputs)[0])

    def test_rename_out_of_scope_is_judged_by_its_old_path(self):
        moved = {"filename": "docs/attic/PulpGpuRuntime.cmake.txt", "status": "renamed",
                 "previous_filename": "tools/cmake/PulpGpuRuntime.cmake"}
        self.assertTrue(rcm.decide([moved], set())[0])

    def test_event_range(self):
        env = {"EVENT": "pull_request", "PR_BASE": "a", "PR_HEAD": "b", "MQ_BASE": "c",
               "MQ_HEAD": "d"}
        self.assertEqual(rcm.event_range(env), ("a", "b"))
        self.assertEqual(rcm.event_range({**env, "EVENT": "merge_group"}), ("c", "d"))
        for event in ("schedule", "workflow_dispatch", ""):
            self.assertIsNone(rcm.event_range({**env, "EVENT": event}))
        self.assertIsNone(rcm.event_range({**env, "PR_HEAD": ""}))

    def test_compare_cap_runs_every_leg(self):
        many = self.files(*[f"docs/{i}.md" for i in range(rcm.COMPARE_FILE_CAP)])
        self.assertTrue(rcm.decide(many)[0])

    def test_verdict_table(self):
        cases = [
            # event, scope, run, configure -> rc
            ("pull_request", "success", "false", "skipped", 0),
            ("pull_request", "success", "true", "success", 0),
            ("pull_request", "success", "true", "failure", 1),
            ("pull_request", "success", "true", "cancelled", 1),
            ("pull_request", "success", "true", "skipped", 1),
            ("pull_request", "success", "", "skipped", 1),
            ("pull_request", "failure", "", "skipped", 1),
            ("pull_request", "cancelled", "true", "success", 1),
            ("pull_request", "skipped", "", "skipped", 1),
            ("merge_group", "success", "true", "failure", 1),
            ("schedule", "skipped", "", "skipped", 0),
        ]
        for event, scope, run, configure, rc in cases:
            with self.subTest(event=event, scope=scope, run=run, configure=configure):
                self.assertEqual(rcm.verdict(event, scope, run, configure)[0], rc)

    def test_unreadable_scope_runs_every_leg(self):
        env = dict(os.environ, PATH="")
        import subprocess
        out = subprocess.run(
            [sys.executable, str(REPO / "tools/ci/release_configure_matrix.py"), "scope",
             "--repo", "o/r", "--base", "a", "--head", "b"],
            env=env, capture_output=True, text=True, encoding="utf-8", check=True)
        self.assertEqual(out.stdout.strip(), "true")


class RunTests(unittest.TestCase):
    def run_plan(self, steps):
        data = {"platform": "darwin-x64", "runner_os": "macOS", "release_tag": "v1.0.0",
                "job_env": {"CCACHE_DISABLE": "1"}, "steps": steps}
        with tempfile.TemporaryDirectory() as tmp:
            summary = Path(tmp) / "summary.md"
            old = os.environ.get("GITHUB_STEP_SUMMARY")
            os.environ["GITHUB_STEP_SUMMARY"] = str(summary)
            try:
                rc = rcm.run_plan(data, Path(tmp))
            finally:
                if old is None:
                    os.environ.pop("GITHUB_STEP_SUMMARY", None)
                else:
                    os.environ["GITHUB_STEP_SUMMARY"] = old
            text = summary.read_text(encoding="utf-8") if summary.exists() else ""
        return rc, text

    def test_env_and_path_carry_between_steps(self):
        rc, _ = self.run_plan([
            {"name": "a", "env": {}, "script":
                'echo "FOO=bar" >> "$GITHUB_ENV"\n'
                'printf "MULTI<<EOF\\nx\\ny\\nEOF\\n" >> "$GITHUB_ENV"\n'
                'echo "/opt/rcm-test-bin" >> "$GITHUB_PATH"\n'},
            {"name": "b", "env": {"STEP": "1"}, "script":
                'test "$FOO" = bar\n'
                'test "$MULTI" = "$(printf "x\\ny")"\n'
                'case "$PATH" in /opt/rcm-test-bin:*) ;; *) exit 3 ;; esac\n'
                'test "$STEP" = 1\ntest "$CCACHE_DISABLE" = 1\n'},
        ])
        self.assertEqual(rc, 0)

    def test_steps_run_with_errexit_and_pipefail(self):
        # GitHub runs `shell: bash` as `bash -eo pipefail`. release-cli.yml's
        # Linux SDK step has no `set -e`, so without errexit a failed configure
        # followed by the stubbed build line would read as a pass.
        rc, _ = self.run_plan([{"name": "no set -e", "env": {}, "script":
                                "false\necho stubbed build\n"}])
        self.assertEqual(rc, 1)
        rc, _ = self.run_plan([{"name": "pipe", "env": {}, "script":
                                "false | cat\n"}])
        self.assertEqual(rc, 1)

    def test_failure_reports_the_cmake_error(self):
        script = f"cat <<'LOG'\n{INCIDENT_LOG}LOG\nexit 1\n"
        rc, summary = self.run_plan([{"name": "Configure (CLI, no WebView on Linux)",
                                      "env": {}, "script": script}])
        self.assertEqual(rc, 1)
        self.assertIn("libdawn_combined.a does not match the lock", summary)
        self.assertIn("darwin-x64", summary)
        self.assertIn("release_configure_matrix.py run --platform darwin-x64", summary)

    def test_annotation_escapes_workflow_command_data(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rcm.annotate("linux-x64", "Configure", ["CMake Error: 100% bad\n  line two"], 1)
        line = buf.getvalue().strip()
        self.assertTrue(line.startswith("::error title="))
        self.assertNotIn("\n", line)
        self.assertIn("100%25 bad%0A  line two", line)

    def test_cmake_error_blocks(self):
        blocks = rcm.cmake_errors(INCIDENT_LOG)
        self.assertEqual(len(blocks), 1)
        self.assertIn("GpuRuntimeDawn.cmake:74", blocks[0])
        self.assertIn("09bbc2c20e30", blocks[0])
        self.assertNotIn("Configuring incomplete", blocks[0])


class WorkflowContractTests(unittest.TestCase):
    def setUp(self):
        self.gate = load_yaml(GATE)
        self.release = load_yaml(rcm.RELEASE_WORKFLOW)

    def test_required_check_always_reports(self):
        on = triggers(self.gate)
        self.assertIn("pull_request", on)
        self.assertIn("merge_group", on)
        for event in ("pull_request", "merge_group"):
            spec = on[event] or {}
            self.assertNotIn("paths", spec, event)
            self.assertNotIn("paths-ignore", spec, event)
        agg = self.gate["jobs"]["release-configure"]
        self.assertEqual(agg["name"], "release-configure")
        self.assertEqual(agg["if"], "always()")
        self.assertEqual(sorted(agg["needs"]), ["configure", "scope"])
        self.assertNotIn("self-hosted", json.dumps(agg["runs-on"]))
        run = next(s for s in agg["steps"] if "run" in s)
        self.assertEqual(run["run"].strip(), "python3 tools/ci/release_configure_matrix.py verdict")
        self.assertEqual(run["env"]["CONFIGURE_RESULT"], "${{ needs.configure.result }}")
        self.assertEqual(run["env"]["SCOPE_RESULT"], "${{ needs.scope.result }}")
        self.assertEqual(run["env"]["RUN"], "${{ needs.scope.outputs.run }}")
        scope = self.gate["jobs"]["scope"]
        decide = next(s for s in scope["steps"] if s.get("id") == "decide")
        self.assertIn("release_configure_matrix.py scope", decide["run"])
        self.assertIn("--from-event", decide["run"])
        self.assertNotRegex(decide["run"], r"run=(true|false)")
        self.assertEqual(scope["outputs"]["run"], "${{ steps.decide.outputs.run }}")

    def test_legs_run_on_hosted_runners_from_the_plan(self):
        configure = self.gate["jobs"]["configure"]
        self.assertEqual(configure["runs-on"], "${{ fromJSON(matrix.runs_on_json) }}")
        self.assertEqual(configure["strategy"]["fail-fast"], False)
        self.assertEqual(configure["needs"], "scope")
        replay = next(s for s in configure["steps"]
                      if s.get("name") == "Replay the release configure")
        self.assertIn("release_configure_matrix.py run-plan", replay["run"])

    def test_mirrored_steps_match_release_cli(self):
        gate_steps = self.gate["jobs"]["configure"]["steps"]
        release_steps = self.release["jobs"][rcm.JOB]["steps"]
        lf = "Check out release sources with upstream LF bytes"
        gate_lf = next(i for i, s in enumerate(gate_steps) if s.get("name") == lf)
        gate_checkout = next(i for i, s in enumerate(gate_steps)
                             if s.get("uses", "").startswith("actions/checkout@"))
        self.assertLess(gate_lf, gate_checkout)
        self.assertEqual(gate_steps[gate_lf]["run"], step({"steps": release_steps}, lf)["run"])
        self.assertIs(gate_steps[gate_checkout]["with"]["lfs"], False)
        deps = "Install Linux dependencies"
        self.assertEqual(next(s for s in gate_steps if s.get("name") == deps)["with"],
                         step({"steps": release_steps}, deps)["with"])
        for key in ("uses", "if"):
            self.assertEqual(next(s for s in gate_steps if s.get("name") == deps)[key],
                             step({"steps": release_steps}, deps)[key])

    def test_ruleset_and_contract_require_the_check(self):
        ruleset = json.loads(RULESET.read_text(encoding="utf-8"))
        required = [c["context"]
                    for rule in ruleset["rules"] if rule["type"] == "required_status_checks"
                    for c in rule["parameters"]["required_status_checks"]]
        self.assertIn("release-configure", required)
        contract = CONTRACT.read_text(encoding="utf-8")
        self.assertIn(".github/workflows/release-configure.yml", contract)


if __name__ == "__main__":
    unittest.main()
