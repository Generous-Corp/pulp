#!/usr/bin/env python3
"""Tests for dawn_binding_census.py.

The rule logic is tested on synthetic symbol tables everywhere. On macOS with a
C compiler, the census also runs over real Mach-O fixtures built here: images
with zero, one and two Dawn definition sites, a plug-in bundle that exports its
Dawn against one that hides it, and an image that installs the proc table
outside the bootstrap. Each failing case is the control that proves the
census can see what it is meant to reject.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dawn_binding_census as census  # noqa: E402


def image(path: str, external=(), local=(), undefined=()) -> census.ImageReport:
    return census.classify(path, census.Symbols(list(external), list(local), list(undefined)))


class Rules(unittest.TestCase):
    def test_dawn_c_api_and_native_namespace_count_as_dawn(self) -> None:
        self.assertTrue(image("a", external=["_wgpuCreateInstance"]).defines_dawn)
        self.assertTrue(image("a", local=["__ZN4dawn6native8GetProcsEv"]).defines_dawn)
        self.assertTrue(image("a", external=["_dawnProcSetProcs"]).defines_dawn)
        self.assertFalse(image("a", external=["_wgpuish", "_main", "_dawning"]).defines_dawn)

    def test_hidden_dawn_is_defined_but_not_exported(self) -> None:
        hidden = image("a", local=["_wgpuCreateInstance"])
        self.assertTrue(hidden.defines_dawn)
        self.assertFalse(hidden.exports_dawn)

    def test_two_dawn_images_fail(self) -> None:
        report = census.judge("app", "app", False, [
            image("app"), image("libA", external=["_wgpuCreateInstance"]),
            image("libB", external=["_wgpuCreateInstance"])])
        self.assertEqual(report.verdict, "FAIL")
        self.assertEqual(report.dawn_images, ["libA", "libB"])

    def test_one_or_zero_dawn_images_pass(self) -> None:
        one = census.judge("app", "app", False, [
            image("app"), image("libA", external=["_wgpuCreateInstance"])])
        zero = census.judge("app", "app", False, [image("app")])
        self.assertEqual((one.verdict, zero.verdict), ("PASS", "PASS"))

    def test_a_plugin_that_exports_its_dawn_fails_and_a_hidden_one_passes(self) -> None:
        exported = census.judge("x.clap", "x", True, [image("x", external=["_wgpuCreateInstance"])])
        hidden = census.judge("x.clap", "x", True, [image("x", local=["_wgpuCreateInstance"])])
        self.assertEqual(exported.verdict, "FAIL")
        self.assertEqual(hidden.verdict, "PASS")
        # An executable is not a plug-in bundle: exporting is not rule 2.
        app = census.judge("app", "x", False, [image("x", external=["_wgpuCreateInstance"])])
        self.assertEqual(app.verdict, "PASS")

    def test_only_the_bootstrap_may_install_the_proc_table(self) -> None:
        provider = image("libdawn", external=["_wgpuCreateInstance", "_dawnProcSetProcs"])
        outsider = census.judge("app", "app", False, [
            image("app", undefined=["_dawnProcSetProcs"]), provider])
        bootstrap = census.judge("app", "app", False, [
            image("app", external=["__ZN6vellum8app_host30register_native_dawn_bootstrapEv"],
                  undefined=["_dawnProcSetProcs"]), provider])
        self.assertEqual(outsider.verdict, "FAIL")
        self.assertIn("outside the bootstrap", outsider.reasons[0])
        self.assertEqual(bootstrap.verdict, "PASS")

    def test_an_unreadable_artifact_is_an_error_not_a_pass(self) -> None:
        with self.assertRaises(census.CensusError):
            census.census(Path("/nonexistent/thing.clap"))


def _compiler() -> str | None:
    return shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(sys.platform == "darwin" and _compiler(), "needs macOS and a C compiler")
class MachOFixtures(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.dir = Path(tempfile.mkdtemp(prefix="dawn-census-"))
        cc = _compiler()

        def build(name: str, source: str, *flags: str) -> Path:
            src = cls.dir / f"{name}.c"
            src.write_text(source, encoding="utf-8")
            out = cls.dir / name
            subprocess.run([cc, "-o", str(out), str(src), *flags], check=True,
                           capture_output=True)
            return out

        dawn = "void wgpuCreateInstance(void) {}\nvoid dawnProcSetProcs(void) {}\n"
        for lib in ("A", "B"):
            build(f"libdawn{lib}.dylib", dawn, "-dynamiclib",
                  f"-Wl,-install_name,@rpath/libdawn{lib}.dylib")
        link = ["-L", str(cls.dir), "-Wl,-rpath,@loader_path"]
        calls = "void wgpuCreateInstance(void);\nint main(void) { wgpuCreateInstance(); return 0; }\n"
        cls.zero = build("zero", "int main(void) { return 0; }\n")
        cls.one = build("one", calls, *link, "-ldawnA")
        cls.two = build("two", calls, *link, "-ldawnA", "-ldawnB")
        installer = ("void dawnProcSetProcs(void);\n"
                     "int main(void) { dawnProcSetProcs(); return 0; }\n")
        cls.outsider = build("outsider", installer, *link, "-ldawnA")
        cls.bootstrap = build("bootstrap", installer.replace(
            "int main", "void register_native_dawn_bootstrap(void) { dawnProcSetProcs(); }\nint main"),
            *link, "-ldawnA")

        def plugin(name: str, visibility: str) -> Path:
            bundle = cls.dir / f"{name}.clap"
            (bundle / "Contents" / "MacOS").mkdir(parents=True)
            binary = build(f"{name}-bin", f'__attribute__((visibility("{visibility}"))) '
                                          "void wgpuCreateInstance(void) {}\n"
                                          "void entry(void) { wgpuCreateInstance(); }\n",
                           "-bundle")
            binary.rename(bundle / "Contents" / "MacOS" / name)
            return bundle

        cls.exporting_plugin = plugin("Exporting", "default")
        cls.hidden_plugin = plugin("Hidden", "hidden")

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.dir, ignore_errors=True)

    def test_zero_one_and_two_dawn_copies(self) -> None:
        zero = census.census(self.zero)
        one = census.census(self.one)
        two = census.census(self.two)
        self.assertEqual((len(zero.dawn_images), zero.verdict), (0, "PASS"))
        self.assertEqual((len(one.dawn_images), one.verdict), (1, "PASS"))
        # The control: two definition sites in one process must be rejected.
        self.assertEqual((len(two.dawn_images), two.verdict), (2, "FAIL"))
        self.assertTrue(any("libdawnB.dylib" in p for p in two.dawn_images))

    def test_a_plugin_bundle_must_hide_its_dawn(self) -> None:
        exporting = census.census(self.exporting_plugin)
        hidden = census.census(self.hidden_plugin)
        self.assertEqual(exporting.verdict, "FAIL")
        self.assertEqual(hidden.verdict, "PASS")
        self.assertEqual(len(hidden.dawn_images), 1)

    def test_installing_the_proc_table_outside_the_bootstrap_fails(self) -> None:
        self.assertEqual(census.census(self.outsider).verdict, "FAIL")
        self.assertEqual(census.census(self.bootstrap).verdict, "PASS")

    def test_the_command_line_exit_status(self) -> None:
        script = Path(census.__file__)
        run = lambda *a: subprocess.run([sys.executable, str(script), *map(str, a)],
                                        capture_output=True, text=True,
                                        encoding="utf-8").returncode
        self.assertEqual(run(self.zero, self.one), 0)
        self.assertEqual(run(self.one, self.two), 1)
        self.assertEqual(run(self.dir / "missing"), 2)


if __name__ == "__main__":
    unittest.main()
