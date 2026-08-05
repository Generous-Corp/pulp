#!/usr/bin/env python3
"""Contract tests for the combined macOS installer component graph."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "scripts" / "build_combined_installer.sh"


class CombinedInstallerTest(unittest.TestCase):
    def _write_tool(self, directory: Path, name: str, body: str) -> None:
        path = directory / name
        path.write_text("#!/bin/bash\nset -euo pipefail\n" + body)
        path.chmod(0o755)

    def _run_installer(
        self, plugins: list[tuple[str, str]], license_text: str | None = None,
        apps: list[tuple[str, bool]] | None = None,
    ) -> str:
        self._last_productbuild_argv = ""
        self._last_pkgbuild_argv = ""
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            capture = tmp / "distribution.xml"
            capture_argv = tmp / "productbuild-argv.txt"
            capture_pkg_argv = tmp / "pkgbuild-argv.txt"
            output = tmp / "out"

            self._write_tool(fake_bin, "codesign", "exit 0\n")
            self._write_tool(fake_bin, "security", "exit 0\n")
            self._write_tool(
                fake_bin,
                "python3",
                "# Bundle relocation validation is outside this graph test.\n"
                "exit 0\n",
            )
            # `--analyze` must produce a component plist that PlistBuddy can
            # actually read. Touching an empty file (what this stub used to do
            # for every invocation) makes PlistBuddy fail and the script exit 2,
            # so any fixture exercising the app path failed for a reason that
            # had nothing to do with the code under test — which is presumably
            # why no fixture exercised it.
            self._write_tool(
                fake_bin,
                "pkgbuild",
                'printf "%s\\n" "$@" >> "$CAPTURE_PKG_ARGV"\n'
                'analyze=0\nlast=""\n'
                'for arg in "$@"; do\n'
                '  [[ "$arg" == "--analyze" ]] && analyze=1\n'
                '  last="$arg"\n'
                'done\n'
                'mkdir -p "$(dirname "$last")"\n'
                'if [[ "$analyze" == 1 ]]; then\n'
                '  cat > "$last" <<\'PLIST\'\n'
                '<?xml version="1.0" encoding="UTF-8"?>\n'
                '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
                '<plist version="1.0">\n'
                '<array>\n'
                '  <dict>\n'
                '    <key>BundleIsRelocatable</key><true/>\n'
                '    <key>BundleHasStrictIdentifier</key><true/>\n'
                '    <key>BundleIsVersionChecked</key><true/>\n'
                '    <key>BundleOverwriteAction</key><string>upgrade</string>\n'
                '    <key>RootRelativeBundlePath</key>'
                '<string>Applications/Fixture.app</string>\n'
                '  </dict>\n'
                '</array>\n'
                '</plist>\n'
                'PLIST\n'
                'else\n'
                '  : > "$last"\n'
                'fi\n',
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
                'cp "$distribution" "$CAPTURE_XML"\n'
                'printf "%s\\n" "$@" > "$CAPTURE_ARGV"\n'
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
            for plugin_name, kind in plugins:
                suffix = {"au": "component", "vst3": "vst3", "clap": "clap"}[kind]
                bundle = tmp / f"{plugin_name}.{suffix}"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--plugin", kind, str(bundle)))

            for app_name, with_scripts in (apps or []):
                bundle = tmp / f"{app_name}.app"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--app", app_name, str(bundle)))
                if with_scripts:
                    scripts = tmp / f"{app_name}-scripts"
                    scripts.mkdir()
                    hook = scripts / "postinstall"
                    hook.write_text("#!/bin/bash\nexit 0\n")
                    hook.chmod(0o755)
                    args.extend(("--app-scripts", app_name, str(scripts)))

            env = {
                "PATH": f"{fake_bin}:/usr/bin:/bin",
                "HOME": str(tmp),
                "TMPDIR": str(tmp),
                "CAPTURE_XML": str(capture),
                "CAPTURE_ARGV": str(capture_argv),
                "CAPTURE_PKG_ARGV": str(capture_pkg_argv),
                "PULP_SKIP_SIGNING_PREFLIGHT": "1",
            }
            if license_text is not None:
                license_file = tmp / "LICENSE.txt"
                license_file.write_text(license_text)
                env["PKG_LICENSE_FILE"] = str(license_file)
            completed = subprocess.run(
                args,
                cwd=ROOT,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(
                completed.returncode,
                0,
                msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            self.assertTrue(
                capture.is_file(),
                msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            if capture_argv.is_file():
                self._last_productbuild_argv = capture_argv.read_text()
            if capture_pkg_argv.is_file():
                self._last_pkgbuild_argv = capture_pkg_argv.read_text()
            return capture.read_text()

    def test_multi_plugin_packages_are_unique_and_grouped_by_plugin(self) -> None:
        xml = self._run_installer(
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
        xml = self._run_installer([("Kick", "au"), ("Kick", "clap")])

        self.assertNotIn('choice="plugin-0">', xml)
        self.assertIn('<line choice="plugin-0-au"/>', xml)
        self.assertIn('<line choice="plugin-0-clap"/>', xml)

    def test_distinct_names_with_the_same_lossy_slug_do_not_collide(self) -> None:
        xml = self._run_installer([("Foo-Bar", "au"), ("Foo Bar", "au")])

        self.assertIn('choice id="plugin-0-au"', xml)
        self.assertIn('choice id="plugin-1-au"', xml)
        self.assertIn('title="Foo-Bar"', xml)
        self.assertIn('title="Foo Bar"', xml)


    # The two license branches, pinned separately.
    #
    # `LICENSE_ARGS` is an array that is EMPTY unless PKG_LICENSE_FILE is set,
    # and macOS ships bash 3.2, where expanding an empty array under `set -u` is
    # an unbound-variable error rather than an empty list. So the default path --
    # no license -- aborted before productbuild ran and produced no package,
    # while the configured path worked. A test that only ever exercises one of
    # the two branches cannot tell those apart, which is how it shipped.

    def test_without_a_license_productbuild_still_runs_and_gets_no_resources(self) -> None:
        xml = self._run_installer([("Kick", "au")])

        argv = self._last_productbuild_argv
        self.assertNotEqual(argv, "", "productbuild never ran")
        self.assertNotIn("--resources", argv.splitlines())
        self.assertNotIn("<license", xml)

    def test_a_license_reaches_productbuild_and_the_distribution(self) -> None:
        xml = self._run_installer([("Kick", "au")], license_text="Rack SDK is VCV's.")

        argv = self._last_productbuild_argv.splitlines()
        self.assertIn("--resources", argv)
        # The value after --resources is the staged resources DIRECTORY, and it
        # must survive as ONE argv entry -- the empty-array fix must not word-split
        # a path containing spaces.
        resources = argv[argv.index("--resources") + 1]
        self.assertTrue(resources.endswith("/resources"), resources)
        self.assertEqual(
            (Path(resources) / "license.txt").name, "license.txt"
        )
        self.assertIn('<license file="license.txt"/>', xml)


    # The app component, which had no coverage on either side of the merge.
    #
    # The app loop carries two flags that are mutually necessary and easy to
    # separate, because each looks redundant next to the other:
    #
    #   --scripts          our postinstall, which places the Rack modules into
    #                      the logged-in user's home directory. It hardcodes
    #                      $VOLUME/Applications/Forge Modular.app and then
    #                      deliberately never fails.
    #   --component-plist  relocation pinning. pkgbuild marks app bundles
    #                      relocatable by default, so on a machine where Launch
    #                      Services knows another copy of the bundle ID,
    #                      Installer overwrites THAT copy instead of installing
    #                      to /Applications.
    #
    # Apart, each is quietly broken: with relocation left on, the payload lands
    # in some other copy, the hardcoded path is absent, the postinstall logs one
    # line and exits 0, and the installer reports success having placed nothing.
    # Relocation pinning alone lands the app but never places the modules.
    #
    # `--component-plist` is NOT about component selectability. Selectability is
    # a productbuild Distribution-XML concern and lives in the <choice> machinery
    # with the `required` lock asserted below. The two never meet, which is why
    # `--component-plist` looks deletable once the lock is visible. It is not.

    def test_an_app_component_passes_its_scripts_to_pkgbuild(self) -> None:
        xml = self._run_installer([("Kick", "au")], apps=[("Forge Modular", True)])

        argv = self._last_pkgbuild_argv.splitlines()
        self.assertIn("--scripts", argv)
        # ONE argv entry despite the space in the title. If the empty-array
        # idiom guarding this ever word-split, the directory would arrive as
        # two entries -- ".../Forge" followed by "Modular-scripts" -- and
        # pkgbuild would be handed a path that does not exist.
        scripts_dir = argv[argv.index("--scripts") + 1]
        self.assertTrue(scripts_dir.endswith("/Forge Modular-scripts"), scripts_dir)
        self.assertNotIn("Modular-scripts", argv)

        # The user must not be able to deselect the app and thereby skip the
        # postinstall that places everything else. The lock is these two
        # attributes on the choice, not the word "Required" in its description.
        self.assertIn('<line choice="forge-modular"/>', xml)
        self.assertIn('choice id="forge-modular"', xml)
        self.assertRegex(xml, r'choice id="forge-modular".*enabled="false"')
        self.assertRegex(xml, r'choice id="forge-modular".*selected="true"')

    def test_an_app_without_scripts_passes_no_scripts_flag(self) -> None:
        self._run_installer([("Kick", "au")], apps=[("Forge Modular", False)])
        self.assertNotIn("--scripts", self._last_pkgbuild_argv.splitlines())

    def test_relocation_is_pinned_whenever_the_component_is_analyzed(self) -> None:
        """If the app component is analyzed, its plist must reach pkgbuild.

        Forward-compatible on purpose. Relocation pinning (`--analyze` +
        PlistBuddy + `--component-plist`) lives on main and is not on this
        branch yet, so today this asserts nothing and passes. The moment the
        two sides merge it becomes load-bearing, and it fails if either half
        of the union is dropped in the resolution -- which is the conflict
        this file exists to guard.
        """
        self._run_installer([("Kick", "au")], apps=[("Forge Modular", True)])
        argv = self._last_pkgbuild_argv.splitlines()
        if "--analyze" in argv:
            self.assertIn("--component-plist", argv)
            plist = argv[argv.index("--component-plist") + 1]
            self.assertTrue(plist.endswith(".plist"), plist)


if __name__ == "__main__":
    unittest.main()
