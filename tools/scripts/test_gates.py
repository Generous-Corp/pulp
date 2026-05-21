#!/usr/bin/env python3
"""Aggregate fixture suite for version_bump_check.py and skill_sync_check.py.

P9-NEW refactor (2026-05): the test bodies were split into focused
cluster modules that mirror the version_bump_check.py module split.
This file stays the single CI entrypoint
(`.github/workflows/version-skill-check.yml` runs
`python3 tools/scripts/test_gates.py`) — it imports every cluster
`TestCase` subclass so the full suite still runs as one command with
no extra deps on PEP-668 systems.

Each test spins up a throwaway git repo (see `gate_test_support.py`),
stages a scenario via real `git commit`s, and asserts the script's exit
code and stdout against the expected verdict.

Run the whole suite:
    python3 tools/scripts/test_gates.py

Or run a single cluster module directly:
    python3 tools/scripts/test_gate_common.py
    python3 tools/scripts/test_version_bump_surfaces.py
    python3 tools/scripts/test_version_bump_heuristics.py
    python3 tools/scripts/test_version_bump_apply.py
    python3 tools/scripts/test_version_bump_fixfeat.py
    python3 tools/scripts/test_skill_sync.py
"""

from __future__ import annotations

import os
import sys
import unittest

# When invoked as a script from the repo root, `tools/scripts` is not on
# sys.path — add it so the sibling cluster modules import cleanly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Re-export the cluster TestCase subclasses. `unittest`'s default loader
# discovers every `TestCase` in this module's namespace, so importing the
# classes here is sufficient to run all of them under `test_gates.py`.
from test_gate_common import GlobToRegexTests  # noqa: E402,F401
from test_gate_common import GitHelperTests  # noqa: E402,F401
from test_gate_common import StripMetaTests  # noqa: E402,F401
from test_gate_common import TrailerParseTests  # noqa: E402,F401
from test_version_bump_surfaces import VersionBumpSurfacesTests  # noqa: E402,F401
from test_version_bump_heuristics import VersionBumpHeuristicsTests  # noqa: E402,F401
from test_version_bump_apply import VersionBumpApplyTests  # noqa: E402,F401
from test_version_bump_fixfeat import VersionBumpFixFeatTests  # noqa: E402,F401
from test_skill_sync import SkillSyncTests  # noqa: E402,F401


# ── Entry ──────────────────────────────────────────────────────────────


if __name__ == "__main__":
    unittest.main(verbosity=2)
