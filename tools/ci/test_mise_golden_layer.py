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
  --version) echo "${{FAKE_MISE_VERSION:-{version}}} macos-arm64 (2026-10-13)";;
  install) echo "install $* locked=${{MISE_LOCKED:-}} auto_install=${{MISE_AUTO_INSTALL:-}}" >> "$HOME/mise-calls"; [ -n "${{FAKE_MISE_FAIL:-}}" ] && exit 1; exit 0;;
  exec) shift; [ "$1" = "--" ] && shift; exec "$@";;
  ls) echo "ls locked=${{MISE_LOCKED:-}} auto_install=${{MISE_AUTO_INSTALL:-}}" >> "$HOME/mise-calls"; echo '{{"cmake": [{{"version": "4.3.3"}}]}}';;
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


# Stands in for tools/toolchain/check_env.py in the guest: records how it was
# asked, then reports a match or a difference as the real one does.
FAKE_CHECK = """import json, os, sys
open(os.path.join(os.environ["HOME"], "check-calls"), "a").write(
    " ".join(sys.argv[1:]) + " rust=" + os.environ.get("RUSTUP_TOOLCHAIN", "") + "\\n")
drift = os.environ.get("FAKE_CHECK_DRIFT")
findings = [{"tool": "xcode", "expected": "27.2 (27B52)", "found": drift}] if drift else []
print(json.dumps({"exit": 1 if drift else 0, "findings": findings}))
sys.exit(1 if drift else 0)
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


def provision_snippet(start: str, stop: str) -> str:
    """The lines of tart-provision.sh from the one starting `start` up to,
    not including, the one starting `stop`."""
    lines = PROVISION.read_text(encoding="utf-8").splitlines()
    first = next(i for i, line in enumerate(lines) if line.startswith(start))
    last = next(i for i in range(first + 1, len(lines)) if lines[i].startswith(stop))
    return "\n".join(lines[first:last]) + "\n"


@unittest.skipIf(os.name == "nt", "tart-provision.sh is a POSIX shell script")
class ProvisionPinTests(unittest.TestCase):
    """tart-provision.sh bakes the OS and Xcode tools/toolchain/manifest.toml pins."""

    def setUp(self) -> None:
        with (ROOT / "tools/toolchain/manifest.toml").open("rb") as handle:
            self.pin = tomllib.load(handle)["toolchain"]

    def bash(self, script: str, **env: str) -> subprocess.CompletedProcess:
        return subprocess.run(["bash", "-c", script], env=dict(os.environ, **env), capture_output=True,
                              text=True, encoding="utf-8", check=False)

    def test_the_pins_are_read_from_the_generated_image_manifest(self) -> None:
        reader = provision_snippet("IMAGE_PINS=", "# The guest's Xcode lives")
        out = self.bash(reader + 'echo "$BASE_MACOS_IMAGE|$MACOS_BUILD|$XCODE_VERSION|$XCODE_BUILD"',
                        PULP_IMAGE_PINS=str(ROOT / ".shipyard/vm-image.toml"))
        want = "|".join(self.pin[k] for k in ("macos_base", "macos_build", "xcode", "xcode_build"))
        self.assertEqual(want, out.stdout.strip(), out.stderr)
        # Control: a missing manifest yields no pins, which require_pins refuses.
        out = self.bash(reader + 'echo "$BASE_MACOS_IMAGE|$XCODE_BUILD"', PULP_IMAGE_PINS="/nonexistent")
        self.assertEqual("|", out.stdout.strip())
        text = PROVISION.read_text(encoding="utf-8")
        self.assertNotIn("macos-tahoe-base:latest", text)
        self.assertNotIn(':-26.5}', text)

    def guest(self, answers: str, check: str) -> subprocess.CompletedProcess:
        helpers = provision_snippet("app_build(){", "# ── Tier 0")
        stubs = ('note(){ echo "note: $*"; }\nwarn(){ echo "warn: $*" >&2; }\ndie(){ echo "die: $*" >&2; exit 1; }\n'
                 f"vm_ssh(){{ printf '%b' '{answers}'; }}\n"
                 'MACOS_BUILD=26B123; XCODE_VERSION=27.2; XCODE_BUILD=27B52; GUEST_XCODE_APP=/Applications/Xcode.app\n')
        return self.bash(stubs + helpers + check + " 10.0.0.9")

    def test_a_guest_on_another_macos_build_is_refused(self) -> None:
        self.assertEqual(0, self.guest("26B123\\n", "require_guest_macos").returncode)  # control
        out = self.guest("25G83\\n", "require_guest_macos")
        self.assertEqual(1, out.returncode)
        self.assertIn("guest macOS build is '25G83', the manifest pins 26B123", out.stderr)

    def test_a_guest_xcode_must_be_the_pinned_build_at_the_host_path_and_licensed(self) -> None:
        good = "/Applications/Xcode.app/Contents/Developer\\nXcode 27.2\\nBuild version 27B52\\nLICENSE-OK\\n"
        self.assertEqual(0, self.guest(good, "require_guest_xcode").returncode)  # control
        for name, answers in {
            "older Xcode": good.replace("27.2", "26.5").replace("27B52", "17F42"),
            "other build": good.replace("27B52", "27B40a"),
            "other path": good.replace("/Applications/Xcode.app", "/Applications/Xcode-27.2.0.app"),
            "licence not accepted": good.replace("LICENSE-OK\\n", ""),
        }.items():
            with self.subTest(case=name):
                out = self.guest(answers, "require_guest_xcode")
                self.assertEqual(1, out.returncode, out.stdout)
                self.assertIn("guest Xcode is not the pin", out.stderr)


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
        self.check = self.tmp / "fake_check_env.py"
        self.check.write_text(FAKE_CHECK, encoding="utf-8")

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
                   MISE_LAYER_GUEST_HOME=str(self.guest), MISE_LAYER_CHECK_ENV=str(self.check), **extra)
        # The layer must set these itself; inheriting them would hide a removal.
        for key in ("MISE_LOCKED", "MISE_AUTO_INSTALL"):
            if key not in extra:
                env.pop(key, None)
        return subprocess.run(["bash", str(LAYER), str(manifest), "10.0.0.1", str(self.receipt)],
                              env=env, capture_output=True, text=True, encoding="utf-8", check=False)

    def test_every_guest_mise_call_is_locked_with_auto_install_off(self) -> None:
        out = self.run_layer(self.manifest())
        self.assertEqual(0, out.returncode, out.stderr)
        calls = (self.guest / "mise-calls").read_text(encoding="utf-8").splitlines()
        self.assertEqual(["install", "ls"], [line.split()[0] for line in calls])  # control: both ran
        for line in calls:
            self.assertTrue(line.endswith("locked=1 auto_install=0"), line)

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

    def test_the_guest_is_checked_against_the_manifest_and_the_check_is_in_the_receipt(self) -> None:
        out = self.run_layer(self.manifest())
        self.assertEqual(0, out.returncode, out.stderr)
        call = (self.guest / "check-calls").read_text(encoding="utf-8")
        for arg in ("--require-tools", "--fleet-path", "--expected expected.json", "--format json", "rust=1.97.1"):
            self.assertIn(arg, call)
        self.assertEqual((ROOT / "tools/toolchain/expected.json").read_bytes(),
                         (self.guest / ".pulp-toolchain/expected.json").read_bytes())
        receipt = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual({"exit": 0, "findings": []}, receipt["toolchain_check"])

    def test_a_guest_that_differs_from_the_manifest_is_refused(self) -> None:
        out = self.run_layer(self.manifest(), FAKE_CHECK_DRIFT="26.5 (17F42)")
        self.assertEqual(1, out.returncode)
        self.assertIn("the guest differs from tools/toolchain/manifest.toml", out.stderr)
        self.assertIn("26.5 (17F42)", out.stderr)
        self.assertFalse(self.receipt.exists() and self.receipt.stat().st_size)

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

    def test_a_guest_mise_reporting_another_version_is_refused(self) -> None:
        # Same bytes, but the guest binary reports a different release.
        out = self.run_layer(self.manifest(), FAKE_MISE_VERSION="2026.5.16")
        self.assertEqual(1, out.returncode)
        self.assertIn(f"guest mise reports 2026.5.16, the manifest pins {self.version}", out.stderr)
        self.assertFalse((self.guest / "mise-calls").exists())

    def test_toolchain_files_that_differ_from_the_commit_are_refused(self) -> None:
        # A copy of the layer in its own repository, so the checkout can be dirtied.
        repo = self.tmp / "repo"
        (repo / "tools/ci").mkdir(parents=True)
        shutil.copy2(LAYER, repo / "tools/ci/mise-golden-layer.sh")
        (repo / "tools/toolchain").mkdir(parents=True)
        for name in (".mise.toml", ".mise.toolchain.toml", "mise.toolchain.lock", "tools/toolchain/expected.json"):
            shutil.copy2(ROOT / name, repo / name)
        git = ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid"]
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(git + ["add", "-A"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", "toolchain"], check=True)
        layer = repo / "tools/ci/mise-golden-layer.sh"
        env = dict(os.environ, HOME=str(self.host), MISE_LAYER_GUEST="local", MISE_LAYER_GUEST_HOME=str(self.guest),
                   MISE_LAYER_CHECK_ENV=str(self.check))
        clean = subprocess.run(["bash", str(layer), str(self.manifest()), "10.0.0.1", str(self.receipt)],
                               env=env, capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(0, clean.returncode, clean.stderr)  # control: the clean copy bakes
        with (repo / ".mise.toml").open("a", encoding="utf-8") as handle:
            handle.write("# an uncommitted edit\n")
        dirty = subprocess.run(["bash", str(layer), str(self.manifest()), "10.0.0.1", str(self.receipt)],
                               env=env, capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(1, dirty.returncode)
        self.assertIn("the toolchain files differ from", dirty.stderr)
        # The expectation the guest is checked against is held to the same rule.
        subprocess.run(["git", "-C", str(repo), "checkout", "-q", "--", ".mise.toml"], check=True)
        with (repo / "tools/toolchain/expected.json").open("a", encoding="utf-8") as handle:
            handle.write("\n")
        dirty = subprocess.run(["bash", str(layer), str(self.manifest()), "10.0.0.1", str(self.receipt)],
                               env=env, capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(1, dirty.returncode)
        self.assertIn("the toolchain files differ from", dirty.stderr)

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
