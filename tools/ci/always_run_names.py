"""Test names that always run, whatever a reuse key says about them.

A drift, census, registry, sync, guard, lint, inventory or probe test reads
the whole tree or the host rather than inputs a per-test or per-executable
key can name, so neither the per-test receipts shadow nor the executable keys
ever skip one. The pattern lives in its own file so that the executable keys'
code digest covers it without covering the rest of the receipts shadow.
"""
from __future__ import annotations

import re

ALWAYS_RUN_NAME_RE = re.compile(r"drift|census|registry|sync|guard|lint|inventory|probe", re.I)
