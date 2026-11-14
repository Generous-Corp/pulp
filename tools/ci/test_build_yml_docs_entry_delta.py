#!/usr/bin/env python3
"""Contract tests for build.yml's docs-only entry-delta step (merge groups).

What must hold:
- the step runs on merge groups (and, report-only, on pull requests as the
  positive control of the strategy read), never fails the job, and prints one
  `docs-entry-delta` notice on every run;
- it classifies with the PROTECTED BASE's copy of classify_changes.py, read
  with `git show "$MERGE_GROUP_MAIN_BASE:..."`, never the group's checkout;
- `skip=true` is written in one place, guarded by `docs-only`, `ALLGREEN` and
  mode `enforce` together; any other mode reports `would_skip`;
- the strategy read requires exactly one active merge_queue rule on the
  default branch; two rules, none, a read error or HEADGREEN keep the build;
- classify applies the verdict to native_build_required alone, on merge
  groups, never over an A2T receipt change, and before the bump fast path.

The behaviour tests run the step's own script against a scratch repository
and a local stand-in for the rulesets API.

Run:
    python3 tools/ci/test_build_yml_docs_entry_delta.py
"""
from __future__ import annotations

import http.server
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Spelled as whole literals so test/ctest_script_inputs/ records them as this
# test's inputs: a change to any of them selects this test.
BUILD_WORKFLOW = ROOT / ".github/workflows/build.yml"
CLASSIFIER = ROOT / "tools/scripts/classify_changes.py"
FINDER = ROOT / "tools/ci/find_python311.py"
STEP_ID = "docs_entry_delta"


def step_block(text: str, step_id: str) -> str:
    """One step of build.yml, from its `- name:` line to the next step."""
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.strip() in (f"id: {step_id}", f"- id: {step_id}"):
            start = index
            while not lines[start].startswith("      - "):
                start -= 1
            end = index + 1
            while end < len(lines) and not lines[end].startswith("      - ") \
                    and not re.match(r"^  \S", lines[end]):
                end += 1
            return "".join(lines[start:end])
    raise AssertionError(f"step {step_id!r} not found in {BUILD_WORKFLOW}")


def step_script(block: str) -> str:
    """The step's `run: |` body, dedented as YAML would."""
    match = re.search(r"^        run: \|\n(.*)", block, re.MULTILINE | re.DOTALL)
    assert match, "the step has no run block"
    out = []
    for line in match.group(1).splitlines():
        if line.strip() and not line.startswith(" " * 10):
            break
        out.append(line[10:])
    return "\n".join(out) + "\n"


def classify_script(text: str) -> str:
    return step_script(step_block(text, "classify"))


class StepContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = BUILD_WORKFLOW.read_text(encoding="utf-8")
        cls.block = step_block(cls.text, STEP_ID)
        cls.script = step_script(cls.block)

    def test_runs_on_merge_groups_with_the_pull_request_control(self) -> None:
        cond = re.search(r"^        if: (.*)$", self.block, re.MULTILINE).group(1)
        self.assertIn("github.event_name == 'merge_group'", cond)
        self.assertIn("github.event_name == 'pull_request'", cond)
        # The control branch exits before any classification.
        control = self.script.index("report control")
        self.assertLess(control, self.script.index("git diff"))
        self.assertIn('if [ "$GITHUB_EVENT_NAME" != merge_group ]; then', self.script)

    def test_the_token_reaches_only_the_inline_strategy_reader(self) -> None:
        self.assertNotIn("GH_TOKEN:", self.block)
        self.assertEqual(self.script.count("${{ github.token }}"), 1)
        self.assertIn('strategy=$(GH_TOKEN="${{ github.token }}" python3 - <<', self.script)

    def test_never_fails_the_job(self) -> None:
        self.assertIn("continue-on-error: true", self.block)
        self.assertIn("set +e", self.script)

    def test_classifies_with_the_protected_base_copy_only(self) -> None:
        self.assertIn('git show "$MERGE_GROUP_MAIN_BASE:tools/scripts/classify_changes.py"', self.script)
        self.assertIn('-I "$classifier"', self.script)
        # No invocation of the checkout's (the group's) classifier.
        self.assertNotRegex(self.script, r"python3?\S*\s+(-I\s+)?tools/scripts/classify_changes\.py")
        self.assertNotIn("$ci_python\" tools/", self.script)

    def test_lists_the_entry_delta_without_renames(self) -> None:
        self.assertIn('git diff --name-only --no-renames -z "$MERGE_GROUP_BASE_SHA" "$EVENT_HEAD_SHA"',
                      self.script)

    def test_enforce_is_guarded_by_docs_only_and_allgreen(self) -> None:
        self.assertEqual(self.script.count("skip=true"), 1)
        guarded = re.search(
            r'if \[ "\$change_class" = docs-only \] && \[ "\$strategy" = ALLGREEN \]; then\n'
            r'\s*if \[ "\$mode" = enforce \]; then\n\s*skip=true\n', self.script)
        self.assertIsNotNone(guarded, "skip=true must sit under the docs-only, ALLGREEN and enforce tests")
        self.assertIn('[ "${DOCS_ENTRY_DELTA_MODE:-}" = enforce ] && mode=enforce', self.script)
        self.assertIn("DOCS_ENTRY_DELTA_MODE: ${{ vars.PULP_DOCS_ENTRY_DELTA }}", self.block)

    def test_reads_the_live_strategy_every_run(self) -> None:
        self.assertIn("rulesets?targets=branch&includes_parents=true", self.script)
        self.assertIn('rule.get("type") == "merge_queue"', self.script)
        self.assertIn('"~DEFAULT_BRANCH" not in', self.script)
        self.assertIn('ref_name.get("exclude")', self.script)
        self.assertIn("DEFAULT_BRANCH: ${{ github.event.repository.default_branch }}", self.block)
        self.assertIn("grouping_strategy", self.script)
        # Read before the lone-group exit, so every notice carries it.
        self.assertLess(self.script.index("rulesets?targets"), self.script.index("report not_applicable"))

    def test_notice_title_and_verdicts(self) -> None:
        self.assertIn('title="docs-entry-delta"', self.script)
        self.assertIn("::notice title=%s::%s class=%s strategy=%s mode=%s base=%s head=%s", self.script)
        verdicts = set(re.findall(r"report (\w+)", self.script))
        self.assertEqual(verdicts, {"control", "not_applicable", "kept", "skipped", "would_skip"})

    def test_classify_applies_it_to_native_build_required_before_the_bump_path(self) -> None:
        script = classify_script(self.text)
        apply = re.search(
            r'if \[ "\$EVENT_NAME" = merge_group \] && \[ "\$\{DOCS_ENTRY_DELTA_SKIP:-\}" = true \] \\\n'
            r'\s*&& \[ "\$a2t_receipt_verification_required" != true \]; then\n'
            r'\s*native_build_required=false\n', script)
        self.assertIsNotNone(apply)
        a2t = re.search(r'"\$a2t_receipt_verification_required" = true \]; then\n\s*native_build_required=true',
                        script)
        self.assertIsNotNone(a2t)
        self.assertLess(a2t.start(), apply.start())
        self.assertLess(apply.start(), script.index("trusted_bump_check=$(mktemp)"))
        self.assertEqual(script.count("DOCS_ENTRY_DELTA_SKIP"), 1)
        self.assertIn("DOCS_ENTRY_DELTA_SKIP: ${{ steps.docs_entry_delta.outputs.skip }}",
                      step_block(self.text, "classify"))

    def test_runs_after_the_hold_check_and_queue_breaker(self) -> None:
        order = [self.text.index(marker) for marker in (
            "id: merge_group_base", "name: Refuse a merge group whose pull request is held",
            "id: queue_breaker", f"id: {STEP_ID}", "      - id: classify")]
        self.assertEqual(order, sorted(order))


class _Rulesets(http.server.BaseHTTPRequestHandler):
    listing: object = []
    rulesets: dict = {}
    status = 200

    def do_GET(self) -> None:  # noqa: N802
        if self.headers.get("Authorization") != "Bearer test-token" or self.status != 200:
            self.send_response(self.status if self.status != 200 else 401)
            self.end_headers()
            return
        path = self.path.split("?", 1)[0]
        body = None
        if path == "/repos/O/R/rulesets":
            body = self.listing
        else:
            match = re.fullmatch(r"/repos/O/R/rulesets/(\d+)", path)
            if match:
                body = self.rulesets.get(int(match.group(1)))
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args) -> None:  # silence
        pass


def queue_ruleset(rid: int, strategy: str, include: tuple[str, ...] = ("~DEFAULT_BRANCH",),
                  exclude: tuple[str, ...] = ()) -> dict:
    return {"id": rid, "enforcement": "active",
            "conditions": {"ref_name": {"include": list(include), "exclude": list(exclude)}},
            "rules": [{"type": "merge_queue", "parameters": {"grouping_strategy": strategy}}]}


@unittest.skipUnless(shutil.which("git") and shutil.which("bash"), "needs git and bash")
class StepBehaviourTests(unittest.TestCase):
    """The step's own script, against a scratch repository and a fake API."""

    @classmethod
    def setUpClass(cls) -> None:
        # Actions substitutes the token expression before the shell runs.
        cls.script = step_script(step_block(BUILD_WORKFLOW.read_text(encoding="utf-8"), STEP_ID)).replace(
            "${{ github.token }}", "test-token")
        cls.tmp = tempfile.TemporaryDirectory()
        repo = Path(cls.tmp.name) / "repo"
        repo.mkdir()
        cls.repo = repo

        def git(*args: str) -> str:
            return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                                  text=True, encoding="utf-8").stdout.strip()

        def commit(message: str, files: dict[str, str]) -> str:
            for rel, content in files.items():
                path = repo / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
                git("add", rel)
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", message)
            return git("rev-parse", "HEAD")

        git("init", "-q")
        cls.bare = commit("no classifier", {"README.md": "x\n"})
        cls.main = commit("main", {
            "tools/scripts/classify_changes.py": CLASSIFIER.read_text(encoding="utf-8"),
            "tools/ci/find_python311.py": FINDER.read_text(encoding="utf-8"),
        })
        cls.code_group = commit("code entry", {"core/x.cpp": "int x;\n"})
        cls.docs_group = commit("docs entry", {"docs/guides/a.md": "a\n"})
        cls.code_after_docs = commit("code entry after docs", {"core/y.cpp": "int y;\n"})

        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Rulesets)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def setUp(self) -> None:
        _Rulesets.status = 200
        self.serve(queue_ruleset(7, "ALLGREEN"))

    def serve(self, *rulesets: dict) -> None:
        _Rulesets.listing = [{"id": r["id"], "enforcement": r["enforcement"]} for r in rulesets]
        _Rulesets.rulesets = {r["id"]: r for r in rulesets}

    def run_step(self, event: str = "merge_group", base: str | None = None, head: str | None = None,
                 main: str | None = None, mode: str = "", default_branch: str = "main") -> tuple[dict, str]:
        out = Path(self.tmp.name) / "output"
        out.write_text("", encoding="utf-8")
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("GITHUB_", "MERGE_GROUP_", "DOCS_", "DEFAULT_BRANCH"))}
        env.update({
            "DEFAULT_BRANCH": default_branch,
            "GITHUB_EVENT_NAME": event, "GITHUB_REPOSITORY": "O/R", "GITHUB_OUTPUT": str(out),
            "GITHUB_API_URL": f"http://127.0.0.1:{self.server.server_address[1]}",
            "DOCS_ENTRY_DELTA_MODE": mode,
            "MERGE_GROUP_BASE_SHA": base if base is not None else self.code_group,
            "EVENT_HEAD_SHA": head if head is not None else self.docs_group,
            "MERGE_GROUP_MAIN_BASE": main if main is not None else self.main,
        })
        proc = subprocess.run(["bash", "-c", self.script], cwd=self.repo, env=env, capture_output=True,
                              text=True, encoding="utf-8", check=False)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        outputs = dict(line.split("=", 1) for line in out.read_text(encoding="utf-8").splitlines() if "=" in line)
        notices = [line for line in proc.stdout.splitlines() if line.startswith("::notice title=docs-entry-delta::")]
        self.assertEqual(len(notices), 1, proc.stdout)
        return outputs, notices[0]

    def test_docs_entry_under_allgreen_in_enforce_skips(self) -> None:
        outputs, notice = self.run_step(mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("true", "skipped"))
        self.assertIn("class=docs-only strategy=ALLGREEN mode=enforce", notice)

    def test_any_other_mode_only_reports_would_skip(self) -> None:
        for mode in ("", "shadow", "Enforce", "enforce "):
            outputs, notice = self.run_step(mode=mode)
            self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "would_skip"), mode)
            self.assertIn("mode=shadow", notice)

    def test_a_strategy_other_than_allgreen_keeps_the_build(self) -> None:
        cases = {
            "HEADGREEN": (queue_ruleset(7, "HEADGREEN"),),
            "multiple": (queue_ruleset(7, "ALLGREEN"), queue_ruleset(8, "ALLGREEN")),
            "none": (),
            "not_default_branch": (queue_ruleset(7, "ALLGREEN", ("refs/heads/release",)),),
        }
        for expected, rulesets in cases.items():
            self.serve(*rulesets)
            outputs, notice = self.run_step(mode="enforce")
            self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"), expected)
            self.assertIn(f"strategy={expected} ", notice)

    def test_a_queue_rule_that_excludes_the_default_branch_keeps_the_build(self) -> None:
        # Excluded wins over included, so the queue is not ALLGREEN on main.
        for exclude in (("~DEFAULT_BRANCH",), ("~ALL",), ("refs/heads/main",), ("refs/heads/ma*",),
                        ("refs/heads/release", "refs/heads/main")):
            self.serve(queue_ruleset(7, "ALLGREEN", exclude=exclude))
            outputs, notice = self.run_step(mode="enforce")
            self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"), exclude)
            self.assertIn("strategy=default_branch_excluded ", notice, exclude)

    def test_an_exclusion_of_another_branch_still_skips(self) -> None:
        # Control: an exclusion that cannot match the default branch changes nothing.
        self.serve(queue_ruleset(7, "ALLGREEN", exclude=("refs/heads/release", "refs/heads/main-*")))
        outputs, notice = self.run_step(mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("true", "skipped"))
        self.assertIn("strategy=ALLGREEN ", notice)

    def test_an_exclusion_without_the_default_branch_name_or_unreadable_is_unreadable(self) -> None:
        self.serve(queue_ruleset(7, "ALLGREEN", exclude=("refs/heads/release",)))
        outputs, notice = self.run_step(mode="enforce", default_branch="")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"))
        self.assertIn("strategy=unreadable ", notice)
        for conditions in ({"include": ["~DEFAULT_BRANCH"], "exclude": "refs/heads/main"},
                           {"include": ["~DEFAULT_BRANCH"], "exclude": [{"pattern": "main"}]}):
            ruleset = queue_ruleset(7, "ALLGREEN")
            ruleset["conditions"]["ref_name"] = conditions
            self.serve(ruleset)
            outputs, notice = self.run_step(mode="enforce")
            self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"), conditions)
            self.assertRegex(notice, r"strategy=(unreadable|default_branch_excluded) ", conditions)

    def test_an_unreadable_strategy_keeps_the_build(self) -> None:
        _Rulesets.status = 500
        outputs, notice = self.run_step(mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"))
        self.assertIn("strategy=unreadable", notice)

    def test_an_inactive_second_queue_rule_does_not_count(self) -> None:
        inactive = dict(queue_ruleset(8, "HEADGREEN"), enforcement="evaluate")
        self.serve(queue_ruleset(7, "ALLGREEN"), inactive)
        outputs, _ = self.run_step(mode="enforce")
        self.assertEqual(outputs["skip"], "true")

    def test_a_code_entry_keeps_the_build(self) -> None:
        outputs, notice = self.run_step(base=self.docs_group, head=self.code_after_docs, mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"))
        self.assertIn("class=code:core", notice)

    def test_a_lone_group_is_not_applicable(self) -> None:
        outputs, notice = self.run_step(base=self.code_group, main=self.code_group, mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "not_applicable"))
        self.assertIn("strategy=ALLGREEN", notice)

    def test_a_base_without_the_classifier_keeps_the_build(self) -> None:
        outputs, notice = self.run_step(main=self.bare, mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "kept"))
        self.assertIn("class=classifier_missing", notice)

    def test_an_unresolved_main_base_or_unreadable_delta_keeps_the_build(self) -> None:
        outputs, notice = self.run_step(main="origin/main", mode="enforce")
        self.assertEqual(outputs["skip"], "false")
        self.assertIn("class=main_base_unresolved", notice)
        outputs, notice = self.run_step(base="f" * 40, mode="enforce")
        self.assertEqual(outputs["skip"], "false")
        self.assertIn("class=delta_unreadable", notice)
        outputs, notice = self.run_step(base=self.docs_group, head=self.docs_group, mode="enforce")
        self.assertEqual(outputs["skip"], "false")
        self.assertIn("class=delta_unreadable", notice)

    def test_a_pull_request_reports_the_strategy_control_only(self) -> None:
        outputs, notice = self.run_step(event="pull_request", mode="enforce")
        self.assertEqual((outputs["skip"], outputs["verdict"]), ("false", "control"))
        self.assertIn("control class=not_read strategy=ALLGREEN", notice)


if __name__ == "__main__":
    unittest.main()
