#!/usr/bin/env python3
"""A discarded CONTROL_PROFILE is announced, and only when one is discarded.

`_pulp_attach_control_shipping` force-downgrades any non-Standalone artifact to
`production-stripped`. That default is right for a shipped plug-in -- a module
is loaded into a host process its author does not own, so Pulp does not offer a
control endpoint there. What was wrong is that it happened in silence.

The branch can only be reached when an author explicitly passed CONTROL_PROFILE
to `pulp_add_plugin()`, because the default is already `production-stripped`.
So reaching it means an explicit request is being thrown away, and an author
who wrote `CONTROL_PROFILE developer-local` got a stripped bundle with nothing
in the build output connecting the two -- the Standalone-only limitation was
discoverable only by reading the CMake.

These assertions are structural because the defect is a shell/CMake surface
losing a message, which no synthetic project reproduces more honestly than
reading the branch itself.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHIPPING = ROOT / "tools" / "cmake" / "PulpControlShipping.cmake"
UTILS = ROOT / "tools" / "cmake" / "PulpUtils.cmake"

# The guard that decides whether a profile survives to the artifact.
GUARD = re.compile(
    r'if\(NOT artifact_format STREQUAL "Standalone" AND\s*\n'
    r'\s*NOT _profile STREQUAL "production-stripped"\)\s*\n'
    r'(?P<body>.*?)\n\s*set\(_profile "production-stripped"\)',
    re.DOTALL,
)


class DiscardedControlProfileIsAnnounced(unittest.TestCase):
    def setUp(self) -> None:
        self.shipping = SHIPPING.read_text()
        self.utils = UTILS.read_text()

    def test_the_downgrade_branch_exists_at_all(self) -> None:
        # Control for every assertion below: if the guard is ever restructured,
        # a regex that silently stops matching would make the rest vacuous.
        self.assertIsNotNone(GUARD.search(self.shipping))

    def test_the_downgrade_announces_itself(self) -> None:
        body = GUARD.search(self.shipping).group("body")
        self.assertIn("message(WARNING", body,
                      "a discarded CONTROL_PROFILE must not be discarded silently")

    def test_the_message_names_what_an_author_needs(self) -> None:
        body = GUARD.search(self.shipping).group("body")
        for fragment in ("${target}", "${_profile}", "${artifact_format}"):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, body,
                              "the warning must name the target, the requested "
                              "profile and the format it was dropped for")

    def test_the_safe_default_is_unchanged(self) -> None:
        # The fix is honesty, not a behaviour change: a plug-in format still
        # ships stripped, with no capabilities and no endpoint. That literal
        # lives after the `set(_profile ...)` the guard regex stops at, so it is
        # asserted against the region following the guard rather than its body.
        match = GUARD.search(self.shipping)
        tail = self.shipping[match.end():]
        self.assertIn(r'\"endpoint_included\": false', tail,
                      "the downgraded artifact must still declare no endpoint")
        self.assertIn(r'\"capabilities\": []', tail,
                      "the downgraded artifact must still declare no capabilities")
        self.assertIn('set(_artifact_capabilities "")', self.shipping)

    def test_an_ordinary_build_cannot_trip_the_warning(self) -> None:
        # The load-bearing half. If the default were anything but
        # production-stripped, this warning would fire on every plug-in build
        # and become noise an author learns to ignore.
        self.assertIn('set(_pulp_control_profile "production-stripped")', self.utils,
                      "the no-CONTROL_PROFILE default must stay production-stripped, "
                      "or the downgrade warning fires on ordinary builds")


if __name__ == "__main__":
    unittest.main(verbosity=2, argv=[sys.argv[0]])
