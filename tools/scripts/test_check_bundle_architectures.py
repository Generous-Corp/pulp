#!/usr/bin/env python3
"""Tests for tools/scripts/check_bundle_architectures.py.

Two layers:
  * Pure decision core (`check_binary` / `evaluate`) exercised with injected
    lipo/codesign front-ends — runs on ANY host, no toolchain needed.
  * An end-to-end fixture pass that builds REAL thin + fat Mach-O dylibs with
    clang/lipo/codesign and drives the checker over a synthetic bundle. Skips
    cleanly when the Apple toolchain is absent (non-macOS CI).

Run:
    python3 tools/scripts/test_check_bundle_architectures.py
"""
from __future__ import annotations

import importlib.util
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/check_bundle_architectures.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_bundle_architectures", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ParseArchs(unittest.TestCase):
    def setUp(self):
        self.mod = _load()

    def test_separators(self):
        self.assertEqual(self.mod.parse_archs("arm64,x86_64"), {"arm64", "x86_64"})
        self.assertEqual(self.mod.parse_archs("arm64;x86_64"), {"arm64", "x86_64"})
        self.assertEqual(self.mod.parse_archs("arm64 x86_64"), {"arm64", "x86_64"})
        self.assertEqual(self.mod.parse_archs("arm64"), {"arm64"})
        self.assertEqual(self.mod.parse_archs(" arm64 , x86_64 "), {"arm64", "x86_64"})


class DecisionCore(unittest.TestCase):
    """check_binary / evaluate with injected front-ends — the arch+signature
    logic, no toolchain required."""

    def setUp(self):
        self.mod = _load()

    def test_exact_match_signed_is_clean(self):
        f = self.mod.check_binary(
            "bin", {"arm64", "x86_64"},
            lipo_fn=lambda p: ["x86_64", "arm64"],
            sign_fn=lambda p: True)
        self.assertEqual(f, [])

    def test_missing_arch_flagged(self):
        # The wgpu thin-slice bug: universal expected, dylib is arm64-only.
        f = self.mod.check_binary(
            "libwgpu_native.dylib", {"arm64", "x86_64"},
            lipo_fn=lambda p: ["arm64"],
            sign_fn=lambda p: True)
        self.assertEqual(len(f), 1)
        self.assertIn("missing ['x86_64']", f[0])
        self.assertIn("architecture mismatch", f[0])

    def test_unexpected_arch_flagged(self):
        f = self.mod.check_binary(
            "bin", {"arm64"},
            lipo_fn=lambda p: ["arm64", "x86_64"],
            sign_fn=lambda p: True)
        self.assertEqual(len(f), 1)
        self.assertIn("unexpected ['x86_64']", f[0])

    def test_unsigned_flagged(self):
        # Raw lipo output: right archs, but signature does not verify.
        f = self.mod.check_binary(
            "libwgpu_native.dylib", {"arm64", "x86_64"},
            lipo_fn=lambda p: ["x86_64", "arm64"],
            sign_fn=lambda p: False)
        self.assertEqual(len(f), 1)
        self.assertIn("code signature does not verify", f[0])

    def test_both_findings(self):
        f = self.mod.check_binary(
            "bin", {"arm64", "x86_64"},
            lipo_fn=lambda p: ["arm64"],
            sign_fn=lambda p: False)
        self.assertEqual(len(f), 2)

    def test_signature_check_skippable(self):
        f = self.mod.check_binary(
            "bin", {"arm64", "x86_64"},
            lipo_fn=lambda p: ["x86_64", "arm64"],
            sign_fn=lambda p: False,
            verify_signature=False)
        self.assertEqual(f, [])

    def test_non_macho_skipped(self):
        f = self.mod.check_binary(
            "Info.plist", {"arm64"},
            lipo_fn=lambda p: None,       # lipo says "not a Mach-O"
            sign_fn=lambda p: False)
        self.assertEqual(f, [])

    def test_evaluate_counts_and_filters(self):
        bins = ["Contents/MacOS/App", "Contents/MacOS/libwgpu_native.dylib",
                "Contents/Info.plist"]
        archmap = {
            "Contents/MacOS/App": ["arm64", "x86_64"],
            "Contents/MacOS/libwgpu_native.dylib": ["arm64"],  # thin — bad
            "Contents/Info.plist": None,                        # not Mach-O
        }
        results, inspected = self.mod.evaluate(
            bins, {"arm64", "x86_64"},
            lipo_fn=lambda p: archmap[p],
            sign_fn=lambda p: True,
            relpath_fn=lambda p: p)
        self.assertEqual(inspected, 2)  # plist skipped
        self.assertIn("Contents/MacOS/libwgpu_native.dylib", results)
        self.assertNotIn("Contents/MacOS/App", results)


def _have_apple_toolchain():
    return (os.uname().sysname == "Darwin"
            and shutil.which("clang") and shutil.which("lipo")
            and shutil.which("codesign"))


@unittest.skipUnless(_have_apple_toolchain(), "Apple toolchain (clang/lipo/codesign) required")
class EndToEndFixture(unittest.TestCase):
    """Build REAL Mach-O binaries and drive the checker end-to-end over a
    synthetic, properly code-signed .app bundle."""

    def setUp(self):
        self.mod = _load()
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = os.path.join(self.tmp, "s.c")
        with open(self.src, "w") as fh:
            fh.write("int pulp_probe(void){return 42;}\n")

    def _macho(self, out, archs, sign):
        """Build a thin or fat (lipo'd) Mach-O at `out`, optionally ad-hoc
        signed. A raw lipo'd fat file is deliberately left UNSIGNED when
        sign=False — that is the wgpu 'code object is not signed at all' case."""
        thin = []
        for a in archs:
            o = os.path.join(self.tmp, f"{os.path.basename(out)}.{a}")
            subprocess.run(["clang", "-arch", a, "-dynamiclib", "-o", o, self.src],
                           check=True, capture_output=True)
            thin.append(o)
        if len(thin) == 1:
            shutil.copy(thin[0], out)
        else:
            subprocess.run(["lipo", "-create", *thin, "-output", out],
                           check=True, capture_output=True)
        if sign:
            subprocess.run(["codesign", "-f", "-s", "-", out],
                           check=True, capture_output=True)
        return out

    def _signed_bundle(self, exe_archs, dylib_archs):
        """Assemble a bundle and DEEP-sign it (signs the main executable and
        every nested Mach-O), the realistic POST_BUILD input."""
        app = os.path.join(self.tmp, "PulpProbe.app")
        macos = os.path.join(app, "Contents", "MacOS")
        os.makedirs(macos, exist_ok=True)
        with open(os.path.join(app, "Contents", "Info.plist"), "w") as fh:
            fh.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                     '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                     '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                     '<plist version="1.0"><dict>'
                     '<key>CFBundleExecutable</key><string>PulpProbe</string>'
                     '<key>CFBundleIdentifier</key><string>com.pulp.probe</string>'
                     '</dict></plist>\n')
        self._macho(os.path.join(macos, "PulpProbe"), exe_archs, sign=False)
        self._macho(os.path.join(macos, "libwgpu_native.dylib"), dylib_archs, sign=False)
        # Deep-sign the whole bundle (signs the exe + nested dylib).
        subprocess.run(["codesign", "-f", "-s", "-", "--deep", app],
                       check=True, capture_output=True)
        return app

    def _run(self, target, archs, strict=True, extra=()):
        argv = [target, "--archs", archs, "--label", "PulpProbe", *extra]
        if strict:
            argv.append("--strict")
        return self.mod.main(argv)

    def test_universal_bundle_passes(self):
        app = self._signed_bundle(["arm64", "x86_64"], ["arm64", "x86_64"])
        self.assertEqual(self._run(app, "arm64,x86_64"), 0)

    def test_thin_embedded_dylib_fails(self):
        # Universal exe, but the embedded dylib is arm64-only (the real bug).
        app = self._signed_bundle(["arm64", "x86_64"], ["arm64"])
        self.assertEqual(self._run(app, "arm64,x86_64"), 1)

    def test_raw_lipo_unsigned_fat_dylib_fails(self):
        # The wgpu case: a fat dylib straight out of `lipo -create` — correct
        # archs, but NO valid signature (arm64 slice would be killed at load).
        dylib = self._macho(os.path.join(self.tmp, "libwgpu_native.dylib"),
                            ["arm64", "x86_64"], sign=False)
        self.assertEqual(self._run(dylib, "arm64,x86_64"), 1)
        # Passes once signature verification is turned off (arch-only check).
        self.assertEqual(
            self._run(dylib, "arm64,x86_64", extra=["--no-verify-signature"]), 0)
        # And passes outright once ad-hoc re-signed (the required fix).
        subprocess.run(["codesign", "-f", "-s", "-", dylib],
                       check=True, capture_output=True)
        self.assertEqual(self._run(dylib, "arm64,x86_64"), 0)

    def test_thin_arm64_bundle_passes_for_arm64_target(self):
        app = self._signed_bundle(["arm64"], ["arm64"])
        self.assertEqual(self._run(app, "arm64"), 0)

    def test_missing_target(self):
        self.assertEqual(self.mod.main(["/no/such.app", "--archs", "arm64"]), 2)


if __name__ == "__main__":
    unittest.main()
