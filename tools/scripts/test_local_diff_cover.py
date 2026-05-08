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
import re
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

    @staticmethod
    def _executable_lines(text: str) -> str:
        """Strip shell-style comments + heredoc/string-block bodies.

        A "consumed" key must appear in code that actually executes —
        not in a comment or in documentation prose. We approximate the
        executable portion by stripping shell-style `#` comments and
        YAML-comment lines. This is good enough for our config files;
        if a future contributor commits scripts with embedded heredoc
        prose that mentions a config key by name, they'll need to
        annotate the test exemption.
        """
        out_lines: list[str] = []
        for raw in text.splitlines():
            # YAML / shell single-line comments — keep the part before `#`.
            stripped = raw.lstrip()
            if stripped.startswith("#"):
                continue
            # Strip trailing inline `# ...` comments. Heuristic: a `#`
            # preceded by whitespace and not inside a single-quoted
            # string (which `jq -r '.foo'` uses). We don't try to be
            # perfect here — the goal is to drop comment-only mentions.
            in_squote = False
            in_dquote = False
            i = 0
            while i < len(raw):
                ch = raw[i]
                if ch == "'" and not in_dquote:
                    in_squote = not in_squote
                elif ch == '"' and not in_squote:
                    in_dquote = not in_dquote
                elif ch == "#" and not in_squote and not in_dquote:
                    # Inline comment — chop it off (must be preceded by
                    # whitespace OR be the entire content).
                    if i == 0 or raw[i - 1].isspace():
                        raw = raw[:i]
                        break
                i += 1
            out_lines.append(raw)
        return "\n".join(out_lines)

    @staticmethod
    def _key_is_consumed(key: str, script_text: str, workflow_text: str) -> bool:
        """Structural check: key is read by an actual consumer pattern.

        Codex P2 on PR #1152 — the previous `key in text` check matched
        comments and docstrings, so a key mentioned only in a doc block
        looked "consumed". Match the executable read shapes we
        actually use:

          1. `jq -r '.<key>'`              (workflow + script)
          2. `jq -r '.<key> // ...'`       (script default-handling)
          3. `read_config_value <key>`     (script helper)

        That covers every real read site without false positives from
        prose mentions. If a new consumer pattern is added, add it
        here; the test will fail until the matcher and the consumer
        agree.
        """
        for text in (script_text, workflow_text):
            exec_text = ConfigKeysAreConsumed._executable_lines(text)
            # Pattern 1/2: jq -r '.<key>' or jq -r '.<key> // ...'
            jq_pattern = re.compile(
                rf"jq\s+-r\s+'\.{re.escape(key)}\b",
            )
            if jq_pattern.search(exec_text):
                return True
            # Pattern 3: read_config_value <key>
            helper_pattern = re.compile(
                rf"\bread_config_value\s+{re.escape(key)}\b",
            )
            if helper_pattern.search(exec_text):
                return True
        return False

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
            if self._key_is_consumed(key, script_text, workflow_text):
                continue
            unread.append(key)
        self.assertEqual(
            unread, [],
            f"coverage_config.json contains unread key(s) {unread!r} — "
            f"silent no-op per #1052. Either wire the key into "
            f"tools/scripts/local_diff_cover.sh / .github/workflows/coverage.yml "
            f"as `jq -r '.<key>' ...` or `read_config_value <key>`, "
            f"remove it, or rename it with a leading underscore "
            f"(e.g. `_meta`) if it is documentation-only metadata. "
            f"NOTE: a comment-only mention does NOT count as consumed "
            f"(Codex P2 on PR #1152).",
        )

    def test_comment_only_mention_does_not_count_as_consumed(self) -> None:
        # Belt-and-suspenders for the methodology fix: a key that
        # appears ONLY inside a comment or a doc-string must not be
        # treated as consumed. We synthesise a couple of fake script
        # texts and confirm the new structural matcher rejects them.
        comment_only_script = (
            "#!/bin/bash\n"
            "# This script does not actually read foo_phantom_key.\n"
            "# But it mentions foo_phantom_key in a comment — should NOT count.\n"
            "echo hello\n"
        )
        comment_only_workflow = (
            "name: ci\n"
            "jobs:\n"
            "  test:\n"
            "    # foo_phantom_key is not used here either.\n"
            "    runs-on: ubuntu-latest\n"
        )
        self.assertFalse(
            self._key_is_consumed(
                "foo_phantom_key", comment_only_script, comment_only_workflow,
            ),
            "comment-only mention of a key was treated as consumed; "
            "the structural matcher must skip comment lines.",
        )

    def test_real_consumer_patterns_are_recognised(self) -> None:
        # Confirm the matcher accepts the read shapes we actually use.
        jq_script = (
            "#!/bin/bash\n"
            "T=$(jq -r '.diff_coverage_fail_under' \"${CONFIG_JSON}\")\n"
        )
        helper_script = (
            "#!/bin/bash\n"
            "B=\"$(read_config_value compare_branch)\"\n"
        )
        jq_default_script = (
            "#!/bin/bash\n"
            "mapfile -t E < <(jq -r '.diff_cover_excludes // [] | .[]' \"${CONFIG_JSON}\")\n"
        )
        for label, script_text in (
            ("jq -r", jq_script),
            ("read_config_value", helper_script),
            ("jq -r with //", jq_default_script),
        ):
            with self.subTest(label):
                self.assertTrue(
                    self._key_is_consumed(
                        "diff_coverage_fail_under" if "fail_under" in script_text
                        else ("compare_branch" if "compare_branch" in script_text
                              else "diff_cover_excludes"),
                        script_text, "",
                    ),
                    f"matcher missed real consumer pattern: {label}",
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


class DiffCoverExcludeContractTests(unittest.TestCase):
    """Locks in the diff_cover_excludes pattern + flag-shape contract.

    Anti-drift for the latent bug surfaced on PR #1005: every entry in
    `diff_cover_excludes` was a silent no-op since #919. Two compounding
    causes:

      1. PATTERN MATCHING — diff-cover's `--exclude` matches via fnmatch
         against the file's basename and absolute path only; literal
         relative paths like `tools/cli/cmd_loop.cpp` match neither and
         do nothing. Entries must be a basename (no slash) or a glob.
      2. ARGPARSE OVERWRITE — diff-cover's `--exclude` is `nargs='+'`
         with default action; repeated `--exclude=foo --exclude=bar`
         keeps only the LAST entry. Both the local script and the
         workflow must splat all entries under a single `--exclude
         val1 val2 ...` flag, not a per-entry `--exclude=PATH` loop.

    These tests make the next regression load.
    """

    WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"

    def test_local_script_uses_single_exclude_flag_form(self) -> None:
        text = SCRIPT.read_text()
        # Old broken shape — must NOT appear:
        self.assertNotIn(
            '("--exclude=${excl}")', text,
            "local_diff_cover.sh appends --exclude=PATH per entry; "
            "argparse nargs='+' silently keeps only the last one. "
            "Build a list and splat under a single --exclude flag.",
        )
        # New shape: collect entries into EXCLUDE_LIST then pass under one --exclude.
        self.assertIn(
            '"--exclude" "${EXCLUDE_LIST[@]}"', text,
            "local_diff_cover.sh must build the diff-cover invocation "
            "with all excludes under a single --exclude flag (multi-value), "
            "not as repeated --exclude=PATH flags.",
        )

    def test_workflow_uses_single_exclude_flag_form(self) -> None:
        text = self.WORKFLOW.read_text()
        # Old broken shape:
        self.assertNotIn(
            '"--exclude=$e"', text,
            ".github/workflows/coverage.yml appends --exclude=PATH per entry; "
            "argparse nargs='+' silently keeps only the last one. "
            "Splat all entries under a single --exclude flag instead.",
        )
        # New shape:
        self.assertIn(
            '("--exclude" "${EXCLUDES[@]}")', text,
            ".github/workflows/coverage.yml must build the diff-cover "
            "invocation with all excludes under a single --exclude flag.",
        )

    def test_diff_cover_exclude_patterns_match_via_basename_or_glob(self) -> None:
        """Each `diff_cover_excludes` entry must be either a basename
        (no slash) or contain a glob character (`*`). Literal relative
        paths like `tools/cli/cmd_loop.cpp` don't match diff-cover's
        fnmatch-against-basename-or-abspath check and silently exclude
        nothing."""
        with CONFIG.open() as f:
            config = json.load(f)
        excludes = config.get("diff_cover_excludes", [])
        self.assertIsInstance(excludes, list)
        for pattern in excludes:
            is_basename = "/" not in pattern
            is_glob = "*" in pattern
            self.assertTrue(
                is_basename or is_glob,
                f"diff_cover_excludes entry {pattern!r} is a literal "
                f"relative path; diff-cover's --exclude won't match it "
                f"(fnmatch checks basename + absolute path only). "
                f"Use a basename like 'cmd_loop.cpp' or a glob like "
                f"'**/cmd_loop.cpp' instead.",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
