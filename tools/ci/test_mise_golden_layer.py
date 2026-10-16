#!/usr/bin/env python3
"""Tests for the pinned-toolchain golden layer (tools/ci/mise-golden-layer.sh)
and its manifest (.shipyard/vm-image.mise.toml), without a VM: the layer's
guest commands run in a local shell with a separate guest home, against
stand-in mise and rustup binaries.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAYER = ROOT / "tools/ci/mise-golden-layer.sh"
MANIFEST = ROOT / ".shipyard/vm-image.mise.toml"
PROVISION = ROOT / "tools/ci/tart-provision.sh"

FAKE_MISE = """#!/bin/sh
case "$1" in
  --version) echo "{version} macos-arm64 (2026-10-13)";;
  install) echo "install $*" >> "$HOME/mise-calls"; [ -n "${{FAKE_MISE_FAIL:-}}" ] && exit 1; exit 0;;
  ls) echo '{{"cmake": [{{"version": "4.3.3"}}]}}';;
esac
"""
FAKE_RUSTUP = """#!/bin/sh
echo "$*" >> "$HOME/rustup-calls"
case "$1 $2" in
  "toolchain install") [ -n "${FAKE_RUSTUP_SILENT:-}" ] || : > "$HOME/.rust-installed";;
  "toolchain list") [ -e "$HOME/.rust-installed" ] && echo "1.97.1-test-host";;
esac
exit 0
"""


class ManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        with MANIFEST.open("rb") as handle:
            self.m = tomllib.load(handle)

    def test_a_candidate_layered_on_the_runner_golden(self) -> None:
        self.assertEqual("pulp-build-runner:latest", self.m["base"])
        self.assertEqual("pulp-build-runner-mise-candidate", self.m["name"])
        self.assertNotIn("brew", self.m)  # adds only the pinned layer
        self.assertNotIn("xcode", self.m.get("toolchain", {}))

    def test_the_mise_table_is_the_manifest_pin(self) -> None:
        with (ROOT / "tools/toolchain/manifest.toml").open("rb") as handle:
            pin = tomllib.load(handle)
        mise = self.m["mise"]
        self.assertEqual(pin["mise"]["version"], mise["version"])
        self.assertEqual(pin["mise"]["binary"]["sha256"]["macos-arm64"], mise["sha256"])
        self.assertEqual(pin["mise"]["binary"]["path"], mise["path"])
        self.assertEqual(pin["toolchain"]["rust"], mise["rust"])

    def test_tart_provision_runs_the_layer_only_for_mise_manifests(self) -> None:
        text = PROVISION.read_text(encoding="utf-8")
        self.assertIn("grep -q '^\\[mise\\]' \"$path\"", text)
        self.assertIn("mise-golden-layer.sh\" \"$path\" \"$ip\"", text)
        self.assertIn('die "pinned toolchain layer failed', text)
        self.assertNotIn("[mise]", (ROOT / ".shipyard/vm-image.toml").read_text(encoding="utf-8"))


@unittest.skipIf(os.name == "nt" or shutil.which("shasum") is None, "the layer is a POSIX shell script")
class LayerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.addCleanup(self.td.cleanup)
        self.tmp = Path(self.td.name)
        self.host = self.tmp / "host"
        self.guest = self.tmp / "guest"
        self.version = "2026.10.8"
        mise = self.host / f".local/opt/mise-{self.version}/bin/mise"
        mise.parent.mkdir(parents=True)
        mise.write_text(FAKE_MISE.format(version=self.version), encoding="utf-8")
        mise.chmod(0o755)
        self.digest = hashlib.sha256(mise.read_bytes()).hexdigest()
        rustup = self.guest / ".cargo/bin/rustup"
        rustup.parent.mkdir(parents=True)
        rustup.write_text(FAKE_RUSTUP, encoding="utf-8")
        rustup.chmod(0o755)
        self.receipt = self.tmp / "receipt.json"

    def manifest(self, sha256: str | None = None) -> Path:
        path = self.tmp / "vm-image.mise.toml"
        path.write_text(
            f'schema = 1\nname = "x"\nbase = "y"\n[mise]\nversion = "{self.version}"\n'
            f'path = "~/.local/opt/mise-{self.version}/bin/mise"\nsha256 = "{sha256 or self.digest}"\n'
            'rust = "1.97.1"\nrust_components = ["rustfmt", "clippy"]\nrust_targets = ["x86_64-test-target"]\n',
            encoding="utf-8")
        return path

    def run_layer(self, manifest: Path, **extra: str) -> subprocess.CompletedProcess:
        env = dict(os.environ, HOME=str(self.host), MISE_LAYER_GUEST="local",
                   MISE_LAYER_GUEST_HOME=str(self.guest), **extra)
        return subprocess.run(["bash", str(LAYER), str(manifest), "10.0.0.1", str(self.receipt)],
                              env=env, capture_output=True, text=True, encoding="utf-8", check=False)

    def test_installs_the_locked_toolchain_and_rust_by_name_and_reports(self) -> None:
        out = self.run_layer(self.manifest())
        self.assertEqual(0, out.returncode, out.stderr)
        guest_mise = self.guest / f".local/opt/mise-{self.version}/bin/mise"
        self.assertEqual(self.digest, hashlib.sha256(guest_mise.read_bytes()).hexdigest())
        for name in (".mise.toml", ".mise.toolchain.toml", "mise.toolchain.lock"):
            self.assertEqual((ROOT / name).read_bytes(), (self.guest / ".pulp-toolchain" / name).read_bytes())
        self.assertIn("install --locked", (self.guest / "mise-calls").read_text(encoding="utf-8"))
        calls = (self.guest / "rustup-calls").read_text(encoding="utf-8")
        self.assertIn("set auto-self-update disable", calls)
        self.assertIn("toolchain install 1.97.1 --profile minimal --component rustfmt --component clippy "
                      "--target x86_64-test-target", calls)
        # rustup is told never to self-update before it installs anything.
        self.assertLess(calls.index("set auto-self-update"), calls.index("toolchain install"))
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True,
                                text=True, encoding="utf-8", check=True).stdout.strip()
        self.assertEqual(commit, receipt["commit"])
        self.assertEqual(hashlib.sha256((ROOT / "mise.toolchain.lock").read_bytes()).hexdigest(),
                         receipt["lock_sha256"])
        self.assertEqual({"cmake": [{"version": "4.3.3"}]}, receipt["mise_ls"])
        self.assertIn("1.97.1-test-host", receipt["rustup_toolchains"])

    def test_a_host_binary_that_is_not_the_pin_is_refused_before_any_copy(self) -> None:
        out = self.run_layer(self.manifest("0" * 64))
        self.assertEqual(1, out.returncode)
        self.assertIn(f"has sha256 {self.digest}, the manifest pins {'0' * 64}", out.stderr)
        self.assertFalse((self.guest / ".local").exists())

    def test_a_guest_copy_that_differs_is_refused(self) -> None:
        # A shasum that reports a different digest for anything in the guest home.
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        fake = bin_dir / "shasum"
        fake.write_text(
            f'#!/bin/sh\ncase "$*" in *"{self.guest}"*) echo "{"1" * 64}  x";; *) exec /usr/bin/shasum "$@";; esac\n',
            encoding="utf-8")
        fake.chmod(0o755)
        out = self.run_layer(self.manifest(), PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
        self.assertEqual(1, out.returncode)
        self.assertIn(f"guest mise has sha256 {'1' * 64}, the manifest pins {self.digest}", out.stderr)
        self.assertFalse((self.guest / "mise-calls").exists())

    def test_a_failed_locked_install_stops_the_layer(self) -> None:
        out = self.run_layer(self.manifest(), FAKE_MISE_FAIL="1")
        self.assertEqual(1, out.returncode)
        self.assertIn("mise install --locked failed", out.stderr)
        self.assertFalse((self.guest / "rustup-calls").exists())
        self.assertFalse(self.receipt.exists())

    def test_rust_must_be_listed_by_name_after_installing(self) -> None:
        out = self.run_layer(self.manifest(), FAKE_RUSTUP_SILENT="1")
        self.assertEqual(1, out.returncode)
        self.assertIn("lists no toolchain named 1.97.1", out.stderr)


if __name__ == "__main__":
    unittest.main()
