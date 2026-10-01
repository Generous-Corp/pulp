#!/usr/bin/env python3
"""The Shipyard local `mac` lane is opt-in, and asking for it still runs the same lane.

`shipyard pr` / `shipyard ship` validate every declared target except those with
`default = false`, unless an active `[profiles.<name>].targets` list selects
targets explicitly. This test resolves that default set from
`.shipyard/config.toml` the way Shipyard does and asserts it excludes `mac`, so
a plain `shipyard pr` delegates the verdict to the required GitHub checks
instead of spending hours of host time on a local Debug build.

It also pins the lane's recipe, so `shipyard pr --target mac` remains the Debug,
examples-ON, full-ctest lane that decisions-contract row #9 describes rather
than silently becoming something cheaper.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CONFIG = REPO_ROOT / ".shipyard" / "config.toml"


def load_config() -> dict:
    with CONFIG.open("rb") as handle:
        return tomllib.load(handle)


def profile_selection(config: dict) -> list[str] | None:
    """Targets the active profile selects, or None when it selects nothing."""
    name = config.get("project", {}).get("profile")
    if not name:
        return None
    profile = config.get("profiles", {}).get(name)
    if not isinstance(profile, dict):
        return None
    names = [value for value in profile.get("targets", []) if isinstance(value, str)]
    return names or None


def default_targets(config: dict) -> list[str]:
    """The targets a plain `shipyard pr` validates."""
    targets = config["targets"]
    selected = profile_selection(config)
    if selected is not None:
        return selected
    return [name for name, table in targets.items() if table.get("default", True) is not False]


@unittest.skipIf(tomllib is None, "tomllib unavailable; cannot read .shipyard/config.toml")
class ShipyardTargetDefaultsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.config = load_config()

    def test_mac_is_declared(self) -> None:
        # Control for the exclusion below: an empty or unread target table
        # would make "mac is not a default target" trivially true.
        self.assertIn("mac", self.config.get("targets", {}))

    def test_plain_shipyard_pr_validates_no_local_target(self) -> None:
        self.assertNotIn("mac", default_targets(self.config))
        self.assertEqual(default_targets(self.config), [])

    def test_opt_in_flag_is_a_boolean(self) -> None:
        # Shipyard rejects a non-boolean `default`; a string "false" would be
        # a configuration error on every `shipyard pr`.
        self.assertIs(self.config["targets"]["mac"].get("default"), False)

    def test_requested_lane_keeps_its_recipe(self) -> None:
        mac = self.config["targets"]["mac"]
        self.assertEqual(mac.get("backend"), "local")
        self.assertEqual(mac.get("validation_build_type"), "debug")
        recipe = self.config["validation"]["default"]
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", recipe["configure"])
        self.assertIn("-DPULP_BUILD_EXAMPLES=ON", recipe["configure"])
        self.assertIn("ctest", recipe["test"])


class DefaultTargetResolutionTests(unittest.TestCase):
    """The resolver above, on synthetic configs, so it cannot pass vacuously."""

    def test_targets_without_the_key_are_default(self) -> None:
        config = {"targets": {"mac": {}, "linux": {"default": False}}}
        self.assertEqual(default_targets(config), ["mac"])

    def test_an_active_profile_selection_wins(self) -> None:
        config = {
            "project": {"profile": "local"},
            "profiles": {"local": {"targets": ["mac"]}},
            "targets": {"mac": {"default": False}},
        }
        self.assertEqual(default_targets(config), ["mac"])

    def test_an_empty_profile_list_selects_nothing(self) -> None:
        config = {
            "project": {"profile": "solo"},
            "profiles": {"solo": {"targets": []}},
            "targets": {"mac": {"default": False}},
        }
        self.assertEqual(default_targets(config), [])


if __name__ == "__main__":
    sys.exit(unittest.main())
