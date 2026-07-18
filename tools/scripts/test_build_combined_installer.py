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

    def _run_installer(self, plugins: list[tuple[str, str]]) -> str:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = Path(raw_tmp)
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            capture = tmp / "distribution.xml"
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
                'last=""\nfor arg in "$@"; do last="$arg"; done\n'
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

            env = os.environ.copy()
            env["PATH"] = f"{fake_bin}:{env['PATH']}"
            env["CAPTURE_XML"] = str(capture)
            # This graph test must never inspect or mutate the user's keychains.
            # Signing itself is still exercised through the fake codesign tool.
            env["PULP_SKIP_SIGNING_PREFLIGHT"] = "1"
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
            self.assertTrue(capture.is_file())
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


if __name__ == "__main__":
    unittest.main()
