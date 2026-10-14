#!/usr/bin/env python3
"""release-cli, the release dry-run and the release-path PR gate package alike.

Each of the three used to carry its own copy of the packaging step. The PR
gate's copy never packaged, so a FetchContent layout change that broke Linux
packaging passed it and stalled v0.963.0 to v0.965.0; the dry-run's copy fell
behind release-cli's arguments and went red unnoticed. Now all three call
tools/ci/release_package_cli.sh and isolate their dependencies with the same
step body, and this test fails if any of them stops doing so.

Run:
    python3 tools/scripts/test_release_package_step.py
"""

from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
SCRIPT = "tools/ci/release_package_cli.sh"
ISOLATION_STEP = "Isolate the build from shared caches"
sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))

import release_path_changed  # noqa: E402

# (workflow, job, packaging step) — every caller of the shared script.
CALLERS = (
    ("release-cli.yml", "build-cli", "Package CLI (Unix)"),
    ("release-dry-run.yml", "dry-run", "Package CLI (dry-run — no upload)"),
    ("release-path-pr-gate.yml", "release-build", "Package CLI (release path)"),
)
# PowerShell cannot run the bash script; the Windows legs package inline.
INLINE_ALLOWED = {("release-cli.yml", "Package CLI (Windows)")}


def load(name: str) -> dict:
    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def steps(workflow: dict, job: str) -> list[dict]:
    return workflow["jobs"][job]["steps"]


def direct_packager_calls(name: str, workflow: dict) -> list[str]:
    """Steps that run package_cli.py themselves instead of the shared script."""
    found = []
    for job in (workflow.get("jobs") or {}).values():
        for step in job.get("steps") or []:
            run = str(step.get("run", ""))
            if ("package_cli.py" in run or "PULP_RELEASE_PACKAGER\"" in run
                    or "$env:PULP_RELEASE_PACKAGER" in run) \
                    and (name, step.get("name")) not in INLINE_ALLOWED \
                    and "--binary" in run:
                found.append(step.get("name", "?"))
    return found


class EveryCallerUsesTheSharedScript(unittest.TestCase):
    def test_each_caller_packages_through_the_script(self) -> None:
        for name, job, step_name in CALLERS:
            with self.subTest(workflow=name):
                step = next(s for s in steps(load(name), job) if s.get("name") == step_name)
                # release-cli resolves the script through the overlay variable
                # (see ReleaseCliDispatchFindsTheScript); the others run it
                # from their own checkout.
                expected = ('bash "$PULP_RELEASE_PACKAGE_SCRIPT"' if name == "release-cli.yml"
                            else f"bash {SCRIPT}")
                self.assertIn(expected, step["run"])

    def test_no_caller_keeps_a_private_copy(self) -> None:
        for name, _, _ in CALLERS:
            with self.subTest(workflow=name):
                self.assertEqual(direct_packager_calls(name, load(name)), [])

    def test_a_private_copy_is_caught(self) -> None:
        """Control: the dry-run step as it was before the shared script."""
        wf = {"jobs": {"dry-run": {"steps": [{
            "name": "Package CLI (dry-run — no upload)",
            "run": "python3 tools/scripts/package_cli.py --binary build/pulp "
                   "--build-dir build --platform darwin-arm64 --out x.tar.gz",
        }]}}}
        self.assertEqual(direct_packager_calls("release-dry-run.yml", wf),
                         ["Package CLI (dry-run — no upload)"])

    def test_the_pr_gate_bootstraps_rust_like_the_release(self) -> None:
        """Packaging needs the Rust build/pulp CLI; a gate runner without
        cargo must recover the way a release runner does."""
        name = "Ensure a working Rust toolchain on PATH"
        release = next(s for s in steps(load("release-cli.yml"), "build-cli")
                       if s.get("name") == name)
        gate = next(s for s in steps(load("release-path-pr-gate.yml"), "release-build")
                    if s.get("name") == name)
        self.assertEqual(gate["run"], release["run"])

    def test_every_caller_isolates_with_the_release_step_body(self) -> None:
        reference = next(
            s for s in steps(load("release-cli.yml"), "build-cli")
            if s.get("name") == ISOLATION_STEP
        )["run"]
        for name, job, step_name in CALLERS:
            with self.subTest(workflow=name):
                job_steps = steps(load(name), job)
                names = [s.get("name") for s in job_steps]
                self.assertIn(ISOLATION_STEP, names)
                isolate = names.index(ISOLATION_STEP)
                self.assertEqual(job_steps[isolate]["run"], reference,
                                 "the isolation body drifted from release-cli.yml's")
                self.assertNotIn("if", job_steps[isolate])
                self.assertLess(isolate, names.index("Bootstrap dependencies"))
                self.assertLess(isolate, names.index(step_name))


class ReleaseCliDispatchFindsTheScript(unittest.TestCase):
    """A release-cli dispatch runs main's workflow against the TAG's checkout
    (`ref: inputs.source_ref || inputs.version`), and no tag before this script
    existed contains it. Main's copy is overlaid into the backfill helpers dir
    and run through PULP_RELEASE_PACKAGE_SCRIPT, like package_cli.py."""

    HELPERS = "Ensure release-content verifier helpers exist"

    def helpers_run(self) -> str:
        return next(s for s in steps(load("release-cli.yml"), "build-cli")
                    if s.get("name") == self.HELPERS)["run"]

    def test_dispatch_overlays_main_copy_and_exports_it(self) -> None:
        run = self.helpers_run()
        self.assertRegex(run, rf"\n\s+{re.escape(SCRIPT)} \\\n", "the script is in the helper list")
        # The condition list that makes a dispatch overlay main's copy: from
        # the workflow_dispatch test to its `; }; then`. A match elsewhere in
        # the step (the compat_path block) does not count.
        dispatch = re.search(
            r"if \[ '\$\{\{ github\.event_name \}\}' = 'workflow_dispatch' \] \\\n"
            r"(?P<cond>.*?); \}; then", run, re.S)
        self.assertIsNotNone(dispatch, "the dispatch overlay condition was not found")
        self.assertIn(f'|| [ "$path" = {SCRIPT} ]', dispatch.group("cond"),
                      "a dispatch overlays main's copy")
        self.assertIn('compat_path="$compat_dir/tools/ci/release_package_cli.sh"', run)
        self.assertIn('PULP_RELEASE_PACKAGE_SCRIPT=$compat_dir/tools/ci/release_package_cli.sh', run)
        self.assertIn(f"PULP_RELEASE_PACKAGE_SCRIPT={SCRIPT}", run, "a push uses the checkout's")

    def test_a_tag_checkout_without_the_script_still_packages(self) -> None:
        """Behaviour: the tag tree lacks the script; the overlaid copy, run from
        the tag checkout as cwd, packages it. Control: the checkout-relative
        call the step used to make fails there."""
        tag = Path(tempfile.mkdtemp(prefix="tag-checkout-"))
        self.addCleanup(shutil.rmtree, tag, True)
        (tag / "CMakeLists.txt").write_text("project(pulp\n    VERSION 0.966.0\n)\n",
                                            encoding="utf-8")
        (tag / "tools/scripts").mkdir(parents=True)
        (tag / "tools/scripts/release_product_matrix.json").write_bytes(
            (REPO_ROOT / "tools/scripts/release_product_matrix.json").read_bytes())
        packager = tag / ".packager.py"
        packager.write_text(FAKE_PACKAGER, encoding="utf-8")
        compat_root = Path(tempfile.mkdtemp(prefix="compat-"))
        self.addCleanup(shutil.rmtree, compat_root, True)
        compat = compat_root / "tools/ci/release_package_cli.sh"
        compat.parent.mkdir(parents=True)
        compat.write_bytes((REPO_ROOT / SCRIPT).read_bytes())
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tag),
               "PYTHON": sys.executable, "PULP_RELEASE_PACKAGER": str(packager),
               "PULP_RELEASE_PACKAGE_SCRIPT": str(compat)}
        self.assertFalse((tag / SCRIPT).exists())
        control = subprocess.run(["bash", "-c", f"bash {SCRIPT} linux-x64 v0.966.0"],
                                 cwd=tag, env=env, capture_output=True, text=True,
                                 encoding="utf-8")
        self.assertNotEqual(control.returncode, 0)
        self.assertIn("No such file", control.stderr)
        fixed = subprocess.run(
            ["bash", "-c", 'bash "$PULP_RELEASE_PACKAGE_SCRIPT" linux-x64 v0.966.0'],
            cwd=tag, env=env, capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)
        self.assertTrue((tag / "pulp-linux-x64.tar.gz").is_file())


FAKE_PACKAGER = """\
import sys, pathlib
args = sys.argv[1:]
pathlib.Path("argv.txt").write_text("\\n".join(args), encoding="utf-8")
out = args[args.index("--out") + 1]
pathlib.Path(out).write_bytes(b"tarball")
"""


class ScriptBehaviour(unittest.TestCase):
    """Run the real script against a fake build tree and a recording packager."""

    def run_script(self, platform: str, tag: str | None, *, import_design: bool = True,
                   node: bool = False) -> list[str]:
        tmp = Path(tempfile.mkdtemp(prefix="release-package-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        (tmp / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(pulp\n    VERSION 0.966.0\n)\n",
            encoding="utf-8")
        scripts = tmp / "tools" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "release_product_matrix.json").write_bytes(
            (REPO_ROOT / "tools/scripts/release_product_matrix.json").read_bytes())
        packager = tmp / "fake_packager.py"
        packager.write_text(FAKE_PACKAGER, encoding="utf-8")
        if import_design:
            idd = tmp / "build/tools/import-design"
            (idd / "browser_capture-v1").mkdir(parents=True)
            (idd / "jsx-runtime").mkdir()
            binary = idd / "pulp-import-design"
            binary.write_text("", encoding="utf-8")
            binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
            for mjs in ("materialized_binding_contract", "materialized_source_emitter",
                        "materialized_ids_map"):
                (idd / "jsx-runtime" / f"{mjs}.mjs").write_text("", encoding="utf-8")
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tmp),
               "PYTHON": sys.executable, "PULP_RELEASE_PACKAGER": str(packager)}
        if node:
            env["PULP_NODE_RUNTIME_EXECUTABLE"] = str(tmp / "node-rt" / "node")
        cmd = ["bash", str(REPO_ROOT / SCRIPT), platform] + ([tag] if tag else [])
        proc = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True,
                              encoding="utf-8")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue((tmp / f"pulp-{platform}.tar.gz").is_file())
        return (tmp / "argv.txt").read_text(encoding="utf-8").splitlines()

    def test_darwin_release_carries_every_component(self) -> None:
        argv = self.run_script("darwin-arm64", "v0.966.0", node=True)
        for flag in ("--binary", "--cpp-binary", "--mcp-binary", "--control-broker-binary",
                     "--control-standalone-host-binary", "--control-standalone-manifest",
                     "--import-design-binary", "--import-design-runtime-dir",
                     "--import-design-contract", "--import-design-source-emitter",
                     "--import-design-ids-map", "--node-runtime", "--node-license"):
            with self.subTest(flag=flag):
                self.assertIn(flag, argv)
        self.assertEqual(argv[argv.index("--platform") + 1], "darwin-arm64")
        self.assertEqual(argv[argv.index("--out") + 1], "pulp-darwin-arm64.tar.gz")

    def test_linux_carries_no_control_binaries(self) -> None:
        argv = self.run_script("linux-x64", "v0.966.0")
        self.assertNotIn("--control-broker-binary", argv)
        self.assertNotIn("--control-standalone-host-binary", argv)
        self.assertIn("--import-design-ids-map", argv)

    def test_without_a_tag_the_project_version_decides(self) -> None:
        """The dry-run and the PR gate pass no tag; 0.966.0 is past every floor."""
        self.assertEqual(self.run_script("darwin-arm64", None),
                         self.run_script("darwin-arm64", "v0.966.0"))

    def test_an_old_tag_keeps_its_old_contract(self) -> None:
        argv = self.run_script("darwin-arm64", "v0.900.0")
        self.assertIn("--allow-missing-import-design-contract", argv)
        self.assertNotIn("--import-design-source-emitter", argv)

    def test_without_import_design_no_import_design_args(self) -> None:
        argv = self.run_script("linux-x64", "v0.966.0", import_design=False)
        self.assertFalse([a for a in argv if a.startswith("--import-design")])


class MergeGroupClassifier(unittest.TestCase):
    GATE = WORKFLOWS / "release-path-pr-gate.yml"

    def test_the_parser_reads_the_same_list_as_yaml(self) -> None:
        text = self.GATE.read_text(encoding="utf-8")
        on = yaml.safe_load(text)[True]  # YAML 1.1 reads the `on` key as True
        self.assertEqual(release_path_changed.workflow_paths(text),
                         on["pull_request"]["paths"])

    def test_release_paths_run_and_others_do_not(self) -> None:
        globs = release_path_changed.workflow_paths(self.GATE.read_text(encoding="utf-8"))
        for path, expected in (
            ("tools/scripts/package_cli.py", True),
            ("tools/ci/release_package_cli.sh", True),
            ("tools/cmake/PulpFetchContent.cmake", True),
            ("examples/threejs-native-demo/src/main.cpp", True),
            (".github/workflows/release-cli.yml", True),
            ("docs/guides/release-watchdog.md", False),
            ("tools/cmake/sub/PulpX.cmake", False),
            ("core/view/src/widgets.cpp", False),
        ):
            with self.subTest(path=path):
                self.assertIs(release_path_changed.touches([path], globs), expected)

    def test_a_truncated_compare_list_runs(self) -> None:
        self.assertTrue(release_path_changed.touches(["docs/a.md"] * 300, ["x"]))

    def test_the_gate_runs_in_merge_group_behind_the_classifier(self) -> None:
        gate = yaml.safe_load(self.GATE.read_text(encoding="utf-8"))
        self.assertIn("merge_group", gate[True])
        for job in ("resolve-macos-runner", "release-build"):
            with self.subTest(job=job):
                self.assertIn("classify", gate["jobs"][job]["needs"])
                self.assertEqual(gate["jobs"][job]["if"], "needs.classify.outputs.run == 'true'")


if __name__ == "__main__":
    unittest.main()
