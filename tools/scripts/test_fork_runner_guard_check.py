#!/usr/bin/env python3
"""fork_runner_guard_check.py flags every way a fork pull request could take a variable-selected runner."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("guard", HERE / "fork_runner_guard_check.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)

GUARD = "(github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository)"
HOSTED = {"PULP_PREAMBLE_RUNS_ON_JSON"}


def check(runs_on: str, on: str = "pull_request", steps: str = "      - run: make\n") -> list[str]:
    text = f"name: x\non:\n  {on}:\njobs:\n  build:\n    runs-on: {runs_on}\n    steps:\n{steps}"
    path = Path(tempfile.mkdtemp(prefix="frg-")) / "x.yml"
    path.write_text(text, encoding="utf-8")
    return guard.findings(path, HOSTED)


class RunsOn(unittest.TestCase):
    def test_an_unguarded_variable_is_flagged(self):
        self.assertTrue(check("""${{ fromJSON(vars.PULP_LOCAL_GCC_RUNS_ON_JSON || '"ubuntu-latest"') }}"""))

    def test_a_fork_flag_check_is_not_a_guard(self):
        self.assertTrue(check("""${{ fromJSON(!github.event.pull_request.head.repo.fork && vars.PULP_LOCAL_GCC_RUNS_ON_JSON || '"ubuntu-latest"') }}"""))

    def test_the_guard_dispatch_only_and_hosted_lanes_pass(self):
        for runs_on in (f"""${{{{ fromJSON({GUARD} && vars.PULP_LOCAL_GCC_RUNS_ON_JSON || '"ubuntu-latest"') }}}}""",
                        """${{ fromJSON(github.event_name == 'workflow_dispatch' && vars.PULP_LOCAL_LINUX_RUNS_ON_JSON || '"ubuntu-latest"') }}""",
                        """${{ fromJSON(vars.PULP_PREAMBLE_RUNS_ON_JSON || '"ubuntu-latest"') }}""", "ubuntu-latest"):
            with self.subTest(runs_on=runs_on[:60]):
                self.assertEqual(check(runs_on), [])

    def test_a_guard_on_one_branch_does_not_cover_another(self):
        mixed = (f"""${{{{ fromJSON({GUARD} && vars.PULP_LOCAL_GCC_RUNS_ON_JSON || """
                 """vars.PULP_LOCAL_IWYU_RUNS_ON_JSON || '"ubuntu-latest"') }}""")
        found = check(mixed)
        self.assertTrue(found)
        self.assertIn("PULP_LOCAL_IWYU_RUNS_ON_JSON", found[0])
        self.assertNotIn("PULP_LOCAL_GCC_RUNS_ON_JSON", found[0])

    def test_workflows_a_pull_request_cannot_start_are_ignored(self):
        self.assertEqual(check("""${{ fromJSON(vars.PULP_LOCAL_GCC_RUNS_ON_JSON || '"ubuntu-latest"') }}""", on="push"), [])


class ResolverCalls(unittest.TestCase):
    call = "      - run: python3 tools/scripts/resolve_runs_on.py --target-name x --mode default --default-label macos-15\n"

    def test_a_resolver_call_needs_the_head_repository(self):
        self.assertTrue(check("ubuntu-latest", steps=self.call))
        with_env = ("      - env:\n          PR_HEAD_REPO: ${{ github.event.pull_request.head.repo.full_name }}\n"
                    "        run: python3 tools/scripts/resolve_runs_on.py --target-name x\n")
        self.assertEqual(check("ubuntu-latest", steps=with_env), [])


class TheTree(unittest.TestCase):
    def test_every_workflow_in_the_repository_passes(self):
        hosted = guard.hosted_lane_variables()
        found = [f for p in sorted(guard.WORKFLOWS.glob("*.yml")) for f in guard.findings(p, hosted)]
        self.assertEqual(found, [])
        # Control: the walk saw the pull-request workflows this guard exists for.
        names = {p.name for p in guard.WORKFLOWS.glob("*.yml")}
        self.assertTrue({"build.yml", "drift-fast.yml", "sanitizers.yml"} <= names)


if __name__ == "__main__":
    unittest.main()
