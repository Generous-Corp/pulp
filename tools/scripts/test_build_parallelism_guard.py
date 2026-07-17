#!/usr/bin/env python3
"""Tests for build_parallelism_guard.py."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "build_parallelism_guard", HERE / "build_parallelism_guard.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)


def scan(text: str, suffix: str = ".sh") -> list[tuple[int, str]]:
    """Back-compat findings-only scan (bare + any whole-machine on the temp
    file's own — non-shared — path)."""
    with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        return guard.scan_file(path)
    finally:
        path.unlink()


def kinds(text: str, suffix: str = ".sh", shared_host: bool = False) -> list[str]:
    """Return just the finding kinds ("bare"/"whole-machine") for `text`,
    classified as if the surface were (or were not) a shared host."""
    with tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        return [k for _l, _s, k in guard.scan_file_kinds(path, shared_host=shared_host)]
    finally:
        path.unlink()


class BareClassTest(unittest.TestCase):
    """The `bare` class — no job count at all — is wrong on any host."""

    def test_bare_parallel_is_flagged(self):
        self.assertTrue(scan("cmake --build build --parallel\n"))

    def test_bare_dash_j_is_flagged(self):
        self.assertTrue(scan("cmake --build build -j\n"))

    def test_bare_is_flagged_on_ephemeral_and_shared_alike(self):
        self.assertEqual(kinds("cmake --build build --parallel\n", shared_host=False), ["bare"])
        self.assertEqual(kinds("cmake --build build --parallel\n", shared_host=True), ["bare"])

    def test_literal_count_is_bounded(self):
        self.assertFalse(scan("cmake --build build --parallel 8\n"))
        self.assertFalse(scan("cmake --build build -j8\n"))
        self.assertFalse(scan("cmake --build build --parallel=8\n"))

    def test_shell_expansion_is_bounded_shape(self):
        # A `$`-expansion satisfies the *bare* check (it is a count, not empty).
        # Whether it is *whole-machine* is a separate axis, tested below.
        self.assertFalse(scan('cmake --build b -j"$JOBS"\n'))
        self.assertFalse(scan('cmake --build b -j"${JOBS}"\n'))

    def test_cxx_concat_form_is_bounded(self):
        line = '"cmake --build " + dir + " --target install --parallel " + std::to_string(jobs());\n'
        self.assertFalse(scan(line, suffix=".cpp"))

    def test_flag_name_literal_is_not_a_build_command(self):
        self.assertFalse(scan('if (arg == "--parallel" || arg == "-j") {\n', suffix=".cpp"))

    def test_non_build_dash_j_is_ignored(self):
        self.assertFalse(scan('date -j -f "%Y" "$stamp"\n'))

    def test_comment_mentioning_parallel_is_ignored(self):
        self.assertFalse(scan("# a note about cmake --build --parallel with no count\n"))

    def test_powershell_bare_before_semicolon_is_flagged(self):
        self.assertTrue(scan("cmake --build build --config Release --parallel; exit 0\n"))

    def test_bare_parallel_on_continuation_line_is_flagged(self):
        self.assertTrue(scan("cmake --build build \\\n  --parallel\n"))

    def test_bounded_parallel_on_continuation_line_is_ok(self):
        self.assertFalse(scan("cmake --build build \\\n  --parallel 8\n"))

    def test_continuation_reports_the_command_start_line(self):
        findings = scan("echo hi\ncmake --build build \\\n  --parallel\n")
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 2)


class WholeMachineClassTest(unittest.TestCase):
    """The `whole-machine` class — an explicit but undivided core-count read — is
    the melt on a SHARED host, and correct only on a runner that is genuinely
    unshared. The finding is a property of the host, not the command — so the
    guard flags it only where it can classify the surface as shared."""

    WHOLE_MACHINE = [
        "cmake --build build -j$(nproc)\n",
        "cmake --build build -j$(sysctl -n hw.ncpu)\n",
        "cmake --build b --parallel $(getconf _NPROCESSORS_ONLN)\n",
        "cmake --build b --parallel `nproc`\n",
        "cmake --build build --config Release --parallel %NUMBER_OF_PROCESSORS%\n",
    ]

    def test_whole_machine_is_rejected_on_a_shared_host(self):
        # The core assertion: `-j$(nproc)` on CLAUDE.md / .shipyard / a skill.
        for cmd in self.WHOLE_MACHINE:
            self.assertEqual(kinds(cmd, shared_host=True), ["whole-machine"], cmd)

    def test_whole_machine_is_not_flagged_on_an_unclassified_surface(self):
        # The SAME command on a surface the scan does not classify as shared is
        # not flagged. That is a limit of static classification, not a promise
        # the surface is unshared: `.github/workflows/**` lands here, and one of
        # its legs can be self-hosted (see the workflow-author-owns test below).
        for cmd in self.WHOLE_MACHINE:
            self.assertEqual(kinds(cmd, shared_host=False), [], cmd)

    def test_redirection_does_not_mask_whole_machine(self):
        # Regression: the `/` in `2>/dev/null` must not parse as a division and
        # wave the whole-machine command through. This is the exact shape the
        # .shipyard smoke lane used.
        cmd = ("cmake --build build --parallel "
               "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)\n")
        self.assertEqual(kinds(cmd, shared_host=True), ["whole-machine"])

    def test_derived_share_is_accepted_even_on_a_shared_host(self):
        # A core count divided into a share is fine — it asks for a slice.
        for cmd in (
            "cmake --build build -j$(( $(nproc) / 4 ))\n",
            "cmake --build build -j$(( $(sysctl -n hw.ncpu) / 4 ))\n",
        ):
            self.assertEqual(kinds(cmd, shared_host=True), [], cmd)

    def test_literal_count_is_accepted_on_a_shared_host(self):
        self.assertEqual(kinds("cmake --build build -j8\n", shared_host=True), [])

    def test_governed_wrapper_build_is_accepted_everywhere(self):
        # The steer: route the raw build through the governor and carry no -j.
        cmd = "tools/ci/governed-build.sh cmake --build build --target foo\n"
        self.assertEqual(kinds(cmd, shared_host=True), [])
        self.assertEqual(kinds(cmd, shared_host=False), [])

    def test_env_var_expansion_is_not_whole_machine(self):
        # `-j"$JOBS"` is a count from a variable, not a hardware read.
        self.assertEqual(kinds('cmake --build b -j"$JOBS"\n', shared_host=True), [])


class MarkdownScanTest(unittest.TestCase):
    """In a .md surface only fenced *shell* blocks are executable; prose that
    quotes the trap must not self-trip the guard."""

    def test_fenced_shell_command_is_scanned(self):
        md = "text\n```bash\ncmake --build build -j$(nproc)\n```\n"
        self.assertEqual(kinds(md, suffix=".md", shared_host=True), ["whole-machine"])

    def test_prose_mentioning_the_trap_is_not_scanned(self):
        md = "Never write `cmake --build build -j$(nproc)` on a shared Mac.\n"
        self.assertEqual(kinds(md, suffix=".md", shared_host=True), [])

    def test_non_shell_fence_is_not_scanned(self):
        md = "```python\ncmake --build build -j$(nproc)\n```\n"
        self.assertEqual(kinds(md, suffix=".md", shared_host=True), [])

    def test_prose_after_a_non_shell_block_is_not_scanned(self):
        # A non-shell block's closing ``` must not re-open a shell block, or the
        # prose that follows it would be read as commands.
        md = ("```json\n{\"x\": 1}\n```\n"
              "Then, in prose, we mention `cmake --build build -j$(nproc)`.\n")
        self.assertEqual(kinds(md, suffix=".md", shared_host=True), [])

    def test_shell_block_after_a_non_shell_block_still_scans(self):
        md = ("```python\nprint('hi')\n```\n"
              "```bash\ncmake --build build -j$(nproc)\n```\n")
        self.assertEqual(kinds(md, suffix=".md", shared_host=True), ["whole-machine"])


class SharedHostClassificationTest(unittest.TestCase):
    """`is_shared_host_surface` decides which surfaces get the whole-machine
    rule — it is the host/command distinction, expressed as paths."""

    def _p(self, rel: str) -> Path:
        return guard.REPO_ROOT / rel

    def test_shared_host_surfaces(self):
        for rel in (
            "CLAUDE.md",
            ".shipyard/config.toml",
            ".agents/skills/stretch/SKILL.md",
            ".agents/skills/ci/SKILL.md",
        ):
            self.assertTrue(guard.is_shared_host_surface(self._p(rel)), rel)

    def test_unclassifiable_and_other_surfaces_are_not_shared_host(self):
        # A `.github/workflows` file is NOT a shared-host surface for this scan —
        # not because such a leg is always unshared (it is not: build.yml's macOS
        # leg resolves to the shared self-hosted Studios), but because its
        # `runs-on` is dynamic and a static scan cannot resolve it. The other
        # entries are genuinely not copy-from shared surfaces.
        for rel in (
            ".github/workflows/build.yml",
            "tools/scripts/local_diff_cover.sh",
            "setup.sh",
            "ci/some-lane.yml",
        ):
            self.assertFalse(guard.is_shared_host_surface(self._p(rel)), rel)


class WorkflowAuthorOwnsWholeMachineTest(unittest.TestCase):
    """`.github/workflows/**` is not scanned for whole-machine because a file
    scan cannot resolve a dynamic `runs-on` to a specific host. The honest
    contract: the workflow author bounds a self-hosted leg directly. These tests
    pin both halves so the exemption cannot silently regress to a false premise."""

    def test_workflow_whole_machine_is_not_flagged_by_the_scan(self):
        # build.yml's intel-canary leg is macOS-only and resolves to the shared
        # self-hosted Studios; the scan still cannot see that statically, so a
        # whole-machine line in a workflow is not a guard finding.
        step = "cmake --build build-intel-canary -j$(sysctl -n hw.ncpu) --target foo\n"
        self.assertEqual(kinds(step, suffix=".yml", shared_host=False), [])

    def test_the_known_self_hosted_legs_are_bounded_in_the_workflow_files(self):
        # The guard cannot enforce this, so the author must: the self-hosted
        # macOS legs route their build through the governor and carry no
        # whole-machine -j. This asserts that hand-bounding actually landed.
        import re
        root = guard.REPO_ROOT
        for rel in (
            ".github/workflows/build.yml",
            ".github/workflows/format-baseline-diff.yml",
            ".github/workflows/web-plugins.yml",
        ):
            text = (root / rel).read_text(encoding="utf-8")
            # No *executed* whole-machine build command survives (matches in
            # prose/comments are allowed; a real command starts the line).
            for line in text.splitlines():
                stripped = line.strip()
                if stripped.startswith("#"):
                    continue
                if re.search(r'cmake\s+--build.*-j"?\$\(sysctl -n hw\.ncpu\)', stripped):
                    self.fail(f"{rel}: un-governed whole-machine build survives: {stripped}")


class TreeIsCleanTest(unittest.TestCase):
    """The default scan over the real repo surfaces must pass — the guard runs
    as a ctest against the tree, so a stray whole-machine/bare command anywhere
    in a scanned surface reddens the required gate."""

    def test_default_targets_are_all_bounded(self):
        self.assertEqual(guard.main(["build_parallelism_guard.py"]), 0)


if __name__ == "__main__":
    unittest.main()
