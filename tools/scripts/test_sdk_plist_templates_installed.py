#!/usr/bin/env python3
"""Every Info.plist template a format helper can reach must ship in the SDK.

`PulpPluginFormats.cmake` selects templates with `elseif(EXISTS ...)`. A
template missing from the SDK install list therefore does NOT error: the helper
falls through and the bundle is built with an empty `CFBundleIdentifier` and
package type `APPL`. Nothing fails, nothing warns, and the breakage only
surfaces when a host rejects the plugin or `codesign` reports the identifier as
`<name>-<hash>`.

That happened: the CLAP identity fix landed for in-tree builds but
`PulpInfoPlist.clap.in` was never added to the install list, so every SDK
consumer kept shipping identity-less CLAP bundles.

This asserts the two lists agree, so the next format added cannot repeat it.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CMAKE = REPO / "tools" / "cmake"
FORMATS = CMAKE / "PulpPluginFormats.cmake"
INSTALL_RULES = CMAKE / "PulpInstallRules.cmake"


def templates_referenced() -> set[str]:
    """Template basenames PulpPluginFormats can consume at configure time."""
    text = FORMATS.read_text(encoding="utf-8")
    return set(re.findall(r"(PulpInfoPlist\.[A-Za-z0-9_]+\.in)", text))


def templates_installed() -> set[str]:
    text = INSTALL_RULES.read_text(encoding="utf-8")
    return set(re.findall(r"(PulpInfoPlist\.[A-Za-z0-9_]+\.in)", text))


def templates_on_disk() -> set[str]:
    return {p.name for p in CMAKE.glob("PulpInfoPlist.*.in")}


class PlistTemplatesShipWithTheSdk(unittest.TestCase):
    def test_every_referenced_template_is_installed(self) -> None:
        referenced = templates_referenced()
        self.assertTrue(referenced, "found no template references — did the "
                                    "reference syntax in PulpPluginFormats change?")
        missing = sorted(referenced - templates_installed())
        self.assertEqual(
            missing, [],
            "these templates are reachable by a format helper but are NOT "
            "installed into the SDK, so consumer builds will silently produce "
            "bundles with an empty CFBundleIdentifier: " + ", ".join(missing),
        )

    def test_every_referenced_template_exists(self) -> None:
        missing = sorted(templates_referenced() - templates_on_disk())
        self.assertEqual(missing, [],
                         "referenced but absent from tools/cmake: " + ", ".join(missing))

    def test_no_template_is_installed_without_existing(self) -> None:
        # A stale install entry fails the install itself, which is loud, but it
        # is cheap to catch here rather than at package time.
        missing = sorted(templates_installed() - templates_on_disk())
        self.assertEqual(missing, [],
                         "installed but absent from tools/cmake: " + ", ".join(missing))

    def test_clap_specifically_ships(self) -> None:
        # The regression that motivated this file. Kept explicit so a future
        # refactor that drops CLAP names it rather than thinning a set diff.
        self.assertIn("PulpInfoPlist.clap.in", templates_installed())


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
