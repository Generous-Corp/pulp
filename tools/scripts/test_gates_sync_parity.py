#!/usr/bin/env python3
"""gates.sh must run every check the required sync job and drift-fast run.

`Enforce version & skill sync` (.github/workflows/version-skill-check.yml) and
`drift-fast` (.github/workflows/drift-fast.yml) failed on pull requests that
tools/scripts/gates.sh had called clean, because they ran commands gates.sh did
not. gates.sh now reads the sync job's commands from the workflow
(`source_selftests.py run --sync-job`) and the drift-fast selection from
tools/ci/drift_fast.json (`--drift-fast`), so a new step reaches it with no
edit here. This test fails when that stops being true:

* a command in the sync job that is neither run by the lane, nor listed in
  tools/ci/sync_job_parity.json as run by a block of gates.sh itself, nor
  listed there as CI-only;
* a manifest entry for a command the workflow no longer has;
* a block of gates.sh that lost the command the manifest says it runs, or that
  runs it without one of CI's flags (unless the manifest says why);
* a lane whose arguments are not CI's, or that stopped being wired in;
* a drift-fast selection gates.sh cannot resolve.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ci"))
import source_selftests as lane  # noqa: E402

GATES = ROOT / "tools" / "scripts" / "gates.sh"
WORKFLOW = ROOT / ".github" / "workflows" / "version-skill-check.yml"
PARITY = ROOT / "tools" / "ci" / "sync_job_parity.json"
DRIFT_FAST_MANIFEST = ROOT / "tools" / "ci" / "drift_fast.json"
BASE = "0123456789abcdef0123456789abcdef01234567"


def gates_commands(text: str) -> str:
    """gates.sh as one searchable string: no comments or echo, continuations joined."""
    kept = [line for line in text.splitlines()
            if not line.lstrip().startswith("#") and not line.lstrip().startswith("echo ")]
    return re.sub(r"\s+", " ", re.sub(r"\\\n", " ", "\n".join(kept)))


def flags(args: list[str]) -> list[str]:
    return [a for a in args if a.startswith("--")]


class SyncJobParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.commands = lane.sync_job_commands(WORKFLOW.read_text(encoding="utf-8"))
        cls.manifest = json.loads(PARITY.read_text(encoding="utf-8"))
        cls.gates = gates_commands(GATES.read_text(encoding="utf-8"))
        cls.native = cls.manifest["gates_native"]
        cls.ci_only = cls.manifest["ci_only"]

    def test_the_job_has_commands(self) -> None:
        # A parser that finds nothing would pass every test below.
        self.assertGreater(len(self.commands), 40)

    def test_every_command_is_run_native_ci_only_or_by_the_lane(self) -> None:
        entries = lane.sync_job_entries(BASE, "a title")
        run_by_lane = {e["name"] for e in entries}
        lane_keys = {c["key"].replace("{base}", BASE).replace("{pr_title}", "a title")
                     for c in self.commands
                     if c["key"] not in self.native and c["key"] not in self.ci_only
                     and (ROOT / c["script"]).is_file()}
        self.assertEqual(run_by_lane, lane_keys)
        for command in self.commands:
            key = command["key"]
            listed = (key in self.native) + (key in self.ci_only)
            self.assertLessEqual(listed, 1, f"{key} is in both lists of the parity manifest")
            if listed == 0:
                self.assertTrue(command["resolved"], key)

    def test_the_lane_runs_the_listed_gate_tests_and_sync_checks(self) -> None:
        names = {e["name"] for e in lane.sync_job_entries(BASE, "a title")}
        for expected in (
            "tools/scripts/docs_sync_check.py --base {b} --config tools/scripts/docs_sync_map.json --mode=report",
            "tools/scripts/screenshot_sync_check.py --base {b} --mode=report",
            "tools/scripts/check_cli_mcp_parity.py --mode=report --no-color",
            "tools/import-validation/check-source-contracts.py --strict",
            "tools/import-validation/test_source_contracts.py",
            "tools/scripts/test_ci_diff_base.py",
            "tools/scripts/test_intel_canary_lint.py",
            "tools/scripts/pr_release_tag_report.py --base {b} --head HEAD --guard-head HEAD --pr-title a title",
        ):
            self.assertIn(expected.replace("{b}", BASE), names)

    def test_the_lane_uses_ci_arguments_and_marks_advisory_steps(self) -> None:
        by_script = {e["script"]: e for e in lane.sync_job_entries(BASE, "a title")}
        docs = by_script["tools/scripts/docs_sync_check.py"]
        self.assertTrue(docs["advisory"])  # continue-on-error in the workflow
        self.assertEqual(docs["argv"][1:], ["--base", BASE, "--config",
                                            "tools/scripts/docs_sync_map.json", "--mode=report"])
        self.assertFalse(by_script["tools/scripts/screenshot_sync_check.py"]["advisory"])
        tags = by_script["tools/scripts/pr_release_tag_report.py"]
        self.assertEqual(tags["argv"][1:], ["--base", BASE, "--head", "HEAD",
                                            "--guard-head", "HEAD", "--pr-title", "a title"])

    def test_no_manifest_entry_outlives_its_workflow_command(self) -> None:
        keys = {c["key"] for c in self.commands}
        for key in [*self.native, *self.ci_only]:
            self.assertIn(key, keys, f"{key} is no longer in version-skill-check.yml")
        scripts = {c["script"] for c in self.commands}
        for script in self.manifest["diff_scoped"]:
            self.assertIn(script, scripts)

    def test_every_exception_and_ci_only_entry_says_why(self) -> None:
        for key, why in self.ci_only.items():
            self.assertTrue(why.strip(), key)
        for key, spec in self.native.items():
            for flag, why in spec.get("flag_exceptions", {}).items():
                self.assertTrue(why.strip(), f"{key} {flag}")
        for script, why in self.manifest["diff_scoped"].items():
            self.assertTrue(why.strip(), script)

    def test_each_native_command_is_in_gates_with_every_ci_flag(self) -> None:
        by_key = {c["key"]: c for c in self.commands}
        for key, spec in self.native.items():
            fragments = spec["gates"]
            for fragment in fragments:
                self.assertTrue(
                    re.sub(r"\s+", " ", fragment) in self.gates,
                    f"gates.sh no longer runs `{fragment}` ({key})")
            joined = " ".join(fragments)
            ci_flags = flags(by_key[key]["args"]) if by_key[key]["resolved"] else \
                re.findall(r"--[\w-]+", key)
            for flag in ci_flags:
                if flag in spec.get("flag_exceptions", {}):
                    continue
                self.assertIn(flag.split("=")[0], joined,
                              f"gates.sh runs `{fragments[0]}` without CI's {flag} ({key})")

    def test_a_flag_exception_must_still_be_needed(self) -> None:
        by_key = {c["key"]: c for c in self.commands}
        for key, spec in self.native.items():
            for flag in spec.get("flag_exceptions", {}):
                self.assertIn(flag, by_key[key]["args"], f"{key}: stale exception {flag}")
                self.assertNotIn(flag, " ".join(spec["gates"]),
                                 f"{key}: gates.sh now passes {flag}; drop the exception")

    def test_gates_wires_the_lane_and_the_drift_selection(self) -> None:
        self.assertTrue('source_selftests.py" run --sync-job "$sync_job_base"' in self.gates,
                        "gates.sh does not run the sync-job lane")
        self.assertTrue("--drift-fast --build-dir auto" in self.gates,
                        "gates.sh does not run the drift-fast selection")
        # The lane is skipped, and reported, only where gates.sh already refuses
        # the source-selftest suites (a tartci gate host).
        text = GATES.read_text(encoding="utf-8")
        self.assertRegex(text, r'gate_host_skipped_what="[^"]*the required sync job')

    def test_version_bump_runs_the_way_ci_runs_it_on_a_pull_request(self) -> None:
        # --require-bump-for-fix-feat accepts only a bump commit or a skip
        # trailer, so paired with intent trailers it fails a PR CI passes.
        self.assertTrue("--accept-intent-trailers" in self.gates)
        self.assertTrue("--require-bump-for-fix-feat" not in self.gates)


class SyntheticWorkflowTests(unittest.TestCase):
    """A step added to the workflow reaches the lane; one it cannot resolve is refused."""

    WORKFLOW = """\
jobs:
  version-skill-check:
    steps:
      - name: Existing
        run: |
          python3 tools/scripts/kept_native.py --base "${{ steps.base.outputs.ref }}"
      - name: Brand new check
        continue-on-error: true
        run: |
          python3 tools/scripts/brand_new_check.py \\
              --base "${{ steps.base.outputs.ref }}" \\
              --mode=report
  other-job:
    steps:
      - run: |
          python3 tools/scripts/other_job_only.py
"""

    def _entries(self, workflow: str, native=("tools/scripts/kept_native.py --base {base}",)):
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            (root / "tools" / "scripts").mkdir(parents=True)
            for name in ("kept_native", "brand_new_check", "other_job_only", "unresolvable"):
                (root / "tools" / "scripts" / f"{name}.py").write_text("", encoding="utf-8")
            wf = root / "wf.yml"
            wf.write_text(workflow, encoding="utf-8")
            parity = root / "parity.json"
            parity.write_text(json.dumps({
                "gates_native": {key: {"gates": []} for key in native},
                "ci_only": {}, "diff_scoped": {}}), encoding="utf-8")
            return lane.sync_job_entries(BASE, "t", repo=root, workflow=wf, parity=parity)

    def test_a_new_step_is_run_with_its_own_arguments(self) -> None:
        entries = self._entries(self.WORKFLOW)
        self.assertEqual([e["script"] for e in entries], ["tools/scripts/brand_new_check.py"])
        self.assertEqual(entries[0]["argv"][1:], ["--base", BASE, "--mode=report"])
        self.assertTrue(entries[0]["advisory"])

    def test_a_listed_native_command_is_left_to_gates(self) -> None:
        self.assertEqual(self._entries(self.WORKFLOW, native=()) [0]["script"],
                         "tools/scripts/kept_native.py")

    def test_a_command_with_an_unresolvable_expression_is_refused(self) -> None:
        workflow = self.WORKFLOW.replace(
            'brand_new_check.py \\\n              --base "${{ steps.base.outputs.ref }}"',
            'unresolvable.py --event "$EVENT_NAME"')
        with self.assertRaises(lane.ManifestError):
            self._entries(workflow)


class DriftFastSelectionTests(unittest.TestCase):
    def test_gates_resolves_every_test_drift_fast_runs(self) -> None:
        data = json.loads(DRIFT_FAST_MANIFEST.read_text(encoding="utf-8"))
        entries, not_checked = lane.drift_fast_entries_from_source(ROOT)
        resolved = {e["name"] for e in entries} | set(not_checked)
        for test in data["tests"]:
            self.assertIn(test["name"], resolved,
                          f"{test['name']} is in drift_fast.json but gates.sh neither runs nor reports it")
        for label in data["ctest_labels"]:
            self.assertEqual(label, "pr-fast",
                             "gates.sh reads only the pr-fast label without a build; "
                             "teach drift_fast_entries_from_source the new one")
        self.assertTrue({m for m in lane.pr_fast_names(ROOT)} <= resolved)

    def test_a_listed_test_that_cannot_run_is_reported_not_dropped(self) -> None:
        entries, not_checked = lane.drift_fast_entries_from_source(ROOT)
        ran = {e["name"] for e in entries}
        self.assertTrue(ran.isdisjoint(not_checked))
        for name, why in not_checked.items():
            self.assertTrue(why.strip(), name)


if __name__ == "__main__":
    unittest.main()
