#!/usr/bin/env python3
"""Contract tests for the combined macOS installer component graph."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "scripts" / "build_combined_installer.sh"


class NotarizationHeartbeatTest(unittest.TestCase):
    """The liveness heartbeat around the notarization wait.

    `xcrun notarytool submit --wait` writes its whole poll phase as one
    unterminated line, flushed only at the verdict, and stdio block-buffers
    once stdout is a file. The packaging wrappers also `exec` into this recipe,
    so their name leaves the command line seconds in. Both instruments a
    watcher can reach therefore read "dead" on a healthy run, for the 5-30
    minutes every notarized release spends there — which is how two live
    pipelines were declared dead and hand-"recovered" while still running.

    The heartbeat is the recipe's own signal, and these tests drive the real
    recipe through its real notarize path with a stand-in `xcrun`, so they
    cover the product and not just the helper. Two properties, pulling
    opposite ways: the heartbeat must be VISIBLE mid-flight, and it must not
    weaken the `set -euo pipefail` guarantee that a failed notarization aborts
    before `stapler staple`.
    """

    def _fixture(self, tmp: Path, notary_rc: int, notary_delay: str = "0") -> dict:
        """A minimal signable tree plus a stand-in `xcrun`.

        Returns the argv/env/capture paths for a run that takes the notarytool
        branch (no `$CLI`, so the recipe falls through to notarytool exactly as
        a submodule/standalone consumer does).
        """
        fake_bin = tmp / "bin"
        fake_bin.mkdir()
        staple_capture = tmp / "staple-argv.txt"
        output = tmp / "out"

        def tool(name: str, body: str) -> None:
            path = fake_bin / name
            path.write_text("#!/bin/bash\n" + body)
            path.chmod(0o755)

        tool("codesign", "exit 0\n")
        tool("file", 'case "${!#}" in\n'
                     '  */Contents/MacOS/*) echo "Mach-O 64-bit executable";;\n'
                     '  *) /usr/bin/file "$@";;\n'
                     'esac\n')
        # Mirrors the graph test's shim: the recipe runs the real
        # ensure_signing_ready.sh preflight, which reads the keychain search
        # list and refuses to sign if it comes back empty.
        tool("security",
             'if [[ "${1:-}" == "find-identity" ]]; then\n'
             '  echo \'  1) ABC "Developer ID Application: Test (TEAMID0000)"\'\n'
             'elif [[ "${1:-}" == "list-keychains" && "$*" != *" -s "* ]]; then\n'
             '  printf \'    "%s"\\n\' "$PULP_SIGN_KEYCHAIN"\n'
             'fi\n'
             'exit 0\n')
        # Bundle relocation validation is outside what these tests cover.
        tool("python3", "exit 0\n")
        tool("pkgbuild", 'last=""\nfor a in "$@"; do last="$a"; done\n'
                         'mkdir -p "$(dirname "$last")"\n: > "$last"\n')
        tool("productbuild", 'last=""\nfor a in "$@"; do last="$a"; done\n'
                             'mkdir -p "$(dirname "$last")"\n: > "$last"\n')
        tool("productsign", 'last=""\nfor a in "$@"; do last="$a"; done\n'
                            'mkdir -p "$(dirname "$last")"\n: > "$last"\n')
        # The stand-in for Apple. `submit --wait` sleeps (standing in for the
        # dark poll phase), prints what notarytool prints, and exits with the
        # status under test. `staple` records that it ran at all — that record
        # is the whole point of the failure-path test.
        tool("xcrun",
             'case "$1 $2" in\n'
             '  "notarytool submit")\n'
             f'    sleep {notary_delay}\n'
             '    echo "  id: 00000000-0000-0000-0000-000000000000"\n'
             '    echo "notarytool-stderr-marker" >&2\n'
             f'    exit {notary_rc}\n'
             '    ;;\n'
             '  "stapler staple"|"stapler validate")\n'
             '    printf "%s\\n" "$*" >> "$CAPTURE_STAPLE_ARGV"\n'
             '    exit 0\n'
             '    ;;\n'
             'esac\n'
             'exit 0\n')

        bundle = tmp / "Fixture.clap"
        macos = bundle / "Contents" / "MacOS"
        macos.mkdir(parents=True)
        (bundle / "Contents" / "Info.plist").write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<plist version="1.0"><dict>'
            "<key>CFBundleExecutable</key><string>Fixture</string>"
            "</dict></plist>\n"
        )
        executable = macos / "Fixture"
        executable.write_text("#!/bin/bash\nexit 0\n")
        executable.chmod(0o755)

        argv = [
            "/bin/bash", str(SCRIPT),
            "--name", "Fixture",
            "--version", "1.2.3",
            "--sign-identity", "application-fixture",
            "--installer-identity", "installer-fixture",
            "--out", str(output),
            "--plugin", "clap", str(bundle),
        ]
        env = {
            # PULP_CPP is pointed at a path that does not exist so the recipe
            # takes the notarytool fallback, the branch a consumer without a
            # built pulp-cpp always takes.
            "PATH": f"{fake_bin}:/usr/bin:/bin",
            "HOME": str(tmp),
            "TMPDIR": str(tmp),
            "PULP_CPP": str(tmp / "no-such-cli"),
            "CAPTURE_STAPLE_ARGV": str(staple_capture),
            "PULP_NOTARY_KEY_ID": "FIXTUREKEY",
            "PULP_NOTARY_ISSUER_ID": "fixture-issuer",
            "PULP_NOTARY_KEY_PATH": str(tmp / "AuthKey_FIXTUREKEY.p8"),
            "PULP_SIGN_KEYCHAIN": str(tmp / "signing.keychain-db"),
            "PULP_SIGN_KEYCHAIN_PW": "test-keychain-password",
            "PULP_SIGN_P12": str(tmp / "signing.p12"),
            "PULP_SIGN_P12_PW": "test-p12-password",
            "PULP_SIGN_IDENTITY_HASH": "ABC",
            "PULP_NOTARIZE_HEARTBEAT_SECS": "1",
        }
        Path(env["PULP_NOTARY_KEY_PATH"]).touch()
        Path(env["PULP_SIGN_KEYCHAIN"]).touch()
        Path(env["PULP_SIGN_P12"]).touch()
        return {"argv": argv, "env": env, "staple": staple_capture}

    def _run(self, fixture: dict) -> subprocess.CompletedProcess:
        return subprocess.run(
            fixture["argv"], cwd=ROOT, env=fixture["env"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )

    def test_the_wait_is_observable_while_it_is_still_running(self) -> None:
        """The discriminator notarytool's own output does not provide.

        Read the log MID-FLIGHT, not after: a heartbeat that only appears in
        the final transcript would leave the dark window exactly as it was.
        """
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            fixture = self._fixture(tmp, notary_rc=0, notary_delay="6")
            log = tmp / "run.log"
            with log.open("w") as handle:
                proc = subprocess.Popen(
                    fixture["argv"], cwd=ROOT, env=fixture["env"], text=True,
                    stdout=handle, stderr=subprocess.STDOUT,
                )
                try:
                    beats = 0
                    alive_when_seen = False
                    for _ in range(300):  # up to 30s, well inside the 6s wait
                        time.sleep(0.1)
                        text = log.read_text(errors="replace")
                        if "[heartbeat] notarization in progress" in text:
                            beats = text.count("[heartbeat] notarization in progress")
                            alive_when_seen = proc.poll() is None
                            break
                    # Positive control: a heartbeat seen only after the run
                    # finished would prove nothing about the dark window.
                    self.assertTrue(
                        alive_when_seen,
                        msg="no heartbeat observed while the run was still alive; "
                            f"log so far:\n{log.read_text(errors='replace')}",
                    )
                    self.assertGreaterEqual(beats, 1)
                finally:
                    proc.wait(timeout=120)
            self.assertEqual(proc.returncode, 0, msg=log.read_text(errors="replace"))

    def test_a_wait_shorter_than_one_interval_emits_no_heartbeat(self) -> None:
        """Negative control for the test above.

        If a heartbeat line appeared here too, that test would be asserting a
        constant rather than measuring the wait.
        """
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            fixture = self._fixture(tmp, notary_rc=0, notary_delay="0")
            fixture["env"]["PULP_NOTARIZE_HEARTBEAT_SECS"] = "30"
            result = self._run(fixture)
            self.assertEqual(result.returncode, 0, msg=result.stdout)
            self.assertNotIn("[heartbeat]", result.stdout)

    def test_a_successful_notarization_staples_and_keeps_notarytool_output(self) -> None:
        """The success path, plus the output a human reads to check on Apple.

        Wrapping the wait must not swallow notarytool's own stdout/stderr —
        the submission ID is what someone uses to query Apple by hand, and
        losing it would trade one dark instrument for another.
        """
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            fixture = self._fixture(tmp, notary_rc=0)
            result = self._run(fixture)
            self.assertEqual(result.returncode, 0, msg=result.stdout)
            self.assertIn("id: 00000000-0000-0000-0000-000000000000", result.stdout)
            self.assertIn("notarytool-stderr-marker", result.stdout)
            self.assertIn("stapler staple", fixture["staple"].read_text())
            self.assertIn("OK \u2192", result.stdout)

    def test_a_failed_notarization_never_reaches_stapler_staple(self) -> None:
        """The guarantee the heartbeat must not weaken.

        An un-notarized .pkg that reached staple/validate/"OK \u2192" would look
        finished and be rejected by Gatekeeper on the user's machine. The
        recipe's own history has that incident: a failing `$CLI ship notarize`
        under `set -e` once left exactly such a package.
        """
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            fixture = self._fixture(tmp, notary_rc=4)
            result = self._run(fixture)
            self.assertNotEqual(result.returncode, 0, msg=result.stdout)
            self.assertFalse(
                fixture["staple"].exists(),
                msg=f"stapler ran after a failed notarization:\n{result.stdout}",
            )
            self.assertNotIn("OK \u2192", result.stdout)


class CombinedInstallerTest(unittest.TestCase):
    def _write_tool(self, directory: Path, name: str, body: str) -> None:
        path = directory / name
        path.write_text("#!/bin/bash\nset -euo pipefail\n" + body)
        path.chmod(0o755)

    def _run_installer(
        self,
        plugins: list[tuple[str, str]],
        apps: list[tuple[str, str]] | None = None,
        grouped_apps: list[tuple[str, str, str]] | None = None,
        product_titles: list[tuple[str, str]] | None = None,
        scripted_apps: set[str] | None = None,
        architectures: str | None = None,
        d15_fixture: str | None = None,
        expect_success: bool = True,
    ) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            capture = tmp / "distribution.xml"
            relocation_capture = tmp / "app-relocation.txt"
            pkg_argv_capture = tmp / "pkgbuild-argv.txt"
            codesign_argv_capture = tmp / "codesign-argv.txt"
            productbuild_argv_capture = tmp / "productbuild-argv.txt"
            productsign_argv_capture = tmp / "productsign-argv.txt"
            output = tmp / "out"

            self._write_tool(
                fake_bin,
                "codesign",
                'printf "%s\\n" "$*" >> "$CAPTURE_CODESIGN_ARGV"\nexit 0\n',
            )
            self._write_tool(
                fake_bin,
                "file",
                'case "${!#}" in\n'
                '  */Contents/MacOS/*) echo "Mach-O 64-bit executable";;\n'
                '  *) /usr/bin/file "$@";;\n'
                'esac\n',
            )
            self._write_tool(
                fake_bin,
                "security",
                'if [[ "${1:-}" == "find-identity" ]]; then\n'
                '  echo \'  1) ABC "Developer ID Application: Test (TEAMID0000)"\'\n'
                'elif [[ "${1:-}" == "list-keychains" && "$*" != *" -s "* ]]; then\n'
                '  printf \'    "%s"\\n\' "$PULP_SIGN_KEYCHAIN"\n'
                'fi\n'
                "exit 0\n",
            )
            self._write_tool(
                fake_bin,
                "python3",
                "# Bundle relocation validation is outside this graph test.\n"
                "exit 0\n",
            )
            self._write_tool(
                fake_bin,
                "pkgbuild",
                'printf "BEGIN\\n" >> "$CAPTURE_PKG_ARGV"\n'
                'printf "%s\\n" "$@" >> "$CAPTURE_PKG_ARGV"\n'
                'last=""\nanalyze=0\ncomponent_plist=""\nwant_component_plist=0\n'
                'for arg in "$@"; do\n'
                '  if [[ "$want_component_plist" == 1 ]]; then component_plist="$arg"; want_component_plist=0; fi\n'
                '  [[ "$arg" == "--analyze" ]] && analyze=1\n'
                '  [[ "$arg" == "--component-plist" ]] && want_component_plist=1\n'
                '  last="$arg"\n'
                'done\n'
                'if [[ "$analyze" == 1 ]]; then\n'
                '  cat > "$last" <<\'PLIST\'\n'
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                '<plist version="1.0"><array><dict>\n'
                '<key>BundleIsRelocatable</key><true/>\n'
                '<key>RootRelativeBundlePath</key><string>Applications/Fixture.app</string>\n'
                '</dict><dict>\n'
                '<key>BundleIsRelocatable</key><true/>\n'
                '<key>RootRelativeBundlePath</key>'
                '<string>Applications/Fixture.app/Contents/Helpers/Helper.app</string>\n'
                '</dict></array></plist>\n'
                'PLIST\n'
                '  exit 0\n'
                'fi\n'
                'if [[ -n "$component_plist" ]]; then\n'
                '  /usr/libexec/PlistBuddy -c "Print :0:BundleIsRelocatable" '
                '"$component_plist" >> "$CAPTURE_APP_RELOCATION"\n'
                '  /usr/libexec/PlistBuddy -c "Print :1:BundleIsRelocatable" '
                '"$component_plist" >> "$CAPTURE_APP_RELOCATION"\n'
                'fi\n'
                'mkdir -p "$(dirname "$last")"\n: > "$last"\n',
            )
            self._write_tool(
                fake_bin,
                "productbuild",
                'distribution=""\nlast=""\nwant_distribution=0\n'
                'for arg in "$@"; do\n'
                '  if [[ "$want_distribution" == 1 ]]; then distribution="$arg"; want_distribution=0; fi\n'
                '  [[ "$arg" == "--distribution" ]] && want_distribution=1\n'
                '  last="$arg"\n'
                'done\n'
                'printf "%s\\n" "$*" >> "$CAPTURE_PRODUCTBUILD_ARGV"\n'
                'cp "$distribution" "$CAPTURE_XML"\n'
                'mkdir -p "$(dirname "$last")"\n: > "$last"\n',
            )
            self._write_tool(
                fake_bin,
                "productsign",
                'printf "%s\\n" "$*" >> "$CAPTURE_PRODUCTSIGN_ARGV"\n'
                'last=""\nfor arg in "$@"; do last="$arg"; done\n'
                'mkdir -p "$(dirname "$last")"\n: > "$last"\n',
            )

            args = [
                "/bin/bash",
                str(SCRIPT),
                "--name",
                "Fixture",
                "--version",
                "1.2.3",
                "--sign-identity",
                "application-fixture",
                "--installer-identity",
                "installer-fixture",
                "--out",
                str(output),
                "--no-notarize",
            ]
            if architectures:
                args.extend(("--architectures", architectures))
            plugin_bundles: list[Path] = []
            for plugin_name, kind in plugins:
                suffix = {"au": "component", "vst3": "vst3", "clap": "clap"}[kind]
                bundle = tmp / f"{plugin_name}.{suffix}"
                macos = bundle / "Contents" / "MacOS"
                macos.mkdir(parents=True)
                (bundle / "Contents" / "Info.plist").write_text(
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<plist version=\"1.0\"><dict>"
                    f"<key>CFBundleExecutable</key><string>{plugin_name}</string>"
                    "</dict></plist>\n"
                )
                executable = macos / plugin_name
                executable.write_text("fixture executable\n")
                executable.chmod(0o755)
                if d15_fixture and not plugin_bundles:
                    if d15_fixture == "file":
                        (macos / "libvellum-gpu.dylib").write_bytes(
                            b"D15 fixture\n"
                        )
                    elif d15_fixture == "sidecar-symlink":
                        provider = tmp / "provider-libvellum-gpu.dylib"
                        provider.write_bytes(b"D15 fixture\n")
                        (macos / "libvellum-gpu.dylib").symlink_to(provider)
                    elif d15_fixture == "bundle-symlink":
                        (macos / "libvellum-gpu.dylib").write_bytes(
                            b"D15 fixture\n"
                        )
                        real_bundle = tmp / f"real-{bundle.name}"
                        bundle.rename(real_bundle)
                        bundle.symlink_to(real_bundle, target_is_directory=True)
                    else:
                        raise AssertionError(f"unknown D15 fixture: {d15_fixture}")
                for evidence_name in (
                    f"{plugin_name}.inspector-capabilities.json",
                    f"{plugin_name}.{kind}.control-shipping.json",
                    f"{plugin_name}.{kind}.control-shipping-report.json",
                ):
                    (macos / evidence_name).write_text("{}\n")
                plugin_bundles.append(bundle)
                args.extend(("--plugin", kind, str(bundle)))
            for title, app_name in apps or []:
                bundle = tmp / f"{app_name}.app"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--app", title, str(bundle)))
                if title in (scripted_apps or set()):
                    scripts = tmp / f"{title}-scripts"
                    scripts.mkdir()
                    hook = scripts / "postinstall"
                    hook.write_text("#!/bin/bash\nexit 0\n")
                    hook.chmod(0o755)
                    args.extend(("--app-scripts", title, str(scripts)))
            # Apps nested under a product group, which are also forced on.
            for group, title, app_name in grouped_apps or []:
                bundle = tmp / f"{app_name}.app"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--app-for", group, title, str(bundle)))
                if title in (scripted_apps or set()):
                    scripts = tmp / f"{title}-scripts"
                    scripts.mkdir()
                    hook = scripts / "postinstall"
                    hook.write_text("#!/bin/bash\nexit 0\n")
                    hook.chmod(0o755)
                    args.extend(("--app-scripts", title, str(scripts)))
            for bundle_name, title in product_titles or []:
                args.extend(("--product-title", bundle_name, title))

            env = {
                "PATH": f"{fake_bin}:/usr/bin:/bin",
                "HOME": str(tmp),
                "TMPDIR": str(tmp),
                "CAPTURE_XML": str(capture),
                "CAPTURE_APP_RELOCATION": str(relocation_capture),
                "CAPTURE_PKG_ARGV": str(pkg_argv_capture),
                "CAPTURE_CODESIGN_ARGV": str(codesign_argv_capture),
                "CAPTURE_PRODUCTBUILD_ARGV": str(productbuild_argv_capture),
                "CAPTURE_PRODUCTSIGN_ARGV": str(productsign_argv_capture),
                "PULP_SIGN_KEYCHAIN": str(tmp / "signing.keychain-db"),
                "PULP_SIGN_KEYCHAIN_PW": "test-keychain-password",
                "PULP_SIGN_P12": str(tmp / "signing.p12"),
                "PULP_SIGN_P12_PW": "test-p12-password",
                "PULP_SIGN_IDENTITY_HASH": "ABC",
            }
            Path(env["PULP_SIGN_KEYCHAIN"]).touch()
            Path(env["PULP_SIGN_P12"]).touch()
            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if not expect_success:
                self.assertNotEqual(completed.returncode, 0)
                for forbidden_capture in (
                    codesign_argv_capture,
                    pkg_argv_capture,
                    productbuild_argv_capture,
                    productsign_argv_capture,
                ):
                    self.assertFalse(
                        forbidden_capture.exists(),
                        f"release tool ran before D15 rejection: {forbidden_capture}",
                    )
                return completed.stdout + completed.stderr, ""
            self.assertEqual(
                completed.returncode,
                0,
                msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            self.assertTrue(
                capture.is_file(),
                msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            relocation = (
                relocation_capture.read_text()
                if relocation_capture.is_file()
                else ""
            )
            self._last_pkgbuild_argv = (
                pkg_argv_capture.read_text() if pkg_argv_capture.is_file() else ""
            )
            self._last_codesign_argv = (
                codesign_argv_capture.read_text()
                if codesign_argv_capture.is_file()
                else ""
            )
            self._last_productbuild_argv = (
                productbuild_argv_capture.read_text()
                if productbuild_argv_capture.is_file()
                else ""
            )
            self._last_productsign_argv = (
                productsign_argv_capture.read_text()
                if productsign_argv_capture.is_file()
                else ""
            )
            self._last_plugin_evidence_relocated = all(
                not list((bundle / "Contents" / "MacOS").glob("*.json"))
                and len(list((bundle / "Contents" / "Resources" /
                              "pulp-control-shipping-evidence").glob("*.json"))) == 3
                for bundle in plugin_bundles
            )
            self._last_plugin_main_paths = [
                str(bundle / "Contents" / "MacOS" / bundle.stem)
                for bundle in plugin_bundles
            ]
            return capture.read_text(), relocation

    def test_development_vellum_runtime_is_rejected_before_signing(self) -> None:
        for fixture in ("file", "sidecar-symlink", "bundle-symlink"):
            with self.subTest(fixture=fixture):
                output, _ = self._run_installer(
                    [("GpuAudio", "clap")],
                    d15_fixture=fixture,
                    expect_success=False,
                )
                self.assertIn("development-only Vellum D15", output)

    def test_the_product_archive_is_signed_by_productsign_not_productbuild(
        self,
    ) -> None:
        """The dedicated signing keychain authorizes the Developer ID Installer
        key for productsign, not productbuild. `productbuild --sign` is
        therefore denied and, headless, returns CSSMERR_CSP_USER_CANCELED after
        every bundle has already been signed."""
        self._run_installer([("Kick", "au")])
        self.assertNotIn(
            "--sign",
            self._last_productbuild_argv,
            msg="productbuild must build the product archive UNSIGNED",
        )
        self.assertIn(
            "installer-fixture",
            self._last_productsign_argv,
            msg="productsign must sign the archive with the installer identity",
        )

    def test_multi_plugin_packages_are_unique_and_grouped_by_plugin(self) -> None:
        xml, _ = self._run_installer(
            [("Kick", "au"), ("Kick", "clap"),
             ("Snare", "au"), ("Snare", "clap")]
        )

        for choice in ("plugin-0-au", "plugin-0-clap",
                       "plugin-1-au", "plugin-1-clap"):
            self.assertEqual(xml.count(f'choice id="{choice}"'), 1)
            self.assertIn(f'com.pulp.Fixture.{choice}.pkg', xml)
        for package in ("Kick.au.pkg", "Kick.clap.pkg",
                        "Snare.au.pkg", "Snare.clap.pkg"):
            self.assertEqual(xml.count(package), 1)
        self.assertIn('<line choice="plugin-0">', xml)
        self.assertIn('<line choice="plugin-1">', xml)
        self.assertIn('<line choice="plugin-0-au"/>', xml)
        self.assertIn('<line choice="plugin-1-au"/>', xml)

    def test_single_plugin_keeps_a_flat_format_outline(self) -> None:
        xml, _ = self._run_installer([("Kick", "au"), ("Kick", "clap")])

        self.assertNotIn('choice="plugin-0">', xml)
        self.assertIn('<line choice="plugin-0-au"/>', xml)
        self.assertIn('<line choice="plugin-0-clap"/>', xml)
        self.assertTrue(self._last_plugin_evidence_relocated)
        for main_path in self._last_plugin_main_paths:
            self.assertFalse(any(
                line.endswith(main_path)
                for line in self._last_codesign_argv.splitlines()
            ))

    def test_distinct_names_with_the_same_lossy_slug_do_not_collide(self) -> None:
        xml, _ = self._run_installer([("Foo-Bar", "au"), ("Foo Bar", "au")])

        self.assertIn('choice id="plugin-0-au"', xml)
        self.assertIn('choice id="plugin-1-au"', xml)
        self.assertIn('title="Foo-Bar"', xml)
        self.assertIn('title="Foo Bar"', xml)

    def test_a_products_standalone_nests_in_its_group_and_cannot_be_deselected(
            self) -> None:
        # A user thinks in products — "Kelvin, and which formats of it" — not in
        # a flat list where the same plugin appears once as a format group and
        # again as an app under a different name. The standalone also carries
        # the uninstaller, so a user who deselects it installs plugins they
        # cannot later remove; the row is shown but its checkbox is refused.
        xml, _ = self._run_installer(
            [("Kelvin", "au"), ("Kelvin", "vst3"), ("Lattice", "au")],
            grouped_apps=[("Kelvin", "Standalone app", "Kelvin")],
            product_titles=[("Kelvin", "Kelvin \u2014 instrument")],
        )
        # The display title replaces the bundle name on the group.
        self.assertIn('title="Kelvin \u2014 instrument"', xml)
        # The app choice is nested inside the group, not at the top level.
        group = re.search(
            r'<line choice="plugin-0">(.*?)</line>', xml, re.S)
        self.assertIsNotNone(group)
        self.assertIn("standalone-app", group.group(1))
        # And it is forced on.
        choice = re.search(
            r'<choice id="standalone-app"[^>]*>', xml)
        self.assertIsNotNone(choice)
        self.assertIn('enabled="false"', choice.group(0))
        self.assertIn('selected="true"', choice.group(0))

    def test_apps_are_pinned_to_applications_instead_of_relocated(self) -> None:
        xml, relocation = self._run_installer(
            [], [("Fixture standalone", "Fixture")]
        )

        self.assertIn("Fixture.app.pkg", xml)
        self.assertEqual(relocation.splitlines(), ["false", "false"])

    def test_an_app_component_passes_scripts_without_distribution_javascript(self) -> None:
        xml, relocation = self._run_installer(
            [],
            [("Forge Modular", "Forge Modular")],
            scripted_apps={"Forge Modular"},
        )

        argv = self._last_pkgbuild_argv.splitlines()
        self.assertIn("--scripts", argv)
        scripts_dir = argv[argv.index("--scripts") + 1]
        self.assertTrue(scripts_dir.endswith("/Forge Modular-scripts"), scripts_dir)
        self.assertNotIn("Modular-scripts", argv)
        self.assertIn('require-scripts="false"', xml)
        self.assertEqual(relocation.splitlines(), ["false", "false"])

    def test_an_app_without_scripts_keeps_scripts_disabled(self) -> None:
        xml, _ = self._run_installer([], [("Fixture", "Fixture")])

        self.assertNotIn("--scripts", self._last_pkgbuild_argv.splitlines())
        self.assertIn('require-scripts="false"', xml)

    def test_intel_installer_declares_x86_64_host_support(self) -> None:
        xml, _ = self._run_installer([], [("Fixture", "Fixture")],
                                     architectures="x86_64")

        self.assertIn('hostArchitectures="x86_64"', xml)


if __name__ == "__main__":
    unittest.main()
