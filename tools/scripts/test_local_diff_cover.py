#!/usr/bin/env python3
"""Fixture tests for tools/scripts/local_diff_cover.sh.

We deliberately don't run the full coverage build in these tests —
that's minutes of clang time and the real toolchain already runs in
.github/workflows/coverage.yml. Instead we exercise:

  1. The PULP_SKIP_DIFF_COVER=1 bypass — must exit 0 with the
     documented skip message before doing any work.
  2. The config-driven threshold — assert that coverage_config.json
     parses, the threshold is the documented integer, and the script
     reads the same value via its embedded python helper.
  3. The dependency preflight — when a required binary is missing
     from PATH, the script must exit 2 with a clear message (not
     wedge or run a partial build).

Run:
    python3 tools/scripts/test_local_diff_cover.py
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "local_diff_cover.sh"
CONFIG = REPO_ROOT / "tools" / "scripts" / "coverage_config.json"
CI_SCRIPT = REPO_ROOT / "scripts" / "run_coverage.sh"


class ConfigTests(unittest.TestCase):
    """coverage_config.json is well-formed and exposes the expected keys."""

    def test_config_exists(self) -> None:
        self.assertTrue(CONFIG.exists(), f"missing: {CONFIG}")

    def test_config_parses(self) -> None:
        with CONFIG.open() as f:
            cfg = json.load(f)
        # Required keys consumed by .github/workflows/coverage.yml AND
        # tools/scripts/local_diff_cover.sh — keep them in lockstep.
        self.assertIn("diff_coverage_fail_under", cfg)
        self.assertIn("compare_branch", cfg)
        self.assertIsInstance(cfg["diff_coverage_fail_under"], int)
        self.assertGreaterEqual(cfg["diff_coverage_fail_under"], 0)
        self.assertLessEqual(cfg["diff_coverage_fail_under"], 100)
        self.assertIsInstance(cfg["compare_branch"], str)


class SkipFlagTests(unittest.TestCase):
    """PULP_SKIP_DIFF_COVER=1 short-circuits before any work."""

    def test_skip_flag_exits_zero(self) -> None:
        env = os.environ.copy()
        env["PULP_SKIP_DIFF_COVER"] = "1"
        result = subprocess.run(
            ["bash", str(SCRIPT)],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped via PULP_SKIP_DIFF_COVER=1", result.stderr)

    def test_skip_flag_works_with_args(self) -> None:
        # Even with positional targets, the skip flag must short-circuit.
        env = os.environ.copy()
        env["PULP_SKIP_DIFF_COVER"] = "1"
        result = subprocess.run(
            ["bash", str(SCRIPT), "pulp-test-state", "pulp-test-widget-bridge"],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped via PULP_SKIP_DIFF_COVER=1", result.stderr)


class DependencyPreflightTests(unittest.TestCase):
    """The dep-preflight branch is structured to exit 2 with a clear msg."""

    def test_preflight_exits_two_branch_present(self) -> None:
        # Static check: the script must declare an exit-2 path with a
        # "missing required deps" message. We can't reliably hide clang
        # from PATH on macOS (clang lives in /usr/bin alongside dirname
        # and other essentials this script needs), so we assert the
        # exit-2 contract is present in the source.
        text = SCRIPT.read_text()
        self.assertIn("missing required deps", text)
        self.assertIn("exit 2", text)
        # Must check for the canonical Clang toolchain.
        self.assertRegex(text, r"\bclang\b")
        self.assertRegex(text, r"\bllvm-cov\b")
        self.assertRegex(text, r"\bllvm-profdata\b")
        # Must check for diff-cover importability via python3.
        self.assertIn("import diff_cover", text)


class WorkflowSourceOfTruthTests(unittest.TestCase):
    """coverage.yml reads --fail-under from coverage_config.json (anti-drift)."""

    WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"

    def test_workflow_reads_threshold_from_json(self) -> None:
        text = self.WORKFLOW.read_text()
        # The workflow must extract the threshold via jq from the
        # config file BEFORE invoking diff-cover. If a future edit
        # hardcodes --fail-under=NN again, this test fails loudly.
        self.assertIn(
            "jq -r '.diff_coverage_fail_under' tools/scripts/coverage_config.json",
            text,
            "coverage.yml must read the threshold from coverage_config.json — "
            "do not hardcode --fail-under in the workflow",
        )
        # And the diff-cover step must use the resolved variable, not
        # a literal.
        self.assertIn('--fail-under="${THRESHOLD}"', text)
        # Defense in depth: assert no literal --fail-under=<number>
        # remains in the workflow. A hardcoded literal would silently
        # win over the JSON-driven value.
        import re
        literals = re.findall(r"--fail-under=\d+", text)
        self.assertEqual(
            literals, [],
            f"workflow has hardcoded --fail-under literal(s): {literals}; "
            "remove and read from coverage_config.json instead",
        )


class ConfigKeysAreConsumed(unittest.TestCase):
    """Every non-comment key in coverage_config.json must actually be read.

    Anti-drift for #1052: `filters` and `exclude_filters` lived in this
    JSON unread for months, implying a contract that didn't exist
    (source-set filtering actually happens via COVERAGE_IGNORE_REGEX in
    local_diff_cover.sh / scripts/run_coverage.sh, NOT via this JSON).
    A user who edits `exclude_filters` to skip `external/**` sees no
    effect — silent no-op.

    Contract: every key in coverage_config.json (except `_comment` and
    other underscore-prefixed metadata keys) must appear by name in
    either tools/scripts/local_diff_cover.sh or
    .github/workflows/coverage.yml. If you add a key here without
    wiring it up, this test fails loudly with the offending key and a
    pointer back to this issue.

    To intentionally add documentation-only metadata: prefix the key
    with `_` (e.g. `_meta`, `_doc`) and it will be skipped, matching
    the existing `_comment` convention.
    """

    WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"

    def test_every_config_key_has_a_consumer(self) -> None:
        with CONFIG.open() as f:
            cfg = json.load(f)
        script_text = SCRIPT.read_text()
        workflow_text = self.WORKFLOW.read_text()
        unread = []
        for key in cfg.keys():
            # Documentation-only keys (matching the `_comment` pattern)
            # are exempt — they exist for readers, not consumers.
            if key.startswith("_"):
                continue
            if key in script_text or key in workflow_text:
                continue
            unread.append(key)
        self.assertEqual(
            unread, [],
            f"coverage_config.json contains unread key(s) {unread!r} — "
            f"silent no-op per #1052. Either wire the key into "
            f"tools/scripts/local_diff_cover.sh / .github/workflows/coverage.yml, "
            f"remove it, or rename it with a leading underscore "
            f"(e.g. `_meta`) if it is documentation-only metadata.",
        )

    def test_silent_noop_filter_keys_stay_removed(self) -> None:
        """Belt-and-suspenders: explicit reject for the two keys that
        triggered #1052. Even if a future change adds them back AND
        wires them up partially, this test forces the contributor to
        revisit the audit and decide intentionally."""
        with CONFIG.open() as f:
            cfg = json.load(f)
        for ghost in ("filters", "exclude_filters"):
            self.assertNotIn(
                ghost, cfg,
                f"coverage_config.json must not define `{ghost}` — these "
                f"keys were a silent no-op (#1052). Source-set filtering "
                f"belongs in COVERAGE_IGNORE_REGEX (local_diff_cover.sh / "
                f"scripts/run_coverage.sh), not in this JSON. If you have "
                f"a genuine reason to wire one of these in, update this "
                f"test and link the new design.",
            )


class ObjectDiscoveryParityTests(unittest.TestCase):
    """local_diff_cover.sh must mirror run_coverage.sh's object-set passes.

    Anti-drift guard for #919: the local mirror previously only added
    build/test/* binaries to llvm-cov, so coverage data from CLI
    shell-out tests (cmd_coverage.cpp, cmd_loop.cpp, anything reached
    via pulp-cli / pulp-standalone / pulp-inspect) never propagated.
    The fix lifts run_coverage.sh's wider find passes; this test fails
    if either side drops one.
    """

    REQUIRED_FIND_ROOTS = [
        # (substring that must appear in the script, human description)
        ('"${BUILD_DIR}/test"', "test executables"),
        ("libpulp-*.a", "first-party static archives (Unix)"),
        ("pulp-*.lib", "first-party static archives (Windows)"),
        ('"${BUILD_DIR}/tools"', "non-test executables (CLI / standalone)"),
        ('"${BUILD_DIR}/inspect"', "non-test executables (inspector)"),
        ('"${BUILD_DIR}/bindings"', "loadable first-party modules"),
    ]

    def test_local_script_includes_all_object_roots(self) -> None:
        text = SCRIPT.read_text()
        for needle, desc in self.REQUIRED_FIND_ROOTS:
            self.assertIn(
                needle, text,
                f"local_diff_cover.sh missing object-discovery for {desc} "
                f"({needle!r}); without it llvm-cov drops coverage from "
                f"that surface — see #919.",
            )

    def test_ci_script_still_includes_all_object_roots(self) -> None:
        # If the CI script ever drops one of these, the local mirror is
        # the wrong place to keep it — fix CI first, then update both.
        if not CI_SCRIPT.exists():
            self.skipTest(f"CI script not present: {CI_SCRIPT}")
        text = CI_SCRIPT.read_text()
        for needle, desc in self.REQUIRED_FIND_ROOTS:
            self.assertIn(
                needle, text,
                f"scripts/run_coverage.sh missing object-discovery for "
                f"{desc} ({needle!r}); local_diff_cover.sh mirrors this "
                f"surface set, so dropping it here desyncs both lanes.",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
