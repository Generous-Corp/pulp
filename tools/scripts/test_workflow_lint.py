#!/usr/bin/env python3
"""Static regression tests for .github/workflows/workflow-lint.yml.

The workflow lint gate is the first release-watchdog layer: it must run
on CI-definition changes and keep the three local checks that catch
workflow-file failures before merge.

It also owns the fleet-wide action-pin invariant below: GitHub retires a
runtime under an action's old major, and the resulting deprecation
annotation is only visible on a real run, so a drifting pin is otherwise
found by reading warnings rather than by a gate.

Run:
    python3 tools/scripts/test_workflow_lint.py
"""

from __future__ import annotations

import json
import re
import shlex
import unittest
from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "workflow-lint.yml"
ACTIONLINT_CONFIG = REPO_ROOT / ".github" / "actionlint.yaml"
STEWARD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "shipyard-merge-steward.yml"


def _workflow_text() -> str:
    return WORKFLOW.read_text(encoding="utf-8")


def _find_step(text: str, name: str) -> str:
    pattern = re.compile(
        rf"^\s{{6}}-\s+name:\s+{re.escape(name)}\s*\n"
        r"([\s\S]*?)(?=^\s{6}-\s+(?:name:|uses:)|\Z)",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if not match:
        raise AssertionError(f"could not find workflow step named {name!r}")
    return match.group(0)


def _find_uses_step(text: str, uses: str) -> str:
    # Keep the test bound to the requested major while allowing Dependabot's
    # full semver action pins (for example, v7.0.1) to advance within it.
    ref_pattern = re.escape(uses)
    if re.search(r"@v\d+$", uses):
        ref_pattern += r"(?:\.\d+(?:\.\d+)?)?"
    pattern = re.compile(
        rf"^\s{{6}}-\s+uses:\s+{ref_pattern}\s*\n"
        r"([\s\S]*?)(?=^\s{6}-\s+(?:name:|uses:)|\Z)",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if not match:
        raise AssertionError(f"could not find workflow uses step {uses!r}")
    return match.group(0)


class WorkflowLintWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.assertTrue(WORKFLOW.exists(), f"missing workflow: {WORKFLOW}")
        self.text = _workflow_text()

    def test_trigger_scope_covers_workflow_and_action_changes(self) -> None:
        self.assertRegex(self.text, r"(?m)^on:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{2}pull_request:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{2}push:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{4}branches:\s*\[main\]\s*$")

        path_patterns = re.findall(r"(?m)^\s{6}-\s+'([^']+)'\s*$", self.text)
        self.assertGreaterEqual(path_patterns.count(".github/actionlint.yaml"), 2)
        self.assertGreaterEqual(path_patterns.count(".github/workflows/**"), 2)
        self.assertGreaterEqual(path_patterns.count(".github/actions/**"), 2)
        self.assertGreaterEqual(path_patterns.count("tools/shipyard.toml"), 2)
        for path in (
            "tools/scripts/fetch_skia_for_release.py",
            "tools/scripts/test_fetch_skia_for_release.py",
            "tools/scripts/test_fetch_skia_for_release_extra.py",
            "tools/scripts/verify_skia_m153_capabilities.py",
            "tools/scripts/test_verify_skia_m153_capabilities.py",
            "tools/build-skia.sh",
            "tools/scripts/test_build_skia_source_pin.py",
            "tools/deps/validate_render_update.py",
            "tools/deps/test_validate_render_update.py",
            "tools/deps/validate_hosts.py",
            "tools/deps/test_validate_hosts.py",
            "tools/ci/pulp-worktree.sh",
            "tools/ci/test_pulp_worktree_cache_env.py",
            "tools/ci/test_pulp_worktree_gc.py",
        ):
            self.assertGreaterEqual(path_patterns.count(path), 2, path)

    def test_actionlint_knows_the_self_hosted_runner_labels(self) -> None:
        self.assertTrue(
            ACTIONLINT_CONFIG.exists(),
            f"missing actionlint config: {ACTIONLINT_CONFIG}",
        )
        config = ACTIONLINT_CONFIG.read_text(encoding="utf-8")
        self.assertRegex(config, r"(?m)^self-hosted-runner:\s*$")
        self.assertRegex(config, r"(?m)^\s{2}labels:\s*$")
        self.assertRegex(config, r"(?m)^\s{4}-\s+pulp-build\s*$")
        self.assertRegex(config, r"(?m)^\s{4}-\s+pulp-build-vm\s*$")
        self.assertRegex(config, r"(?m)^\s{4}-\s+pulp-build-merge-group\s*$")

    def test_workflow_lint_gate_runs_this_regression_suite(self) -> None:
        self.assertIn(
            "python3 tools/scripts/test_workflow_lint.py",
            self.text,
        )

    def test_every_named_path_and_invoked_script_exists(self) -> None:
        # A deleted script that stays listed here is silent twice over: its
        # trigger path can never match, and its `python3 <script>` line fails
        # only after the gate has already been skipped on the PR that deleted it.
        workflow = yaml.safe_load(self.text)
        triggers = workflow.get("on", workflow.get(True))
        named: set[str] = set()
        for event in ("pull_request", "push"):
            for entry in (triggers.get(event) or {}).get("paths", []):
                if not any(ch in entry for ch in "*?[!"):
                    named.add(entry)
        invoked = set(
            re.findall(r"python3\s+((?:tools|scripts)/[\w./-]+\.py)", self.text)
        )
        # Controls: both collections must be populated, or a parser change
        # would turn this check into one that inspects nothing.
        self.assertGreater(len(named), 50)
        self.assertGreater(len(invoked), 20)
        missing = sorted(
            path for path in named | invoked if not (REPO_ROOT / path).exists()
        )
        self.assertEqual(missing, [], "workflow-lint.yml names paths that do not exist")

    def test_workflow_has_minimal_permissions_and_concurrency(self) -> None:
        self.assertRegex(
            self.text,
            r"(?m)^permissions:\s*\n\s{2}contents:\s*read\s*$",
        )
        self.assertRegex(
            self.text,
            r"(?m)^concurrency:\s*\n"
            r"\s{2}group:\s*workflow-lint-\$\{\{\s*github\.ref\s*\}\}\s*\n"
            r"\s{2}cancel-in-progress:\s*true\s*$",
        )

    def test_every_job_inherits_or_declares_token_permissions(self) -> None:
        missing: list[str] = []
        workflows = sorted(
            (REPO_ROOT / ".github" / "workflows").glob("*.y*ml")
        )
        for path in workflows:
            value = yaml.safe_load(path.read_text(encoding="utf-8"))
            if value.get("permissions") is not None:
                continue
            for job_name, job in value.get("jobs", {}).items():
                if job.get("permissions") is None:
                    missing.append(f"{path.name}:{job_name}")
        self.assertEqual(
            missing,
            [],
            "jobs inherit repository-default token permissions: "
            + ", ".join(missing),
        )

    def test_lint_job_runs_on_github_ubuntu_with_checkout_and_python(self) -> None:
        self.assertRegex(self.text, r"(?m)^\s{2}lint:\s*$")
        self.assertRegex(self.text, r"(?m)^\s{4}runs-on:\s*ubuntu-latest\s*$")
        self.assertIn("yamllint + actionlint + structural parse", self.text)

        checkout = _find_uses_step(self.text, "actions/checkout@v7")
        self.assertRegex(checkout, r"(?m)^\s{10}fetch-depth:\s*1\s*$")

        setup_python = _find_step(self.text, "Set up Python")
        # The major lives in ActionMajorPinTests so a routine action bump
        # touches one invariant instead of every workflow assertion.
        self.assertRegex(setup_python, r"uses: actions/setup-python@v\d+")
        self.assertRegex(setup_python, r"(?m)^\s{10}python-version:\s*'3\.12'\s*$")

    def test_yamllint_step_uses_pinned_local_workflow_lint(self) -> None:
        step = _find_step(self.text, "yamllint")
        self.assertIn("set -euo pipefail", step)
        self.assertIn("yamllint==1.35.1", step)
        self.assertIn("yamllint --no-warnings -d 'relaxed' .github/workflows/", step)

    def test_structural_parse_checks_all_workflow_yaml_files(self) -> None:
        step = _find_step(self.text, "Structural YAML parse")
        self.assertIn("set -euo pipefail", step)
        self.assertIn("pyyaml>=6", step)
        self.assertIn("yaml.safe_load", step)
        self.assertIn("pathlib.Path('.github/workflows').rglob('*.yml')", step)
        self.assertIn("pathlib.Path('.github/workflows').rglob('*.yaml')", step)
        self.assertIn("except yaml.YAMLError", step)
        self.assertIn("sys.exit(1)", step)
        self.assertIn("structural parse OK", step)

    def test_release_regression_tests_remain_in_lint_gate(self) -> None:
        step = _find_step(self.text, "Release-pipeline regression tests (#720, #1962, #2467)")
        self.assertIn("python3 tools/scripts/test_ctest_nonruns.py", step)
        for path in ("tools/scripts/ctest_nonruns.py", "tools/scripts/test_ctest_nonruns.py"):
            self.assertEqual(self.text.count(f"- '{path}'"), 2)
        self.assertIn("set -euo pipefail", step)
        self.assertIn(
            "python3 tools/scripts/test_release_workflow_test_step.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_workflow_build_dirs.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_build_workflow.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_visual_python_deps_step.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_build_macos_workflow_dispatch.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_fetch_skia_for_release.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_fetch_skia_for_release_extra.py",
            step,
        )
        self.assertIn(
            "python3 tools/scripts/test_preamble_python_stable_cwd.py",
            step,
        )
        self.assertIn(
            "python3 tools/ci/test_pulp_worktree_cache_env.py",
            step,
        )
        self.assertIn(
            "python3 tools/ci/test_pulp_worktree_gc.py",
            step,
        )

    def test_dependabot_tracks_github_actions(self) -> None:
        config = REPO_ROOT / ".github" / "dependabot.yml"
        self.assertTrue(config.exists(), f"missing Dependabot config: {config}")
        value = yaml.safe_load(config.read_text(encoding="utf-8"))
        self.assertEqual(value.get("version"), 2)
        updates = value.get("updates", [])
        self.assertIn(
            {"package-ecosystem": "github-actions", "directory": "/"},
            [{k: item.get(k) for k in ("package-ecosystem", "directory")} for item in updates],
        )

    def test_actionlint_step_keeps_core_actionlint_enabled(self) -> None:
        step = _find_step(self.text, "actionlint")
        self.assertIn("uses: raven-actions/actionlint@3d39aea434753780c3b3d4a1a31c854b4dbf49d7 # v2.2.0", step)
        self.assertRegex(step, r"(?m)^\s{10}matcher:\s*true\s*$")
        self.assertRegex(step, r"(?m)^\s{10}shellcheck:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}pyflakes:\s*false\s*$")
        self.assertRegex(step, r"(?m)^\s{10}flags:\s*''\s*$")


class ActionMajorPinTests(unittest.TestCase):
    """Every workflow must agree on one non-retired major per action.

    A split pin is the failure mode that actually happens: a new workflow
    copies a current pin while the older ones keep an obsolete major, and
    the deprecation surfaces only as a warning annotation on a live run.
    Comparing the pins against each other catches the drift without this
    test needing to know GitHub's current runtime deprecation schedule.
    """

    #: Minimum major per action, raised as GitHub retires the runtime under
    #: the previous one. `setup-python` moved to 6 when v5 was forced onto
    #: Node 24 and started emitting a Node 20 deprecation annotation.
    MINIMUM_MAJOR = {"setup-python": 6}

    def _pins(self) -> dict[str, dict[str, set[int]]]:
        found: dict[str, dict[str, set[int]]] = {}
        pattern = re.compile(r"uses:\s*actions/([a-z0-9-]+)@v(\d+)")
        for workflow in sorted((REPO_ROOT / ".github" / "workflows").rglob("*.yml")):
            for action, major in pattern.findall(workflow.read_text(encoding="utf-8")):
                found.setdefault(action, {}).setdefault(workflow.name, set()).add(int(major))
        return found

    def test_setup_python_is_pinned_above_the_retired_major(self) -> None:
        pins = self._pins()
        for action, minimum in self.MINIMUM_MAJOR.items():
            with self.subTest(action=action):
                usage = pins.get(action)
                self.assertIsNotNone(usage, f"no workflow uses actions/{action}")
                stale = {
                    name: sorted(majors)
                    for name, majors in usage.items()
                    if any(major < minimum for major in majors)
                }
                self.assertEqual(
                    stale,
                    {},
                    f"actions/{action} must be pinned to v{minimum} or newer; "
                    f"stale pins: {stale}",
                )

    def test_setup_python_major_does_not_split_across_workflows(self) -> None:
        usage = self._pins().get("setup-python") or {}
        majors = sorted({major for majors in usage.values() for major in majors})
        self.assertEqual(
            len(majors),
            1,
            f"actions/setup-python is pinned to more than one major: {majors}",
        )

    def test_merge_steward_workflow_is_a_manual_diagnostic(self) -> None:
        # The local steward tick on the mutation machine is the single
        # admission writer; this workflow must not race it on a schedule.
        text = STEWARD_WORKFLOW.read_text(encoding="utf-8")
        document = yaml.load(text, Loader=yaml.BaseLoader)
        self.assertNotIn("schedule", document["on"])
        self.assertIn("workflow_dispatch", document["on"])
        # Read-only: no apply input, no --apply, no write-scoped token.
        self.assertNotIn("inputs", document["on"]["workflow_dispatch"] or {})
        self.assertNotIn("--apply", text)
        self.assertNotIn(": write", text)
        self.assertNotIn("github.event_name == 'schedule'", text)
        self.assertIn("runner steward --repo", text)
        self.assertIn("--arm-unqueued", text)
        self.assertNotIn("dispatch_recovery", text)
        self.assertNotIn("shipyard-merge-steward.yml",
                         (REPO_ROOT / ".github" / "schedule-backstop.json").read_text(encoding="utf-8"))
        self.assertIn("actions: read", text)
        self.assertIn("actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1", text)
        self.assertIn("actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9", text)
        self.assertEqual(text.count("run_steward_with_retry()"), 1)


# --- Workflow credentials and the environments that hold them -----------------
# A repository-level secret is readable by any workflow run, including one a
# pushed branch starts after editing a workflow. An environment secret reaches
# only a job whose ref passes the environment's deployment policy. These tests
# pin the code half of that boundary (the policy half is repository settings):
# - every credential a workflow names is classified below, and each one moved
#   into an environment is read only by jobs that declare it;
# - no workflow reads secrets dynamically (`secrets[...]`, `toJSON(secrets)`,
#   `secrets: inherit`), which would hide a credential from this scan;
# - a job in a workflow pull requests can trigger names an environment only
#   behind an event allowlist that excludes every pull-request event;
# - a job in a NO_REPO_CODE environment checks out nothing, so no repository
#   code runs in the job that holds the credential.

SECRET_WORKFLOWS = REPO_ROOT / ".github" / "workflows"

# Credential -> the environment that holds it.
ENVIRONMENT_SECRETS: dict[str, str] = {
    # The WebCLAP deploy job, which only unpacks a digest-checked tarball.
    "CLOUDFLARE_API_TOKEN": "deploy",
    "CLOUDFLARE_ACCOUNT_ID": "deploy",
    "TIER_RECORD_APP_ID": "tier-record",
    "TIER_RECORD_APP_KEY": "tier-record",
}

# Environments whose jobs must run no repository code.
NO_REPO_CODE = {"deploy"}

# Credentials that stay repository secrets this round, with the reason.
REPOSITORY_SECRETS: dict[str, str] = {
    # Deferred: read by Shipyard and the steward at runtime; moving them needs
    # a canary plan.
    "SHIPYARD_APP_ID": "deferred: shipyard-local",
    "SHIPYARD_APP_PRIVATE_KEY": "deferred: shipyard-local",
    # Deferred: the release-bot PAT and its signing key also serve
    # version-at-land and three monitors; they move with the versioner App.
    "RELEASE_BOT_TOKEN": "deferred: bump-at-merge and monitors still read it",
    "RELEASE_BOT_SSH_SIGNING_KEY": "deferred: version-at-land signs with it",
    "VELLUM_DISPATCHER_APP_PRIVATE_KEY": "deferred: not a release secret",
    # Upload-only coverage token; pull-request jobs upload with it.
    "CODECOV_TOKEN": "upload-only, read by pull-request jobs",
    # Read-only App the pull_request_target / merge_group trusted gate uses.
    "VELLUM_READER_APP_PRIVATE_KEY": "read-only App, read by merge-group jobs",
    # Optional developer-supplied SDK location; not configured.
    "PULP_AAX_SDK_ZIP_URL": "not a credential, unset",
    "PULP_AAX_SDK_ZIP_SHA256": "not a credential, unset",
    # macOS signing for sign-and-release.yml (tag-triggered); not configured.
    # Set them only in an environment whose job runs no repository code, and
    # only once the v* tag ruleset restricts creation to the release identity.
    "MACOS_CERTIFICATE": "unset",
    "MACOS_CERTIFICATE_PWD": "unset",
    "KEYCHAIN_PASSWORD": "unset",
    "SIGNING_IDENTITY": "unset",
    "APPLE_ID": "unset",
    "APPLE_TEAM_ID": "unset",
    "APPLE_PASSWORD": "unset",
}

# The only actions a NO_REPO_CODE job may use, pinned as the workflow pins them.
NO_REPO_CODE_ACTIONS = (
    "actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333",
    "cloudflare/wrangler-action@953926a2e2182532811c01a25e53647d93bf07c0",
)
# Commands a credential-holding job's shell may run: no interpreter, no
# package manager, nothing that fetches or executes other code.
ALLOWED_COMMANDS = {
    "gh", "git", "tar", "sha256sum", "find", "mkdir", "test", "echo", "printf",
    "cut", "awk", "grep", "set", "exit", "true", "false", "local", "return",
    "[", "[[", "case", "esac", "fi", "done", "for", "}", "python3 -c",
}
_COMMAND_SEPARATORS = {"|", "||", "&&", ";", ";;", "(", "()", "\n", "&", "|&", "$("}
_TRANSPARENT = {"if", "then", "else", "elif", "do", "while", "until", "!", "{", "time"}


def disallowed_commands(script: str, functions: frozenset[str] = frozenset()) -> list[str]:
    """First words of the commands in `script` that ALLOWED_COMMANDS lacks.

    Tokenizes with shell quoting, so separators inside quotes, regexes and
    awk programs are not commands. A word that names a function the script
    defines is allowed.
    """
    lines = [line for line in script.replace("\\\n", " ").splitlines()
             if not line.strip().startswith("#")]
    text = "\n".join(line for line in lines
                     if not re.match(r"^\s*[\w|*.-]+\)", line))
    lexer = shlex.shlex(text, posix=True, punctuation_chars="();<>|&\n")
    lexer.whitespace = " \t"
    lexer.commenters = ""
    lexer.whitespace_split = True
    tokens = list(lexer)
    functions = set(functions) | set(re.findall(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\)", text))
    found = []
    expect = True
    for i, tok in enumerate(tokens):
        if tok in _COMMAND_SEPARATORS or tok.endswith("$"):
            expect = True
            continue
        if not expect:
            continue
        if tok in functions and tokens[i + 1:i + 2] in (["("], ["()"]):
            continue  # the definition itself
        if tok in _TRANSPARENT or re.match(r"^[A-Za-z_][A-Za-z0-9_]*\+?=", tok) or tok in (")", "<", ">"):
            continue
        expect = False
        word = "python3 -c" if tok == "python3" and tokens[i + 1:i + 2] == ["-c"] else tok
        if word not in ALLOWED_COMMANDS and word not in functions:
            found.append(word)
    # Command substitutions inside double quotes are one word to the lexer.
    for inner in re.findall(r"\$\(([^()]*)\)", text):
        found += [w for w in disallowed_commands(inner, frozenset(functions)) if w not in found]
    if "`" in text:
        found.append("`")
    return found


PULL_REQUEST_EVENTS = {"pull_request", "pull_request_target", "merge_group"}
SECRET_RE = re.compile(r"secrets\.([A-Za-z_][A-Za-z0-9_]*)")
DYNAMIC_SECRETS_RE = re.compile(r"secrets\s*\[|toJSON\(\s*secrets\s*\)|secrets:\s*inherit")
EVENT_ALLOWLIST_RE = re.compile(r"contains\(fromJSON\('(\[[^']*\])'\),\s*github\.event_name\)")


def _secret_triggers(workflow: dict) -> set[str]:
    on = workflow.get(True, workflow.get("on"))
    if isinstance(on, str):
        return {on}
    if isinstance(on, list):
        return set(on)
    return set(on or {})


def _secret_environment(job: dict) -> str | None:
    env = job.get("environment")
    if isinstance(env, dict):
        return env.get("name")
    return env


def _secret_jobs():
    for path in sorted(SECRET_WORKFLOWS.glob("*.y*ml")):
        workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
        for name, job in (workflow.get("jobs") or {}).items():
            text = yaml.safe_dump(job, width=100000)
            secrets = set(SECRET_RE.findall(text)) - {"GITHUB_TOKEN"}
            yield path.name, _secret_triggers(workflow), name, job, secrets


class WorkflowSecretEnvironments(unittest.TestCase):
    def test_the_scan_sees_every_known_credential(self) -> None:
        # Control: a renamed secret or a broken scan would make every
        # assertion below vacuous.
        seen = set().union(*(secrets for *_, secrets in _secret_jobs()))
        self.assertEqual(set(ENVIRONMENT_SECRETS) - seen, set())
        envs = {_secret_environment(job) for _, _, _, job, _ in _secret_jobs()}
        self.assertTrue(set(ENVIRONMENT_SECRETS.values()) <= envs)

    def test_every_credential_is_classified_and_read_in_its_environment(self) -> None:
        problems = []
        for wf, _, job_name, job, secrets in _secret_jobs():
            env = _secret_environment(job)
            for secret in sorted(secrets - set(REPOSITORY_SECRETS)):
                want = ENVIRONMENT_SECRETS.get(secret)
                if want is None:
                    problems.append(f"{wf}:{job_name}: classify {secret} in this test")
                elif env != want:
                    problems.append(f"{wf}:{job_name}: {secret} read in env {env!r}, not {want!r}")
        self.assertEqual(problems, [])

    def test_no_workflow_reads_secrets_dynamically(self) -> None:
        offenders = [path.name for path in sorted(SECRET_WORKFLOWS.glob("*.y*ml"))
                     if DYNAMIC_SECRETS_RE.search(path.read_text(encoding="utf-8"))]
        self.assertEqual(offenders, [])

    def test_no_pull_request_run_is_pointed_at_an_environment(self) -> None:
        offenders = []
        for wf, triggers, job_name, job, _ in _secret_jobs():
            if _secret_environment(job) is None or not (triggers & PULL_REQUEST_EVENTS):
                continue
            allow = EVENT_ALLOWLIST_RE.search(str(job.get("if") or ""))
            if not allow or set(json.loads(allow.group(1))) & PULL_REQUEST_EVENTS:
                offenders.append(f"{wf}:{job_name}")
        self.assertEqual(offenders, [])

    def test_credential_jobs_in_no_repo_code_environments_check_out_nothing(self) -> None:
        # A job holding a release or deploy credential may use only the
        # pinned actions below, and its shell may not fetch or run code from
        # the repository or the network.
        offenders = []
        for wf, _, job_name, job, _ in _secret_jobs():
            if _secret_environment(job) not in NO_REPO_CODE:
                continue
            for step in job.get("steps") or []:
                uses = str(step.get("uses") or "")
                if uses and not uses.startswith(NO_REPO_CODE_ACTIONS):
                    offenders.append(f"{wf}:{job_name}: uses {uses}")
                run = str(step.get("run") or "")
                offenders += [f"{wf}:{job_name}: runs {word}" for word in disallowed_commands(run)]
        self.assertEqual(offenders, [])

    def test_the_no_repo_code_check_is_live(self) -> None:
        # Control: each forbidden shape is caught, and allowed shell is not.
        for run in ("node x.mjs", "python3 tools/scripts/x.py", "source x", ". x.sh",
                    "npx wrangler", "make", "bash ./x.sh", "./x", "echo a | sh",
                    'echo "$(npm i x)"', "echo `node x`", "f() { curl -s y; }\nf"):
            self.assertNotEqual(disallowed_commands(run), [], run)
        self.assertEqual(disallowed_commands('set -e\nx=$(sha256sum "$f" | cut -d" " -f1)\n'
                                             'if [ -z "$x" ]; then echo no >&2; exit 1; fi'), [])
        self.assertFalse("./local-action".startswith(NO_REPO_CODE_ACTIONS))


if __name__ == "__main__":
    unittest.main(verbosity=2)
