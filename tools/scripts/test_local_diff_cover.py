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
import tempfile
import time
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
        # macOS developer shells often expose llvm tools through xcrun
        # rather than PATH; the local gate should make that automatic.
        self.assertIn("xcrun -f llvm-cov", text)
        self.assertLess(
            text.index("xcrun -f llvm-cov"),
            text.index("for tool in clang llvm-profdata llvm-cov"),
            "xcrun path discovery must happen before the dependency loop.",
        )


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

        Per PR #1152 — the previous `key in text` check matched
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
            f"(see PR #1152).",
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

    def test_test_binaries_are_discovered_before_static_archives(self) -> None:
        """Duplicate maps must let executed test binaries win.

        Production files linked into both libpulp-*.a and a test executable
        can otherwise report the archive's zero-hit map even when CTest
        exercised the code. The CI and local scripts should keep test
        binaries first, then archive-only maps.
        """

        for path in (SCRIPT, CI_SCRIPT):
            text = path.read_text()
            archive_idx = text.index("libpulp-*.a")
            tests_idx = text.index('"${BUILD_DIR}/test"')
            self.assertLess(
                tests_idx,
                archive_idx,
                f"{path} must discover test binaries before static archives",
            )


class TargetedCtestTests(unittest.TestCase):
    """Targeted local diff-cover runs must also limit the ctest pass."""

    def test_script_supports_ctest_regex_selector(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            "PULP_DIFF_COVER_CTEST_REGEX",
            text,
            "local_diff_cover.sh needs an env selector so focused parser "
            "coverage runs don't execute the entire CTest registry.",
        )
        self.assertIn(
            'CTEST_ARGS+=(-R "${PULP_DIFF_COVER_CTEST_REGEX}")',
            text,
            "local_diff_cover.sh must pass the regex through ctest -R.",
        )
        self.assertIn(
            'ctest "${CTEST_ARGS[@]}"',
            text,
            "local_diff_cover.sh should assemble ctest arguments once so "
            "the regex selector and default full-suite path share the same call.",
        )

    def test_profraw_cleanup_does_not_expand_a_large_glob(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            'find "${PROFRAW_DIR}" -name \'*.profraw\' -type f -delete',
            text,
            "local_diff_cover.sh must delete stale profraw files via find so "
            "a previous full run with thousands of profiles cannot hit ARG_MAX.",
        )
        self.assertNotIn(
            'rm -f "${PROFRAW_DIR}"/*.profraw',
            text,
            "rm over a profraw glob fails with 'Argument list too long' after "
            "large full-suite coverage runs.",
        )

    def test_profraw_pattern_merges_by_instrumented_binary(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            'LLVM_PROFILE_FILE="${PROFRAW_DIR}/pulp-%m.profraw"',
            text,
            "local_diff_cover.sh should use LLVM's module-signature merge "
            "placeholder so thousands of one-test Catch2 runs merge per binary.",
        )
        self.assertNotIn(
            "pulp-%p-%m.profraw",
            text,
            "per-PID profraw files are too noisy for the full CTest registry "
            "and can lose attribution when PIDs recycle.",
        )


class CoverageConfigureTests(unittest.TestCase):
    """Coverage configure should stay headless-safe on non-Skia worktrees."""

    def test_local_diff_cover_disables_gpu_examples(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            "-DPULP_ENABLE_GPU=OFF",
            text,
            "local_diff_cover.sh must not require Skia/GPU examples before "
            "it can measure coverage for core/tooling diffs.",
        )
        self.assertIn(
            "-DPULP_BUILD_EXAMPLES=OFF",
            text,
            "local_diff_cover.sh should build tests, not example apps, for "
            "the local diff-cover gate.",
        )


class ImporterCoverageAutoInclusionTests(unittest.TestCase):
    """Importer CLI test targets are wired into the targeted diff-cover lane.

    The framework-importer SDK CLI helpers (tools/cli/import_*.cpp,
    importer_install.cpp, tool_registry.cpp) are in the measured diff-coverage
    surface but are exercised by pure-in-process Catch2 tests whose binaries
    compile those .cpp files directly. Those cases use lowercase Catch2 titles
    ("import emit …", "tool registry …", "sha256 …", "install from …") that no
    `-R Import` regex matches, so a targeted local_diff_cover.sh run narrowed by
    PULP_DIFF_COVER_CTEST_REGEX silently dropped them and the diff-cover gate
    reported 0% patch coverage on every importer PR. This locks the auto-
    inclusion in so the script can't regress back to the 0%-patch state.
    """

    REQUIRED_TARGETS = (
        "pulp-test-cli-import",
        "pulp-test-cli-import-emit",
        "pulp-test-cli-import-terms",
        "pulp-test-cli-tool-registry",
        "pulp-test-cli-importer-install",
    )

    def test_importer_test_targets_are_named(self) -> None:
        text = SCRIPT.read_text()
        for target in self.REQUIRED_TARGETS:
            self.assertIn(
                target,
                text,
                f"local_diff_cover.sh must name the importer test target "
                f"{target!r} so a targeted importer build attributes its "
                f"in-process coverage (otherwise the diff-cover gate reads 0% "
                f"patch coverage on importer PRs).",
            )

    def test_importer_paths_trigger_auto_inclusion(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            "IMPORTER_COVERAGE_PATHS_REGEX",
            text,
            "local_diff_cover.sh must detect importer CLI source changes to "
            "decide when to add the importer test targets.",
        )
        # The detection must cover the helper TUs the tests actually link.
        for token in ("import_", "importer_install", "tool_registry"):
            self.assertIn(
                token,
                text,
                f"importer path-detection regex must include {token!r}.",
            )

    def test_importer_ctest_pass_is_case_correct(self) -> None:
        text = SCRIPT.read_text()
        self.assertIn(
            "IMPORTER_COVERAGE_CTEST_REGEX",
            text,
            "local_diff_cover.sh must define the importer ctest case regex.",
        )
        # The Catch2 case titles are lowercase — guard against an uppercase
        # regex that matches nothing (the original 0%-patch root cause).
        self.assertIn("import ", text)
        self.assertIn("tool registry ", text)
        self.assertIn("sha256 ", text)

    def test_importer_diff_detection_includes_working_tree(self) -> None:
        # diff-cover measures "<base>...HEAD plus staged and unstaged
        # changes", so a working-tree-only edit to an importer file (the
        # common pre-push case) must still trip auto-inclusion. Assert the
        # script unions a working-tree diff, not only the committed range.
        text = SCRIPT.read_text()
        self.assertIn(
            'git diff --name-only --cached "${COMPARE_BRANCH}"',
            text,
            "importer detection must include staged changes.",
        )
        self.assertIn(
            'git diff --name-only "${COMPARE_BRANCH}"',
            text,
            "importer detection must include unstaged working-tree changes "
            "so a pre-push importer edit is detected before it is committed.",
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


def _fake_worktree(tmp: pathlib.Path, name: str) -> pathlib.Path:
    """Stage a <root>/tools/scripts/local_diff_cover.sh copy and return <root>.

    The script derives every path it owns from its own location, so a copy in a
    throwaway tree behaves like a real worktree without a checkout or a build.
    """
    root = tmp / name
    (root / "tools" / "scripts").mkdir(parents=True)
    dest = root / "tools" / "scripts" / "local_diff_cover.sh"
    dest.write_text(SCRIPT.read_text())
    return root


def _lib_eval(root: pathlib.Path, snippet: str, timeout: float = 30,
              env_extra: dict | None = None) -> subprocess.CompletedProcess:
    """Source the script in lib-only mode and run `snippet` against its helpers."""
    env = os.environ.copy()
    env["PULP_DIFF_COVER_LIB_ONLY"] = "1"
    env.pop("PULP_DIFF_COVER_HTML_REPORT", None)
    if env_extra:
        env.update(env_extra)
    script = root / "tools" / "scripts" / "local_diff_cover.sh"
    return subprocess.run(
        ["bash", "-c", f'source "{script}"\n{snippet}'],
        env=env, capture_output=True, text=True, timeout=timeout,
    )


class ReportPathTests(unittest.TestCase):
    """The HTML report path is per-worktree, not a shared $TMPDIR mailbox."""

    def test_report_path_is_not_a_shared_tmpdir_file(self) -> None:
        # A fixed name under $TMPDIR (which is per-USER) is the collision:
        # concurrent runs in other worktrees clobber it, and the loser reads
        # someone else's coverage as evidence for their own change.
        out = _lib_eval(REPO_ROOT, 'echo "$HTML_REPORT"').stdout.strip()
        tmpdir = os.environ.get("TMPDIR", "/tmp")
        # normpath collapses the `//` a trailing-slash $TMPDIR produces —
        # without it this compare silently passes against the very path it
        # is meant to reject.
        self.assertNotEqual(
            os.path.normpath(out),
            os.path.normpath(os.path.join(tmpdir, "diff-cover.html")),
            "report path is a fixed per-user $TMPDIR file — concurrent runs "
            "in other worktrees overwrite it",
        )

    def test_report_path_differs_between_worktrees(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            a = _lib_eval(_fake_worktree(tmp, "wt-a"), 'echo "$HTML_REPORT"')
            b = _lib_eval(_fake_worktree(tmp, "wt-b"), 'echo "$HTML_REPORT"')
        self.assertNotEqual(
            a.stdout.strip(), b.stdout.strip(),
            "two worktrees resolve the same report path — one run's report "
            "would overwrite the other's",
        )

    def test_report_path_is_under_the_worktree_build_cov(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = _fake_worktree(pathlib.Path(td), "wt")
            out = _lib_eval(root, 'echo "$HTML_REPORT"').stdout.strip()
        # build-cov is per-worktree, single-writer under the lock, and already
        # reclaimed by clean_build_cov.sh — so it leaves no tmp litter.
        self.assertTrue(
            out.startswith(str(root / "build-cov")),
            f"expected report under the worktree's build-cov, got {out!r}",
        )

    def test_report_path_env_override_is_honored(self) -> None:
        out = _lib_eval(
            REPO_ROOT, 'echo "$HTML_REPORT"',
            env_extra={"PULP_DIFF_COVER_HTML_REPORT": "/tmp/custom-report.html"},
        ).stdout.strip()
        self.assertEqual(out, "/tmp/custom-report.html")

    def test_script_prints_the_report_path_on_both_outcomes(self) -> None:
        # Whoever runs this needs to find the report it actually wrote — on a
        # pass and on a failure.
        text = SCRIPT.read_text()
        self.assertEqual(
            text.count('HTML report: ${HTML_REPORT}'), 2,
            "the script must print the report path it wrote on both the "
            "pass and the fail path",
        )


class BuildCovLockTests(unittest.TestCase):
    """build-cov is single-writer: concurrent runs serialize, never interleave."""

    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.root = _fake_worktree(pathlib.Path(self._td.name), "wt")
        self.lock = self.root / "build-cov.lock"

    def tearDown(self) -> None:
        self._td.cleanup()

    def test_release_is_armed_before_the_owner_pid_is_written(self) -> None:
        # Writing the pid can fail on its own (a full disk — build-cov
        # accumulates, and that is how it shows up here). Under `set -e` that
        # exits the script, so the trap must already be armed or the run
        # strands a pid-less lock no later run can prove is dead.
        text = SCRIPT.read_text()
        self.assertLess(
            text.index("trap release_build_cov_lock EXIT"),
            text.index('echo "$$" > "${BUILD_COV_LOCK}/pid"'),
            "the EXIT trap must be armed before the pid write that can fail",
        )

    def test_acquire_creates_lock_and_records_owner_pid(self) -> None:
        r = _lib_eval(self.root, 'acquire_build_cov_lock; cat "$BUILD_COV_LOCK/pid"')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout.strip(), r"^\d+$")

    def test_lock_is_released_on_normal_exit(self) -> None:
        r = _lib_eval(self.root, 'acquire_build_cov_lock; exit 0')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self.lock.exists(), "EXIT trap did not release the lock")

    def test_lock_is_released_on_failing_exit(self) -> None:
        # A run that fails (coverage below threshold, a build error) must not
        # strand the lock and wedge the next run.
        r = _lib_eval(self.root, 'acquire_build_cov_lock; exit 1')
        self.assertEqual(r.returncode, 1)
        self.assertFalse(self.lock.exists(), "EXIT trap did not fire on failure")

    def test_second_run_waits_for_the_first(self) -> None:
        script = self.root / "tools" / "scripts" / "local_diff_cover.sh"
        env = os.environ.copy()
        env["PULP_DIFF_COVER_LIB_ONLY"] = "1"
        marker = self.root / "holder-done"

        # Holder takes the lock, sleeps, then releases via its EXIT trap.
        holder = subprocess.Popen(
            ["bash", "-c",
             f'source "{script}"\nacquire_build_cov_lock\nsleep 3\n'
             f'touch "{marker}"'],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.time() + 10
            while not self.lock.exists() and time.time() < deadline:
                time.sleep(0.05)
            self.assertTrue(self.lock.exists(), "holder never took the lock")

            waiter = subprocess.run(
                ["bash", "-c",
                 f'source "{script}"\nacquire_build_cov_lock\n'
                 f'test -f "{marker}" && echo SERIALIZED || echo RACED'],
                env=env, capture_output=True, text=True, timeout=30,
            )
        finally:
            holder.communicate(timeout=15)

        self.assertIn("waiting for another diff-coverage run", waiter.stderr)
        self.assertIn(
            "SERIALIZED", waiter.stdout,
            "the second run proceeded while the first still held the lock — "
            "both would have written the same build-cov",
        )

    def test_stale_lock_from_a_dead_owner_is_reclaimed(self) -> None:
        # A run killed with SIGKILL never fires its EXIT trap. The lock it
        # leaves must not wedge every later run forever.
        dead = subprocess.Popen(["true"])
        dead.wait()
        self.lock.mkdir(parents=True)
        (self.lock / "pid").write_text(str(dead.pid))

        r = _lib_eval(self.root, 'acquire_build_cov_lock; echo ACQUIRED', timeout=30)
        self.assertIn("ACQUIRED", r.stdout, r.stderr)
        self.assertIn("reclaiming stale lock", r.stderr)

    def test_live_lock_is_not_reclaimed(self) -> None:
        # Reclaim must prove the owner is dead. This test process is alive, so
        # a run seeing our pid must wait, not steal the lock.
        self.lock.mkdir(parents=True)
        (self.lock / "pid").write_text(str(os.getpid()))

        with self.assertRaises(subprocess.TimeoutExpired):
            _lib_eval(self.root, 'acquire_build_cov_lock; echo STOLE', timeout=6)
        self.assertEqual(
            (self.lock / "pid").read_text(), str(os.getpid()),
            "a live owner's lock was reclaimed",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
