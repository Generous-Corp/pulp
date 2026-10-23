#!/usr/bin/env python3
"""Tests for tools/toolchain/host_sync.py.

Each case runs the script as a host would, against a real git origin in a
temporary directory and shell-script stand-ins for mise, rustup and rustc. No
case reaches the network or the real home directory: HOME is the fixture's.
"""
from __future__ import annotations

import ast
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List

import host_sync

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PIN = "1.97.1"
MISE_VERSION = "2026.10.17"

FAKE_MISE = r"""#!/bin/sh
echo "$*" >> "$HOME/mise.log"
case "$1" in
  --version) echo "2026.10.17 macos-arm64 (2026-10-17)" ;;
  trust)
    # Only ever one named file, never a directory.
    [ "$2" = --quiet ] && [ -f "$3" ] && [ "$(basename "$3")" = .mise.toml ] || { echo "fake mise: bad trust $*" >&2; exit 3; }
    ;;
  install)
    if [ -n "$FAKE_MISE_FAIL" ]; then
      echo "mise ERROR failed to download https://github.com/Kitware/CMake/releases/download/v4.3.3/cmake.tar.gz: Could not resolve host: github.com" >&2
      exit 1
    fi
    # The pins must be locked, install-only, and read from the copy it trusts.
    [ "$MISE_ENV" = toolchain ] && [ "$MISE_LOCKED" = 1 ] && [ "$MISE_AUTO_INSTALL" = 0 ] && [ "$MISE_YES" = 1 ] \
      || { echo "fake mise: not a locked install" >&2; exit 3; }
    [ "$(cd "$MISE_TRUSTED_CONFIG_PATHS" && pwd -P)" = "$(pwd -P)" ] && [ -f mise.toolchain.lock ] && [ -f .mise.toolchain.toml ] \
      || { echo "fake mise: not run in the trusted copy of the pins" >&2; exit 3; }
    touch "$HOME/mise-installed" ;;
  exec)
    shift
    while [ "$1" != "--" ]; do shift; done
    shift
    [ -f "$HOME/mise-installed" ] && PATH="$HOME/mise-tools/bin:$PATH"
    export PATH
    exec "$@" ;;
  *) echo "fake mise: unexpected $*" >&2; exit 2 ;;
esac
"""

FAKE_RUSTUP = r"""#!/bin/sh
echo "$*" >> "$HOME/rustup.log"
S="$HOME/rustup-state"
mkdir -p "$S"
case "$1 $2" in
  "toolchain list")
    # A toolchain is a directory under $S; `stable` holds a `version` file.
    for t in "$S"/*; do [ -d "$t" ] && echo "$(basename "$t")-aarch64-apple-darwin"; done
    exit 0 ;;
  "toolchain install")
    if [ -n "$FAKE_RUSTUP_FAIL" ]; then
      echo "error: could not download file from 'https://static.rust-lang.org/dist/channel-rust-$3.toml.sha256': Could not resolve host: static.rust-lang.org" >&2
      exit 1
    fi
    pin="$3"; shift 3
    mkdir -p "$S/$pin"
    while [ $# -gt 0 ]; do
      case "$1" in
        --component) echo "$2" | tr ',' '\n' >> "$S/$pin/components"; shift ;;
        --target) echo "$2" | tr ',' '\n' >> "$S/$pin/targets"; shift ;;
      esac
      shift
    done
    if [ -n "$FAKE_RUSTUP_DROP" ]; then
      grep -v -x "$FAKE_RUSTUP_DROP" "$S/$pin/components" > "$S/$pin/kept"
      mv "$S/$pin/kept" "$S/$pin/components"
    fi
    exit 0 ;;
  "component list")
    [ -d "$S/$5" ] || { echo "error: toolchain '$5-aarch64-apple-darwin' is not installed" >&2; exit 1; }
    echo "rustc-aarch64-apple-darwin"
    sed 's/$/-aarch64-apple-darwin/' "$S/$5/components" 2>/dev/null
    exit 0 ;;
  "target list")
    [ -d "$S/$5" ] || { echo "error: toolchain '$5-aarch64-apple-darwin' is not installed" >&2; exit 1; }
    echo "aarch64-apple-darwin"
    cat "$S/$5/targets" 2>/dev/null
    exit 0 ;;
esac
if [ "$1" = run ] && [ "$3 $4" = "rustc --version" ]; then
  [ -d "$S/$2" ] || { echo "error: toolchain '$2' is not installed" >&2; exit 1; }
  if [ -f "$S/$2/version" ]; then v=$(cat "$S/$2/version"); else v="$2"; fi
  echo "rustc $v (fake 2026-06-12)"
  exit 0
fi
if [ "$1" = default ] && [ $# -eq 1 ]; then echo "stable-aarch64-apple-darwin (default)"; exit 0; fi
echo "fake rustup: unexpected $*" >&2
exit 2
"""

# The rustup proxy: the toolchain rust-toolchain.toml in the working directory names.
FAKE_RUSTC = r"""#!/bin/sh
pin=$(sed -n 's/^channel = "\(.*\)"$/\1/p' rust-toolchain.toml 2>/dev/null)
[ -n "$pin" ] || pin=stable
if [ -d "$HOME/rustup-state/$pin" ]; then echo "rustc $pin (fake 2026-06-12)"; exit 0; fi
echo "error: toolchain '$pin-aarch64-apple-darwin' is not installed" >&2
exit 1
"""

PINNED_TOOLS = {
    "cmake": "cmake version 4.3.3",
    "ninja": "1.13.2",
    "node": "v26.10.0",
    "clang-format": "clang-format version 21.1.8",
}


def script(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)
    return path


@unittest.skipIf(os.name == "nt", "the fake tools are shell scripts")
class HostSyncTests(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name).resolve()
        self.home = self.root / "home"
        self.base_bin = self.root / "bin"
        self.home.mkdir()
        # The interpreter check_env.py runs on, and a stand-in Xcode so the
        # Xcode note never asks the real one.
        self.base_bin.mkdir()
        (self.base_bin / "python3").symlink_to(sys.executable)
        script(self.base_bin / "xcodebuild", "#!/bin/sh\necho 'Xcode 26.5'\necho 'Build version 17F42'\n")
        git = shutil.which("git")
        assert git, "git is required"
        (self.base_bin / "git").symlink_to(git)
        self.env: Dict[str, str] = {
            "PATH": os.pathsep.join([str(self.base_bin), "/usr/bin", "/bin"]),
            "HOME": str(self.home),
            "GIT_CONFIG_NOSYSTEM": "1",
            "TMPDIR": str(self.root / "tmp"),
        }
        (self.root / "tmp").mkdir()
        self.mise = script(self.home / f".local/opt/mise-{MISE_VERSION}/bin/mise", FAKE_MISE)
        script(self.home / ".cargo/bin/rustup", FAKE_RUSTUP)
        script(self.home / ".cargo/bin/rustc", FAKE_RUSTC)
        tools = self.home / "mise-tools/bin"
        for name, text in PINNED_TOOLS.items():
            script(tools / name, f"#!/bin/sh\necho '{text}'\n")
        (tools / "python3").symlink_to(sys.executable)
        self.origin = self.root / "origin.git"
        self.checkout = self.root / "checkout"
        self.git("init", "--quiet", "--bare", "-b", "main", str(self.origin))
        self.git("clone", "--quiet", str(self.origin), str(self.checkout))
        self.commit(self.pins())
        self.git("-C", str(self.checkout), "push", "--quiet", "origin", "HEAD:main")
        self.git("-C", str(self.checkout), "fetch", "--quiet", "origin")

    def tearDown(self) -> None:
        self.td.cleanup()

    # -- fixture -------------------------------------------------------------

    def git(self, *args: str) -> str:
        out = subprocess.run(
            ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "core.hooksPath=/dev/null", *args],
            env=self.env, capture_output=True, text=True, encoding="utf-8", check=True,
        )
        return out.stdout.strip()

    def expected(self, rust: str = PIN, mise_sha: str = "") -> dict:
        return {
            "schema": 1,
            "source_sha256": "a" * 64,
            "enforcement": "warn",
            "tools": {
                "python": platform.python_version(), "cmake": "4.3.3", "ninja": "1.13.2",
                "node": "26.10.0", "rust": rust, "clang_format": "21.1.8",
            },
            "platform_overrides": {},
            "mise": {
                "version": MISE_VERSION,
                "path": f"~/.local/opt/mise-{MISE_VERSION}/bin/mise",
                "sha256": {host_sync.host_platform(): mise_sha or hashlib.sha256(self.mise.read_bytes()).hexdigest()},
            },
            # A per-checkout tree the pins copy never has: reported, never counted.
            "npm_prefixes": ["web"],
            "rust_components": ["rustfmt", "clippy"],
            "rust_targets": ["x86_64-apple-darwin"],
        }

    def pins(self, rust: str = PIN, **kw: str) -> Dict[str, str]:
        return {
            "tools/toolchain/expected.json": json.dumps(self.expected(rust, **kw), indent=2),
            "tools/toolchain/check_env.py": (HERE / "check_env.py").read_text(encoding="utf-8"),
            ".mise.toolchain.toml": '[tools]\ncmake = "4.3.3"\n',
            "mise.toolchain.lock": "# fake lock\n",
            ".mise.toml": "[settings]\nlocked = true\n",
            "experimental/pulp-rs/rust-toolchain.toml": f'[toolchain]\nchannel = "{rust}"\n',
        }

    def commit(self, files: Dict[str, str], where: Path = None) -> None:
        repo = where or self.checkout
        for rel, text in files.items():
            path = repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        self.git("-C", str(repo), "add", *files)
        self.git("-C", str(repo), "commit", "--quiet", "-m", "pins")

    def advance_origin(self, files: Dict[str, str]) -> None:
        """Land a commit on origin/main that the checkout has not fetched."""
        other = self.root / "other"
        if not other.exists():
            self.git("clone", "--quiet", str(self.origin), str(other))
        self.git("-C", str(other), "fetch", "--quiet", "origin")
        self.git("-C", str(other), "reset", "--quiet", "--hard", "origin/main")
        self.commit(files, other)
        self.git("-C", str(other), "push", "--quiet", "origin", "HEAD:main")

    def sync(self, *args: str, **env: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(HERE / "host_sync.py"), "--repo", str(self.checkout), *args],
            env=dict(self.env, **env), capture_output=True, text=True, encoding="utf-8", check=False,
        )

    def receipt(self) -> dict:
        return json.loads((self.home / ".config/pulp/toolchain-sync.json").read_text(encoding="utf-8"))

    def log(self, name: str) -> List[str]:
        path = self.home / f"{name}.log"
        return path.read_text(encoding="utf-8").splitlines() if path.exists() else []

    def installs(self) -> List[str]:
        return [line for line in self.log("rustup") if line.startswith("toolchain install")]

    def forget_installs(self) -> None:
        (self.home / "mise-installed").unlink(missing_ok=True)
        shutil.rmtree(self.home / "rustup-state", ignore_errors=True)

    def step(self, name: str) -> dict:
        return next(s for s in self.receipt()["steps"] if s["step"] == name)

    # -- cases ---------------------------------------------------------------

    def test_a_drifted_host_converges_and_a_second_run_changes_nothing(self) -> None:
        out = self.sync()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        receipt = self.receipt()
        self.assertEqual([], receipt["findings"])
        self.assertEqual([], receipt["network_failures"])
        self.assertEqual(self.git("-C", str(self.checkout), "rev-parse", "origin/main"), receipt["ref_sha"])
        self.assertEqual("a" * 64, receipt["manifest_sha256"])
        self.assertIn("install --locked", self.log("mise"))
        self.assertIn(f"trust --quiet {self.checkout}/.mise.toml", self.log("mise"))
        self.assertEqual(
            [f"toolchain install {PIN} --profile minimal --no-self-update --component rustfmt,clippy "
             "--target x86_64-apple-darwin"],
            self.installs(),
        )
        # The per-checkout npm tree is reported, never counted against the host.
        self.assertEqual(["npm web"], [f["tool"] for f in receipt["ignored_per_checkout"]])
        again = self.sync()
        self.assertEqual(0, again.returncode, again.stdout + again.stderr)
        self.assertEqual(1, len(self.installs()), "an installed pin is never installed again")
        self.assertIn("already has every pinned component", self.step("rustup")["detail"])

    def test_pins_come_from_the_fetched_origin_main_not_the_working_tree(self) -> None:
        # The checkout's own branch moves the pin one way, an unfetched commit
        # on origin/main moves it another. Only origin/main counts.
        self.commit(self.pins("1.99.0"))
        self.advance_origin(self.pins("1.98.0"))
        out = self.sync()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertEqual(1, len(self.installs()))
        self.assertTrue(self.installs()[0].startswith("toolchain install 1.98.0 "), self.installs())
        origin_main = self.git("--git-dir", str(self.origin), "rev-parse", "main")
        self.assertEqual(origin_main, self.receipt()["ref_sha"])

    def test_a_pinned_toolchain_missing_a_component_fails_the_check(self) -> None:
        out = self.sync(FAKE_RUSTUP_DROP="clippy")
        self.assertEqual(1, out.returncode, out.stdout + out.stderr)
        receipt = self.receipt()
        self.assertEqual(["rust components"], [f["tool"] for f in receipt["findings"]])
        self.assertIn("missing clippy", receipt["findings"][0]["found"])
        self.assertEqual("failed", self.step("check")["status"])
        self.assertEqual([], receipt["network_failures"])

    def test_a_download_failure_is_kept_for_the_next_run_never_counted_as_fixed(self) -> None:
        for env, step in (({"FAKE_MISE_FAIL": "1"}, "mise"), ({"FAKE_RUSTUP_FAIL": "1"}, "rustup")):
            with self.subTest(step=step):
                self.forget_installs()
                out = self.sync(**env)
                self.assertEqual(2, out.returncode, out.stdout + out.stderr)
                receipt = self.receipt()
                self.assertEqual([step], receipt["network_failures"])
                self.assertNotEqual([], receipt["findings"])
                self.assertIn("retry later", out.stdout)
        out = self.sync()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)  # the next run converges

    def test_an_unreachable_origin_still_syncs_what_was_last_fetched_and_retries(self) -> None:
        self.origin.rename(self.root / "gone.git")
        out = self.sync()
        self.assertEqual(2, out.returncode, out.stdout + out.stderr)
        receipt = self.receipt()
        self.assertEqual(["fetch"], receipt["network_failures"])
        self.assertEqual([], receipt["findings"])
        self.assertEqual(1, len(self.installs()))

    def test_a_mise_binary_that_is_not_the_pinned_one_is_never_run(self) -> None:
        self.advance_origin(self.pins(mise_sha="f" * 64))
        out = self.sync()
        self.assertEqual(1, out.returncode, out.stdout + out.stderr)
        # check_env.py may still ask it for --version; it is never asked to install or exec.
        self.assertEqual([], [line for line in self.log("mise") if line != "--version"])
        self.assertEqual("skipped", self.step("mise")["status"])
        self.assertIn("never run", self.step("mise")["detail"])
        self.assertIn("mise", [f["tool"] for f in self.receipt()["findings"]])
        # Control: the same host with the right digest runs it.
        self.advance_origin(self.pins())
        self.assertEqual(0, self.sync().returncode)
        self.assertIn("install --locked", self.log("mise"))

    def test_a_missing_pinned_mise_is_reported_not_installed(self) -> None:
        self.mise.unlink()
        out = self.sync()
        self.assertEqual(1, out.returncode, out.stdout + out.stderr)
        self.assertIn("tartci's host installer", self.step("mise")["detail"])
        self.assertFalse(self.mise.exists())

    def test_only_additive_commands_are_run(self) -> None:
        self.assertEqual(0, self.sync().returncode)
        mise = {line.split()[0] for line in self.log("mise")}
        self.assertLessEqual(mise, {"trust", "install", "exec", "--version"}, self.log("mise"))
        rustup = {" ".join(line.split()[:2]) for line in self.log("rustup")}
        allowed = {"toolchain list", "toolchain install", "component list", "target list", "default"}
        self.assertLessEqual(rustup, allowed, self.log("rustup"))
        # `rustup default` is only ever read, never given a toolchain.
        self.assertIn("default", self.log("rustup"))
        self.assertFalse([line for line in self.log("rustup") if line.startswith("default ")])

    def test_what_it_must_not_change_is_reported(self) -> None:
        script(self.base_bin / "rustc", "#!/bin/sh\necho 'rustc 1.99.0 (homebrew)'\n")
        out = self.sync()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        notes = self.receipt()["report_only"]
        self.assertTrue(any("not rustup's proxy" in n for n in notes), notes)
        self.assertTrue(any("default rustup toolchain is stable" in n for n in notes), notes)
        if sys.platform == "darwin":
            self.assertTrue(any(n.startswith("Xcode: Xcode 26.5") for n in notes), notes)
        self.assertTrue(host_sync.is_homebrew("/opt/homebrew/bin/cmake"))
        self.assertFalse(host_sync.is_homebrew(str(self.base_bin / "rustc")))

    def test_a_cache_placed_outside_the_volume_caches_root_fails_the_sync(self) -> None:
        target = self.root / "tmp/mise-data"  # a temporary directory, like the dangling Aug-29 link
        target.mkdir()
        link = self.home / ".local/share/mise"
        link.parent.mkdir(parents=True)
        link.symlink_to(target)
        out = self.sync()
        self.assertEqual(1, out.returncode, out.stdout + out.stderr)
        found = [f for f in self.receipt()["findings"] if f["tool"] == "cache mise data"]
        self.assertEqual(1, len(found), self.receipt()["findings"])
        self.assertIn("temporary directory", found[0]["note"])
        self.assertTrue(target.is_dir(), "a finding is reported, never moved or deleted")

    def test_a_platform_without_rust_skips_rustup(self) -> None:
        pins = self.pins()
        doc = json.loads(pins["tools/toolchain/expected.json"])
        doc["platform_overrides"] = {host_sync.host_platform().split("-")[0]: {"rust": "absent"}}
        pins["tools/toolchain/expected.json"] = json.dumps(doc)
        self.advance_origin(pins)
        self.sync()
        self.assertEqual("skipped", self.step("rustup")["status"])
        self.assertEqual([], self.installs())

    def test_a_ref_without_the_pins_changes_nothing(self) -> None:
        out = self.sync("--ref", "origin/no-such-branch")
        self.assertEqual(3, out.returncode, out.stdout + out.stderr)
        self.assertEqual([], self.log("mise"))
        self.assertEqual([], self.log("rustup"))
        self.assertEqual(3, self.receipt()["exit"])

    @unittest.skipIf(os.name == "nt", "flock")
    def test_a_concurrent_run_changes_nothing(self) -> None:
        import fcntl

        lock = self.home / host_sync.LOCK
        lock.parent.mkdir(parents=True)
        with open(lock, "a+", encoding="utf-8") as handle:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
            out = self.sync()
        self.assertEqual(4, out.returncode, out.stdout + out.stderr)
        self.assertEqual([], self.log("mise"))
        self.assertFalse((self.home / ".config/pulp/toolchain-sync.json").exists())
        self.assertEqual(0, self.sync().returncode)  # control: once released, it runs


@unittest.skipIf(os.name == "nt", "the fake tools are shell scripts")
class ProvidePinnedRustTests(unittest.TestCase):
    """tools/ci/provide_pinned_rust.sh, which build.yml's build legs run before
    cargo. They set RUSTUP_AUTO_INSTALL=0, so without it a runner whose image
    lacks the pinned Rust by name fails the pulp-rs build."""

    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name).resolve()
        self.home = self.root / "home"
        script(self.home / ".cargo/bin/rustup", FAKE_RUSTUP)
        # A copy of the checkout the script resolves its root from.
        self.work = self.root / "work"
        (self.work / "tools/ci").mkdir(parents=True)
        shutil.copy(REPO / "tools/ci/provide_pinned_rust.sh", self.work / "tools/ci/provide_pinned_rust.sh")
        crate = self.work / "experimental/pulp-rs"
        crate.mkdir(parents=True)
        shutil.copy(REPO / "experimental/pulp-rs/rust-toolchain.toml", crate / "rust-toolchain.toml")
        self.github_env = self.root / "github_env"
        self.github_env.write_text("", encoding="utf-8")
        self.expected = json.loads((HERE / "expected.json").read_text(encoding="utf-8"))
        self.pin = self.expected["tools"]["rust"]

    def tearDown(self) -> None:
        self.td.cleanup()

    def provide(self, *args: str, **env: str) -> subprocess.CompletedProcess:
        # /bin/bash: macOS runners run 3.2.
        base = {"PATH": "/usr/bin:/bin", "HOME": str(self.home), "GITHUB_ENV": str(self.github_env),
                "PULP_RUST_INSTALL_RETRY_DELAY_SECS": "0", "RUSTUP_AUTO_INSTALL": "0"}
        return subprocess.run(["/bin/bash", str(self.work / "tools/ci/provide_pinned_rust.sh"), *args],
                              cwd=self.root, env=dict(base, **env), capture_output=True, text=True,
                              encoding="utf-8", check=False)

    def installs(self) -> List[str]:
        log = self.home / "rustup.log"
        lines = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
        return [line for line in lines if line.startswith("toolchain install")]

    def stable(self, version: str, components=None, targets=None) -> None:
        """Give the image a `stable` toolchain at `version` with these parts
        (default: every pinned part)."""
        root = self.home / "rustup-state/stable"
        root.mkdir(parents=True)
        (root / "version").write_text(version, encoding="utf-8")
        comps = self.expected["rust_components"] if components is None else components
        targs = self.expected["rust_targets"] if targets is None else targets
        (root / "components").write_text("".join(f"{c}\n" for c in comps), encoding="utf-8")
        (root / "targets").write_text("".join(f"{t}\n" for t in targs), encoding="utf-8")

    def branch(self, out: subprocess.CompletedProcess) -> str:
        lines = [l for l in out.stdout.splitlines() if l.startswith("rust: branch=")]
        self.assertEqual(1, len(lines), out.stdout + out.stderr)
        return lines[0].split("=", 1)[1].split(":", 1)[0]

    def test_pin_present_by_name_is_a_no_op(self) -> None:
        self.assertEqual("install", self.branch(self.provide("--fallback-to-default")))
        out = self.provide("--fallback-to-default")
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertEqual("present", self.branch(out))
        self.assertEqual(1, len(self.installs()), out.stdout)
        self.assertEqual("", self.github_env.read_text(encoding="utf-8"))

    def test_an_image_stable_that_is_the_pin_is_selected_offline(self) -> None:
        self.stable(self.pin)
        out = self.provide("--fallback-to-default", FAKE_RUSTUP_FAIL="1")  # any download would fail
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertEqual("stable-alias", self.branch(out))
        self.assertEqual([], self.installs())
        self.assertEqual("RUSTUP_TOOLCHAIN=stable\n", self.github_env.read_text(encoding="utf-8"))

    def test_a_stable_of_another_release_or_missing_a_target_installs_the_pin(self) -> None:
        for name, version, targets in (("other release", "1.99.0", None), ("no cross std", self.pin, [])):
            with self.subTest(case=name):
                shutil.rmtree(self.home / "rustup-state", ignore_errors=True)
                (self.home / "rustup.log").unlink(missing_ok=True)
                self.github_env.write_text("", encoding="utf-8")
                self.stable(version, targets=targets)
                out = self.provide("--fallback-to-default")
                self.assertEqual(0, out.returncode, out.stdout + out.stderr)
                self.assertEqual("install", self.branch(out))
                self.assertEqual(
                    [f"toolchain install {self.pin} --profile minimal --no-self-update "
                     f"--component {','.join(self.expected['rust_components'])} "
                     f"--target {','.join(self.expected['rust_targets'])}"],
                    self.installs(),
                )
                self.assertEqual("", self.github_env.read_text(encoding="utf-8"))

    def test_a_missing_component_is_installed_into_the_existing_toolchain(self) -> None:
        self.assertEqual(0, self.provide(FAKE_RUSTUP_DROP="clippy").returncode)
        out = self.provide()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertEqual(2, len(self.installs()))
        self.assertIn("was missing: clippy", out.stdout)

    def test_a_failed_install_falls_back_to_stable_naming_both_versions_only_when_asked(self) -> None:
        self.stable("1.99.0")
        out = self.provide("--fallback-to-default", FAKE_RUSTUP_FAIL="1")
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertEqual("fallback", self.branch(out))
        self.assertEqual(3, len(self.installs()), "three spaced attempts")
        self.assertIn(f"::warning title=toolchain::could not install the pinned Rust {self.pin}; "
                      f"this run builds pulp-rs with stable (rustc 1.99.0), not {self.pin}", out.stdout)
        self.assertEqual("RUSTUP_TOOLCHAIN=stable\n", self.github_env.read_text(encoding="utf-8"))
        strict = self.provide(FAKE_RUSTUP_FAIL="1")
        self.assertEqual(1, strict.returncode, strict.stdout + strict.stderr)
        self.assertIn("::error::could not install the pinned Rust", strict.stderr)

    def test_a_runner_without_rustup_is_left_alone(self) -> None:
        (self.home / ".cargo/bin/rustup").unlink()
        out = self.provide()
        self.assertEqual(0, out.returncode, out.stdout + out.stderr)
        self.assertIn("no rustup on this runner", out.stdout)

    def test_every_lane_that_builds_pulp_rs_provides_the_pin_first(self) -> None:
        build = (REPO / ".github/workflows/build.yml").read_text(encoding="utf-8")
        job = build[build.index("\n  build:\n"):]
        provide = job.index("run: bash tools/ci/provide_pinned_rust.sh --fallback-to-default")
        self.assertLess(provide, job.index("- name: Fetch pulp-rs crates"))
        self.assertLess(provide, job.index("- name: Configure"))
        for lane in ("release-cli.yml", "release-path-pr-gate.yml"):
            with self.subTest(lane=lane):
                text = (REPO / ".github/workflows" / lane).read_text(encoding="utf-8")
                self.assertIn("tools/ci/provide_pinned_rust.sh", text)
                # Adding a target to the default toolchain misses the pinned one.
                self.assertNotIn("rustup target add", text)
                self.assertNotIn("rustup component add", text)


@unittest.skipIf(os.name == "nt", "the fake tools are shell scripts")
class PostCheckoutTrustHookTests(unittest.TestCase):
    """.githooks/post-checkout trusts a new worktree's own .mise.toml."""

    NULL = "0" * 40

    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory()
        self.root = Path(self.td.name).resolve()
        self.home = self.root / "home"
        self.bin = self.root / "bin"
        script(self.bin / "mise", '#!/bin/sh\necho "$(pwd -P)|$*" >> "$HOME/mise.log"\n'
                                  '[ -z "$FAKE_MISE_TRUST_FAIL" ] || exit 1\n')
        self.home.mkdir()
        git = shutil.which("git")
        assert git
        (self.bin / "git").symlink_to(git)
        self.env = {"PATH": f"{self.bin}:/usr/bin:/bin", "HOME": str(self.home), "GIT_CONFIG_NOSYSTEM": "1"}
        self.repo = self.root / "repo"
        self.git("init", "--quiet", "-b", "main", str(self.repo))
        hooks = self.repo / ".githooks"
        hooks.mkdir()
        shutil.copy(REPO / ".githooks/post-checkout", hooks / "post-checkout")
        (hooks / "post-checkout").chmod(0o755)
        (self.repo / ".mise.toml").write_text("[settings]\nlocked = true\n", encoding="utf-8")
        self.git("-C", str(self.repo), "add", ".githooks", ".mise.toml")
        self.git("-C", str(self.repo), "commit", "--quiet", "-m", "init")
        self.git("-C", str(self.repo), "config", "core.hooksPath", ".githooks")

    def tearDown(self) -> None:
        self.td.cleanup()

    def git(self, *args: str, **env: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                              env=dict(self.env, **env), capture_output=True, text=True, encoding="utf-8", check=True)

    def log(self) -> List[str]:
        path = self.home / "mise.log"
        return path.read_text(encoding="utf-8").splitlines() if path.exists() else []

    def hook(self, cwd: Path, *args: str, path: str = "", **env: str) -> subprocess.CompletedProcess:
        run_env = dict(self.env, **env)
        if path:
            run_env["PATH"] = path
        return subprocess.run(["/bin/sh", str(REPO / ".githooks/post-checkout"), *args], cwd=cwd, env=run_env,
                              capture_output=True, text=True, encoding="utf-8", check=False)

    def test_a_new_worktree_trusts_exactly_its_own_config(self) -> None:
        worktree = self.root / "wt"
        self.git("-C", str(self.repo), "worktree", "add", "--quiet", "-b", "topic", str(worktree))
        self.assertEqual([f"{worktree}|trust --quiet {worktree}/.mise.toml"], self.log())

    def test_a_branch_switch_trusts_nothing(self) -> None:
        out = self.hook(self.repo, "1" * 40, "2" * 40, "1")
        self.assertEqual(0, out.returncode)
        self.assertEqual([], self.log())
        out = self.hook(self.repo, self.NULL, "2" * 40, "0")  # a file checkout, not a branch
        self.assertEqual([], self.log())
        self.hook(self.repo, self.NULL, "2" * 40, "1")  # control: the creation case does trust
        self.assertEqual(1, len(self.log()))

    def test_no_mise_or_no_config_is_a_quiet_success(self) -> None:
        out = self.hook(self.repo, self.NULL, "2" * 40, "1", path="/usr/bin:/bin")
        self.assertEqual((0, ""), (out.returncode, out.stderr))
        bare = self.root / "no-config"
        bare.mkdir()
        out = self.hook(bare, self.NULL, "2" * 40, "1")
        self.assertEqual((0, ""), (out.returncode, out.stderr))
        self.assertEqual([], self.log())

    def test_a_failed_trust_never_fails_the_checkout(self) -> None:
        out = self.hook(self.repo, self.NULL, "2" * 40, "1", FAKE_MISE_TRUST_FAIL="1")
        self.assertEqual(0, out.returncode)
        self.assertIn(f"run: mise trust {self.repo}/.mise.toml", out.stderr)


class SourceTests(unittest.TestCase):
    def test_source_parses_as_python_39(self) -> None:
        # It runs on a stock Mac's /usr/bin/python3 (3.9) before mise exists.
        source = (HERE / "host_sync.py").read_text(encoding="utf-8")
        ast.parse(source, feature_version=(3, 9))
        self.assertNotIn("tomllib", source)

    @unittest.skipIf(os.name == "nt", "POSIX signals")
    def test_a_hung_command_is_abandoned_within_its_bound(self) -> None:
        import time

        start = time.monotonic()
        code, _, err = host_sync.run_cmd([sys.executable, "-c", "import time; time.sleep(60)"], None,
                                         dict(os.environ), 0.5)
        self.assertEqual(124, code)
        self.assertIn("timed out", err)
        self.assertLess(time.monotonic() - start, 10)

    def test_network_errors_are_recognised(self) -> None:
        for text in ("Could not resolve host: github.com", "error: could not download file", "HTTP status 503",
                     "operation timed out"):
            self.assertTrue(host_sync.NETWORK.search(text), text)
        self.assertFalse(host_sync.NETWORK.search("error: component 'clippy' is unavailable for this target"))


if __name__ == "__main__":
    unittest.main()
