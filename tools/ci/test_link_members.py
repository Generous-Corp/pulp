#!/usr/bin/env python3
"""Tests for tools/ci/link_members.py and tools/ci/link-members-launcher.sh.

What must hold:
- a real linker map (testdata/link_members/pulp-test-state.map, cut down from
  an ld-prime map of pulp-test-state) parses to exactly its direct objects and
  the archive members it pulled, leaving out SDK stubs, the synthesized input
  and everything after the object list;
- an archive is `whole` when the link line force-loads it (as `-force_load
  X`, `-Wl,-force_load,X` or inside a response file) and every archive is
  whole under `-all_load` or `-ObjC`; no other archive is;
- `collect` stores member names once per archive and `expand` gives back the
  same members per executable; an unreadable record is counted;
- the launcher adds `-Wl,-map`, passes the linker's exit status through,
  keeps only the map's head and the arguments after a successful executable
  link, deletes the map, and leaves shared libraries, failed links and an
  unwritable record directory exactly as the plain command would.

Run:
    python3 tools/ci/test_link_members.py
"""
from __future__ import annotations

import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import link_members as lm  # noqa: E402

FIXTURE = ROOT / "tools/ci/testdata/link_members/pulp-test-state.map"
LAUNCHER = ROOT / "tools/ci/link-members-launcher.sh"


def build_tree(tmp: str) -> Path:
    """A build root holding the directories of every input the fixture's link
    (run from <build>/test) names, so relative inputs resolve inside it."""
    root = Path(tmp) / "build"
    (root / "test").mkdir(parents=True)
    _, _, inputs = lm.parse_map(FIXTURE.read_text().splitlines(True))
    for item in inputs:
        if not item.startswith("/") and item != "linker synthesized":
            (root / "test" / item.split("(")[0]).parent.mkdir(parents=True, exist_ok=True)
    return root


class ParseTests(unittest.TestCase):
    def parse(self, args: list[str]) -> tuple[str, dict]:
        with tempfile.TemporaryDirectory() as tmp:
            root = build_tree(tmp)
            lines = [f"# Cwd: {root / 'test'}\n"] + FIXTURE.read_text().splitlines(True)
            lines = [l.replace("/work/build", str(root)) for l in lines]
            return lm.parse_link(lines, args, root)

    def test_the_map_parses_to_its_objects_and_pulled_members(self) -> None:
        exe, rec = self.parse([])
        self.assertEqual(exe, "<build>/test/pulp-test-state")
        self.assertEqual(rec["objects"], ["<build>/test/CMakeFiles/pulp-test-state.dir/harness/rt_allocation_probe.cpp.o",
                                          "<build>/test/CMakeFiles/pulp-test-state.dir/test_state.cpp.o"])
        members = {a: v["members"] for a, v in rec["archives"].items()}
        self.assertEqual(members, {
            "<build>/_deps/catch2-build/src/libCatch2Maind.a": ["catch_main.cpp.o"],
            "<build>/core/events/libpulp-events.a": ["event_loop.cpp.o", "main_thread_dispatcher.cpp.o"],
            "<build>/core/runtime/libpulp-runtime.a": ["scoped_no_alloc.cpp.o"],
            "<build>/core/state/libpulp-state.a": ["state_migration.cpp.o", "store.cpp.o"],
            "<build>/fonts/libpulp-fonts.a": ["inter_bold.cpp.o", "inter_regular.cpp.o"],
        })
        self.assertFalse(any(v["whole"] for v in rec["archives"].values()))

    def test_force_load_marks_only_that_archive_whole(self) -> None:
        for args in (["-Wl,-force_load,../fonts/libpulp-fonts.a"],
                     ["-Xlinker", "-force_load", "-Xlinker", "../fonts/libpulp-fonts.a"]):
            _, rec = self.parse(["c++", "-o", "pulp-test-state", *args])
            whole = sorted(a for a, v in rec["archives"].items() if v["whole"])
            self.assertEqual(whole, ["<build>/fonts/libpulp-fonts.a"], args)

    def test_all_load_and_objc_mark_every_archive_whole(self) -> None:
        for flag in ("-Wl,-all_load", "-ObjC"):
            _, rec = self.parse(["c++", flag])
            self.assertTrue(all(v["whole"] for v in rec["archives"].values()), flag)

    def test_a_force_load_inside_a_response_file_is_seen(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "link.rsp").write_text("-Wl,-force_load,lib/x.a other.o\n")
            forced, everything = lm.whole_archive_flags(["@link.rsp"], Path(tmp), Path(tmp))
        self.assertEqual((forced, everything), ({"<build>/lib/x.a"}, False))

    def test_parsing_stops_at_the_end_of_the_object_list(self) -> None:
        # A full map's symbol table runs to tens of megabytes; it is never read.
        def lines():
            yield from ["# Path: /x/out\n", "# Object files:\n", "[  1] a.o\n", "# Sections:\n"]
            raise AssertionError("read past the object list")
        self.assertEqual(lm.parse_map(lines()), (None, "/x/out", ["a.o"]))

    def test_a_map_without_an_object_list_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            lm.parse_link(["# Path: /x\n", "# Sections:\n"], [], Path("/"))


class CollectTests(unittest.TestCase):
    def test_collect_compacts_members_and_expand_restores_them(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = build_tree(tmp)
            folder = root / lm.MEMBERS_DIR
            folder.mkdir()
            text = FIXTURE.read_text().replace("/work/build", str(root))
            (folder / "a.objects").write_text(f"# Cwd: {root / 'test'}\n" + text)
            (folder / "a.args").write_text("c++\n-Wl,-force_load,../fonts/libpulp-fonts.a\n")
            other = text.replace("pulp-test-state", "pulp-test-other").replace(
                "[  4] ../core/state/libpulp-state.a(state_migration.cpp.o)\n", "")
            (folder / "b.objects").write_text(f"# Cwd: {root / 'test'}\n" + other)
            (folder / "broken.objects").write_text("# Path: /x\n")
            doc = lm.collect(root)
        self.assertEqual(doc["unreadable"], 1)
        self.assertEqual(doc["members"]["<build>/core/state/libpulp-state.a"],
                         ["state_migration.cpp.o", "store.cpp.o"])
        full = lm.expand(doc)
        self.assertEqual(full["<build>/test/pulp-test-state"]["archives"]["<build>/core/state/libpulp-state.a"],
                         {"members": ["state_migration.cpp.o", "store.cpp.o"], "whole": False})
        self.assertEqual(full["<build>/test/pulp-test-other"]["archives"]["<build>/core/state/libpulp-state.a"],
                         {"members": ["store.cpp.o"], "whole": False})
        self.assertTrue(full["<build>/test/pulp-test-state"]["archives"]["<build>/fonts/libpulp-fonts.a"]["whole"])
        self.assertFalse(full["<build>/test/pulp-test-other"]["archives"]["<build>/fonts/libpulp-fonts.a"]["whole"])


FAKE_LINKER = """#!/bin/sh
# Writes an ld-style map where asked, records its arguments, exits $FAKE_RC.
printf '%s\\n' "$@" > "$FAKE_LOG"
for a in "$@"; do
  case $a in -Wl,-map,*) m=${a#-Wl,-map,}; printf '# Path: %s/out\\n# Object files:\\n[  0] linker synthesized\\n[  1] main.o\\n[  2] lib.a(one.o)\\n# Sections:\\n0x0 BIG SYMBOL TABLE\\n' "$PWD" > "$m";; esac
done
exit "${FAKE_RC:-0}"
"""


class LauncherTests(unittest.TestCase):
    def run_launcher(self, tmp: str, *args: str, rc: int = 0) -> tuple[int, list[str], Path]:
        linker = Path(tmp) / "fake-ld"
        linker.write_text(FAKE_LINKER)
        linker.chmod(linker.stat().st_mode | stat.S_IXUSR)
        log = Path(tmp) / "log"
        env = {**os.environ, "FAKE_LOG": str(log), "FAKE_RC": str(rc)}
        root = Path(tmp) / "build"
        root.mkdir(exist_ok=True)
        proc = subprocess.run(["/bin/sh", str(LAUNCHER), str(root), str(linker), *args],
                              cwd=root, env=env, capture_output=True, text=True, timeout=30)
        return proc.returncode, log.read_text().splitlines() if log.exists() else [], root / lm.MEMBERS_DIR

    def test_an_executable_link_keeps_its_object_list_and_arguments_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rc, argv, folder = self.run_launcher(tmp, "main.o", "-o", "out", "-Wl,-force_load,lib.a")
            files = sorted(p.name for p in folder.iterdir())
            objects = next(folder.glob("*.objects")).read_text()
            args = next(folder.glob("*.args")).read_text().splitlines()
        self.assertEqual(rc, 0)
        self.assertEqual(argv[:4], ["main.o", "-o", "out", "-Wl,-force_load,lib.a"])
        self.assertTrue(argv[4].startswith("-Wl,-map,"))
        self.assertEqual([f.rsplit(".", 1)[1] for f in files], ["args", "objects"], files)
        self.assertIn("[  2] lib.a(one.o)", objects)
        self.assertNotIn("SYMBOL TABLE", objects)
        self.assertTrue(objects.startswith("# Cwd: "))
        self.assertIn("-Wl,-force_load,lib.a", args)

    def test_the_linker_status_passes_through_and_a_failed_link_records_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rc, _, folder = self.run_launcher(tmp, "main.o", "-o", "out", rc=3)
            left = sorted(p.name for p in folder.iterdir())
        self.assertEqual((rc, left), (3, []))

    def test_shared_libraries_run_untouched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rc, argv, folder = self.run_launcher(tmp, "-dynamiclib", "a.o", "-o", "libx.dylib")
        self.assertEqual((rc, argv), (0, ["-dynamiclib", "a.o", "-o", "libx.dylib"]))
        self.assertFalse(folder.exists())

    def test_an_unwritable_record_directory_runs_the_plain_link(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "build").mkdir()
            (Path(tmp) / "build" / lm.MEMBERS_DIR).write_text("a file, not a directory")
            rc, argv, _ = self.run_launcher(tmp, "main.o", "-o", "out")
        self.assertEqual((rc, argv), (0, ["main.o", "-o", "out"]))


class CMakeWiringTests(unittest.TestCase):
    def test_the_option_sets_every_linker_launcher_before_any_target(self) -> None:
        text = (ROOT / "tools/cmake/PulpLinkMaps.cmake").read_text()
        self.assertIn("link-members-launcher.sh", text)
        for lang in ("C", "CXX", "OBJC", "OBJCXX"):
            self.assertIn(lang, text.split("foreach(_pulp_lang", 1)[1].split(")", 1)[0])
        lines = (ROOT / "CMakeLists.txt").read_text().splitlines()
        include = next(i for i, l in enumerate(lines) if "tools/cmake/PulpLinkMaps.cmake" in l)
        first = next(i for i, l in enumerate(lines)
                     if re.match(r"\s*(add_library|add_executable|add_subdirectory|FetchContent_MakeAvailable)\(", l))
        self.assertLess(include, first, lines[first])

    def test_the_gate_turns_it_on_and_records_it(self) -> None:
        text = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertIn("-DPULP_RECORD_LINK_MAPS=ON", text)
        self.assertIn("--link-members", text)


if __name__ == "__main__":
    unittest.main()
