#!/usr/bin/env python3
"""Regression test for install-shipyard.sh queue-truncation repair.

Issue #534: the queue-file reset block used to
sit *after* the "already installed" short-circuit, so the documented
recovery path — "rerun the installer after Shipyard dies with
JSONDecodeError" — did not actually fire when the pinned binary was
already installed. This test exercises the script in --status mode (the
only mode that doesn't need network access) via a stubbed PULP_HOME with
a pre-populated install, and confirms that a truncated
`queue/queue.json` under the stubbed state dir is reinitialized even
though the installer reports the binary as already present.

The upstream installer is served by a stub `curl` on PATH, so the test is
offline and deterministic: it checks the wrapper's own behavior (repair the
queue, then delegate to the pinned tag's install.sh). Set
INSTALL_SHIPYARD_LIVE=1 to delegate to the real upstream install.sh instead.

It also covers the CI token preflight: under GitHub Actions the wrapper
refuses to run without GITHUB_TOKEN or SHIPYARD_GITHUB_TOKEN, while a CI run
with a token and a local run without one both proceed.

Run:
    python3 tools/scripts/test_install_shipyard_queue_repair.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
INSTALL_SH = REPO_ROOT / "tools/install-shipyard.sh"


# Binaries the upstream installer expects to find in place when
# SHIPYARD_SKIP_DOWNLOAD=1; it refuses to continue if any is missing.
_INSTALLED_BINARIES = ("shipyard", "shipyard-workstream-provider")

# Stand-in for upstream install.sh under SHIPYARD_SKIP_DOWNLOAD=1: confirm the
# pre-placed binaries and stop, as the real one does before its smoke test.
_STUB_UPSTREAM_INSTALLER = """#!/bin/sh
for binary in shipyard shipyard-workstream-provider; do
    if [ ! -x "$SHIPYARD_INSTALL_DIR/$binary" ]; then
        echo "SHIPYARD_SKIP_DOWNLOAD=1 but $SHIPYARD_INSTALL_DIR/$binary does not exist." >&2
        exit 1
    fi
done
echo "stub upstream installer: reused $SHIPYARD_INSTALL_DIR ($SHIPYARD_VERSION)"
"""


def _install_fixture(home: Path) -> tuple[Path, dict[str, str]]:
    """Pre-place every binary the upstream installer reuses, and build the
    environment that runs the wrapper against them offline."""
    bin_dir = home / ".local" / "bin"
    bin_dir.mkdir(parents=True)
    suffix = ".exe" if sys.platform.startswith("win") else ""
    for name in _INSTALLED_BINARIES:
        binary = bin_dir / f"{name}{suffix}"
        # Upstream's post-install smoke reads `<name> <semver>` from --version.
        binary.write_text(f"#!/bin/sh\necho '{name} 0.0.0-test'\n")
        binary.chmod(0o755)

    env = os.environ.copy()
    path = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
    if os.environ.get("INSTALL_SHIPYARD_LIVE") != "1":
        stub_dir = home / "stub-bin"
        stub_dir.mkdir()
        installer = stub_dir / "upstream-install.sh"
        installer.write_text(_STUB_UPSTREAM_INSTALLER)
        curl = stub_dir / "curl"
        curl.write_text(
            f'#!/bin/sh\nprintf \'%s\\n\' "$@" >> "{stub_dir / "curl-args"}"\n'
            f'cat "{installer}"\n'
        )
        curl.chmod(0o755)
        path = f"{stub_dir}{os.pathsep}{path}"
    env["HOME"] = str(home)
    env["PATH"] = path
    env["SHIPYARD_INSTALL_DIR"] = str(bin_dir)
    env["SHIPYARD_SKIP_DOWNLOAD"] = "1"
    env["SHIPYARD_SKIP_SMOKE"] = "1"
    # Unset XDG_STATE_HOME so Linux path falls back to ~/.local/state.
    env.pop("XDG_STATE_HOME", None)
    # Each test states its own CI/token context; inheriting the runner's would
    # make the outcome depend on where the suite happens to run.
    for name in ("GITHUB_ACTIONS", "GITHUB_TOKEN", "SHIPYARD_GITHUB_TOKEN"):
        env.pop(name, None)
    return bin_dir, env


def _stub_state_dir(home: Path) -> Path:
    """Return the platform-specific Shipyard state dir under a fake HOME."""
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "shipyard"
    if sys.platform.startswith("win"):
        return home / "AppData" / "Local" / "shipyard"
    return home / ".local" / "state" / "shipyard"


class InstallShipyardQueueRepair(unittest.TestCase):
    """#534: queue repair must run before the already-installed early exit."""

    def _run_install(self, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(INSTALL_SH)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_queue_repaired_even_when_already_installed(self) -> None:
        """Truncated queue.json gets repaired on the already-installed path."""
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)

            # Pre-populate the upstream installer's canonical destination
            # and ask it to reuse those binaries, so the wrapper is proven to
            # repair queue.json before delegating to Shipyard's installer.
            _bin_dir, env = _install_fixture(home)

            # Write a truncated (zero-byte) queue file under the stubbed
            # state dir to simulate the #528 recovery scenario.
            state_dir = _stub_state_dir(home)
            queue_file = state_dir / "queue" / "queue.json"
            queue_file.parent.mkdir(parents=True)
            queue_file.touch()
            self.assertEqual(queue_file.stat().st_size, 0)

            result = self._run_install(env)
            self.assertEqual(
                result.returncode, 0,
                msg=f"installer failed: stdout={result.stdout} stderr={result.stderr}",
            )

            # The queue file must now contain the reinitialized JSON even
            # though the script took the "already installed" short-circuit.
            body = queue_file.read_text()
            self.assertIn("jobs", body,
                          msg=f"queue file was not repaired; contents: {body!r}")
            self.assertIn("→ Shipyard queue file is empty — reinitializing",
                          result.stdout)
            if os.environ.get("INSTALL_SHIPYARD_LIVE") != "1":
                # Delegated to the pinned tag's installer, not main's.
                requested = (home / "stub-bin" / "curl-args").read_text()
                self.assertRegex(
                    requested,
                    r"https://raw\.githubusercontent\.com/danielraffel/Shipyard/refs/tags/v[0-9.]+/install\.sh",
                )
                self.assertIn("stub upstream installer: reused", result.stdout)

    def test_healthy_queue_file_is_untouched(self) -> None:
        """A non-empty queue file must NOT be overwritten."""
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            _bin_dir, env = _install_fixture(home)

            state_dir = _stub_state_dir(home)
            queue_file = state_dir / "queue" / "queue.json"
            queue_file.parent.mkdir(parents=True)
            original = '{"jobs": [{"id": "abc", "status": "running"}]}'
            queue_file.write_text(original)

            result = self._run_install(env)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(queue_file.read_text(), original,
                             msg="installer clobbered a healthy queue.json")


class InstallShipyardCiTokenPreflight(unittest.TestCase):
    """Under GitHub Actions the wrapper requires a token for the API lookup."""

    def _run(self, extra: dict[str, str]) -> tuple[subprocess.CompletedProcess[str], Path]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        home = Path(tmp.name)
        _bin_dir, env = _install_fixture(home)
        env.update(extra)
        result = subprocess.run(
            ["bash", str(INSTALL_SH)],
            env=env, capture_output=True, text=True, check=False,
        )
        return result, home

    def test_ci_without_token_refuses_before_any_download(self) -> None:
        result, home = self._run({"GITHUB_ACTIONS": "true"})
        self.assertEqual(result.returncode, 1, msg=result.stdout + result.stderr)
        self.assertIn("running under GitHub Actions without a token", result.stderr)
        self.assertIn("GITHUB_TOKEN: ${{ github.token }}", result.stderr)
        self.assertNotIn("stub upstream installer", result.stdout)
        if os.environ.get("INSTALL_SHIPYARD_LIVE") != "1":
            self.assertFalse((home / "stub-bin" / "curl-args").exists(),
                             msg="refusal must happen before the installer is fetched")

    def test_ci_with_empty_token_refuses(self) -> None:
        result, _home = self._run({"GITHUB_ACTIONS": "true", "GITHUB_TOKEN": ""})
        self.assertEqual(result.returncode, 1)

    def test_ci_with_either_token_proceeds(self) -> None:
        for var in ("GITHUB_TOKEN", "SHIPYARD_GITHUB_TOKEN"):
            with self.subTest(var=var):
                result, _home = self._run({"GITHUB_ACTIONS": "true", var: "test-token"})
                self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
                if os.environ.get("INSTALL_SHIPYARD_LIVE") != "1":
                    self.assertIn("stub upstream installer: reused", result.stdout)

    def test_local_without_token_proceeds(self) -> None:
        result, _home = self._run({})
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)
        if os.environ.get("INSTALL_SHIPYARD_LIVE") != "1":
            self.assertIn("stub upstream installer: reused", result.stdout)


_ASSET = b"pinned shipyard linux-x64 binary\n"
_PROVIDER = b"pinned provider linux-x64 binary\n"


def _sha(data: bytes) -> str:
    import hashlib

    return hashlib.sha256(data).hexdigest()


@unittest.skipIf(sys.platform.startswith("win"), "the stubs are POSIX shell scripts")
class InstallShipyardVerifiesDigests(unittest.TestCase):
    """The wrapper refuses a release asset whose sha256 differs from the pin,
    before the upstream installer runs, and refuses an installed binary that is
    not the verified one. Runs as a Linux x64 host through a stub `uname`."""

    def _tree(self, root: Path, *, sums: dict[str, str], installed: bytes) -> dict[str, str]:
        tools = root / "tools"
        (tools / "toolchain").mkdir(parents=True)
        (tools / "install-shipyard.sh").write_bytes(INSTALL_SH.read_bytes())
        (tools / "shipyard.toml").write_text(
            '[shipyard]\nversion = "v9.9.9"\nrepo = "example/Shipyard"\n', encoding="utf-8")
        (tools / "toolchain/shipyard.sha256").write_text(
            "# generated\n" + "".join(f"{sha}  {name}\n" for name, sha in sums.items()), encoding="utf-8")
        stub = root / "stub"
        stub.mkdir()
        assets = root / "assets"
        assets.mkdir()
        (assets / "shipyard-linux-x64").write_bytes(_ASSET)
        (assets / "shipyard-workstream-provider-linux-x64").write_bytes(_PROVIDER)
        (root / "installed").write_bytes(installed)
        installer = stub / "upstream-install.sh"
        installer.write_text(
            "#!/bin/sh\nmkdir -p \"$SHIPYARD_INSTALL_DIR\"\n"
            f"cp \"{root / 'installed'}\" \"$SHIPYARD_INSTALL_DIR/shipyard\"\necho 'stub upstream installer: ran'\n",
            encoding="utf-8")
        # Release downloads are served from `assets`, the installer from the stub.
        (stub / "curl").write_text(
            "#!/bin/sh\n"
            f'printf "%s\\n" "$*" >> "{root / "curl-args"}"\n'
            'out=""; url=""\n'
            'while [ $# -gt 0 ]; do case "$1" in -o) out="$2"; shift 2;; -*) shift;; *) url="$1"; shift;; esac; done\n'
            'case "$url" in\n'
            f'  */releases/download/*) cp "{assets}/$(basename "$url")" "$out" ;;\n'
            f'  *install.sh) cat "{installer}" ;;\n'
            '  *) exit 22 ;;\n'
            'esac\n', encoding="utf-8")
        (stub / "uname").write_text(
            '#!/bin/sh\ncase "$1" in -s) echo Linux;; -m) echo x86_64;; *) echo Linux;; esac\n', encoding="utf-8")
        for name in ("curl", "uname"):
            (stub / name).chmod(0o755)
        home = root / "home"
        home.mkdir()
        env = {k: v for k, v in os.environ.items() if k not in ("GITHUB_ACTIONS", "GITHUB_TOKEN", "SHIPYARD_GITHUB_TOKEN", "SHIPYARD_SKIP_DOWNLOAD")}
        env.update({"HOME": str(home), "PATH": f"{stub}{os.pathsep}{env.get('PATH', '')}",
                    "SHIPYARD_INSTALL_DIR": str(home / "bin")})
        return env

    def _run(self, root: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["bash", str(root / "tools/install-shipyard.sh")], env=env,
                              capture_output=True, text=True, encoding="utf-8", check=False)

    def pinned(self) -> dict[str, str]:
        return {"shipyard-linux-x64": _sha(_ASSET), "shipyard-workstream-provider-linux-x64": _sha(_PROVIDER)}

    def test_pinned_assets_install(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            result = self._run(root, self._tree(root, sums=self.pinned(), installed=_ASSET))
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn("Verified shipyard-linux-x64 shipyard-workstream-provider-linux-x64", result.stdout)
            self.assertIn("stub upstream installer: ran", result.stdout)
            args = (root / "curl-args").read_text(encoding="utf-8")
            self.assertIn("https://github.com/example/Shipyard/releases/download/v9.9.9/shipyard-linux-x64", args)

    def test_replaced_asset_is_refused_before_the_installer_runs(self) -> None:
        for name in self.pinned():
            with self.subTest(asset=name), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                sums = self.pinned()
                sums[name] = "0" * 64
                result = self._run(root, self._tree(root, sums=sums, installed=_ASSET))
                self.assertEqual(2, result.returncode, result.stdout + result.stderr)
                self.assertIn(f"/{name} does not match the pin", result.stderr)
                self.assertIn("pinned sha256:   " + "0" * 64, result.stderr)
                self.assertIn("generate.py --shipyard v9.9.9", result.stderr)
                self.assertNotIn("install.sh", (root / "curl-args").read_text(encoding="utf-8"))

    def test_asset_without_a_pinned_digest_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            sums = self.pinned()
            del sums["shipyard-workstream-provider-linux-x64"]
            result = self._run(root, self._tree(root, sums=sums, installed=_ASSET))
            self.assertEqual(2, result.returncode)
            self.assertIn("pins no sha256 for shipyard-workstream-provider-linux-x64", result.stderr)

    def test_installed_binary_must_be_the_verified_one(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            result = self._run(root, self._tree(root, sums=self.pinned(), installed=b"swapped between downloads\n"))
            self.assertEqual(2, result.returncode, result.stdout + result.stderr)
            self.assertIn("is not the pinned Shipyard v9.9.9 binary", result.stderr)


if __name__ == "__main__":
    unittest.main()
