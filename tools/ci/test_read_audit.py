#!/usr/bin/env python3
"""Tests for tools/ci/read_audit.py.

The strace logs below are in the shape `strace -f -qq -y` writes, so the
parser, the path resolution and the declaration diff run without strace (the
macOS gate has none). The live instrument is proven by the audit's own control
on every nightly run, which fails the run when the control is not flagged.

    python3 tools/ci/test_read_audit.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import read_audit as ra  # noqa: E402

REPO = "/w/repo"
BUILD = "/w/repo/build"

# ctest (100) spawns the test (101), which chdirs into the checkout, opens a
# declared fixture by a relative path, an undeclared one by an absolute path,
# lists a directory, probes a file, and spawns a helper (102) that inherits the
# directory and opens a third file relatively. ctest's own reads are not the
# test's.
LOG = f"""100   openat(AT_FDCWD</w/repo/build>, "/w/repo/build/CTestTestfile.cmake", O_RDONLY) = 3</w/repo/build/CTestTestfile.cmake>
100   openat(AT_FDCWD</w/repo/build>, "/w/repo/README.md", O_RDONLY) = 3</w/repo/README.md>
100   clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|SIGCHLD <unfinished ...>
101   chdir("/w/repo") = 0
100   <... clone resumed>, child_tidptr=0x7f) = 101
101   execve("/w/repo/build/test/pulp-test-x", ["/w/repo/build/test/pulp-test-x"], 0x7ff /* 3 vars */) = 0
101   openat(AT_FDCWD, "test/fixtures/declared/a.json", O_RDONLY) = 3</w/repo/test/fixtures/declared/a.json>
101   openat(AT_FDCWD, "/w/repo/test/fixtures/other/b.json", O_RDONLY|O_CLOEXEC) = 4</w/repo/test/fixtures/other/b.json>
101   openat(AT_FDCWD, "/w/repo/test/fixtures/other", O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY) = 5</w/repo/test/fixtures/other>
101   newfstatat(AT_FDCWD, "docs/x.md", {{st_mode=S_IFREG|0644, st_size=3, ...}}, 0) = 0
101   openat(AT_FDCWD, "/w/repo/build/generated.h", O_RDONLY) = 6</w/repo/build/generated.h>
101   openat(AT_FDCWD, "/w/repo/tools/cmake/PulpX.cmake", O_RDONLY) = 7</w/repo/tools/cmake/PulpX.cmake>
101   openat(AT_FDCWD, "/w/repo/.git/HEAD", O_RDONLY) = 8</w/repo/.git/HEAD>
101   openat(AT_FDCWD, "/usr/lib/libc.so.6", O_RDONLY|O_CLOEXEC) = 3</usr/lib/libc.so.6>
101   openat(AT_FDCWD, "/w/repo/test/fixtures/missing.json", O_RDONLY) = -1 ENOENT (No such file or directory)
101   vfork( <unfinished ...>
102   execve("/usr/bin/python3", ["python3", "tools/helper.py"], 0x7ff /* 3 vars */) = 0
102   openat(AT_FDCWD, "tools/helper.py", O_RDONLY|O_CLOEXEC) = 3</w/repo/tools/helper.py>
101   <... vfork resumed>) = 102
102   +++ exited with 0 +++
101   +++ exited with 0 +++
"""

FILES = {"README.md", "test/fixtures/declared/a.json", "test/fixtures/other/b.json", "docs/x.md",
         "tools/cmake/PulpX.cmake", "tools/helper.py", "LICENSE.md"}


def parse(text: str, cwd: str = BUILD) -> ra.Trace:
    return ra.parse_strace(text.splitlines(True), cwd)


class ParseTests(unittest.TestCase):
    def test_relative_paths_resolve_against_the_callers_directory(self) -> None:
        t = parse(LOG)
        paths = {(a.pid, a.path) for a in t.accesses}
        self.assertIn((101, "/w/repo/test/fixtures/declared/a.json"), paths)
        # The helper never chdirs: it inherits 101's directory at the vfork.
        self.assertIn((102, "/w/repo/tools/helper.py"), paths)
        self.assertEqual((t.root_pid, t.unresolved), (100, 0))
        self.assertEqual(t.programs[102], "/usr/bin/python3")

    def test_a_relative_path_with_no_known_directory_is_counted_not_guessed(self) -> None:
        # 200 appears with no clone line naming it, so its directory is unknown.
        t = parse('100   openat(AT_FDCWD, "/x", O_RDONLY) = 3\n'
                  '200   openat(AT_FDCWD, "rel/file", O_RDONLY) = 3\n')
        self.assertEqual(t.unresolved, 1)
        self.assertNotIn("rel/file", " ".join(a.path for a in t.accesses))

    def test_a_dirfd_annotation_anchors_its_relative_path(self) -> None:
        t = parse('100   openat(5</w/repo/test>, "fixtures/c.txt", O_RDONLY) = 3\n'
                  '100   newfstatat(7, "orphan", {st_mode=S_IFREG}, 0) = 0\n')
        self.assertEqual([a.path for a in t.accesses], ["/w/repo/test/fixtures/c.txt"])
        self.assertEqual(t.unresolved, 1)

    def test_escaped_strings_decode(self) -> None:
        t = parse('100   openat(AT_FDCWD, "/w/repo/a\\"b\\x41\\101.txt", O_RDONLY) = 3\n')
        self.assertEqual(t.accesses[0].path, '/w/repo/a"bAA.txt')

    def test_a_failed_chdir_keeps_the_directory(self) -> None:
        t = parse('100   chdir("/nope") = -1 ENOENT (No such file or directory)\n'
                  '100   openat(AT_FDCWD, "x", O_RDONLY) = 3\n', cwd="/w/repo")
        self.assertEqual(t.accesses[-1].path, "/w/repo/x")


class DiffTests(unittest.TestCase):
    def accesses(self) -> dict:
        return ra.checkout_accesses(parse(LOG), Path(REPO), [Path(BUILD)], FILES, ra.tracked_dirs(FILES))

    def test_only_the_tests_tracked_checkout_accesses_are_kept(self) -> None:
        got = self.accesses()
        self.assertEqual(got, {
            ("test/fixtures/declared/a.json", "read"): "pulp-test-x",
            ("test/fixtures/other/b.json", "read"): "pulp-test-x",
            ("test/fixtures/other", "listing"): "pulp-test-x",
            ("docs/x.md", "probe"): "pulp-test-x",
            ("tools/cmake/PulpX.cmake", "read"): "pulp-test-x",
            ("tools/helper.py", "read"): "python3",
        })
        # ctest's README.md read, the build tree, .git, system files and a
        # missing path are all absent.

    def test_undeclared_accesses_are_findings_and_declared_or_ruled_ones_are_not(self) -> None:
        found, covered = ra.findings_for(self.accesses(), ["test/fixtures/declared"])
        self.assertEqual([(f.path, f.kind) for f in found], [
            ("docs/x.md", "probe"), ("test/fixtures/other", "listing"),
            ("test/fixtures/other/b.json", "read"), ("tools/helper.py", "read")])
        self.assertEqual(covered, 2)  # the declared fixture and the CMake file

    def test_a_whole_checkout_declaration_covers_every_access(self) -> None:
        found, covered = ra.findings_for(self.accesses(), [], ra.WHOLE_CHECKOUT)
        self.assertEqual((found, covered), ([], 6))
        found, _ = ra.findings_for(self.accesses(), [], "none")
        self.assertEqual(len(found), 5)

    def test_a_glob_declaration_covers_what_the_selector_would(self) -> None:
        found, _ = ra.findings_for({("test/fixtures/other/b.json", "read"): "x"}, ["test/fixtures/*/b.json"])
        self.assertEqual(found, [])

    def test_a_read_outranks_a_probe_of_the_same_file(self) -> None:
        t = parse('100   clone(child_stack=NULL) = 101\n'
                  '101   newfstatat(AT_FDCWD, "/w/repo/docs/x.md", {st_mode=S_IFREG}, 0) = 0\n'
                  '101   openat(AT_FDCWD, "/w/repo/docs/x.md", O_RDONLY) = 3\n')
        got = ra.checkout_accesses(t, Path(REPO), [Path(BUILD)], FILES, ra.tracked_dirs(FILES))
        self.assertEqual(list(got), [("docs/x.md", "read")])


class UnreadablePathTests(unittest.TestCase):
    def test_pseudo_filesystems_and_refused_paths_are_skipped_not_fatal(self) -> None:
        # Another process's /proc/<pid>/cwd raises EACCES on readlink, which
        # os.path.realpath passes up; one such path once ended a whole run.
        t = parse('100   clone(child_stack=NULL) = 101\n'
                  '101   openat(AT_FDCWD, "/proc/1/cwd", O_RDONLY) = -1 EACCES (Permission denied)\n'
                  '101   openat(AT_FDCWD, "/w/repo/docs/x.md", O_RDONLY) = 3\n'
                  '101   openat(AT_FDCWD, "/w/repo/README.md", O_RDONLY) = 3\n')
        real = ra.os.path.realpath

        def refusing(path, *args, **kwargs):
            if str(path).startswith("/proc/") or str(path).endswith("README.md"):
                raise PermissionError(13, "Permission denied", path)
            return real(path, *args, **kwargs)

        with mock.patch.object(ra.os.path, "realpath", refusing):
            got = ra.checkout_accesses(t, Path(REPO), [Path(BUILD)], FILES, ra.tracked_dirs(FILES))
        self.assertEqual(list(got), [("docs/x.md", "read")])


class GroupTests(unittest.TestCase):
    def test_compiled_registrations_group_by_executable_and_scripts_are_counted(self) -> None:
        inv = {"tests": [
            {"name": "a1", "command": [f"{BUILD}/test/pulp-test-a", "a1"]},
            {"name": "a2", "command": [f"{BUILD}/test/pulp-test-a", "a2"]},
            {"name": "b", "command": [f"{BUILD}/test/pulp-test-b"]},
            {"name": "s", "command": ["/usr/bin/python3", f"{REPO}/tools/x.py"]}]}
        groups, scripts = ra.groups_from(inv, Path(BUILD), {"executables": {"pulp-test-a": {"data": "none"}}}, None)
        self.assertEqual([(g.executable, g.tests, g.manifest) for g in groups],
                         [("pulp-test-a", ["a1", "a2"], {"data": "none"}), ("pulp-test-b", ["b"], None)])
        self.assertEqual(scripts, 1)


class SummaryTests(unittest.TestCase):
    def test_the_summary_names_the_test_the_path_and_the_coverage_gaps(self) -> None:
        report = {"control": {"ok": True}, "gaps": {"manifest_not_registered": ["pulp-test-mac"], "script_tests": 3},
                  "totals": {"executables": 2, "tests": 3, "audited": 2, "unobserved": 0, "findings": 1,
                             "executables_with_findings": 1, "finding_reads": 1, "finding_listings": 0,
                             "finding_probes": 0, "covered": 4, "unresolved": 0},
                  "executables": {"pulp-test-x": {"data": "declared", "findings": [
                      {"path": "test/fixtures/other/b.json", "kind": "read", "program": "pulp-test-x",
                       "tests": ["reads b"]}]}}}
        text = ra.summarize(report)
        self.assertIn("| `pulp-test-x` | declared | read | `test/fixtures/other/b.json` | reads b |", text)
        self.assertIn("1 manifest executables this platform does not register (macOS-only tests)", text)
        self.assertIn("3 script-driven tests", text)


class Stage0Tests(unittest.TestCase):
    @staticmethod
    def report(ok=True, **recs) -> dict:
        return {"control": {"ok": ok}, "executables": recs}

    def test_clean_needs_every_declared_executable_audited_and_no_finding(self) -> None:
        audited = {"status": "audited"}
        got = ra.stage0(self.report(a=audited, b=audited, c=audited), {"a", "b", "mac-only"})
        self.assertEqual((got["verdict"], got["declared"], got["declared_audited"], got["declared_clean"]),
                         ("clean", 3, 2, 2))
        self.assertEqual(got["declared_not_registered"], ["mac-only"])

    def test_a_finding_anywhere_is_not_clean(self) -> None:
        bad = {"status": "audited", "findings": [{"path": "x"}]}
        got = ra.stage0(self.report(a={"status": "audited"}, z=bad), {"a"})
        self.assertEqual((got["verdict"], got["undeclared_with_findings"]), ("findings", ["z"]))
        got = ra.stage0(self.report(a=bad), {"a"})
        self.assertEqual((got["verdict"], got["declared_with_findings"], got["declared_clean"]), ("findings", ["a"], 0))

    def test_a_blind_control_or_an_unaudited_declared_executable_is_incomplete(self) -> None:
        self.assertEqual(ra.stage0(self.report(ok=False, a={"status": "audited"}), {"a"})["verdict"], "incomplete")
        got = ra.stage0(self.report(a={"status": "error"}), {"a"})
        self.assertEqual((got["verdict"], got["declared_not_audited"]), ("incomplete", ["a"]))


@unittest.skipUnless(shutil.which("strace") and shutil.which("ctest"), "strace runs on Linux only; "
                     "the nightly audit runs this control itself and fails without it")
class LiveControlTests(unittest.TestCase):
    def test_the_control_flags_its_undeclared_read(self) -> None:
        files = ra.tracked_files(ROOT)
        with tempfile.TemporaryDirectory() as tmp:
            got = ra.run_control(ROOT, Path(tmp), files, ra.tracked_dirs(files))
        self.assertTrue(got["ok"], got)
        self.assertEqual(got["findings"], [ra.CONTROL_UNDECLARED])


class WorkflowTests(unittest.TestCase):
    def test_the_nightly_runs_the_audit_on_linux_with_strace(self) -> None:
        text = (ROOT / ".github/workflows/read-audit-nightly.yml").read_text()
        self.assertIn("schedule:", text)
        self.assertIn("ubuntu-", text)
        self.assertIn("strace", text)
        self.assertIn("tools/ci/read_audit.py run", text)
        self.assertIn("tools/ci/governed-build.sh", text)


if __name__ == "__main__":
    unittest.main()
