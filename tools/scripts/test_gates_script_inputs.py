#!/usr/bin/env python3
"""gates.sh configures a build to check test/ctest_script_inputs.json.

Each test builds a throwaway repository holding a copy of gates.sh, the
script-inputs lane and its two library modules, stubs for the gate scripts
gates.sh refuses to start without, and one script-driven test. `cmake` and
`ctest` are fakes on PATH: the fake cmake records its argv and writes the
files a configured Ninja build has; the fake ctest prints a json-v1 inventory.
PYTHON is a wrapper that runs the real interpreter for the lane under test and
exits 0 for every other gate, so the verdict read back is this lane's alone.

What must hold:
- a diff touching a declared input with no configured build configures
  build-gate (configure only) and runs the check;
- a diff touching no declared input does not configure;
- a current configured build is reused, not reconfigured;
- PULP_GATES_NO_CONFIGURE=1 does not configure and says NOT CHECKED loudly,
  except that a change to a surface the families check blocks FAILS: the
  build-free prediction runs first, and the required gate would check it;
- drift fails gates.sh and prints the exact `--write` command;
- the same lane checks tools/ci/source_selftests.json against the registrations
  and the changed-surface script families: a selftest added without
  regenerating the manifest fails with the `write` command, family drift fails
  with the `--write` command, and a build without a codemodel reply is
  reconfigured once (no compile) rather than skipped.

Run:
    python3 tools/scripts/test_gates_script_inputs.py
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COPIED = [
    "tools/scripts/gates.sh",
    "tools/scripts/gates_script_inputs.py",
    "tools/scripts/script_test_inputs.py",
    "tools/scripts/gate_common.py",
    "tools/ci/source_selftests.py",
    "tools/ci/ctest_gate_args.py",
    "tools/ui-build/lint/clean_output_lint.py",
    "tools/scripts/gate_host_policy.sh",
]

# Stands in for changed_surface_script_families.py: its own suite covers what it
# derives; here only the lane's handling of each exit code is under test.
FAKE_FAMILIES = """import os, sys
mode = "STATIC" if "--static" in sys.argv else "FAMILIES"
default = "changed-surface script families" + (" (static)" if mode == "STATIC" else "") + ": OK"
print(os.environ.get(f"FAKE_{mode}_SAYS", default))
sys.exit(int(os.environ.get(f"FAKE_{mode}_RC", "0")))
"""
STUBS = [
    "tools/scripts/version_bump_check.py",
    "tools/scripts/skill_sync_check.py",
    # gates.sh now fails closed when this instrument is absent; keep the
    # synthetic repository's ordinary-path stub explicit.
    "tools/scripts/vellum_boundary_lint.py",
]

FAKE_CMAKE = r"""#!/bin/sh
echo "$*" >> "$FAKE_CMAKE_LOG"
dir=""
while [ $# -gt 0 ]; do
    [ "$1" = "-B" ] && { dir="$2"; shift; }
    shift
done
mkdir -p "$dir"
printf 'CMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_BUILD_TYPE:STRING=Release\n' > "$dir/CMakeCache.txt"
echo "# fake" > "$dir/CTestTestfile.cmake"
"""

FAKE_CTEST = r"""#!/bin/sh
sed "s#@ROOT@#$FAKE_ROOT#g" "$FAKE_CTEST_INVENTORY"
"""

# Runs the real interpreter for the lane under test; every other gate passes.
FAKE_PYTHON = """#!/bin/sh
case "$1" in
    *gates_script_inputs.py|*script_test_inputs.py) exec "{real}" "$@" ;;
esac
exit 0
"""


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)


# The whole-machine entries EXCLUSIVE_SHARD_PLAN places, as (name, shard, lock).
# `check` fails unless the manifest holds each one, so the fixture carries them.
EXCLUSIVE = [
    ("gpu-dpr-runner-selftest", 1, None),
    ("gpu-first-visible-external-adapter-selftest", 1, None),
    ("gpu-dpr-v2-evidence-selftest", 2, None),
    ("agent-capability-manifest-selftest", 2, "agent-capability-manifest-source"),
    ("agent-capability-rederive-selftest", 2, "agent-capability-manifest-source"),
]
# The three wiring lines `check` requires in the required lane's workflow.
LANE_WORKFLOW = (
    "jobs:\n  lane:\n    strategy:\n      matrix:\n        shard: [1, 2]\n    steps:\n"
    "      - run: python3 tools/ci/source_selftests.py run --min-count 130 --part shared\n"
    "      - run: python3 tools/ci/source_selftests.py run --min-count 130 --part exclusive "
    "--shard ${{ matrix.shard }}/2\n"
)


class GatesScriptInputsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="gates-script-inputs-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.root = self.tmp / "repo"
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for rel in COPIED:
            dst = self.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dst)
        for rel in STUBS:
            (self.root / rel).write_text("import sys\nsys.exit(0)\n", encoding="utf-8")
        (self.root / "tools/scripts/versioning.json").write_text("{}\n", encoding="utf-8")
        self.write("tools/scripts/test_alpha.py", "print('alpha')\n")
        self.write("tools/scripts/alpha_lib.py", "X = 1\n")
        self.write("test/CMakeLists.txt", "# registrations\n")
        self.write("README.md", "readme\n")
        self.write("tools/scripts/changed_surface_script_families.py", FAKE_FAMILIES)
        self.write(".github/workflows/version-skill-check.yml", LANE_WORKFLOW)
        for name, _, _ in EXCLUSIVE:
            self.write(f"tools/ci/test_{name}.py", f"print('{name}')\n")
        self.write("tools/ci/test_beta.py", "print('beta')\n")
        self.write(
            "tools/ui-build/lint/fixtures/clean/FilterPanel.tsx",
            "export function FilterPanel({value}: {value: number}) {\n"
            "  return <button data-pulp-action=\"filter\" aria-label=\"Filter\" "
            "style={{color: tokens.text}}>{value}</button>;\n"
            "}\n",
        )
        self.write(
            "tools/ui-build/lint/fixtures/generated/CapturedFilterPanel.tsx",
            "export function CapturedFilterPanel({value, tokens}: "
            "{value: number; tokens: {text: string}}) {\n"
            "  return <button data-pulp-action=\"filter\" aria-label=\"Filter\" "
            "style={{color: tokens.text}}>{value}</button>;\n"
            "}\n",
        )
        exclusive_entries = [
            {"name": name, "argv": [f"{{repo}}/tools/ci/test_{name}.py"], "processors": 8,
             **({"resource_lock": [lock]} if lock else {})}
            for name, _, lock in EXCLUSIVE]
        self.write("tools/ci/source_selftests.json", json.dumps({"schema_version": 1, "tests": [
            {"name": "beta", "argv": ["{repo}/tools/ci/test_beta.py"], "timeout": 120.0},
            *exclusive_entries]}) + "\n")
        inventory = {"tests": [
            {"name": "alpha", "properties": [],
             "command": ["/usr/bin/python3", "@ROOT@/tools/scripts/test_alpha.py"]},
            {"name": "beta", "properties": [{"name": "LABELS", "value": ["source-selftest"]},
                                            {"name": "TIMEOUT", "value": 120.0}],
             "command": ["/usr/bin/python3", "@ROOT@/tools/ci/test_beta.py"]},
            *({"name": name,
               "properties": [{"name": "LABELS", "value": ["source-selftest"]},
                              {"name": "PROCESSORS", "value": 8},
                              *([{"name": "RESOURCE_LOCK", "value": [lock]}] if lock else [])],
               "command": ["/usr/bin/python3", f"@ROOT@/tools/ci/test_{name}.py"]}
              for name, _, lock in EXCLUSIVE)]}
        self.inventory = self.tmp / "inventory.json"
        self.inventory.write_text(json.dumps(inventory), encoding="utf-8")
        self.cmake_log = self.tmp / "cmake.log"
        for name, text in (("cmake", FAKE_CMAKE), ("ctest", FAKE_CTEST),
                           ("python", FAKE_PYTHON.format(real=sys.executable))):
            p = self.bin / name
            p.write_text(text, encoding="utf-8")
            p.chmod(0o755)
        tartci = self.bin / "tartci"
        tartci.write_text("#!/bin/sh\n"
                          "echo TARTCI_ROLE=${FAKE_TARTCI_ROLE:-light}\n"
                          "echo TARTCI_SERVES_GATE=${FAKE_SERVES_GATE:-0}\n",
                          encoding="utf-8")
        tartci.chmod(0o755)
        git(self.root, "init", "-q", "-b", "main")
        git(self.root, "config", "user.email", "t@example.com")
        git(self.root, "config", "user.name", "t")
        # The base carries a list regenerated the right way: from a configure.
        scratch = self.tmp / "scratch-build"
        scratch.mkdir()
        subprocess.run([sys.executable, str(self.root / "tools/scripts/script_test_inputs.py"),
                        "--repo-root", str(self.root), "--build-dir", str(scratch), "--write"],
                       check=True, capture_output=True, env=self.env())
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", "base")
        git(self.root, "checkout", "-q", "-b", "topic")

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def commit(self, msg: str = "change") -> None:
        git(self.root, "add", ".")
        git(self.root, "commit", "-q", "-m", msg)

    def env(self, **extra: str) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("PULP_GATES", "PULP_SCRIPT_INPUTS", "GITHUB_"))}
        env.update(PATH=f"{self.bin}{os.pathsep}{env.get('PATH', '')}", FAKE_ROOT=str(self.root.resolve()),
                   FAKE_CTEST_INVENTORY=str(self.inventory), FAKE_CMAKE_LOG=str(self.cmake_log),
                   PYTHON=str(self.bin / "python"), PULP_GATES_SETUP="",
                   PULP_TARTCI_BIN=str(self.bin / "tartci"),
                   PULP_SKIP_SOURCE_SELFTESTS="1")
        env.update(extra)
        return env

    def gates(self, **extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", str(self.root / "tools/scripts/gates.sh"), "main"], cwd=self.root,
                              env=self.env(**extra), capture_output=True, text=True, timeout=300)

    def configures(self) -> list[str]:
        return self.cmake_log.read_text().splitlines() if self.cmake_log.exists() else []

    def test_fixture_matches_the_shard_plan(self) -> None:
        sys.path.insert(0, str(REPO / "tools/ci"))
        import source_selftests  # noqa: PLC0415
        self.assertEqual(
            [sorted(names) for names in source_selftests.EXCLUSIVE_SHARD_PLAN],
            [sorted(n for n, shard, _ in EXCLUSIVE if shard == i)
             for i in range(1, source_selftests.EXCLUSIVE_SHARDS + 1)],
        )

    def test_touched_input_without_a_build_configures_build_gate_and_checks(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        calls = self.configures()
        self.assertEqual(len(calls), 1, r.stderr)
        self.assertIn(f"-B {self.root.resolve() / 'build-gate'}", calls[0])
        self.assertIn("-DPULP_BUILD_EXAMPLES=OFF", calls[0])
        self.assertNotIn("--build", calls[0])  # configure only, never a compile
        self.assertIn("script-test-inputs: OK", r.stderr)
        self.assertNotIn("[script-inputs] NOT CHECKED", r.stderr)

    def test_untouched_inputs_do_not_configure(self) -> None:
        self.write("README.md", "edited\n")
        self.commit()
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("nothing to check", r.stderr)

    def test_gate_host_refuses_heavy_lanes_and_says_where_to_run_them(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        for role in ("dedicated-builder", "dev-overflow", "a-role-tartci-adds-later"):
            with self.subTest(role=role):
                self.cmake_log.write_text("", encoding="utf-8")
                # Source selftests left enabled: the refusal, not the opt-out, must stop them.
                r = self.gates(FAKE_SERVES_GATE="1", FAKE_TARTCI_ROLE=role,
                               PULP_SKIP_SOURCE_SELFTESTS="")
                self.assertEqual(self.configures(), [])
                # The manifest lane still runs, build-free: its static families
                # prediction is a cheap gate that can fail.
                self.assertIn("gate host: build-free checks only, never configures", r.stderr)
                self.assertIn("changed-surface script families (static): OK", r.stderr)
                self.assertNotIn("▸ source-only selftests", r.stderr)
                self.assertNotIn("▸ ctest pr-fast tier", r.stderr)
                self.assertIn("[gate-host] REFUSED", r.stderr)
                self.assertIn(f"TARTCI_ROLE={role}", r.stderr)
                self.assertIn("TARTCI_ROLE=light host (m1) or a disposable VM", r.stderr)
                self.assertIn("Run tools/scripts/gates.sh main from", r.stderr)
                # The cheap gates still run, and the verdict is not a full pass.
                self.assertIn("▸ skill-sync check", r.stderr)
                self.assertIn("▸ version-bump check", r.stderr)
                self.assertIn("[gate-host] NOT CHECKED", r.stderr)
                self.assertNotIn("all gates pass — safe to push", r.stderr)

    def test_gate_host_still_fails_a_families_flip_without_configuring(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(FAKE_SERVES_GATE="1", FAKE_TARTCI_ROLE="dedicated-builder",
                       FAKE_STATIC_RC="1",
                       FAKE_STATIC_SAYS="  tools/scripts/test_alpha.py: newly mapped")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("tools/scripts/test_alpha.py: newly mapped", r.stderr)
        self.assertIn("one or more gates failed", r.stderr)

    def test_gate_host_summary_points_diff_coverage_off_the_host(self) -> None:
        self.write("README.md", "readme, edited\n")
        self.commit()
        r = self.gates(FAKE_SERVES_GATE="1", FAKE_TARTCI_ROLE="dev-overflow")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Diff-coverage NOT run, and refused on this gate host", r.stderr)
        self.assertNotIn("Diff-coverage NOT run (slow)", r.stderr)

    def test_light_and_non_gate_profiles_keep_local_build_path(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        for serves, role in (("1", "light"), ("0", "dedicated-builder")):
            with self.subTest(serves=serves, role=role):
                r = self.gates(FAKE_SERVES_GATE=serves, FAKE_TARTCI_ROLE=role)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("▸ generated test manifests", r.stderr)
                self.assertNotIn("[gate-host]", r.stderr)
        # The first run configured build-gate; the second reused it.
        self.assertEqual(len(self.configures()), 1)

    def test_a_current_configured_build_is_reused(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        subprocess.run([str(self.bin / "cmake"), "-S", str(self.root), "-B", str(self.root / "build-gate")],
                       check=True, env=self.env(FAKE_CMAKE_LOG=str(self.tmp / "pre.log")))
        query = self.root / "build-gate/.cmake/api/v1/query/codemodel-v2"
        query.parent.mkdir(parents=True)
        query.touch()
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("script-test-inputs: OK", r.stderr)

    def test_the_lane_accepts_the_build_dir_flag_its_merge_tree_caller_passes(self) -> None:
        # merge_tree_script_state.py runs this lane with `--build-dir <configured>`
        # rather than through gates.sh. The lane must take that flag as the same
        # declaration PULP_GATES_BUILD_DIR makes: check that build, configure
        # nothing. It rejected the flag outright, which made every merge-tree
        # verdict on a host with a configured build read "stale against main".
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        elsewhere = self.root / "build-elsewhere"
        subprocess.run([str(self.bin / "cmake"), "-S", str(self.root), "-B", str(elsewhere)],
                       check=True, env=self.env(FAKE_CMAKE_LOG=str(self.tmp / "pre.log")))
        query = elsewhere / ".cmake/api/v1/query/codemodel-v2"
        query.parent.mkdir(parents=True)
        query.touch()
        r = subprocess.run([sys.executable, str(self.root / "tools/scripts/gates_script_inputs.py"),
                            "--base", "main", "--repo-root", str(self.root),
                            "--build-dir", str(elsewhere)],
                           cwd=self.root, env=self.env(), capture_output=True, text=True,
                           encoding="utf-8", timeout=300)
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, out)
        self.assertNotIn("unrecognized arguments", out)
        self.assertEqual(self.configures(), [])
        self.assertIn(str(elsewhere), out)
        self.assertIn("script-test-inputs: OK", out)

    def test_a_reused_build_without_a_codemodel_query_is_reconfigured_once(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        subprocess.run([str(self.bin / "cmake"), "-S", str(self.root), "-B", str(self.root / "build-gate")],
                       check=True, env=self.env(FAKE_CMAKE_LOG=str(self.tmp / "pre.log")))
        r = self.gates()
        self.assertEqual(r.returncode, 0, r.stderr)
        calls = self.configures()
        self.assertEqual(len(calls), 1, r.stderr)
        self.assertNotIn("-G", calls[0].split())  # a reconfigure of the same tree, not a fresh one
        self.assertIn("changed-surface script families: OK", r.stderr)
        self.assertNotIn("NOT CHECKED", r.stderr)

    def test_opt_out_keeps_not_checked_and_says_so_loudly(self) -> None:
        self.write("test/CMakeLists.txt", "# registrations, edited\n")
        self.commit()
        r = self.gates(PULP_GATES_NO_CONFIGURE="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("!!! PULP_GATES_NO_CONFIGURE=1", r.stderr)
        for test in ("script-test-inputs-drift", "source-selftest-lane-contract",
                     "changed-surface-script-families-drift"):
            self.assertIn(f"[script-inputs] NOT CHECKED locally: {test}", r.stderr)
        self.assertIn("PASSED WITH 3 NOT CHECKED", r.stderr)
        self.assertNotIn("(static)", r.stderr)  # no families surface touched, no prediction

    def test_opt_out_on_a_families_surface_fails_instead_of_not_checked(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(PULP_GATES_NO_CONFIGURE="1")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(self.configures(), [])
        self.assertIn("changed-surface script families (static): OK", r.stderr)
        self.assertIn("changed-surface script families: FAILED, not verified", r.stderr)
        self.assertIn("python3 tools/scripts/changed_surface_script_families.py --build-dir <dir> --write",
                      r.stderr)
        self.assertNotIn("NOT CHECKED locally: changed-surface-script-families-drift", r.stderr)
        for test in ("script-test-inputs-drift", "source-selftest-lane-contract"):
            self.assertIn(f"[script-inputs] NOT CHECKED locally: {test}", r.stderr)

    def test_static_prediction_of_a_flip_is_reported_before_the_configured_check(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(PULP_GATES_NO_CONFIGURE="1", FAKE_STATIC_RC="1",
                       FAKE_STATIC_SAYS="  tools/scripts/test_alpha.py: newly mapped")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("tools/scripts/test_alpha.py: newly mapped", r.stderr)
        self.assertIn("the build-free prediction above found a mapping flip", r.stderr)

    def test_the_configured_check_overrides_a_wrong_static_prediction(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(FAKE_STATIC_RC="1", FAKE_STATIC_SAYS="  tools/scripts/test_alpha.py: newly mapped")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.configures()), 1)
        self.assertIn("the configured check decides", r.stderr)

    def test_families_surface_mirrors_the_generators_blocks_on(self) -> None:
        sys.path.insert(0, str(HERE))
        sys.path.insert(0, str(HERE.parent / "ci"))
        import changed_surface_script_families as generator
        import gates_script_inputs as lane
        sample = {".shipyard/config.toml", ".shipyard/changed-surface-families.toml",
                  "test/ctest_script_inputs.json", "tools/scripts/changed_surface_script_families.py",
                  "tools/scripts/new_tool.py", "tools/scripts/sub/dir.py", "tools/scripts/run.sh",
                  ".agents/skills/ci/SKILL.md", ".agents/skills/ci/notes.md", "core/x.cpp",
                  "tools/ci/thing.py", "test/CMakeLists.txt"}
        self.assertEqual(set(lane.families_surface(sample)),
                         {p for p in sample if generator.blocks_on(p)})

    def test_hand_edited_list_fails_and_prints_the_write_command(self) -> None:
        self.write("tools/scripts/test_alpha.py", "import alpha_lib\nprint('alpha')\n")
        self.commit("add an import without regenerating the list")
        r = self.gates()
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual(len(self.configures()), 1)
        self.assertIn("stale entry: alpha", r.stderr)
        self.assertIn("python3 tools/scripts/script_test_inputs.py --build-dir build-gate --write",
                      r.stderr)


    def test_selftest_added_without_regenerating_the_manifest_fails(self) -> None:
        # The registration gains TIMEOUT 300; the hand-kept manifest still says 120.
        inventory = json.loads(self.inventory.read_text())
        inventory["tests"][1]["properties"][1]["value"] = 300.0
        self.inventory.write_text(json.dumps(inventory), encoding="utf-8")
        self.write("tools/ci/test_beta.py", "print('beta, slower')\n")
        self.commit("change a selftest's budget without regenerating the manifest")
        r = self.gates()
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("beta: TIMEOUT 300.0 != manifest 120.0", r.stderr)
        self.assertIn("python3 tools/ci/source_selftests.py write --build-dir build-gate", r.stderr)

    def test_family_drift_fails_with_the_write_command(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(FAKE_FAMILIES_RC="1",
                       FAKE_FAMILIES_SAYS="  tools/scripts/test_alpha.py: newly mapped")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("tools/scripts/test_alpha.py: newly mapped", r.stderr)
        self.assertIn("python3 tools/scripts/changed_surface_script_families.py --build-dir build-gate --write",
                      r.stderr)

    def test_a_families_skip_on_a_families_surface_fails_not_passes(self) -> None:
        self.write("tools/scripts/test_alpha.py", "print('alpha, edited')\n")
        self.commit()
        r = self.gates(FAKE_FAMILIES_RC="77", FAKE_FAMILIES_SAYS="SKIP: no codemodel reply")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("FAILED, not verified (changed_surface_script_families.py --check exited 77", r.stderr)
        self.assertNotIn("NOT CHECKED locally: changed-surface-script-families-drift", r.stderr)


class TouchReasonsTests(unittest.TestCase):
    """The configure decision itself, without gates.sh."""

    def setUp(self) -> None:
        sys.path.insert(0, str(HERE))
        import script_test_inputs
        self.sti = script_test_inputs
        self.tmp = Path(tempfile.mkdtemp(prefix="touch-reasons-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        root = self.root = self.tmp
        for rel, text in {"tools/lib/a.py": "x\n", "tools/lib/data.json": "{}\n",
                          "tools/test_a.py": "x\n", "core/x.cpp": "x\n",
                          "core/CMakeLists.txt": "add_library(x)\n",
                          "tools/CMakeLists.txt": "add_test(NAME a COMMAND a)\n"}.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        lst = {"tests": {"a": {"entry": "tools/test_a.py", "inputs": ["tools/lib", "tools/test_a.py"],
                               "kind": "python"}}}
        (root / "test").mkdir()
        (root / "test/ctest_script_inputs.json").write_text(json.dumps(lst))
        git(root, "init", "-q", "-b", "main")
        git(root, "config", "user.email", "t@example.com")
        git(root, "config", "user.name", "t")
        git(root, "add", ".")
        git(root, "commit", "-q", "-m", "base")

    def reasons_after(self, rel: str) -> list[str]:
        (self.root / rel).write_text("changed\n" + ("add_test(\n" if rel == "tools/CMakeLists.txt" else ""))
        return self.sti.touch_reasons(self.root, "main")

    def test_each_path_class(self) -> None:
        self.assertTrue(self.reasons_after("tools/test_a.py"))            # declared entry (uncommitted)
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("tools/lib/a.py"))             # script under an input dir
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("tools/lib/data.json"), [])   # data under an input dir
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("core/x.cpp"), [])
        git(self.root, "checkout", "-q", ".")
        self.assertEqual(self.reasons_after("core/CMakeLists.txt"), [])   # registers no tests
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("tools/CMakeLists.txt"))       # registers tests
        git(self.root, "checkout", "-q", ".")
        self.assertTrue(self.reasons_after("test/ctest_script_inputs.json"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
