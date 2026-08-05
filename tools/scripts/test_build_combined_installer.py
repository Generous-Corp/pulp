#!/usr/bin/env python3
"""Contract tests for the combined macOS installer component graph."""

from __future__ import annotations

import os
from pathlib import Path
import re
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
        self,
        plugins: list[tuple[str, str]],
        apps: list[tuple[str, str]] | None = None,
        grouped_apps: list[tuple[str, str, str]] | None = None,
        product_titles: list[tuple[str, str]] | None = None,
    ) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            capture = tmp / "distribution.xml"
            relocation_capture = tmp / "app-relocation.txt"
            output = tmp / "out"

            self._write_tool(fake_bin, "codesign", "exit 0\n")
            self._write_tool(fake_bin, "security", "exit 0\n")
            self._write_tool(
                fake_bin,
                "python3",
                "# Bundle relocation validation is outside this graph test.\n"
                "exit 0\n",
            )
            self._write_tool(
                fake_bin,
                "pkgbuild",
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
                'cp "$distribution" "$CAPTURE_XML"\n'
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
            for title, app_name in apps or []:
                bundle = tmp / f"{app_name}.app"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--app", title, str(bundle)))
            # Apps nested under a product group, which are also forced on.
            for group, title, app_name in grouped_apps or []:
                bundle = tmp / f"{app_name}.app"
                (bundle / "Contents" / "MacOS").mkdir(parents=True)
                args.extend(("--app-for", group, title, str(bundle)))
            for bundle_name, title in product_titles or []:
                args.extend(("--product-title", bundle_name, title))

            env = {
                "PATH": f"{fake_bin}:/usr/bin:/bin",
                "HOME": str(tmp),
                "TMPDIR": str(tmp),
                "CAPTURE_XML": str(capture),
                "CAPTURE_APP_RELOCATION": str(relocation_capture),
                "PULP_SKIP_SIGNING_PREFLIGHT": "1",
            }
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
            relocation = (
                relocation_capture.read_text()
                if relocation_capture.is_file()
                else ""
            )
            return capture.read_text(), relocation

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


if __name__ == "__main__":
    unittest.main()
