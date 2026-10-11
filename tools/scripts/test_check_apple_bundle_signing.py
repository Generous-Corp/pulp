#!/usr/bin/env python3
"""Tests for tools/scripts/check_apple_bundle_signing.sh over real bundles.

The bundles are built here with clang: a shallow iOS-simulator app extension
and a deep macOS plug-in bundle. The checker signs copies ad hoc, inside out
and without --deep, the way Xcode signs a product, so it must reject the layouts
codesign rejects and accept the ones it seals.

What must hold:
- a shallow bundle with its resources at the root signs and verifies;
- a shallow bundle with a top-level Resources/ directory is rejected (the
  layout pulp-build-info.json used to produce, which made iOS AUv3 extensions
  unsignable);
- a deep macOS bundle with data in Contents/Resources signs and verifies, and
  one with data in Contents/MacOS is rejected (Apple reserves it for code);
- the original bundle is never modified, and a missing argument is a usage
  error.

Run:
    python3 tools/scripts/test_check_apple_bundle_signing.py
"""
from __future__ import annotations

import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "scripts" / "check_apple_bundle_signing.sh"
SKIP = 77


def sdk_available(sdk: str) -> bool:
    return subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"],
                          capture_output=True).returncode == 0


def build_executable(out: Path, *, ios: bool) -> None:
    src = out.with_suffix(".c")
    src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    sdk = "iphonesimulator" if ios else "macosx"
    target = ["-target", "arm64-apple-ios16.4-simulator"] if ios else ["-arch", "arm64"]
    sysroot = subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"],
                             capture_output=True, text=True, encoding="utf-8", check=True).stdout.strip()
    subprocess.run(["xcrun", "--sdk", sdk, "clang", *target, "-isysroot", sysroot,
                    str(src), "-o", str(out)], check=True, capture_output=True)
    src.unlink()


def info_plist(name: str, package_type: str) -> bytes:
    return plistlib.dumps({"CFBundleExecutable": name, "CFBundleIdentifier": f"dev.pulp.test.{name}",
                           "CFBundlePackageType": package_type, "CFBundleName": name})


def shallow_bundle(root: Path, *, resources_dir: bool) -> Path:
    bundle = root / "Probe.appex"
    bundle.mkdir(parents=True)
    (bundle / "Info.plist").write_bytes(info_plist("Probe", "XPC!"))
    build_executable(bundle / "Probe", ios=True)
    (bundle / "Probe.inspector-capabilities.json").write_text("{}\n", encoding="utf-8")
    if resources_dir:
        (bundle / "Resources").mkdir()
        (bundle / "Resources" / "pulp-build-info.json").write_text("{}\n", encoding="utf-8")
    else:
        (bundle / "pulp-build-info.json").write_text("{}\n", encoding="utf-8")
    return bundle


def deep_bundle(root: Path, *, data_in_macos: bool) -> Path:
    bundle = root / "Probe.clap"
    macos = bundle / "Contents" / "MacOS"
    macos.mkdir(parents=True)
    (bundle / "Contents" / "Resources").mkdir()
    (bundle / "Contents" / "Info.plist").write_bytes(info_plist("Probe", "BNDL"))
    build_executable(macos / "Probe", ios=False)
    data_dir = macos if data_in_macos else bundle / "Contents" / "Resources"
    (data_dir / "Probe.CLAP.control-shipping.json").write_text("{}\n", encoding="utf-8")
    return bundle


def tree_digest(path: Path) -> list[tuple[str, bytes]]:
    return sorted((p.relative_to(path).as_posix(), p.read_bytes()) for p in path.rglob("*") if p.is_file())


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("codesign") and shutil.which("xcrun"),
                     "needs macOS with codesign and Xcode")
class CheckAppleBundleSigningTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def check(self, *bundles: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(CHECKER), *map(str, bundles)],
                              capture_output=True, text=True, encoding="utf-8", timeout=120)

    def test_shallow_bundle_with_root_resources_signs(self) -> None:
        if not sdk_available("iphonesimulator"):
            self.skipTest("iphonesimulator SDK not installed")
        bundle = shallow_bundle(self.root, resources_dir=False)
        before = tree_digest(bundle)
        proc = self.check(bundle)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("OK: Probe.appex", proc.stdout)
        self.assertEqual(tree_digest(bundle), before, "the checker modified the original bundle")
        self.assertFalse((bundle / "_CodeSignature").exists())

    def test_resource_only_nested_bundle_is_not_signed_as_code(self) -> None:
        if not sdk_available("iphonesimulator"):
            self.skipTest("iphonesimulator SDK not installed")
        bundle = shallow_bundle(self.root, resources_dir=False)
        settings = bundle / "Settings.bundle"
        settings.mkdir()
        (settings / "Root.plist").write_bytes(plistlib.dumps({"PreferenceSpecifiers": []}))
        proc = self.check(bundle)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_shallow_bundle_with_resources_directory_is_rejected(self) -> None:
        if not sdk_available("iphonesimulator"):
            self.skipTest("iphonesimulator SDK not installed")
        proc = self.check(shallow_bundle(self.root, resources_dir=True))
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("cannot be code-signed", proc.stderr)

    def test_deep_bundle_data_placement(self) -> None:
        ok = self.check(deep_bundle(self.root / "ok", data_in_macos=False))
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        bad = self.check(deep_bundle(self.root / "bad", data_in_macos=True))
        self.assertEqual(bad.returncode, 1, bad.stdout + bad.stderr)
        self.assertIn("not signed at all", bad.stderr)

    def test_usage_errors(self) -> None:
        self.assertEqual(self.check().returncode, 2)
        self.assertEqual(self.check(self.root / "absent.appex").returncode, 2)


if __name__ == "__main__":
    if sys.platform == "darwin" and shutil.which("xcrun") and not sdk_available("iphonesimulator"):
        print("SKIP: the iphonesimulator SDK is not installed; the shallow-bundle cases cannot run")
        sys.exit(SKIP)
    unittest.main()
