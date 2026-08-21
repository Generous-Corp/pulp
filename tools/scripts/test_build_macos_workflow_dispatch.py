#!/usr/bin/env python3
"""Execute the PR resolver embedded in the macOS retarget workflow."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-macos.yml"
MACOS_COMMAND = REPO_ROOT / "tools" / "cli" / "cmd_macos.cpp"
BASE_SHA = "a" * 40
HEAD_SHA = "b" * 40


def _pr(*, number: int = 7723, state: str = "open",
        base_repo: str = "Generous-Corp/pulp", base_ref: str = "main",
        head_repo: str = "Generous-Corp/pulp",
        head_ref: str = "repair/security-7723",
        base_sha: str = BASE_SHA, head_sha: str = HEAD_SHA) -> dict[str, object]:
    return {
        "number": number,
        "state": state,
        "base": {"sha": base_sha, "ref": base_ref,
                 "repo": {"full_name": base_repo}},
        "head": {"sha": head_sha, "ref": head_ref,
                 "repo": {"full_name": head_repo}},
    }


def _resolver_script() -> str:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    document = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    steps = document["jobs"]["resolve-runner"]["steps"]
    step = next((item for item in steps if item.get("id") == "pr"), None)
    if step is None:
        raise AssertionError("build-macos.yml lost the exact PR resolver")
    return step["run"]


def _run(*, explicit: str = "7723", target_ref: str = "repair/security-7723",
         workflow_ref: str = "main",
         detail: dict[str, object] | None = None,
         matches: list[dict[str, object]] | None = None) -> tuple[int, dict[str, str], str]:
    detail = detail if detail is not None else _pr()
    matches = matches if matches is not None else [_pr()]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        fixtures = root / "fixtures"
        fixtures.mkdir()
        (fixtures / "detail.json").write_text(json.dumps(detail), encoding="utf-8")
        (fixtures / "matches.json").write_text(json.dumps(matches), encoding="utf-8")
        fake_gh = fake_bin / "gh"
        fake_gh.write_text(
            "#!/bin/sh\n"
            "case \"$*\" in\n"
            "  */pulls/[0-9]*) cat \"$FIXTURES/detail.json\" ;;\n"
            "  *) cat \"$FIXTURES/matches.json\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        fake_gh.chmod(0o755)
        output = root / "output"
        output.write_text("", encoding="utf-8")
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("GITHUB_")}
        env.update({
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "FIXTURES": str(fixtures),
            "GITHUB_OUTPUT": str(output),
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "GITHUB_REPOSITORY_OWNER": "Generous-Corp",
            "INPUT_PR_NUMBER": explicit,
            "TARGET_REF": target_ref,
            "WORKFLOW_REF": workflow_ref,
            "GH_TOKEN": "test-token",
        })
        process = subprocess.run(
            ["bash", "-c", _resolver_script()], cwd=root, env=env,
            capture_output=True, text=True,
        )
        parsed = dict(
            line.split("=", 1) for line in output.read_text().splitlines()
            if "=" in line
        )
        return process.returncode, parsed, process.stderr


class BuildMacosWorkflowDispatchTests(unittest.TestCase):
    def test_explicit_pr_pins_exact_event_base_and_head(self) -> None:
        rc, output, _ = _run()
        self.assertEqual(rc, 0)
        self.assertEqual(output, {
            "number": "7723", "base_sha": BASE_SHA, "head_sha": HEAD_SHA,
        })

    def test_target_ref_resolves_one_open_pr_when_number_is_omitted(self) -> None:
        rc, output, _ = _run(explicit="")
        self.assertEqual(rc, 0)
        self.assertEqual(output["number"], "7723")

    def test_target_ref_must_resolve_exactly_one_pr(self) -> None:
        for matches in ([], [_pr(number=1), _pr(number=2)]):
            with self.subTest(count=len(matches)):
                rc, output, error = _run(explicit="", matches=matches)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected exactly one", error)

    def test_closed_pr_is_rejected(self) -> None:
        rc, output, error = _run(detail=_pr(state="closed"))
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("closed", error)

    def test_untrusted_base_is_rejected(self) -> None:
        for detail in (_pr(base_repo="attacker/pulp"), _pr(base_ref="develop")):
            with self.subTest(base=detail["base"]):
                rc, output, error = _run(detail=detail)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected Generous-Corp/pulp:main", error)

    def test_wrong_head_repository_or_ref_is_rejected(self) -> None:
        for detail in (_pr(head_repo="attacker/pulp"), _pr(head_ref="other")):
            with self.subTest(head=detail["head"]):
                rc, output, error = _run(detail=detail)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected Generous-Corp/pulp:repair/security-7723", error)

    def test_non_numeric_pr_input_is_rejected_before_api_lookup(self) -> None:
        rc, output, error = _run(explicit="7723/merge")
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("decimal digits", error)

    def test_non_full_sha_is_rejected(self) -> None:
        rc, output, error = _run(detail=_pr(base_sha="abc"))
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("40-character", error)

    def test_non_main_workflow_definition_is_rejected(self) -> None:
        rc, output, error = _run(workflow_ref="repair/security-7723")
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("protected main", error)

    def test_cli_dispatches_the_trusted_workflow_with_explicit_pr_identity(self) -> None:
        source = MACOS_COMMAND.read_text(encoding="utf-8")
        self.assertIn('" --ref main"', source)
        self.assertIn('" --field pr_number=" + shell_quote(pr_number)', source)
        self.assertNotIn('" --ref " + shell_quote(head_ref)', source)


if __name__ == "__main__":
    unittest.main()
