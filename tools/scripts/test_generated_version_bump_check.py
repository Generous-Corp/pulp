#!/usr/bin/env python3
"""Hostile fixtures for the generated version-bump CI fast path."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest


SCRIPT = Path(__file__).with_name("generated_version_bump_check.py")
ROOT = SCRIPT.parents[2]
SPEC = importlib.util.spec_from_file_location("generated_version_bump_check", SCRIPT)
assert SPEC and SPEC.loader
CHECK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CHECK
SPEC.loader.exec_module(CHECK)


def run(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=repo, check=True, text=True, capture_output=True
    ).stdout.strip()


class GeneratedVersionBumpCheckTest(unittest.TestCase):
    def test_merge_group_api_callers_have_explicit_pull_read_scope(self) -> None:
        for relative in (
            ".github/workflows/build.yml",
            ".github/workflows/wclap-cloudflare.yml",
        ):
            with self.subTest(workflow=relative):
                text = (ROOT / relative).read_text(encoding="utf-8")
                permissions = text.split("\npermissions:\n", 1)[1].split("\n\n", 1)[0]
                self.assertIn("  pull-requests: read", permissions)

    def setUp(self) -> None:
        self.holder = tempfile.TemporaryDirectory(prefix="pulp-generated-bump-test-")
        self.root = Path(self.holder.name)
        source = Path(__file__).resolve().parents[2]
        self.repo = self.root / "repo"
        subprocess.run(
            ["git", "clone", "--quiet", "--shared", str(source), str(self.repo)],
            check=True,
        )
        run(self.repo, "config", "user.name", CHECK.BOT_NAME)
        run(self.repo, "config", "user.email", CHECK.BOT_EMAIL)
        run(self.repo, "config", "commit.gpgsign", "false")
        self.base = run(self.repo, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.holder.cleanup()

    def _generated_commit(self, *, extra_path: str | None = None) -> str:
        scripts = self.repo / "tools" / "scripts"
        sys.path.insert(0, str(scripts))
        try:
            for name in (
                "version_at_land",
                "version_bump_surfaces",
                "version_bump_check",
                "version_bump_apply",
                "version_bump_heuristics",
                "version_bump_render",
            ):
                sys.modules.pop(name, None)
            version_at_land = __import__("version_at_land")
            surfaces = __import__("version_bump_surfaces")
            config = surfaces.load_config(scripts / "versioning.json")
            plugin = next(surface for surface in config.surfaces if surface.name == "plugin")
            current = surfaces.read_version(self.repo, plugin.version_files[0])
            assert current
            assigned = version_at_land.bump_version(current, "patch")
            plan = [version_at_land.Assignment("plugin", "patch", current, assigned)]
            edited = version_at_land._write_plan(self.repo, config, plan)
            subprocess.run(["git", "add", "--", *edited], cwd=self.repo, check=True)
            if extra_path:
                path = self.repo / extra_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("hostile extra byte\n", encoding="utf-8")
                subprocess.run(["git", "add", "--", extra_path], cwd=self.repo, check=True)
            message = version_at_land._bump_message(plan, self.base, self.base)
            subprocess.run(
                ["git", "commit", "--quiet", "--no-verify", "-m", message],
                cwd=self.repo,
                check=True,
            )
            return run(self.repo, "rev-parse", "HEAD")
        finally:
            sys.path.remove(str(scripts))

    def _inputs(self, candidate: str, *, branch: str = CHECK.BUMP_BRANCH,
                verified: bool = True,
                bot_login: str = CHECK.BOT_LOGIN) -> types.SimpleNamespace:
        event = {
            "repository": {"full_name": "Generous-Corp/pulp"},
            "pull_request": {
                "state": "open",
                "title": CHECK.BUMP_SUBJECT,
                "commits": 1,
                "head": {
                    "ref": branch,
                    "sha": candidate,
                    "repo": {"full_name": "Generous-Corp/pulp"},
                },
                "base": {"ref": "main", "sha": self.base},
            },
        }
        metadata = {
            "sha": candidate,
            "author": {"login": bot_login},
            "committer": {"login": bot_login},
            "commit": {
                "author": {"name": CHECK.BOT_NAME, "email": CHECK.BOT_EMAIL},
                "committer": {"name": CHECK.BOT_NAME, "email": CHECK.BOT_EMAIL},
                "verification": {
                    "verified": verified,
                    "reason": "valid" if verified else "unsigned",
                    "signature": "-----BEGIN SSH SIGNATURE-----\nfixture",
                },
            },
        }
        event_path = self.root / "event.json"
        commit_path = self.root / "commit.json"
        event_path.write_text(json.dumps(event), encoding="utf-8")
        commit_path.write_text(json.dumps(metadata), encoding="utf-8")
        return types.SimpleNamespace(
            repo=self.repo,
            event_name="pull_request",
            event_path=event_path,
            base=self.base,
            head=candidate,
            commit_json=commit_path,
            pulls_json=None,
            pull_json=None,
        )

    def _cumulative_inputs(
        self,
        candidate: str,
        *,
        extra_group_path: str | None = None,
        change_writer_path: str | None = None,
    ) -> types.SimpleNamespace:
        """Model #7771: a generated bump queued behind one proven PR.

        The real group was 439bee29 with parents 93b8a3bd (the prior #7734
        group) and 813a90bd (the signed bump, based on older 171cd61b).  This
        hermetic fixture preserves that exact topology without depending on
        live GitHub history.
        """
        run(self.repo, "checkout", "--quiet", "--detach", self.base)
        prior = self.repo / "docs" / "prior-queue-entry.md"
        prior.parent.mkdir(parents=True, exist_ok=True)
        prior.write_text("prior queue entry\n", encoding="utf-8")
        subprocess.run(["git", "add", "--", str(prior.relative_to(self.repo))],
                       cwd=self.repo, check=True)
        if change_writer_path:
            writer = self.repo / change_writer_path
            writer.write_text(writer.read_text(encoding="utf-8") + "\n# queue drift\n",
                              encoding="utf-8")
            subprocess.run(["git", "add", "--", str(writer.relative_to(self.repo))],
                           cwd=self.repo, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-verify", "-m", "prior queue entry"],
            cwd=self.repo,
            check=True,
        )
        prior_candidate = run(self.repo, "rev-parse", "HEAD")
        prior_tree = run(self.repo, "rev-parse", f"{prior_candidate}^{{tree}}")
        cumulative_base = subprocess.run(
            [
                "git",
                "commit-tree",
                prior_tree,
                "-p",
                self.base,
                "-p",
                prior_candidate,
            ],
            cwd=self.repo,
            check=True,
            text=True,
            input="prior queue group\n",
            capture_output=True,
        ).stdout.strip()
        run(self.repo, "checkout", "--quiet", "--detach", cumulative_base)
        subprocess.run(
            ["git", "merge", "--quiet", "--no-commit", "--no-ff", candidate],
            cwd=self.repo,
            check=True,
        )
        if extra_group_path:
            hostile = self.repo / extra_group_path
            hostile.parent.mkdir(parents=True, exist_ok=True)
            hostile.write_text("hostile cumulative byte\n", encoding="utf-8")
            subprocess.run(["git", "add", "--", extra_group_path], cwd=self.repo,
                           check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-verify", "-m", "queue group"],
            cwd=self.repo,
            check=True,
        )
        group = run(self.repo, "rev-parse", "HEAD")

        inputs = self._inputs(candidate)
        pull = json.loads(inputs.event_path.read_text(encoding="utf-8"))["pull_request"]
        pull["number"] = 7771
        inputs.event_name = "merge_group"
        inputs.base = cumulative_base
        inputs.head = group
        inputs.event_path.write_text(
            json.dumps(
                {
                    "repository": {"full_name": "Generous-Corp/pulp"},
                    "merge_group": {
                        "base_sha": cumulative_base,
                        "head_sha": group,
                    },
                }
            ),
            encoding="utf-8",
        )
        inputs.pulls_json = self.root / "pulls.json"
        inputs.pulls_json.write_text(json.dumps([[{"number": 7771}]]),
                                     encoding="utf-8")
        inputs.pull_json = self.root / "pull.json"
        inputs.pull_json.write_text(json.dumps(pull), encoding="utf-8")
        return inputs

    def test_exact_generated_transaction_is_accepted(self) -> None:
        candidate = self._generated_commit()
        result = CHECK.verify(self._inputs(candidate))
        self.assertTrue(result["generated_version_bump"])
        self.assertEqual(result["candidate"], candidate)
        self.assertEqual(result["surfaces"], ["plugin"])

    def test_killer_extra_source_byte_fails_exact_tree_reproduction(self) -> None:
        # This candidate satisfies bot identity, signature metadata, branch,
        # one-commit topology, marker, and every generated version edit. The
        # exact-tree guard is the only thing preventing the hostile source byte
        # from riding the fast path; deleting that guard must break this test.
        candidate = self._generated_commit(extra_path="core/hostile.cpp")
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "byte regeneration"):
            CHECK.verify(self._inputs(candidate))

    def test_unsigned_candidate_falls_back_before_regeneration(self) -> None:
        candidate = self._generated_commit()
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "signature"):
            CHECK.verify(self._inputs(candidate, verified=False))

    def test_wrong_signed_account_falls_back_before_regeneration(self) -> None:
        candidate = self._generated_commit()
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "release-bot account"):
            CHECK.verify(self._inputs(candidate, bot_login="not-the-release-bot"))

    def test_second_commit_is_not_a_single_base_transaction(self) -> None:
        candidate = self._generated_commit()
        (self.repo / "docs" / "second.md").write_text("second\n", encoding="utf-8")
        subprocess.run(["git", "add", "docs/second.md"], cwd=self.repo, check=True)
        subprocess.run(
            ["git", "commit", "--quiet", "--no-verify", "-m", CHECK.BUMP_SUBJECT],
            cwd=self.repo,
            check=True,
        )
        second = run(self.repo, "rev-parse", "HEAD")
        inputs = self._inputs(second)
        metadata = json.loads(inputs.commit_json.read_text(encoding="utf-8"))
        metadata["sha"] = second
        inputs.commit_json.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "one-commit fast-forward"):
            CHECK.verify(inputs)
        self.assertNotEqual(candidate, second)

    def test_branch_name_is_not_optional_provenance(self) -> None:
        candidate = self._generated_commit()
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "head ref/SHA"):
            CHECK.verify(self._inputs(candidate, branch="topic/not-the-bot-lock"))

    def test_malformed_paginated_pull_response_fails_closed(self) -> None:
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "non-array page"):
            CHECK._flatten_pages([[{"number": 1}], {"number": 2}])

    def test_unpinned_derived_generator_fails_closed(self) -> None:
        module = types.SimpleNamespace(
            _DERIVED_REGENERATORS=[
                ("docs/status/hostile.json", ["python3", "tools/scripts/hostile.py"])
            ]
        )
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "not pinned"):
            CHECK._bind_trusted_regenerators(module, self.repo)

    def test_merge_group_uses_detailed_pull_and_rejects_mixed_topology(self) -> None:
        candidate = self._generated_commit()
        tree = run(self.repo, "rev-parse", f"{candidate}^{{tree}}")
        group = subprocess.run(
            ["git", "commit-tree", tree, "-p", self.base, "-p", candidate],
            cwd=self.repo,
            check=True,
            text=True,
            input="exact group\n",
            capture_output=True,
        ).stdout.strip()
        inputs = self._inputs(candidate)
        pull = json.loads(inputs.event_path.read_text(encoding="utf-8"))["pull_request"]
        pull["number"] = 99
        inputs.event_name = "merge_group"
        inputs.head = group
        inputs.event_path.write_text(
            json.dumps(
                {
                    "repository": {"full_name": "Generous-Corp/pulp"},
                    "merge_group": {"base_sha": self.base, "head_sha": group},
                }
            ),
            encoding="utf-8",
        )
        inputs.pulls_json = self.root / "pulls.json"
        inputs.pulls_json.write_text(json.dumps([[{"number": 99}]]), encoding="utf-8")
        inputs.pull_json = self.root / "pull.json"
        inputs.pull_json.write_text(json.dumps(pull), encoding="utf-8")
        self.assertTrue(CHECK.verify(inputs)["generated_version_bump"])

        side = subprocess.run(
            ["git", "commit-tree", tree, "-p", self.base],
            cwd=self.repo,
            check=True,
            text=True,
            input="side\n",
            capture_output=True,
        ).stdout.strip()
        mixed_group = subprocess.run(
            [
                "git",
                "commit-tree",
                tree,
                "-p",
                self.base,
                "-p",
                candidate,
                "-p",
                side,
            ],
            cwd=self.repo,
            check=True,
            text=True,
            input="mixed group\n",
            capture_output=True,
        ).stdout.strip()
        event = {
            "repository": {"full_name": "Generous-Corp/pulp"},
            "merge_group": {"base_sha": self.base, "head_sha": mixed_group},
        }
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "mixed, nested"):
            CHECK._event_commits(
                self.repo,
                "merge_group",
                event,
                explicit_base=self.base,
                explicit_head=mixed_group,
            )

    def test_7771_cumulative_queue_topology_is_replayed_exactly(self) -> None:
        candidate = self._generated_commit()
        result = CHECK.verify(self._cumulative_inputs(candidate))
        self.assertTrue(result["generated_version_bump"])
        self.assertEqual(result["candidate"], candidate)

    def test_cumulative_group_rejects_extra_tree_byte(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(
            candidate, extra_group_path="core/hostile-cumulative.cpp"
        )
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "cumulative projection"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_swapped_parents(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(candidate)
        tree = run(self.repo, "rev-parse", f"{inputs.head}^{{tree}}")
        swapped = subprocess.run(
            [
                "git",
                "commit-tree",
                tree,
                "-p",
                candidate,
                "-p",
                inputs.base,
            ],
            cwd=self.repo,
            check=True,
            text=True,
            input="swapped group\n",
            capture_output=True,
        ).stdout.strip()
        event = json.loads(inputs.event_path.read_text(encoding="utf-8"))
        event["merge_group"]["head_sha"] = swapped
        inputs.event_path.write_text(json.dumps(event), encoding="utf-8")
        inputs.head = swapped
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "mixed, nested"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_multiple_associated_pulls(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(candidate)
        inputs.pulls_json.write_text(
            json.dumps([[{"number": 7771}, {"number": 7772}]]),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "exactly one pull"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_stale_pull_base_record(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(candidate)
        pull = json.loads(inputs.pull_json.read_text(encoding="utf-8"))
        pull["base"]["sha"] = inputs.base
        inputs.pull_json.write_text(json.dumps(pull), encoding="utf-8")
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "immutable protected main"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_unknown_ancestry(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(candidate)
        root_tree = run(self.repo, "rev-parse", f"{inputs.base}^{{tree}}")
        unrelated = subprocess.run(
            ["git", "commit-tree", root_tree],
            cwd=self.repo,
            check=True,
            text=True,
            input="unrelated cumulative base\n",
            capture_output=True,
        ).stdout.strip()
        group_tree = run(self.repo, "rev-parse", f"{inputs.head}^{{tree}}")
        group = subprocess.run(
            ["git", "commit-tree", group_tree, "-p", unrelated, "-p", candidate],
            cwd=self.repo,
            check=True,
            text=True,
            input="unknown ancestry group\n",
            capture_output=True,
        ).stdout.strip()
        event = json.loads(inputs.event_path.read_text(encoding="utf-8"))
        event["merge_group"] = {"base_sha": unrelated, "head_sha": group}
        inputs.event_path.write_text(json.dumps(event), encoding="utf-8")
        inputs.base = unrelated
        inputs.head = group
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "one exact prior group"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_writer_drift_ahead(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(
            candidate,
            change_writer_path="tools/scripts/version_at_land.py",
        )
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "writer changed"):
            CHECK.verify(inputs)

    def test_cumulative_group_rejects_derived_generator_drift_ahead(self) -> None:
        candidate = self._generated_commit()
        inputs = self._cumulative_inputs(
            candidate,
            change_writer_path="tools/scripts/pulp_tooling_disposition.py",
        )
        with self.assertRaisesRegex(CHECK.NotGeneratedBump, "writer changed"):
            CHECK.verify(inputs)


if __name__ == "__main__":
    unittest.main()
