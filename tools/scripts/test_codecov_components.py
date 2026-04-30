#!/usr/bin/env python3
"""Structural classification tests for codecov.yml components.

Audit resolution for issue #1055 (parent #1049). Sibling precedent #841.

The contract enforced here:

  Every first-party file under ``core/``, ``tools/``, ``apple/``,
  ``android/``, or ``inspect/`` matches at least ONE Codecov component
  declared in ``codecov.yml :: component_management.individual_components``.

  Multi-bucket matches are allowed only along the documented
  cross-cutting axes (subsystem × platform, plus the
  ``cli`` / ``tools`` more-specific-wins surface pair).

  The full rationale and the allowed-overlap table live in
  ``docs/status/coverage-classification.md``.

Why not "exactly one component per file" (the strict audit recipe in
#1055): Pulp's component layout intentionally cross-cuts subsystem
(``audio``, ``view``, …) with platform (``apple``, ``windows``, …) and
surface (``cli``, ``tools``, …) so the dashboard can answer
"what fraction of ``core/audio`` is exercised on Windows". A platform
shim like ``core/audio/platform/mac/coreaudio_device.mm`` therefore
legitimately matches two components — ``audio`` and ``apple`` — by
design. See ``codecov.yml`` lines 79-83 and 219-229.

Run::

    python3 tools/scripts/test_codecov_components.py

This test does no network calls and parses ``codecov.yml`` only via
PyYAML, which is already a Pulp tooling dependency.
"""

from __future__ import annotations

import collections
import fnmatch
import pathlib
import subprocess
import unittest

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CODECOV = REPO_ROOT / "codecov.yml"

# First-party path prefixes — the same set named by issue #1055's audit
# recipe and by the parent #1049 framing (the surfaces Pulp ships and
# tracks coverage on).
FIRST_PARTY_PREFIXES = ("core/", "tools/", "apple/", "android/", "inspect/")

# Component IDs partitioned by axis. These are the axis labels — the
# actual path-glob → component mapping lives in codecov.yml.
SUBSYSTEM_IDS = {
    "audio", "canvas", "dsl", "events", "format", "host", "midi",
    "osc", "platform", "render", "runtime", "signal", "state", "view",
}
PLATFORM_IDS = {"android", "apple", "linux", "windows"}
SURFACE_IDS = {"cli", "inspect", "ship", "tools"}

# Allowed multi-component overlaps. Mirrors the table in
# docs/status/coverage-classification.md. A file may match multiple
# components only if the matched set is one of these.
ALLOWED_OVERLAPS: set[frozenset[str]] = {
    # Subsystem × Platform shims.
    frozenset({"audio", "android"}),
    frozenset({"audio", "apple"}),
    frozenset({"audio", "linux"}),
    frozenset({"audio", "windows"}),
    frozenset({"canvas", "apple"}),
    frozenset({"midi", "android"}),
    frozenset({"midi", "apple"}),
    frozenset({"midi", "linux"}),
    frozenset({"midi", "windows"}),
    frozenset({"platform", "android"}),
    frozenset({"platform", "apple"}),
    frozenset({"platform", "linux"}),
    frozenset({"platform", "windows"}),
    frozenset({"render", "android"}),
    frozenset({"view", "android"}),
    frozenset({"view", "apple"}),
    frozenset({"view", "linux"}),
    frozenset({"view", "windows"}),
    # Surface more-specific-wins overlap.
    frozenset({"cli", "tools"}),
}


def _load_components() -> list[dict]:
    with CODECOV.open("r", encoding="utf-8") as fh:
        doc = yaml.safe_load(fh)
    return (
        (doc.get("component_management") or {}).get("individual_components", [])
    )


def _first_party_files() -> list[str]:
    out = subprocess.check_output(
        ["git", "ls-files"], cwd=REPO_ROOT, text=True
    )
    return [
        f for f in out.splitlines() if f.startswith(FIRST_PARTY_PREFIXES)
    ]


def _components_for(path: str, components: list[dict]) -> list[str]:
    """Return the component_ids whose path-globs match ``path``.

    A component appears at most once even if multiple of its path-globs
    overlap onto the same file (e.g. android's ``core/**/android/**``
    plus ``core/platform/src/android/**``). Cosmetic glob redundancy
    inside a single component is not a classification gap.
    """
    matched: list[str] = []
    for c in components:
        cid = c["component_id"]
        for p in c.get("paths", []):
            if fnmatch.fnmatch(path, p):
                if cid not in matched:
                    matched.append(cid)
                break
    return matched


class CodecovComponentClassification(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        cls.components = _load_components()
        cls.files = _first_party_files()
        cls.classification = {
            f: _components_for(f, cls.components) for f in cls.files
        }

    def test_no_first_party_file_is_unclassified(self):
        """Every first-party file matches at least one component.

        An UNCLASSIFIED file is silent omission on the dashboard — its
        coverage shows up nowhere when filtering by component, which is
        the primary drilldown surface contributors and agents read.
        """
        unclassified = sorted(
            f for f, ids in self.classification.items() if not ids
        )
        self.assertFalse(
            unclassified,
            "first-party files match zero components — silent omission "
            "from the Codecov dashboard. Either (a) widen an existing "
            "component's `paths:`, (b) add a new component, or (c) list "
            "the file under 'Deliberate-uncategorized' in "
            "docs/status/coverage-classification.md with a rationale.\n"
            f"unclassified ({len(unclassified)}):\n  "
            + "\n  ".join(unclassified[:20])
            + ("\n  …" if len(unclassified) > 20 else ""),
        )

    def test_multi_component_files_match_allowed_axes(self):
        """Files in ≥2 components match a documented cross-cutting pair.

        Pulp deliberately cross-cuts subsystem × platform (and the
        cli/tools surface pair). Any other multi-bucket combination
        means a glob got too greedy and is double-counting coverage on
        the dashboard.
        """
        offenders = collections.defaultdict(list)
        for f, ids in self.classification.items():
            if len(ids) < 2:
                continue
            key = frozenset(ids)
            if key not in ALLOWED_OVERLAPS:
                offenders[",".join(sorted(ids))].append(f)

        if offenders:
            lines = []
            for pair, fs in sorted(offenders.items()):
                lines.append(f"  {pair}: {len(fs)} files")
                for f in fs[:5]:
                    lines.append(f"    {f}")
            self.fail(
                "first-party files match multiple components in an "
                "undocumented combination — either tighten a path-glob "
                "or add the new pair to ALLOWED_OVERLAPS in this test "
                "and to the table in "
                "docs/status/coverage-classification.md.\n"
                + "\n".join(lines)
            )

    def test_axis_partition_matches_codecov_yml(self):
        """The (subsystem, platform, surface) axis labels above match
        the live ``codecov.yml`` component set, so this test fails loudly
        when a new component is added without classifying its axis."""
        live_ids = {c["component_id"] for c in self.components}
        all_axes = SUBSYSTEM_IDS | PLATFORM_IDS | SURFACE_IDS
        missing_in_axis_table = live_ids - all_axes
        missing_in_yaml = all_axes - live_ids
        self.assertFalse(
            missing_in_axis_table,
            "codecov.yml has component_ids not partitioned into an axis "
            "in this test — update SUBSYSTEM_IDS/PLATFORM_IDS/SURFACE_IDS "
            "and the docs/status/coverage-classification.md table.\n"
            f"unclassified ids: {sorted(missing_in_axis_table)}",
        )
        self.assertFalse(
            missing_in_yaml,
            "this test names component_ids that are not in codecov.yml — "
            "either remove the stale id from the axis sets or restore "
            "the missing component.\n"
            f"missing in codecov.yml: {sorted(missing_in_yaml)}",
        )


if __name__ == "__main__":
    unittest.main()
