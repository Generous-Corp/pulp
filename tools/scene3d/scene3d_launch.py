#!/usr/bin/env python3
"""Launch a configurable scene3d tool, including a script stand-in on Windows.

The contracts in this directory pass a fake ``--inspect-tool``,
``--probe-tool``, ``--sidecar-tool`` or ``--preflight-tool`` written as a
``#!`` Python script, and the smoke scripts launch whatever path they are
given. Windows cannot execute a ``#!`` script, so every launch goes through
``script_argv.argv_for``, which runs a script through its interpreter on
Windows and changes nothing anywhere else.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from script_argv import argv_for  # noqa: E402

__all__ = ["argv_for"]
