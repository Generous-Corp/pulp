#!/usr/bin/env python3
"""Tests for the pre-push hook + run_prepush_gates.sh wrapper (#1144).

These tests pin the post-#1144 contract: gate failures BLOCK the push by
default. Without this contract a deliberate skill-sync / version-bump /
deps-audit / diff-cover violation slips through pre-push silently and
only surfaces 5+ minutes later when CI fails (the painful pattern that
PR #1005 hit seven times in a row).

Each test is designed to deliberately fail when the flip introduced in
issue #1144 is reverted — i.e. if the hook ever returns to advisory-by-
default, the assertions below will catch it.

Strategy:
    Build a throwaway git repo with the *real* hook + wrapper script
    copied in, plus stub gate scripts placed at the paths the hook reads
    (tools/scripts/skill_sync_check.py, version_bump_check.py,
    compat_sync_check.py, tools/deps/audit.py, tools/scripts/local_diff_cover.sh).
    Each stub deterministically exits 0 or 1 based on env vars passed by
    the test, so we exercise the hook's orchestration logic — not the
    Python gate scripts themselves (those have their own tests).

Run:
    python3 tools/scripts/test_prepush_gates.py
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
HOOK = REPO_ROOT / ".githooks" / "pre-push"
WRAPPER = REPO_ROOT / "tools" / "scripts" / "run_prepush_gates.sh"


# ── Static-text contract checks ────────────────────────────────────────


class HookContractTests(unittest.TestCase):
    """The hook source must encode the post-#1144 enforce-by-default
    contract. These checks fail loudly if a future edit reverts the flip
    by accident or in the name of a "small fix".
    """

    def test_hook_exists_and_is_executable(self) -> None:
        self.assertTrue(HOOK.exists(), f"missing: {HOOK}")
        self.assertTrue(os.access(HOOK, os.X_OK),
                        f"hook not executable: {HOOK}")

    def test_hook_documents_disable_var(self) -> None:
        text = HOOK.read_text()
        # Both the new opt-out env vars must be documented in the
        # header so anyone running `cat .githooks/pre-push` learns the
        # bypass without hunting through other docs.
        self.assertIn("PULP_DISABLE_PREPUSH_GATES", text)
        self.assertIn("PULP_DISABLE_PREPUSH_DIFF_COVER", text)
        # Total bypass remains.
        self.assertIn("PULP_SKIP_PREPUSH", text)

    def test_hook_default_path_blocks(self) -> None:
        text = HOOK.read_text()
        # Anti-revert: the closing block must `exit 1` when there is a
        # gate failure and `PULP_DISABLE_PREPUSH_GATES` is NOT set. A
        # revert that flips the default back to advisory would replace
        # the trailing `exit 1` with `exit 0` and this test catches it.
        self.assertIn("BLOCKING push", text,
                      "hook must announce that it is blocking on failure")
        # The legacy advisory message MUST be gone — a bare `exit 0` at
        # the end of the failure path is the bug we're fixing.
        self.assertNotIn(
            "warnings above are advisory (export PULP_ENFORCE_PREPUSH=1 to block)",
            text,
            "legacy advisory-by-default message resurfaced — the flip "
            "introduced for #1144 was reverted",
        )

    def test_hook_diff_cover_blocks_by_default(self) -> None:
        text = HOOK.read_text()
        # The legacy diff-cover advisory message must be gone too.
        self.assertNotIn(
            "diff-coverage advisory: failure above is non-blocking",
            text,
            "legacy diff-cover advisory message resurfaced — the flip "
            "introduced for #1144 was reverted",
        )
        self.assertIn("PULP_DISABLE_PREPUSH_DIFF_COVER", text)

    def test_hook_passes_enforce_to_compat(self) -> None:
        # The hook must pass --enforce to compat_sync_check so the
        # script actually exits non-zero on drift even when the caller
        # didn't export PULP_ENFORCE_PREPUSH=1. Without this, the flip
        # at the hook level is undermined for the compat gate.
        text = HOOK.read_text()
        self.assertRegex(
            text,
            r"compat_sync_check\.py.*--enforce|--enforce.*compat_sync_check\.py|"
            r'\$CSC".*--enforce|\$CSC.*--enforce',
            "hook must pass --enforce to compat_sync_check.py",
        )


class WrapperContractTests(unittest.TestCase):

    def test_wrapper_exists_and_is_executable(self) -> None:
        self.assertTrue(WRAPPER.exists(), f"missing: {WRAPPER}")
        self.assertTrue(os.access(WRAPPER, os.X_OK),
                        f"wrapper not executable: {WRAPPER}")

    def test_wrapper_documents_contract(self) -> None:
        text = WRAPPER.read_text()
        # Same opt-outs as the hook; must be documented inline so
        # agents reading the script learn the contract.
        self.assertIn("PULP_DISABLE_PREPUSH_GATES", text)
        self.assertIn("PULP_DISABLE_PREPUSH_DIFF_COVER", text)
        self.assertIn("PULP_SKIP_PREPUSH", text)
        # Wrapper must mention the same gate names the hook runs.
        for gate in (
            "skill-sync", "version-bump", "compat-sync",
            "deps-audit", "diff-cover",
        ):
            self.assertIn(gate, text,
                          f"wrapper omits documented gate: {gate}")

    def test_wrapper_help_succeeds(self) -> None:
        result = subprocess.run(
            ["bash", str(WRAPPER), "--help"],
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0,
                         f"--help should exit 0, got {result.returncode}\n"
                         f"stderr: {result.stderr}")


# ── Integration: hook orchestration with stub gate scripts ─────────────


def _git(cwd: Path, *args: str) -> None:
    env = os.environ.copy()
    env["GIT_AUTHOR_NAME"] = env["GIT_COMMITTER_NAME"] = "Test"
    env["GIT_AUTHOR_EMAIL"] = env["GIT_COMMITTER_EMAIL"] = "test@example.com"
    subprocess.run(
        ["git", "-C", str(cwd), *args],
        check=True, capture_output=True, env=env,
    )


# Stub gate scripts. Each respects an env var that lets the test pick
# pass / fail. We ship these as Python scripts because the hook calls
# them via $PYTHON; signalling pass/fail via `sys.exit` is the cleanest.

_STUB_SKILL_SYNC = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    sys.exit(1 if os.environ.get("STUB_SKILL_SYNC_FAIL") == "1" else 0)
""")

_STUB_VERSION_BUMP = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    sys.exit(1 if os.environ.get("STUB_VERSION_BUMP_FAIL") == "1" else 0)
""")

_STUB_COMPAT_SYNC = textwrap.dedent("""\
    #!/usr/bin/env python3
    # Real compat_sync_check.py reads PULP_ENFORCE_PREPUSH and only
    # exits 1 if --enforce or that var is set. The hook (per #1144)
    # MUST pass --enforce explicitly. This stub asserts both: it
    # records that --enforce was seen, and exits 1 only when the test
    # asks for failure.
    import os, sys
    saw_enforce = "--enforce" in sys.argv
    if os.environ.get("STUB_COMPAT_SYNC_REQUIRE_ENFORCE") == "1" and not saw_enforce:
        # Hook didn't pass --enforce — record this so the test fails.
        with open(os.environ["STUB_LOG"], "a") as f:
            f.write("compat_sync MISSING --enforce\\n")
        sys.exit(2)
    sys.exit(1 if os.environ.get("STUB_COMPAT_SYNC_FAIL") == "1" else 0)
""")

_STUB_DEPS_AUDIT = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    sys.exit(1 if os.environ.get("STUB_DEPS_AUDIT_FAIL") == "1" else 0)
""")

_STUB_DIFF_COVER = textwrap.dedent("""\
    #!/usr/bin/env bash
    if [ "${STUB_DIFF_COVER_FAIL:-0}" = "1" ]; then
        echo "stub diff-cover: simulated failure" >&2
        exit 1
    fi
    exit 0
""")


def _stub_versioning_json() -> str:
    return '{"surfaces": {}, "skills_dir": ".agents/skills"}\n'


def _stub_compat_path_map() -> str:
    return '{"compat-schema-version": "0.1", "schema_version": 1, "paths": {}}\n'


class HookOrchestrationFixture:
    """Reusable scaffold: minimal git repo with stub gate scripts and a
    copy of the real .githooks/pre-push.

    Each test can:
      - configure pass/fail of each stub via env vars
      - invoke the hook directly (it just reads stdin like git would)
      - assert the hook's exit code under different bypass settings
    """

    def __init__(self, root: Path) -> None:
        self.root = root
        self.hook_path = root / ".githooks" / "pre-push"
        self.log = root / "stub.log"

    def init(self) -> None:
        r = self.root
        _git(r, "init", "-q", "-b", "main")
        _git(r, "config", "user.email", "test@example.com")
        _git(r, "config", "user.name", "Test")
        _git(r, "config", "commit.gpgsign", "false")

        (r / ".githooks").mkdir()
        shutil.copy2(HOOK, self.hook_path)
        os.chmod(self.hook_path, 0o755)

        scripts = r / "tools" / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "skill_sync_check.py").write_text(_STUB_SKILL_SYNC)
        (scripts / "version_bump_check.py").write_text(_STUB_VERSION_BUMP)
        (scripts / "compat_sync_check.py").write_text(_STUB_COMPAT_SYNC)
        (scripts / "versioning.json").write_text(_stub_versioning_json())
        (scripts / "compat_path_map.json").write_text(_stub_compat_path_map())
        (scripts / "local_diff_cover.sh").write_text(_STUB_DIFF_COVER)
        for s in (
            "skill_sync_check.py", "version_bump_check.py",
            "compat_sync_check.py",
        ):
            os.chmod(scripts / s, 0o755)
        os.chmod(scripts / "local_diff_cover.sh", 0o755)

        deps = r / "tools" / "deps"
        deps.mkdir(parents=True)
        (deps / "audit.py").write_text(_STUB_DEPS_AUDIT)
        os.chmod(deps / "audit.py", 0o755)

        # Seed an initial commit + origin/main ref so the hook's
        # `git diff $BASE...HEAD` resolves.
        (r / "README.md").write_text("# fixture\n")
        _git(r, "add", "-A")
        _git(r, "commit", "-q", "-m", "initial")
        _git(r, "update-ref", "refs/remotes/origin/main", "HEAD")

        # Add a touch in core/ so the diff-cover stub is invoked when we
        # ask for it (the real hook only runs the diff-cover script when
        # the diff touches core/, tools/cli/, or tools/scripts/).
        (r / "core").mkdir()
        (r / "core" / "thing.cpp").write_text("// touched\n")
        _git(r, "add", "-A")
        _git(r, "commit", "-q", "-m", "touch core/")

    def run_hook(self, env_overrides: dict[str, str] | None = None
                 ) -> subprocess.CompletedProcess:
        env = {k: v for k, v in os.environ.items()
               if k not in (
                   "PULP_ENFORCE_PREPUSH",
                   "PULP_ENFORCE_PREPUSH_DIFF_COVER",
                   "PULP_DISABLE_PREPUSH_GATES",
                   "PULP_DISABLE_PREPUSH_DIFF_COVER",
                   "PULP_SKIP_PREPUSH",
                   "PULP_SKIP_DIFF_COVER",
                   "STUB_SKILL_SYNC_FAIL", "STUB_VERSION_BUMP_FAIL",
                   "STUB_COMPAT_SYNC_FAIL", "STUB_COMPAT_SYNC_REQUIRE_ENFORCE",
                   "STUB_DEPS_AUDIT_FAIL", "STUB_DIFF_COVER_FAIL",
               )}
        env["STUB_LOG"] = str(self.log)
        if env_overrides:
            env.update(env_overrides)
        # The hook reads stdin like `git push` would; we just feed it
        # nothing (it doesn't actually consume the stream).
        return subprocess.run(
            ["bash", str(self.hook_path)],
            cwd=self.root, capture_output=True, text=True,
            env=env, timeout=60, input="",
        )


class HookOrchestrationTests(unittest.TestCase):
    """Integration: run the real hook against stub gate scripts.

    Every test here would FAIL if the flip introduced in #1144 is
    reverted — that's the whole point of the file (#290 contract).
    """

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-prepush-test-"))
        self.fixture = HookOrchestrationFixture(self.tmp)
        self.fixture.init()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ── default-blocks-by-gate tests (the core of the flip) ──

    def test_skill_sync_violation_blocks_by_default(self) -> None:
        result = self.fixture.run_hook({"STUB_SKILL_SYNC_FAIL": "1"})
        self.assertEqual(
            result.returncode, 1,
            f"skill-sync failure must BLOCK by default; got rc={result.returncode}\n"
            f"stderr: {result.stderr}",
        )
        self.assertIn("BLOCKING push", result.stderr)
        self.assertIn("skill-sync", result.stderr)

    def test_version_bump_violation_blocks_by_default(self) -> None:
        result = self.fixture.run_hook({"STUB_VERSION_BUMP_FAIL": "1"})
        self.assertEqual(
            result.returncode, 1,
            f"version-bump failure must BLOCK by default; got rc={result.returncode}",
        )
        self.assertIn("version-bump", result.stderr)

    def test_compat_sync_violation_blocks_by_default(self) -> None:
        result = self.fixture.run_hook({
            "STUB_COMPAT_SYNC_FAIL": "1",
            # Verify the hook passes --enforce so the script's own
            # advisory-by-default behavior doesn't undermine the flip.
            "STUB_COMPAT_SYNC_REQUIRE_ENFORCE": "1",
        })
        self.assertEqual(
            result.returncode, 1,
            f"compat-sync failure must BLOCK by default; got rc={result.returncode}\n"
            f"stderr: {result.stderr}",
        )
        self.assertIn("compat-sync", result.stderr)
        # The stub records to a log if --enforce was missing.
        log_text = self.fixture.log.read_text() if self.fixture.log.exists() else ""
        self.assertNotIn(
            "compat_sync MISSING --enforce", log_text,
            "hook must pass --enforce to compat_sync_check.py — without it "
            "the script silently exits 0 on drift and the flip is "
            "undermined for that gate",
        )

    def test_deps_audit_violation_blocks_by_default(self) -> None:
        result = self.fixture.run_hook({"STUB_DEPS_AUDIT_FAIL": "1"})
        self.assertEqual(
            result.returncode, 1,
            f"deps-audit failure must BLOCK by default; got rc={result.returncode}",
        )
        self.assertIn("deps-audit", result.stderr)

    def test_diff_cover_violation_blocks_by_default(self) -> None:
        result = self.fixture.run_hook({"STUB_DIFF_COVER_FAIL": "1"})
        self.assertEqual(
            result.returncode, 1,
            f"diff-cover failure must BLOCK by default; got rc={result.returncode}\n"
            f"stderr: {result.stderr}",
        )
        self.assertIn("diff-cover", result.stderr)

    # ── opt-out tests (env vars demote / skip) ──

    def test_disable_var_demotes_to_advisory(self) -> None:
        result = self.fixture.run_hook({
            "STUB_SKILL_SYNC_FAIL": "1",
            "PULP_DISABLE_PREPUSH_GATES": "1",
        })
        self.assertEqual(
            result.returncode, 0,
            f"PULP_DISABLE_PREPUSH_GATES=1 must demote to advisory; "
            f"got rc={result.returncode}\nstderr: {result.stderr}",
        )
        self.assertIn("DEMOTED", result.stderr)

    def test_diff_cover_disable_var_demotes_only_diff_cover(self) -> None:
        # diff-cover failure with PULP_DISABLE_PREPUSH_DIFF_COVER=1
        # MUST be demoted (the diff-cover gate is its own knob).
        result = self.fixture.run_hook({
            "STUB_DIFF_COVER_FAIL": "1",
            "PULP_DISABLE_PREPUSH_DIFF_COVER": "1",
        })
        self.assertEqual(
            result.returncode, 0,
            f"PULP_DISABLE_PREPUSH_DIFF_COVER=1 must demote diff-cover; "
            f"got rc={result.returncode}\nstderr: {result.stderr}",
        )

    def test_diff_cover_disable_var_does_not_demote_other_gates(self) -> None:
        # PULP_DISABLE_PREPUSH_DIFF_COVER must NOT demote skill-sync or
        # any other gate — it's diff-cover specific.
        result = self.fixture.run_hook({
            "STUB_SKILL_SYNC_FAIL": "1",
            "PULP_DISABLE_PREPUSH_DIFF_COVER": "1",
        })
        self.assertEqual(
            result.returncode, 1,
            f"PULP_DISABLE_PREPUSH_DIFF_COVER must not demote skill-sync; "
            f"got rc={result.returncode}\nstderr: {result.stderr}",
        )

    def test_skip_var_short_circuits_all_gates(self) -> None:
        # PULP_SKIP_PREPUSH=1 must short-circuit before any gate runs.
        # Even with every stub set to fail, the hook should exit 0.
        result = self.fixture.run_hook({
            "STUB_SKILL_SYNC_FAIL": "1",
            "STUB_VERSION_BUMP_FAIL": "1",
            "STUB_COMPAT_SYNC_FAIL": "1",
            "STUB_DEPS_AUDIT_FAIL": "1",
            "STUB_DIFF_COVER_FAIL": "1",
            "PULP_SKIP_PREPUSH": "1",
        })
        self.assertEqual(
            result.returncode, 0,
            f"PULP_SKIP_PREPUSH=1 must short-circuit; "
            f"got rc={result.returncode}\nstderr: {result.stderr}",
        )
        self.assertIn("PULP_SKIP_PREPUSH", result.stderr)

    # ── happy path ──

    def test_clean_run_exits_zero(self) -> None:
        # No stubs set to fail — hook should report clean and exit 0.
        result = self.fixture.run_hook({})
        self.assertEqual(
            result.returncode, 0,
            f"clean run must exit 0; got rc={result.returncode}\n"
            f"stderr: {result.stderr}",
        )
        self.assertIn("gates clean", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
