#!/usr/bin/env python3
"""tools/install/SHA256SUMS must describe the installers beside it.

The README's "verify before installation" recipe downloads the installer from
the website (which serves this checkout's main-branch copy) and checks it
against this file. A stale entry makes that recipe fail for every user, so any
edit to an installer must update its checksum in the same change.
"""

from __future__ import annotations

import hashlib
import unittest
from pathlib import Path

INSTALL_DIR = Path(__file__).resolve().parents[2] / "tools" / "install"


def recorded() -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in (INSTALL_DIR / "SHA256SUMS").read_text().splitlines():
        if line.strip():
            digest, name = line.split(maxsplit=1)
            entries[name.strip()] = digest
    return entries


class InstallerChecksumsTest(unittest.TestCase):
    def test_every_installer_has_a_current_checksum(self) -> None:
        entries = recorded()
        installers = sorted(p.name for p in INSTALL_DIR.glob("install.*"))
        self.assertEqual(sorted(entries), installers)
        for name in installers:
            actual = hashlib.sha256((INSTALL_DIR / name).read_bytes()).hexdigest()
            self.assertEqual(
                entries[name],
                actual,
                f"{name} changed without its SHA256SUMS entry; run "
                "`(cd tools/install && shasum -a 256 install.sh install.ps1 > SHA256SUMS)`",
            )


if __name__ == "__main__":
    unittest.main()
